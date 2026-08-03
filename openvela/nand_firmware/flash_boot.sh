#!/bin/bash
#
# openvela NAND 烧录脚本
# 支持单独烧录 boot/system/data 分区或完整烧录
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_DIR="/home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm"
UPGRADE_TOOL="$SDK_DIR/tools/linux/Linux_Upgrade_Tool/Linux_Upgrade_Tool/upgrade_tool"

echo "========================================"
echo "  openvela NAND 烧录工具"
echo "========================================"
echo ""
echo "分区布局:"
echo "  boot      - NuttX 内核 (FIT 镜像)"
echo "  system    - 系统分区 (ROMFS)"
echo "  data      - 应用数据分区"
echo "  vendor    - 厂商定制分区"
echo "  oem       - OEM 分区"
echo ""
echo "烧录选项:"
echo "  1) 只烧 boot 分区 (最快验证)"
echo "  2) 烧 boot + system 分区"
echo "  3) 烧 boot + system + data 分区"
echo "  4) 完整烧录 (update.img)"
echo "  5) 自定义选择分区"
echo ""
read -p "请选择 [1-5]: " choice

case $choice in
    1)
        PARTITIONS=("boot")
        ;;
    2)
        PARTITIONS=("boot" "system")
        ;;
    3)
        PARTITIONS=("boot" "system" "data")
        ;;
    4)
        echo ""
        echo "完整烧录需要 update.img"
        if [ -f "$SCRIPT_DIR/update.img" ]; then
            echo "找到 update.img: $(du -h "$SCRIPT_DIR/update.img" | cut -f1)"
            echo ""
            read -p "按 Enter 继续烧录..."
            $UPGRADE_TOOL uf "$SCRIPT_DIR/update.img"
            echo ""
            echo "烧录完成，重启开发板..."
            $UPGRADE_TOOL rd
            exit 0
        else
            echo "错误: update.img 不存在"
            echo "请先运行 pack_nand.sh 生成"
            exit 1
        fi
        ;;
    5)
        echo ""
        echo "可选分区: boot system data vendor oem"
        read -p "输入分区名 (空格分隔): " -a PARTITIONS
        ;;
    *)
        echo "无效选择"
        exit 1
        ;;
esac

echo ""
echo "即将烧录以下分区:"
for part in "${PARTITIONS[@]}"; do
    img="$SCRIPT_DIR/${part}.img"
    if [ -f "$img" ]; then
        echo "  $part -> $(du -h "$img" | cut -f1)"
    else
        echo "  $part -> [缺失] $img"
    fi
done

echo ""
echo "请确保开发板已进入 Loader 模式:"
echo "  1. 短接 RECOVERY 排针"
echo "  2. 插入 USB Type-C 到 USB Device 口"
echo "  3. 等待工具识别到设备后松开短接"
echo ""
read -p "按 Enter 继续，或 Ctrl+C 取消..."

for part in "${PARTITIONS[@]}"; do
    img="$SCRIPT_DIR/${part}.img"
    if [ -f "$img" ]; then
        echo ""
        echo "烧录 $part ..."
        $UPGRADE_TOOL di "$part" "$img"
    else
        echo ""
        echo "跳过 $part (文件不存在)"
    fi
done

echo ""
echo "烧录完成，重启开发板..."
$UPGRADE_TOOL rd

echo ""
echo "========================================"
echo "  烧录完成！"
echo "========================================"
echo ""
echo "UART0 (115200) 应输出 NuttX 启动日志。"
echo ""
echo "恢复方法:"
echo "  1. 重新进入 Loader 模式"
echo "  2. 烧回原始固件: $UPGRADE_TOOL uf <原始update.img>"
