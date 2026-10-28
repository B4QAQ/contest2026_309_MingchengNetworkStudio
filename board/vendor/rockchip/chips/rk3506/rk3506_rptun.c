/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_rptun.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506G2 A7 rptun lower half for the bus Cortex-M0.
 *
 * The A7 is the rpmsg master (virtio driver).  The M0 runs the unmodified
 * SDK rpmsg-lite test firmware (HAL project rk3506-mcu, master id 0,
 * remote id 3).  Under the SDK AMP flow the M0 firmware is packaged into
 * the standalone FIT image amp/amp.img (load=0x03a00000, DRAM) burned
 * into the "amp" GPT partition, and U-Boot loads + releases the core in
 * board_late_init() -> amp_cpus_on() -> fit_standalone_release()
 * (u-boot arch/arm/mach-rockchip/rk3506/rk3506.c) before the kernel runs.
 * The 32 KiB M0 SRAM window is too small for a HAL firmware with a
 * console + rpmsg, so the image runs from the 1 MiB DRAM window
 * 0x03a00000-0x03b00000 - which is why the NuttX heap must stop below
 * that address (see the board ld.script).
 *
 * M0 LIFE CYCLE BELONGS TO U-BOOT, NOT TO THIS FILE.  This lower half
 * therefore NEVER resets the M0, NEVER copies firmware into its load
 * window and NEVER re-issues the SIP_MCU_CFG release SMC: doing any of
 * those after
 * U-Boot has already released a running M0 either destroys the peer state
 * or drives the TEE through an unendorsed second MCU_CODE_START from a
 * non-POR reset topology.  This file only:
 *
 *   - ungates the M0 time-base + mailbox/intmux clocks that U-Boot does
 *     NOT open (fit_standalone_release opens only HCLK_M0/SWCLKTCK_M0);
 *   - runs the mailbox0/mailbox3 kick/interrupt protocol (the rpmsg
 *     master half);
 *   - performs a READ-ONLY readiness probe of the peer (INTMUX group3
 *     bit21) so the first kick is not sent before the M0 armed its
 *     receive interrupt.
 *
 * Every register value below is taken from the Linux SDK sources:
 *
 *   - Clock gates: u-boot fit_standalone_release() opens HCLK_M0/
 *     SWCLKTCK_M0 (value 0x0c000000); the remaining clocks are the six
 *     the Linux rockchip,amp core enables (arch/arm/boot/dts/
 *     rk3506-amp.dtsi) + the mailbox/intmux pclocks needed on the A7
 *     side.  Gate polarity is CLK_GATE_HIWORD_MASK|CLK_GATE_SET_TO_DISABLE
 *     (drivers/clk/rockchip/clk-rk3506.c GFLAGS), so an enable writes the
 *     bit position in the high half-word and 0 in the low half-word.
 *   - Kick protocol: drivers/mailbox/rockchip-mailbox.c +
 *     hal/lib/hal/src/hal_mbox.c (mailbox v2 registers, level interrupts,
 *     w1c status, busy-drop, CMD-before-DATA order).
 *   - Peer side: hal/middleware/rpmsg-lite/.../platform/RK3506/
 *     rpmsg_platform.c (first-kick handshake, mailbox3 wiring, INTMUX
 *     group3 bit21 as the last arming step).
 *
 * Shared memory layout (matches the M0 linker script LINUX_RPMSG region
 * and the Linux AMP reserved-memory nodes):
 *
 *   0x03c00000  vring0 (master rvq, 64 desc, align 0x1000)
 *   0x03c08000  vring1 (master svq)
 *   0x03c10000  rpmsg-lite peer buffer pool (M0 bump allocator)
 *   0x03d00000  master buffer pool / carveout heap, 0x100000
 *
 * Mailbox wiring (see M0 rpmsg_platform.c platform_notify / clients):
 *
 *   A7 -> M0 : mailbox3 A2B (CMD 0x03, DATA "RMSG"); the peer's first
 *              received kick is its link-up event, every later kick
 *              addresses its tx vq (first_notify filter).
 *   M0 -> A7 : mailbox0 B2A -> GIC SPI138 -> vring0 (NS + data)
 *              mailbox3 B2A -> GIC SPI141 -> vring1 (consumed buffers)
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/spinlock.h>
#include <nuttx/rptun/rptun.h>

#include <arch/irq.h>

#include "rk3506_rptun.h"
#include "rk3506_cru.h"
#include "rk3506_register.h"
#include "hardware/rk3506_memorymap.h"

#ifdef CONFIG_RK3506_RPTUN

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* M0-cluster clock gate bits (main CRU CLKGATE_CON, clk-rk3506.c).
 * These are plain bit masks; the HIWORD_UPDATE store is done by the
 * shared rk3506_cru_gate() primitive (data bit 0 = enable), so this
 * driver no longer carries a private copy of the CRU register layout.
 * All values were verified against clk-rk3506.c.
 */

/* CON5:  HCLK_M0 bit10, SWCLKTCK_M0 bit11 (exact u-boot value 0x0c00;
 * u-boot already opens these, re-opening is idempotent). */

#define RK3506_GATE_CON5_M0         ((1u << 10) | (1u << 11))

/* CON6:  PCLK_TIMER b2, CLK_TIMER0_CH5 b8, PCLK_MAILBOX b13,
 *        PCLK_INTMUX b14.
 * The time-base clocks (PCLK_TIMER/CLK_TIMER0_CH5) keep the M0's
 * HAL_DelayMs() alive; PCLK_MAILBOX/INTMUX are needed on the A7 side.
 */

#define RK3506_GATE_CON6_M0         ((1u << 2) | (1u << 8) | \
                                     (1u << 13) | (1u << 14))

/* CON8:  STCLK_M0 bit2 (M0 SysTick source). */

#define RK3506_GATE_CON8_STCLK_M0   (1u << 2)

/* CON11: the M0 debug UART clock (UART1).  PCLK_UART1 is bit5 and
 * SCLK_UART1 is bit10 of CLKGATE_CON11 (kernel-6.1 clk-rk3506.c: GATE
 * "pclk_uart1" @CON11/5, COMPOSITE "sclk_uart1" @CON11/10).  The M0
 * firmware prints on UART1 with the pads remapped by RM_IO to
 * GPIO0_C0 (RX) / GPIO0_C1 (TX), matching the validated Linux AMP flow.
 */

#define RK3506_GATE_CON11_UART1     ((1u << 5) | (1u << 10))

/* Mailbox v2 registers (rockchip-mailbox.c reg_a2b table).  Controller
 * bases come from hardware/rk3506_memorymap.h (RK3506_MAILBOX0/3_ADDR).
 */

#define RK3506_MBOX0_BASE           RK3506_MAILBOX0_ADDR
#define RK3506_MBOX3_BASE           RK3506_MAILBOX3_ADDR

#define MBOX_A2B_INTEN              0x00u
#define MBOX_A2B_STATUS             0x04u
#define MBOX_A2B_CMD                0x08u
#define MBOX_A2B_DAT                0x0cu
#define MBOX_B2A_INTEN              0x10u
#define MBOX_B2A_STATUS             0x14u
#define MBOX_B2A_CMD                0x18u
#define MBOX_B2A_DAT                0x1cu

#define MBOX_CHAN0                  0x1u     /* bit0, w1c on STATUS     */
#define MBOX_INTEN_WE               0x10000u /* hiword write enable    */

/* INTMUX (rk3506.h struct INTMUX_REG): ENABLE group 3 at 0x0c, FLAG
 * group 3 at 0x8c.  MAILBOX_BB_3 IRQ 117 lands in group 3 bit 21 and is
 * fanned out to M0 NVIC IRQ31; the M0 rpmsg_platform.c
 * platform_interrupt_enable() sets it as the very last step of the peer's
 * rpmsg initialisation, and it is the ONLY writer of that bit (A7/U-Boot/
 * TEE never touch INTMUX).  Read-only readiness probe input.
 */

#define RK3506_INTMUX_BASE          RK3506_INTMUX_ADDR
#define RK3506_INTMUX_EN_GRP3       (RK3506_INTMUX_BASE + 0x0cu)
#define RK3506_INTMUX_FLAG_GRP3     (RK3506_INTMUX_BASE + 0x8cu)
#define RK3506_INTMUX_MBOX3_BIT     (1u << 21)

/* Split vring layout (num=64, standard vring with 0x1000 alignment):
 * desc[] occupies 64*16 = 0x400 bytes, avail[] at +0x400, used[] at
 * the next 0x1000 boundary (+0x1000).  The idx halfword sits at
 * offset +2 of each structure.  Used by the diagnostic dump only.
 */

#define RK3506_VR_AVAIL_IDX_OFF     (0x0400u + 2u)
#define RK3506_VR_USED_IDX_OFF      (0x1000u + 2u)
#define RK3506_VR_DESC_OFF          0x0000u

/* Kick protocol (rpmsg_config.h / rpmsg_platform.h). */

#define RK3506_LINK_ID              0x03u       /* SET_LINK_ID(0, 3)    */
#define RK3506_RPMSG_MAGIC          0x524d5347u /* "RMSG"              */

/* Shared window / vring parameters (M0 linker script +
 * include/linux/rpmsg/rockchip_rpmsg.h).
 */

#define RK3506_SHMEM_BASE           0x03c00000u
#define RK3506_VRING0_DA            (RK3506_SHMEM_BASE)
#define RK3506_VRING1_DA            (RK3506_SHMEM_BASE + 0x8000u)
#define RK3506_VRING_ALIGN          0x1000u
#define RK3506_VRING_NUM            64u
#define RK3506_CARVEOUT_DA          0x03d00000u
#define RK3506_CARVEOUT_LEN         0x100000u

/* Read-only readiness probe bounds.  Non-fatal: U-Boot releases the M0
 * well before the kernel runs, so the bit is normally already set; the
 * short poll only covers the worst-case scheduling gap.
 */

#define RK3506_READY_TIMEOUT_US     200000u   /* 200 ms */
#define RK3506_READY_STEP_US        1000u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_rptun_priv_s
{
  /* The rptun device must be the first member. */

  struct rptun_dev_s rptun;

  /* Upper-half callback registered after start(). */

  rptun_callback_t callback;
  FAR void        *cbarg;

  /* Serialises mailbox A2B writes.  The notify path also runs from IRQ
   * context (rpmsg rx processing kicks the reclaim vq), so an irqsave
   * spinlock is required.
   */

  spinlock_t       tx_lock;

  /* The first DRIVER_OK kick is the peer's link-up handshake; only one
   * is ever sent (rockchip_rpmsg_mbox.c rk_rpmsg_notify).
   */

  bool             handshake_sent;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static FAR const char *rp_get_cpuname(FAR struct rptun_dev_s *dev);
static FAR struct resource_table *
rp_get_resource(FAR struct rptun_dev_s *dev);
static bool rp_is_autostart(FAR struct rptun_dev_s *dev);
static bool rp_is_master(FAR struct rptun_dev_s *dev);
static int  rp_start(FAR struct rptun_dev_s *dev);
static int  rp_stop(FAR struct rptun_dev_s *dev);
static int  rp_notify(FAR struct rptun_dev_s *dev, uint32_t vqid);
static int  rp_register_callback(FAR struct rptun_dev_s *dev,
                                 rptun_callback_t callback, FAR void *arg);

static int rk3506_mbox_isr(int irq, FAR void *context, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Static resource table: one rpmsg vdev (NS + CPUNAME features) and one
 * carveout for the master-side buffer pool.  The vring device addresses
 * are fixed at the addresses the peer firmware was built for, so the
 * rptun core leaves them untouched and the carveout heap never overlaps
 * them or the peer's own pool at 0x03c10000.
 *
 * The table is mutable: the rptun/openamp core writes gfeatures and the
 * vdev status back into it.  The M0 rpmsg-lite firmware never parses the
 * resource table, so the cpuname config fields are A7-side bookkeeping
 * and are pre-filled here (config space is not shared with the peer).
 */

static struct rptun_rsc_s g_rk3506_rsc =
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

  .rpmsg_vdev =
    {
      .type          = RSC_VDEV,
      .id            = VIRTIO_ID_RPMSG,
      .notifyid      = RSC_NOTIFY_ID_ANY,
      .dfeatures     = (1u << VIRTIO_RPMSG_F_NS) |
                       (1u << VIRTIO_RPMSG_F_CPUNAME),
      .gfeatures     = 0,
      .config_len    = sizeof(struct fw_rsc_config),
      .status        = 0,
      .num_of_vrings = 2,
      .reserved      = { 0, 0 },
    },

  .config =
    {
      .host_cpuname   = "ap",
      .remote_cpuname = "mcu",
    },

  .rpmsg_vring0 =
    {
      .da       = RK3506_VRING0_DA,
      .align    = RK3506_VRING_ALIGN,
      .num      = RK3506_VRING_NUM,
      .notifyid = 0,
    },

  .rpmsg_vring1 =
    {
      .da       = RK3506_VRING1_DA,
      .align    = RK3506_VRING_ALIGN,
      .num      = RK3506_VRING_NUM,
      .notifyid = 1,
    },

  .carveout =
    {
      .type  = RSC_CARVEOUT,
      .da    = RK3506_CARVEOUT_DA,
      .pa    = RK3506_CARVEOUT_DA,
      .len   = RK3506_CARVEOUT_LEN,
      .flags = 0,
      .name  = "vdev0buffer",
    },
};

static const struct rptun_ops_s g_rk3506_rptun_ops =
{
  .get_cpuname       = rp_get_cpuname,
  .get_resource      = rp_get_resource,
  .is_autostart      = rp_is_autostart,
  .is_master         = rp_is_master,
  .start             = rp_start,
  .stop              = rp_stop,
  .notify            = rp_notify,
  .register_callback = rp_register_callback,
};

static struct rk3506_rptun_priv_s g_rk3506_rptun;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_mcu_clock_enable
 *
 * Description:
 *   Ungate the clocks the M0 + the A7-side kick path need.  U-Boot's
 *   fit_standalone_release() opens only HCLK_M0/SWCLKTCK_M0, so the M0
 *   time-base clocks (STCLK_M0, PCLK_TIMER, CLK_TIMER0_CH5) and the
 *   mailbox/intmux pclocks MUST be opened here or the peer dies in
 *   HAL_DelayMs() right after link-up and the mailbox block has no pclk.
 *   All writes are HIWORD data=0 (SET_TO_DISABLE polarity -> enable).
 *
 ****************************************************************************/

static void rk3506_mcu_clock_enable(void)
{
  rk3506_cru_gate(5,  RK3506_GATE_CON5_M0,        true);
  rk3506_cru_gate(6,  RK3506_GATE_CON6_M0,        true);
  rk3506_cru_gate(8,  RK3506_GATE_CON8_STCLK_M0,  true);
  rk3506_cru_gate(11, RK3506_GATE_CON11_UART1,    true);
}

/****************************************************************************
 * Name: rk3506_m0_wait_ready
 *
 * Description:
 *   READ-ONLY probe of the peer's rpmsg readiness.  The M0 sets INTMUX
 *   group3 bit21 as the last step of its rpmsg init; this function polls
 *   that bit for up to RK3506_READY_TIMEOUT_US so the link-up kick is not
 *   sent before the peer armed its receive interrupt.  It never writes
 *   INTMUX and never fails: the M0 is released by U-Boot, not by us.
 *
 ****************************************************************************/

static void rk3506_m0_wait_ready(void)
{
  unsigned int waited;

  for (waited = 0;
       (getreg32(RK3506_INTMUX_EN_GRP3) & RK3506_INTMUX_MBOX3_BIT) == 0;
       waited += RK3506_READY_STEP_US)
    {
      if (waited >= RK3506_READY_TIMEOUT_US)
        {
          return;
        }

      nxsig_usleep(RK3506_READY_STEP_US);
    }
}

/****************************************************************************
 * Name: rk3506_mbox_kick
 *
 * Description:
 *   Post one kick to the M0 through mailbox3 A2B (CMD/DAT order per
 *   HAL MBOX_ChanSendMsg).  A still-set A2B status means the peer has
 *   not consumed the previous kick: the message is dropped, exactly as
 *   rockchip_rpmsg_mbox.c does on EBUSY - the peer reprocesses the
 *   vring state when it handles the previous kick.
 *
 ****************************************************************************/

static void rk3506_mbox_kick(uint32_t reason)
{
  FAR struct rk3506_rptun_priv_s *priv = &g_rk3506_rptun;
  uint32_t status;
  irqstate_t flags = spin_lock_irqsave(&priv->tx_lock);

  status = getreg32(RK3506_MBOX3_BASE + MBOX_A2B_STATUS);
  if ((status & MBOX_CHAN0) == 0)
    {
      putreg32(RK3506_LINK_ID, RK3506_MBOX3_BASE + MBOX_A2B_CMD);
      putreg32(RK3506_RPMSG_MAGIC, RK3506_MBOX3_BASE + MBOX_A2B_DAT);
    }

  spin_unlock_irqrestore(&priv->tx_lock, flags);
}

/****************************************************************************
 * Name: rk3506_mbox_isr
 *
 * Description:
 *   B2A interrupt handler for mailbox0 (vring0) and mailbox3 (vring1).
 *   Status bits are write-1-to-clear and the IRQ is level high.
 *
 ****************************************************************************/

static int rk3506_mbox_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3506_rptun_priv_s *priv = &g_rk3506_rptun;
  uintptr_t base = (uintptr_t)arg;
  uint32_t vqid;

  (void)irq;
  (void)context;

  if ((getreg32(base + MBOX_B2A_STATUS) & MBOX_CHAN0) == 0)
    {
      return OK;
    }

  vqid = (base == RK3506_MBOX3_BASE) ? 1u : 0u;

  putreg32(MBOX_CHAN0, base + MBOX_B2A_STATUS);

  if (priv->callback != NULL)
    {
      priv->callback(priv->cbarg, vqid);
    }

  return OK;
}

/****************************************************************************
 * Rptun ops
 ****************************************************************************/

static FAR const char *rp_get_cpuname(FAR struct rptun_dev_s *dev)
{
  (void)dev;
  return "mcu";
}

static FAR struct resource_table *
rp_get_resource(FAR struct rptun_dev_s *dev)
{
  (void)dev;
  return (FAR struct resource_table *)&g_rk3506_rsc;
}

static bool rp_is_autostart(FAR struct rptun_dev_s *dev)
{
  (void)dev;
  return true;
}

static bool rp_is_master(FAR struct rptun_dev_s *dev)
{
  (void)dev;
  return true;
}

static int rp_start(FAR struct rptun_dev_s *dev)
{
  FAR struct rk3506_rptun_priv_s *priv =
    (FAR struct rk3506_rptun_priv_s *)dev;

  priv->handshake_sent = false;

  /* The M0 was loaded and released by U-Boot.  Open the time-base +
   * mailbox/intmux clocks it needs (U-Boot opened only HCLK_M0/
   * SWCLKTCK), then clear any stale A2B status so the first kick raises
   * a fresh level into the armed receiver.
   */

  rk3506_mcu_clock_enable();
  putreg32(MBOX_CHAN0, RK3506_MBOX3_BASE + MBOX_A2B_STATUS);

  /* Read-only readiness probe (non-fatal): wait for the M0 to have armed
   * its mailbox3 receive interrupt before the link-up kick.
   */

  rk3506_m0_wait_ready();

  return OK;
}

static int rp_stop(FAR struct rptun_dev_s *dev)
{
  /* The M0 life cycle belongs to U-Boot; there is nothing to stop here. */

  (void)dev;
  return OK;
}

static int rp_notify(FAR struct rptun_dev_s *dev, uint32_t vqid)
{
  FAR struct rk3506_rptun_priv_s *priv =
    (FAR struct rk3506_rptun_priv_s *)dev;

  /* Peer protocol (rockchip_rpmsg_mbox.c rk_rpmsg_notify / M0
   * rpmsg_remote_cb first_notify):
   *   - the peer spins waiting for its FIRST kick, which is the link-up
   *     handshake and is forced onto its tvq;
   *   - after that, only vring1 (master svq, consumed buffers) kicks
   *     are posted; further vring0 kicks are dropped (no tx-done);
   *
   * openamp emits notifyid = RSC_NOTIFY_ID_ANY twice before the link is
   * up: once when negotiating features (vdev status still 0, vring0 not
   * seeded yet) and once when setting DRIVER_OK (status 0x04).  Only the
   * latter is the Linux "first notify" handshake, so gate on the vdev
   * status byte in the resource table.
   */

  if (vqid == 1u)
    {
      rk3506_mbox_kick(1);
    }
  else if (!priv->handshake_sent &&
           (g_rk3506_rsc.rpmsg_vdev.status &
            VIRTIO_CONFIG_STATUS_DRIVER_OK) != 0)
    {
      /* openamp emits vdev-level notifyid (RSC_NOTIFY_ID_ANY) when the
       * DRIVER_OK status is set; the M0 treats its first kick as the
       * link-up event (forced onto its tvq).
       */

      priv->handshake_sent = true;
      rk3506_mbox_kick(0);
    }
  else
    {
      /* Feature-negotiation ANY while status != DRIVER_OK, and every
       * later vring0 notification, are dropped like the Linux driver.
       */
    }

  return OK;
}

static int rp_register_callback(FAR struct rptun_dev_s *dev,
                                rptun_callback_t callback, FAR void *arg)
{
  FAR struct rk3506_rptun_priv_s *priv =
    (FAR struct rk3506_rptun_priv_s *)dev;
  int ret;

  if (callback == NULL)
    {
      up_disable_irq(RK3506_IRQ_MAILBOX0);
      up_disable_irq(RK3506_IRQ_MAILBOX3);
      irq_detach(RK3506_IRQ_MAILBOX0);
      irq_detach(RK3506_IRQ_MAILBOX3);
      priv->callback = NULL;
      priv->cbarg    = NULL;
      return OK;
    }

  ret = irq_attach(RK3506_IRQ_MAILBOX0, rk3506_mbox_isr,
                   (FAR void *)RK3506_MBOX0_BASE);
  if (ret < 0)
    {
      _err("rptun: attach mbox0 irq failed: %d\n", ret);
      return ret;
    }

  ret = irq_attach(RK3506_IRQ_MAILBOX3, rk3506_mbox_isr,
                   (FAR void *)RK3506_MBOX3_BASE);
  if (ret < 0)
    {
      _err("rptun: attach mbox3 irq failed: %d\n", ret);
      irq_detach(RK3506_IRQ_MAILBOX0);
      return ret;
    }

  priv->callback = callback;
  priv->cbarg    = arg;

  /* Clear stale status, then arm the B2A receive interrupts (write
   * enable in the high half-word, enable bit0 low).
   */

  putreg32(MBOX_CHAN0, RK3506_MBOX0_BASE + MBOX_B2A_STATUS);
  putreg32(MBOX_CHAN0, RK3506_MBOX3_BASE + MBOX_B2A_STATUS);
  putreg32(MBOX_INTEN_WE | MBOX_CHAN0,
           RK3506_MBOX0_BASE + MBOX_B2A_INTEN);
  putreg32(MBOX_INTEN_WE | MBOX_CHAN0,
           RK3506_MBOX3_BASE + MBOX_B2A_INTEN);

  up_enable_irq(RK3506_IRQ_MAILBOX0);
  up_enable_irq(RK3506_IRQ_MAILBOX3);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_rptun_initialize
 ****************************************************************************/

int rk3506_rptun_initialize(void)
{
  FAR struct rk3506_rptun_priv_s *priv = &g_rk3506_rptun;
  int ret;

  memset(priv, 0, sizeof(*priv));
  priv->rptun.ops = &g_rk3506_rptun_ops;

  ret = rptun_initialize(&priv->rptun);
  if (ret < 0)
    {
      _err("rptun: rptun_initialize failed: %d\n", ret);
    }

  return ret;
}

#endif /* CONFIG_RK3506_RPTUN */
