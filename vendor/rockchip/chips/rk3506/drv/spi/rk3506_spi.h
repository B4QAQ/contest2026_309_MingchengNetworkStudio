/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_spi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SPI_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SPI_H

#include <nuttx/config.h>
#include <nuttx/spi/spi.h>

/* The RK3506 has exactly two SPI controllers (SPI0 @0xFF120000,
 * SPI1 @0xFF130000, per the SDK CMSIS rk3506.h -- there is no SPI2),
 * matching RK3506_SPI_NUM_BUS == 2 in rk3506_spi.c.
 */

#define RK3506_SPI0_BUS           0
#define RK3506_SPI1_BUS           1

#if defined(__cplusplus)
extern "C" {
#endif

FAR struct spi_dev_s *rk3506_spibus_initialize(int bus);
void                  rk3506_spibus_uninitialize(int bus);

#if defined(__cplusplus)
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SPI_H */
