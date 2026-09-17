/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_spinand.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Glue between the RK3506 SPI controller and the NuttX GD5F SPI NAND
 * upper-half driver.  This wires up the on-board SPI NAND (GigaDevice
 * GD5F1GQ4RE or compatible, 1 Gbit / 128 MByte, standard 1-bit SPI,
 * via SPI1) so it can be mounted as a SmartFS partition.
 *
 * Bring-up order (matches the SDK/Linux flow):
 *   1. rk3506_spi1_clock_init()  - ungate PCLK_SPI1 / CLK_SPI1 in the
 *                                   CRU (the controller needs a live
 *                                   APB clock before any register
 *                                   access).
 *   2. rk3506_ioc_spi1_setup()   - mux GPIO0 PB0/PB1/PB2/PB6 to SPI1
 *                                   alt function 2 (clk/mosi/miso/cs0)
 *                                   through the ioc_pmu mux registers.
 *   3. rk3506_spibus_initialize() - register the DesignWare SSI host
 *                                   and hand back a struct spi_dev_s.
 *   4. gd5f_initialize()         - probe the GD5F part; returns an MTD
 *                                   device on success, NULL otherwise.
 *
 * The board's FSPI host (0xFF488000, IRQ 85) is the high-performance
 * quad-mode path used by the SDK; the standard SPI controller is the
 * fallback 1-bit path that exercises the gd5f.c upper half.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>

#include <nuttx/mtd/mtd.h>
#include <nuttx/spi/spi.h>

#include "rk3506_spi.h"
#include "rk3506_cru.h"
#include "rk3506_iomux.h"

/* gd5f_initialize is defined in drivers/mtd/gd5f.c; we forward-declare
 * it here so we don't need to ship a public header in the chip tree.
 */

extern FAR struct mtd_dev_s *gd5f_initialize(FAR struct spi_dev_s *dev,
                                              uint32_t spi_devid);

#define RK3506_SPINAND_SPI_BUS    RK3506_SPI1_BUS
#define RK3506_SPINAND_DEVID      0

FAR struct mtd_dev_s *rk3506_spinand_initialize(void)
{
  FAR struct spi_dev_s *spi;
  FAR struct mtd_dev_s *mtd;

  /* 1. Ungate the SPI1 clocks before touching the controller. */

  rk3506_spi1_clock_init();

  /* 2. Route the SPI1 function onto the GPIO0 pads. */

  rk3506_ioc_spi1_setup();

  /* 3. Initialise the DesignWare SSI host controller. */

  spi = rk3506_spibus_initialize(RK3506_SPINAND_SPI_BUS);
  if (spi == NULL)
    {
      _err("ERROR: rk3506_spibus_initialize(%d) failed\n",
           RK3506_SPINAND_SPI_BUS);
      return NULL;
    }

  /* 4. Probe the GD5F SPI NAND and wrap it as an MTD device. */

  mtd = gd5f_initialize(spi, RK3506_SPINAND_DEVID);
  if (mtd == NULL)
    {
      _err("ERROR: gd5f_initialize failed (no GD5F device on SPI%d?)\n",
           RK3506_SPINAND_SPI_BUS);
      return NULL;
    }

  _info("SPI NAND (GD5F on SPI%d) ready\n", RK3506_SPINAND_SPI_BUS);
  return mtd;
}
