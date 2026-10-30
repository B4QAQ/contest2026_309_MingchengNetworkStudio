/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_rpmsgtest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NSH "rpmsgtest": verify the rpmsg link to the bus Cortex-M0.
 *
 * Follows the upstream apps/examples/rpmsgchar flow: open the rpmsg
 * control device, create a fixed endpoint with RPMSG_CREATE_EPT_IOCTL,
 * open the resulting endpoint device and wait for data.
 *
 * The M0 runs the unmodified SDK rpmsg-lite Linux test
 * (hal/project/rk3506-mcu test_demo.c): it announces the
 * "rpmsg-mcu0-test" service at endpoint 0x4003 and, once the master
 * endpoint is NS-bound, sends "Rockchip rpmsg linux test!" (26 bytes
 * including the terminating NUL) without being prompted.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/rpmsg/rpmsg.h>

#ifdef CONFIG_HD_RK3506_RPMSGTEST

#define RPMSGTEST_CTRL_DEV    "/dev/rpmsg/mcu"
#define RPMSGTEST_EPT_NAME    "rpmsg-mcu0-test"
#define RPMSGTEST_EPT_SRC     0x30
#define RPMSGTEST_EPT_DST     0x4003
#define RPMSGTEST_TIMEOUT_MS  5000

#define RPMSGTEST_PING        "ping from vela"
#define RPMSGTEST_EXPECTED    "Rockchip rpmsg linux test!"
#define RPMSGTEST_EXPECTED_LEN 26

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct rpmsg_endpoint_info eptinfo;
  struct pollfd pfd;
  char eptdev[32];
  char buf[64];
  int ctrlfd;
  int eptfd;
  int ret;
  int rc = 1;
  ssize_t n;

  ctrlfd = open(RPMSGTEST_CTRL_DEV, O_RDWR);
  if (ctrlfd < 0)
    {
      fprintf(stderr, "rpmsgtest: open %s failed: %d\n",
              RPMSGTEST_CTRL_DEV, errno);
      return 1;
    }

  memset(&eptinfo, 0, sizeof(eptinfo));
  strlcpy(eptinfo.name, RPMSGTEST_EPT_NAME, sizeof(eptinfo.name));
  eptinfo.src = RPMSGTEST_EPT_SRC;
  eptinfo.dst = RPMSGTEST_EPT_DST;

  ret = ioctl(ctrlfd, RPMSG_CREATE_EPT_IOCTL,
              (unsigned long)&eptinfo);
  if (ret < 0)
    {
      fprintf(stderr, "rpmsgtest: create endpoint failed: %d\n", errno);
      close(ctrlfd);
      return 1;
    }

  snprintf(eptdev, sizeof(eptdev), "/dev/rpmsg-%s", RPMSGTEST_EPT_NAME);
  eptfd = open(eptdev, O_RDWR);
  if (eptfd < 0)
    {
      fprintf(stderr, "rpmsgtest: open %s failed: %d\n", eptdev, errno);
      goto out_destroy;
    }

  /* The M0 demo only sends its reply after receiving a data message
   * on endpoint 0x4003 (remote_ept_cb sets cb_sta, then replies).
   */

  n = write(eptfd, RPMSGTEST_PING, strlen(RPMSGTEST_PING));
  if (n != (ssize_t)strlen(RPMSGTEST_PING))
    {
      fprintf(stderr, "rpmsgtest: write failed: %d\n", errno);
      goto out_close;
    }

  pfd.fd     = eptfd;
  pfd.events = POLLIN;
  ret = poll(&pfd, 1, RPMSGTEST_TIMEOUT_MS);
  if (ret <= 0)
    {
      fprintf(stderr, "rpmsgtest: %s\n",
              ret == 0 ? "timeout waiting for M0 message" : "poll failed");
      goto out_close;
    }

  n = read(eptfd, buf, sizeof(buf) - 1);
  if (n <= 0)
    {
      fprintf(stderr, "rpmsgtest: read failed: %d\n", errno);
      goto out_close;
    }

  buf[n] = '\0';
  printf("rpmsgtest: reply (%zd bytes): \"%s\"\n", n, buf);

  if (strncmp(buf, RPMSGTEST_EXPECTED, RPMSGTEST_EXPECTED_LEN) != 0)
    {
      fprintf(stderr, "rpmsgtest: unexpected reply\n");
      goto out_close;
    }

  printf("rpmsgtest: PASS\n");
  rc = 0;

out_close:
  ioctl(eptfd, RPMSG_DESTROY_EPT_IOCTL, 0);
  close(eptfd);
out_destroy:
  close(ctrlfd);
  return rc;
}

#endif /* CONFIG_HD_RK3506_RPMSGTEST */
