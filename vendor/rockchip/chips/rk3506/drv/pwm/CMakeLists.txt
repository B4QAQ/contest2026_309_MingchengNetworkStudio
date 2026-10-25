############################################################################
# vendor/rockchip/chips/rk3506/drv/pwm/CMakeLists.txt
#
# PWM driver for Rockchip RK3506
############################################################################

if(CONFIG_RK3506_PWM)
  list(APPEND SRCS drv/pwm/rk3506_pwm.c)
  list(APPEND INCLUDE_DIRS drv/pwm)
endif()
