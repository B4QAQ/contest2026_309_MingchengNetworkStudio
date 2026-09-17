# ##############################################################################
# vendor/rockchip/boards/rk3506/hd-rk3506-evm/CMakeLists.txt
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
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

add_subdirectory(src)

file(GLOB EXTRA_LIBS
     ${NUTTX_BOARD_ABS_DIR}/libs/${CONFIG_ARCH_BOARD_CUSTOM_NAME}/*.a)

set_property(GLOBAL APPEND PROPERTY NUTTX_EXTRA_LIBRARIES ${EXTRA_LIBS})

set_property(GLOBAL PROPERTY LD_SCRIPT ${NUTTX_BOARD_ABS_DIR}/scripts/ld.script)

#-----------------------------------------------------------------------------
# Post-build: Generate compact firmware image (vela.bin) and full update.img
#
# Two issues fixed in the post-build flow:
#  1. `objcopy -O binary nuttx nuttx.bin` produces a 34 MB file with 33 MB of
#     zero-padding because `.note.gnu.build-id` lives at VMA=0 while `.text`
#     lives at VMA=0x02080000. We use `--only-section` to drop the build-id
#     and other non-loadable sections, yielding ~500 KB vela.bin.
#  2. `pack_nand.sh` was previously hard-coded to a fixed SDK path. The
#     post-build now delegates to a fully env-var-driven pack script.
#-----------------------------------------------------------------------------

# NUTTX_BOARD_ABS_DIR = .../openvela/vendor/rockchip/boards/rk3506/hd-rk3506-evm
# Path components: hd-rk3506-evm -> rk3506 -> boards -> rockchip -> vendor -> openvela
# So 5 levels up = openvela
get_filename_component(OPENVELA_DIR
  "${NUTTX_BOARD_ABS_DIR}/../../../../.." ABSOLUTE)

# Find arm-none-eabi tools. NOTE: get_filename_component gives the absolute
# path, but we add PATHS explicitly to avoid PATH lookup ambiguity.
set(TOOLCHAIN_BIN_DIR "${OPENVELA_DIR}/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin")

# Hard-set tools (faster and more reliable than find_program):
#   find_program was previously returning NOTFOUND and falling back to
#   CMAKE_OBJCOPY/CMAKE_STRIP which were sometimes empty or wrong.
set(ARM_OBJCOPY "${TOOLCHAIN_BIN_DIR}/arm-none-eabi-objcopy")
set(ARM_STRIP   "${TOOLCHAIN_BIN_DIR}/arm-none-eabi-strip")

# Output directories
set(NAND_FIRMWARE_DIR "${OPENVELA_DIR}/nand_firmware")
set(PACK_SCRIPT       "${NAND_FIRMWARE_DIR}/pack_nand.sh")

message(STATUS "openvela: NAND_FIRMWARE_DIR=${NAND_FIRMWARE_DIR}")
message(STATUS "openvela: PACK_SCRIPT=${PACK_SCRIPT}")
message(STATUS "openvela: ARM_OBJCOPY=${ARM_OBJCOPY}")
message(STATUS "openvela: ARM_STRIP=${ARM_STRIP}")

# Define the nuttx_post_build custom target
# Use a wrapper script to avoid shell quoting issues with parens in echo strings
set(VELA_BUILD_SCRIPT ${CMAKE_BINARY_DIR}/vela_build.sh)

# Build the script content using a CMake list (avoids bash/CMake quoting issues)
set(VELA_BUILD_CMDS
  "#!/bin/sh"
  "set -e"
  "cd \"${CMAKE_BINARY_DIR}\""
  "echo '==> Stripping debug symbols...'"
  "\"${ARM_STRIP}\" --remove-section=.note.gnu.build-id --remove-section=.debug_abbrev --remove-section=.debug_info --remove-section=.debug_line --remove-section=.debug_aranges --remove-section=.debug_line_str --remove-section=.debug_str --remove-section=.debug_frame --remove-section=.debug_loclists --remove-section=.debug_rnglists --remove-section=.debug_macro --remove-section=.debug_loc --remove-section=.debug_ranges --remove-section=.debug_addr nuttx"
  "echo '==> Generating vela.bin (compact, no zero padding)...'"
  "\"${ARM_OBJCOPY}\" -O binary --only-section=.text --only-section=.ARM.exidx --only-section=.ARM.extab --only-section=.init_section --only-section=.data nuttx vela.bin"
  "cp -f vela.bin \"${NAND_FIRMWARE_DIR}/nuttx.bin\""
  "cp -f nuttx \"${NAND_FIRMWARE_DIR}/nuttx.elf\""
  "echo 'Firmware: '\"\$(ls -lh vela.bin | awk '{print \$5}')\"' done'"
  "exit 0"
)

# Write each line separately to avoid list/quote issues
file(WRITE  ${VELA_BUILD_SCRIPT} "")
foreach(line ${VELA_BUILD_CMDS})
  file(APPEND ${VELA_BUILD_SCRIPT} "${line}\n")
endforeach()

add_custom_target(nuttx_post_build ALL
  DEPENDS nuttx
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMAND sh ${VELA_BUILD_SCRIPT}
  COMMENT "Building compact vela.bin from nuttx ELF"
)

# Optional: package the firmware into update.img if pack_nand.sh is available
if(EXISTS ${PACK_SCRIPT})
  add_custom_command(
    TARGET nuttx_post_build
    WORKING_DIRECTORY ${NAND_FIRMWARE_DIR}
    COMMAND bash ${PACK_SCRIPT}
    COMMENT "Packaging update.img for NAND flashing"
  )
endif()
