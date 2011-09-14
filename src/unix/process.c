
/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h> /* O_CLOEXEC, O_NONBLOCK */
#include <poll.h>
#include <unistd.h>
#include <stdio.h>

#ifdef __APPLE__
# include <crt_externs.h>
# define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif


static void uv__chld(EV_P_ ev_child* watcher, int revents) {
  int status = watcher->rstatus;
  int exit_status = 0;
  int term_signal = 0;
  uv_process_t *process = watcher->data;

  assert(&process->child_watcher == watcher);
  assert(revents & EV_CHILD);

  ev_child_stop(EV_A_ &process->child_watcher);

  if (WIFEXITED(status)) {
    exit_status = WEXITSTATUS(status);
  }

  if (WIFSIGNALED(status)) {
    term_signal = WTERMSIG(status);
  }

  if (process->exit_cb) {
    process->exit_cb(process, exit_status, term_signal);
  }
}

#ifndef SPAWN_WAIT_EXEC
# define SPAWN_WAIT_EXEC 1
#endif

int uv_spawn(uv_loop_t* loop, uv_process_t* process,
    uv_process_options_t options) {
  /*
   * Save environ in the case that we get it clobbered
   * by the child process.
   */
  char** save_our_env = environ;
  int stdin_pipe[2] = { -1, -1 };
  int stdout_pipe[2] = { -1, -1 };
  int stderr_pipe[2] = { -1, -1 };
#if SPAWN_WAIT_EXEC
  int signal_pipe[2] = { -1, -1 };
  struct pollfd pfd;
#endif
  int status;
  pid_t pid;

  uv__handle_init(loop, (uv_handle_t*)process, UV_PROCESS);
  loop->counters.process_init++;

  process->exit_cb = options.exit_cb;

  if (options.stdin_stream) {
    if (options.stdin_stream->type != UV_NAMED_PIPE) {
      errno = EINVAL;
      goto error;
    }

    if (pipe(stdin_pipe) < 0) {
      goto error;
    }
    uv__cloexec(stdin_pipe[0], 1);
    uv__cloexec(stdin_pipe[1], 1);
  }

  if (options.stdout_stream) {
    if (options.stdout_stream->type != UV_NAMED_PIPE) {
      errno = EINVAL;
      goto error;
    }

    if (pipe(stdout_pipe) < 0) {
      goto error;
    }
    uv__cloexec(stdout_pipe[0], 1);
    uv__cloexec(stdout_pipe[1], 1);
  }

  if (options.stderr_stream) {
    if (options.stderr_stream->type != UV_NAMED_PIPE) {
      errno = EINVAL;
      goto error;
    }

    if (pipe(stderr_pipe) < 0) {
      goto error;
    }
    uv__cloexec(stderr_pipe[0], 1);
    uv__cloexec(stderr_pipe[1], 1);
  }

  /* This pipe is used by the parent to wait until
   * the child has called `execve()`. We need this
   * to avoid the following race condition:
   *
   *    if ((pid = fork()) > 0) {
   *      kill(pid, SIGTERM);
   *    }
   *    else if (pid == 0) {
   *      execve("/bin/cat", argp, envp);
   *    }
   *
   * The parent sends a signal immediately after forking.
   * Since the child may not have called `execve()` yet,
   * there is no telling what process receives the signal,
   * our fork or /bin/cat.
   *
   * To avoid ambiguity, we create a pipe with both ends
   * marked close-on-exec. Then, after the call to `fork()`,
   * the parent polls the read end until it sees POLLHUP.
   */
#if SPAWN_WAIT_EXEC
# ifdef HAVE_PIPE2
  if (pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
    goto error;
  }
# else
  if (pipe(signal_pipe) < 0) {
    goto error;
  }
  uv__cloexec(signal_pipe[0], 1);
  uv__cloexec(signal_pipe[1], 1);
  uv__nonblock(signal_pipe[0], 1);
  uv__nonblock(signal_pipe[1], 1);
# endif
#endif

  pid = fork();

  if (pid == -1) {
#if SPAWN_WAIT_EXEC
    uv__close(signal_pipe[0]);
    uv__close(signal_pipe[1]);
#endif
    environ = save_our_env;
    goto error;
  }

  if (pid == 0) {
    if (stdin_pipe[0] >= 0) {
      uv__close(stdin_pipe[1]);
      dup2(stdin_pipe[0],  STDIN_FILENO);
    }

    if (stdout_pipe[1] >= 0) {
      uv__close(stdout_pipe[0]);
      dup2(stdout_pipe[1], STDOUT_FILENO);
    }

    if (stderr_pipe[1] >= 0) {
      uv__close(stderr_pipe[0]);
      dup2(stderr_pipe[1], STDERR_FILENO);
    }

    if (options.cwd && chdir(options.cwd)) {
      perror("chdir()");
      _exit(127);
    }

    environ = options.env;

    execvp(options.file, options.args);
    perror("execvp()");
    _exit(127);
    /* Execution never reaches here. */
  }

  /* Parent. */

  /* Restore environment. */
  environ = save_our_env;

#if SPAWN_WAIT_EXEC
  /* POLLHUP signals child has exited or execve()'d. */
  uv__close(signal_pipe[1]);
  do {
    pfd.fd = signal_pipe[0];
    pfd.events = POLLIN|POLLHUP;
    pfd.revents = 0;
    errno = 0, status = poll(&pfd, 1, -1);
  }
  while (status == -1 && (errno == EINTR || errno == ENOMEM));

  uv__close(signal_pipe[0]);
  uv__close(signal_pipe[1]);

  assert((status == 1)
      && "poll() on pipe read end failed");
  assert((pfd.revents & POLLHUP) == POLLHUP
      && "no POLLHUP on pipe read end");
#endif

  process->pid = pid;

  ev_child_init(&process->child_watcher, uv__chld, pid, 0);
  ev_child_start(process->loop->ev, &process->child_watcher);
  process->child_watcher.data = process;

  if (stdin_pipe[1] >= 0) {
    assert(options.stdin_stream);
    assert(stdin_pipe[0] >= 0);
    uv__close(stdin_pipe[0]);
    uv__nonblock(stdin_pipe[1], 1);
    uv__stream_open((uv_stream_t*)options.stdin_stream, stdin_pipe[1],
        UV_WRITABLE);
  }

  if (stdout_pipe[0] >= 0) {
    assert(options.stdout_stream);
    assert(stdout_pipe[1] >= 0);
    uv__close(stdout_pipe[1]);
    uv__nonblock(stdout_pipe[0], 1);
    uv__stream_open((uv_stream_t*)options.stdout_stream, stdout_pipe[0],
        UV_READABLE);
  }

  if (stderr_pipe[0] >= 0) {
    assert(options.stderr_stream);
    assert(stderr_pipe[1] >= 0);
    uv__close(stderr_pipe[1]);
    uv__nonblock(stderr_pipe[0], 1);
    uv__stream_open((uv_stream_t*)options.stderr_stream, stderr_pipe[0],
        UV_READABLE);
  }

  return 0;

error:
  uv_err_new(process->loop, errno);
  uv__close(stdin_pipe[0]);
  uv__close(stdin_pipe[1]);
  uv__close(stdout_pipe[0]);
  uv__close(stdout_pipe[1]);
  uv__close(stderr_pipe[0]);
  uv__close(stderr_pipe[1]);
  return -1;
}


int uv_process_kill(uv_process_t* process, int signum) {
  int r = kill(process->pid, signum);

  if (r) {
    uv_err_new(process->loop, errno);
    return -1;
  } else {
    return 0;
  }
}

static int sigchld_pipe[2];

#define MAX(a, b) ((a) > (b) ? (a) : (b))

void SyncCHLDHandler(int sig) {
  char c;
  assert(sig == SIGCHLD);
  assert(sigchld_pipe[0] >= 0);
  assert(sigchld_pipe[1] >= 0);

  write(sigchld_pipe[1], &c, 1);
}

int uv_spawn_sync(uv_loop_t* loop, uv_spawn_sync_t* spawn) {
  int stdin_pipe[2];
  int stdout_pipe[2];
  int stderr_pipe[2];
  int nfds;
  int64_t start_time;
  struct sigaction siga;
  static sigset_t sigset;
  fd_set read_fds;
  fd_set write_fds;

  struct timeval select_timeout;

  spawn->pid = -1;
  spawn->exit_code = -1;
  spawn->exit_signal = -1;
  spawn->exit_timeout = 0;
  spawn->stdout_read = 0;
  spawn->stderr_read = 0;

  if (spawn->stdin_buf && pipe(stdin_pipe)) {
    uv_err_new(loop, errno);
    return -1;
  }

  if (spawn->stdout_buf && pipe(stdout_pipe)) {
    uv_err_new(loop, errno);
    return -1;
  }

  if (spawn->stderr_buf && pipe(stderr_pipe)) {
    uv_err_new(loop, errno);
    return -1;
  }

  if (pipe(sigchld_pipe)) {
    uv_err_new(loop, errno);
    return -1;
  }

  switch (spawn->pid = fork()) {
    case -1:
      uv_err_new(loop, errno);
      return -1;

    case 0: /* child */
      if (spawn->stdin_buf) {
        close(stdin_pipe[1]); /* close write end */
        dup2(stdin_pipe[0], STDIN_FILENO);
      }

      if (spawn->stdout_buf) {
        close(stdout_pipe[0]); /* close read end */
        dup2(stdout_pipe[1], STDOUT_FILENO);

        if (spawn->combine) {
          dup2(stdout_pipe[1], STDERR_FILENO);
          assert(!spawn->stderr_buf);
        }
      }

      if (spawn->stderr_buf) {
        close(stderr_pipe[0]); /* close read end */
        dup2(stderr_pipe[1], STDERR_FILENO);
      }

      execvp(spawn->file, spawn->args);
      uv_err_new(loop, errno);
      _exit(127);
      break;
  }

  /* parent */
  nfds = sigchld_pipe[0];

  if (spawn->stdout_buf) {
    close(stdout_pipe[1]); /* close the write end */
    nfds = MAX(nfds, stdout_pipe[0]);
  }

  if (spawn->stderr_buf) {
    close(stderr_pipe[1]); /* close the write end */
    nfds = MAX(nfds, stderr_pipe[0]);
  }

  if (spawn->stdin_buf) {
    close(stdin_pipe[0]); /* close the read end */
    nfds = MAX(nfds, stdin_pipe[1]);
  }

  nfds = nfds + 1;
  start_time = uv_now(loop); /* time in ms */

  /* Set up sighandling */
  sigfillset(&sigset);
  siga.sa_handler = SyncCHLDHandler;
  siga.sa_mask = sigset;
  siga.sa_flags = 0;
  if (sigaction(SIGCHLD, &siga, (struct sigaction *)NULL) != 0) {
    uv_err_new(loop, errno);
    goto error;
  }

  while (1) {
    int64_t elapsed;
    int64_t time_to_timeout;
    int r;

    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);

    if (spawn->stdout_buf) {
      FD_SET(stdout_pipe[0], &read_fds);
    }

    if (spawn->stderr_buf) {
      FD_SET(stderr_pipe[0], &read_fds);
    }

    if (spawn->stdin_buf) {
      FD_SET(stdin_pipe[1], &write_fds);
    }

    FD_SET(sigchld_pipe[0], &read_fds);

    elapsed = uv_now(loop) - start_time;
    time_to_timeout = spawn->timeout - elapsed;
    if (time_to_timeout < 0) {
      /* TODO: is this the right thing todo, or does this mean we timed out? */
      time_to_timeout = 0;
    }

    select_timeout.tv_sec = time_to_timeout / 1000;
    select_timeout.tv_usec = time_to_timeout % 1000;

    r = select(nfds, &read_fds, &write_fds, NULL, &select_timeout);

    if (r == -1) {
      if (errno == EINTR) {
        continue;
      }

      uv_err_new(loop, errno);
      goto error;
    }

    if (r == 0) {
      /* timeout */
      close(sigchld_pipe[0]);
      close(sigchld_pipe[1]);

      if (spawn->stdin_buf) {
        close(stdin_pipe[1]);
      }

      if (spawn->stdout_buf) {
        close(stdout_pipe[0]);
      }

      if (spawn->stderr_buf) {
        close(stderr_pipe[0]);
      }

      kill(spawn->pid, SIGKILL);
      spawn->exit_timeout = 1;
      return 0;
    }

    if (spawn->stdin_buf && FD_ISSET(stdin_pipe[1], &write_fds) && spawn->stdin_written < spawn->stdin_size) {
      int written = write(stdin_pipe[1], spawn->stdin_buf + spawn->stdin_written, spawn->stdin_size - spawn->stdin_written);

      if (written == -1 && errno != EINTR) {
        uv_err_new(loop, errno);
        goto error;
      } else {
        spawn->stdin_written += written;
      }
    }

    if (spawn->stdout_buf && FD_ISSET(stdout_pipe[0], &read_fds)) {
      if (spawn->stdout_size - spawn->stdout_read <= 0) {
        /* Check for buffer overflow. */
        uv_err_new_artificial(loop, UV_ENOBUFS);
        goto error;
      }

      r = read(stdout_pipe[0], spawn->stdout_buf + spawn->stdout_read, spawn->stdout_size - spawn->stdout_read);

      /* @TODO HANDLE EAGAIN / EINTR */
      if (r == -1) {
        uv_err_new(loop, errno);
        goto error;
      }

      spawn->stdout_read += r;
    }

    if (spawn->stderr_buf && FD_ISSET(stderr_pipe[0], &read_fds)) {
      if (spawn->stderr_size - spawn->stderr_read <= 0) {
        /* Check for buffer overflow. */
        uv_err_new_artificial(loop, UV_ENOBUFS);
        goto error;
      }

      r = read(stderr_pipe[0], spawn->stderr_buf + spawn->stderr_read, spawn->stderr_size - spawn->stderr_read);
      if (r == -1) {
        uv_err_new(loop, errno);
        goto error;
      }

      spawn->stderr_read += r;
    }

    if (FD_ISSET(sigchld_pipe[0], &read_fds)) {
      /* The child process has exited. */
      int status;

      pid_t p = wait(&status);
      if (p < 0) {
        uv_err_new(loop, errno);
        goto error;
      }

      close(sigchld_pipe[0]);
      close(sigchld_pipe[1]);

      if (spawn->stdin_buf) {
        close(stdin_pipe[1]);
      }

      if (spawn->stdout_buf) {
        close(stdout_pipe[0]);
      }

      if (spawn->stderr_buf) {
        close(stderr_pipe[0]);
      }

      if (WIFEXITED(status)) {
        spawn->exit_code = WEXITSTATUS(status);
      }

      if (WIFSIGNALED(status)) {
        spawn->exit_signal = WTERMSIG(status);
      }

      return 0;
    }

    uv_update_time(loop);
  }

error:
  close(sigchld_pipe[0]);
  close(sigchld_pipe[1]);

  if (spawn->stdin_buf) {
    close(stdin_pipe[1]);
  }

  if (spawn->stdout_buf) {
    close(stdout_pipe[0]);
  }

  if (spawn->stderr_buf) {
    close(stderr_pipe[0]);
  }

  kill(spawn->pid, SIGKILL);
  return -1;
}
