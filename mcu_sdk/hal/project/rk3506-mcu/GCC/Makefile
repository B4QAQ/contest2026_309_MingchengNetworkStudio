# SPDX-License-Identifier: BSD-3-Clause */

# Copyright (c) 2022 Rockchip Electronics Co., Ltd.

PROJECT		:= RK3506
SOC		:= RK3506
CPU		:= -mcpu=cortex-m0

# BUS M0
CFLAGS := -Os
LDFLAGS := --specs=nosys.specs
# DRAM 方案：指向另存的 gcc_bus_m0_ddr.ld（ORIGIN=0x03a00000），
# 原 gcc_bus_m0.ld（SRAM，ORIGIN=0x0）保持不动。
LINKER_SCRIPT := gcc_bus_m0_ddr.ld

-include ../../../middleware/benchmark/benchmark.mk
-include ../../../middleware/rpmsg-lite/rpmsg-lite.mk
-include ../../../middleware/simple_console/simple_console.mk
include ../../common/GCC/Cortex-M.mk
