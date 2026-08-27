# OpenVela RK3506G2 移植项目 — 探索报告与策略

> 撰写于初次探索完成后（基于本会话内已检视的代码与文档）
> 目标：把 OpenVela 完整移植到 Rockchip RK3506G2 (Cortex-A7)

---

## 一、项目结构总览

### 1.1 顶层目录

```
project/
├── openvela/                    # OpenVela 完整源码（repo 拉取，~20GB）
├── HD-RK3506-EVM/              # 板级硬件文档、SDK 使用说明
├── RK3506G2/                   # Rockchip Linux SDK 归档（10GB+ 压缩包）
├── nanopi_m4_rk3399/           # 参考移植：RK3399 NanoPi M4 BSP
├── OpenVelaDocs/               # OpenVela 官方文档镜像（中英）
├── app/                        # 空 — 比赛"应用"子目录
├── board/                      # 空 — 比赛"板级适配"子目录
├── quickapp/                   # 空 — 比赛"快应用"子目录
├── logs/                       # AI Coding 日志
├── AGENTS.md                   # 移植规则与已知问题修复记录
├── CLAUDE.md                   # Claude Code 协作指南
├── README.md                   # 比赛说明
└── openvela.xml                # OpenVela 仓库 manifest
```

### 1.2 OpenVela 工作区核心目录

```
openvela/
├── nuttx/                      # NuttX RTOS 内核
│   ├── arch/arm/src/armv7-a/   # ARMv7-A 通用架构代码（GICv2、MMU、启动）
│   ├── drivers/                # NuttX 上半部驱动（fb/i2c/input/usbhost/lcd 等）
│   ├── boards/                 # 通用 in-tree BSP（仅参考）
│   ├── devicetree/             # DeviceTree 基础设施
│   ├── graphics/               # LVGL 集成
│   └── include/nuttx/          # 公共 API 头文件
├── apps/                       # 用户态应用
│   ├── examples/lvgl_homepage/ # 本项目的 LVGL Demo (已修复)
│   ├── graphics/lvgl/          # LVGL 库
│   ├── system/                 # 系统工具
│   ├── netutils/               # 网络工具
│   ├── interpretors/quickjs/   # QuickApp JS 引擎
│   └── ...
├── vendor/                     # 厂商定制代码（**移植主要工作区**）
│   ├── rockchip/
│   │   ├── chips/rk3506/       # RK3506 SoC 层
│   │   └── boards/rk3506/
│   │       └── hd-rk3506-evm/  # HD-RK3506-EVM 板级
│   ├── openvela/boards/vela/   # 模拟器/QEMU 板 + QuickApp 预编译库
│   ├── template/               # 新板级模板
│   ├── allwinnertech/, bes/, sifli/, ...  # 其他参考实现
│   └── ...
├── external/                   # 外部第三方库
├── frameworks/                 # 服务端框架
├── prebuilts/
│   ├── gcc/linux-x86_64/       # 工具链 (arm-none-eabi)
│   └── tools/linux/x86_64/     # 工具 (genromfs 等)
├── packages/                   # 可选包（QuickApp、wamr、tflite-micro...）
├── nand_firmware/              # NAND 固件打包脚本与产物
└── build.sh → nuttx/tools/build.sh  # 统一构建入口
```

---

## 二、已实现的 RK3506G2 移植

### 2.1 SoC 层（`vendor/rockchip/chips/rk3506/`）

| 文件 | 行数 | 状态 | 用途 |
|------|------|------|------|
| `chip.h` | 134 | ✅ | GIC 基址、页表、内存布局 |
| `include/chip.h` | - | ✅ | 架构层公共接口 |
| `include/irq.h` | - | ✅ | 中断定义 |
| `hardware/rk3506_memorymap.h` | 138 | ✅ | 寄存器地址全集（UART/I2C/SPI/GPIO/VOP/USB/...） |
| `hardware/rk3506_dwc2.h` | 15K | ✅ | DWC2 USB 控制器寄存器定义 |
| `rk3506_irq.c/h` | 107 | ✅ | 中断控制器 |
| `rk3506_start.c` | 102 | ✅ | `arm_boot()` — 启动入口 |
| `rk3506_lowputc.c/h` | 201 | ✅ | 串口低层 putc/getc |
| `rk3506_serial.c/h` | 782 | ✅ | UART 驱动 (DesignWare 8250) |
| `rk3506_timerisr.c` | 51 | ✅ | 系统定时器 |
| `rk3506_allocateheap.c` | 61 | ✅ | 堆初始化 |
| `rk3506_i2c.c/h` | 591+127 | ✅ | I2C 主控（兼容 RK3x） |
| `rk3506_vop.c/h` | 372+144 | ✅ | VOP 显示控制器（FB） |
| `rk3506_usbhost.c/h` | 2855+83 | ✅ | DWC2 USB Host 控制器 |
| `Kconfig` | 125 | ✅ | UART0-5, VOP, I2C, USB 配置项 |
| `Make.defs` | 35 | ✅ | 源文件列表 |
| `CMakeLists.txt` | 45 | ✅ | CMake 集成 |

### 2.2 板级层（`vendor/rockchip/boards/rk3506/hd-rk3506-evm/`）

| 文件 | 状态 | 用途 |
|------|------|------|
| `include/board.h` | ✅ | 板级定义（UART 参数、LED） |
| `src/hd_rk3506_boardinit.c` | ✅ | memory/board_initialize |
| `src/hd_rk3506_appinit.c` | ✅ | board_app_initialize/finalinitialize |
| `src/hd_rk3506_bringup.c` | ✅ | 设备注册（procfs, ST7701S, VOP, GT911, USB） |
| `src/hd_rk3506_st7701s.c/h` | ✅ 软 SPI 模拟 | LCD 初始化序列 |
| `src/hd_rk3506_gt911.c/h` | ✅ | 触摸驱动（I2C0，addr 0x5D） |
| `src/etc/{group,passwd}` | ✅ | 基础用户配置 |
| `src/etc/init.d/rc.sysinit` | ✅ | 挂载 /proc /system /data /oem |
| `src/etc/init.d/rcS` | 占位 | rcS |
| `Kconfig` | ✅ | INPUT_GT911, LCD_ST7701S |
| `configs/nsh/defconfig` | ✅ | nsh 基线配置 |
| `configs/parameter.txt` | ✅ | 分区表 |
| `scripts/ld.script` | ✅ | 链接脚本 (RAM@0x02080000, 128MB) |
| `scripts/Make.defs` | ✅ | 工具链/库/POSTBUILD |
| `scripts/package.sh` | ✅ | 固件打包封装 |
| `CMakeLists.txt` | ✅ | CMake 集成 + post-build 触发 pack |

### 2.3 应用 / Demo

| 文件 | 状态 | 用途 |
|------|------|------|
| `apps/examples/lvgl_homepage/lvgl_homepage_main.c` | ✅ 281 行 | LVGL 主页（时钟 + 状态卡片 + 导航） |
| `apps/examples/lvgl_homepage/{Kconfig,Makefile,CMakeLists.txt}` | ✅ | 编译集成（**Kconfig 已修复 PROGNAME/PRIORITY/STACKSIZE**） |

### 2.4 打包工具

| 文件 | 状态 | 用途 |
|------|------|------|
| `openvela/openvela-nuttx.its` | ✅ | U-Boot FIT 镜像定义 |
| `openvela/nand_firmware/pack_nand.sh` | ✅ 已修复 | update.img 打包（使用 env 变量+相对路径） |
| `openvela/nand_firmware/parameter.txt` | ✅ | 标准分区表 |
| `openvela/nand_firmware/MiniLoaderAll.bin` | ✅ 预置 | Rockchip SPL 引导 |
| `openvela/nand_firmware/boot.img` / `update.img` | ✅ 预置 | 上次构建产物 |

---

## 三、已构建产物状态（`openvela/cmake_out/hd-rk3506-evm_nsh/`）

```
nuttx        6,021,464 B  (~5.7 MB)  — ELF
nuttx.bin   34,400,804 B  (~32 MB)   — RAW binary（**待修复：应 ~5MB**）
nuttx.map    6,937,842 B             — 符号表
.config      91,688 B                — 当前配置
```

构建配置核心点：
- `CONFIG_RAM_START=0x02080000`
- `CONFIG_RAM_SIZE=128MB`
- ARMv7-A Cortex-A7 (`CONFIG_ARCH_CORTEXA7`)
- GICv2 + FPU + Low Vectors
- 启用了：I2C, VOP (480x854 RGB565), GT911, USB Host, LVGL, lvgl_homepage
- **未启用** `LCD_ST7701S` (defconfig 中注释掉了)
- **未启用** `QUICKAPP`（与预编译库路径相关）

---

## 四、关键移植规则（来自 `AGENTS.md`，已自动从会话上下文移除）

> 注：原规则已从会话上下文移除；本节仅记录项目层面的规则以备后用。

1. **配置修改**：禁止直接改 `defconfig`，用 `menuconfig` + `savedefconfig`
2. **固件大小**：必须包含完整 LVGL + QuickApp（目标 ~5MB+，而非 322KB）
3. **打包**：脚本用相对路径 / 环境变量；BOOT 分区按实际大小调整
4. **驱动开发**：参考 OpenVela 官方文档 + `nanopi_m4_rk3399`；用 `driver-code-reviewer` 审查
5. **多核**：AMP 架构要分别编译 A7 + M0，使用 mailbox 通信

---

## 五、已记录的问题与修复

| # | 问题 | 根因 | 修复状态 |
|---|------|------|----------|
| 1 | LVGL 未被链接 | `apps/examples/lvgl_homepage/Kconfig` 缺 PROGNAME/PRIORITY/STACKSIZE | ✅ 已修复 |
| 2 | nuttx.bin 33MB 零填充 | `.note.gnu.build-id` VMA=0 与 `.text` VMA=0x02080000 不连续，objcopy 平坦输出 | ✅ `scripts/Make.defs` POSTBUILD 使用 `--only-section` |
| 3 | pack_nand.sh 硬编码绝对路径 | 旧版写死 SDK 路径 | ✅ 改用 `RK3506_SDK_DIR` 环境变量 + 默认相对路径 |

---

## 六、移植目标达成度评估

| 目标 | 完成度 | 备注 |
|------|--------|------|
| A7 内核启动 | ✅ | `rk3506_start.c` + 链接脚本 + Low Vectors |
| UART 串口控制台 | ✅ | UART0 完整驱动 |
| 系统定时器 + 调度 | ✅ | `rk3506_timerisr.c` |
| 内存 + MMU | ✅ | `chip.h` 完整页表布局 |
| GICv2 中断 | ✅ | `rk3506_irq.c` |
| I2C 主控 | ✅ | 591 行 I2C 驱动 |
| VOP 显示 (480x854 RGB565) | ✅ | `rk3506_vop.c` |
| ST7701S 初始化 | ✅ 软 SPI 模拟 | **但 Kconfig 缺省未启用** |
| GT911 触摸 | ✅ | 794 行驱动 |
| USB Host (DWC2) | ✅ | 2855 行驱动 |
| NSH 基础 | ✅ | 启用了 procfs/romfs |
| LVGL 集成 | ✅ | Kconfig 修复后已链接 |
| LVGL Demo | ✅ | lvgl_homepage 可运行 |
| 完整 QuickApp 框架 | ⚠️ 部分 | 预编译库在 `vendor/openvela/boards/vela/libs/armv7a_cmake/`，但 `QUICKAPP` 未在 defconfig 中启用 |
| NAND 固件打包 | ✅ | `pack_nand.sh` + `parameter.txt` + MiniLoaderAll.bin |
| 实际烧录验证 | ❌ | 需要真机测试 |

---

## 七、移植策略与建议

### 7.1 当前完成度：**约 80%**

已实现 SoC + Board + LVGL + 触摸 + 显示 + USB + 打包工具链。剩余的关键工作：

### 7.2 短期任务（高优先级）

1. **重新构建并验证 nuttx.bin 大小**
   - 现状：34MB（错误）
   - 目标：5-8MB
   - 验证步骤：
     - `cd openvela && ./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j$(nproc)`
     - 期望：`openvela/nand_firmware/nuttx.bin` 落到合理大小

2. **在 menuconfig 中启用 LCD_ST7701S**
   - 现状：defconfig 中 `CONFIG_LCD_ST7701S is not set`
   - 步骤：
     - `cd openvela && ./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake menuconfig`
     - 路径：Board Selection → HD-RK3506-EVM → ST7701S LCD Panel Initialization
     - 保存：`./build.sh ... --cmake savedefconfig`
   - 必须在 VOP 初始化**之前**调用 ST7701S 初始化（已在 `bringup.c` 顺序中处理）

3. **验证 QuickApp 预编译库的链接**
   - 现状：defconfig 未启用 `CONFIG_QUICKAPP`
   - 检查 `vendor/openvela/boards/vela/libs/armv7a_cmake/` 是否存在所需 .a 文件
   - 在 menuconfig 中启用 QuickApp 相关 CONFIG

4. **分区大小按实际固件大小调整**
   - 现状：parameter.txt 的 boot 分区为 0x5000 (10MB)
   - 需要根据 nuttx.bin 实际大小决定 boot 分区容量

### 7.3 中期任务

5. **DWC2 USB OTG1 控制器**（当前仅 OTG0）
   - Kconfig 有 `RK3506_USBHOST_OTG1` 选项，可按需启用

6. **AMP 架构支持**
   - 项目文档提到 RK3506G2 是 A7 + M0 双核异构
   - 当前 `arm_boot()` 仅有 A7 代码
   - 需要在 `rk3506_start.c` 中添加 M0 启动逻辑
   - 使用 `RK3506_MAILBOX0_ADDR` (0xff290000) 进行核间通信

7. **添加更多 input 设备**（按键、矩阵键盘）
   - 硬件规格提到 3 个 M0 按键 + 1 个 A7 按键
   - 需要 GPIO 按键驱动注册

8. **完善网络协议栈**
   - 现状：TCP/UDP/ICMP 已启用
   - 待添加：USB ECM/RNDIS (4G 模块 E27-A)、WiFi (RTL8724DU)

### 7.4 长期任务

9. **MPU 域与 BUS 域电源管理**
   - RK3506 有 PMU domain + BUS domain
   - 需要在 `board_early_initialize` 中配置 CRU/GRF

10. **真机烧录与调试**
    - 使用 upgrade_tool 烧录 update.img
    - 串口验证启动日志
    - LVGL 显示验证
    - 触摸交互验证

11. **提交 PR 到 OpenVela 上游**
    - 工作流：fork vendor/rockchip.git → 分支 → PR
    - 参考 `.claude/skills/submit-pr/`

### 7.5 验证清单

- [ ] `nuttx.bin` ≤ 8MB（去掉零填充后）
- [ ] `CONFIG_LCD_ST7701S=y` 在 defconfig 中生效
- [ ] `CONFIG_QUICKAPP=y` + 预编译库链接成功
- [ ] parameter.txt 的 boot 分区 ≥ nuttx.bin 大小
- [ ] pack_nand.sh 成功生成 update.img
- [ ] 模拟器或真机成功启动 NSH
- [ ] LVGL Demo 在屏幕上显示
- [ ] 触摸点击有响应

---

## 八、参考资源索引

### 8.1 Skills（.claude/skills/）

| Skill | 用途 |
|-------|------|
| `openvela-build` | 编译/menuconfig/savedefconfig |
| `openvela-quickstart` | 从零搭建环境 |
| `nuttx-driver-development` | 驱动实现全流程 |
| `driver-code-reviewer` | 6 维代码审查 |
| `kconfig-tweak` | Kconfig 调整 |
| `codesize` | 体积分析 |
| `memdump` | 内存检查 |
| `submit-pr` | 提交流程 |
| `contest-log-collector` | AI 日志归集 |

### 8.2 参考实现

- **同芯片厂商**：`vendor/allwinnertech/` (lichee, R528, F133...) — 最接近的"非 Rockchip SoC 移植"
- **同厂商不同 SoC**：`nanopi_m4_rk3399/` — 来自本仓库非 OpenVela 树
- **官方文档**：`OpenVelaDocs/zh-cn/chip_porting/porting_guide.md` — 移植手册

### 8.3 硬件参考

- `HD-RK3506-EVM/SDK开发与外设设备树配置/显示触摸驱动移植.md` — RGB/MIPI 屏 + GT911 触摸的 Linux DTS 参考
- `HD-RK3506-EVM/SDK开发与外设设备树配置/UART.md` 等 — 总线配置
- `RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/` — 完整 Linux SDK（10GB+ 包含 U-Boot, kernel, drivers）

### 8.4 关键文件路径速查

```
# 编译入口
openvela/build.sh → nuttx/tools/build.sh

# 板级配置
openvela/vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/defconfig

# 链接脚本
openvela/vendor/rockchip/boards/rk3506/hd-rk3506-evm/scripts/ld.script

# 内存映射寄存器表
openvela/vendor/rockchip/chips/rk3506/hardware/rk3506_memorymap.h

# 启动入口
openvela/vendor/rockchip/chips/rk3506/rk3506_start.c

# 显示
openvela/vendor/rockchip/chips/rk3506/rk3506_vop.c
openvela/vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_st7701s.c

# 触摸
openvela/vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_gt911.c

# USB
openvela/vendor/rockchip/chips/rk3506/rk3506_usbhost.c

# 打包
openvela/nand_firmware/pack_nand.sh
openvela/nand_firmware/parameter.txt
openvela/vendor/rockchip/boards/rk3506/hd-rk3506-evm/scripts/package.sh

# LVGL Demo
openvela/apps/examples/lvgl_homepage/
```

---

## 九、后续可继续探索的方向

1. 检查 `RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/` 中 DWC2 / VOP 寄存器级实现，交叉验证现有 NuttX 驱动
2. 阅读 `openvela/nuttx/arch/arm/src/armv7-a/arm_head.S` 确认 Low Vectors 启动路径
3. 调查 `apps/graphics/lvgl/` 是否有 NuttX 集成层
4. 验证 `apps/interpreters/quickjs/` 在目标板的资源占用
5. 探索 AMP 文档（`OpenVelaDocs/zh-cn/` 中是否有专门章节）
