#!/bin/bash
#
# HD-RK3506-EVM 板级固件打包脚本
# 编译后运行此脚本生成可烧录的 update.img
#
# 用法: ./package.sh [build_dir]
# 示例: ./package.sh ../../cmake_out/hd-rk3506-evm_nsh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Find project root by looking for build.sh
TOPDIR="$(cd "$SCRIPT_DIR" && while [ ! -f "build.sh" ] && [ "$(pwd)" != "/" ]; do cd ..; done && pwd)"
BUILD_DIR="${1:-$TOPDIR/cmake_out/hd-rk3506-evm_nsh}"
NAND_FIRMWARE_DIR="$TOPDIR/nand_firmware"

echo "========================================"
echo "  HD-RK3506-EVM 固件打包工具"
echo "========================================"
echo ""

# 检查 nuttx.bin 是否存在
if [ ! -f "$BUILD_DIR/nuttx.bin" ]; then
    echo "错误: nuttx.bin 不存在: $BUILD_DIR/nuttx.bin"
    echo "请先编译: ./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j\$(nproc)"
    exit 1
fi

# 复制 nuttx.bin 到 nand_firmware 目录
echo "[1/2] 复制 nuttx.bin -> nand_firmware/nuttx.bin"
cp "$BUILD_DIR/nuttx.bin" "$NAND_FIRMWARE_DIR/nuttx.bin"
echo "  -> $(ls -lh "$NAND_FIRMWARE_DIR/nuttx.bin" | awk '{print $5}')"

# 运行 pack_nand.sh 生成 update.img
echo ""
echo "[2/2] 生成 update.img..."
cd "$NAND_FIRMWARE_DIR"
bash pack_nand.sh

echo ""
echo "========================================"
echo "  打包完成！"
echo "========================================"
echo ""
echo "输出文件:"
ls -lh "$NAND_FIRMWARE_DIR"/*.img "$NAND_FIRMWARE_DIR"/*.bin "$NAND_FIRMWARE_DIR"/*.txt 2>/dev/null
echo ""
echo "烧录方法:"
echo "  1. 安装 upgrade_tool: sudo cp $NAND_FIRMWARE_DIR/../tools/linux/Linux_Upgrade_Tool/Linux_Upgrade_Tool/upgrade_tool /usr/local/bin/"
echo "  2. 进入 Loader 模式 (短接 RECOVERY + 插 USB)"
echo "  3. 烧录完整固件: sudo upgrade_tool uf $NAND_FIRMWARE_DIR/update.img"
echo ""
echo "或者只烧 boot + system:"
echo "  sudo upgrade_tool di boot $NAND_FIRMWARE_DIR/boot.img"
echo "  sudo upgrade_tool di system $NAND_FIRMWARE_DIR/system.img"
echo "  sudo upgrade_tool rd"
