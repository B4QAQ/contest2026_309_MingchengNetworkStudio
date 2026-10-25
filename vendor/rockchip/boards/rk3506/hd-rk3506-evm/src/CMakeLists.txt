# ##############################################################################
# vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/CMakeLists.txt
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
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
# ##############################################################################

set(SRCS hd_rk3506_boardinit.c hd_rk3506_appinit.c hd_rk3506_bringup.c)

# Board boot (early GPIO0 clock init) and the userled/heartbeat driver

list(APPEND SRCS hd_rk3506_boot.c)
list(APPEND SRCS hd_rk3506_leds.c)

if(CONFIG_LCD_NV3007)
  list(APPEND SRCS hd_rk3506_nv3007.c)
endif()

if(CONFIG_GRAPHICS_LVGL)
  list(APPEND SRCS hd_rk3506_lv_port_disp.c)
  if(CONFIG_HD_RK3506_LV_BUTTONS)
    list(APPEND SRCS hd_rk3506_lv_port_indev.c)
  endif()
endif()

if(CONFIG_RK3506_MMC)
  list(APPEND SRCS hd_rk3506_mmc.c)
endif()

if(CONFIG_IOEXPANDER)
  list(APPEND SRCS hd_rk3506_gpio.c)
endif()

# netcfg: network configuration daemon.  Parses /etc/net.conf and
# (with auto_dhcp=1) re-runs DHCP on GMAC0 link-up via the driver's
# SIOCMIINOTIFY signal.  Registered as an NSH builtin so rcS can
# start it in the background.  NOTE: the source must NOT also go into
# the board SRCS list - nuttx_add_application archives it into
# libapps.a and compiles it with main=netcfg_main; adding it to the
# board library would duplicate the object.  Board-dir targets are
# outside the apps/ CMake scope, so apps/include must be passed
# explicitly (for netutils/netlib.h).
if(CONFIG_HD_RK3506_NETCFG)
  nuttx_add_application(
    NAME
    netcfg
    STACKSIZE
    3072
    PRIORITY
    90
    INCLUDE_DIRECTORIES
    ${NUTTX_APPS_DIR}/include
    SRCS
    hd_rk3506_netcfg.c)
endif()

# ota: NSH builtin for the A/B OTA slot manager (/dev/ota).  Same
# builtin-registration rules as netcfg above.  The chip driver header
# (rk3506_ota.h) lives in the chip include dir - add it explicitly since
# board-dir targets are outside the chip CMake scope.
if(CONFIG_HD_RK3506_OTACMD)
  nuttx_add_application(
    NAME
    ota
    STACKSIZE
    8192
    PRIORITY
    100
    INCLUDE_DIRECTORIES
    ${NUTTX_BOARD_ABS_DIR}/../../../chips/rk3506
    ${NUTTX_APPS_DIR}/include
    SRCS
    hd_rk3506_ota.c)
endif()

# rpmsgtest: NSH builtin exercising the bus-M0 rpmsg link.  Uses only
# the standard rpmsg char ioctls from nuttx/rpmsg/rpmsg.h.
if(CONFIG_HD_RK3506_RPMSGTEST)
  nuttx_add_application(
    NAME
    rpmsgtest
    STACKSIZE
    3072
    PRIORITY
    100
    INCLUDE_DIRECTORIES
    ${NUTTX_APPS_DIR}/include
    SRCS
    hd_rk3506_rpmsgtest.c)
endif()

# Both init scripts are shipped as RAW NSH scripts (RCRAWS).  They use
# only '#' comment lines and plain NSH commands - no C preprocessor
# directives (#include/#ifdef) and no C-style '/* ... */' comments.  They
# are listed under RCRAWS (not RCSRCS) so they are copied verbatim into
# the romfs and never fed through cpp; this also sidesteps the fact that
# this board's direct nuttx_add_romfs() packs the raw source PATH into
# the image (the RCSRCS cpp output is not merged into that staging dir).
nuttx_add_romfs(
  NAME
  etc
  MOUNTPOINT
  etc
  RCRAWS
  etc/init.d/rc.sysinit
  etc/init.d/rcS
  etc/group
  etc/passwd
  # network config consumed by the netcfg builtin
  etc/net.conf
  # TLS trust store for libcurl (curl_config.h: CURL_CA_BUNDLE =
  # /etc/ssl/curl/ca-certificates.crt).  Shipped as a raw file so it is
  # copied verbatim into the romfs.
  etc/ssl/curl/ca-certificates.crt
  PATH
  ${CMAKE_CURRENT_LIST_DIR}/etc)

target_link_libraries(board PRIVATE romfs_etc)

target_sources(board PRIVATE ${SRCS})
