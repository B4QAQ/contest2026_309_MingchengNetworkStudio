/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_rptun.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 A7-side rptun (remote processor tunnel) lower half for the bus
 * Cortex-M0 ("mcu"), used by the NuttX rpmsg stack (master role).
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
 * M0 boot (u-boot arch/arm/mach-rockchip/rk3506/rk3506.c
 * fit_standalone_release(); the prebuilt uboot.img FIT contains OP-TEE
 * whose plat-rockchip smc_sip.c serves the ROCKCHIP SIP and stays
 * resident, so NuttX can issue the same SMC):
 *   1. open M0 swclk/hclk gates: CRU_GATE_CON5 (0xff9a0000+0x814) <- 0x0c000000
 *   2. copy the raw M0 image to 0xfff84000 (A7 view of the M0 SRAM;
 *      the image links at M0 da 0x00000000, vector table at offset 0)
 *   3. SMC SIP_MCU_CFG (0x82000028) with (BUSMCU_0_ID = 0,
 *      MCU_CODE_START_ADDR = 1, entry = 0xfff84000)
 *   4. GRF_SOC_CON36 (0xff288090) <- 0x0bcd3d80 (M0 system time calibration)
 *   5. PMU_INT_MASK_CON (0xff90000c) <- 0x00060004
 *      (mcu_rst_dis_cfg = 1, glb_int_mask_mcu = 0)
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
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/rptun/rptun.h>
#include <nuttx/spinlock.h>

#include <openamp/remoteproc.h>
#include <openamp/virtio.h>

#ifdef CONFIG_RK3506_RPTUN_BOOT_M0
#  include "rk3506_mcu_fw.h"
#endif
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

/* A2B_INTEN bit0: remote-side rx interrupt enable.  The M0 firmware sets
 * this during rpmsg_lite_remote_init mailbox channel setup.  Used by the
 * boot handshake so the A7 does not kick before the M0 is ready.
 */

#define RK3506_MBOX_A2B_RX_ENABLE   (1u << 0)

/* Boot handshake: max wait for M0 to signal rx readiness, and poll step */

#define RK3506_MCU_READY_POLL_MS    1000
#define RK3506_MCU_READY_POLL_DELAY 1

/* Mailbox blocks used by the rpmsg link */

#define RK3506_MBOX0_BASE           0xff290000u /* rx: MCU tvq kicks        */
#define RK3506_MBOX3_BASE           0xff293000u /* rx: MCU rvq kicks + tx   */

/* CRU / GRF / PMU registers (u-boot rk3506.c fit_standalone_release)
 * Plus the peripheral clock gates the rpmsg/M0 path needs.  Sources:
 * kernel-6.1 drivers/clk/rockchip/clk-rk3506.c GATE() table —
 *   PCLK_MAILBOX CLKGATE_CON(6) bit 13, PCLK_INTMUX CON(6) bit 14,
 *   PCLK_UART4  CLKGATE_CON(11) bit 8,  SCLK_UART4  CON(11) bit 13.
 * Rockchip gate registers are HIWORD (write-enable in bits 31:16) with
 * set-to-disable semantics: writing bit=1 gates the clock, writing 0
 * with the WE bit enabled it.  CLKGATE_CON(n) = CRU + 0x800 + n*4.
 */

#define RK3506_CRU_BASE             0xff9a0000u
#define RK3506_CRU_GATE_CON(n)      (RK3506_CRU_BASE + 0x800 + (n) * 4)
#define RK3506_CRU_GATE_CON5        (RK3506_CRU_BASE + 0x814)
#define RK3506_M0_GATE_VAL          0x0c000000u /* WE 27|26 -> set bits 11,10 */

/* Enable (write 0 with WE) PCLK_MAILBOX | PCLK_INTMUX: with these gates
 * closed the mailbox register file and the M0's INTMUX (its mailbox IRQ
 * router) are dead - all A7 mbox reads return 0 and every write is
 * silently dropped (v8e: the v8d mbox probe read all zeros).
 */

#define RK3506_GATE_EN_MAILBOX_INTMUX \
                                    ((1u << (16 + 13)) | (1u << (16 + 14)))

/* Enable (write 0 with WE) PCLK_UART4 | SCLK_UART4 for the M0's debug
 * console (UART4 @ GPIO1_C2/C3, 1500000 baud, SDK rk3506-mcu main.c).
 */

#define RK3506_GATE_EN_M0_UART4     ((1u << (16 + 8)) | (1u << (16 + 13)))

/* Remaining AMP-core clocks.  rk3506-amp.dtsi declares exactly what the A
 * side must enable for the remote core:
 *
 *   clocks = <&cru HCLK_M0>, <&cru STCLK_M0>, <&cru SCLK_UART4>,
 *            <&cru PCLK_UART4>, <&cru PCLK_TIMER>, <&cru CLK_TIMER0_CH5>;
 *
 * On Linux the rockchip-amp driver clk_prepare_enable()s all six.  u-boot's
 * fit_standalone_release() only opens HCLK_M0 (CLKGATE_CON5 bit10 via
 * 0x0c000000), because Linux does the rest later - so a port that copies
 * only the u-boot sequence leaves the M0 without its timebase.
 *
 * The M0 needs them from its very first instruction: rk3506-mcu hal_conf.h
 * has "#define SYS_TIMER TIMER5" plus HAL_SYSTICK_MODULE_ENABLED, and
 * main.c calls HAL_Init() before anything else, so any HAL delay spins on a
 * timer/SysTick that never advances -> the M0 hangs before rpmsg init.
 *
 * clk-rk3506.c gate bits (SET_TO_DISABLE, so write the WE bit with data 0):
 *   PCLK_TIMER      CLKGATE_CON(6) bit 2   (timer register access)
 *   CLK_TIMER0_CH5  CLKGATE_CON(6) bit 8   (TIMER5 = the M0's SYS_TIMER)
 *   STCLK_M0        CLKGATE_CON(8) bit 2   (the M0's SysTick clock)
 */

#define RK3506_GATE_EN_M0_TIMER     ((1u << (16 + 2)) | (1u << (16 + 8)))
#define RK3506_GATE_EN_M0_STCLK     (1u << (16 + 2))

/* Execution canary: M0 SRAM is 0xfff80000+0xc000 (rk3506-amp.dtsi
 * mcu_reserved) and the image is loaded at 0xfff84000, so the M0 owns
 * 32 KiB - matching its linker script (RAM LENGTH = 0x8000, __STACK_SIZE
 * 0x400, vector[0] SP = 0x7c00).  The 3 KiB at M0 offset 0x7000..0x7c00 is
 * bss/heap topped by the descending stack and lies well above the 0x69f0
 * image, so filling it with a pattern before release and checking it after
 * proves whether the M0 executed at all (bss zeroing or the first stack
 * push both disturb it).
 */

#define RK3506_MCU_CANARY_OFF       0x7000u
#define RK3506_MCU_CANARY_LEN       0x0c00u
#define RK3506_MCU_CANARY_VAL       0xa5a5a5a5u

/* INTMUX: the M0's second-level interrupt controller.  The M0 takes the
 * mailbox3 receive interrupt as MAILBOX_BB_3_IRQn, which soc.h numbers
 * 117 + NUM_INTERRUPTS(32) = 149; hal_intmux.c's _TO_INTMUX_* macros turn
 * that into INTMUX input (149 - 32) = 117, i.e. group 117/32 = 3, bit
 * 117%32 = 21, feeding INTMUX_OUT3 = NVIC IRQ 31.  INT_ENABLE_GROUP[] is
 * at offset 0x00 and the read-only INT_FLAG_GROUP[] at 0x80 (rk3506.h
 * struct INTMUX_REG), so group 3 lives at +0x0c / +0x8c.
 *
 * INT_ENABLE_GROUP[3] bit21 is the single best readiness signal we have:
 * its only writer anywhere in the SDK is HAL_INTMUX_EnableIRQ() called
 * from the MCU's own rpmsg_platform.c:295, which is the *last* step of
 * rpmsg_lite_remote_init() - after HAL_MBOX_RegisterClient() has already
 * armed A2B_INTEN and w1c-cleared A2B_STATUS.  Neither the A7, u-boot,
 * OP-TEE nor Linux ever touch INTMUX, and it reads 0 out of reset, so
 * unlike A2B_INTEN bit0 it cannot be a stale value from a previous boot.
 * Seeing it set means "the M0 finished rpmsg init and its whole delivery
 * path (mailbox -> INTMUX -> NVIC) is armed": exactly when a kick is safe.
 */

#define RK3506_INTMUX_EN_GROUP3     0xff2a000cu
#define RK3506_INTMUX_FLAG_GROUP3   0xff2a008cu
#define RK3506_INTMUX_MBOX3_BIT     (1u << 21)

/* Extra grace period for the INTMUX arming that follows the mailbox
 * arming by only a handful of MCU instructions.
 */

#define RK3506_MCU_INTMUX_POLL_MS   100

/* Cortex-M0 has no VTOR: its vector table is fixed at address 0 (the SMC
 * code-start remap points that at our SRAM) and start_rk3506_mcu.S:41
 * reserves the 64 IRQ vectors as ".space (64 * 4)" - all zero in the
 * image.  Handlers are written at run time by hal_nvic.c:55, which pokes
 * "(uint32_t *)0x0U".  So the IRQ31 (INTMUX_OUT3) slot at 0x40 + 31*4 =
 * 0xbc is zero in the blob and becomes an odd (Thumb) code pointer only
 * once the M0 has actually executed HAL_INTMUX_Init() from SystemInit().
 * Reading it from the A7 is therefore a direct, hardware-independent
 * "did the M0 execute any code at all" test - and it also catches the
 * case where the remap left address 0 unwritable, which would silently
 * drop the handler stores and turn the first IRQ into a HardFault.
 */

#define RK3506_MCU_VECTOR_IRQ31_OFF 0x000000bcu

#define RK3506_GRF_SOC_CON36        0xff288090u
#define RK3506_SOC_CON36_VAL        0x0bcd3d80u

/* PMU_INT_MASK_CON.  u-boot fit_standalone_release() ends with
 * "writel(0x00060004, 0xff90000c)" and comments it as "enable m0
 * interrupt: mcu_rst_dis_cfg=1, glb_int_mask_mcu=0".  The write-enable
 * bits are 17|18, so bit2 = mcu_rst_dis_cfg and bit1 = glb_int_mask_mcu.
 * This register is NOT described in the CMSIS header (the PMU block at
 * 0xff900000 is absent from rk3506.h); that source comment is its only
 * documentation anywhere in the SDK.
 *
 * mcu_rst_dis_cfg literally reads "MCU reset disable config" and u-boot
 * sets it as the LAST step of releasing the core, so clearing it is the
 * closest thing to a vendor-sanctioned "hold the M0" that exists.  We use
 * it as stage 1 of the halt, before the CRU softreset, because while it
 * stays 1 (its state after a warm reboot) a CRU reset request may simply
 * be masked off.
 */

#define RK3506_PMU_INT_MASK_CON     0xff90000cu
#define RK3506_PMU_INT_MASK_VAL     0x00060004u /* WE 18|17: rst_dis=1 */
#define RK3506_PMU_INT_MASK_HOLD    0x00060000u /* WE 18|17: rst_dis=0 */

/* M0 softreset.  CRU_SOFTRST_CON(x) = CRU_BASE + 0xa00 + x*4 (rk3506.h
 * CRU_SOFTRST_CON00_OFFSET 0xA00, kernel clk.h RK3506_SOFTRST_CON(x)).
 * The softreset registers are HIWORD-masked with ROCKCHIP_SOFTRST_HIWORD_
 * MASK (clk-rk3506.c:837 registers all 23 of them that way; softrst.c:95
 * writes "BIT(n) | BIT(n) << 16" to assert and "BIT(n) << 16" to release).
 *
 * NOTE the polarity is the OPPOSITE of the gate registers: for SOFTRST a
 * data bit of 1 ASSERTS reset, while for CLKGATE (SET_TO_DISABLE) a data
 * bit of 1 DISABLES the clock.  Both use the same WE convention, so a
 * value copied from the wrong family silently does the wrong thing.
 *
 *   CRU_SOFTRST_CON05 bit10 = HRESETN_M0       (rk3506.h:19111)
 *   CRU_SOFTRST_CON05 bit11 = RESETN_M0_JTAG   (rk3506.h:19113)
 *   CRU_SOFTRST_CON06 bit13 = PRESETN_MAILBOX  (rk3506.h:19145)
 *   CRU_SOFTRST_CON06 bit14 = PRESETN_INTMUX   (rk3506.h:19147)
 *
 * Cross-checked against the kernel dt-binding ids (SRST_H_M0 90,
 * SRST_M0_JTAG 91, SRST_P_MAILBOX 109, SRST_P_INTMUX 110; id/16 = bank,
 * id%16 = bit) and the HAL ids in rk3506_cru.h (0x5A/0x5B/0x6D/0x6E with
 * hal_cru.h's CLK_RESET_GET_REG_OFFSET/_BITS_SHIFT).
 *
 * !! Never touch CON05 bits 6/7 (ARESETN_SYSRAM / HRESETN_SYSRAM): that is
 *    the SRAM we are loading the firmware into.  Never touch CON00 either:
 *    that bank holds the A7 core resets (NCORESET / NL2RESET), and although
 *    it also carries HRESETN_M0_AC (bit10) a mis-set WE mask there resets
 *    the cores we are running on.
 *
 * !! NOT SDK-ATTESTED: no code anywhere in the SDK ever asserts an M0
 *    reset, and rk3506 is the only SoC of its family whose release path
 *    does not use CRU at all (rk3562/rk3576/rk3528 all write SOFTRST to
 *    deassert; rk3506's CRU_SOFTRST_CON5 macro is dead code).  Whether the
 *    release is owned by the TEE's MCU_CODE_START_ADDR handler or by
 *    mcu_rst_dis_cfg cannot be determined - the SDK ships no OP-TEE source,
 *    only the prebuilt rk3506_tee_v2.10.bin.  The halt is therefore
 *    verified at run time by the canary (see rk3506_mcu_hold below).
 */

#define RK3506_CRU_SOFTRST_CON(n)   (RK3506_CRU_BASE + 0xa00 + (n) * 4)
#define RK3506_SRST_ASSERT(b)       ((1u << ((b) + 16)) | (1u << (b)))
#define RK3506_SRST_RELEASE(b)      (1u << ((b) + 16))

#define RK3506_SRST_M0_ASSERT       (RK3506_SRST_ASSERT(10) | \
                                     RK3506_SRST_ASSERT(11))
#define RK3506_SRST_M0_RELEASE      (RK3506_SRST_RELEASE(10) | \
                                     RK3506_SRST_RELEASE(11))
#define RK3506_SRST_LINK_ASSERT     (RK3506_SRST_ASSERT(13) | \
                                     RK3506_SRST_ASSERT(14))
#define RK3506_SRST_LINK_RELEASE    (RK3506_SRST_RELEASE(13) | \
                                     RK3506_SRST_RELEASE(14))

/* Softreset settle time.  The SDK specifies no pulse width for any
 * softreset; rk3506_cru.c already uses a short udelay between assert and
 * release for SRST_A_MAC0, so mirror that order of magnitude.
 */

#define RK3506_SRST_SETTLE_US       20

/* How long to watch the canary for M0 writes after the halt (ms). */

#define RK3506_MCU_HOLD_CHECK_MS    2

/* M0 code SRAM (A7 view); the MCU links at da 0x00000000 */

#define RK3506_MCU_SRAM_ADDR        0xfff84000u

/* ROCKCHIP SIP (u-boot rockchip_smccc.h) */

#define ROCKCHIP_SIP_MCU_CFG        0x82000028u
#define ROCKCHIP_SIP_BUSMCU_0_ID    0x00u
#define ROCKCHIP_SIP_MCU_CODE_START 0x01u

/* SMC #0 instruction (ARM encoding; avoids needing .arch_extension sec) */

#define ARM_SMC_INSN                0xe1600070u

/* Kick message content (hal rpmsg_platform.c platform_notify) */

#define RK3506_RPMSG_LINK_ID        0x03u       /* RL_PLATFORM_SET_LINK_ID(0, 3) */
#define RK3506_RPMSG_MAGIC          0x524d5347u /* RL_RPMSG_MAGIC "RMSG" */

/* Notify busy-wait bound: the MCU clears A2B_STATUS bit0 as soon as its
 * INTMUX ISR runs; a few hundred register-poll iterations are plenty. */

#define RK3506_NOTIFY_SPIN_MAX      10000u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_rptun_dev_s
{
  struct rptun_dev_s    rptun;      /* Rptun device (MUST be first) */
  spinlock_t            lock;       /* Protects mailbox tx */
  rptun_callback_t      callback;   /* Upper half callback */
  FAR void             *arg;        /* Callback arg */
  bool                  m0_booted;  /* M0 started once */
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
 * Name: rk3506_mcu_smc
 *
 * Description:
 *   Issue a fast SMC call to the resident secure firmware (OP-TEE
 *   plat-rockchip smc_sip.c).  SMCCC: a0-a3 in r0-r3, result in r0-r3;
 *   r0-r3 are caller-saved per the AAPCS so no extra clobber list beyond
 *   memory is required.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3506_RPTUN_BOOT_M0
static unsigned long rk3506_mcu_smc(unsigned long fid, unsigned long a1,
                                    unsigned long a2, unsigned long a3)
{
  register unsigned long r0 asm("r0") = fid;
  register unsigned long r1 asm("r1") = a1;
  register unsigned long r2 asm("r2") = a2;
  register unsigned long r3 asm("r3") = a3;

  __asm__ __volatile__
    (
      ".word 0xe1600070\n"          /* SMC #0 */
      : "+r" (r0), "+r" (r1), "+r" (r2), "+r" (r3)
      :
      : "memory", "cc"
    );

  return r0;
}

/****************************************************************************
 * Name: rk3506_mcu_canary_fill / rk3506_mcu_canary_dirty
 *
 * Description:
 *   Fill / test the execution canary region in the M0's SRAM.  Used both
 *   to prove the halt in rk3506_mcu_hold() and to classify a failed
 *   readiness handshake later on.
 *
 ****************************************************************************/

static void rk3506_mcu_canary_fill(void)
{
  unsigned int off;

  for (off = 0; off < RK3506_MCU_CANARY_LEN; off += 4)
    {
      putreg32(RK3506_MCU_CANARY_VAL,
               RK3506_MCU_SRAM_ADDR + RK3506_MCU_CANARY_OFF + off);
    }
}

static unsigned int rk3506_mcu_canary_dirty(void)
{
  unsigned int dirty = 0;
  unsigned int off;

  for (off = 0; off < RK3506_MCU_CANARY_LEN; off += 4)
    {
      if (getreg32(RK3506_MCU_SRAM_ADDR + RK3506_MCU_CANARY_OFF + off) !=
          RK3506_MCU_CANARY_VAL)
        {
          dirty++;
        }
    }

  return dirty;
}

/****************************************************************************
 * Name: rk3506_mcu_hold
 *
 * Description:
 *   Put the bus M0 into reset and prove that it stopped.
 *
 *   This is the step u-boot never has to perform: it releases the core
 *   exactly once, from a cold power-on, with the firmware already in SRAM.
 *   We are in the opposite situation - after an A7 warm reboot (including
 *   the one this driver's own crash handler triggers) the M0 from the
 *   previous session is STILL EXECUTING, because nothing in the SoC resets
 *   it when the A7 restarts.  Loading a new image on top of a running core
 *   corrupts its code, vector table, .data and .bss mid-execution, and a
 *   derailed M0 can store anywhere in the 4 GiB map - including A7 DRAM.
 *
 *   It also explains the stale-handshake failures: A2B_INTEN bit0 (v8f) and
 *   INT_ENABLE_GROUP[3] bit21 (v8h) were never "leftover register values",
 *   they were being driven by a live previous-generation M0.  Holding the
 *   core and pulsing the mailbox/INTMUX presets is what finally makes those
 *   two signals mean "this boot".
 *
 *   Both halt mechanisms are applied, weakest-assumption first:
 *
 *     1. mcu_rst_dis_cfg = 0.  u-boot sets this bit to 1 as the final step
 *        of releasing the core, so clearing it is the vendor sequence run
 *        backwards.  It must come first: while it reads 1 a CRU reset
 *        request may be masked off entirely.
 *     2. HRESETN_M0 + RESETN_M0_JTAG asserted in CRU_SOFTRST_CON05.  This
 *        mirrors what rk3562/rk3576/rk3528 deassert in their own release
 *        paths, but is NOT attested for rk3506 (see the macro comment).
 *
 *   Because step 2 is extrapolation, the result is measured rather than
 *   assumed: the canary region is filled and then re-read after a short
 *   window.  A held core cannot touch it; a running one almost certainly
 *   will (its bss/heap live there).  A clean read-back turns the riskiest
 *   assumption in this sequence into board evidence, and a dirty one is
 *   reported so we know the halt did not take.
 *
 * Returned Value:
 *   true if the M0 is believed held (canary intact), false otherwise.
 *
 ****************************************************************************/

static bool rk3506_mcu_hold(void)
{
  bool held;

  /* 1. Re-enable the M0 reset path (mcu_rst_dis_cfg = 0), keeping
   *    glb_int_mask_mcu = 0 as u-boot leaves it.
   */

  putreg32(RK3506_PMU_INT_MASK_HOLD, RK3506_PMU_INT_MASK_CON);

  /* 2. Assert the M0 core and JTAG resets. */

  putreg32(RK3506_SRST_M0_ASSERT, RK3506_CRU_SOFTRST_CON(5));
  up_udelay(RK3506_SRST_SETTLE_US);

  /* 3. Measure: fill the canary, give a stopped core no chance to write
   *    and a running one every chance, then read it back.
   */

  rk3506_mcu_canary_fill();
  up_mdelay(RK3506_MCU_HOLD_CHECK_MS);
  held = (rk3506_mcu_canary_dirty() == 0);

  if (!held)
    {
      _err("ERROR: M0 still running with HRESETN_M0 asserted "
           "(canary at 0x%08lx dirtied within %d ms). Reloading its "
           "firmware anyway is unsafe: a derailed M0 can corrupt A7 "
           "memory. PMU 0x%08lx CRU_SOFTRST_CON05 0x%08lx\n",
           (unsigned long)(RK3506_MCU_SRAM_ADDR + RK3506_MCU_CANARY_OFF),
           RK3506_MCU_HOLD_CHECK_MS,
           (unsigned long)getreg32(RK3506_PMU_INT_MASK_CON),
           (unsigned long)getreg32(RK3506_CRU_SOFTRST_CON(5)));
    }

  /* 4. Pulse the mailbox and INTMUX APB resets while the core is down, so
   *    that neither handshake signal can survive from the previous
   *    session.  Whether these presets actually clear A2B_INTEN and
   *    INT_ENABLE_GROUP[] is not documented in the SDK, so the explicit
   *    A2B_INTEN clears further down are kept as the attested fallback.
   */

  putreg32(RK3506_SRST_LINK_ASSERT, RK3506_CRU_SOFTRST_CON(6));
  up_udelay(RK3506_SRST_SETTLE_US);
  putreg32(RK3506_SRST_LINK_RELEASE, RK3506_CRU_SOFTRST_CON(6));

  return held;
}

/****************************************************************************
 * Name: rk3506_mcu_boot
 *
 * Description:
 *   Boot the bus M0 with the embedded rpmsg-test firmware, replicating
 *   u-boot fit_standalone_release().  Safe to call once per power cycle;
 *   further calls are ignored so that an rptun stop/start cycle cannot
 *   overwrite a running M0.
 *
 ****************************************************************************/

static void rk3506_mcu_boot(void)
{
  unsigned long ret;
  uint32_t vector;
  unsigned int dirty;
  bool held;
  int i;
  int j;

  if (g_rk3506_rptun.m0_booted)
    {
      return;
    }

  /* 0. Stop any M0 left running by a previous session BEFORE touching its
   *    SRAM, and pulse the mailbox/INTMUX presets so the readiness
   *    handshake below can only observe state produced by this boot.
   */

  held = rk3506_mcu_hold();

  /* 0a. Disarm the remote rx-enable bit before the core is released, so
   *    that the readiness handshake in step 6 can only ever observe a
   *    value the M0 wrote during *this* boot.  Kept even though step 0
   *    pulses PRESETN_MAILBOX: the SDK does not document whether that
   *    preset clears A2B_INTEN, so this is the attested fallback.
   *
   *    HIWORD write-enable is confirmed on this register (writing
   *    0x01000100 reads back as 0x101, bit24 is not stored), so WE(0)
   *    with data bit0 = 0 clears just the rx-enable and leaves the A7's
   *    own trigger-method bit8 untouched.
   */

  putreg32(RK3506_MBOX_WE(0), RK3506_MBOX3_BASE + RK3506_MBOX_A2B_INTEN);
  putreg32(RK3506_MBOX_WE(0), RK3506_MBOX0_BASE + RK3506_MBOX_A2B_INTEN);

  /* 1. Open the M0 swclk/hclk (and bus) gates before touching the M0
   *    SRAM or starting the core.  HIWORD value written by u-boot.
   */

  putreg32(RK3506_M0_GATE_VAL, RK3506_CRU_GATE_CON5);

  /* 1b. Open the AMP-core clocks that u-boot leaves to Linux: PCLK_TIMER
   *     and CLK_TIMER0_CH5 (the M0's SYS_TIMER = TIMER5) plus STCLK_M0
   *     (its SysTick).  Without these the M0's timebase never advances
   *     and HAL_Init() spins forever - see the macro comment above.
   */

  putreg32(RK3506_GATE_EN_M0_TIMER, RK3506_CRU_GATE_CON(6));
  putreg32(RK3506_GATE_EN_M0_STCLK, RK3506_CRU_GATE_CON(8));

  /* 2. Copy the raw M0 image into the M0 code SRAM.  The region is
   *    identity-mapped as device memory (rk3506_start.c maps
   *    0xff000000-0xffffffff), so stores bypass the dcache.
   */

  memcpy((FAR void *)(uintptr_t)RK3506_MCU_SRAM_ADDR,
         g_rk3506_mcu_fw, RK3506_MCU_FW_SIZE);

  /* 2a. Re-lay the execution canary above the image (rk3506_mcu_hold()
   *     already used it to prove the halt; the memcpy does not reach it,
   *     but refill it anyway so the post-handshake classification below
   *     starts from a known pattern).
   */

  rk3506_mcu_canary_fill();

  /* 2b. Enable PCLK_UART4 + SCLK_UART4 (CLKGATE_CON(11) bits 8,13,
   *     kernel clk-rk3506.c).  The M0 firmware prints its first log
   *     ("Hello RK3506 mcu", SDK main.c) on UART4 immediately after
   *     start, so the gates must be open before the core is released.
   *     SET_TO_DISABLE convention: write the WE bits with data = 0.
   */

  putreg32(RK3506_GATE_EN_M0_UART4, RK3506_CRU_GATE_CON(11));

  /* 3. Tell the secure firmware to configure the bus MCU and set its
   *    code start address.  a0=SIP_MCU_CFG, a1=mcu id 0, a2=function
   *    MCU_CODE_START_ADDR, a3=entry point (system address).
   */

  ret = rk3506_mcu_smc(ROCKCHIP_SIP_MCU_CFG,
                       ROCKCHIP_SIP_BUSMCU_0_ID,
                       ROCKCHIP_SIP_MCU_CODE_START,
                       RK3506_MCU_SRAM_ADDR);
  if (ret != 0)
    {
      _err("ERROR: SIP_MCU_CFG failed: %lu\n", ret);
      return;
    }

  /* 4. M0 system time calibration (GRF_SOC_CON36). */

  putreg32(RK3506_SOC_CON36_VAL, RK3506_GRF_SOC_CON36);

  /* 5. Release the M0, undoing rk3506_mcu_hold() in reverse order: first
   *    deassert the CRU core/JTAG resets, then re-arm the PMU reset-disable
   *    bit.  Finishing with the PMU write leaves exactly the register state
   *    u-boot's fit_standalone_release() ends in, so whichever of the two
   *    actually owns the release on this SoC (the SDK does not say - see
   *    the RK3506_CRU_SOFTRST_CON macro comment) the core does start.
   */

  putreg32(RK3506_SRST_M0_RELEASE, RK3506_CRU_SOFTRST_CON(5));
  putreg32(RK3506_PMU_INT_MASK_VAL, RK3506_PMU_INT_MASK_CON);

  g_rk3506_rptun.m0_booted = true;

  /* 6. Wait until the M0 arms its own mailbox receive interrupt before
   *    returning, so that no kick can be posted while its receiver is
   *    still masked.  rk3506_rptun_start() calls us before the rpmsg
   *    framework issues its first notify(), hence blocking here covers
   *    every kick.
   *
   *    Why this is needed (v8e board data): once the mailbox pclk gate
   *    bug was fixed, the A7 kick did land in the registers (A2B_CMD =
   *    0x03, A2B_DATA = 0x524d5347) but A2B_STATUS bit0 stayed set
   *    forever and the M0 never answered.  The A7 posted that kick
   *    microseconds after releasing the core, while the M0 was still in
   *    HAL/UART/INTMUX/rpmsg init with A2B_INTEN bit0 == 0: the status
   *    0->1 edge happened with the remote receive interrupt masked, so
   *    the M0 never took the IRQ, never w1c-cleared the status, and all
   *    later kicks were refused as "A2B busy".  A2B_INTEN bit0 is
   *    written only by the M0 (the A7 writes bit8 alone), which makes
   *    reading it back a reliable "remote receiver armed" handshake.
   */

  for (i = 0; i < RK3506_MCU_READY_POLL_MS; i++)
    {
      if (getreg32(RK3506_MBOX3_BASE + RK3506_MBOX_A2B_INTEN) &
          RK3506_MBOX_A2B_RX_ENABLE)
        {
          break;
        }

      up_mdelay(RK3506_MCU_READY_POLL_DELAY);
    }

  if (i >= RK3506_MCU_READY_POLL_MS)
    {
      dirty = rk3506_mcu_canary_dirty();
      vector = getreg32(RK3506_MCU_SRAM_ADDR +
                        RK3506_MCU_VECTOR_IRQ31_OFF);

      if (!held)
        {
          _err("ERROR: the previous M0 could not be halted (see the "
               "HRESETN_M0 error above), so this boot loaded firmware "
               "on top of a running core - the handshake result below "
               "is meaningless and A7 memory may already be corrupt\n");
        }

      if (dirty == 0 && vector == 0)
        {
          _err("ERROR: M0 never executed a single instruction: canary at "
               "+0x%lx fully intact AND IRQ31 vector slot still 0 after "
               "%d ms - suspect the SMC code-start remap or the M0 core "
               "gates/resets\n",
               (unsigned long)RK3506_MCU_CANARY_OFF,
               RK3506_MCU_READY_POLL_MS);
        }
      else if (vector == 0)
        {
          _err("ERROR: M0 dirtied %u/%lu canary words but its IRQ31 vector "
               "slot is STILL 0 after %d ms: the M0 runs yet cannot write "
               "its vector table at address 0 - the code-start remap gave "
               "it read-only or unmapped memory there, so the first "
               "mailbox IRQ would HardFault\n",
               dirty, (unsigned long)(RK3506_MCU_CANARY_LEN / 4),
               RK3506_MCU_READY_POLL_MS);
        }
      else
        {
          _err("ERROR: M0 is alive (canary %u/%lu dirty, IRQ31 vector "
               "0x%08lx) but never armed its mailbox rx within %d ms: it "
               "hangs inside rpmsg init - tap M0 UART4 GPIO1_C2/C3 "
               "@1500000\n",
               dirty, (unsigned long)(RK3506_MCU_CANARY_LEN / 4),
               (unsigned long)vector, RK3506_MCU_READY_POLL_MS);
        }
    }
  else
    {
      /* The mailbox is armed, but HAL_MBOX_RegisterClient() runs a few
       * instructions before HAL_INTMUX_EnableIRQ(); the level-sensitive
       * mailbox output is only actually delivered once INTMUX group 3
       * bit21 is set.  Wait for that too so a kick cannot land in the
       * narrow window in between.  Should INTMUX be unreadable from the
       * A7 for any reason we must not regress below v8f, so this stage
       * only warns and proceeds.
       */

      for (j = 0; j < RK3506_MCU_INTMUX_POLL_MS; j++)
        {
          if (getreg32(RK3506_INTMUX_EN_GROUP3) &
              RK3506_INTMUX_MBOX3_BIT)
            {
              break;
            }

          up_mdelay(RK3506_MCU_READY_POLL_DELAY);
        }
    }

  /* Drop any stale pending message so the first real kick produces a
   * fresh 0->1 edge on A2B_STATUS for the now-armed M0 receiver
   * (A2B_STATUS bit0 is write-1-to-clear).
   */

  putreg32(RK3506_MBOX_A2B_MSG_PEND,
           RK3506_MBOX3_BASE + RK3506_MBOX_A2B_STATUS);
}
#endif /* CONFIG_RK3506_RPTUN_BOOT_M0 */

/****************************************************************************
 * Name: rk3506_mbox_getreg / putreg32 helpers
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
 *   Initialize the two mailbox blocks used by the rpmsg link:
 *     - mailbox3: A7 -> MCU kick channel (A2B regs, trigger method 1)
 *                 + MCU -> A7 q1 kick channel (B2A regs, int bit0)
 *     - mailbox0: MCU -> A7 q0 kick channel (B2A regs, int bit0)
 *   Mirrors hal_mbox.c MBOX_ChanEnable() (v2) and the kernel's
 *   rockchip-mailbox.c trigger-method handling.  Interrupt enables are
 *   applied later in register_callback() once the upper half callback is
 *   registered; here only the status flags are cleared.
 *
 ****************************************************************************/

static void rk3506_mbox_init(void)
{
  /* Enable PCLK_MAILBOX + PCLK_INTMUX (CLKGATE_CON(6) bits 13,14,
   * kernel clk-rk3506.c GATE table).  Rockchip gates are
   * SET_TO_DISABLE (kernel GFLAGS = CLK_GATE_HIWORD_MASK |
   * CLK_GATE_SET_TO_DISABLE): writing bit=1 GATES the clock off,
   * writing bit=0 with the HIWORD write-enable turns it on - same
   * convention as rk3506_cru.c rk3506_spi1_clock_init() (gates<<16).
   * v8b-v8d wrote WE|bit here, i.e. the value that CLOSES the gate:
   * every mailbox register access afterwards was dead (reads 0,
   * dropped writes) - the v8d mbox probe all-zeros and the never-
   * delivered A7->M0 kick are both explained by this one line.  The
   * MCU also uses the mailboxes and routes its mailbox IRQ through
   * the INTMUX, so both clocks must be on before any mbox register
   * write below. */

  putreg32(RK3506_GATE_EN_MAILBOX_INTMUX, RK3506_CRU_GATE_CON(6));

  /* mailbox3: clear stale status bits on both directions; set the A2B
   * trigger method to 1 (write CMD then DATA) without disturbing the
   * MCU's own A2B_INTEN bit0 (rx enable on its side). */

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
  /* The M0 is booted here (rptun_do_start calls this after the resource
   * table is set up and before register_callback()). */

#ifdef CONFIG_RK3506_RPTUN_BOOT_M0
  rk3506_mcu_boot();
#endif
  return OK;
}

static int rk3506_rptun_stop(FAR struct rptun_dev_s *dev)
{
  /* The M0 keeps running; a restart must not re-copy the firmware over a
   * live core (rk3506_mcu_boot() guards against it). */

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
 *   thread, boots the M0 and brings up the rpmsg virtio device
 *   (/dev/rpmsg/mcu once the virtio rpmsg driver probes it).
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
