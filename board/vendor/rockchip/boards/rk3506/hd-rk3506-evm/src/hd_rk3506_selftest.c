/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_selftest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NSH "selftest": one-shot board health check.
 *
 *   nsh> selftest            # run every section
 *   nsh> selftest mem        # heap usage           (mallinfo)
 *   nsh> selftest cpu        # CPU load             (clock_cpuload)
 *   nsh> selftest fs         # filesystem capacity  (statfs)
 *   nsh> selftest net        # interface + DNS + TCP connect
 *   nsh> selftest rpmsg      # bus-M0 rpmsg link
 *
 * Every section prints its own detail lines and ends in PASS / FAIL /
 * SKIP; the summary line at the bottom repeats the counts so the whole
 * thing survives a screen-scroll during a live demo.
 *
 * The rpmsg section deliberately uses source address 0x31 (rpmsgtest
 * uses 0x30) so both commands can be used back to back without the
 * second one tripping over a stale endpoint.
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/rpmsg/rpmsg.h>

#ifdef CONFIG_HD_RK3506_SELFTEST

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_HD_RK3506_SELFTEST_NET_IFACE
#  define CONFIG_HD_RK3506_SELFTEST_NET_IFACE "eth0"
#endif

#ifndef CONFIG_HD_RK3506_SELFTEST_NET_HOST
#  define CONFIG_HD_RK3506_SELFTEST_NET_HOST "www.baidu.com"
#endif

#ifndef CONFIG_HD_RK3506_SELFTEST_NET_PORT
#  define CONFIG_HD_RK3506_SELFTEST_NET_PORT 443
#endif

#define ST_CPU_WINDOW_MS     500
#define ST_RESOLVE_TIMEOUT   5
#define ST_CONNECT_TIMEOUT   5000

#define ST_RPMSG_CTRL_DEV    "/dev/rpmsg/mcu"
#define ST_RPMSG_EPT_NAME    "rpmsg-mcu0-test"
#define ST_RPMSG_EPT_SRC     0x31
#define ST_RPMSG_EPT_DST     0x4003
#define ST_RPMSG_TIMEOUT_MS  5000
#define ST_RPMSG_PING        "ping from vela"
#define ST_RPMSG_EXPECTED    "Rockchip rpmsg linux test!"
#define ST_RPMSG_EXPECTED_LEN 26

/* Section results */

#define ST_PASS   0
#define ST_FAIL   1
#define ST_SKIP   2

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t st_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void st_result(const char *name, int rc)
{
  printf("[%s] %s\n\n", name,
         rc == ST_PASS ? "PASS" : rc == ST_SKIP ? "SKIP" : "FAIL");
}

/****************************************************************************
 * Section: memory
 ****************************************************************************/

static int st_mem(void)
{
  struct mallinfo info;
  unsigned int used_pct;

  info = mallinfo();

  if (info.arena == 0)
    {
      printf("[mem] mallinfo returned an empty arena\n");
      return ST_FAIL;
    }

  used_pct = (unsigned int)(((uint64_t)info.uordblks * 100) / info.arena);

  printf("[mem] arena   : %u KiB (%u.%u MiB) across all heap regions\n",
         info.arena / 1024, info.arena / 1048576,
         (info.arena % 1048576) / 104858);
  printf("[mem] in use  : %u KiB (%u%% of arena)\n",
         info.uordblks / 1024, used_pct);
  printf("[mem] free    : %u KiB, largest block %u KiB\n",
         info.fordblks / 1024, info.mxordblk / 1024);
  printf("[mem] chunks  : %u allocated / %u free\n",
         info.aordblks, info.ordblks);

  if (info.fordblks == 0 || info.mxordblk == 0)
    {
      printf("[mem] no free chunk available - heap exhausted\n");
      return ST_FAIL;
    }

  return ST_PASS;
}

/****************************************************************************
 * Section: CPU load
 ****************************************************************************/

static int st_cpu(void)
{
#ifndef CONFIG_SCHED_CPULOAD_NONE
  struct cpuload_s first;
  struct cpuload_s last;
  uint64_t total;
  uint64_t idle;
  uint64_t busy;
  unsigned int busy_pm;
  clock_t uptime;

  if (clock_cpuload(0, &first) < 0)
    {
      printf("[cpu] clock_cpuload(0) failed: %d\n", errno);
      return ST_FAIL;
    }

  printf("[cpu] sampling the IDLE thread for %d ms ...\n", ST_CPU_WINDOW_MS);
  usleep(ST_CPU_WINDOW_MS * 1000);

  if (clock_cpuload(0, &last) < 0)
    {
      printf("[cpu] clock_cpuload(0) failed: %d\n", errno);
      return ST_FAIL;
    }

  total = (uint64_t)(last.total - first.total);
  idle  = (uint64_t)(last.active - first.active);

  if (total == 0)
    {
      printf("[cpu] no scheduler ticks were accounted in the window\n");
      return ST_FAIL;
    }

  busy    = total > idle ? total - idle : 0;
  busy_pm = (unsigned int)((busy * 1000) / total);

  printf("[cpu] busy    : %u.%u%%\n",
         busy_pm / 10, busy_pm % 10);
  printf("[cpu] idle    : %u.%u%%\n",
         (1000 - busy_pm) / 10, (1000 - busy_pm) % 10);
  printf("[cpu] ticks   : %lu of %lu were idle in the window\n",
         (unsigned long)idle, (unsigned long)total);

  uptime = clock_systime_ticks() / CLOCKS_PER_SEC;
  printf("[cpu] uptime  : %lu s (%lu Hz system timer)\n",
         (unsigned long)uptime, (unsigned long)CLOCKS_PER_SEC);
  printf("[cpu] per-task breakdown: run 'ps'\n");

  return ST_PASS;
#else
  printf("[cpu] CONFIG_SCHED_CPULOAD_NONE - load accounting is disabled\n");
  return ST_SKIP;
#endif
}

/****************************************************************************
 * Section: filesystem capacity
 ****************************************************************************/

static int st_fs(void)
{
  static const char * const mountpoints[] =
  {
    "/", "/data", "/etc", "/tmp"
  };

  struct statfs buf;
  unsigned int i;
  bool any = false;
  bool fail = false;

  for (i = 0; i < sizeof(mountpoints) / sizeof(mountpoints[0]); i++)
    {
      memset(&buf, 0, sizeof(buf));

      if (statfs(mountpoints[i], &buf) < 0)
        {
          printf("[fs ] %-6s: not mounted or statfs unsupported (%d)\n",
                 mountpoints[i], errno);
          continue;
        }

      any = true;

      if (buf.f_blocks == 0 || buf.f_bsize == 0)
        {
          /* Read-only pseudo filesystems (romfs) legitimately report no
           * capacity; print the type and move on.
           */

          printf("[fs ] %-6s: block size %lu, no capacity accounting\n",
                 mountpoints[i], (unsigned long)buf.f_bsize);
          continue;
        }

      printf("[fs ] %-6s: block %lu B, %lu of %lu blocks free (%lu KiB free)\n",
             mountpoints[i], (unsigned long)buf.f_bsize,
             (unsigned long)buf.f_bfree, (unsigned long)buf.f_blocks,
             (unsigned long)((uint64_t)buf.f_bfree * buf.f_bsize / 1024));

      if (buf.f_bfree == 0)
        {
          printf("[fs ] %-6s: no free blocks - filesystem is full\n",
                 mountpoints[i]);
          fail = true;
        }
    }

  if (!any)
    {
      return ST_FAIL;
    }

  return fail ? ST_FAIL : ST_PASS;
}

/****************************************************************************
 * Section: network
 ****************************************************************************/

static bool st_iface_info(void)
{
  struct ifreq ifr;
  struct sockaddr_in *sin;
  bool ok = false;
  int sd;

  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sd < 0)
    {
      printf("[net] socket() failed: %d\n", errno);
      return false;
    }

  memset(&ifr, 0, sizeof(ifr));
  strlcpy(ifr.ifr_name, CONFIG_HD_RK3506_SELFTEST_NET_IFACE,
          sizeof(ifr.ifr_name));

  if (ioctl(sd, SIOCGIFHWADDR, (unsigned long)&ifr) == 0)
    {
      printf("[net] %s hwaddr : %02x:%02x:%02x:%02x:%02x:%02x\n",
             ifr.ifr_name,
             (unsigned char)ifr.ifr_hwaddr.sa_data[0],
             (unsigned char)ifr.ifr_hwaddr.sa_data[1],
             (unsigned char)ifr.ifr_hwaddr.sa_data[2],
             (unsigned char)ifr.ifr_hwaddr.sa_data[3],
             (unsigned char)ifr.ifr_hwaddr.sa_data[4],
             (unsigned char)ifr.ifr_hwaddr.sa_data[5]);
    }

  memset(&ifr, 0, sizeof(ifr));
  strlcpy(ifr.ifr_name, CONFIG_HD_RK3506_SELFTEST_NET_IFACE,
          sizeof(ifr.ifr_name));

  if (ioctl(sd, SIOCGIFADDR, (unsigned long)&ifr) == 0)
    {
      sin = (struct sockaddr_in *)&ifr.ifr_addr;
      printf("[net] %s address: %s\n", ifr.ifr_name,
             inet_ntoa(sin->sin_addr));
      ok = sin->sin_addr.s_addr != 0;
    }
  else
    {
      printf("[net] %s has no IPv4 address yet (%d)\n",
             CONFIG_HD_RK3506_SELFTEST_NET_IFACE, errno);
    }

  memset(&ifr, 0, sizeof(ifr));
  strlcpy(ifr.ifr_name, CONFIG_HD_RK3506_SELFTEST_NET_IFACE,
          sizeof(ifr.ifr_name));

  if (ioctl(sd, SIOCGIFNETMASK, (unsigned long)&ifr) == 0)
    {
      sin = (struct sockaddr_in *)&ifr.ifr_netmask;
      printf("[net] netmask  : %s\n", inet_ntoa(sin->sin_addr));
    }

  close(sd);
  return ok;
}

static int st_net(void)
{
  struct addrinfo hints;
  FAR struct addrinfo *res = NULL;
  FAR struct addrinfo *ai;
  struct pollfd pfd;
  uint64_t t0;
  uint64_t resolve_ms;
  uint64_t connect_ms;
  char portstr[8];
  int sd = -1;
  int ret;
  int err = 0;
  socklen_t errlen = sizeof(err);
  int rc = ST_FAIL;

  if (!st_iface_info())
    {
      printf("[net] interface is down or unconfigured - "
             "run 'ifconfig %s dhcp'\n",
             CONFIG_HD_RK3506_SELFTEST_NET_IFACE);
      return ST_FAIL;
    }

  /* DNS resolution */

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  t0 = st_now_ms();
  ret = getaddrinfo(CONFIG_HD_RK3506_SELFTEST_NET_HOST, NULL, &hints, &res);
  resolve_ms = st_now_ms() - t0;

  if (ret != 0 || res == NULL)
    {
      printf("[net] DNS resolve %s failed: %d (%lu ms)\n",
             CONFIG_HD_RK3506_SELFTEST_NET_HOST, ret,
             (unsigned long)resolve_ms);
      printf("[net] check the nameserver: 'ifconfig' + 'cat /etc/resolv.conf'"
             " or re-run dhcp\n");
      return ST_FAIL;
    }

  for (ai = res; ai != NULL; ai = ai->ai_next)
    {
      if (ai->ai_family == AF_INET)
        {
          break;
        }
    }

  if (ai == NULL)
    {
      printf("[net] DNS returned no IPv4 address\n");
      freeaddrinfo(res);
      return ST_FAIL;
    }

  printf("[net] DNS       : %s -> %s in %lu ms\n",
         CONFIG_HD_RK3506_SELFTEST_NET_HOST,
         inet_ntoa(((struct sockaddr_in *)ai->ai_addr)->sin_addr),
         (unsigned long)resolve_ms);

  /* TCP connect */

  snprintf(portstr, sizeof(portstr), "%d",
           CONFIG_HD_RK3506_SELFTEST_NET_PORT);
  ((struct sockaddr_in *)ai->ai_addr)->sin_port =
      htons(CONFIG_HD_RK3506_SELFTEST_NET_PORT);

  sd = socket(AF_INET, SOCK_STREAM, 0);
  if (sd < 0)
    {
      printf("[net] socket() failed: %d\n", errno);
      freeaddrinfo(res);
      return ST_FAIL;
    }

  ret = fcntl(sd, F_SETFL, fcntl(sd, F_GETFL, 0) | O_NONBLOCK);
  if (ret < 0)
    {
      printf("[net] fcntl(O_NONBLOCK) failed: %d\n", errno);
      close(sd);
      freeaddrinfo(res);
      return ST_FAIL;
    }

  t0 = st_now_ms();
  ret = connect(sd, ai->ai_addr, ai->ai_addrlen);

  if (ret < 0 && errno == EINPROGRESS)
    {
      pfd.fd     = sd;
      pfd.events = POLLOUT;

      ret = poll(&pfd, 1, ST_CONNECT_TIMEOUT);
      if (ret <= 0)
        {
          printf("[net] TCP connect to port %d timed out (%d ms)\n",
                 CONFIG_HD_RK3506_SELFTEST_NET_PORT, ST_CONNECT_TIMEOUT);
          goto out;
        }

      if (getsockopt(sd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0)
        {
          printf("[net] getsockopt(SO_ERROR) failed: %d\n", errno);
          goto out;
        }

      if (err != 0)
        {
          printf("[net] TCP connect to port %d failed: %d\n",
                 CONFIG_HD_RK3506_SELFTEST_NET_PORT, err);
          goto out;
        }
    }
  else if (ret < 0)
    {
      printf("[net] TCP connect to port %d failed: %d\n",
             CONFIG_HD_RK3506_SELFTEST_NET_PORT, errno);
      goto out;
    }

  connect_ms = st_now_ms() - t0;
  printf("[net] TCP       : port %d connected in %lu ms\n",
         CONFIG_HD_RK3506_SELFTEST_NET_PORT, (unsigned long)connect_ms);
  printf("[net] TLS/HTTPS : not exercised here, use "
         "'curl -sS https://%s/ -o /dev/null'\n",
         CONFIG_HD_RK3506_SELFTEST_NET_HOST);
  rc = ST_PASS;

out:
  if (sd >= 0)
    {
      close(sd);
    }

  if (res != NULL)
    {
      freeaddrinfo(res);
    }

  return rc;
}

/****************************************************************************
 * Section: bus-M0 rpmsg link
 ****************************************************************************/

static int st_rpmsg(void)
{
#if defined(CONFIG_RK3506_RPTUN) && defined(CONFIG_RPMSG_CHAR)
  struct rpmsg_endpoint_info eptinfo;
  struct pollfd pfd;
  char eptdev[48];
  char buf[64];
  ssize_t n;
  int ctrlfd;
  int eptfd;
  int ret;
  int rc = ST_FAIL;

  ctrlfd = open(ST_RPMSG_CTRL_DEV, O_RDWR);
  if (ctrlfd < 0)
    {
      printf("[rpm] open %s failed: %d (is the rptun link up?)\n",
             ST_RPMSG_CTRL_DEV, errno);
      return ST_FAIL;
    }

  memset(&eptinfo, 0, sizeof(eptinfo));
  strlcpy(eptinfo.name, ST_RPMSG_EPT_NAME, sizeof(eptinfo.name));
  eptinfo.src = ST_RPMSG_EPT_SRC;
  eptinfo.dst = ST_RPMSG_EPT_DST;

  ret = ioctl(ctrlfd, RPMSG_CREATE_EPT_IOCTL, (unsigned long)&eptinfo);
  if (ret < 0)
    {
      printf("[rpm] create endpoint %s failed: %d\n", ST_RPMSG_EPT_NAME, errno);
      close(ctrlfd);
      return ST_FAIL;
    }

  snprintf(eptdev, sizeof(eptdev), "/dev/rpmsg-%s", ST_RPMSG_EPT_NAME);

  eptfd = open(eptdev, O_RDWR);
  if (eptfd < 0)
    {
      printf("[rpm] open %s failed: %d\n", eptdev, errno);
      goto out_destroy;
    }

  printf("[rpm] endpoint up: src 0x%02x -> dst 0x%04x\n",
         ST_RPMSG_EPT_SRC, ST_RPMSG_EPT_DST);

  n = write(eptfd, ST_RPMSG_PING, strlen(ST_RPMSG_PING));
  if (n != (ssize_t)strlen(ST_RPMSG_PING))
    {
      printf("[rpm] write failed: %d\n", errno);
      goto out_close;
    }

  pfd.fd     = eptfd;
  pfd.events = POLLIN;

  ret = poll(&pfd, 1, ST_RPMSG_TIMEOUT_MS);
  if (ret <= 0)
    {
      printf("[rpm] %s\n",
             ret == 0 ? "timeout waiting for the M0 reply" : "poll failed");
      goto out_close;
    }

  n = read(eptfd, buf, sizeof(buf) - 1);
  if (n <= 0)
    {
      printf("[rpm] read failed: %d\n", errno);
      goto out_close;
    }

  buf[n] = '\0';
  printf("[rpm] reply: %zd bytes \"%s\"\n", n, buf);

  if (strncmp(buf, ST_RPMSG_EXPECTED, ST_RPMSG_EXPECTED_LEN) != 0)
    {
      printf("[rpm] reply does not match \"%s\"\n", ST_RPMSG_EXPECTED);
      goto out_close;
    }

  rc = ST_PASS;

out_close:
  ioctl(eptfd, RPMSG_DESTROY_EPT_IOCTL, 0);
  close(eptfd);
out_destroy:
  close(ctrlfd);
  return rc;
#else
  printf("[rpm] CONFIG_RK3506_RPTUN / CONFIG_RPMSG_CHAR disabled\n");
  return ST_SKIP;
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  const char *sel = (argc > 1) ? argv[1] : "all";
  int pass = 0;
  int fail = 0;
  int skip = 0;
  int rc;
  bool all;

  all = strcmp(sel, "all") == 0;

  if (argc > 1 && strcmp(sel, "help") == 0)
    {
      printf("usage: selftest [all|mem|cpu|fs|net|rpmsg]\n"
             "  mem    heap usage across all regions (mallinfo)\n"
             "  cpu    CPU busy/idle over a %d ms window (clock_cpuload)\n"
             "  fs     capacity of / /data /etc /tmp (statfs)\n"
             "  net    interface, DNS and TCP connect to %s:%d\n"
             "  rpmsg  bus-M0 rpmsg round trip\n"
             "  all    every section above (default)\n",
             ST_CPU_WINDOW_MS, CONFIG_HD_RK3506_SELFTEST_NET_HOST,
             CONFIG_HD_RK3506_SELFTEST_NET_PORT);
      return 0;
    }

  printf("=== RK3506 board selftest ===\n\n");

  if (all || strcmp(sel, "mem") == 0)
    {
      rc = st_mem();
      st_result("mem", rc);
      rc == ST_PASS ? pass++ : rc == ST_SKIP ? skip++ : fail++;
    }

  if (all || strcmp(sel, "cpu") == 0)
    {
      rc = st_cpu();
      st_result("cpu", rc);
      rc == ST_PASS ? pass++ : rc == ST_SKIP ? skip++ : fail++;
    }

  if (all || strcmp(sel, "fs") == 0)
    {
      rc = st_fs();
      st_result("fs", rc);
      rc == ST_PASS ? pass++ : rc == ST_SKIP ? skip++ : fail++;
    }

  if (all || strcmp(sel, "net") == 0)
    {
      rc = st_net();
      st_result("net", rc);
      rc == ST_PASS ? pass++ : rc == ST_SKIP ? skip++ : fail++;
    }

  if (all || strcmp(sel, "rpmsg") == 0)
    {
      rc = st_rpmsg();
      st_result("rpmsg", rc);
      rc == ST_PASS ? pass++ : rc == ST_SKIP ? skip++ : fail++;
    }

  if (!all && strcmp(sel, "mem") != 0 && strcmp(sel, "cpu") != 0 &&
      strcmp(sel, "fs") != 0 && strcmp(sel, "net") != 0 &&
      strcmp(sel, "rpmsg") != 0)
    {
      printf("selftest: unknown section '%s', try 'selftest help'\n", sel);
      return 1;
    }

  printf("=== %d passed, %d failed, %d skipped ===\n",
         pass, fail, skip);

  return fail == 0 ? 0 : 1;
}

#endif /* CONFIG_HD_RK3506_SELFTEST */
