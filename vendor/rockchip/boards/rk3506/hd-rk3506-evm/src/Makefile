############################################################################
# vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(TOPDIR)/Make.defs

CSRCS = hd_rk3506_boardinit.c
CSRCS += hd_rk3506_appinit.c
CSRCS += hd_rk3506_bringup.c
CSRCS += hd_rk3506_boot.c
CSRCS += hd_rk3506_leds.c

# Temporary bringup diagnostic dump (RM_IO/clock/iomux/USB-PHY readback)

CSRCS += hd_rk3506_diag.c

ifeq ($(CONFIG_LCD_NV3007),y)
  CSRCS += hd_rk3506_nv3007.c
endif

ifeq ($(CONFIG_GRAPHICS_LVGL),y)
  CSRCS += hd_rk3506_lv_port_disp.c
endif

ifeq ($(CONFIG_IOEXPANDER),y)
  CSRCS += hd_rk3506_gpio.c
endif

ifeq ($(CONFIG_RK3506_MMC),y)
  CSRCS += hd_rk3506_mmc.c
endif

ifeq ($(CONFIG_ETC_ROMFS),y)
  RCSRCS = etc/init.d/rc.sysinit etc/init.d/rcS
  RCRAWS = etc/group etc/passwd etc/net.conf etc/ssl/curl/ca-certificates.crt
endif

include $(TOPDIR)/boards/Board.mk
