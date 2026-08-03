#!/bin/bash
#
# openvela NAND 固件打包脚本
# 生成标准 Vela 分区布局的固件镜像
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="/home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm"
NUTTX_BIN="/home/b4qaq/project/openvela/cmake_out/hd-rk3506-evm_nsh/nuttx.bin"
NUTTX_DIR="/home/b4qaq/project/openvela"
OUTPUT_DIR="$SCRIPT_DIR"
PACK_DIR="$OUTPUT_DIR/pack"
DTC="$NUTTX_DIR/vendor/allwinnertech/lichee/brandy-2.0/u-boot-2018/scripts/dtc/dtc"

# 工具路径
MKIMAGE="$SDK_DIR/rkbin/tools/mkimage"
BOOT_MERGER="$SDK_DIR/rkbin/tools/boot_merger"
AFP_TOOL="$SDK_DIR/tools/linux/Linux_Pack_Firmware/rockdev/afptool"
RK_IMAGE_MAKER="$SDK_DIR/tools/linux/Linux_Pack_Firmware/rockdev/rkImageMaker"
UPGRADE_TOOL="$SDK_DIR/tools/linux/Linux_Upgrade_Tool/Linux_Upgrade_Tool/upgrade_tool"
GENROMFS="$(which genromfs 2>/dev/null || echo "$NUTTX_DIR/prebuilts/tools/linux/x86_64/genromfs")"

# 设置 PATH
export PATH="$(dirname $DTC):$PATH"

echo "========================================"
echo "  openvela NAND 固件打包工具"
echo "========================================"
echo ""

# ============================================================
# 分区布局说明 (参考小米 Vela 标准)
# ============================================================
#
# 分区名      大小      用途
# --------    ------    ----
# vnvm        2MB       NV 存储 (MAC地址等)
# uboot       8MB       U-Boot 引导加载器
# misc        2MB       启动模式控制
# recovery    30MB      恢复分区
# boot        32MB      NuttX 内核 (FIT 镜像)
# system      64MB      系统分区 (ROMFS, 只读)
# vendor      16MB      厂商定制分区
# oem         32MB      OEM 分区
# data        256MB     应用数据分区 (LittleFS)
# userdata    剩余      用户数据
#
# 与小米 Vela 官方对齐的分区:
#   /system  → system 分区 (ROMFS)
#   /data    → data 分区 (LittleFS)
#   /vendor  → vendor 分区 (ROMFS)
#   /oem     → oem 分区
# ============================================================

# 检查输入文件
if [ ! -f "$NUTTX_BIN" ]; then
    echo "错误: nuttx.bin 不存在: $NUTTX_BIN"
    echo "请先编译: ./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh --cmake -j\$(nproc)"
    exit 1
fi

echo "[1/7] 生成 FIT 镜像 (boot.img)..."

# 创建 ITS 文件
cat > /tmp/openvela-nuttx.its << 'ITS_EOF'
/dts-v1/;
/ {
    description = "openvela NuttX FIT image for RK3506";
    images {
        kernel {
            data = /incbin/("@KERNEL_IMG@");
            type = "kernel";
            arch = "arm";
            os = "linux";
            compression = "none";
            entry = <0x02080000>;
            load = <0x02080000>;
            hash {
                algo = "sha256";
            };
        };
    };
    configurations {
        default = "conf";
        conf {
            description = "Boot openvela NuttX";
            kernel = "kernel";
        };
    };
};
ITS_EOF

sed -i "s~@KERNEL_IMG@~$(realpath $NUTTX_BIN)~" /tmp/openvela-nuttx.its
$MKIMAGE -f /tmp/openvela-nuttx.its -E -p 0x800 "$OUTPUT_DIR/boot.img" > /dev/null
echo "  -> boot.img ($(du -h "$OUTPUT_DIR/boot.img" | cut -f1))"

echo ""
echo "[2/7] 生成 system.img (ROMFS)..."

# 创建 system ROMFS 内容目录
SYSTEM_DIR=$(mktemp -d)
mkdir -p "$SYSTEM_DIR/framework"
mkdir -p "$SYSTEM_DIR/bin"
mkdir -p "$SYSTEM_DIR/etc"
mkdir -p "$SYSTEM_DIR/lib"

# 从 NuttX 编译产物中复制系统文件
# system ROMFS 包含: 框架库、系统配置、基础资源
if [ -d "$NUTTX_DIR/vendor/openvela/boards/vela/prebuilts/system" ]; then
    cp -r "$NUTTX_DIR/vendor/openvela/boards/vela/prebuilts/system/"* "$SYSTEM_DIR/" 2>/dev/null || true
fi

# 创建 build.prop
cat > "$SYSTEM_DIR/build.prop" << 'EOF'
# openvela system properties
ro.build.version.sdk=35
ro.build.display.id=openvela-trunk-5.5
ro.product.model=HD-RK3506-EVM
ro.product.board=rk3506
ro.hardware=rk3506
ro.board.platform=rk3506
EOF

# 生成 ROMFS 镜像
if [ -x "$GENROMFS" ]; then
    $GENROMFS -f "$OUTPUT_DIR/system.img" -d "$SYSTEM_DIR" -V "system" 2>/dev/null
    echo "  -> system.img ($(du -h "$OUTPUT_DIR/system.img" | cut -f1))"
else
    # 如果没有 genromfs，创建空的占位镜像
    dd if=/dev/zero of="$OUTPUT_DIR/system.img" bs=1K count=64 2>/dev/null
    echo "  -> system.img (占位, 64KB) - 需要安装 genromfs"
fi
rm -rf "$SYSTEM_DIR"

echo ""
echo "[3/7] 生成 data.img (LittleFS)..."

# 创建 data 分区镜像 (LittleFS for NAND)
# 使用 dd 创建空镜像，运行时由 NuttX 格式化
dd if=/dev/zero of="$OUTPUT_DIR/data.img" bs=1M count=2 2>/dev/null
echo "  -> data.img ($(du -h "$OUTPUT_DIR/data.img" | cut -f1))"

echo ""
echo "[4/7] 生成 vendor.img 和 oem.img..."

# vendor 和 oem 分区 (空镜像，运行时由系统使用)
dd if=/dev/zero of="$OUTPUT_DIR/vendor.img" bs=1K count=64 2>/dev/null
dd if=/dev/zero of="$OUTPUT_DIR/oem.img" bs=1K count=64 2>/dev/null
echo "  -> vendor.img (占位)"
echo "  -> oem.img (占位)"

echo ""
echo "[5/7] 生成 MiniLoaderAll.bin..."

cd "$SDK_DIR/rkbin"
$BOOT_MERGER RKBOOT/RK3506MINIALL.ini > /dev/null 2>&1
cp rk3506_spl_loader_v1.06.111.bin "$OUTPUT_DIR/MiniLoaderAll.bin"
echo "  -> MiniLoaderAll.bin ($(du -h "$OUTPUT_DIR/MiniLoaderAll.bin" | cut -f1))"

echo ""
echo "[6/7] 复制分区表..."

cp "$SDK_DIR/device/rockchip/.chips/rk3506/parameter-evm-nand.txt" "$OUTPUT_DIR/parameter.txt"
echo "  -> parameter.txt"

echo ""
echo "[7/7] 打包 update.img..."

# 创建打包目录
rm -rf "$PACK_DIR"
mkdir -p "$PACK_DIR"

# 复制必要文件
cp "$OUTPUT_DIR/MiniLoaderAll.bin" "$PACK_DIR/"
cp "$OUTPUT_DIR/parameter.txt" "$PACK_DIR/"
cp "$OUTPUT_DIR/boot.img" "$PACK_DIR/"
cp "$OUTPUT_DIR/system.img" "$PACK_DIR/"
cp "$OUTPUT_DIR/data.img" "$PACK_DIR/"
cp "$OUTPUT_DIR/vendor.img" "$PACK_DIR/"
cp "$OUTPUT_DIR/oem.img" "$PACK_DIR/"

# 创建空的占位镜像
for img in misc recovery; do
    dd if=/dev/zero of="$PACK_DIR/${img}.img" bs=1K count=1 2>/dev/null
done

# uboot.img (占位，实际使用时需要从 SDK 编译)
dd if=/dev/zero of="$PACK_DIR/uboot.img" bs=1K count=1 2>/dev/null

# 创建 package-file
cat > "$PACK_DIR/package-file" << 'EOF'
# NAME	PATH
package-file	package-file
parameter	parameter.txt
bootloader	MiniLoaderAll.bin
uboot	uboot.img
misc	misc.img
boot	boot.img
recovery	recovery.img
system	system.img
vendor	vendor.img
oem	oem.img
data	data.img
EOF

# 打包
cd "$PACK_DIR"
$AFP_TOOL -pack ./ "$OUTPUT_DIR/update.raw.img" > /dev/null 2>&1
$RK_IMAGE_MAKER -RK3506 MiniLoaderAll.bin "$OUTPUT_DIR/update.raw.img" "$OUTPUT_DIR/update.img" -os_type:androidos > /dev/null 2>&1
rm -f "$OUTPUT_DIR/update.raw.img"

echo "  -> update.img ($(du -h "$OUTPUT_DIR/update.img" | cut -f1))"

echo ""
echo "[完成] 清理临时文件..."
rm -rf "$PACK_DIR"
rm -f /tmp/openvela-nuttx.its

echo ""
echo "========================================"
echo "  打包完成！"
echo "========================================"
echo ""
echo "输出文件:"
ls -lh "$OUTPUT_DIR"/*.img "$OUTPUT_DIR"/*.bin "$OUTPUT_DIR"/*.txt 2>/dev/null
echo ""
echo "分区布局:"
echo "  vnvm      2MB    NV 存储"
echo "  uboot     8MB    U-Boot"
echo "  misc      2MB    启动模式"
echo "  recovery  30MB   恢复分区"
echo "  boot      32MB   NuttX 内核"
echo "  system    64MB   系统 (ROMFS)"
echo "  vendor    16MB   厂商定制"
echo "  oem       32MB   OEM"
echo "  data      256MB  应用数据"
echo "  userdata  剩余   用户数据"
echo ""
echo "烧录方法:"
echo "  1. 开发板进入 Loader 模式 (短接 RECOVERY + 插 USB)"
echo "  2. 烧录完整固件:  $UPGRADE_TOOL uf $OUTPUT_DIR/update.img"
echo ""
echo "或者只烧 boot + system:"
echo "  $UPGRADE_TOOL di boot $OUTPUT_DIR/boot.img"
echo "  $UPGRADE_TOOL di system $OUTPUT_DIR/system.img"
echo "  $UPGRADE_TOOL rd"
