/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_netcfg.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * netcfg - network configuration daemon for the HD-RK3506-EVM.
 *
 * Reads /etc/net.conf at startup and configures eth0 accordingly:
 *
 *   mode=dhcp          - obtain IP/netmask/router/DNS via DHCP
 *   mode=static        - apply ip/netmask/gateway/dns from the file
 *   auto_dhcp=1        - after the initial configuration, register
 *                        SIOCMIINOTIFY and re-run DHCP every time the
 *                        GMAC0 link transitions to up (cable (re)plug,
 *                        switch STP forward-delay expiry, ...).
 *
 * The link notification uses the GMAC0 driver's SIOCMIINOTIFY ioctl
 * (CONFIG_NETDEV_PHY_IOCTL): the driver's link poll fires a signal on
 * every link CHANGE, so netcfg wakes immediately, checks whether the
 * link is up, and (re)runs the DHCP client.  This replaces the
 * NuttX netinit monitor, whose ifup/ifdown + MDIO ioctls interacted
 * badly with the driver's sleeping ifup path (M3 regression).
 *
 * The config-file keywords are parsed with strtok; unknown lines are
 * ignored, so the file doubles as its own documentation.
 *
 * Included files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <netutils/netlib.h>
#include <nuttx/net/mii.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NETCFG_CONF_PATH   "/etc/net.conf"
#define NETCFG_IFNAME      "eth0"
#define NETCFG_LINE_MAX    128

/* Signal used for the GMAC0 link-change notification.  SIGUSR1 is not
 * used by any resident component of this image (NTP daemon uses
 * CONFIG_NETUTILS_NTPCLIENT_SIGWAKEUP=18).
 */

#define NETCFG_LINK_SIGNO  SIGUSR1

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct netcfg_conf_s
{
  bool dhcp;                 /* true: mode=dhcp */
  bool auto_dhcp;            /* true: re-DHCP on link up */
  struct in_addr ip;
  struct in_addr netmask;
  struct in_addr gateway;
  struct in_addr dns;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: netcfg_parse_conf
 *
 * Description:
 *   Parse /etc/net.conf.  Returns OK even when the file is missing
 *   (defaults: dhcp + auto_dhcp=1) so the daemon behaves sensibly on a
 *   stock image.
 *
 ****************************************************************************/

static int netcfg_parse_conf(FAR struct netcfg_conf_s *conf)
{
  FAR FILE *fp;
  char line[NETCFG_LINE_MAX];

  memset(conf, 0, sizeof(*conf));
  conf->dhcp      = true;   /* defaults */
  conf->auto_dhcp = true;

  fp = fopen(NETCFG_CONF_PATH, "r");
  if (fp == NULL)
    {
      printf("netcfg: no %s, defaults: mode=dhcp auto_dhcp=1\n",
             NETCFG_CONF_PATH);
      return OK;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      char *key;
      char *val;

      /* Strip comment and newline */

      key = strchr(line, '#');
      if (key != NULL)
        {
          *key = '\0';
        }

      key = strtok(line, " \t\r\n");
      if (key == NULL)
        {
          continue;
        }

      val = strtok(NULL, " \t\r\n");

      if (strcasecmp(key, "mode") == 0 && val != NULL)
        {
          conf->dhcp = (strcasecmp(val, "dhcp") == 0);
        }
      else if (strcasecmp(key, "auto_dhcp") == 0 && val != NULL)
        {
          conf->auto_dhcp = (strcmp(val, "1") == 0 ||
                             strcasecmp(val, "true") == 0 ||
                             strcasecmp(val, "yes") == 0);
        }
      else if (val != NULL)
        {
          if (strcasecmp(key, "ip") == 0)
            {
              inet_pton(AF_INET, val, &conf->ip);
            }
          else if (strcasecmp(key, "netmask") == 0)
            {
              inet_pton(AF_INET, val, &conf->netmask);
            }
          else if (strcasecmp(key, "gateway") == 0)
            {
              inet_pton(AF_INET, val, &conf->gateway);
            }
          else if (strcasecmp(key, "dns") == 0)
            {
              inet_pton(AF_INET, val, &conf->dns);
            }
        }
    }

  fclose(fp);
  return OK;
}

/****************************************************************************
 * Name: netcfg_apply_static
 *
 * Description:
 *   Apply static ip/netmask/gateway/dns from the parsed config.
 *
 ****************************************************************************/

static int netcfg_apply_static(FAR const struct netcfg_conf_s *conf)
{
  int ret;

  ret = netlib_set_ipv4addr(NETCFG_IFNAME, &conf->ip);
  if (ret < 0)
    {
      printf("netcfg: set_ipv4addr failed: %d\n", errno);
      return ret;
    }

  if (conf->netmask.s_addr != 0)
    {
      netlib_set_ipv4netmask(NETCFG_IFNAME, &conf->netmask);
    }

  if (conf->gateway.s_addr != 0)
    {
      netlib_set_dripv4addr(NETCFG_IFNAME, &conf->gateway);
    }

  if (conf->dns.s_addr != 0)
    {
      netlib_set_ipv4dnsaddr(&conf->dns);
    }

  printf("netcfg: static %s applied\n", NETCFG_IFNAME);
  return OK;
}

/****************************************************************************
 * Name: netcfg_run_dhcp
 *
 * Description:
 *   Run one DHCP negotiation (netlib_obtain_ipv4addr also updates the
 *   DNS server from the DHCP reply when present).
 *
 ****************************************************************************/

static int netcfg_run_dhcp(void)
{
  int ret;

  ret = netlib_obtain_ipv4addr(NETCFG_IFNAME);
  if (ret < 0)
    {
      printf("netcfg: dhcp on %s failed: %d\n", NETCFG_IFNAME, errno);
    }
  else
    {
      printf("netcfg: dhcp on %s ok\n", NETCFG_IFNAME);
    }

  return ret;
}

/****************************************************************************
 * Name: netcfg_is_link_up
 *
 * Description:
 *   Query the GMAC0 PHY link status via the driver's SIOCGMIIPHY /
 *   SIOCGMIIREG ioctls (CONFIG_NETDEV_PHY_IOCTL) and return the
 *   MII_MSR link bit.  Falls back to IFF_RUNNING when the PHY ioctls
 *   are unavailable.
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_PHY_IOCTL
static bool netcfg_is_link_up(void)
{
  struct ifreq ifr;
  struct mii_ioctl_data_s *mii;
  uint8_t flags;
  int sockfd;
  int ret;

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      return false;
    }

  memset(&ifr, 0, sizeof(ifr));
  strlcpy(ifr.ifr_name, NETCFG_IFNAME, IFNAMSIZ);

  ret = ioctl(sockfd, SIOCGMIIPHY, (unsigned long)&ifr);
  if (ret >= 0)
    {
      mii = (FAR struct mii_ioctl_data_s *)((uintptr_t)&ifr.ifr_ifru);

      mii->reg_num = MII_MSR;
      ret = ioctl(sockfd, SIOCGMIIREG, (unsigned long)&ifr);
      if (ret >= 0)
        {
          close(sockfd);
          return (mii->val_out & MII_MSR_LINKSTATUS) != 0;
        }
    }

  /* PHY ioctls unavailable: fall back to the running flag */

  close(sockfd);
  if (netlib_getifstatus(NETCFG_IFNAME, &flags) == 0)
    {
      return (flags & IFF_RUNNING) != 0;
    }

  return false;
}
#else
static bool netcfg_is_link_up(void)
{
  uint8_t flags = 0;

  if (netlib_getifstatus(NETCFG_IFNAME, &flags) == 0)
    {
      return (flags & IFF_RUNNING) != 0;
    }

  return false;
}
#endif

/****************************************************************************
 * Name: netcfg_link_notify_register
 *
 * Description:
 *   Register with the GMAC0 driver for link-change signals
 *   (SIOCMIINOTIFY).  The driver fires NETCFG_LINK_SIGNO at this task
 *   on every link transition.
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_PHY_IOCTL
static int netcfg_link_notify_register(void)
{
  struct ifreq ifr;
  struct mii_ioctl_notify_s *notify;
  struct sigaction sa;
  sigset_t mask;
  int sockfd;
  int ret;

  /* Block NETCFG_LINK_SIGNO in the main thread BEFORE registering:
   * sigwait() consumes it synchronously, so no async handler runs
   * and no DHCP code ever executes in signal context.
   */

  sigemptyset(&mask);
  sigaddset(&mask, NETCFG_LINK_SIGNO);
  sigprocmask(SIG_BLOCK, &mask, NULL);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigaction(NETCFG_LINK_SIGNO, &sa, NULL);

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      printf("netcfg: notify socket failed: %d\n", errno);
      return -errno;
    }

  memset(&ifr, 0, sizeof(ifr));
  strlcpy(ifr.ifr_name, NETCFG_IFNAME, IFNAMSIZ);

  notify = (FAR struct mii_ioctl_notify_s *)((uintptr_t)&ifr.ifr_ifru);
  memset(notify, 0, sizeof(*notify));
  notify->pid                  = 0;   /* 0 = this task */
  notify->event.sigev_notify   = SIGEV_SIGNAL;
  notify->event.sigev_signo    = NETCFG_LINK_SIGNO;
  notify->event.sigev_value.sival_ptr = NULL;

  ret = ioctl(sockfd, SIOCMIINOTIFY, (unsigned long)&ifr);
  close(sockfd);

  if (ret < 0)
    {
      printf("netcfg: SIOCMIINOTIFY failed: %d\n", errno);
      return -errno;
    }

  printf("netcfg: link notification registered (signo %d)\n",
         NETCFG_LINK_SIGNO);
  return OK;
}
#endif

/****************************************************************************
 * Name: netcfg_wait_link_change
 *
 * Description:
 *   Block until the next link-change signal.  Returns true when the
 *   link is up afterwards (checked directly from the PHY so a burst of
 *   up/down signals collapses into one decision).
 *
 ****************************************************************************/

static bool netcfg_wait_link_change(void)
{
  sigset_t mask;
  int sig;

  sigemptyset(&mask);
  sigaddset(&mask, NETCFG_LINK_SIGNO);
  sigwait(&mask, &sig);

  return netcfg_is_link_up();
}

/****************************************************************************
 * Name: main
 *
 * Description:
 *   netcfg entry point.
 *
 *   netcfg        - parse config, configure, then (optionally) loop on
 *                   link-change signals re-running DHCP when auto_dhcp
 *                   is enabled.
 *   netcfg once   - configure once and exit (rcS/boot scripts).
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct netcfg_conf_s conf;
  bool oneshot = (argc > 1 && strcmp(argv[1], "once") == 0);
  bool was_up;
  int ret;

  ret = netcfg_parse_conf(&conf);
  if (ret != OK)
    {
      return EXIT_FAILURE;
    }

  /* Initial configuration: only bring up the interface if it is not
   * already up (the netinit thread may have done it already).
   */

  was_up = netcfg_is_link_up();

  if (conf.dhcp)
    {
      if (!was_up)
        {
          netlib_ifup(NETCFG_IFNAME);
        }

      netcfg_run_dhcp();
    }
  else
    {
      if (!was_up)
        {
          netlib_ifup(NETCFG_IFNAME);
        }

      netcfg_apply_static(&conf);
    }

  if (oneshot || !conf.auto_dhcp)
    {
      return EXIT_SUCCESS;
    }

#ifdef CONFIG_NETDEV_PHY_IOCTL
  ret = netcfg_link_notify_register();
  if (ret != OK)
    {
      return EXIT_FAILURE;
    }

  /* Link-change service loop: each signal means the link CHANGED.
   * Re-run DHCP only when it is now up (fresh plug, STP expiry, ...).
   */

  for (; ; )
    {
      if (netcfg_wait_link_change())
        {
          printf("netcfg: link up -> dhcp\n");
          netcfg_run_dhcp();
        }
      else
        {
          printf("netcfg: link down\n");
        }
    }
#else
  printf("netcfg: NETDEV_PHY_IOCTL disabled, no link watching\n");
#endif

  return EXIT_SUCCESS;
}
