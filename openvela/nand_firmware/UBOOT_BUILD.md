# 自建支持 A/B 的 U-Boot 构建方法 (v6)

## 背景

预编译 `SDK/u-boot/fit/uboot.itb` (md5 17cd4423…, 813568B) **未开 `CONFIG_ANDROID_AB`**
（`configs/rk3506_defconfig` 默认 n，且 `rk3506_tb.config` fragment 只关 SPL AB），
导致 `boot_fit` → `part_get_info_by_name("boot")` 只做字面量查找：
v5 分区表把 `boot` 改名 `boot_a/boot_b` 后 U-Boot 找不到任何分区，
启动卡死在 `FIT: No boot partition`（2026-08-30 早晨实机复现）。

## 方案

用 SDK 自带 U-Boot 源码（HEAD=e35b8af，与预编译 banner 一致）+ Rockchip 官方
AB fragment 配方（参考 `configs/rk3588-ab.config`）重编，只加 A/B 能力，
其余配置与预编译完全一致（DTB hash 逐字节相同，TEE 取自 rkbin 同一文件）。

## 步骤（不改 SDK 仓库，全部在拷贝的工作区进行）

```bash
SDK=/home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm
WS=/home/b4qaq/project/uboot_build

# 1. 拷出源码 + 链接 rkbin (make.sh 依赖 ../rkbin 相对路径)
mkdir -p $WS && cp -a $SDK/u-boot $WS/u-boot
ln -sfn $SDK/rkbin $WS/rkbin

# 2. AB + AMP fragment (Rockchip 官方 AB 参考配置的最小集 + AMP 支持)
cat > $WS/u-boot/configs/vanxoak_ab.config <<'EOF'
CONFIG_ANDROID_AB=y
CONFIG_AVB_LIBAVB=y
CONFIG_AVB_LIBAVB_AB=y
CONFIG_AVB_LIBAVB_ATX=y
CONFIG_AVB_LIBAVB_USER=y
CONFIG_RK_AVB_LIBAVB_USER=y
CONFIG_AMP=y
CONFIG_ROCKCHIP_AMP=y
EOF

# 3. 构建 + FIT 打包 (fragment 顺序: defconfig -> tb -> ab)
TC=$SDK/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
cd $WS/u-boot
PATH=$WS/u-boot/scripts/dtc:$PATH \
  ./make.sh CROSS_COMPILE=$TC rk3506 rk3506_tb vanxoak_ab --spl-new

# 4. 产物
ls -la fit/uboot.itb   # -> 拷到 openvela/nand_firmware/uboot.itb
```

### 为什么加 CONFIG_AMP / CONFIG_ROCKCHIP_AMP (v8m)

M0 固件改由 U-Boot 加载放行（SDK 流程）：M0 固件打包成 standalone
FIT（`amp/amp.img`，load=0xfff84000）烧在独立的 `amp` GPT 分区。
U-Boot 在 `board_late_init` 里 `amp_cpus_on()`
（`drivers/cpu/rockchip_amp.c`）读出该分区，`boot_get_loadable`
在 M0 复位态拷入 SRAM，再调 `fit_standalone_release()`
（`arch/arm/mach-rockchip/rk3506/rk3506.c`，SMC TCM 映射 →
CRU_GATE_CON5 → GRF_SOC_CON36 → PMU_INT_MASK_CON）放行。
NuttX 不再内嵌/拷贝/启动 M0。`amp/amp.img` 由
`nand_firmware/build_mcu_fw.sh` 从 SDK HAL 的 TestDemo.bin 生成。

## 踩过的坑

1. **只加 `CONFIG_ANDROID_AB=y` 会链接失败**：该开关只编译"引用方"
   (`android_ab.c`/`part.c`/`boot_android.c`)，符号提供方 AVB 库由
   `AVB_LIBAVB*/RK_AVB_LIBAVB_USER` 控制（即预编译二进制里
   "Please enable CONFIG_RK_AVB_LIBAVB_USER" 运行时报错的出处）。
   必须按 rk3588-ab.config 全开。
2. **make.sh 要求 PATH 里有 `dtc`**：U-Boot 2026.09 树内会编出
   `scripts/dtc/dtc`，把它所在目录加进 PATH 即可，不用装系统包。
3. **拷贝树里的 `.git` 不完整**（指向失效），git 会向上解析到项目主仓库——
   已删除拷贝内 .git，版本串降级为 `2017.09 (Aug 30 2026 - ...)`，无功能影响。
4. `fit-core.sh` 里 `fdtget: command not found` 只影响版本号打印（fit_msg），
   不影响 uboot.itb 生成。

## 打包接入

`pack_nand.sh` 的 U-Boot 来源优先级（3.3 环境变量约定）：
`UBOOT_IMG 环境变量` > `nand_firmware/uboot.itb`（自建 AB 版，随仓库提交）>
`SDK fit/uboot.itb`（预编译，无 AB，只认字面 `boot` 分区）。

## 验证

- 二进制内 AB 字符串核对（`strings fit/uboot.itb | grep A/B-slot` 应有 1 处）
- `mkimage -l` 对比新旧 itb：结构一致、DTB hash 相同、均无 Signature 段
- 上电验证：U-Boot 打印 `A/B-slot: _a, successful: 0, tries-remain: 7` 后进 NSH
