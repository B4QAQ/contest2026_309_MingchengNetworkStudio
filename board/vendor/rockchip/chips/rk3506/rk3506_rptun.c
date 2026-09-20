/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_rptun.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 A7-side rptun (remote processor tunnel) lower half for the bus
 * Cortex-M0 ("mcu"), used by the NuttX rpmsg stack (master role).
 *
 * M0 lifecycle (SDK flow, v8m):
 *   The M0 firmware is NOT embedded in or started by NuttX.  It is packed
 *   as a standalone FIT (amp.img, load = 0xfff84000) into the "amp" GPT
 *   partition.  U-Boot (CONFIG_AMP / CONFIG_ROCKCHIP_AMP) loads and
 *   releases it before booting NuttX:
 *     amp_cpus_on() (u-boot drivers/cpu/rockchip_amp.c) reads the amp
 *     partition, boot_get_loadable() copies the standalone image into the
 *     M0 SRAM while the core is held reset, fit_standalone_release()
 *     (arch/arm/mach-rockchip/rk3506/rk3506.c) issues the SIP_MCU_CFG
 *     code-start SMC, opens CRU_GATE_CON5, writes GRF_SOC_CON36 and
 *     PMU_INT_MASK_CON.
 *   By the time NuttX runs, the M0 has already finished rpmsg-lite remote
 *   init and is waiting for link up.  NuttX's only responsibilities are
 *   the A-side clocks (same role as the Linux rockchip_amp driver's
 *   clk_prepare_enable of the six clocks) and the mailbox kick/rx path.
 *
 * Facts verified against the Linux SDK sources:
 *
 * Mailbox hardware (hal/lib/CMSIS/Device/RK3506/Include/rk3506.h,
 * hal/lib/hal/src/hal_mbox.c, kernel-6.1/drivers/mailbox/rockchip-mailbox.c):
 *   - mailbox0..3 at 0xff290000 + 0x1000*n.  v2 register layout (revision
 *     >= 0x200): A2B_INTEN 0x00, A2B_STATUS 0x04, A2B_CMD 0x08,
 *     A2B_DATA 0x0c, B2A_INTEN 0x10, B2A_STATUS 0x14, B2A_CMD 0x18,
 *     B2A_DATA 0x1c.  All INTEN/STATUS writes use HIWORD write-enable
 *     (bit(16+n) enables data bit n).
 *   - B2A_STATUS bit0: message pending from the B side until the A side
 *     reads CMD/DATA and w1c-clears bit0.
 *   - A2B_STATUS bit0: message pending from the A side until the B side
 *     reads CMD/DATA and w1c-clears bit0.
 *   - A2B_INTEN bit8: A->B trigger method (0 = write DATA triggers,
 *     1 = write CMD then DATA triggers).  The kernel sets trigger method
 *     1 (MAILBOX_V2_TRIGGER_SHIFT = 8, trigger_method = 1).
 *
 * Link wiring for master=A7 / remote=M0, link-id 0x03 =
 * RL_PLATFORM_SET_LINK_ID(0, 3) (hal test_demo.c: MASTER_ID=0,
 * REMOTE_ID=3; M_CPU_ID=0, R_CPU_ID=3):
 *   - A7 -> MCU kicks: mailbox3 A2B (MCU receives it via INTMUX line
 *     BB_3; kernel dts example rpmsg-rockchip link-id 0x03 uses
 *     mboxes <&mailbox 0 &mailbox 3> with tx = mailbox 3).
 *   - MCU q0 (tvq) kicks: mailbox0 B2A -> A7 GIC SPI 138.
 *   - MCU q1 (rvq) kicks: mailbox3 B2A -> A7 GIC SPI 141.
 *   - Kick message: { CMD = link_id & 0xff (0x03), DATA = RL_RPMSG_MAGIC
 *     (0x524d5347) } (hal rpmsg_platform.c platform_notify()).
 *
 * Shared memory (hal/project/rk3506-mcu/GCC/gcc_bus_m0.ld, the MCU's
 * LINUX_RPMSG region is da 0x03c00000 len 0x200000, identity to the A7
 * physical window; rk3506-amp.dtsi reserves the same 2 MiB):
 *   - vring0 (master rvq = remote tvq) da=pa=0x03c00000, vring1 (master
 *     svq = remote rvq) da=pa=0x03c08000, align 0x1000, 64 descriptors
 *     (hal rpmsg_platform.h VRING_ALIGN/VRING_SIZE, rpmsg_config.h
 *     RL_BUFFER_COUNT).
 *   - carveout 0x03c10000 len 0x1f0000: buffer pool (2 x 64 x 512 B rx
 *     seed + tx pool), allocated by the openamp master via the rptun
 *     carveout heap; the remote takes all its tx buffers from the
 *     master-seeded rvq.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <debug.h>
#include <nuttx/irq.h>
#include <nuttx/rptun/rptun.h>
#include <nuttx/spinlock.h>

#include <openamp/remoteproc.h>
#include <openamp/virtio.h>

#include "rk3506_rptun.h"

#ifdef CONFIG_RK3506_RPTUN

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif
#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Mailbox register block offsets (v2 layout) */

#define RK3506_MBOX_A2B_INTEN       0x00
#define RK3506_MBOX_A2B_STATUS      0x04
#define RK3506_MBOX_A2B_CMD         0x08
#define RK3506_MBOX_A2B_DATA        0x0c
#define RK3506_MBOX_B2A_INTEN       0x10
#define RK3506_MBOX_B2A_STATUS      0x14
#define RK3506_MBOX_B2A_CMD         0x18
#define RK3506_MBOX_B2A_DATA        0x1c

#define RK3506_MBOX_B2A_MSG_PEND    (1u << 0)   /* B2A_STATUS: B posted msg */
#define RK3506_MBOX_A2B_MSG_PEND    (1u << 0)   /* A2B_STATUS: A posted msg */
#define RK3506_MBOX_B2A_TX_DONE     (1u << 1)   /* B2A_STATUS: A consumed   */

/* HIWORD write-enable: bit(16 + n) enables writing data bit n */

#define RK3506_MBOX_WE(n)           (1u << ((n) + 16))

/* Mailbox blocks used by the rpmsg link */

#define RK3506_MBOX0_BASE           0xff290000u /* rx: MCU tvq kicks        */
#define RK3506_MBOX3_BASE           0xff293000u /* rx: MCU rvq kicks + tx   */

/* CRU clock gates.  Rockchip gate registers are HIWORD (write-enable in
 * bits 31:16) with set-to-disable semantics: writing bit=1 gates the
 * clock, writing 0 with the WE bit enables it.  CLKGATE_CON(n) =
 * CRU + 0x800 + n*4.  Gate bit numbers come from the kernel
 * drivers/clk/rockchip/clk-rk3506.c GATE() table.
 */

#define RK3506_CRU_BASE             0xff9a0000u
#define RK3506_CRU_GATE_CON(n)      (RK3506_CRU_BASE + 0x800 + (n) * 4)

/* PCLK_MAILBOX CON(6) bit13, PCLK_INTMUX CON(6) bit14: required for our
 * own mailbox access and for the M0's INTMUX (its mailbox IRQ router).
 * With these gates closed the mailbox register file is dead - all A7
 * mbox reads return 0 and every write is silently dropped (v8e board).
 */

#define RK3506_GATE_EN_MAILBOX_INTMUX \
                                    ((1u << (16 + 13)) | (1u << (16 + 14)))

/* The six clocks the A side must ensure are running for the remote core,
 * per rk3506-amp.dtsi "rockchip,amp" clocks =
 *   <&cru HCLK_M0>, <&cru STCLK_M0>, <&cru SCLK_UART4>,
 *   <&cru PCLK_UART4>, <&cru PCLK_TIMER>, <&cru CLK_TIMER0_CH5>;
 * the Linux rockchip-amp driver clk_prepare_enable()s all six.  U-Boot's
 * fit_standalone_release() opens HCLK_M0 itself, so by NuttX boot that one
 * is already on; enabling it again (data 0 + WE) is harmless.
 *
 * Gate bits (clk-rk3506.c):
 *   HCLK_M0 / SWCLKTCK_M0  CON(5) bits 10,11 (u-boot value 0x0c000000)
 *   PCLK_TIMER            CON(6) bit 2
 *   CLK_TIMER0_CH5        CON(6) bit 8   (TIMER5 = M0 SYS_TIMER)
 *   STCLK_M0              CON(8) bit 2   (M0 SysTick clock)
 *   PCLK_UART4            CON(11) bit 8  (M0 debug UART4)
 *   SCLK_UART4            CON(11) bit 13
 */

#define RK3506_GATE_CON5_M0         0x0c000000u
#define RK3506_GATE_EN_TIMER        ((1u << (16 + 2)) | (1u << (16 + 8)))
#define RK3506_GATE_EN_STCLK        (1u << (16 + 2))
#define RK3506_GATE_EN_UART4        ((1u << (16 + 8)) | (1u << (16 + 13)))

/* Kick message content (hal rpmsg_platform.c platform_notify) */

#define RK3506_RPMSG_LINK_ID        0x03u       /* RL_PLATFORM_SET_LINK_ID(0, 3) */
#define RK3506_RPMSG_MAGIC          0x524d5347u /* RL_RPMSG_MAGIC "RMSG" */

/* Notify busy-wait bound: the MCU clears A2B_STATUS bit0 as soon as its
 * INTMUX ISR runs; a few thousand register-poll iterations are plenty. */

#define RK3506_NOTIFY_SPIN_MAX      10000u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_rptun_dev_s
{
  struct rptun_dev_s    rptun;      /* Rptun device (MUST be first) */
  spinlock_t            lock;       /* Protects mailbox tx */
  rptun_callback_t      callback;   /* Upper half callback */
  FAR void              *arg;       /* Callback arg */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Resource table: one rpmsg virtio device + one buffer-pool carveout.
 * The trace slot of struct rptun_rsc_s is intentionally unused (num = 2).
 *
 * dfeatures: NS (endpoint announce; the MCU demo calls rpmsg_ns_announce)
 * and CPUNAME (mandatory: rpmsg_virtio_probe DEBUGASSERTs it and the
 * /dev/rpmsg/<name> node comes from remote_cpuname).  BUFSZ/BUFADDR/ACK/
 * PRIORITY are left off: buffers come from the carveout heap + the rvq
 * seeds, and the MCU demo has no NS-ACK handler.
 *
 * The MCU's tvq is vring0 = master rvq: seeded with 64 x 512 B buffers
 * by the master; the MCU borrows those to send.  vring1 = master svq.
 */

static const struct rptun_rsc_s g_rk3506_rptun_rsc =
{
  .rsc_tbl_hdr =
    {
      .ver      = 1,
      .num      = 2,
      .reserved = { 0, 0 },
    },

  .offset =
    {
      offsetof(struct rptun_rsc_s, rpmsg_vdev),
      offsetof(struct rptun_rsc_s, carveout),
    },

  .log_trace =
    {
      .type = RSC_TRACE,
      .da   = FW_RSC_U32_ADDR_ANY,
    },

  .rpmsg_vdev =
    {
      .type          = RSC_VDEV,
      .id            = VIRTIO_ID_RPMSG,
      .notifyid      = 0,
      .dfeatures     = (1UL << VIRTIO_RPMSG_F_NS) |
                       (1UL << VIRTIO_RPMSG_F_CPUNAME),
      .gfeatures     = 0,
      .config_len    = sizeof(struct fw_rsc_config),
      .status        = 0,
      .num_of_vrings = 2,
      .reserved      = { 0, 0 },
    },

  .rpmsg_vring0 =
    {
      .da       = 0x03c00000,          /* master rvq = remote tvq */
      .align    = 0x1000,
      .num      = 64,
      .notifyid = 0,
    },

  .rpmsg_vring1 =
    {
      .da       = 0x03c08000,          /* master svq = remote rvq */
      .align    = 0x1000,
      .num      = 64,
      .notifyid = 1,
    },

  .config =
    {
      .h2r_buf_size   = 512,
      .r2h_buf_size   = 512,
      .h2r_buf_addr   = 0,
      .r2h_buf_addr   = 0,
      .host_cpuname   = "ap",
      .remote_cpuname = "mcu",
    },

  .carveout =
    {
      .type     = RSC_CARVEOUT,
      .da       = 0x03c10000,          /* buffer pool after the 2 vrings */
      .pa       = 0x03c10000,
      .len      = 0x1f0000,            /* up to 0x03e00000 */
      .flags    = 0,
      .reserved = 0,
      .name     = "rsc-shm",
    },
};

static struct rk3506_rptun_dev_s g_rk3506_rptun =
{
  .rptun =
    {
      .ops = NULL,
    },
  .lock = SP_UNLOCKED,
};

/****************************************************************************
 * Private Function Prototypes
 *
 * (The ops table below references the implementations further down; make
 * the static functions visible to it instead of relying on implicit
 * declarations, which would shadow them with implicit externs.)
 ****************************************************************************/

static FAR const char *rk3506_rptun_get_cpuname(FAR struct rptun_dev_s *dev);
static FAR struct resource_table *
rk3506_rptun_get_resource(FAR struct rptun_dev_s *dev);
static bool rk3506_rptun_is_autostart(FAR struct rptun_dev_s *dev);
static bool rk3506_rptun_is_master(FAR struct rptun_dev_s *dev);
static int rk3506_rptun_config(FAR struct rptun_dev_s *dev, FAR void *data);
static int rk3506_rptun_start(FAR struct rptun_dev_s *dev);
static int rk3506_rptun_stop(FAR struct rptun_dev_s *dev);
static int rk3506_rptun_notify(FAR struct rptun_dev_s *dev, uint32_t vqid);
static int rk3506_rptun_register_callback(FAR struct rptun_dev_s *dev,
                                          rptun_callback_t callback,
                                          FAR void *arg);

static const struct rptun_ops_s g_rk3506_rptun_ops =
{
  .get_cpuname       = rk3506_rptun_get_cpuname,
  .get_firmware      = NULL,
  .get_addrenv       = NULL,
  .get_resource      = rk3506_rptun_get_resource,
  .is_autostart      = rk3506_rptun_is_autostart,
  .is_master         = rk3506_rptun_is_master,
  .config            = rk3506_rptun_config,
  .start             = rk3506_rptun_start,
  .stop              = rk3506_rptun_stop,
  .notify            = rk3506_rptun_notify,
  .register_callback = rk3506_rptun_register_callback,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_mbox_getreg / putreg helpers
 ****************************************************************************/

static inline uint32_t rk3506_mbox_getreg(uintptr_t base, unsigned int off)
{
  return getreg32(base + off);
}

static inline void rk3506_mbox_putreg(uintptr_t base, unsigned int off,
                                      uint32_t val)
{
  putreg32(val, base + off);
}

/****************************************************************************
 * Name: rk3506_mbox_init
 *
 * Description:
 *   A-side clocks + mailbox setup for the rpmsg link:
 *     - enable PCLK_MAILBOX + PCLK_INTMUX (CON(6) bits 13,14)
 *     - ensure the six AMP-core clocks are on (Linux rockchip_amp role)
 *     - mailbox3: clear stale status on both directions, set the A2B
 *       trigger method to 1 (write CMD then DATA) without disturbing the
 *       M0's own A2B_INTEN bit0
 *     - mailbox0: clear stale status; the rx interrupt enable happens in
 *       register_callback()
 *
 ****************************************************************************/

static void rk3506_mbox_init(void)
{
  /* A7's own access clocks. */

  putreg32(RK3506_GATE_EN_MAILBOX_INTMUX, RK3506_CRU_GATE_CON(6));

  /* AMP-core clocks (u-boot already did CON5; the rest are the Linux
   * rockchip-amp clock set). */

  putreg32(RK3506_GATE_CON5_M0, RK3506_CRU_GATE_CON(5));
  putreg32(RK3506_GATE_EN_TIMER, RK3506_CRU_GATE_CON(6));
  putreg32(RK3506_GATE_EN_STCLK, RK3506_CRU_GATE_CON(8));
  putreg32(RK3506_GATE_EN_UART4, RK3506_CRU_GATE_CON(11));

  /* mailbox3: clear stale status on both directions; set the A2B trigger
   * method to 1 without disturbing the M0's A2B_INTEN bit0. */

  rk3506_mbox_putreg(RK3506_MBOX3_BASE, RK3506_MBOX_B2A_STATUS,
                     RK3506_MBOX_B2A_MSG_PEND | RK3506_MBOX_B2A_TX_DONE);
  rk3506_mbox_putreg(RK3506_MBOX3_BASE, RK3506_MBOX_A2B_STATUS,
                     RK3506_MBOX_A2B_MSG_PEND);
  rk3506_mbox_putreg(RK3506_MBOX3_BASE, RK3506_MBOX_A2B_INTEN,
                     RK3506_MBOX_WE(8) | (1u << 8));

  /* mailbox0: clear stale status; the rx interrupt enable happens in
   * register_callback(). */

  rk3506_mbox_putreg(RK3506_MBOX0_BASE, RK3506_MBOX_B2A_STATUS,
                     RK3506_MBOX_B2A_MSG_PEND | RK3506_MBOX_B2A_TX_DONE);
}

/****************************************************************************
 * Name: rk3506_mbox_send
 *
 * Description:
 *   Send one kick message to the MCU over mailbox3 A2B.  The previous
 *   message must have been consumed by the MCU (A2B_STATUS bit0 cleared
 *   from its side) before a new one is accepted; bounded-wait for that
 *   under the spin lock so concurrent notify() calls cannot interleave.
 *
 ****************************************************************************/

static int rk3506_mbox_send(uint32_t cmd, uint32_t data)
{
  irqstate_t flags;
  uint32_t retry;
  int ret = -ETIMEDOUT;

  flags = spin_lock_irqsave(&g_rk3506_rptun.lock);

  for (retry = 0; retry < RK3506_NOTIFY_SPIN_MAX; retry++)
    {
      if ((rk3506_mbox_getreg(RK3506_MBOX3_BASE, RK3506_MBOX_A2B_STATUS) &
           RK3506_MBOX_A2B_MSG_PEND) == 0)
        {
          rk3506_mbox_putreg(RK3506_MBOX3_BASE, RK3506_MBOX_A2B_CMD, cmd);
          rk3506_mbox_putreg(RK3506_MBOX3_BASE, RK3506_MBOX_A2B_DATA, data);
          ret = OK;
          break;
        }
    }

  spin_unlock_irqrestore(&g_rk3506_rptun.lock, flags);

  if (ret < 0)
    {
      _err("ERROR: mailbox3 A2B busy, kick dropped (cmd=0x%02lx)\n",
           (unsigned long)cmd);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3506_mbox_isr
 *
 * Description:
 *   Common ISR for mailbox0 (MCU tvq kicks) and mailbox3 (MCU rvq kicks).
 *   Reads and w1c-clears the B2A message status, then dispatches
 *   RPTUN_NOTIFY_ALL to the upper half (rproc_virtio_notified() walks all
 *   vrings and only fires the ones with pending used entries).
 *
 ****************************************************************************/

static int rk3506_mbox_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3506_rptun_dev_s *priv = &g_rk3506_rptun;
  uintptr_t base = (uintptr_t)arg;
  uint32_t status;
  uint32_t cmd;
  uint32_t data;

  status = rk3506_mbox_getreg(base, RK3506_MBOX_B2A_STATUS);

  if ((status & RK3506_MBOX_B2A_MSG_PEND) != 0)
    {
      cmd  = rk3506_mbox_getreg(base, RK3506_MBOX_B2A_CMD);
      data = rk3506_mbox_getreg(base, RK3506_MBOX_B2A_DATA);

      /* W1C the message-pending bit: this is also the "consumed" signal
       * for the MCU side. */

      rk3506_mbox_putreg(base, RK3506_MBOX_B2A_STATUS,
                         RK3506_MBOX_B2A_MSG_PEND);

      UNUSED(cmd);
      UNUSED(data);

      if (priv->callback != NULL)
        {
          priv->callback(priv->arg, RPTUN_NOTIFY_ALL);
        }
    }

  if ((status & RK3506_MBOX_B2A_TX_DONE) != 0)
    {
      /* The MCU acknowledged the previous message; nothing to do since
       * the notify path busy-waits on A2B_STATUS instead. */

      rk3506_mbox_putreg(base, RK3506_MBOX_B2A_STATUS,
                         RK3506_MBOX_B2A_TX_DONE);
    }

  return OK;
}

/****************************************************************************
 * Name: rptun op implementations
 ****************************************************************************/

static FAR const char *rk3506_rptun_get_cpuname(FAR struct rptun_dev_s *dev)
{
  return "mcu";
}

static FAR struct resource_table *
rk3506_rptun_get_resource(FAR struct rptun_dev_s *dev)
{
  return (FAR struct resource_table *)&g_rk3506_rptun_rsc;
}

static bool rk3506_rptun_is_autostart(FAR struct rptun_dev_s *dev)
{
  return true;
}

static bool rk3506_rptun_is_master(FAR struct rptun_dev_s *dev)
{
  return true;
}

static int rk3506_rptun_config(FAR struct rptun_dev_s *dev, FAR void *data)
{
  rk3506_mbox_init();
  return OK;
}

static int rk3506_rptun_start(FAR struct rptun_dev_s *dev)
{
  /* The M0 was loaded and released by U-Boot (amp partition); nothing to
   * start here. */

  return OK;
}

static int rk3506_rptun_stop(FAR struct rptun_dev_s *dev)
{
  /* The M0 keeps running; it is out of NuttX's reset domain. */

  return OK;
}

static int rk3506_rptun_notify(FAR struct rptun_dev_s *dev, uint32_t vqid)
{
  /* All A7 -> MCU kicks go over mailbox3 A2B with the link id / magic
   * pair, regardless of the vring id; the MCU's rpmsg-lite remote
   * callback treats the first notification as the link-up handshake and
   * the later ones as rvq events. */

  return rk3506_mbox_send(RK3506_RPMSG_LINK_ID, RK3506_RPMSG_MAGIC);
}

static int rk3506_rptun_register_callback(FAR struct rptun_dev_s *dev,
                                          rptun_callback_t callback,
                                          FAR void *arg)
{
  FAR struct rk3506_rptun_dev_s *priv = &g_rk3506_rptun;
  int ret;

  priv->callback = callback;
  priv->arg      = arg;

  /* Enable the rx interrupts now that the upper half callback is in
   * place: mailbox0 SPI138 (MCU tvq kicks) and mailbox3 SPI141 (MCU rvq
   * kicks).  B2A_INTEN bit0 = AP inbound message interrupt (hal_mbox.c
   * MBOX_ChanEnable writes WE|1). */

  ret = irq_attach(RK3506_IRQ_MAILBOX0, rk3506_mbox_isr,
                   (FAR void *)RK3506_MBOX0_BASE);
  if (ret < 0)
    {
      _err("ERROR: irq_attach mailbox0 failed: %d\n", ret);
      return ret;
    }

  ret = irq_attach(RK3506_IRQ_MAILBOX3, rk3506_mbox_isr,
                   (FAR void *)RK3506_MBOX3_BASE);
  if (ret < 0)
    {
      _err("ERROR: irq_attach mailbox3 failed: %d\n", ret);
      return ret;
    }

  rk3506_mbox_putreg(RK3506_MBOX0_BASE, RK3506_MBOX_B2A_INTEN,
                     RK3506_MBOX_WE(0) | RK3506_MBOX_B2A_MSG_PEND);
  rk3506_mbox_putreg(RK3506_MBOX3_BASE, RK3506_MBOX_B2A_INTEN,
                     RK3506_MBOX_WE(0) | RK3506_MBOX_B2A_MSG_PEND);

  up_enable_irq(RK3506_IRQ_MAILBOX0);
  up_enable_irq(RK3506_IRQ_MAILBOX3);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_rptun_initialize
 *
 * Description:
 *   Register the RK3506 bus-M0 rptun device.  Called from board bringup;
 *   with is_autostart = true the rptun core immediately spawns its start
 *   thread and brings up the rpmsg virtio device (/dev/rpmsg/mcu once the
 *   virtio rpmsg driver probes it).  The M0 itself is already running,
 *   loaded from the amp partition by U-Boot.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3506_rptun_initialize(void)
{
  int ret;

  g_rk3506_rptun.rptun.ops = &g_rk3506_rptun_ops;

  ret = rptun_initialize(&g_rk3506_rptun.rptun);
  if (ret < 0)
    {
      _err("ERROR: rptun_initialize failed: %d\n", ret);
      return ret;
    }

  return ret;
}

#endif /* CONFIG_RK3506_RPTUN */
