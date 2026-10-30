# mcu_sdk — RK3506 Cortex-M0 (bus MCU) 固件构建环境

本目录 vendored 了 Rockchip RK3506 Linux SDK 的 `hal/` 子树，用于在**不依赖外部 SDK** 的情况下编译 M0 的 rpmsg 测试固件。

## 来源

- 上游：`rk3506_linux6.1_sdk_v1.2.0_iot_evm/hal/`
- 只复制了 `hal/`（CMSIS + HAL 驱动 + rpmsg-lite + rk3506-mcu 工程 + mkimage），未复制 SDK 其余部分（kernel/u-boot/prebuilts 等，M0 编译用不到）。
- **本目录是可直接编辑的源码树**（不再是只读参考）：M0 工程需要的改动已经直接提交在这里，`build_mcu_fw.sh` 只负责编译和打包，不再用 sed/awk 打补丁。

## 目录

- `hal/lib/CMSIS/Device/RK3506/Include/` — 寄存器头文件 `rk3506.h` / `soc.h`
- `hal/lib/hal/src|inc/` — HAL 驱动（mailbox / intmux / uart / timer / pinctrl ...）
- `hal/lib/bsp/RK3506/` — `g_uart0..5Dev` 等设备实例
- `hal/middleware/rpmsg-lite/` — rpmsg-lite + RK3506 平台适配
- `hal/project/rk3506-mcu/` — M0 固件工程（`src/main.c`、`src/test_demo.c`、`GCC/` 编译脚本、`Image/amp.its`）
- `hal/tools/mkimage` — 打包 amp.img 用的 mkimage

## 构建

```bash
cd <仓库根>
bash openvela/nand_firmware/build_mcu_fw.sh
```

脚本会先自检下面「当前 M0 固件配置」的 5 项是否都还在（缺一个就报错退出），然后编译并打包。

产物：
- `nand_firmware/amp/amp.img` — U-Boot 从 GPT 的 `amp` 分区读出并放行 M0 的 standalone FIT（load=0x03a00000）
- `nand_firmware/amp/mcu.bin` — 原始 M0 镜像

## 当前 M0 固件配置（相对上游，已提交在本目录）

对齐 Linux SDK 已验证的 AMP 方案（见仓库根 `AMP_RK3506_LINUX_SDK_NOTES.md`）：

1. **`main.c`**: `TEST_DEMO` 打开
2. **`test_demo.c`**: `RPMSG_LINUX_TEST` 打开（MASTER_ID=0 / REMOTE_ID=3，端点 `rpmsg-mcu0-test` ept 0x4003）
3. **`main.c`**: 调试串口 **UART1**（`pUart = UART1`、`g_uart1Dev`、RM_IO `BANK0/PIN_C1=TX / PIN_C0=RX`），115200 8N1
4. **`GCC/Makefile`**: `LINKER_SCRIPT := gcc_bus_m0_ddr.ld`
5. **`GCC/gcc_bus_m0_ddr.ld`**: `RAM ORIGIN = 0x03a00000, LENGTH = 1MB`（DRAM，不是 32KB 的 SRAM）

配套的 A7 侧配置：
- `vendor/rockchip/chips/rk3506/rk3506_rptun.c` 的 `rk3506_mcu_clock_enable()` 打开 UART1 时钟（CRU `CLKGATE_CON11` bit5=PCLK / bit10=SCLK）
- 板级 `scripts/ld.script` 的 RAM 结束于 0x03a00000，把 0x03a00000-0x03e00000 四个 1MiB 窗口全部让给 AMP

## 为什么用 DRAM 而不是 SRAM

M0 的 SRAM 只有 32 KiB，`TestDemo.bin` 已经 27 KB，加上栈/rpmsg 缓冲后没有余量，加任何应用都会栈溢出。
DRAM 方案在 M0 加载窗口 1 MiB，与 Linux SDK 的验证结果一致。

三处地址必须一致（改动时同时改）：
| 出处 | 字段 | 值 |
|---|---|---|
| `Image/amp.its` | `load` | `0x03a00000` |
| `GCC/gcc_bus_m0_ddr.ld` | `RAM ORIGIN` | `0x03a00000` |
| 板级 `scripts/ld.script` | RAM 结束地址 | `0x03a00000`（即 NuttX 堆不越过） |

`arm-none-eabi-readelf -h TestDemo.elf` 的 Entry point 应在 `0x03a0xxxx`（Thumb 态带 bit0 属正常）。

## 调试串口接线

M0 打印走 **UART1**，引脚由 RM_IO 映射到 **GPIO0_C1 (TX) / GPIO0_C0 (RX)**，115200 8N1。
接 USB-TTL 时 TX/RX 交叉，GND 共地。
