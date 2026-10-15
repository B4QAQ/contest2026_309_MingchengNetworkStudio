#!/bin/bash
# build_mcu_fw.sh - Build the RK3506 M0 (bus MCU) rpmsg-test firmware from
# the Linux SDK HAL and package it as amp/amp.img (a standalone FIT that
# U-Boot loads and releases from the "amp" GPT partition).
#
# The SDK tree is READ-ONLY: the project is copied into a scratch dir with
# hal/lib, hal/middleware, ... symlinked back into the SDK, then the two
# demo switches (TEST_DEMO, RPMSG_LINUX_TEST) are enabled in the copy.
#
# Usage:  RK3506_SDK_DIR=/path/to/rk3506_linux6.1_sdk bash build_mcu_fw.sh
set -euo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"

# The full SDK hal/ tree is vendored at <repo root>/mcu_sdk/hal (see
# mcu_sdk/README.md), so this script is self-contained and does NOT need the
# external SDK.  This script lives at openvela/nand_firmware/, so the repo
# root is two levels up.  RK3506_SDK_DIR is a fallback override.
MCU_SDK_ROOT="$(cd "$SELF_DIR/../.." && pwd)"
if [ -d "$MCU_SDK_ROOT/mcu_sdk/hal" ]; then
  SDK_DIR="$MCU_SDK_ROOT/mcu_sdk"
elif [ -d "$SELF_DIR/mcu_sdk/hal" ]; then
  SDK_DIR="$SELF_DIR/mcu_sdk"
elif [ -n "${RK3506_SDK_DIR:-}" ]; then
  SDK_DIR="$RK3506_SDK_DIR"
else
  echo "error: neither <root>/mcu_sdk/hal nor RK3506_SDK_DIR is available" >&2
  exit 1
fi
SDK_DIR="$(realpath "$SDK_DIR")"

# Cross toolchain from the openvela prebuilts (the SDK's own arm-none-eabi
# under prebuilts/gcc/linux-x86/... is preferred when present).
if [ -d "$SDK_DIR/prebuilts/gcc/linux-x86/arm/gcc-arm-none-eabi-10-2020-q4-major-x86_64-linux/bin" ]; then
  export PATH="$SDK_DIR/prebuilts/gcc/linux-x86/arm/gcc-arm-none-eabi-10-2020-q4-major-x86_64-linux/bin:$PATH"
elif [ -d "$SELF_DIR/../prebuilts/gcc/linux-x86_64/arm-none-eabi/bin" ]; then
  export PATH="$SELF_DIR/../prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH"
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

# --- Move the M0 debug UART from UART4 (stamp pins 100/101, not brought out)
# --- to UART2 (J9 pin26 TX / pin25 RX on the base board), all in the COPY:
# ---   pUart        UART4            -> UART2
# ---   device       &g_uart4Dev      -> &g_uart2Dev
# ---   RMIO TX      BANK1 PIN_C2     -> BANK0 PIN_A2  (J9 pin26 = GPIO0_A2)
# ---   RMIO RX      BANK1 PIN_C3     -> BANK0 PIN_A0  (J9 pin25 = GPIO0_A0)
# --- The pin choice is the HD-documented J9 UART2 mapping
# --- (HD-RK3506-EVM UART.md: pinctrl-0 = <&rm_io0_uart2_rx &rm_io2_uart2_tx>;
# --- rm_ioN = GPIO0 pin N, and bank0 pins 0-23 are the programmable RM_IO pads
# --- per u-boot pinctrl-rk3506.c rockchip_set_rmio).
MAIN_C="$WORK/hal/project/rk3506-mcu/src/main.c"
sed -i 's|pUart = UART4;|pUart = UART2;|' "$MAIN_C"
sed -i 's|&g_uart4Dev|\&g_uart2Dev|' "$MAIN_C"
# RMIO calls are multi-line; rewrite each 3-line block with awk.
awk '
  /HAL_PINCTRL_SetRMIO\(GPIO_BANK1,/ {
      getline l2; getline l3
      if (l3 ~ /RMIO_UART4_TX/) {
          print "    HAL_PINCTRL_SetRMIO(GPIO_BANK0,"
          print "                        GPIO_PIN_A2,"
          print "                        RMIO_UART2_TX);"
          next
      } else if (l3 ~ /RMIO_UART4_RX/) {
          print "    HAL_PINCTRL_SetRMIO(GPIO_BANK0,"
          print "                        GPIO_PIN_A0,"
          print "                        RMIO_UART2_RX);"
          next
      } else { print; print l2; print l3; next }
  }
  { print }
' "$MAIN_C" > "$MAIN_C.tmp" && mv "$MAIN_C.tmp" "$MAIN_C"
grep -q 'pUart = UART2;' "$MAIN_C" || { echo "error: pUart not switched to UART2" >&2; exit 1; }
grep -q '&g_uart2Dev' "$MAIN_C" || { echo "error: uart dev not switched to g_uart2Dev" >&2; exit 1; }
grep -q 'RMIO_UART2_TX' "$MAIN_C" || { echo "error: RMIO UART2_TX not set" >&2; exit 1; }
grep -q 'RMIO_UART2_RX' "$MAIN_C" || { echo "error: RMIO UART2_RX not set" >&2; exit 1; }
grep -q 'RMIO_UART4' "$MAIN_C" && { echo "error: UART4 RMIO still present" >&2; exit 1; }
echo "  -> M0 debug UART moved to UART2 (J9 pin26 TX / pin25 RX, 1500000 8N1)"

make -C "$WORK/hal/project/rk3506-mcu/GCC" -j$(nproc)

BIN="$WORK/hal/project/rk3506-mcu/GCC/TestDemo.bin"
if [ ! -f "$BIN" ]; then
  echo "error: $BIN not produced" >&2
  exit 1
fi
if [ "$(stat -c %s "$BIN")" -gt 32768 ]; then
  echo "error: MCU firmware exceeds 32 KiB M0 SRAM" >&2
  exit 1
fi

#---------------------------------------------------------------------------
# Package the M0 firmware FIT (amp.img) — SDK flow:
# U-Boot (CONFIG_AMP / CONFIG_ROCKCHIP_AMP), in amp_cpus_on() during
# board_late_init, reads this FIT from the "amp" GPT partition, copies the
# standalone image (load=0xfff84000) into M0 SRAM while the core is held
# reset (boot_get_loadable), then calls fit_standalone_release().
# Template: SDK hal/project/rk3506-mcu/Image/amp.its, without the rsa
# signature node (our U-Boot does not enable FIT_SIGNATURE; sha256 hash is
# enough and CONFIG_FIT_ENABLE_SHA256_SUPPORT=y).
#---------------------------------------------------------------------------
AMP_DIR="$SELF_DIR/amp"
mkdir -p "$AMP_DIR"
cp -f "$BIN" "$AMP_DIR/mcu.bin"

cat > "$AMP_DIR/amp.its" <<'ITS_EOF'
/dts-v1/;
/ {
	description = "Rockchip AMP FIT Image";
	#address-cells = <1>;

	images {
		mcu {
			description  = "mcu";
			data         = /incbin/("./mcu.bin");
			type         = "standalone";
			compression  = "none";
			arch         = "arm";
			load         = <0xfff84000>;
			udelay       = <1000000>;
			hash {
				algo = "sha256";
			};
		};
	};

	configurations {
		default = "conf";
		conf {
			description = "Rockchip AMP images";
			rollback-index = <0x0>;
			loadables = "mcu";
		};
	};
};
ITS_EOF

# mkimage: SDK hal/tools first (same tool as the SDK mkimage.sh), then the
# locally rebuilt U-Boot tree.
MKIMG=""
for cand in "$SDK_DIR/hal/tools/mkimage" "$SELF_DIR/../../uboot_build/u-boot/tools/mkimage"; do
  [ -x "$cand" ] && MKIMG="$cand" && break
done
if [ -z "$MKIMG" ]; then
  echo "error: mkimage not found (SDK hal/tools/mkimage or uboot_build)" >&2
  exit 1
fi

# dtc: PATH first, then the locally rebuilt U-Boot tree (mkimage -f calls dtc).
DTCCAND=""
if command -v dtc >/dev/null 2>&1; then
  DTCCAND="$(command -v dtc)"
elif [ -x "$SELF_DIR/../../uboot_build/u-boot/scripts/dtc/dtc" ]; then
  DTCCAND="$SELF_DIR/../../uboot_build/u-boot/scripts/dtc/dtc"
fi
if [ -z "$DTCCAND" ]; then
  echo "error: dtc not found (build U-Boot first or install device-tree-compiler)" >&2
  exit 1
fi

PATH="$(dirname "$DTCCAND"):$PATH" "$MKIMG" -f "$AMP_DIR/amp.its" -E -p 0xe00 \
  "$AMP_DIR/amp.img"
echo "wrote $AMP_DIR/amp.img: $(stat -c %s "$AMP_DIR/amp.img") bytes (M0 standalone FIT)"
echo "M0 raw image: $BIN ($(stat -c %s "$BIN") bytes)"
