---
name: openvela-chip-porting
description: "新芯片移植到 openvela (NuttX) 的完整流程。Use when: 移植新芯片、新板子、BSP 适配、bringup、芯片驱动开发、硬件适配、新硬件适配赛道。"
---

# openvela Chip Porting

基于 RK3506G2 完整移植经验总结的芯片移植方法论。涵盖从零到完整 BSP 的全流程。

## 适用场景

- 新芯片首次移植到 openvela
- 新开发板适配已有芯片
- 驱动开发与调试
- 硬件适配赛道参赛

---

## 1. 移植前准备

### 1.1 收集参考资料（优先级顺序）

| 优先级 | 来源 | 用途 |
|--------|------|------|
| 1 | **Linux SDK** | 寄存器定义、时钟树、pinctrl、PHY 驱动 |
| 2 | **已有 NuttX 同架构移植** | 代码骨架、驱动模式、Kconfig 结构 |
| 3 | **芯片数据手册** | 寄存器位域、时序图、电气特性 |
| 4 | **开发板原理图** | 引脚分配、外设连接、电源域 |

**RK3506 参考路径**:
- Linux SDK: `RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/`
- NuttX 参考: `openvela/vendor/allwinnertech/` (R258 Cortex-A7)
- 数据手册: `HD-RK3506-EVM/` 目录

### 1.2 环境搭建

```bash
# 1. 克隆 openvela
repo init -u <manifest_url> -b <branch>
repo sync -c -j8

# 2. 安装工具链
# ARM GCC: prebuilts/gcc/linux-x86_64/arm-none-eabi/
# 构建工具: prebuilts/build-tools/linux-x86_64/

# 3. 配置 ccache
export CCACHE_DIR=/tmp/ccache_dir
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

### 3.1 链接脚本

关键配置：
```ld
MEMORY
{
  flash (rx)  : ORIGIN = 0x02080000, LENGTH = 16M
  sram  (rwx) : ORIGIN = 0x02000000, LENGTH = 512K
}

/* 栈和堆的大小 */
__stack_size = 8K;
__heap_size = 1M;
```

### 3.2 启动代码

```c
// <board>_boardinit.c
void board_late_initialize(void)
{
  // 时钟初始化
  // 内存控制器初始化
  // 串口初始化（用于 early console）
}

// <board>_appinit.c
void board_app_initialize(uintptr_t arg)
{
  // 调用 bringup
  board_bringup();
}
```

### 3.3 defconfig 最小集

```ini
# 架构
CONFIG_ARCH="arm"
CONFIG_ARCH_ARM=y
CONFIG_ARCH_CHIP="<chip>"
CONFIG_ARCH_BOARD="<board>"

# 内存
CONFIG_RAM_START=0x02080000
CONFIG_RAM_SIZE=134217728  # 128MB

# 串口
CONFIG_UART0_SERIAL_CONSOLE=y
CONFIG_SERIAL_CONSOLE="ttyS0"

# NSH
CONFIG_NSH_READLINE=y
CONFIG_INIT_ENTRYPOINT="nsh_main"
```

### 3.4 构建与验证

```bash
cd openvela
rm -rf cmake_out/<board>_nsh
CCACHE_DIR=/tmp/ccache_dir \
PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
./build.sh vendor/<vendor>/boards/<chip>/<board>/configs/nsh/ --cmake -j$(nproc)
```

**成功标志**: 串口输出 `nsh>` 提示符。

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

### 4.2 驱动开发流程

```
1. 阅读 Linux SDK 驱动源码
   ↓
2. 提取寄存器定义和初始化序列
   ↓
3. 创建 NuttX 驱动骨架
   ↓
4. 实现 probe/init/remove
   ↓
5. 注册到 NuttX 框架
   ↓
6. Kconfig 集成
   ↓
7. 板级注册
   ↓
8. 测试验证
```

### 4.3 寄存器操作规范

```c
// 使用 NuttX 标准寄存器访问函数
#include <nuttx/arch.h>

// 32 位寄存器
putreg32(value, base + REG_OFFSET);
value = getreg32(base + REG_OFFSET);

// 位域操作（HIWORD_UPDATE 模式）
#define HIWORD_UPDATE(val, mask, shift) \
  ((val) << (shift) | (mask) << ((shift) + 16))

putreg32(HIWORD_UPDATE(1, 1, BIT_POS), grf_base + GRF_REG);
```

### 4.4 时钟和复位

```c
// 使能外设时钟
modifyreg32(cru_base + CLK_GATE_REG, 0, BIT_CLK_ENABLE);

// 复位外设
modifyreg32(cru_base + SOFTRST_REG, 0, BIT_SOFTRST);
up_udelay(10);  // 保持复位至少 10us
modifyreg32(cru_base + SOFTRST_REG, BIT_SOFTRST, 0);
```

### 4.5 GPIO/Pinctrl

```c
// 引脚复用配置（GRF 寄存器）
// 注意：Rockchip 使用 HIWORD_UPDATE 模式
#define RK3506_GRF_PMU_ADDR  0xff920000

// 配置 GPIO1_A0 为 UART TX
putreg32(HIWORD_UPDATE(2, 0x3, 0),  // 功能选择
         RK3506_GRF_PMU_ADDR + GPIO1A_IOMUX_OFFSET);
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

### 5.2 关键实现点

```c
// 1. DMA 描述符初始化
struct gmac_desc_s {
  uint32_t status;
  uint32_t ctrl;
  uint32_t buf_addr;
  uint32_t next_desc;
};

// 2. netdev 回调
static const struct netdev_ops_s gmac_ops = {
  .ifup     = gmac_ifup,
  .ifdown   = gmac_ifdown,
  .transmit = gmac_transmit,
  .receive  = gmac_receive,
  .addmac   = gmac_addmac,
  .ioctl    = gmac_ioctl,
};

// 3. PHY 初始化（通过 MDIO）
int phy_read(struct net_driver_s *dev, uint8_t phyaddr, uint8_t regaddr);
int phy_write(struct net_driver_s *dev, uint8_t phyaddr, uint8_t regaddr, uint16_t data);
```

### 5.3 常见网络问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| DHCP 超时 | PHY 未就绪 | link-up 后延时 2-3 秒 |
| CRC 错误 | RMII 时钟配置错误 | 检查 REF_CLK 方向 |
| 丢包 | DMA 描述符错误 | 检查缓冲区对齐和长度 |
| 无法连接 | MAC 地址过滤 | 确保 AE 位设置 |

---

## 6. USB 驱动（Phase 4）

### 6.1 DWC2 控制器配置

```c
// FIFO 大小配置（关键！）
#define DWC2_GRXFSIZ   512   // 接收 FIFO
#define DWC2_GNPTXFSIZ 256   // 非周期 TX FIFO
#define DWC2_HPTXFSIZ  224   // 周期 TX FIFO

// 注意：HS bulk 需要足够大的 RX FIFO
// mps=512B → 128 words + 状态字 ≈ 131 words
// 默认 128 words 会导致 bulk 传输失败
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

// 帧缓冲注册
struct fb_video_s;
int fb_register(int display, int plane);
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

static void lcd_init_sequence(void)
{
  lcd_send_cmd(0xFF);  // 命令解锁
  lcd_send_data(0x77);
  // ... 完整初始化序列
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

### 8.2 内存转储

```bash
# 查看内存分布
nsh> free

# 查看进程
nsh> ps

# 查看网络
nsh> ifconfig
nsh> route
```

### 8.3 GDB 调试

```bash
# 启动 GDB Server
openocd -f interface/... -f target/...

# 连接 GDB
arm-none-eabi-gdb cmake_out/<board>_nsh/nuttx
(gdb) target remote :3333
(gdb) break board_bringup
(gdb) continue
```

### 8.4 常见启动问题

| 现象 | 可能原因 | 检查点 |
|------|----------|--------|
| 无串口输出 | 时钟配置错误 | UART 时钟源和分频 |
| HardFault | 内存映射错误 | MPU 配置、链接脚本 |
| 卡在启动 | 外设初始化死循环 | 时钟门控、复位状态 |
| 堆栈溢出 | 栈大小不足 | 增大 CONFIG_IDLETHREAD_STACKSIZE |

---

## 9. Kconfig 集成

### 9.1 芯片级 Kconfig

```kconfig
# vendor/<vendor>/chips/<chip>/Kconfig
config ARCH_CHIP_<CHIP>
    bool "<Chip> series"
    select ARMV7A
    select ARMV7A_DCACHE_WRITETHROUGH
    ---help---
        <Chip> Cortex-A7 processor.

if ARCH_CHIP_<CHIP>

config <CHIP>_UART0
    bool "UART0"
    default y
    ---help---
        Enable UART0.

endif # ARCH_CHIP_<CHIP>
```

### 9.2 板级 Kconfig

```kconfig
# vendor/<vendor>/boards/<chip>/<board>/Kconfig
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
# 网络配置
CONFIG_<BOARD>_NETCFG=y
CONFIG_NETUTILS_DHCPC_RETRIES=10  # STP 等待时间

# 外设
CONFIG_<CHIP>_GMAC0=y
CONFIG_<CHIP>_USBHOST=y
```

---

## 10. 打包与烧录

### 10.1 打包脚本

```bash
#!/bin/bash
# nand_firmware/pack_nand.sh

# 设置环境变量
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

# 2. ADB 模式
adb push update.img /data/
adb shell "ota update /data/update.img"

# 3. OTA 模式（板上）
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

## 12. 常见陷阱

### 12.1 时钟配置

**问题**: 外设不工作，但寄存器写入正常。

**原因**: 时钟门控未使能。Rockchip 等 SoC 使用 SET_TO_DISABLE 模式：
```c
// 错误：写 1 开时钟
putreg32(BIT_CLK, cru + CLK_REG);  // 实际是关时钟！

// 正确：写 0 开时钟（SET_TO_DISABLE）
putreg32(BIT_CLK << 16, cru + CLK_REG);  // 高 16 位是写使能，低 16 位是值
```

### 12.2 内存对齐

**问题**: HardFault 在访问外设寄存器。

**原因**: 未对齐访问。Cortex-A7 要求：
```c
// 错误：可能未对齐
*(uint32_t *)addr = value;

// 正确：使用 NuttX 宏
putreg32(value, addr);
```

### 12.3 DMA 缓冲区

**问题**: DMA 传输数据错误。

**原因**: 缓冲区未对齐或在 cache 中。
```c
// 分配对齐缓冲区
buf = kmm_memalign(32, size);

// 或使用 uncached 映射
buf = kmm_zalloc(size);  // 确保在 uncached 区域
```

### 12.4 中断优先级

**问题**: 中断不触发或嵌套异常。

**原因**: NVIC 优先级配置错误。
```c
// 设置优先级（数值越小优先级越高）
irq_set_priority(IRQ_NUM, 128);  // 中等优先级

// 使能中断
up_enable_irq(IRQ_NUM);
```

### 12.5 GPIO 复用

**问题**: 外设引脚无信号输出。

**原因**: 引脚复用未配置或配置错误。
```c
// 检查原理图确认引脚功能
// 配置 IOMUX 寄存器
// 配置驱动强度和上下拉
```

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

## 16. 问题排查流程

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
  ↓
7. 求助社区
   - NuttX 邮件列表
   - openvela 论坛
```

---

## 17. 总结

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

*基于 RK3506G2 移植经验总结，适用于 Cortex-A7/A53/RISC-V 等架构的 openvela 移植。*