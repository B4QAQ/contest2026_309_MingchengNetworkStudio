#!/bin/bash
# build_mcu_fw.sh - Build the RK3506 M0 (bus MCU) rpmsg-test firmware and
# package it as nand_firmware/amp/amp.img (a standalone FIT that U-Boot
# loads and releases from the "amp" GPT partition).
#
# The M0 source tree is mcu_sdk/hal (a vendored copy of the Linux SDK HAL
# tree), configured exactly like the validated Linux-SDK AMP flow
# (AMP_RK3506_LINUX_SDK_NOTES.md section 3.7-3.9):
#
#   hal/project/rk3506-mcu/GCC/Makefile            LINKER_SCRIPT := gcc_bus_m0_ddr.ld
#   hal/project/rk3506-mcu/GCC/gcc_bus_m0_ddr.ld   RAM ORIGIN = 0x03a00000, 1 MiB
#   hal/project/rk3506-mcu/src/main.c              TEST_DEMO, UART1 @115200
#   hal/project/rk3506-mcu/src/test_demo.c         RPMSG_LINUX_TEST (master 0, remote 3)
#   hal/project/rk3506-mcu/Image/amp.its           load = 0x03a00000
#
# TOOLCHAIN: the SDK's own gcc-arm-none-eabi-10-2020-q4-major.  This is a
# hard requirement, not a preference: with a newer cross compiler the code
# layout shifts (Reset_Handler lands at 0x03a00171 instead of the
# validated 0x03a00141) and the resulting image is NOT the one the Linux
# SDK verified.  The script locates the SDK toolchain and fails loudly if
# it is missing instead of silently falling back.
#
# Usage:  bash build_mcu_fw.sh
set -euo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SELF_DIR/../.." && pwd)"

# --- locate the M0 source tree -------------------------------------------
if [ -d "$ROOT_DIR/mcu_sdk/hal" ]; then
  SDK_DIR="$ROOT_DIR/mcu_sdk"
else
  echo "error: <repo root>/mcu_sdk/hal not found" >&2
  exit 1
fi
SDK_DIR="$(realpath "$SDK_DIR")"
PROJ="$SDK_DIR/hal/project/rk3506-mcu"

# --- locate the RK3506 Linux SDK (toolchain + mkimage) -------------------
LINUX_SDK="${RK3506_LINUX_SDK:-}"
if [ -z "$LINUX_SDK" ]; then
  for cand in "$ROOT_DIR/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm" \
              "$ROOT_DIR"/RK3506G2/rk3506_linux6.1_sdk*; do
    [ -d "$cand/prebuilts" ] && LINUX_SDK="$cand" && break
  done
fi
[ -n "$LINUX_SDK" ] || {
  echo "error: RK3506 Linux SDK not found (needed for the arm-none-eabi" >&2
  echo "       toolchain).  Set RK3506_LINUX_SDK=/path/to/sdk." >&2
  exit 1; }

TC_DIR="$LINUX_SDK/prebuilts/gcc/linux-x86/arm/gcc-arm-none-eabi-10-2020-q4-major-x86_64-linux/bin"
[ -x "$TC_DIR/arm-none-eabi-gcc" ] || {
  echo "error: SDK toolchain missing: $TC_DIR/arm-none-eabi-gcc" >&2
  echo "       A newer compiler produces a different image layout (see header)." >&2
  exit 1; }
export PATH="$TC_DIR:$PATH"

GCC_VER=$(arm-none-eabi-gcc -dumpversion)
case "$GCC_VER" in
  10.*) : ;;
  *) echo "warning: expected SDK gcc 10.x, found $GCC_VER" >&2 ;;
esac

# --- pre-flight: the five configuration points must all be in place ------
grep -q '^#define TEST_DEMO' "$PROJ/src/main.c" || {
  echo "error: TEST_DEMO not enabled in mcu_sdk/.../src/main.c" >&2; exit 1; }
grep -q '^#define RPMSG_LINUX_TEST' "$PROJ/src/test_demo.c" || {
  echo "error: RPMSG_LINUX_TEST not enabled in mcu_sdk/.../src/test_demo.c" >&2
  exit 1; }
grep -q 'pUart = UART1;' "$PROJ/src/main.c" || {
  echo "error: M0 debug UART is not UART1 in mcu_sdk/.../src/main.c" >&2; exit 1; }
grep -q 'LINKER_SCRIPT := gcc_bus_m0_ddr.ld' "$PROJ/GCC/Makefile" || {
  echo "error: M0 linker script is not gcc_bus_m0_ddr.ld" >&2; exit 1; }
grep -q 'ORIGIN = 0x03a00000' "$PROJ/GCC/gcc_bus_m0_ddr.ld" || {
  echo "error: M0 DRAM origin is not 0x03a00000" >&2; exit 1; }

echo "== building M0 firmware (SDK gcc $GCC_VER, DRAM 0x03a00000, UART1) =="
find "$SDK_DIR/hal" -name '*.o' -delete
rm -f "$PROJ/GCC/TestDemo.elf" "$PROJ/GCC/TestDemo.map" "$PROJ/GCC/TestDemo.bin"
make -C "$PROJ/GCC" -j"$(nproc)" >/tmp/m0_build.log 2>&1 || {
  echo "error: M0 build failed, see /tmp/m0_build.log" >&2
  tail -20 /tmp/m0_build.log >&2
  exit 1; }

BIN="$PROJ/GCC/TestDemo.bin"
ELF="$PROJ/GCC/TestDemo.elf"
[ -f "$BIN" ] || { echo "error: $BIN not produced" >&2; exit 1; }

# --- static verification (AMP_RK3506_LINUX_SDK_NOTES.md section 3.7) ----
ENTRY=$(arm-none-eabi-readelf -h "$ELF" | sed -n 's/.*Entry point address:[[:space:]]*//p')
[ "$ENTRY" = "0x3a00141" ] || {
  echo "error: M0 entry point is $ENTRY, expected 0x3a00141" >&2
  echo "       (wrong toolchain or linker script - see the header note)" >&2
  exit 1; }

VEC=$(xxd -p -l 8 "$BIN")
[ "$VEC" = "00fcaf034101a003" ] || {
  echo "error: M0 vector table is $VEC, expected 00fcaf034101a003" >&2
  echo "       (SP should be 0x03affc00, Reset_Handler 0x03a00141)" >&2
  exit 1; }

BIN_SIZE=$(stat -c %s "$BIN")
[ "$BIN_SIZE" -le 1048576 ] || {
  echo "error: M0 firmware ($BIN_SIZE B) exceeds the 1 MiB DRAM window" >&2
  exit 1; }
echo "   entry 0x3a00141, SP 0x03affc00, $BIN_SIZE bytes - matches SDK baseline"

# --- package the FIT ----------------------------------------------------
AMP_DIR="$SELF_DIR/amp"
mkdir -p "$AMP_DIR"
cp -f "$BIN" "$AMP_DIR/mcu.bin"
# The committed .its incbins ../GCC/TestDemo.bin relative to its own home;
# rewrite that one line to point at the copy next to the generated .its.
sed 's|/incbin/("[^"]*")|/incbin/("./mcu.bin")|' \
  "$PROJ/Image/amp.its" > "$AMP_DIR/amp.its"

MKIMG=""
for cand in "$SDK_DIR/hal/tools/mkimage" \
            "$LINUX_SDK/hal/tools/mkimage"; do
  [ -x "$cand" ] && MKIMG="$cand" && break
done
[ -n "$MKIMG" ] || { echo "error: mkimage not found" >&2; exit 1; }

DTCCAND=""
if command -v dtc >/dev/null 2>&1; then
  DTCCAND="$(command -v dtc)"
elif [ -x "$ROOT_DIR/uboot_build/u-boot/scripts/dtc/dtc" ]; then
  DTCCAND="$ROOT_DIR/uboot_build/u-boot/scripts/dtc/dtc"
fi
[ -n "$DTCCAND" ] || { echo "error: dtc not found" >&2; exit 1; }

PATH="$(dirname "$DTCCAND"):$PATH" "$MKIMG" -f "$AMP_DIR/amp.its" -E -p 0xe00 \
  "$AMP_DIR/amp.img"

# --- verify the packaged FIT -------------------------------------------
LOAD=$("$MKIMG" -l "$AMP_DIR/amp.img" | sed -n 's/.*Load Address:[[:space:]]*//p')
[ "$LOAD" = "0x03a00000" ] || {
  echo "error: amp.img load address is $LOAD, expected 0x03a00000" >&2; exit 1; }

echo "   mcu.bin : $BIN_SIZE bytes"
echo "   amp.img : $(stat -c %s "$AMP_DIR/amp.img") bytes (load 0x03a00000)"
echo "== M0 firmware ready =="
