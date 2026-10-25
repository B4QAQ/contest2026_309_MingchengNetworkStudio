/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_pwm.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_PWM_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_PWM_H

#include <nuttx/config.h>

#include <nuttx/timers/pwm.h>

#if defined(__cplusplus)
extern "C"
{
#endif

/* Route RM_IO10 (GPIO0_B2) to PWM0 channel 2, enable its clocks and
 * register /dev/pwm0.  Idempotent: safe to call from both bringup and
 * an in-kernel backlight consumer.
 */

int rk3506_pwm_initialize(void);

/* Return the PWM0 ch2 lower half (valid after rk3506_pwm_initialize). */

FAR struct pwm_lowerhalf_s *rk3506_pwm_getlower(void);

#if defined(__cplusplus)
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_PWM_H */
