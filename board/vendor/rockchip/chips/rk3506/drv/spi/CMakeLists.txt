############################################################################
# vendor/rockchip/chips/rk3506/drv/spi/CMakeLists.txt
#
# SPI driver for Rockchip RK3506
############################################################################

if(CONFIG_RK3506_SPI)
  list(APPEND SRCS drv/spi/rk3506_spi.c)
  list(APPEND INCLUDE_DIRS drv/spi)
endif()
