/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_register.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared 32-bit MMIO accessors for the RK3506 vendor drivers.
 *
 * nuttx/arch/arm/src/common/arm_internal.h already provides
 * getreg32()/putreg32(), but that header lives under arch/.../src and
 * pulls in a large set of arm-internal declarations; several vendor
 * drivers instead carried an identical private fallback copy.  This
 * header centralizes that fallback so each driver has exactly one
 * shared definition.  The #ifndef guards keep it harmless if the arch
 * header was included first (e.g. rk3506_lowputc.c uses
 * arm_internal.h directly).
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_REGISTER_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_REGISTER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif

#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif

#ifndef putreg16
#  define putreg16(v, a) (*(FAR volatile uint16_t *)(a) = (v))
#endif

#ifndef getreg16
#  define getreg16(a)    (*(FAR volatile uint16_t *)(a))
#endif

#ifndef putreg8
#  define putreg8(v, a)  (*(FAR volatile uint8_t *)(a) = (v))
#endif

#ifndef getreg8
#  define getreg8(a)     (*(FAR volatile uint8_t *)(a))
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_REGISTER_H */
