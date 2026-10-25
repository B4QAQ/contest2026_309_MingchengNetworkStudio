# ##############################################################################
# vendor/rockchip/chips/rk3506/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

set(SRCS
    rk3506_start.c
    rk3506_irq.c
    rk3506_lowputc.c
    rk3506_serial.c
    rk3506_timerisr.c
    rk3506_allocateheap.c)

# Infrastructure, always built: pin control, GPIO back-end and the
# clock/reset unit are used by essentially every peripheral (and cru
# was previously gated behind an incomplete peripheral list that
# missed GMAC0).  The ioexpander lower-half inside rk3506_gpio.c is
# compiled out unless CONFIG_IOEXPANDER, but the chip-level
# rk3506_gpio_config/write/read helpers are always available so that
# on-board drivers (LCD, GMAC, ...) share one GPIO back-end.

list(APPEND SRCS rk3506_pinctrl.c)
list(APPEND SRCS rk3506_cru.c)

# GPIO driver (drv/gpio/)

if(CONFIG_RK3506_GPIO)
  list(APPEND SRCS drv/gpio/rk3506_gpio.c)
endif()

# SPI driver (drv/spi/)

if(CONFIG_RK3506_SPI)
  list(APPEND SRCS drv/spi/rk3506_spi.c)
endif()

# PWM driver (drv/pwm/)

if(CONFIG_RK3506_PWM)
  list(APPEND SRCS drv/pwm/rk3506_pwm.c)
endif()

if(CONFIG_RK3506_I2C)
  list(APPEND SRCS rk3506_i2c.c)
endif()

if(CONFIG_RK3506_USBHOST)
  list(APPEND SRCS rk3506_usbhost.c)
endif()

if(CONFIG_RK3506_SPINAND OR CONFIG_RK3506_FSPI OR CONFIG_RK3506_PWM OR CONFIG_RK3506_SARADC OR CONFIG_RK3506_CAN OR CONFIG_RK3506_WDT OR CONFIG_RK3506_GMAC0)
  list(APPEND SRCS rk3506_iomux.c)
endif()

if(CONFIG_RK3506_SPINAND)
  list(APPEND SRCS rk3506_spinand.c)
endif()

if(CONFIG_RK3506_FSPI)
  list(APPEND SRCS rk3506_fspi.c)
endif()

if(CONFIG_RK3506_SPINAND_FSPI)
  list(APPEND SRCS rk3506_spinand_fspi.c)
endif()

if(CONFIG_RK3506_SARADC)
  list(APPEND SRCS rk3506_saradc.c)
endif()

if(CONFIG_RK3506_CAN)
  list(APPEND SRCS rk3506_can.c)
endif()

if(CONFIG_RK3506_WDT)
  list(APPEND SRCS rk3506_wdt.c)
endif()

if(CONFIG_RK3506_OTA)
  list(APPEND SRCS rk3506_ota.c)
endif()

if(CONFIG_RK3506_GMAC0)
  list(APPEND SRCS rk3506_gmac0.c)
  list(APPEND SRCS rk3506_gmac_hal.c)
endif()

if(CONFIG_RK3506_RPTUN)
  list(APPEND SRCS rk3506_rptun.c)
endif()

if(CONFIG_OPENAMP AND NOT CONFIG_MM_PGALLOC)
  list(APPEND SRCS rk3506_addrenv.c)
endif()

target_sources(arch PRIVATE ${SRCS})

target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/include
                                        ${NUTTX_CHIP_ABS_DIR}/hardware
                                        ${NUTTX_CHIP_ABS_DIR}/drv/gpio
                                        ${NUTTX_CHIP_ABS_DIR}/drv/spi
                                        ${NUTTX_CHIP_ABS_DIR}/drv/pwm)
