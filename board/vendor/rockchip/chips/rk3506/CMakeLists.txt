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

if(CONFIG_RK3506_VOP)
  list(APPEND SRCS rk3506_vop.c)
endif()

if(CONFIG_RK3506_I2C)
  list(APPEND SRCS rk3506_i2c.c)
endif()

if(CONFIG_RK3506_USBHOST)
  list(APPEND SRCS rk3506_usbhost.c)
endif()

if(CONFIG_RK3506_SPI)
  list(APPEND SRCS rk3506_spi.c)
endif()

if(CONFIG_RK3506_SPINAND OR CONFIG_RK3506_FSPI OR CONFIG_RK3506_PWM OR CONFIG_RK3506_SARADC OR CONFIG_RK3506_CAN OR CONFIG_RK3506_WDT)
  list(APPEND SRCS rk3506_cru.c)
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

if(CONFIG_RK3506_PWM)
  list(APPEND SRCS rk3506_pwm.c)
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

if(CONFIG_RK3506_RPTUN)
  list(APPEND SRCS rk3506_rptun.c)
endif()

if(CONFIG_RK3506_OTA)
  list(APPEND SRCS rk3506_ota.c)
endif()

# Identity va<->pa helpers required by the libmetal NuttX io shim
# (nuttx/openamp/libmetal/lib/system/nuttx/io.c) when the page allocator
# is not used; see rk3506_addrenv.c for the full rationale.

if(CONFIG_OPENAMP AND NOT CONFIG_MM_PGALLOC)
  list(APPEND SRCS rk3506_addrenv.c)
endif()

if(CONFIG_RK3506_GMAC0)
  list(APPEND SRCS rk3506_gmac0.c)
  list(APPEND SRCS rk3506_gmac_hal.c)
endif()

target_sources(arch PRIVATE ${SRCS})

target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/include)
