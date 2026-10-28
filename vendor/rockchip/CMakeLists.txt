############################################################################
# vendor/rockchip/CMakeLists.txt
#
# Top-level CMakeLists for Rockchip vendor.
# Registers chips and boards subdirectories.
############################################################################

# Chips (rk3506, etc.)

if(CONFIG_ARCH_CHIP_RK3506)
  add_subdirectory(chips/rk3506)
endif()

# Boards (hd-rk3506-evm, etc.)

if(CONFIG_ARCH_BOARD_HD_RK3506_EVM)
  add_subdirectory(boards/rk3506/hd-rk3506-evm)
endif()
