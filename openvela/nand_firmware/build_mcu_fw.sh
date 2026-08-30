#!/bin/bash
# build_mcu_fw.sh - Build the RK3506 M0 (bus MCU) rpmsg-test firmware from
# the Linux SDK HAL and regenerate vendor/rockchip/chips/rk3506/rk3506_mcu_fw.h.
#
# The SDK tree is READ-ONLY: the project is copied into a scratch dir with
# hal/lib, hal/middleware, ... symlinked back into the SDK, then the two
# demo switches (TEST_DEMO, RPMSG_LINUX_TEST) are enabled in the copy.
#
# Usage:  RK3506_SDK_DIR=/path/to/rk3506_linux6.1_sdk bash build_mcu_fw.sh
set -euo pipefail

SDK_DIR="${RK3506_SDK_DIR:-}"
if [ -z "$SDK_DIR" ]; then
  echo "error: set RK3506_SDK_DIR (e.g. /home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm)" >&2
  exit 1
fi
SDK_DIR="$(realpath "$SDK_DIR")"
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_FW_H="$SELF_DIR/../vendor/rockchip/chips/rk3506/rk3506_mcu_fw.h"

# Cross toolchain from the openvela prebuilts (the SDK's own arm-none-eabi
# under prebuilts/gcc/linux-x86/... is preferred when present).
if [ -d "$SDK_DIR/prebuilts/gcc/linux-x86/arm/gcc-arm-none-eabi-10-2020-q4-major-x86_64-linux/bin" ]; then
  export PATH="$SDK_DIR/prebuilts/gcc/linux-x86/arm/gcc-arm-none-eabi-10-2020-q4-major-x86_64-linux/bin:$PATH"
elif [ -d "$SELF_DIR/../openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin" ]; then
  export PATH="$SELF_DIR/../openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH"
fi
command -v arm-none-eabi-gcc >/dev/null || { echo "error: arm-none-eabi-gcc not in PATH" >&2; exit 1; }

WORK="$SELF_DIR/.mcu_build"
rm -rf "$WORK"
mkdir -p "$WORK/hal/project/rk3506-mcu"

# Everything under hal/ except project/ stays in the SDK (symlinked);
# project/ contains the real (patched) copy of rk3506-mcu.
for d in "$SDK_DIR"/hal/*; do
  b=$(basename "$d")
  [ "$b" = "project" ] && continue
  [ -e "$d" ] || continue
  ln -s "$d" "$WORK/hal/$b"
done
mkdir -p "$WORK/hal/project"
ln -s "$SDK_DIR/hal/project/common" "$WORK/hal/project/common"
cp -r "$SDK_DIR/hal/project/rk3506-mcu/GCC" \
      "$SDK_DIR/hal/project/rk3506-mcu/src" \
      "$SDK_DIR/hal/project/rk3506-mcu/Image" \
      "$SDK_DIR/hal/project/rk3506-mcu/mkimage.sh" \
      "$WORK/hal/project/rk3506-mcu/"

# Enable the demo entry point and the rpmsg remote test in the COPY.
sed -i 's|^//#define TEST_DEMO|#define TEST_DEMO|' \
  "$WORK/hal/project/rk3506-mcu/src/main.c"
sed -i 's|^//#define RPMSG_LINUX_TEST|#define RPMSG_LINUX_TEST|' \
  "$WORK/hal/project/rk3506-mcu/src/test_demo.c"
grep -q '^#define TEST_DEMO' "$WORK/hal/project/rk3506-mcu/src/main.c" || {
  echo "error: failed to enable TEST_DEMO" >&2; exit 1; }
grep -q '^#define RPMSG_LINUX_TEST' "$WORK/hal/project/rk3506-mcu/src/test_demo.c" || {
  echo "error: failed to enable RPMSG_LINUX_TEST" >&2; exit 1; }

make -C "$WORK/hal/project/rk3506-mcu/GCC" -j$(nproc)

BIN="$WORK/hal/project/rk3506-mcu/GCC/TestDemo.bin"
python3 - "$BIN" "$OUT_FW_H" <<'PYEOF'
import sys
src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()
if len(data) > 0x8000:
    sys.exit(f"error: MCU firmware {len(data)} bytes exceeds 32 KiB M0 SRAM")
lines = []
lines.append("/* rk3506_mcu_fw.h - AUTO-GENERATED, do not edit.")
lines.append(" *")
lines.append(" * Regenerate with openvela/nand_firmware/build_mcu_fw.sh")
lines.append(f" * Source: SDK hal/project/rk3506-mcu TestDemo.bin ({len(data)} bytes)")
lines.append(" * (TEST_DEMO + RPMSG_LINUX_TEST enabled).")
lines.append(" * Raw Cortex-M0 image, loaded at 0xfff84000 (A7 view of M0 SRAM),")
lines.append(" * vector table at offset 0 (initial SP=0x7c00, Reset_Handler=0x171).")
lines.append(" */")
lines.append("")
lines.append("#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_MCU_FW_H")
lines.append("#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_MCU_FW_H")
lines.append("")
lines.append("#include <stdint.h>")
lines.append("")
lines.append(f"#define RK3506_MCU_FW_SIZE {len(data)}")
lines.append("")
lines.append("static const uint8_t g_rk3506_mcu_fw[RK3506_MCU_FW_SIZE] =")
lines.append("{")
for i in range(0, len(data), 12):
    chunk = data[i:i + 12]
    lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
lines.append("};")
lines.append("")
lines.append("#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_MCU_FW_H */")
with open(dst, "w") as f:
    f.write("\n".join(lines) + "\n")
print(f"wrote {dst}: {len(data)} bytes of firmware")
PYEOF
