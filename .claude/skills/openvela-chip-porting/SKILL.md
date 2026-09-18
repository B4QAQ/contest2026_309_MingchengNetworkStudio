---
name: openvela-chip-porting
description: "新芯片移植到 openvela (NuttX) 的完整流程。Use when: 移植新芯片、新板子、BSP 适配、bringup、芯片驱动开发、硬件适配、新硬件适配赛道。"
---

# openvela Chip Porting

基于 RK3506G2 完整移植经验（116 个提交，v8a→v8l）总结的芯片移植方法论。

**核心原则**: 参考 Linux SDK 寄存器定义 + 已有 NuttX 移植骨架，不凭记忆猜测。

---

## 1. 移植前准备

### 1.1 参考资料优先级

| 优先级 | 来源 | 用途 |
|--------|------|------|
| 1 | **Linux SDK** | 寄存器定义、时钟树、pinctrl、PHY 驱动 |
| 2 | **已有 NuttX 同架构移植** | 代码骨架、驱动模式、Kconfig 结构 |
| 3 | **芯片数据手册** | 寄存器位域、时序图、电气特性 |
| 4 | **开发板原理图** | 引脚分配、外设连接、电源域 |

**RK3506 参考路径**:
- Linux SDK: `RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/`
  - 寄存器: `hal/lib/CMSIS/Device/RK3506/Include/rk3506.h`
  - HAL 驱动: `hal/lib/hal/src/<periph>.c`
  - 时钟: `kernel-6.1/drivers/clk/rockchip/clk-rk3506.c`
  - Pinctrl: `u-boot/arch/arm/dts/rk3506-pinctrl.dtsi`
- NuttX 参考: `openvela/vendor/allwinnertech/` (R258 Cortex-A7)

### 1.2 环境搭建

```bash
# 1. 克隆 openvela
repo init -u <manifest_url> -b <branch>
repo sync -c -j8

# 2. 配置 ccache
export CCACHE_DIR=/tmp/ccache_dir

# 3. 构建命令模板
cd openvela
rm -rf cmake_out/<board>_nsh
CCACHE_DIR=/tmp/ccache_dir \
PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
./build.sh vendor/<vendor>/boards/<chip>/<board>/configs/nsh/ --cmake -j$(nproc)
```

---

## 2. BSP 目录结构

```
vendor/<vendor>/boards/<chip>/<board>/
├── CMakeLists.txt          # 板级源文件注册
├── Kconfig                 # 板级配置选项
├── configs/
│   └── nsh/
│       └── defconfig       # 默认配置
├── include/
│   └── board.h             # 板级头文件
├── scripts/
│   ├── Make.defs           # 构建系统集成
│   ├── ld.script           # 链接脚本
│   └── package.sh          # 打包脚本
└── src/
    ├── CMakeLists.txt      # 源文件列表
    ├── <board>_boardinit.c # 板级初始化
    ├── <board>_appinit.c   # 应用初始化
    ├── <board>_bringup.c   # 外设探测
    └── <driver>.c          # 板级驱动

vendor/<vendor>/chips/<chip>/
├── CMakeLists.txt
├── Kconfig
├── Make.defs
├── chip.h                  # 芯片级定义
├── hardware/               # 寄存器定义
├── include/                # 芯片级头文件
└── <peripheral>.c          # 外设驱动
```

---

## 3. 最小启动（Phase 1）

目标：串口输出 `nsh>` 提示符。

### 3.1 链接脚本关键配置

```ld
MEMORY
{
  flash (rx)  : ORIGIN = 0x02080000, LENGTH = 16M
  sram  (rwx) : ORIGIN = 0x02000000, LENGTH = 512K
}
__stack_size = 8K;
__heap_size = 1M;
```

### 3.2 defconfig 最小集

```ini
CONFIG_ARCH="arm"
CONFIG_ARCH_ARM=y
CONFIG_ARCH_CHIP="<chip>"
CONFIG_ARCH_BOARD="<board>"
CONFIG_RAM_START=0x02080000
CONFIG_RAM_SIZE=134217728  # 128MB
CONFIG_UART0_SERIAL_CONSOLE=y
CONFIG_SERIAL_CONSOLE="ttyS0"
CONFIG_INIT_ENTRYPOINT="nsh_main"
```

### 3.3 常见启动问题

| 现象 | 原因 | 解决方案 |
|------|------|----------|
| 无串口输出 | 时钟配置错误 | 检查 UART 时钟源和分频 |
| HardFault | 内存映射错误 | 检查 MPU 配置和链接脚本 |
| 卡在启动 | 外设初始化死循环 | 检查时钟门控和复位状态 |

---

## 4. 外设驱动开发（Phase 2）

### 4.1 驱动开发顺序

按依赖关系和验证难度排序：

1. **GPIO** - 最简单，LED 闪烁验证
2. **UART** - 串口控制台（已在 Phase 1 完成）
3. **I2C** - 传感器、触摸屏
4. **SPI** - Flash、显示屏
5. **网络** - 以太网 MAC/PHY
6. **USB** - 主机/设备模式
7. **显示** - LCD 控制器、帧缓冲
8. **音频** - I2S、编解码器

### 4.2 寄存器操作规范

```c
// 使用 NuttX 标准寄存器访问函数
#include <nuttx/arch.h>

// 32 位寄存器
putreg32(value, base + REG_OFFSET);
value = getreg32(base + REG_OFFSET);

// 位域操作（Rockchip HIWORD_UPDATE 模式）
#define HIWORD_UPDATE(val, mask, shift) \
  ((val) << (shift) | (mask) << ((shift) + 16))

putreg32(HIWORD_UPDATE(1, 1, BIT_POS), grf_base + GRF_REG);
```

**⚠️ 真实案例 - 时钟门控写反 (v8e)**:
```c
// 错误：写 1 实际是关时钟（SET_TO_DISABLE）
putreg32(BIT_CLK, cru + CLK_REG);  // 实际关了时钟！

// 正确：写 0 开时钟
putreg32(BIT_CLK << 16, cru + CLK_REG);  // 高 16 位是写使能
```

### 4.3 日志宏使用规范

**⚠️ 真实案例 - 日志宏误用 (v8c)**:
```c
// 错误：使用 INPUT 子系统专用宏（CONFIG_DEBUG_INPUT_* 门控）
ierr("error\n");  // 全部静默！
iwarn("warning\n");
iinfo("info\n");

// 正确：使用通用日志宏
_err("error\n");  // CONFIG_DEBUG_ERROR
_warn("warning\n");  // CONFIG_DEBUG_WARN
_info("info\n");  // CONFIG_DEBUG_INFO
```

---

## 5. 网络驱动（Phase 3）

### 5.1 GMAC 驱动架构

```
应用层 (curl, ping)
    ↓
NuttX 网络栈 (TCP/IP)
    ↓
netdev 接口 (net_driver_s)
    ↓
GMAC 驱动 (你的代码)
    ↓
DMA 描述符 + MAC 寄存器
    ↓
PHY (MII/RMII)
```

### 5.2 网络调试真实案例

**案例 1: DHCP 失败 (v8a)**:
```
原因: GMAC0 TX 描述符 TDES3[14:0] 帧长未填
修复: 补充帧长字段
```

**案例 2: RX 全部 CRC 错 (v8a)**:
```
原因: YT8512B PHY 未配置 RMII_EN + PLL refclk
修复: 补充 PHY vendor 特定配置
```

**案例 3: 无法接收包 (v8a)**:
```
原因: MAC 地址过滤未设置 AE 位
修复: PACKET_FILTER.PM 补 AE 位
```

**案例 4: DHCP 超时重连 (v8l)**:
```
原因: 网线拔插后立即 DHCP，PHY 未就绪
修复: link-up 后延时 2-3 秒 + 重试 5 次
```

### 5.3 网络配置最佳实践

```c
// netcfg 守护进程模式
// 1. 启动时读取 /etc/net.conf
// 2. 注册 SIOCMIINOTIFY 信号
// 3. link-up 时自动重试 DHCP

// 配置示例 (/etc/net.conf)
mode=dhcp
auto_dhcp=1
```

---

## 6. USB 驱动（Phase 4）

### 6.1 DWC2 控制器配置

**⚠️ 真实案例 - FIFO 大小卡死 (v8d)**:
```c
// 错误：默认 128 words，HS bulk mps=512B 需要 ~131 words
#define DWC2_GRXFSIZ   128   // 太小！

// 正确：按 SDK 值配置
#define DWC2_GRXFSIZ   512   // 接收 FIFO
#define DWC2_GNPTXFSIZ 256   // 非周期 TX FIFO
#define DWC2_HPTXFSIZ  224   // 周期 TX FIFO
```

**案例: USB 枚举过但 bulk 传输死 (v8d)**:
```
现象: U 盘枚举成功，但读写卡死
根因: RX FIFO 太小，核心静默拒绝发 token
修复: 提到 SDK 值 512/256/224
```

### 6.2 USB Host 架构

```
USB 设备 (U盘/键盘)
    ↓
USB Host Controller (DWC2)
    ↓
usbhost 驱动
    ↓
块设备 (/dev/sda) 或 HID
    ↓
文件系统 (FAT) 或 应用
```

---

## 7. 显示驱动（Phase 5）

### 7.1 LCD 控制器 (VOP)

```c
// 时序配置
struct lcd_timing_s {
  uint32_t hactive;   // 水平有效像素
  uint32_t hfp;       // 水平前肩
  uint32_t hbp;       // 水平后肩
  uint32_t hsync;     // 水平同步
  uint32_t vactive;   // 垂直有效行
  uint32_t vfp;       // 垂直前肩
  uint32_t vbp;       // 垂直后肩
  uint32_t vsync;     // 垂直同步
};
```

### 7.2 RGB LCD 初始化

```c
// 3-wire SPI 初始化序列（ST7701S 等）
static void lcd_send_cmd(uint8_t cmd)
{
  // 片选拉低
  // 发送命令字节
  // 片选拉高
}
```

---

## 8. 调试技巧

### 8.1 串口日志

```c
// 使用 NuttX 日志宏（不要用 printf）
#include <nuttx/config.h>
#include <debug.h>

_err("error: %d\n", errno);    // CONFIG_DEBUG_ERROR
_warn("warning: %d\n", val);  // CONFIG_DEBUG_WARN
_info("info: %d\n", status);  // CONFIG_DEBUG_INFO
```

**重要**: 不要用 `ierr/iwarn/iinfo`，那是 input 子系统专用宏！

### 8.2 常用调试命令

```bash
nsh> free          # 内存分布
nsh> ps            # 进程列表
nsh> ifconfig      # 网络接口
nsh> route         # 路由表
nsh> ping <ip>     # 网络连通性
nsh> date          # 系统时间
```

### 8.3 问题排查流程

```
现象
  ↓
1. 确认现象可复现
  ↓
2. 检查最简单原因
   - 电源？时钟？复位？
  ↓
3. 添加诊断日志
   - 寄存器值
   - 函数调用链
  ↓
4. 对比参考代码
   - Linux SDK 怎么做的？
   - 其他 NuttX 移植怎么做的？
  ↓
5. 二分法定位
   - 禁用部分功能
   - 简化测试用例
  ↓
6. 硬件验证
   - 示波器看信号
   - 逻辑分析仪看协议
```

---

## 9. Kconfig 集成

### 9.1 芯片级 Kconfig

```kconfig
config ARCH_CHIP_<CHIP>
    bool "<Chip> series"
    select ARMV7A
    ---help---
        <Chip> Cortex-A7 processor.

if ARCH_CHIP_<CHIP>

config <CHIP>_UART0
    bool "UART0"
    default y

endif # ARCH_CHIP_<CHIP>
```

### 9.2 板级 Kconfig

```kconfig
config <BOARD>_NETCFG
    bool "netcfg network configuration daemon"
    default n
    depends on NET
    ---help---
        Enable the netcfg NSH builtin.
```

### 9.3 defconfig 规范

```ini
# 每个配置项加注释说明用途
CONFIG_<BOARD>_NETCFG=y
CONFIG_NETUTILS_DHCPC_RETRIES=10  # STP 等待时间
```

---

## 10. 打包与烧录

### 10.1 打包脚本

```bash
#!/bin/bash
# nand_firmware/pack_nand.sh

export PARAM_FILE="parameter.txt"
export NUTTX_BIN="../cmake_out/<board>_nsh/nuttx.bin"

# 调用 Rockchip 打包工具
./rkImageMaker -ARM:RK3506 MiniLoaderAll.bin \
  -OUT:MiniLoaderAll.bin.pack

# 生成 update.img
./afptool -pack ./ update.img
```

### 10.2 烧录方式

```bash
# 1. Loader 模式（首次）
sudo upgrade_tool lf  # 检查 Loader
sudo upgrade_tool uf update.img

# 2. OTA 模式（板上）
nsh> ota update /data/boot.fit
nsh> reboot
nsh> ota confirm  # 确认启动成功
```

---

## 11. 质量检查清单

### 11.1 编译检查

- [ ] 清理构建通过：`rm -rf cmake_out/<board>_nsh && ./build.sh ...`
- [ ] 无 warning（或已知 warning 已记录）
- [ ] 产物存在：`vela.bin`、`update.img`

### 11.2 功能检查

- [ ] NSH 启动正常
- [ ] `free` 命令显示正确内存
- [ ] `ps` 命令显示正常进程
- [ ] 网络 ping 通
- [ ] 文件系统可读写

### 11.3 驱动检查

- [ ] 每个驱动有 Kconfig 选项
- [ ] 每个驱动在 Make.defs 和 CMakeLists.txt 注册
- [ ] 寄存器操作封装为 putreg32/getreg32
- [ ] 错误路径用 _err/_warn/_info 日志

### 11.4 代码规范

- [ ] 符合 NuttX 编码风格
- [ ] 无硬编码绝对路径
- [ ] 路径用 `${NUTTX_BOARD_ABS_DIR}` 等变量
- [ ] 交叉工具链用 `find_program(... NO_DEFAULT_PATH)`

---

## 12. 常见陷阱（基于真实案例）

### 12.1 时钟门控

**问题**: 外设不工作，但寄存器写入正常。

**原因**: 时钟门控未使能。Rockchip 等 SoC 使用 SET_TO_DISABLE 模式：

```c
// 错误：写 1 开时钟
putreg32(BIT_CLK, cru + CLK_REG);  // 实际是关时钟！

// 正确：写 0 开时钟（SET_TO_DISABLE）
putreg32(BIT_CLK << 16, cru + CLK_REG);  // 高 16 位是写使能，低 16 位是值
```

**真实案例 (v8e)**: `rk3506_rptun mbox 时钟门控写反, PCLK_MAILBOX 被关死致 rpmsg 全链路失效`

### 12.2 日志宏误用

**问题**: 驱动日志全部静默。

**原因**: 使用了 INPUT 子系统专用宏 `ierr/iwarn/iinfo`（CONFIG_DEBUG_INPUT_* 门控，本工程未开）。

```c
// 错误：使用 INPUT 子系统宏
ierr("error\n");  // 全部静默！

// 正确：使用通用日志宏
_err("error\n");
```

**真实案例 (v8c)**: `驱动日志宏误用 INPUT 子系统 ierr/iwarn/iinfo, 全部静默`

### 12.3 USB FIFO 大小

**问题**: USB 枚举成功，但 bulk 传输卡死。

**原因**: DWC2 RX FIFO 太小。HS bulk mps=512B 需要 ~131 words，但默认 128 words。

```c
// 错误：默认值
#define DWC2_GRXFSIZ   128   // 太小！

// 正确：按 SDK 值
#define DWC2_GRXFSIZ   512
#define DWC2_GNPTXFSIZ 256
#define DWC2_HPTXFSIZ  224
```

**真实案例 (v8d)**: `DWC2 RX FIFO 默认 128 words 卡死 HS bulk, 提到 SDK 值 512/256/224`

### 12.4 共享内存映射

**问题**: rpmsg 通信失败。

**原因**: 共享内存窗口使用 cached 映射，导致数据不一致。

```c
// 错误：cached 映射
mmap(..., PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);

// 正确：uncached 映射
mmap(..., PROT_READ | PROT_WRITE, MAP_SHARED | MAP_UNCACHED, fd, offset);
```

**真实案例 (v8b)**: `rpmsg 共享窗改 uncached 映射 (根因)`

### 12.5 网络 DHCP 超时

**问题**: 网线拔插后 DHCP 失败。

**原因**: link-up 后立即 DHCP，但 PHY 未就绪。

```c
// 错误：立即 DHCP
if (netcfg_wait_link_change()) {
  netcfg_run_dhcp();  // PHY 可能未就绪
}

// 正确：延时 + 重试
if (netcfg_wait_link_change()) {
  usleep(2000 * 1000);  // 等 2 秒
  for (i = 0; i < 5; i++) {
    if (netcfg_run_dhcp() >= 0) break;
    usleep(3000 * 1000);  // 重试间隔 3 秒
  }
}
```

**真实案例 (v8l)**: `netcfg add 2s link-settle delay + 5x DHCP retry for cable replug`

### 12.6 curl 挂死

**问题**: curl 任何命令都卡死，需 Ctrl+C。

**原因**: curl 的 TCP socketpair 在 loopback 未配置时 accept 永久阻塞。

```c
// curl socketpair.c 的 TCP 回退实现
// 1. 创建 listener 绑定 127.0.0.1:随机端口
// 2. connect 连到自己
// 3. accept 等待连接
// 如果 loopback 未配置，connect 失败，accept 永久阻塞

// 修复：禁用 socketpair
#define CURL_DISABLE_SOCKETPAIR 1
```

**真实案例 (v8l)**: `curl hang root cause: TCP socketpair accept blocks forever when loopback not configured`

### 12.7 文件系统选择

**问题**: SmartFS 挂载失败 (mount failed: 25)。

**原因**: SmartFS 需要 SMART 层 BIOC_GETFORMAT ioctl，但 dhara 块设备不支持。

```c
// 错误：dhara + SmartFS
mount -t smartfs /dev/mtdblock0 /data  // ENOTTY

// 正确：dhara + littlefs
mount -t littlefs -o autoformat /dev/mtdblock0 /data
```

**真实案例 (v8)**: `/data 切换 dhara+littlefs, 修复 mount failed:25`

---

## 13. 性能优化

### 13.1 启动时间优化

- 使用 XIP（Execute In Place）从 Flash 直接执行
- 延迟初始化非关键外设
- 使用 `CONFIG_BOARD_LATE_INITIALIZE=y`

### 13.2 运行时优化

- 启用 D-cache 和 I-cache
- 使用 DMA 代替 CPU 搬运
- 优化中断处理（底半部处理）

### 13.3 内存优化

- 使用 `CONFIG_MM_SMALL=y` 减少内存管理开销
- 合理设置栈大小（避免过大）
- 使用静态分配代替动态分配

---

## 14. 参考资源

### 14.1 NuttX 官方文档

- [NuttX Porting Guide](https://nuttx.apache.org/docs/latest/guides/porting.html)
- [NuttX Driver Framework](https://nuttx.apache.org/docs/latest/components/drivers.html)

### 14.2 openvela 文档

- `OpenVelaDocs/zh-cn/chip_porting/` - 芯片移植指南
- `openvela/docs/` - 开发文档

### 14.3 调试工具

- `arm-none-eabi-gdb` - 源码级调试
- `OpenOCD` - JTAG/SWD 调试
- `Logic Analyzer` - 信号分析
- `Oscilloscope` - 时序验证

---

## 15. 移植里程碑模板

| 里程碑 | 目标 | 验收标准 |
|--------|------|----------|
| M1 | 最小启动 | NSH 提示符 |
| M2 | 基础外设 | GPIO/UART/I2C 工作 |
| M3 | 网络 | ping/DHCP/curl |
| M4 | 存储 | SPI NAND/文件系统 |
| M5 | 显示 | LCD 显示图片 |
| M6 | USB | U 盘挂载 |
| M7 | 音频 | 播放/录音 |
| M8 | 完整 BSP | 所有外设工作 |

---

## 16. 总结

芯片移植的关键成功因素：

1. **参考资料充足** - Linux SDK + 已有 NuttX 移植
2. **分阶段验证** - 先最小启动，再逐步添加功能
3. **日志完善** - 关键路径都要有日志
4. **代码规范** - 遵循 NuttX 编码风格
5. **测试充分** - 每个功能都要验证
6. **文档完整** - 记录遇到的问题和解决方案

**记住**: 移植不是一次性完成的，是迭代过程。每次迭代都要：
1. 编译
2. 烧录
3. 测试
4. 记录问题
5. 修复
6. 重复

---

*基于 RK3506G2 移植经验（116 个提交，v8a→v8l）总结，适用于 Cortex-A7/A53/RISC-V 等架构的 openvela 移植。*