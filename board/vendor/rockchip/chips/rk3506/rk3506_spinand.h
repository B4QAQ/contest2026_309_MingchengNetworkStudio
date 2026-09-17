/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_spinand.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SPINAND_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SPINAND_H

#include <nuttx/config.h>
#include <nuttx/mtd/mtd.h>

#if defined(__cplusplus)
extern "C" {
#endif

FAR struct mtd_dev_s *rk3506_spinand_initialize(void);

#if defined(__cplusplus)
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SPINAND_H */
