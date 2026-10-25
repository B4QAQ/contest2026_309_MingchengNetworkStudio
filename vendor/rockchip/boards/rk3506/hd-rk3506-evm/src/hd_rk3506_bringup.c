/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations under
 * the License.
 *
 * Updated (2026-08-27):
 *   - Continue past individual driver failures (was returning on first error)
 *   - Add I2C bus registration (CONFIG_RK3506_I2C)
 *   - Better logging with subsystem names
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/board.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/fb.h>

#include "rk3506_usbhost.h"
#include "rk3506_i2c.h"
#include "drv/spi/rk3506_spi.h"
#include "rk3506_spinand.h"
#include "rk3506_cru.h"
#include "rk3506_iomux.h"
#include "rk3506_fspi.h"
#include "rk3506_gmac0.h"
#include "hd_rk3506.h"

#ifdef CONFIG_RK3506_SPINAND_FSPI
#  include <nuttx/mtd/mtd.h>

/* vendor/rockchip/chips/rk3506/rk3506_spinand_fspi.c */

FAR struct mtd_dev_s *rk3506_spinand_fspi_initialize(void);

/* Start of the "userdata" region in nand_firmware/parameter.txt (v8, A/B
 * layout with the amp partition: mtdparts sector 0x11000, 512-byte units)
 * = 35,651,584 bytes (34 MiB, immediately after amp).  Everything below
 * this offset belongs to the boot chain
 * (vnvm/uboot/misc/boot_a/boot_b/amp) and is only touched via the OTA
 * manager / U-Boot.
 */

#  define SPINAND_USERDATA_START_BYTES 0x11000ULL * 512ULL
#endif

#ifdef CONFIG_RK3506_OTA
/* vendor/rockchip/chips/rk3506/rk3506_ota.c */

int rk3506_ota_initialize(FAR struct mtd_dev_s *mtd);
#endif

#ifdef CONFIG_RK3506_PWM
/* vendor/rockchip/chips/rk3506/rk3506_pwm.c */

int rk3506_pwm_initialize(void);
#endif

#ifdef CONFIG_RK3506_SARADC
/* vendor/rockchip/chips/rk3506/rk3506_saradc.c */

int rk3506_saradc_initialize(void);
#endif

#ifdef CONFIG_RK3506_CAN
/* vendor/rockchip/chips/rk3506/rk3506_can.c */

int rk3506_can_initialize(void);
#endif

#ifdef CONFIG_RK3506_WDT
/* vendor/rockchip/chips/rk3506/rk3506_wdt.c */

int rk3506_wdt_initialize(void);
#endif

#ifdef CONFIG_RK3506_RPTUN
/* vendor/rockchip/chips/rk3506/rk3506_rptun.c */

int rk3506_rptun_initialize(void);
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Helper to log + return; never aborts the boot sequence. */

#define BRINGUP_LOG_ERR(stmt, fmt, ...) \
  do { \
    int _r = (stmt); \
    if (_r < 0) \
      { \
        _err("ERROR: " fmt " (errno=%d)\n", \
               ##__VA_ARGS__, _r); \
      } \
  } while (0)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_i2c_init
 *
 * Description:
 *   Initialize I2C bus controllers.
 *
 *   I2C client drivers (like GT911) call I2C_TRANSFER() on the master
 *   device returned by rk3506_i2cbus_initialize().  The client obtains
 *   the master via CONFIG_RK3506_I2C0_BUS=y etc. so we just need to
 *   initialize the controllers here.  No /dev/i2cN character device is
 *   created unless CONFIG_I2C_DRIVER is also enabled.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3506_I2C
static int hd_rk3506_i2c_init(void)
{
  int ret = OK;

  _info("Initializing I2C controllers...\n");

  /* Route RM_IO24/25 to I2C0 SDA/SCL first: without this the
   * GPIO1_B1/B2 pins stay in func 0 (GPIO) and every I2C0 transfer
   * NAKs (GT911 probe failed with -110 ETIMEDOUT).
   */

  rk3506_ioc_i2c0_setup();

  /* I2C0: GT911 touch controller (0x5D/0x14) */

  if (rk3506_i2cbus_initialize(0) == NULL)
    {
      _err("ERROR: rk3506_i2cbus_initialize(0) failed\n");
      ret = -ENODEV;
    }
  else
    {
      _info("I2C0 controller initialized (used by GT911)\n");
    }

  /* NOTE: only I2C0 is brought up.  The chip driver supports 3 buses
   * (RK3506_I2C_NUM_BUS), but I2C1/I2C2 have no pads or devices wired
   * on this board, so they are intentionally not initialized here (the
   * earlier CONFIG_RK3506_I2C1/2 blocks were dead code: those symbols
   * were never defined in Kconfig).
   */

  /* Optionally register I2C character device for userspace testing.
   * When CONFIG_I2C_DRIVER is enabled, we need to also expose the I2C
   * controller as a character device. The I2C client drivers (e.g. GT911)
   * do NOT need this - they call I2C_TRANSFER() directly.
   *
   * NOTE: This is left disabled by default; user can enable via
   * CONFIG_I2C_DRIVER=y in defconfig.
   */

#ifdef CONFIG_I2C_DRIVER
  _info("I2C_DRIVER enabled - userspace /dev/i2cN access available\n");
#endif

  return ret;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_bringup
 *
 * Description:
 *   Bring up board features. Each driver is initialized independently and
 *   errors are logged but do not abort the boot sequence.
 *
 ****************************************************************************/

int hd_rk3506_bringup(void)
{
  int ret;

  _info("\n"
         " ██████╗ ███████╗██████╗ ███╗   ██╗██╗   ██╗███████╗██╗      █████╗                             \n"
         "██╔═══██╗██╔════╝██╔══██╗████╗  ██║██║   ██║██╔════╝██║     ██╔══██╗                            \n"
         "██║   ██║█████╗  ██████╔╝██╔██╗ ██║██║   ██║█████╗  ██║     ███████║                            \n"
         "██║   ██║██╔══╝  ██╔═══╝ ██║╚██╗██║╚██╗ ██╔╝██╔══╝  ██║     ██╔══██║                            \n"
         "╚██████╔╝███████╗██║     ██║ ╚████║ ╚████╔╝ ███████╗███████╗██║  ██║                            \n"
         " ╚═════╝ ╚══════╝╚═╝     ╚═╝  ╚═══╝  ╚═══╝  ╚══════╝╚══════╝╚═╝  ╚═╝                            \n"
         "                                                                                                \n"
         "███████╗ ██████╗ ██████╗     ██████╗ ██╗  ██╗██████╗ ███████╗ ██████╗  ██████╗  ██████╗ ██████╗ \n"
         "██╔════╝██╔═══██╗██╔══██╗    ██╔══██╗██║ ██╔╝╚════██╗██╔════╝██╔═████╗██╔════╝ ██╔════╝ ╚════██╗\n"
         "█████╗  ██║   ██║██████╔╝    ██████╔╝█████╔╝  █████╔╝███████╗██║██╔██║███████╗ ██║  ███╗ █████╔╝\n"
         "██╔══╝  ██║   ██║██╔══██╗    ██╔══██╗██╔═██╗  ╚═══██╗╚════██║████╔╝██║██╔═══██╗██║   ██║██╔═══╝ \n"
         "██║     ╚██████╔╝██║  ██║    ██║  ██║██║  ██╗██████╔╝███████║╚██████╔╝╚██████╔╝╚██████╔╝███████╗\n"
         "╚═╝      ╚═════╝ ╚═╝  ╚═╝    ╚═╝  ╚═╝╚═╝  ╚═╝╚═════╝ ╚══════╝ ╚═════╝  ╚═════╝  ╚═════╝ ╚══════╝\n");

  _info("=== HD-RK3506-EVM bringup ===\n");

#ifdef CONFIG_FS_PROCFS
  /* Mount the procfs file system */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      _err("ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
  else
    {
      _info("procfs mounted at /proc\n");
    }
#endif

#ifdef CONFIG_RK3506_I2C
  /* Initialize I2C controllers (must be before any I2C client drivers) */

  BRINGUP_LOG_ERR(hd_rk3506_i2c_init(),
                  "hd_rk3506_i2c_init() failed");
#endif

#ifdef CONFIG_IOEXPANDER
  /* Register the board's exported GPIO pins as /dev/gpioN nodes on top
   * of the per-bank ioexpander devices (rk3506_gpio.c).  Pads owned by
   * on-board drivers stay private; only the table in hd_rk3506_gpio.c
   * is exported.
   */

  BRINGUP_LOG_ERR(hd_rk3506_gpio_initialize(),
                  "hd_rk3506_gpio_initialize() failed");
#endif

#ifdef CONFIG_FS_DEVFS
  /* Mount devfs at /dev so character devices (e.g. /dev/fb0) and the
   * I2C/GPIO/etc. drivers become accessible by path. (Currently a no-op:
   * NuttX has no CONFIG_FS_DEVFS; paths are created by register_driver
   * itself via inode_reserve_path.)
   */
#endif

#ifdef CONFIG_FS_TMPFS
  /* Mount the in-memory tmpfs at /tmp for scratch storage.  U 盘 host
   * (CONFIG_USBHOST_MSC) is already wired via usbhost_drivers_initialize,
   * so when a USB mass-storage device is plugged in, it will appear at
   * /dev/sda and can be mounted with `mount -t vfat /dev/sda /mnt/usb`
   * (NOT /tmp/usb: mount() cannot create a mountpoint under the /tmp
   * tmpfs mountpoint - fs_mount() rejects it with -ENOTDIR; /mnt/usb
   * lives in the pseudo root and is auto-created by mount()).
   */

  ret = nx_mount(NULL, "/tmp", "tmpfs", 0, NULL);
  if (ret < 0)
    {
      _err("ERROR: Failed to mount tmpfs at /tmp: %d\n", ret);
    }
  else
    {
      _info("tmpfs mounted at /tmp\n");
    }
#endif

#ifdef CONFIG_LCD_NV3007
  /* Register the NV3007 SPI panel as /dev/fb0.  fb_register() drives the
   * lcd_framebuffer bridge: up_fbinitialize() runs board_lcd_initialize()
   * (SPI0 + RM_IO pads + vendor init table + PWM backlight), allocates the
   * RAM framebuffer and creates /dev/fb0.
   */

  ret = fb_register(0, 0);
  if (ret < 0)
    {
      _err("ERROR: fb_register(NV3007) failed: %d\n", ret);
    }
  else
    {
      _info("NV3007 /dev/fb0 registered\n");
    }
#endif

#ifdef CONFIG_GRAPHICS_LVGL
  /* Initialise the LVGL display port: mmap /dev/fb0, register the LVGL
   * display driver, and start the LVGL timer/tick threads. Must run
   * after fb_register().
   */

  BRINGUP_LOG_ERR(hd_rk3506_lv_port_disp_init(),
                  "hd_rk3506_lv_port_disp_init() failed");
#endif

#ifdef CONFIG_RK3506_USBHOST
  /* Initialize the USB Host controller on OTG1 (0xFF780000, IRQ 79).
   *
   * The USB-A host connector on this board is wired to OTG1; OTG0
   * (0xFF740000) is the Type-C firmware-flash / device port:
   *   - vanxoak-hd-rk3506-evm-v1.dtsi: usb20_otg0 dr_mode = "peripheral",
   *     usb20_otg1 dr_mode = "host"
   *   - baseboard manual: 1x USB2.0 Type-A host + 1x Type-C flash port
   *   - HD-RK3506-EVM "USB接口电路设计": USB20_OTG0_DP/DN 是固件烧写口
   *
   * Driving OTG0 as a host enumerates whatever is on the flash cable
   * (usually the flashing PC) and produced phantom connects + a garbage
   * device descriptor.
   */

  do
    {
      struct usbhost_connection_s *conn;
      conn = rk3506_usbhost_initialize(1);
      if (conn == NULL)
        {
          _err("ERROR: rk3506_usbhost_initialize(1) failed\n");
        }
      else
        {
          /* Start the USB host waiter thread: without it nobody drains
           * the connection/waitqueue events, so plugging a U盘 never
           * instantiates the MSC class and /dev/sda never appears.
           * (The MSC class itself registers via
           * usbhost_drivers_initialize() during drivers_initialize().)
           */

          int waiter = usbhost_waiter_initialize(conn);
          if (waiter < 0)
            {
              _err("ERROR: usbhost_waiter_initialize "
                              "failed: %d\n", waiter);
            }
          else
            {
              _info("USB Host (OTG1, USB-A) initialized, "
                               "waiter started\n");
            }
        }
    }
  while (0);
#endif

#ifdef CONFIG_RK3506_FSPI
  /* The on-board GD5F SPI NAND is on the dedicated FSPI controller
   * (0xFF488000, IRQ85, CS0), NOT on the SPI1 rk_spi controller -- a
   * previous SPI1-based probe path lived here behind #if 0 and has been
   * removed (the FSPI controller driver below is the live path).  Bring
   * up clock + pinmux, reset/init the controller and run a single-bit
   * READ_ID to validate the register sequence.  This never blocks the
   * boot - every poll in the FSPI driver has a bounded timeout.
   */

  do
    {
      FAR struct mtd_dev_s *mtd;

      rk3506_fspi_clock_init();
      rk3506_ioc_fspi_setup();

      /* Probe the chip and register the whole-chip MTD (read/write/
       * erase with on-die ECC, lazy BBM scan).  Logs the READ_ID.
       */

      mtd = rk3506_spinand_fspi_initialize();
      if (mtd == NULL)
        {
          _err("ERROR: SPI NAND init failed (FSPI CS0)\n");
          break;
        }

      /* Hand the "userdata" region of the Rockchip partition layout
       * (nand_firmware/parameter.txt v8, A/B, with amp partition) to
       * dhara/littlefs.  The boot-chain partitions
       * (vnvm/uboot/misc/boot_a/boot_b/amp) are only touched through the
       * OTA manager / U-Boot below.
       *
       * userdata starts at sector 0x11000 (512-byte units) =
       * 35,651,584 B = erase block 272 on the XCSP2AAPK (1728 blocks,
       * ~182 MiB remain).
       *
       * dhara provides the wear-leveling / bad-block / logical-sector
       * mapping between littlefs (mounted -o autoformat by rcS on this
       * plain block device; SmartFS can not be used here - it needs the
       * SMART BIOC_* ioctls of drivers/mtd/smart.c, which dhara does
       * not provide) and the raw erase blocks, and registers
       * /dev/mtdblock0.
       *
       * UNITS: mtd_partition() takes firstblock in geo.blocksize units
       * (2 KiB pages here).  The doc comment in mtd.h claims "bytes",
       * but part_bread/part_bwrite/part_read add firstblock in block
       * units, so an erase-block index would land the partition 64x
       * too early (this was a latent bug: /data used to map 384 KiB at
       * 3.7 MB - inside the uboot partition - and was only masked
       * because every reflash rewrote the uboot partition).
       */

      {
        struct mtd_geometry_s geo;
        off_t first_page;
        off_t npages;
        FAR struct mtd_dev_s *part;

        if (mtd->ioctl(mtd, MTDIOC_GEOMETRY,
                       (unsigned long)((uintptr_t)&geo)) < 0)
          {
            _err("ERROR: SPI NAND geometry ioctl failed\n");
            break;
          }

        first_page = SPINAND_USERDATA_START_BYTES / (off_t)geo.blocksize;
        npages = (off_t)geo.neraseblocks * geo.erasesize / geo.blocksize -
                 first_page;

        if (npages <= 0)
          {
            _err("ERROR: userdata region empty (chip "
                            "smaller than the parameter layout?)\n");
            break;
          }

        part = mtd_partition(mtd, first_page, npages);
        if (part == NULL)
          {
            _err("ERROR: mtd_partition(userdata) failed\n");
            break;
          }

        if (dhara_initialize(0, part) < 0)
          {
            _err("ERROR: dhara_initialize(/dev/mtdblock0) "
                            "failed\n");
            break;
          }

        _info("SPI NAND /data: /dev/mtdblock0 = userdata "
                         "(page %ld +%ld of %ld)\n",
               (long)first_page, (long)npages,
               (long)geo.neraseblocks * geo.erasesize / geo.blocksize);

#ifdef CONFIG_RK3506_OTA
        /* A/B OTA: expose misc/boot_a/boot_b and register /dev/ota
         * (AvbABData slot protocol consumed by the prebuilt U-Boot's
         * CONFIG_ANDROID_AB boot_fit path). */

        do
          {
            int ota_ret = rk3506_ota_initialize(mtd);
            if (ota_ret < 0)
              {
                _err("ERROR: rk3506_ota_initialize failed: "
                                "%d\n", ota_ret);
              }
          }
        while (0);
#endif
      }
    }
  while (0);
#endif

#ifdef CONFIG_RK3506_PWM
  /* M2b.6: /dev/pwm0 on the PWM0 channel-2 block (0xFF932000),
   * output pin RM_IO12 (GPIO0_B4 = J9-29, RM_IO function 47 = PWM0_CH2).
   * The SDK iot dts uses this same channel for the LCD backlight,
   * period 25000 ns (40 kHz) - a good first multimeter target:
   *   pwm -f 40000 -d 50
   */

  do
    {
      int pwm_ret = rk3506_pwm_initialize();
      if (pwm_ret < 0)
        {
          _err("ERROR: rk3506_pwm_initialize failed: %d\n",
                 pwm_ret);
        }
    }
  while (0);
#endif

#ifdef CONFIG_RK3506_SARADC
  /* M2b.7: /dev/adc0 on the SARADC (0xFF4E8000).  Channels 2/3 =
   * SARADC_IN2/SARADC_IN3 on the 40-pin header (1.8 V domain):
   *   adc -n 1   (triggers a scan via ANIOC_TRIGGER, SWTRIG mode)
   */

  do
    {
      int adc_ret = rk3506_saradc_initialize();
      if (adc_ret < 0)
        {
          _err("ERROR: rk3506_saradc_initialize failed: "
                          "%d\n", adc_ret);
        }
    }
  while (0);
#endif

#ifdef CONFIG_RK3506_CAN
  /* M2b.8: SocketCAN devices can0/can1 (0xFF320000/0xFF330000).
   * Default build uses loopback mode (single-node self-test):
   *   cansend can0 123#DEADBEEF
   *   candump can0 -n 1   (must echo the frame)
   * Pin routing: CAN0 -> RM_IO11, CAN1 -> RM_IO12 (kept clear of
   * RM_IO12 (J9-29) = PWM0_CH2, NV3007 backlight).
   */

  do
    {
      int can_ret = rk3506_can_initialize();
      if (can_ret < 0)
        {
          _err("ERROR: rk3506_can_initialize failed: %d\n",
                 can_ret);
        }
    }
  while (0);
#endif

#ifdef CONFIG_RK3506_WDT
  /* M2b.9: DesignWare watchdogs /dev/watchdog0 (0xFF260000) and
   * /dev/watchdog1 (0xFF268000).  Acceptance:
   *   nsh> wtdog -o 8000 -p 2000 /dev/watchdog0   (auto-ping: no reset)
   *   nsh> wtdog -o 3000 /dev/watchdog0           (no ping: reset ~3s)
   */

  do
    {
      int wdt_ret = rk3506_wdt_initialize();
      if (wdt_ret < 0)
        {
          _err("ERROR: rk3506_wdt_initialize failed: %d\n",
                 wdt_ret);
        }
    }
  while (0);
#endif

#ifdef CONFIG_RK3506_GMAC0
  /* M2b.3: register the GMAC0 netdev ("eth0").  All hardware
   * sequences are the Linux SDK GMAC HAL ported verbatim
   * (rk3506_gmac_hal.c); clocks / IOMUX / PHY reset run in the
   * netdev's ifup callback.  NETINIT + DHCPC (both enabled in the
   * nsh defconfig) bring the interface up and get a lease when
   * a cable is connected.
   */

  int gmac_ret = rk3506_gmac0_initialize();
  if (gmac_ret < 0)
    {
      _warn("GMAC0: netdev register failed: %d\n",
             gmac_ret);
    }
#endif

#ifdef CONFIG_RK3506_RPTUN
  /* Master side of the bus-M0 rpmsg link: boots the embedded M0
   * firmware and registers /dev/rpmsg/mcu.  The rptun core autostarts
   * the link from its own worker thread.
   */

  do
    {
      int rptun_ret = rk3506_rptun_initialize();
      if (rptun_ret < 0)
        {
          _err("ERROR: rk3506_rptun_initialize failed: %d\n",
                 rptun_ret);
        }
    }
  while (0);
#endif

  _info("=== HD-RK3506-EVM bringup done ===\n");
  return OK;
}
