#!/bin/bash
# build_mcu_fw.sh - Build the RK3506 M0 (bus MCU) rpmsg-test firmware and
# package it as amp/amp.img (a standalone FIT that U-Boot loads and
# releases from the "amp" GPT partition).
#
# The M0 firmware source lives in <repo root>/mcu_sdk/hal (a vendored copy
# of the Linux SDK HAL tree).  It is a normal, editable source tree now:
# the DRAM linker script, UART1 debug console, TEST_DEMO and
# RPMSG_LINUX_TEST switches are all committed there, so this script only
# builds and packages - no sed patching of a read-only SDK copy.
#
# Validated reference: RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm
# (see AMP_RK3506_LINUX_SDK_NOTES.md).
#
# Usage:  bash build_mcu_fw.sh
set -euo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SELF_DIR/../.." && pwd)"

if [ -d "$ROOT_DIR/mcu_sdk/hal" ]; then
  SDK_DIR="$ROOT_DIR/mcu_sdk"
elif [ -n "${RK3506_SDK_DIR:-}" ]; then
  SDK_DIR="$RK3506_SDK_DIR"
else
  echo "error: <repo root>/mcu_sdk/hal not found" >&2
  exit 1
fi
SDK_DIR="$(realpath "$SDK_DIR")"

# Cross toolchain: prefer the one vendored next to the M0 tree, fall back
# to the openvela prebuilts.
if [ -d "$SDK_DIR/prebuilts/gcc/linux-x86/arm/gcc-arm-none-eabi-10-2020-q4-major-x86_64-linux/bin" ]; then
  export PATH="$SDK_DIR/prebuilts/gcc/linux-x86/arm/gcc-arm-none-eabi-10-2020-q4-major-x86_64-linux/bin:$PATH"
elif [ -d "$ROOT_DIR/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin" ]; then
  export PATH="$ROOT_DIR/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH"
fi
command -v arm-none-eabi-gcc >/dev/null || {
  echo "error: arm-none-eabi-gcc not in PATH" >&2; exit 1; }

PROJ="$SDK_DIR/hal/project/rk3506-mcu"

# Sanity-check the committed M0 configuration before building.  These are
# the four differences from the stock SDK project that the validated Linux
# AMP flow requires; catching them here gives a clear error instead of a
# silently broken firmware.
grep -q '^#define TEST_DEMO' "$PROJ/src/main.c" || {
  echo "error: TEST_DEMO is not enabled in mcu_sdk/.../src/main.c" >&2; exit 1; }
grep -q '^#define RPMSG_LINUX_TEST' "$PROJ/src/test_demo.c" || {
  echo "error: RPMSG_LINUX_TEST is not enabled in mcu_sdk/.../src/test_demo.c" >&2; exit 1; }
grep -q 'pUart = UART1;' "$PROJ/src/main.c" || {
  echo "error: M0 debug UART is not UART1 in mcu_sdk/.../src/main.c" >&2; exit 1; }
grep -q 'LINKER_SCRIPT := gcc_bus_m0_ddr.ld' "$PROJ/GCC/Makefile" || {
  echo "error: M0 linker script is not gcc_bus_m0_ddr.ld" >&2; exit 1; }
grep -q 'ORIGIN = 0x03a00000' "$PROJ/GCC/gcc_bus_m0_ddr.ld" || {
  echo "error: M0 DRAM origin is not 0x03a00000" >&2; exit 1; }

echo "== building M0 firmware (DRAM 0x03a00000, UART1 debug) =="
make -C "$PROJ/GCC" -j"$(nproc)"

BIN="$PROJ/GCC/TestDemo.bin"
[ -f "$BIN" ] || { echo "error: $BIN not produced" >&2; exit 1; }

# The M0 now runs from DRAM; only the 1 MiB reserved window bounds it.
BIN_SIZE=$(stat -c %s "$BIN")
if [ "$BIN_SIZE" -gt 1048576 ]; then
  echo "error: MCU firmware ($BIN_SIZE B) exceeds the 1 MiB DRAM window" >&2
  exit 1
fi

# The entry point must be inside the DRAM window, otherwise U-Boot copies
# the image somewhere it will never execute from.
ELF="$PROJ/GCC/TestDemo.elf"
if [ -f "$ELF" ]; then
  ENTRY=$(arm-none-eabi-readelf -h "$ELF" | sed -n 's/.*Entry point address:[[:space:]]*//p')
  case "$ENTRY" in
    0x03a*|0x3a*) : ;;
    *) echo "error: M0 entry point $ENTRY is outside 0x03a00000" >&2; exit 1 ;;
  esac
  echo "   entry point: $ENTRY"
fi

#---------------------------------------------------------------------------
# Package the M0 firmware FIT (amp.img).
#
# U-Boot (CONFIG_AMP / CONFIG_ROCKCHIP_AMP) reads this FIT from the "amp"
# GPT partition during board_late_init() -> amp_cpus_on(), copies the
# standalone image to load=0x03a00000 while the M0 is held in reset
# (boot_get_loadable), then calls fit_standalone_release().
#
# The .its is committed in mcu_sdk; it is copied here next to mcu.bin so
# mkimage's /incbin/("./mcu.bin") resolves.
#---------------------------------------------------------------------------
AMP_DIR="$SELF_DIR/amp"
mkdir -p "$AMP_DIR"
cp -f "$BIN" "$AMP_DIR/mcu.bin"
# The committed .its refers to ../GCC/TestDemo.bin relative to its home
# directory; rewrite that one line so mkimage finds the mcu.bin copy that
# sits next to the generated .its.
sed 's|/incbin/("[^"]*")|/incbin/("./mcu.bin")|' \
  "$PROJ/Image/amp.its" > "$AMP_DIR/amp.its"

MKIMG=""
for cand in "$SDK_DIR/hal/tools/mkimage" \
            "$ROOT_DIR/uboot_build/u-boot/tools/mkimage"; do
  [ -x "$cand" ] && MKIMG="$cand" && break
done
[ -n "$MKIMG" ] || {
  echo "error: mkimage not found (mcu_sdk/hal/tools/mkimage)" >&2; exit 1; }

# mkimage -f shells out to dtc.
DTCCAND=""
if command -v dtc >/dev/null 2>&1; then
  DTCCAND="$(command -v dtc)"
elif [ -x "$ROOT_DIR/uboot_build/u-boot/scripts/dtc/dtc" ]; then
  DTCCAND="$ROOT_DIR/uboot_build/u-boot/scripts/dtc/dtc"
fi
[ -n "$DTCCAND" ] || {
  echo "error: dtc not found (build U-Boot first or install device-tree-compiler)" >&2
  exit 1; }

PATH="$(dirname "$DTCCAND"):$PATH" "$MKIMG" -f "$AMP_DIR/amp.its" -E -p 0xe00 \
  "$AMP_DIR/amp.img"

echo "   mcu.bin : $BIN_SIZE bytes"
echo "   amp.img : $(stat -c %s "$AMP_DIR/amp.img") bytes (M0 standalone FIT, load 0x03a00000)"
echo "== M0 firmware ready =="
