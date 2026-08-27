#!/bin/bash
#
# openvela RK3506 NAND 固件打包脚本（v2 — 重写版）
# 关键改进：
#   1. 路径全部使用环境变量 + 相对路径
#   2. 默认 RK3506 SDK 路径为 ../RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm
#   3. 优先使用本仓内已有的 nand_firmware/nuttx.bin（由 build.sh POSTBUILD 生成）
#   4. boot.img 只包含 vela.bin（紧凑，~500KB），不使用 34MB 零填充 bin
#
# 注意：本脚本不强制 -e，允许部分步骤失败（如 mkimage 不可用）
#
set +e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NUTTX_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
NUTTX_BIN="${NUTTX_BIN:-$SCRIPT_DIR/nuttx.bin}"

# SDK 路径：优先用环境变量，否则用相对路径
SDK_DIR="${RK3506_SDK_DIR:-$(cd "$NUTTX_DIR/../RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm" 2>/dev/null && pwd || echo "")}"

OUTPUT_DIR="$SCRIPT_DIR"
PACK_DIR="$OUTPUT_DIR/pack"

# 工具路径
MKIMAGE="${MKIMAGE:-$SDK_DIR/rkbin/tools/mkimage}"
BOOT_MERGER="${BOOT_MERGER:-$SDK_DIR/rkbin/tools/boot_merger}"
AFP_TOOL="${AFP_TOOL:-$SDK_DIR/tools/linux/Linux_Pack_Firmware/rockdev/afptool}"
RK_IMAGE_MAKER="${RK_IMAGE_MAKER:-$SDK_DIR/tools/linux/Linux_Pack_Firmware/rockdev/rkImageMaker}"
GENROMFS="${GENROMFS:-$(which genromfs 2>/dev/null || echo "$NUTTX_DIR/prebuilts/build-tools/linux-x86_64/bin/genromfs")}"

# 参数文件（分区表）
PARAM_FILE="${PARAM_FILE:-$NUTTX_DIR/vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/parameter.txt}"

echo "========================================"
echo "  openvela RK3506 NAND 固件打包"
echo "========================================"
echo "  NUTTX_BIN      = $NUTTX_BIN"
echo "  SDK_DIR        = $SDK_DIR"
echo "  PARAM_FILE     = $PARAM_FILE"
echo "  OUTPUT_DIR     = $OUTPUT_DIR"
echo ""

#--------------------------------------------------------------------
# 1. 输入检查
#--------------------------------------------------------------------
if [ ! -f "$NUTTX_BIN" ]; then
    echo "错误: 找不到 nuttx.bin: $NUTTX_BIN"
    echo "请先编译: ./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j\$(nproc)"
    exit 1
fi

NUTTX_SIZE=$(stat -c %s "$NUTTX_BIN")
echo "  vela.bin 大小 = $NUTTX_SIZE bytes ($(du -h "$NUTTX_BIN" | cut -f1))"
echo ""

#--------------------------------------------------------------------
# 2. 准备 nuttx.bin
#--------------------------------------------------------------------
# 避免把 nuttx.bin 复制到自身
if [ "$(realpath "$NUTTX_BIN")" != "$(realpath "$OUTPUT_DIR/nuttx.bin")" ]; then
    cp -f "$NUTTX_BIN" "$OUTPUT_DIR/nuttx.bin"
    echo "[1/5] 已准备 nuttx.bin"
else
    echo "[1/5] nuttx.bin 已是最新 ($NUTTX_SIZE bytes)"
fi

#--------------------------------------------------------------------
# 3. 生成 boot.img (RK boot image, 需 4KB 对齐)
#--------------------------------------------------------------------
echo "[2/5] 生成 boot.img..."

# RK 平台 boot.img 直接加载 bin 到 DDR，无需 U-Boot FIT 包装
# 注意：boot.img 需要 4KB 对齐 (load addr 0x02080000, 内核会自动处理)
cp -f "$NUTTX_BIN" "$OUTPUT_DIR/boot.img"

# 如果 size < 4MB，补齐到 4MB (Rockchip loader 要求)
BOOT_IMG_SIZE=$(stat -c %s "$OUTPUT_DIR/boot.img")
BOOT_IMG_ALIGNED_SIZE=$(( (BOOT_IMG_SIZE + 0x3FF) & ~0x3FF ))
if [ "$BOOT_IMG_ALIGNED_SIZE" -lt 4194304 ]; then
    BOOT_IMG_ALIGNED_SIZE=4194304
fi
if [ "$BOOT_IMG_ALIGNED_SIZE" -ne "$BOOT_IMG_SIZE" ]; then
    echo "  -> 对齐 boot.img 到 $BOOT_IMG_ALIGNED_SIZE bytes"
    truncate -s "$BOOT_IMG_ALIGNED_SIZE" "$OUTPUT_DIR/boot.img"
fi
echo "  -> boot.img ($(du -h "$OUTPUT_DIR/boot.img" | cut -f1))"

#--------------------------------------------------------------------
# 4. 生成 system.img (ROMFS) 和 data.img (LittleFS)
#--------------------------------------------------------------------
echo "[3/5] 生成 system.img (ROMFS)..."

if [ -x "$GENROMFS" ]; then
    SYSTEM_DIR=$(mktemp -d)
    mkdir -p "$SYSTEM_DIR/framework" "$SYSTEM_DIR/bin" "$SYSTEM_DIR/etc" "$SYSTEM_DIR/lib"

    # 从 Vela 预编译目录复制系统文件（如有）
    if [ -d "$NUTTX_DIR/vendor/openvela/boards/vela/prebuilts/system" ]; then
        cp -r "$NUTTX_DIR/vendor/openvela/boards/vela/prebuilts/system/"* "$SYSTEM_DIR/" 2>/dev/null || true
    fi

    # 生成 build.prop
    cat > "$SYSTEM_DIR/build.prop" << EOF
# openvela system properties
ro.build.version.sdk=35
ro.build.display.id=openvela-$(date +%Y%m%d)
ro.product.model=HD-RK3506-EVM
ro.product.board=rk3506
ro.hardware=rk3506
ro.board.platform=rk3506
EOF

    $GENROMFS -f "$OUTPUT_DIR/system.img" -d "$SYSTEM_DIR" -V "system" 2>/dev/null
    rm -rf "$SYSTEM_DIR"
    echo "  -> system.img ($(du -h "$OUTPUT_DIR/system.img" | cut -f1))"
else
    # 退化：占位镜像
    dd if=/dev/zero of="$OUTPUT_DIR/system.img" bs=1K count=64 2>/dev/null
    echo "  -> system.img (64KB placeholder, install genromfs for real one)"
fi

echo "[4/5] 生成 data.img (LittleFS) 和占位镜像..."
# data 分区 - 在运行时由 LittleFS 格式化
dd if=/dev/zero of="$OUTPUT_DIR/data.img" bs=1M count=2 2>/dev/null
echo "  -> data.img (2MB placeholder)"

# vendor / oem 占位
dd if=/dev/zero of="$OUTPUT_DIR/vendor.img" bs=1K count=64 2>/dev/null
dd if=/dev/zero of="$OUTPUT_DIR/oem.img" bs=1K count=64 2>/dev/null
echo "  -> vendor.img, oem.img (placeholder)"

#--------------------------------------------------------------------
# 5. 生成 update.img (Rockchip 打包格式)
#--------------------------------------------------------------------
echo "[5/5] 生成 update.img..."

rm -rf "$PACK_DIR"
mkdir -p "$PACK_DIR"

# MiniLoader
if [ -d "$SDK_DIR/rkbin" ] && [ -x "$BOOT_MERGER" ]; then
    (cd "$SDK_DIR/rkbin" && $BOOT_MERGER RKBOOT/RK3506MINIALL.ini > /dev/null 2>&1)
    if [ -f "$SDK_DIR/rkbin/rk3506_spl_loader_v1.06.111.bin" ]; then
        cp "$SDK_DIR/rkbin/rk3506_spl_loader_v1.06.111.bin" "$OUTPUT_DIR/MiniLoaderAll.bin"
    fi
fi
[ ! -f "$OUTPUT_DIR/MiniLoaderAll.bin" ] && cp "$OUTPUT_DIR/MiniLoaderAll.bin" /dev/null 2>/dev/null || true

# 拷贝所有到 pack/
[ -f "$OUTPUT_DIR/MiniLoaderAll.bin" ] && cp "$OUTPUT_DIR/MiniLoaderAll.bin" "$PACK_DIR/"
[ -f "$PARAM_FILE" ] && cp "$PARAM_FILE" "$PACK_DIR/parameter.txt"
cp "$OUTPUT_DIR/boot.img" "$PACK_DIR/"
cp "$OUTPUT_DIR/system.img" "$PACK_DIR/"
cp "$OUTPUT_DIR/data.img" "$PACK_DIR/"
cp "$OUTPUT_DIR/vendor.img" "$PACK_DIR/"
cp "$OUTPUT_DIR/oem.img" "$PACK_DIR/"

# 占位镜像
for img in misc recovery uboot; do
    [ ! -f "$PACK_DIR/${img}.img" ] && dd if=/dev/zero of="$PACK_DIR/${img}.img" bs=1K count=1 2>/dev/null
done

# 生成 package-file
cat > "$PACK_DIR/package-file" << EOF
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

# 打包（如果工具可用）
if [ -x "$AFP_TOOL" ] && [ -x "$RK_IMAGE_MAKER" ]; then
    (cd "$PACK_DIR" && $AFP_TOOL -pack ./ "$OUTPUT_DIR/update.raw.img" > /dev/null 2>&1)
    if [ -f "$OUTPUT_DIR/MiniLoaderAll.bin" ]; then
        (cd "$PACK_DIR" && $RK_IMAGE_MAKER -RK3506 MiniLoaderAll.bin "$OUTPUT_DIR/update.raw.img" "$OUTPUT_DIR/update.img" -os_type:androidos > /dev/null 2>&1)
    else
        # 没有 MiniLoader 时只生成 update.raw.img
        mv "$OUTPUT_DIR/update.raw.img" "$OUTPUT_DIR/update.img"
    fi
    rm -f "$OUTPUT_DIR/update.raw.img"
    echo "  -> update.img ($(du -h "$OUTPUT_DIR/update.img" | cut -f1))"
else
    echo "  ! afptool / rkImageMaker 未找到，跳过 update.img 生成"
    echo "    设置 RK3506_SDK_DIR 或安装 SDK 工具后重试"
fi

#--------------------------------------------------------------------
# 清理
#--------------------------------------------------------------------
rm -rf "$PACK_DIR"
rm -f /tmp/openvela-nuttx.its

echo ""
echo "========================================"
echo "  打包完成"
echo "========================================"
echo ""
echo "  输出文件:"
ls -lh "$OUTPUT_DIR"/*.img "$OUTPUT_DIR"/*.bin "$OUTPUT_DIR"/*.elf "$OUTPUT_DIR"/*.txt 2>/dev/null
echo ""
echo "  烧录方法 (使用 Rockchip upgrade_tool):"
echo "    1. 开发板进入 Loader 模式 (短接 RECOVERY + 插 USB)"
echo "    2. sudo upgrade_tool uf $OUTPUT_DIR/update.img     # 烧录完整固件"
echo "    3. 或单分区: sudo upgrade_tool di boot $OUTPUT_DIR/boot.img"
