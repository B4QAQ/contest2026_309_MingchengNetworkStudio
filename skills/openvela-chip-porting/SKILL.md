---
name: openvela-chip-porting
description: "新芯片/新板子移植到 openvela (NuttX) 的完整流程。Use when: 移植新芯片、新板子、BSP 适配、bringup、芯片驱动开发、硬件适配、新硬件适配赛道。重点覆盖 RK3506G2 (3×A7 + M0) 同构与异构 AMP 经验。"
---

# openvela Chip Porting

基于 RK3506G2 完整移植经验（222+ 个提交，v8a→v10）总结的芯片移植方法论。

**核心原则**：寄存器/时序以 Linux SDK 和已适配的同架构 NuttX 移植为唯一事实基准，不凭记忆猜测；两处都没有覆盖的点，先停下确认方案再写代码。

---

## 1. 移植前准备

### 1.1 参考资料优先级

| 优先级 | 来源 | 用途 |
|--------|------|------|
| 1 | **Linux SDK** | 启动链/内存布局、寄存器位域、时钟树、pinctrl、PHY、AMP 配置 |
| 2 | **已有 NuttX 同架构移植** | 代码骨架、`spi_ops`/MTD/netdev 集成方式、HAL→NuttX 封装套路 |
| 3 | **芯片数据手册** | 电气特性、时序图（但**不**用来回忆具体寄存器） |
| 4 | **开发板原理图** | 引脚分配、RM_IO vs 普通 pad、电源域 |

**RK3506 参考路径**：

- Linux SDK：`rk3506_linux6.1_sdk_v1.2.0_iot_evm/`
  - 寄存器：`hal/lib/CMSIS/Device/RK3506/Include/rk3506.h`
  - HAL 驱动：`hal/lib/hal/src/<periph>.c`
  - 时钟：`kernel-6.1/drivers/clk/rockchip/clk-rk3506.c`
  - Pinctrl：`u-boot/arch/arm/dts/rk3506-pinctrl.dtsi`
  - AMP：`kernel-6.1/arch/arm/boot/dts/rockchip/rk3506-amp.dtsi`
  - M0 HAL 工程：`hal/project/rk3506-mcu/`
  - U-Boot AMP：`arch/arm/mach-rockchip/rk3506/rk3506.c`（`amp_cpus_on()` / `fit_standalone_release()`）
- NuttX 参考：`openvela/vendor/allwinnertech/chips/r528/`（Cortex-A7 已适配，学集成套路）

### 1.2 环境搭建

```bash
# 1. 克隆 openvela
repo init -u <manifest_url> -b <branch>
repo sync -c -j8

# 2. ccache 目录必须可写
export CCACHE_DIR=/tmp/ccache_dir

# 3. 构建命令模板（prebuilt 工具链必须加在 PATH 前）
cd openvela
rm -rf cmake_out/<board>_nsh
CCACHE_DIR=/tmp/ccache_dir \
PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
./build.sh vendor/<vendor>/boards/<chip>/<board>/configs/nsh/ --cmake -j$(nproc)
```

> **异构 M0 核有独立的工具链硬约束**（见 §12）：必须用厂商 SDK 自带的固定版本 gcc-arm-none-eabi，不能顺手用 openvela 自带的新版。

---

## 2. BSP 目录结构

```
vendor/<vendor>/boards/<chip>/<board>/
├── CMakeLists.txt          # 板级源文件注册
├── Kconfig                 # 板级配置选项
├── configs/nsh/defconfig   # 默认配置（改配置走 menuconfig + savedefconfig）
├── include/board.h
├── scripts/
│   ├── Make.defs           # 构建系统集成
│   ├── ld.script           # 链接脚本（AMP 时要给协处理器预留内存窗）
│   └── package.sh
└── src/
    ├── CMakeLists.txt
    ├── <board>_boardinit.c # 板级早期初始化
    ├── <board>_appinit.c   # 应用期初始化
    ├── <board>_bringup.c   # 外设探测（单个驱动失败不能阻断后续）
    └── <driver>.c          # 板级驱动

vendor/<vendor>/chips/<chip>/
├── CMakeLists.txt
├── Kconfig
├── Make.defs
├── chip.h
├── hardware/               # 寄存器定义/内存映射
├── include/
├── drv/                    # SDK HAL 封装层（gpio/spi/pwm ...）
└── <peripheral>.c          # 芯片级驱动
```

---

## 3. 最小启动（Phase 1）

目标：串口输出 `nsh>`。

### 3.1 defconfig 最小集

```ini
CONFIG_ARCH="arm"
CONFIG_ARCH_ARM=y
CONFIG_ARCH_CHIP="<chip>"
CONFIG_ARCH_BOARD="<board>"
CONFIG_RAW_BINARY=y
CONFIG_INIT_ENTRYPOINT="nsh_main"
CONFIG_SERIAL_CONSOLE="ttyS0"
```

### 3.2 链接脚本

RAM 起点/长度必须与 U-Boot、AMP 预留窗严格对齐；AMP SoC 要把协处理器的代码窗与共享内存窗从主核 heap 里挖掉（RK3506：主核 RAM 0x02080000–0x03a00000，其后四个 1MB no-map 窗口归 M0/rpmsg）。

### 3.3 常见启动问题

| 现象 | 排查方向 |
|------|----------|
| 无串口输出 | UART 时钟源/分频（RK3506 UART 是 24 MHz，不是常见的 1.8432 MHz） |
| HardFault/Data abort | 链接地址与 U-Boot 加载地址不一致；heap 越过预留窗 |
| 卡在启动 | 初始化里有轮询死循环；时钟/复位没开；bringup 被单个驱动失败阻断 |

---

## 4. 外设驱动开发（Phase 2）

### 4.1 推荐开发顺序

1. **GPIO**（LED 闪烁验证）→ 2. **UART**（控制台）→ 3. **I2C** → 4. **SPI/FSPI + NAND**（有了文件系统才好迭代）→ 5. **网络** → 6. **USB** → 7. **显示/输入** → 8. **AMP 协处理器**（最后，依赖最多）

### 4.2 寄存器操作规范

```c
#include <nuttx/arch.h>

putreg32(value, base + REG_OFFSET);
value = getreg32(base + REG_OFFSET);

/* Rockchip GRF 写 1 生效：HIWORD 放写使能 mask */
#define HIWORD_UPDATE(val, mask, shift) ((val) << (shift) | (mask) << ((shift) + 16))
```

**⚠️ 真实案例 — Rockchip 时钟门控是 SET_TO_DISABLE 语义**：

```c
/* 错误：以为写 1 开门，实际数据位 1 = 关闭（SET_TO_DISABLE） */
putreg32(BIT_CLK | (BIT_CLK << 16), cru + CLKGATE_CON);   /* 把时钟关死了！ */

/* 正确：高 16 位写使能、低 16 位数据写 0 = 使能；回读到 0 表示门已开 */
putreg32(BIT_CLK << 16, cru + CLKGATE_CON);
```

rpmsg 排期里 A7 侧就曾这样亲手关掉 PCLK_MAILBOX，导致 kick 全丢、探针全零。写完一定要回读验证。

### 4.3 日志宏规范

```c
#include <debug.h>

_err("error: %d\n", ret);    /* 只在真正的错误路径 */
_warn("warn\n");
_info("info\n");
```

**⚠️ 真实案例**：`ierr()/iwarn()/iinfo()` 是 **INPUT 子系统**的模块宏（`CONFIG_DEBUG_INPUT_*` 门控），工程没开这些配置时全部静默——而且编译器不报错。USB 子系统同理用 `uerr/uwarn/uinfo`。提交前全局 grep 一遍。

---

## 5. 存储（SPI NAND）

踩出来的正确栈：**FSPI 控制器 → MTD（spinand）→ mtd_partition → dhara（磨损均衡）→ littlefs**。

- 传分区偏移时注意单位：bringup 容易把擦块索引当页索引传，导致分区映射错位几 MiB
- **SmartFS 在这条栈上不可用**：它要 SMART 层的 `BIOC_GETFORMAT` ioctl，dhara 只透传 MTD ioctl，返回 ENOTTY(25)
- littlefs 原生吃块设备，挂载带自动格式化：

```bash
nsh> mount -t littlefs -o autoformat /dev/mtdblock0 /data
```

- FSPI DLL training 期间不要加噪声日志；多消费者（/data、OTA 分区）并发访问时控制器要互斥锁
- NAND 首刷 littlefs 报一次 corrupt pair 后自动格式化属正常

---

## 6. 网络驱动

### 6.1 分层

```
应用 (curl/ping) → NuttX net 栈 → netdev (net_driver_s) → GMAC 驱动(DMA 描述符) → PHY
```

### 6.2 真实案例

| 现象 | 根因 |
|------|------|
| TX 发不出 | DMA 描述符 TDES3 帧长字段没填 |
| RX 全 CRC 错 | PHY（YT8512）缺 RMII_EN + refclk PLL 配置 |
| 收不到任何包 | 地址过滤没开 promiscuous/multicast 位 |
| 插网线后 boot DHCP 必失败 | 交换机端口 STP listening/learning（10~30s）丢 BOOTP；**修法是把 DHCP 重试拉长（10 次≈30s）覆盖 STP 窗口，不是加链路延时** |
| 间歇性解析失败 | DNS 完全取决于 DHCP 是否带 option 6；要开 `NETINIT_DNS` 并给网关地址兜底 |
| curl: 60 证书来自未来 | 无电池 RTC，开机停在 1970；ntpc daemon 指数退避校时解决 |
| curl: 6 getaddrinfo thread failed | **不是 socketpair/loopback 问题**（该说法已被源码推翻）。真因：threaded resolver 自检（pthread + 环回 socketpair）在 NuttX 上失败；curl 配置改为同步解析（关掉 `USE_THREADS_POSIX`）后真实 EAI 错误才可见 |
| curl: 77 CA 文件空 | 把 ca-certificates bundle 放进板级 romfs `/etc/ssl/curl/` 并在 CMakeLists 登记 |
| TLS 熵源失败 | 开 `CRYPTO` + `CRYPTO_RANDOM_POOL` + `DEV_URANDOM(_RANDOM_POOL)`；注意子选项在 `if CRYPTO` 块内 |

**教训**：netinit monitor 之类“链路变化自动 ifup/down + ioctl”的监控线程会和驱动持锁等待死锁；没有 PHY 中断之前不要加。链路稳定后驱动本身应保持静默（RK3506 GMAC 的 link up/down 日志最终按要求全删）。

---

## 7. USB Host（DWC2）

- **先确认 OTG 口角色**：RK3506 上 otg0 是 Type-C 烧录口（peripheral），USB-A 主机在 **otg1**；初始化错控制器会出现各种玄学
- **PHY 要唤醒**：inno-usb2 PHY 复位后处于 suspend（GRF phy_sus=0x1d1），不唤醒则 UTMI 线态垃圾、幻象枚举
- **⚠️ FIFO 大小决定 bulk 能否工作**：枚举全过、bulk 传输零中断静默死掉，根因是 RX FIFO 默认 128 words < HS bulk 单包需求（512B/4 + 状态字 ≈ 131 words）。按 SDK 配 `GRXFSIZ=512 / GNPTXFSIZ=256 / HPTXFSIZ=224`
- NAK 重试不能被 5s 传输超时误杀；XFRC 不置位时用 ACK 判据
- `CONFIG_USBHOST_HAVE_ASYNCH` 要在驱动 Kconfig 里 `select`，否则 `USBHOST_HUB=y` 会被静默丢弃

---

## 8. 显示与输入

### 8.1 面板

- RGB 并口屏走 VOP/framebuffer 时按 SDK 时序来（format 位域、`dsp_layer_sel`、GRF HIWORD 更新）——本项目早期 ST7701S 方案的经验，该驱动已随产品切到 SPI 小屏删除
- SPI 小屏（如 NV3007 142×428）走 3-wire SPI 初始化序列；逻辑横屏可在 LVGL 层旋转 90°，PARTIAL 模式只刷脏区省带宽
- 上电白屏问题：LVGL 首帧与背光/屏幕 load 的顺序要固定

### 8.2 ⚠️ LVGL 按键焦点：ESC 默认到不了屏幕回调

这是做完 4 键 UI 后踩到的关键陷阱：

- LVGL keypad indev 对 ESC 的处理是 `lv_group_send_data(g, LV_KEY_ESC)`，它**只把 `LV_EVENT_KEY` 发给当前焦点对象**（`lv_group.c` 的 `lv_group_send_data`）
- 事件冒泡要求目标对象带 `LV_OBJ_FLAG_EVENT_BUBBLE`，**默认关闭**
- 所以把"退出应用"回调挂在 screen 上，只在 screen 自己持焦点时有效；应用一旦创建按钮/滑块等可聚焦控件，返回键立刻失灵

**正确做法**：返回键在板级 indev 层直接截获（不送 LVGL），维护一个 handler 栈（UI 的"回主页"先注册，快应用运行时压栈自己的"退出应用"，退出时弹栈）；动作通过 `lv_async_call()` 延迟到事件派发之外执行，才能安全删除当前 screen。按键在**按下沿**触发，不要等 release，否则在 30Hz 轮询下手感迟钝。

---

## 9. Kconfig / defconfig 纪律

- 改配置走 `menuconfig` + `savedefconfig`，不手改 defconfig
- 子选项可能藏在 `if XXX` 块里（如只加 `CRYPTO_RANDOM_POOL` 不加 `CRYPTO` 会被静默丢弃）
- `select` 是让依赖真正生效的手段（HUB 依赖 HAVE_ASYNCH）
- 被静默丢弃的配置不报错——功能不工作时先回 `.config` 确认项还在
- NSH 默认行长只有 80、参数 7 个；curl/OTA 的长 URL 场景要放宽

---

## 10. 打包与烧录

```bash
# M0 固件（AMP 机型）：构建脚本对工具链版本/entry/向量表/大小/FIT load 做硬断言
bash nand_firmware/build_mcu_fw.sh

# 整盘：parameter.txt(GPT) + MiniLoader + U-Boot + boot_a/boot_b + amp + misc
NUTTX_BIN=$PWD/cmake_out/<board>_nsh/nuttx.bin bash nand_firmware/pack_nand.sh

sudo upgrade_tool uf nand_firmware/update.img   # 分区布局变更后必须整盘刷
```

- A/B 机型 U-Boot 必须带 `CONFIG_ANDROID_AB`，否则只认字面 `boot` 分区，首刷卡死
- **thunderboot 陷阱**（见 §12.2）：U-Boot 配置一旦含 `rk3506_tb`（`CONFIG_SPL_KERNEL_BOOT=y`），SPL 直跳内核，`board_late_init()` 不执行，AMP 放行代码整个被绕过
- OTA：misc 分区放 AvbABData（大端、CRC，字段与 U-Boot `avb_ab_flow.c` 对齐）；bootcheck 写 try、用户 confirm、失败回滚

---

## 11. 调试方法论

1. **先证伪最简单假设**：电源、时钟（门控写反）、复位、引脚（RM_IO pad 不能当普通 GPIO）
2. **独立证人优先**：协处理器问题先接上它自己的调试 UART（M0 是 UART1），不要只从主核寄存器猜它跑没跑
3. **找"只有对端会写"的寄存器当就绪信号**：如 INTMUX group3 bit21 全 SDK 只有 M0 的 rpmsg init 写、POR=0，比邮箱 INTEN（温复位不清零、可能是陈旧值）可靠
4. **产物级核验**：怀疑工具链/镜像差异时，直接和 SDK 产物逐字节比对（md5/entry/向量表），不要靠行为猜
5. **热路径不加诊断**：邮箱 ISR/自旋锁里不要 syslog，更不要为填统计字段加忙轮询；诊断脚手架在问题闭环后整组删除
6. 二分法：禁功能、最小复现、示波器/逻辑分析仪看物理波形

常用板上命令：`free`、`ps`、`ifconfig`、`ping`、`date`、`mount`、`rpmsgtest`、`ota status`。

---

## 12. 异构 AMP 移植（A7 + M0，最大的一课）

### 12.1 内存拓扑先定死

照厂商 AMP dtsi 来，主核 linker script / Kconfig RAM 必须同步收缩：

```
M0 代码/数据：  DRAM 0x03a00000, 1MB（M0 自己的 linker script ORIGIN 与 FIT load 地址一致）
rpmsg 共享窗：  0x03c00000, 2MB（两个 vring + 缓冲，no-map）
主核 NuttX：    0x02080000–0x03a00000（主堆区不得越过）
高位第二堆区：  0x03e00000–0x07c00000（62MiB，见下）
```

M0 GCC 工程里 rpmsg 窗的 `LINUX_RPMSG` ORIGIN 要和主核资源表地址逐一对齐。

**AMP 窗口之上的 DDR 不要浪费**：主堆区因镜像紧跟其后、且必须连续，不能跨过中间 4MB 窗口；但窗口之上到 DDR 顶部仍是可缓存普通内存（启动时 `mmu_l1_map_regions` 已整段 identity 映射）。用 `CONFIG_MM_REGIONS=2` + `arm_addregion()` 里 `umm_addregion()` 加为非连续第二堆区即可，无需动 MMU。约束：顶部留 U-Boot 重定位/栈余量（本例留 4MiB），低位 0–0x02080000 是 TEE 常驻 + U-Boot bootm 暂存区（fdt/Image/ramdisk），热重启风险高，不要回收。

### 12.2 启动链：谁放行协处理器

RK3506 的正解（与 Linux SDK 一致）是 **U-Boot 放行**：GPT 里独立 `amp` 分区存 M0 的 standalone FIT，`board_late_init() → amp_cpus_on() → fit_standalone_release()` 读出镜像、SMC 交给 TEE 解除 M0 复位。主核 OS 只开自己一侧的时钟 + 跑 rpmsg 协议，不碰 M0 复位/拷贝。

两个致命配置：

1. **thunderboot**：U-Boot 带 `rk3506_tb` fragment 时 `CONFIG_SPL_KERNEL_BOOT=y`，SPL 直接跳内核，`board_late_init` 从不执行 → M0 永远不被放行。板上判据：看不到完整 U-Boot banner。配置组合应为 `rk3506` defconfig + AB fragment + `rk3506-amp-mcu`，`.config` 里 `CONFIG_SPL_KERNEL_BOOT` 必须不存在
2. **设备树要选对**：确认 `CONFIG_DEFAULT_DEVICE_TREE` 是含 amp 节点的那块板

板上放行成功的日志：`Handle standalone: 'mcu' at 0x03a00000 ... OK`。

### 12.3 ⚠️ 工具链必须锁版本（耗了最久的坑）

M0 固件必须用 **SDK 自带 `gcc-arm-none-eabi-10-2020-q4-major`（gcc 10.2.1）**。用 openvela 的 gcc 13.4 编出来：

- Reset_Handler 从 `0x03a00141` 漂到 `0x03a00171`
- 镜像大小 27504 → 27160 字节
- 行为未经验证，表现为 M0 不武装邮箱中断

换对工具链后产物与 SDK `output/update/Image/amp.img` 内嵌 payload **逐字节相同**。构建脚本把以下做成硬断言，工具链缺失直接失败、不允许回退：

- 工具链路径与版本
- entry = `0x3a00141`、向量表头 8 字节 `00fcaf03 4101a003`、大小 ≤ 1MB
- FIT `load = 0x03a00000`

### 12.4 邮箱/rpmsg 协议要点

- 拓扑：M0 kernel id = 3、rpmsg link-id 0x03；A7→M0 用 mailbox3（A2B），M0→A7 用 mailbox0（B2A）
- 中断电平敏感，状态位写 1 清零；INTEN 写法是高 16 位写使能（`0x10000 | bit`）
- kick 顺序：先写 CMD（link id）再写 DATA（magic `RMSG`）；A2B status 还没被对端清掉就丢弃本次 kick（与厂商驱动 EBUSY 行为一致，对端处理旧 kick 时会重扫 vring），**不要**为了等它清而在自旋锁里忙轮询
- 第一个 kick 是建链握手：openamp 会发两次 notifyid=ANY，只有 vdev status 带 DRIVER_OK 的那次才能发
- 主核 start() 里只读轮询"M0 已武装"信号（INTMUX group3 bit21）再发首 kick；超时只放弃本次探测、不算错误
- A7 侧要替 M0 开 UART1 等时钟（Rockchip 门控 SET_TO_DISABLE：写 0 使能）

### 12.5 验收顺序

1. 完整 U-Boot banner + `Handle standalone: 'mcu' ... OK`
2. A7 三核 SMP 起来、`free` 内存与预留窗一致
3. `rpmsgtest`：建端点 0x4003 → 发 ping → 收到 `Rockchip rpmsg linux test!` → **PASS（充分条件：双向收发成功即证明 M0 完整跑完 HAL/INTMUX/rpmsg 初始化，不依赖第二串口）**
4. 有第二串口时再看 M0 自己的 UART1 打印 `[HAL INFO] Hello RK3506 mcu`（独立旁证，非必需）

---

## 13. 质量检查清单

- [ ] 全清构建（`rm -rf cmake_out/<board>_nsh`）通过，无新增 warning
- [ ] 每个驱动有 Kconfig 项且在 Make.defs/CMakeLists 注册
- [ ] 只用 `putreg32/getreg32`；日志只用 `_err/_warn/_info`（无 `ierr/iinfo`、无 ISR 内 syslog）
- [ ] 无硬编码绝对路径；交叉工具链 `find_program(... NO_DEFAULT_PATH)`
- [ ] 无过期注释/过期文档；错误结论在文档里显式标注作废
- [ ] AMP：M0 工具链/entry/向量表/FIT load 硬断言通过，产物与 SDK 可比对
- [ ] 板上验收通过后再提交，主仓与源码快照仓同步无断档

---

## 14. 常见陷阱速查

| # | 陷阱 | 一句话 |
|---|------|--------|
| 1 | Rockchip 门控 | SET_TO_DISABLE：数据位写 0 才是开时钟 |
| 2 | `ierr/iinfo` | INPUT 子系统宏，没开对应 CONFIG 就全静默 |
| 3 | DWC2 FIFO | RX FIFO <131 words 时枚举过但 bulk 静默死 |
| 4 | DHCP 失败 | 多半是 STP 窗口，拉长重试而非加延时 |
| 5 | curl resolver | threaded resolver 自检失败报误导错误，改同步解析 |
| 6 | /data 文件系统 | dhara 上挂 littlefs，不是 SmartFS |
| 7 | U-Boot AB | 不带 ANDROID_AB 就只认字面 boot 分区 |
| 8 | thunderboot | tb fragment 让 SPL 直跳内核，AMP 放行整个被绕过 |
| 9 | M0 工具链 | 必须锁 SDK gcc 10.2；新版编出的 entry 都不一样 |
| 10 | 陈旧寄存器 | 邮箱等块温复位不复位；选只有对端会写且 POR=0 的位当握手信号 |
| 11 | LVGL ESC | KEY 事件只给焦点对象且默认不冒泡，返回键要板级截获 |
| 12 | RM_IO pad | 不能当普通 GPIO 接按键 |
| 13 | AMP 内存 | 主堆不能跨中间窗口，但窗口之上的 DDR 用 MM_REGIONS=2 + umm_addregion 回收，别浪费 |

---

## 15. 移植里程碑模板

| 里程碑 | 目标 | 验收标准 |
|--------|------|----------|
| M1 | 最小启动 | U-Boot 完整 banner + NSH 提示符 |
| M2 | 存储 | SPI NAND 分区 + littlefs /data 可读写 |
| M3 | 网络 | DHCP/ping/curl HTTPS/date 校时 |
| M4 | USB | U 盘枚举 + vfat 挂载 + 大文件读 |
| M5 | 显示输入 | LCD 出图 + 按键/触摸操作闭环 |
| M6 | AMP | 协处理器被 U-Boot 放行、rpmsgtest PASS（M0 自身串口 banner 为可选旁证，单串口环境下不必强求） |
| M7 | OTA | A/B 升级 + confirm + 断电回滚 |
| M8 | 收尾 | 日志治理、文档更新、全清构建 |

---

## 16. 总结

1. **事实基准单一化**：Linux SDK + 已适配同架构 NuttX，超出覆盖就停下问
2. **每阶段板上验收**，不靠"应该能跑"进入下一阶段
3. **用独立证人证伪**：协处理器串口、只有对端会写的寄存器、产物字节比对
4. **诊断脚手架用完即拆**，错误结论显式作废，决策全程留痕
5. 移植是迭代循环：改码 → 全清构建 → 烧录 → 板验 → 记录 → 重复

---

*基于 RK3506G2 移植经验（222+ 提交，v8a→v10，含 AMP 架构一次推倒重做）总结，适用于 Cortex-A7/A53/RISC-V 及带协处理器的异构 openvela 移植。*
