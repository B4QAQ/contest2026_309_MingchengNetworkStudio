# RK3506G2 openvela 移植 — HD-RK3506-EVM

## 一、作品简介

本作品完成了 Rockchip RK3506G2 芯片到 openvela (NuttX) 的完整 BSP 移植，在 HD-RK3506-EVM 开发板上实现从零启动、网络通信、USB/存储、图形界面与 **A7↔M0 异构 AMP** 的全功能运行。

**核心亮点**：

- **3×Cortex-A7 (SMP) + 1×Cortex-M0 异构 AMP**：U-Boot 从 `amp` 分区加载并放行 M0，M0 跑瑞芯微 HAL 裸机 rpmsg 固件（DRAM 0x03a00000），A7 侧 NuttX 经 mailbox/rpmsg 与 M0 通信；M0 固件与 Linux SDK 产物**逐字节一致**（锁定 SDK gcc 10.2 工具链）
- **28 个芯片级源文件 + 17 个板级源文件**：UART、I2C、SPI、FSPI、GMAC、USB Host、MMC、SARADC、TSADC、PWM、WDT、RNG、rptun 等（早期 RGB 屏的 VOP 驱动已随产品切到 NV3007 SPI 方案删除）
- **完整网络栈**：DHCP（STP 窗口重试）、DNS 兜底、NTP 校时、curl HTTPS（mbedTLS + 板载 CA 证书）
- **SPI NAND 存储栈**：FSPI→MTD→dhara 磨损均衡→littlefs（/data），ROMFS（/etc），FAT（U 盘）
- **A/B 双分区 OTA**：misc 分区 AvbABData + bootcheck try/confirm/回滚
- **428×142 LVGL 图形系统**：NV3007 SPI 面板（竖屏 90° 旋转）、深色主题、4 键操作（UP/DOWN/OK/ESC）、状态/终端/应用/信息四个页面
- **快应用系统**：QuickJS FULL + 手写 LVGL 绑定，`/etc/apps` romfs 内置应用与 `/data/apps` 用户应用并存，ESC 退出
- **rpmsg 多核验证应用**：`rpmsgtest` 与 M0 demo 端点 0x4003 收发 `"Rockchip rpmsg linux test!"`

---

## 二、选题方向

**新硬件适配赛道**

1. RK3506G2 是瑞芯微低功耗 IoT 芯片（3×A7 + M0），openvela 官方此前无 BSP
2. 异构 AMP（A7 Linux/openvela + M0 HAL 固件）在 NuttX 侧无现成范本，需要对齐瑞芯微 Linux SDK 的启动链、内存窗口与邮箱协议
3. 完整外设覆盖（网络/USB/显示/存储/AMP）验证 openvela 的可扩展性
4. 沉淀了一份可复用的芯片移植方法论（见 `skills/openvela-chip-porting/SKILL.md`）

---

## 三、目录结构

```
contest2026_309_MingchengNetworkStudio/
├── openvela/                             # openvela 主代码（.repo 多仓，仅 nand_firmware 入本仓）
│   └── nand_firmware/                    # 烧录镜像与打包脚本
│       ├── parameter.txt                 # GPT 分区表（含 amp 分区）
│       ├── pack_nand.sh                  # update.img 打包
│       ├── build_mcu_fw.sh               # M0 固件构建（锁定 SDK gcc 10.2 工具链）
│       ├── UBOOT_BUILD.md                # AB + AMP U-Boot 构建记录（thunderboot 陷阱）
│       └── amp/amp.img                   # M0 standalone FIT（load 0x03a00000）
│   vendor/rockchip/（.repo 子仓，不入本仓）
│       ├── chips/rk3506/                 # 芯片驱动（28 个 .c，含 drv/ 子目录）
│       └── boards/rk3506/hd-rk3506-evm/  # 板级 BSP（17 个 .c + defconfig + ld.script）
├── vendor/rockchip/                      # ↑ 上述 vendor/rockchip 的源码快照（供审阅）
├── board/vendor/rockchip/                # 同一份快照（比赛 manifest linkfile 目标）
├── mcu_sdk/                              # vendored M0 HAL SDK（可直接编辑的固件源码树）
├── skills/openvela-chip-porting/SKILL.md # 芯片移植方法论（本作品沉淀）
├── logs/B4QAQ/                           # AI Coding 完整对话日志
├── AMP_RK3506_LINUX_SDK_NOTES.md         # AMP 移植对照 Linux SDK 的核查笔记
├── AGENTS.md                             # AI 协作规范与全部技术决策记录
├── contest2026_309_*.xml / openvela.xml  # repo manifest（比赛提交基础设施）
└── README.md                             # 本文件
```

> 注：RK3506G2 Linux SDK、HD-RK3506-EVM 板厂文档等参考资料在本机使用，按比赛规则不入仓（见 `.gitignore`）。

---

## 四、运行方式

### 4.1 环境准备

```bash
# 1. 用 manifest 拉取 openvela 多仓源码
repo init -u https://github.com/open-vela/contest2026_309_MingchengNetworkStudio \
  -b dev-ai-contest-2026 -m contest2026_309_MingchengNetworkStudio.xml
repo sync -c -j8

# 2. 工具链随仓位于 openvela/prebuilts，另需 ccache
export CCACHE_DIR=/tmp/ccache_dir
```

M0 固件构建还需要 Linux SDK 自带工具链 `gcc-arm-none-eabi-10-2020-q4-major`（gcc 10.2.1），`build_mcu_fw.sh` 会定位并对 entry/向量表/大小做硬断言（见 `mcu_sdk/README.md`）。

### 4.2 编译 A7 固件

```bash
cd openvela
rm -rf cmake_out/hd-rk3506-evm_nsh
CCACHE_DIR=/tmp/ccache_dir \
PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j$(nproc)
```

**成功标志**：`#### build completed successfully`，产物 `cmake_out/hd-rk3506-evm_nsh/nuttx.bin`（即 vela.bin）。

### 4.3 构建 M0 固件并打包整盘镜像

```bash
bash nand_firmware/build_mcu_fw.sh   # 产出 nand_firmware/amp/amp.img（32256 B，FIT load 0x03a00000）
NUTTX_BIN=$PWD/cmake_out/hd-rk3506-evm_nsh/nuttx.bin bash nand_firmware/pack_nand.sh
```

**产物**：`nand_firmware/update.img`（约 9.4 MB）。

### 4.4 烧录运行

```bash
# Loader 模式（短接 RECOVERY 插 USB），整盘烧录；布局变更后必须整盘刷
sudo upgrade_tool uf nand_firmware/update.img
```

启动顺序：MiniLoader → U-Boot（AB 选择 + `amp_cpus_on()` 从 amp 分区放行 M0）→ NuttX A7 SMP。
M0 调试串口为 **UART1**（GPIO0_C1=TX / GPIO0_C0=RX，115200），启动应打印 `[HAL INFO] Hello RK3506 mcu`。

板上 OTA：

```bash
nsh> ota update /data/boot.fit
nsh> reboot
nsh> ota confirm
```

### 4.5 功能验证

```bash
nsh> free                                       # 约 25.5 MiB 堆（0x02080000..0x03a00000）
nsh> ping 192.168.10.1
nsh> curl https://stdl.b4qaq.cn/fwtb/info.json # HTTPS
nsh> mount -t vfat /dev/sda /mnt/usb            # U 盘
nsh> rpmsgtest                                  # A7→M0 rpmsg，期望 PASS
nsh> ota status                                 # A/B 槽位状态
```

板载 4 键（J9）：UP/DOWN 移动焦点，OK 确认，ESC 为返回键（菜单页回主页，快应用内退出应用）。

---

## 五、AI Coding 使用说明

### 5.1 协作模式

- **AI 负责**：代码实现、寄存器/协议级根因分析、文档
- **选手负责**：硬件接线、烧录、板上验证、方案拍板，疑难问题方案指导

规范与全部决策记录见 `AGENTS.md`（每条结论附版本号、源码位置和被否决方案）。

### 5.2 AI Coding 日志

完整对话记录在 `logs/B4QAQ/`，随比赛日志归集工具自动入仓（只入 openvela 工作区内的会话）。

### 5.3 代表性攻关与分工

本项目所有架构/方案决策均由选手拍板（见 `AGENTS.md` §3.6），AI 只在选定方向上做实现、源码/寄存器级验证和文档，不自行决定技术路线。几个代表性过程：

1. **AMP 异构（最艰难的一战）**：经历 v8→v9→v10 多轮失败。"M0 生命周期改由 U-Boot 从 `amp` 分区放行、NuttX 只跑协议不碰 M0"（方案 A）是选手在多轮分歧中坚持定的方向；在方向被否、排查停滞时也是选手推动继续。AI 的工作是按此读 U-Boot/SDK 源码逐寄存器核对、板上日志取证，最终在该方向下定位到 U-Boot 误带 `rk3506_tb` fragment（`CONFIG_SPL_KERNEL_BOOT=y` 使 SPL 直跳内核、`amp_cpus_on()` 从不执行）和 gcc 13.4 导致 entry 漂移（0x3a00141→0x3a00171）两个根因
2. **TLS/网络连环问题**：排查方向由选手推动并拍板每个改法；AI 负责从源码定位无电池 RTC 的证书未来时间、DHCP 不带 option 6、curl threaded resolver 误导报错等具体机理并落地修复
3. **USB bulk 传输卡死**：AI 在源码比对中定位的代码级根因——DWC2 RX FIFO 默认 128 words 小于 HS bulk 所需 ~131 words，核心静默不发 token；按 SDK 值改为 512/256/224
4. **rpmsg 时钟门控写反**：AI 定位的代码级错误——Rockchip 门控是 SET_TO_DISABLE 语义，写 `WE|1` 实为关时钟，等于自己把 PCLK_MAILBOX 关死

---

## 六、技术亮点

### 6.1 异构 AMP（A7 SMP + M0）

- M0 固件 **link/load 在 DRAM 0x03a00000（1MB）**，rpmsg 共享窗 0x03c00000（2MB）；NuttX 堆收缩到 0x03a00000 让出四个 1MB no-map 预留窗
- U-Boot：`rk3506` defconfig + `vanxoak_ab` + `rk3506-amp-mcu`（**不含** thunderboot `rk3506_tb`），FIT standalone `fit_standalone_release()` 经 SIP 调用放行 M0
- NuttX 侧 `rk3506_rptun.c`：只开时钟 + 只读 INTMUX 就绪探测 + mailbox/vring 协议，不碰 M0 复位/拷贝/放行
- 邮箱：M0 kernel id 3、link-id 0x03；A7→M0 走 mailbox3 A2B，M0→A7 走 mailbox0 B2A（电平中断，w1c）
- 固件可复现：锁定 SDK gcc 10.2.1，entry=0x3a00141、向量表 `00fcaf03 4101a003`、27504 B 全部硬断言

### 6.2 LVGL UI 与快应用

- NV3007 面板原生竖屏 142×428，LVGL 逻辑分辨率 428×142（旋转 90°），PARTIAL 渲染
- 4 键 KEYPAD indev；**ESC 不走 LVGL 焦点投递**（`lv_group_send_data` 只发给焦点对象且 KEY 事件默认不冒泡），而由板级 back handler 栈处理，因此快应用内任意控件持有焦点时 ESC 都可靠退出
- QuickJS FULL 运行时 + 手写 LVGL 绑定（同进程 lv_timer 任务），romfs `/etc/apps` + 可写 `/data/apps`

### 6.3 其他子系统

- **网络**：GMAC DMA + YT8512 PHY；DHCP 重试 10 次覆盖交换机 STP 窗口；DNS 网关兜底 + 5s 超时；ntpc 指数退避校时；GMAC 运行时完全静默（无 link/error 打印）
- **存储**：FSPI DLL training → MTD → mtd_partition → dhara → littlefs（`-o autoformat`）；A/B 双 10MB boot 槽
- **USB Host**：OTG1 控制器（otg0 是烧录口）+ inno-usb2 PHY 唤醒；FIFO 512/256/224；MSC + vfat
- **TLS**：BLAKE2s 熵池喂 /dev/urandom，板载 121 根证书 bundle

---

## 七、已完成功能

- [x] NSH 控制台（UART0，115200，Ctrl+C / 256 列命令行）
- [x] 3×A7 SMP、GIC、generic timer、25.5 MiB 堆
- [x] UART / I2C / SPI1 / FSPI / GMAC0 / USB Host / MMC / SARADC / TSADC / PWM / WDT / RNG
- [x] SPI NAND：dhara + littlefs /data、ROMFS /etc、tmpfs /tmp、FAT U 盘
- [x] 网络：DHCP / DNS / NTP / ping / iperf / curl HTTPS
- [x] A/B OTA（bootcheck + ota 命令）
- [x] NV3007 LVGL UI（4 键、4 页面）+ QuickJS 快应用
- [x] AMP：U-Boot 放行 M0、rpmsg 链路、`rpmsgtest` 应用

---

## 八、已知限制 / 待板上验证

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| AMP 端到端 | ⚠️ 待最终验收 | M0 固件已与 SDK 产物逐字节一致、U-Boot 已见 `Handle standalone: 'mcu' at 0x03a00000 OK`；待验 M0 UART1 `Hello RK3506 mcu` 与 `rpmsgtest` PASS |
| GT911 触摸 / ST7701S RGB 屏 | ❌ 方案已弃用 | 旧板厂屏驱动在 NV3007 转型时整体删除（vendor 子仓 285e5a6）；当前产品显示是 NV3007 SPI 小屏 + 4 键 |
| USB Hub | ⚠️ 待测 | asynch 传输框架已实现，hub 枚举未在板上验证 |
| OTA 端到端 | ⚠️ 部分验证 | bootcheck/try/confirm 已验，`ota update` 整链路待测 |
| 音频 | ❌ 未实现 | I2S 驱动未开发 |
| M0 温复位 | ⚠️ 架构变更 | v10 为 U-Boot 放行模型，温复位由 U-Boot 每次放行前保证，NuttX 不再做停核 |

---

## 九、移植方法论

完整方法论见 **`skills/openvela-chip-porting/SKILL.md`**，核心：

1. **事实基准**：寄存器/时序只认 Linux SDK + 已适配的同架构 NuttX 移植（R258），两处都没有就停下问，不凭记忆发明
2. **分阶段验收**：最小启动 → 存储 → 网络 → USB → 显示 → AMP，每阶段板上验证后再进下一阶段
3. **高价值教训**：Rockchip SET_TO_DISABLE 门控语义、`_err/_warn/_info` vs 静默的 `ierr/iinfo`、DWC2 FIFO、thunderboot 绕过 board_late_init、M0 工具链必须锁版本、LVGL 焦点事件不冒泡
4. **决策留痕**：AGENTS.md 记录每个根因、被否决方案和版本演进，错误结论显式标注作废

---

## 十、版本演进（要点）

| 版本 | 主要变更 |
| --- | --- |
| v8a–v8d | 最小 BSP、NSH、GMAC 基础、USB 枚举与 FIFO 根因 |
| v8e–v8h | rpmsg 时钟门控修正、邮箱竞态、M0 时基时钟、INTMUX 就绪握手，rpmsgtest 首版 PASS |
| v8i–v8m | 全仓日志治理（GMAC/iomux/FSPI 静默）、NuttX 停核方案、curl/DHCP/DNS/TLS 连环修复 |
| v9 | AMP 架构重构：M0 生命周期改由 U-Boot 从 amp 分区放行，NuttX 只跑协议 |
| v10 | 对齐 Linux SDK 重做：去掉 thunderboot fragment、M0 移到 DRAM 0x03a00000、锁定 gcc 10.2（产物与 SDK 逐字节一致）；UI 4 键 + 快应用系统；返回键板级化、rptun 诊断脚手架清零 |

提交数：222+（持续更新中）。

---

## 十一、致谢

- openvela 团队与大赛组委会
- 瑞芯微 RK3506 Linux SDK 与 U-Boot 源码（本移植的事实基准）

---

*本作品参加 2026 首届 openvela AI 硬件开发者大赛 · 新硬件适配赛道 · 队伍 MingchengNetworkStudio（编号 309）。*
