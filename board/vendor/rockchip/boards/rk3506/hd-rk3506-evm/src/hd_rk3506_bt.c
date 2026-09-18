/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_bt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Diagnostic NSH command to dump a task's call stack.
 * Usage: bt <pid>
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>

int main(int argc, char *argv[])
{
  int pid;

  if (argc < 2)
    {
      fprintf(stderr, "usage: bt <pid>\n");
      return 1;
    }

  pid = atoi(argv[1]);
  if (pid < 0)
    {
      fprintf(stderr, "invalid pid: %s\n", argv[1]);
      return 1;
    }

  sched_dumpstack(pid);
  return 0;
}
