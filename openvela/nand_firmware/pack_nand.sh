#!/bin/bash
#
# openvela RK3506 NAND 固件打包脚本（v5 — A/B 双分区 OTA）
#
# 关键改进 (v5)：
#   1. parameter.txt 换成板级 A/B 布局 (boot_a/boot_b 双槽, 无 recovery/oem),
#      板级文件优先于 SDK 的 parameter-evm-nand.txt (那是旧单 boot 布局)
#   2. package-file 改为内置 A/B 版本: boot_a/boot_b 都写同一个 FIT 镜像,
#      misc 写 8KB 零占位 (U-Boot 首次启动自动初始化 AvbABData -> 启动 boot_a)
#   3. 其余沿用 SDK 官方 afptool + rkImageMaker 流程 (-RK350F chip tag)
#
# A/B 启动链：
#   BootROM -> MiniLoaderAll.bin (SPL) -> U-Boot (boot_fit, 读 misc@2048 的
#   AvbABData 选槽) -> boot_a/boot_b 中的 NuttX FIT -> NuttX rcS `ota bootcheck`
#
set +e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NUTTX_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
NUTTX_BIN="${NUTTX_BIN:-$SCRIPT_DIR/nuttx.bin}"

# SDK 路径：优先用环境变量，否则用相对路径
SDK_DIR="${RK3506_SDK_DIR:-$(cd "$NUTTX_DIR/../RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm" 2>/dev/null && pwd || echo "")}"

OUTPUT_DIR="$SCRIPT_DIR"
PACK_DIR="$OUTPUT_DIR/pack"
WORK_DIR="$OUTPUT_DIR/rockdev"

# SDK 工具路径
MKIMAGE="${MKIMAGE:-$SDK_DIR/rkbin/tools/mkimage}"
BOOT_MERGER="${BOOT_MERGER:-$SDK_DIR/rkbin/tools/boot_merger}"
AFP_TOOL="${AFP_TOOL:-$SDK_DIR/tools/linux/Linux_Pack_Firmware/rockdev/afptool}"
RK_IMAGE_MAKER="${RK_IMAGE_MAKER:-$SDK_DIR/tools/linux/Linux_Pack_Firmware/rockdev/rkImageMaker}"
LINUX_PACK_DIR="${LINUX_PACK_DIR:-$SDK_DIR/tools/linux/Linux_Pack_Firmware/rockdev}"

# 分区表: 环境变量 > 板级 A/B parameter.txt > SDK parameter-evm-nand.txt
# (板级 v5 是 A/B 双槽布局; SDK 那份是旧单 boot 布局, 只在没有板级文件时兜底)
BOARD_PARAM="$NUTTX_DIR/vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/parameter.txt"
PARAM_FILE="${PARAM_FILE:-$BOARD_PARAM}"
if [ ! -f "$PARAM_FILE" ]; then
    PARAM_FILE="$SDK_DIR/device/rockchip/rk3506/parameter-evm-nand.txt"
fi

echo "========================================"
echo "  openvela RK3506 NAND 固件打包 (v3)"
echo "========================================"
echo "  NUTTX_BIN      = $NUTTX_BIN"
echo "  SDK_DIR        = $SDK_DIR"
echo "  PARAM_FILE     = $PARAM_FILE"
echo "  AFP_TOOL       = $AFP_TOOL"
echo "  RK_IMAGE_MAKER = $RK_IMAGE_MAKER"
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

if [ ! -x "$BOOT_MERGER" ] || [ ! -x "$AFP_TOOL" ] || [ ! -x "$RK_IMAGE_MAKER" ]; then
    echo "错误: SDK 工具不完整"
    echo "  BOOT_MERGER     = $BOOT_MERGER"
    echo "  AFP_TOOL        = $AFP_TOOL"
    echo "  RK_IMAGE_MAKER  = $RK_IMAGE_MAKER"
    exit 1
fi

# A/B 布局自检: 分区表必须定义 boot_a/boot_b, 否则 U-Boot 的 A/B 选择
# 没有意义 (旧单 boot 布局会静默退化成单槽).
if ! grep -q "boot_a" "$PARAM_FILE" || ! grep -q "boot_b" "$PARAM_FILE"; then
    echo "警告: $PARAM_FILE 不含 boot_a/boot_b 分区 (旧布局), OTA A/B 不可用!"
fi

NUTTX_SIZE=$(stat -c %s "$NUTTX_BIN")
echo "  nuttx.bin 大小 = $NUTTX_SIZE bytes"
echo ""

#--------------------------------------------------------------------
# 2. 准备 nuttx.bin
#--------------------------------------------------------------------
if [ "$(realpath "$NUTTX_BIN")" != "$(realpath "$OUTPUT_DIR/nuttx.bin")" ]; then
    cp -f "$NUTTX_BIN" "$OUTPUT_DIR/nuttx.bin"
    echo "[1/7] 已准备 nuttx.bin"
fi

#--------------------------------------------------------------------
# 3. 生成 boot.img (4KB 对齐的 raw bin)
#--------------------------------------------------------------------
echo "[2/7] 生成 boot.img..."
cp -f "$NUTTX_BIN" "$OUTPUT_DIR/boot.img"
BOOT_IMG_SIZE=$(stat -c %s "$OUTPUT_DIR/boot.img")
BOOT_IMG_ALIGNED_SIZE=$(( (BOOT_IMG_SIZE + 0x3FF) & ~0x3FF ))
if [ "$BOOT_IMG_ALIGNED_SIZE" -lt 4194304 ]; then
    BOOT_IMG_ALIGNED_SIZE=4194304
fi
if [ "$BOOT_IMG_ALIGNED_SIZE" -ne "$BOOT_IMG_SIZE" ]; then
    truncate -s "$BOOT_IMG_ALIGNED_SIZE" "$OUTPUT_DIR/boot.img"
fi
echo "  -> boot.img $(stat -c %s "$OUTPUT_DIR/boot.img") bytes"

# 同时生成 uImage (U-Boot bootm 用, 备选)
if [ -x "$MKIMAGE" ]; then
    $MKIMAGE -A arm -O linux -T kernel -C none \
        -a 0x02080000 -e 0x02080560 \
        -n "openvela-rk3506" \
        -d "$NUTTX_BIN" \
        "$OUTPUT_DIR/boot.uimg" 2>/dev/null
    if [ -f "$OUTPUT_DIR/boot.uimg" ]; then
        UIMG_SIZE=$(stat -c %s "$OUTPUT_DIR/boot.uimg")
        UIMG_ALIGNED=$(( (UIMG_SIZE + 0x3FF) & ~0x3FF ))
        [ "$UIMG_ALIGNED" -lt 4194304 ] && UIMG_ALIGNED=4194304
        [ "$UIMG_ALIGNED" -ne "$UIMG_SIZE" ] && truncate -s "$UIMG_ALIGNED" "$OUTPUT_DIR/boot.uimg"
        echo "  -> boot.uimg $(stat -c %s "$OUTPUT_DIR/boot.uimg") bytes"
    fi
fi

#--------------------------------------------------------------------
# 3b. 生成 FIT 镜像 boot.fit (默认 bootcmd "boot_fit" 自动引导用)
#
# 关键: Rockchip U-Boot 默认 bootcmd = "boot_fit;boot_android ..."
#   boot_fit 从 boot 分区开头读 FDT, 要求:
#     1) FDT magic 0xd00dfeed
#     2) FDT 结构 < 4KB (fit_is_ext_type)
#     3) kernel 数据用 external data 形式追加在 FDT 之后 (mkimage -E)
#   满足后 boot_fit 自动加载 kernel 到 load 地址并跳到 entry.
#
#   用真实地址 load=0x02080000 / entry=0x02080560 (非 0xffffff01 占位符),
#   U-Boot 不重定位; entry bit0=0 -> bootm 走 ARM 模式跳转
#   (绕开 `go` 命令 entry|1 强制 Thumb 的问题).
#
#   mkimage -E 需要 dtc. 从 openvela 的 allwinner 工具链里找.
#--------------------------------------------------------------------
echo "[3b] 生成 FIT 镜像 boot.fit..."
DTC_BIN="$(command -v dtc 2>/dev/null)"
if [ -z "$DTC_BIN" ]; then
    for cand in \
        "$NUTTX_DIR/vendor/allwinnertech/lichee/brandy-2.0/u-boot-2018/scripts/dtc/dtc" \
        "$SDK_DIR/u-boot/scripts/dtc/dtc"; do
        [ -x "$cand" ] && DTC_BIN="$cand" && break
    done
fi
if [ -n "$DTC_BIN" ]; then
    export PATH="$(dirname "$DTC_BIN"):$PATH"
fi

ITS_FILE="$OUTPUT_DIR/nuttx.its"
if [ -x "$MKIMAGE" ] && [ -n "$DTC_BIN" ]; then
    # 生成最小内核 FDT.
    # Rockchip U-Boot 的 bootm 强制要求内核 FDT (对 fdt 指针调 fdt_check_header;
    # 没有 fdt 节点时指针为 NULL -> data abort @ fdt 校验). NuttX 不读 r2/atags,
    # 所以给一个最小 DTB 仅为满足 U-Boot; 它用占位符 load 0xffffff00,
    # U-Boot 会重定位到 fdt_addr_r (0x63000), 不与内核 0x02080000 重叠.
    cat > "$OUTPUT_DIR/nuttx-fdt.dts" << 'FDT_EOF'
/dts-v1/;
/ {
	model = "HD-RK3506-EVM OpenVela";
	compatible = "rockchip,rk3506-evb", "rockchip,rk3506";
	#address-cells = <1>;
	#size-cells = <1>;
};
FDT_EOF
    ( cd "$OUTPUT_DIR" && dtc -I dts -O dtb -o nuttx-fdt.dtb nuttx-fdt.dts > /dev/null 2>&1 )

    cat > "$ITS_FILE" << 'ITS_EOF'
/dts-v1/;
/ {
	description = "OpenVela NuttX kernel for RK3506G2";
	#address-cells = <1>;
	images {
		kernel {
			description = "NuttX/OpenVela";
			data = /incbin/("./nuttx.bin");
			type = "kernel";
			arch = "arm";
			os = "linux";
			compression = "none";
			load = <0x02080000>;
			entry = <0x02080560>;
			hash { algo = "sha256"; };
		};
		fdt {
			description = "Minimal kernel FDT (NuttX ignores r2)";
			data = /incbin/("./nuttx-fdt.dtb");
			type = "flat_dt";
			arch = "arm";
			compression = "none";
			load = <0xffffff00>;
			hash { algo = "sha256"; };
		};
	};
	configurations {
		default = "conf";
		conf {
			description = "OpenVela kernel";
			kernel = "kernel";
			fdt = "fdt";
		};
	};
};
ITS_EOF
    ( cd "$OUTPUT_DIR" && "$MKIMAGE" -f nuttx.its -E -p 0x1000 boot.fit > /dev/null 2>&1 )
    if [ -f "$OUTPUT_DIR/boot.fit" ]; then
        FIT_SIZE=$(stat -c %s "$OUTPUT_DIR/boot.fit")
        # boot_a/boot_b 分区各 10MB, 镜像填充到 4MB (和 boot.img 一致)
        FIT_ALIGNED=$(( (FIT_SIZE + 0x3FF) & ~0x3FF ))
        [ "$FIT_ALIGNED" -lt 4194304 ] && FIT_ALIGNED=4194304
        [ "$FIT_ALIGNED" -ne "$FIT_SIZE" ] && truncate -s "$FIT_ALIGNED" "$OUTPUT_DIR/boot.fit"
        echo "  -> boot.fit $(stat -c %s "$OUTPUT_DIR/boot.fit") bytes (FIT kernel+fdt, boot_fit 可自动引导)"
        # 让独立的 boot.img 也是 FIT, 这样单分区烧录 (di boot boot.img) 也能自动引导
        cp -f "$OUTPUT_DIR/boot.fit" "$OUTPUT_DIR/boot.img"
    else
        echo "  警告: boot.fit 生成失败 (检查 dtc/mkimage)"
    fi
else
    echo "  警告: 未找到 mkimage 或 dtc, 跳过 FIT 镜像"
fi


#--------------------------------------------------------------------
# 4. 生成 MiniLoaderAll.bin
#--------------------------------------------------------------------
echo "[3/7] 生成 MiniLoaderAll.bin..."
(cd "$SDK_DIR/rkbin" && $BOOT_MERGER RKBOOT/RK3506MINIALL.ini > /dev/null 2>&1)
if [ -f "$SDK_DIR/rkbin/rk3506_spl_loader_v1.06.111.bin" ]; then
    cp -f "$SDK_DIR/rkbin/rk3506_spl_loader_v1.06.111.bin" "$OUTPUT_DIR/MiniLoaderAll.bin"
    echo "  -> MiniLoaderAll.bin $(stat -c %s "$OUTPUT_DIR/MiniLoaderAll.bin") bytes"
fi

#--------------------------------------------------------------------
# 5. 准备 U-Boot FIT 镜像
#--------------------------------------------------------------------
echo "[4/7] 准备 U-Boot..."
# U-Boot FIT 来源: UBOOT_IMG 环境变量 > nand_firmware/uboot.itb (自建 AB 版) >
# SDK fit/uboot.itb (预编译, 无 CONFIG_ANDROID_AB, 只认名为 boot 的分区)
UBOOT_SRC_CANDIDATES=(
    "${UBOOT_IMG:+$UBOOT_IMG}"
    "$SCRIPT_DIR/uboot.itb"
    "$SDK_DIR/u-boot/fit/uboot.itb"
    "$SDK_DIR/u-boot/u-boot.itb"
)
UBOOT_SRC=""
for ub in "${UBOOT_SRC_CANDIDATES[@]}"; do
    if [ -f "$ub" ]; then
        UBOOT_SRC="$ub"
        break
    fi
done
if [ -z "$UBOOT_SRC" ]; then
    echo "错误: 找不到 U-Boot FIT 镜像"
    echo "尝试路径:"
    for ub in "${UBOOT_SRC_CANDIDATES[@]}"; do
        echo "  $ub"
    done
    exit 1
fi
cp -f "$UBOOT_SRC" "$OUTPUT_DIR/uboot.img"
echo "  -> uboot.img $(stat -c %s "$OUTPUT_DIR/uboot.img") bytes (from $UBOOT_SRC)"

#--------------------------------------------------------------------
# 6. 准备 rockdev/Image/ 目录
#--------------------------------------------------------------------
echo "[5/7] 准备 rockdev 目录..."
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR/Image"

# A/B 双槽 package-file (内置, 不用 SDK 的 rk3506-package-file — 那份列的
# boot/rootfs/recovery/oem 在 v5 布局里已不存在).  boot_a/boot_b 写同一个
# FIT 镜像; misc 写 8KB 零占位, U-Boot 首次启动发现元数据非法后自动重置
# 默认值并引导 boot_a.  userdata 不列 (全量刷不清用户 /data).
cat > "$WORK_DIR/package-file" << EOF
# NAME		Relative path
package-file	package-file
bootloader	Image/MiniLoaderAll.bin
parameter	Image/parameter.txt
uboot       Image/uboot.img
boot_a      Image/boot_a.img
boot_b      Image/boot_b.img
misc		Image/misc.img
backup		RESERVED
EOF
echo "  使用内置 A/B package-file (boot_a/boot_b/misc, 无 rootfs/recovery/oem)"

# 拷贝所有镜像
cp -f "$OUTPUT_DIR/MiniLoaderAll.bin" "$WORK_DIR/Image/"
cp -f "$PARAM_FILE" "$WORK_DIR/Image/parameter.txt"
cp -f "$OUTPUT_DIR/uboot.img" "$WORK_DIR/Image/"

# boot_a/boot_b 内容: 优先用 boot.fit (FIT external-data 镜像)
#   -> U-Boot 默认 bootcmd 的 boot_fit 能直接识别并自动引导 NuttX
#      (FDT magic 0xd00dfeed + FDT<4KB + external data), 上电自动进 NSH.
#      两个槽写同一镜像: 首刷后 boot_a 是默认启动槽, boot_b 作为 OTA 目标.
# 回退: boot.uimg (legacy uImage, 需手动 bootm) 或 boot.img (raw bin, 需手动 go).
if [ -f "$OUTPUT_DIR/boot.fit" ]; then
    cp -f "$OUTPUT_DIR/boot.fit" "$WORK_DIR/Image/boot_a.img"
    cp -f "$OUTPUT_DIR/boot.fit" "$WORK_DIR/Image/boot_b.img"
    echo "  -> boot_a/boot_b 使用 boot.fit (FIT, boot_fit 可自动引导)"
elif [ -f "$OUTPUT_DIR/boot.uimg" ]; then
    cp -f "$OUTPUT_DIR/boot.uimg" "$WORK_DIR/Image/boot_a.img"
    cp -f "$OUTPUT_DIR/boot.uimg" "$WORK_DIR/Image/boot_b.img"
    echo "  -> boot_a/boot_b 使用 boot.uimg (uImage, 需 bootm)"
else
    cp -f "$OUTPUT_DIR/boot.img" "$WORK_DIR/Image/boot_a.img"
    cp -f "$OUTPUT_DIR/boot.img" "$WORK_DIR/Image/boot_b.img"
    echo "  -> boot_a/boot_b 使用 boot.img (raw bin, 需 go)"
fi

# 占位镜像 (misc: BCB+AvbABData 区域, 全零 -> U-Boot 首启重置为默认槽位元数据)
for img in misc; do
    if [ ! -f "$WORK_DIR/Image/${img}.img" ]; then
        dd if=/dev/zero of="$WORK_DIR/Image/${img}.img" bs=1K count=8 2>/dev/null
    fi
done

echo "  rockdev/Image/ 准备完成:"
ls -la "$WORK_DIR/Image/"

#--------------------------------------------------------------------
# 7. 打包 update.img (使用 SDK 官方 afptool + rkImageMaker)
#--------------------------------------------------------------------
echo "[6/7] 打包 update.img (afptool)..."
(cd "$WORK_DIR" && $AFP_TOOL -pack ./ Image/update.img > /dev/null 2>&1)
if [ ! -f "$WORK_DIR/Image/update.img" ]; then
    echo "错误: afptool 打包失败"
    exit 1
fi
echo "  -> Image/update.img $(stat -c %s "$WORK_DIR/Image/update.img") bytes"

echo "[7/7] 打包 update.img (rkImageMaker)..."

# NOTE: 必须用 -RK350F (不是 -RK3506!), 因为 chip tag 由 MiniLoaderAll.bin
# 的 IDB 决定. boot_merger RKBOOT/RK3506MINIALL.ini 的 [CHIP_NAME] NAME=RK350F,
# 所以 IDB 里的 chip code 是 0x33303546 (350F). 用 -RK3506 写出来的 image 是
# 0x33303536 (3506), 与 IDB 不一致, 烧录时会出现 "芯片标志位不对".
$RK_IMAGE_MAKER -RK350F "$WORK_DIR/Image/MiniLoaderAll.bin" "$WORK_DIR/Image/update.img" "$OUTPUT_DIR/update.img" -os_type:androidos 2>&1 | head -5
if [ ! -f "$OUTPUT_DIR/update.img" ]; then
    echo "错误: rkImageMaker 失败"
    exit 1
fi
echo "  -> update.img $(stat -c %s "$OUTPUT_DIR/update.img") bytes"

#--------------------------------------------------------------------
# 清理
#--------------------------------------------------------------------
rm -rf "$WORK_DIR" "$PACK_DIR"

echo ""
echo "========================================"
echo "  打包完成"
echo "========================================"
echo ""
echo "  输出文件:"
ls -lh "$OUTPUT_DIR"/update.img "$OUTPUT_DIR"/MiniLoaderAll.bin "$OUTPUT_DIR"/uboot.img "$OUTPUT_DIR"/boot.img "$OUTPUT_DIR"/boot.fit 2>/dev/null
echo ""
echo "  A/B 烧录/升级方法 (Rockchip upgrade_tool):"
echo "    1. 首刷/全量: 开发板进 Loader 模式 (短接 RECOVERY + 插 USB)"
echo "       sudo upgrade_tool uf $OUTPUT_DIR/update.img"
echo "       (boot_a/boot_b 同镜像, misc 清零 -> U-Boot 启动 boot_a)"
echo "    2. 单槽升级 (板上 ota update 流程之外的手动方式):"
echo "       sudo upgrade_tool di boot_b $OUTPUT_DIR/boot.fit && reboot 后 ota confirm"
echo "    3. 板上 OTA (推荐): 拷贝 boot.fit 到 /data 后"
echo "       nsh> ota update /data/boot.fit && nsh> reboot"
echo "       新槽首启后 nsh> ota status 查看, nsh> ota confirm 固化"
