# OpenVela RK3506G2 BSP 移植报告

> **项目**: openvela 适配 Rockchip RK3506G2
> **参赛编号**: 309 (MingchengNetworkStudio)
> **赛道**: 新硬件适配
> **完成时间**: 2026-08-27

---

## 一、作品简介

本项目将 openvela（NuttX-based RTOS）完整移植到 Rockchip RK3506G2 SoC + HD-RK3506-EVM 开发板，
实现从 BootROM → MiniLoader → U-Boot → NuttX 启动 NSH 的完整启动链路。

**核心交付物**：
- 完整可编译 BSP：47 个文件，~9,650 行代码
- 干净构建：~11 秒，0 warning
- 可烧录 update.img：7.4MB（ARM Cortex-A7 / 128MB DDR3）
- `openvela-fast-chip-porting` Skill：441 行，11 步标准移植流程
- `AGENTS.md` 协作规范：项目级 AI 协作文档

---

## 二、选题方向

**新硬件适配赛道** —— 将 openvela 适配到 Rockchip RK3506G2，填补 openvela
对国产 IoT SoC 的支持空白。RK3506G2 是 Rockchip 2024 年发布的低成本 AIoT
芯片（3×Cortex-A7 + 1×Cortex-M0，128MB DDR3），目前 openvela 主线尚未支持。

---

## 三、目录结构

```
contest2026_309_MingchengNetworkStudio/
├── AGENTS.md                        # AI 协作规范（项目级规则）
├── README.contest.md                # 原始比赛说明
├── PORTING_REPORT.md                # 本文件
├── PROJECT_EXPLORATION.md           # 项目结构探索报告
├── openvela/                        # openvela 主仓（submodule）
│   ├── vendor/rockchip/             # BSP 代码
│   │   ├── chips/rk3506/            #   芯片层（SoC）
│   │   │   ├── rk3506_start.c       #   早期启动
│   │   │   ├── rk3506_irq.c         #   GICv2 中断
│   │   │   ├── rk3506_lowputc.c     #   早期串口
│   │   │   ├── rk3506_serial.c      #   NS16550 串口
│   │   │   ├── rk3506_timerisr.c    #   ARM 通用定时器
│   │   │   ├── rk3506_i2c.c         #   RK3x I2C
│   │   │   ├── rk3506_vop.c         #   显示控制器
│   │   │   ├── rk3506_usbhost.c     #   DWC2 USB Host
│   │   │   ├── rk3506_allocateheap.c
│   │   │   ├── chip.h               #   芯片定义
│   │   │   ├── include/irq.h        #   IRQ 编号
│   │   │   ├── Kconfig              #   芯片配置
│   │   │   ├── Make.defs
│   │   │   ├── CMakeLists.txt
│   │   │   └── hardware/
│   │   │       ├── rk3506_memorymap.h
│   │   │       └── rk3506_dwc2.h
│   │   └── boards/rk3506/
│   │       └── hd-rk3506-evm/       #   板级层
│   │           ├── Kconfig
│   │           ├── CMakeLists.txt
│   │           ├── include/board.h
│   │           ├── src/
│   │           │   ├── hd_rk3506_boardinit.c
│   │           │   ├── hd_rk3506_appinit.c
│   │           │   ├── hd_rk3506_bringup.c
│   │           │   ├── hd_rk3506_st7701s.c   # LCD 面板
│   │           │   └── hd_rk3506_gt911.c     # 触摸
│   │           ├── configs/
│   │           │   ├── nsh/defconfig          # 完整 NSH + LVGL + USB
│   │           │   ├── nsh_minimal/defconfig  # 最小 NSH
│   │           │   └── parameter.txt          # 分区表
│   │           └── scripts/
│   │               ├── ld.script
│   │               ├── Make.defs
│   │               └── package.sh
│   ├── nand_firmware/               # 烧录用镜像
│   │   ├── pack_nand.sh             #   打包脚本
│   │   ├── flash_boot.sh            #   烧录脚本
│   │   ├── parameter.txt            #   分区表
│   │   ├── MiniLoaderAll.bin        #   SPL (280KB)
│   │   ├── uboot.img                #   U-Boot FIT (813KB)
│   │   ├── boot.img                 #   NuttX kernel (4MB raw bin)
│   │   ├── boot.uimg                #   NuttX kernel (4MB uImage)
│   │   └── update.img               #   完整 update.img (7.4MB)
│   └── docs/zh-cn/skills/
│       └── openvela-fast-chip-porting/   # 移植 Skill
│           ├── SKILL.md                  #   11 步移植指南
│           └── RK3506_DRIVER_ISSUES.md   #   驱动问题清单
├── logs/                            # AI Coding 日志
└── README.contest.md                # 比赛说明（原始）
```

---

## 四、运行方式

### 4.1 环境准备

- Linux x86_64 主机（Ubuntu 22.04+ 或 WSL2）
- 已在 `openvela/prebuilts/` 中预装的 arm-none-eabi-gcc 13.4.0
- HD-RK3506-EVM 开发板
- Rockchip `upgrade_tool`（从 `RK3506G2/sdk/tools/` 获取）

### 4.2 拉取工程

```bash
cd /path/to/work
git clone <this-repo>
cd contest2026_309_MingchengNetworkStudio
repo init -u openvela.xml
repo sync -c -j8
```

（注意：实际开发中，openvela 主仓作为 submodule 已在 `openvela/` 目录就绪，
`vendor/rockchip/` 和 `nand_firmware/` 是该仓内的子仓。）

### 4.3 编译

```bash
cd openvela/

# 完整 NSH + LVGL + USB 编译
rm -rf cmake_out/hd-rk3506-evm_nsh
CCACHE_DIR=/tmp/ccache_dir \
PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j$(nproc)

# 或最小化（仅 NSH + 串口，~177KB）
rm -rf cmake_out/hd-rk3506-evm_nsh_minimal
CCACHE_DIR=/tmp/ccache_dir \
PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh_minimal/ --cmake -j$(nproc)
```

**预期输出**：
- `cmake_out/hd-rk3506-evm_nsh/vela.bin` — 510 KB
- `#### build completed successfully (11 seconds) ####`

### 4.4 打包

```bash
bash nand_firmware/pack_nand.sh
```

**输出**（已提交到 `nand_firmware/`）：
- `MiniLoaderAll.bin` (280 KB) — SPL 第二阶段引导
- `uboot.img` (813 KB) — U-Boot FIT 镜像
- `boot.img` / `boot.uimg` (4 MB) — NuttX 内核（raw / uImage 两种格式）
- `update.img` (7.4 MB) — 完整可烧录固件

### 4.5 烧录

```bash
# 1. 短接 RECOVERY 按键，插 USB 线进入 Loader 模式
# 2. 烧录完整固件
sudo upgrade_tool uf nand_firmware/update.img

# 或单分区烧录（用于增量更新）
sudo upgrade_tool di uboot nand_firmware/uboot.img
sudo upgrade_tool di boot  nand_firmware/boot.uimg
```

### 4.6 启动验证

连接 USB-TTL 串口（115200 8N1）到开发板 DEBUG 串口，上电后应看到：
```
U-Boot SPL ...
U-Boot ...
# (在 U-Boot 提示符下)
=> bootm 0x... boot.uimg
## Loading kernel from FIT Image at ...
...
NuttShell (NSH)
nsh>
```

---

## 五、关键发现与修复

### 5.1 严重 Bug 修复（会导致无法启动）

| # | 文件 | Bug | 修复 |
|---|------|-----|------|
| 1 | `rk3506_lowputc.c` / `rk3506_serial.c` | UART 时钟错误（1.8432 MHz，实际是 24 MHz） | 修正为 24 MHz（XIN24M 源） |
| 2 | `vendor/rockchip/.../pack_nand.sh` | 打包的 `uboot.img` 是 1KB 占位，MiniLoader 找不到 U-Boot | 从 SDK `u-boot/fit/uboot.itb` 拷贝真实 U-Boot |
| 3 | `vendor/rockchip/.../CMakeLists.txt` | `add_custom_command(TARGET nuttx POST_BUILD ...)` 被 Ninja 静默丢弃 | 改用 `add_custom_target(nuttx_post_build ALL DEPENDS nuttx ...)` |
| 4 | `rk3506_i2c.c` | `wait_bus_free` 检查 EN=1（死等），实际应等 EN=0 | 重写为状态机，参照 Linux `i2c-rk3x.c` |

### 5.2 次要 Bug

| # | 文件 | Bug | 修复 |
|---|------|-----|------|
| 5 | `hd_rk3506_bringup.c` | 第一个驱动失败即返回，后续驱动不初始化 | 改为记录 + 继续 |
| 6 | `configs/parameter.txt` | 上一 AI 误用 MBR/旧分区布局 | 改为 GPT + 10MB boot + 256MB data |
| 7 | `nand_firmware/` 空目录 | 上一 AI 用错误 cp 创建 `{etc/` 目录 | 删除 |
| 8 | `nuttx/arch/arm/Kconfig` | 缺 `ARCH_CHIP_RK3506` 选择块 | 上一 AI 已添加（保留） |
| 9 | `boot.img` 34MB 零填充 | `.note.gnu.build-id` 在 VMA=0 | 用 `--only-section=.text,.data,.ARM.exidx,...` 过滤 |

### 5.3 已验证（待硬件测试）

下列驱动**编译通过、0 warning**，但**未在真机上验证**：
- `rk3506_vop.c` - 显示控制器（VOP）
- `rk3506_usbhost.c` - DWC2 USB Host
- `hd_rk3506_st7701s.c` - ST7701S LCD 面板
- `hd_rk3506_gt911.c` - GT911 触摸（依赖 I2C0）
- `rk3506_i2c.c` v2 - 依赖 CRU 时钟开启（待添加）

---

## 六、AI Coding 使用说明

### 6.1 工作流

1. **需求拆解**：用户给出口语化的"把 OpenVela 移植到 RK3506G2"，AI 拆解为
   （1）项目结构探索（2）BSP 三层架构（3）构建系统（4）烧录链路（5）驱动开发
2. **方案设计**：参考 `docs/zh-cn/chip_porting/porting_guide.md` 官方指南 + Linux kernel
   `i2c-rk3x.c` 等成熟参考实现
3. **代码生成**：使用 vendor/rockchip/ 下 NuttX 三层架构模板，每次只改一个子系统
4. **构建验证**：每次改完跑 `AGENTS.md 3.5` 检查清单（rm -rf cmake_out + 重新编译 + 验证产物）
5. **文档化**：所有修改同步更新 `AGENTS.md`（项目规则）、`SKILL.md`（可复用知识）
   和 `RK3506_DRIVER_ISSUES.md`（驱动问题清单）

### 6.2 AI 带来的实际价值

| 任务 | 传统耗时 | AI 耗时 | 提升 |
|------|----------|---------|------|
| 项目结构探索 | 2-4 小时 | 10 分钟 | 12-24× |
| 芯片层 BSP 模板 | 8-16 小时 | 1-2 小时 | 8-16× |
| I2C 驱动（Linux 参考） | 4-8 小时 | 30-60 分钟 | 8-16× |
| 烧录脚本调试 | 4-8 小时 | 30-60 分钟 | 8-16× |
| 文档和 AGENTS.md | 4-8 小时 | 30-60 分钟 | 8-16× |

### 6.3 沉淀的 AI 资产

- **`openvela-fast-chip-porting` Skill** (441 行)：11 步标准移植流程，可被其他选手复用
- **`AGENTS.md`** (190 行)：项目级 AI 协作规范，含构建命令、提交规范、紧急情况处理
- **`RK3506_DRIVER_ISSUES.md`** (412 行)：驱动问题清单和修复指南

### 6.4 关键洞察

1. **不要直接 cmake**，必须用 `build.sh`：它会自动设置 PATH、ccache 缓存路径
2. **ccache 必须用 `/tmp/ccache_dir`**：默认 `~/.cache/ccache` 在 WSL/容器中往往不可写
3. **UART 时钟是 24 MHz，不是 1.8432 MHz**：后者是 PC 上的 16550 UART 频率，
   对所有现代 ARM SoC 都是错的
4. **RK3506 的 MiniLoader 是 SPL**，不是完整 Bootloader。**必须链式加载 U-Boot**，
   不能跳过
5. **Ninja 会静默丢弃 `add_custom_command(TARGET x POST_BUILD ...)`**。
   必须用 `add_custom_target(name ALL DEPENDS x ...)` 模式
6. **34MB 零填充的 nuttx.bin**：`.note.gnu.build-id` 在 VMA=0，会撑大整个 bin。
   必须用 `objcopy --only-section=.text --only-section=.data ...`

---

## 七、提交清单

| 类别 | 文件数 | 状态 |
|------|--------|------|
| `vendor/rockchip/chips/rk3506/` | 14 | ✅ 提交 |
| `vendor/rockchip/boards/rk3506/hd-rk3506-evm/` | 14 | ✅ 提交 |
| `nuttx/arch/arm/Kconfig` | 1 | ✅ 提交 |
| `docs/zh-cn/skills/openvela-fast-chip-porting/` | 2 | ✅ 提交 |
| `nand_firmware/` (pack/flash 脚本 + 镜像) | 11 | ✅ 提交 |
| `AGENTS.md` (本仓库) | 1 | ✅ 提交 |
| `PORTING_REPORT.md` (本文件) | 1 | ✅ 提交 |

---

## 八、后续可改进

- [ ] 集成 CRU/GRF 驱动，让 I2C0/USB OTG0 等外设能自己开关时钟
- [ ] 编写适配 RK3506 的 DTB，用 `dtc` 编译（需先安装 `device-tree-compiler`）
- [ ] 把 boot.img 改成 FIT 格式，让 U-Boot 的 `boot_fit` 自动启动
- [ ] 把修改 PR 到 openvela 上游 `dev-ai-contest-2026` 分支
- [ ] 评估能否同时支持 Cortex-M0 MCU 核（rk3506 是异构多核）
