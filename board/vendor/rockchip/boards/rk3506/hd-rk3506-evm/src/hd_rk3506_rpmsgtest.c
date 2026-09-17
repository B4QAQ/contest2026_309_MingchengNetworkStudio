/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_rpmsgtest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * rpmsgtest - NSH builtin acceptance test for the A7<->M0 rpmsg link.
 *
 * Flow:
 *   1. open /dev/rpmsg/mcu            (rpmsg ctrl device; created by the
 *                                      virtio rpmsg driver when the rptun
 *                                      master came up)
 *   2. RPMSG_CREATE_EPT_IOCTL with name "rpmsg-mcu0-echo", src 0x30,
 *      dst 0x4003 (the endpoint the SDK MCU demo creates).  With explicit
 *      src+dst the endpoint binds immediately; this registers
 *      /dev/rpmsg-rpmsg-mcu0-echo.  The name deliberately differs from
 *      the demo's NS announcement ("rpmsg-mcu0-test"): once the M0
 *      announces, the A7 already creates /dev/rpmsg-rpmsg-mcu0-test
 *      from the name-service callback, and a second endpoint with the
 *      same name would collide with that node.  Routing is by dst
 *      0x4003 either way.
 *   3. write() the test payload; the MCU echoes its canned reply
 *      "Rockchip rpmsg linux test!" (26 bytes with the trailing NUL,
 *      sent twice by the demo: once from the RX callback and once from
 *      the main loop).
 *   4. poll() for readability with the remaining time, read() the
 *      replies and check that one matches the canned string; print
 *      PASS/FAIL.  (A plain blocking read() hung forever when the MCU
 *      never replied - the deadline check sat after the read, out of
 *      reach.)
 *
 * Usage: rpmsgtest [-d /dev/rpmsg/mcu] [-t seconds]
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/rpmsg/rpmsg.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RPMSGTEST_CTRL_PATH       "/dev/rpmsg/mcu"
#define RPMSGTEST_EPT_NAME        "rpmsg-mcu0-echo"
#define RPMSGTEST_EPT_SRC         0x30
#define RPMSGTEST_EPT_DST         0x4003
#define RPMSGTEST_PAYLOAD         "ping from vela"
#define RPMSGTEST_REPLY           "Rockchip rpmsg linux test!"
#define RPMSGTEST_BUF_SIZE        512
#define RPMSGTEST_DEFAULT_TIMEOUT 10   /* seconds */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rpmsgtest_env_s
{
  FAR const char *ctrlpath;
  int timeout;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void rpmsgtest_usage(FAR const char *progname)
{
  printf("Usage: %s [-d ctrlpath] [-t seconds]\n", progname);
  printf("  -d  rpmsg ctrl device (default: %s)\n", RPMSGTEST_CTRL_PATH);
  printf("  -t  timeout in seconds (default: %d)\n",
         RPMSGTEST_DEFAULT_TIMEOUT);
}

static void rpmsgtest_parse_args(int argc, FAR char **argv,
                                 FAR struct rpmsgtest_env_s *env)
{
  int opt;

  env->ctrlpath = RPMSGTEST_CTRL_PATH;
  env->timeout  = RPMSGTEST_DEFAULT_TIMEOUT;

  while ((opt = getopt(argc, argv, "d:t:")) != ERROR)
    {
      switch (opt)
        {
          case 'd':
            env->ctrlpath = optarg;
            break;
          case 't':
            env->timeout = atoi(optarg);
            if (env->timeout <= 0)
              {
                env->timeout = RPMSGTEST_DEFAULT_TIMEOUT;
              }
            break;
          default:
            rpmsgtest_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct rpmsg_endpoint_info eptinfo;
  struct rpmsgtest_env_s env;
  char eptpath[64];
  char rxbuf[RPMSGTEST_BUF_SIZE];
  struct timespec start;
  int64_t remaining_ms;
  bool pass = false;
  ssize_t n;
  int ctrlfd;
  int eptfd;
  int ret;

  rpmsgtest_parse_args(argc, argv, &env);

  /* 1. Open the rpmsg ctrl device. */

  ctrlfd = open(env.ctrlpath, O_RDWR);
  if (ctrlfd < 0)
    {
      fprintf(stderr, "rpmsgtest: open %s failed: %d "
              "(is the rptun up? see dmesg)\n", env.ctrlpath, errno);
      return EXIT_FAILURE;
    }

  /* 2. Create the endpoint bound to the MCU demo's 0x4003 endpoint. */

  memset(&eptinfo, 0, sizeof(eptinfo));
  strlcpy(eptinfo.name, RPMSGTEST_EPT_NAME, sizeof(eptinfo.name));
  eptinfo.src = RPMSGTEST_EPT_SRC;
  eptinfo.dst = RPMSGTEST_EPT_DST;

  ret = ioctl(ctrlfd, RPMSG_CREATE_EPT_IOCTL, (unsigned long)&eptinfo);
  close(ctrlfd);
  if (ret < 0)
    {
      fprintf(stderr, "rpmsgtest: RPMSG_CREATE_EPT_IOCTL failed: %d\n",
              errno);
      return EXIT_FAILURE;
    }

  snprintf(eptpath, sizeof(eptpath), "/dev/rpmsg-%s", eptinfo.name);
  eptfd = open(eptpath, O_RDWR);
  if (eptfd < 0)
    {
      fprintf(stderr, "rpmsgtest: open %s failed: %d\n", eptpath, errno);
      return EXIT_FAILURE;
    }

  printf("rpmsgtest: endpoint %s (src 0x%x -> dst 0x%x) created, "
         "sending payload...\n", eptpath, RPMSGTEST_EPT_SRC,
         RPMSGTEST_EPT_DST);

  /* 3. Send the payload.  The MCU replies to the sender's source
   *    address with its canned test message. */

  n = write(eptfd, RPMSGTEST_PAYLOAD, sizeof(RPMSGTEST_PAYLOAD));
  if (n < 0)
    {
      fprintf(stderr, "rpmsgtest: write failed: %d\n", errno);
      close(eptfd);
      return EXIT_FAILURE;
    }

  printf("rpmsgtest: sent \"%s\" (%zd bytes), waiting for reply "
         "(%ds timeout)...\n", RPMSGTEST_PAYLOAD, n, env.timeout);

  /* 4. Collect replies until the canned string is seen or timeout.
   *    poll() with the remaining time: a plain blocking read() would
   *    hang forever when the MCU never replies - the old deadline check
   *    sat after the read, unreachable while blocked (v7 defect).
   */

  clock_gettime(CLOCK_MONOTONIC, &start);
  remaining_ms = (int64_t)env.timeout * 1000;
  for (;;)
    {
      struct pollfd pfd;
      struct timespec now_ts;
      int64_t spent_ms;
      int pret;

      pfd.fd      = eptfd;
      pfd.events  = POLLIN;
      pfd.revents = 0;

      pret = poll(&pfd, 1, remaining_ms > INT32_MAX ? INT32_MAX
                                                    : (int)remaining_ms);
      clock_gettime(CLOCK_MONOTONIC, &now_ts);
      spent_ms = (now_ts.tv_sec - start.tv_sec) * 1000 +
                 (now_ts.tv_nsec - start.tv_nsec) / 1000000;
      remaining_ms = (int64_t)env.timeout * 1000 - spent_ms;

      if (pret < 0)
        {
          if (errno == EINTR && remaining_ms > 0)
            {
              continue;
            }

          fprintf(stderr, "rpmsgtest: poll failed: %d\n", errno);
          break;
        }

      if (pret == 0)
        {
          fprintf(stderr, "rpmsgtest: timed out after %ds waiting for "
                  "the MCU reply\n", env.timeout);
          break;
        }

      memset(rxbuf, 0, sizeof(rxbuf));
      n = read(eptfd, rxbuf, sizeof(rxbuf) - 1);
      if (n < 0)
        {
          if ((errno == EINTR || errno == EAGAIN) && remaining_ms > 0)
            {
              continue;
            }

          fprintf(stderr, "rpmsgtest: read failed: %d\n", errno);
          break;
        }

      printf("rpmsgtest: reply (%zd bytes): \"%s\"\n", n, rxbuf);

      if (n >= (ssize_t)strlen(RPMSGTEST_REPLY) &&
          strncmp(rxbuf, RPMSGTEST_REPLY, strlen(RPMSGTEST_REPLY)) == 0)
        {
          pass = true;
          break;
        }

      if (remaining_ms <= 0)
        {
          fprintf(stderr, "rpmsgtest: timed out after %ds\n", env.timeout);
          break;
        }
    }

  close(eptfd);

  printf("rpmsgtest: %s\n", pass ? "PASS" : "FAIL");
  return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
