/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_timetest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Diagnostic NSH command to isolate curl hang root cause.
 * Tests whether poll/select/nanosleep timeout paths work correctly.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <poll.h>
#include <sys/select.h>
#include <semaphore.h>
#include <errno.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct timespec ts_start;
  struct timespec ts_end;
  struct timespec ts_sleep;
  struct timeval tv;
  unsigned long elapsed_ms;
  int ret;
  sem_t sem;

  printf("=== timetest: kernel timeout diagnostics ===\n");

  /* 1. poll(NULL, 0, 1000) — the suspect path (Curl_wait_ms) */

  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  ret = poll(NULL, 0, 1000);
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 +
               (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
  printf("[poll]       ret=%d errno=%d elapsed=%lums (expect ~1000ms)\n",
         ret, errno, elapsed_ms);

  /* 2. select(0, NULL, NULL, NULL, {1s}) — what Curl_wait_ms actually calls */

  tv.tv_sec = 1;
  tv.tv_usec = 0;
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  ret = select(0, NULL, NULL, NULL, &tv);
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 +
               (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
  printf("[select]     ret=%d errno=%d elapsed=%lums (expect ~1000ms)\n",
         ret, errno, elapsed_ms);

  /* 3. nanosleep(1s) — control group, known working */

  ts_sleep.tv_sec = 1;
  ts_sleep.tv_nsec = 0;
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  ret = nanosleep(&ts_sleep, NULL);
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 +
               (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
  printf("[nanosleep]  ret=%d errno=%d elapsed=%lums (expect ~1000ms)\n",
         ret, errno, elapsed_ms);

  /* 4. sem_timedwait with 1s timeout — tests wd_start on semaphore */

  sem_init(&sem, 0, 0);
  ts_sleep.tv_sec = 1;
  ts_sleep.tv_nsec = 0;
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  ret = sem_timedwait(&sem, &ts_sleep);
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 +
               (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
  printf("[sem_timed]  ret=%d errno=%d elapsed=%lums (expect ~1000ms, ret=-1,errno=ETIMEDOUT)\n",
         ret, errno, elapsed_ms);
  sem_destroy(&sem);

  printf("=== timetest done ===\n");
  return 0;
}
