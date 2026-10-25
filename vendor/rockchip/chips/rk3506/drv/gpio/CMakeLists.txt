############################################################################
# vendor/rockchip/chips/rk3506/drv/gpio/CMakeLists.txt
#
# GPIO driver for Rockchip RK3506
############################################################################

if(CONFIG_RK3506_GPIO)
  list(APPEND SRCS drv/gpio/rk3506_gpio.c)
  list(APPEND INCLUDE_DIRS drv/gpio)
endif()
