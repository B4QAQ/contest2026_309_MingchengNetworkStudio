# mcu_sdk — RK3506 Cortex-M0 (bus MCU) 固件构建环境

本目录 vendored 了 Rockchip RK3506 Linux SDK 的 `hal/` 子树，用于在**不依赖外部 SDK** 的情况下编译 M0 的 rpmsg 测试固件。

## 来源

- 上游：`rk3506_linux6.1_sdk_v1.2.0_iot_evm/hal/`
- 只复制了 `hal/`（CMSIS + HAL 驱动 + rpmsg-lite + rk3506-mcu 工程 + mkimage），未复制 SDK 其余部分（kernel/u-boot/prebuilts 等，M0 编译用不到）。
- 内容是**只读参考**：M0 工程的改动（打开 `TEST_DEMO`/`RPMSG_LINUX_TEST`、把调试串口从 UART4 移到 J9 UART2）都在 `../build_mcu_fw.sh` 里以 sed/awk 补丁形式打到 `.mcu_build/` 临时副本上，**不改本目录源码**。

## 目录

- `hal/lib/CMSIS/Device/RK3506/Include/` — 寄存器头文件 `rk3506.h` / `soc.h`
- `hal/lib/hal/src|inc/` — HAL 驱动（mailbox / intmux / uart / timer / pinctrl ...）
- `hal/lib/bsp/RK3506/` — `g_uart0..5Dev` 等设备实例
- `hal/middleware/rpmsg-lite/` — rpmsg-lite + RK3506 平台适配
- `hal/project/rk3506-mcu/` — M0 固件工程（`src/main.c`、`src/test_demo.c`、`GCC/` 编译脚本）
- `hal/tools/mkimage` — 打包 amp.img 用的 mkimage

## 构建

```bash
cd <openvela 根>
bash nand_firmware/build_mcu_fw.sh     # 自动用 ./mcu_sdk，无需 RK3506_SDK_DIR
```

产物：`nand_firmware/amp/amp.img`（U-Boot 从 GPT 的 `amp` 分区读出并放行 M0 的 standalone FIT）+ `nand_firmware/amp/mcu.bin`（原始 M0 镜像）。

## 当前 M0 固件改动（相对上游）

在 `build_mcu_fw.sh` 中以补丁形式应用，**不直接改本目录**：

1. `main.c`: `//#define TEST_DEMO` → `#define TEST_DEMO`
2. `test_demo.c`: `//#define RPMSG_LINUX_TEST` → `#define RPMSG_LINUX_TEST`
3. `main.c`: 调试串口 UART4 → **UART2**（`pUart`、`g_uart2Dev`、RM_IO 改 `BANK0/PIN_A2=TX / PIN_A0=RX`）
   - 目的：UART4 在核心板邮票孔 pin100/101（未引出），UART2 在底板 J9 pin26(TX)/pin25(RX)，可接 USB-TTL 看 M0 日志（1500000 8N1）。
   - 配套：A7 侧 `vendor/rockchip/chips/rk3506/rk3506_rptun.c` 的 `rk3506_mcu_clock_enable()` 把 CRU `CLKGATE_CON11` 从 UART4(b8/b13) 改为 UART2(b6/b11) 开门。

## 已知问题（求助点）

**M0 UART2 串口烧录后无输出**（连 `Hello RK3506 mcu` 都没有）。A7 侧 rpmsg 现象：M0 武装了 mailbox3（INTMUX en3 bit21=1）、A7 的 kick 被 M0 消费（A2B_STATUS 被清），但 M0 不回 B2A、virtqueue 不前进、`rpmsgtest` 超时。怀疑 M0 未真正执行 / 卡在 rpmsg init / UART2 时钟或引脚未配对。相关诊断代码在 `vendor/rockchip/chips/rk3506/rk3506_rptun.c`（`rk3506_rptun_dump`、`kick_consumed_us`、B2A IRQ 日志）。
