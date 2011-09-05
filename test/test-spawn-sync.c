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
#include "task.h"
#include <stdio.h>
#include <stdlib.h>

static char exepath[1024];
static size_t exepath_size = 1024;
static uv_sync_process_t process;
static uv_sync_process_options_t options = { 0 };
static char* args[3];

static void init_process_options(char* test) {
  /* Note spawn_helper1 defined in test/run-tests.c */
  int r = uv_exepath(exepath, &exepath_size);
  ASSERT(r == 0);
  exepath[exepath_size] = '\0';
  args[0] = exepath;
  args[1] = test;
  args[2] = NULL;

  options.file = exepath;
  options.args = args;
}

TEST_IMPL(spawn_sync_exit_code) {
  int r;
  uv_init();

  init_process_options("spawn_helper1");

  r = uv_spawn_sync(uv_default_loop(), &process, options);

  ASSERT(process.pid >= 0);
  ASSERT(r == 0);
  ASSERT(process.exit_code == 1);

  fprintf(stdout, "Fuck!\n");

  return 0;
}
