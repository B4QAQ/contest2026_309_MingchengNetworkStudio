# RK3506G2 openvela 移植 — HD-RK3506-EVM

## 一、作品简介

本作品完成了 Rockchip RK3506G2 芯片到 openvela (NuttX) 的完整 BSP 移植，基于 HD-RK3506-EVM 开发板实现从零启动到全功能运行。

**核心亮点**:

- **3×Cortex-A7 + 1×Cortex-M0** 异构架构完整支持
- **128MB DDR3** 内存管理
- **9 大外设驱动**：UART、I2C、SPI、FSPI、GMAC、USB Host、VOP、SARADC、PWM
- **完整网络栈**：DHCP、DNS、NTP、curl HTTPS
- **A/B OTA 升级**：双分区热升级支持
- **rpmsg 多核通信**：A7↔M0 邮箱驱动

---

## 二、选题方向

**新硬件适配赛道**

选题理由：

1. RK3506G2 是瑞芯微最新低功耗 IoT 芯片，openvela 官方尚未支持
2. 异构架构（A7+M0）带来 rpmsg 多核通信挑战
3. 完整外设覆盖（网络/USB/显示/存储）验证 openvela 可扩展性
4. 为后续 RISC-V/ARM64 移植提供方法论参考

---

## 三、目录结构

```
contest2026_309_MingchengNetworkStudio/
├── board/
│   └── vendor/
│       └── rockchip/
│           ├── boards/
│           │   └── rk3506/
│           │       └── hd-rk3506-evm/    # 板级 BSP
│           │           ├── CMakeLists.txt
│           │           ├── Kconfig
│           │           ├── configs/nsh/defconfig
│           │           ├── include/board.h
│           │           ├── scripts/ld.script
│           │           └── src/           # 板级驱动 (9 个)
│           └── chips/
│               └── rk3506/               # 芯片驱动 (24 个)
│                   ├── rk3506_gmac0.c     # 以太网
│                   ├── rk3506_usbhost.c   # USB Host
│                   ├── rk3506_vop.c       # 显示控制器
│                   ├── rk3506_rptun.c     # rpmsg 多核
│                   └── ...
├── openvela/                             # openvela 源码 (submodule)
│   ├── nuttx/                            # 内核
│   ├── apps/                             # 应用
│   ├── external/                         # 第三方库 (curl, mbedtls)
│   └── vendor/rockchip/                  # 本作品代码
├── RK3506G2/                             # Linux SDK (参考)
├── HD-RK3506-EVM/                        # 开发板文档
├── .claude/skills/
│   └── openvela-chip-porting/SKILL.md   # 芯片移植方法论
├── AGENTS.md                             # AI 协作规范
├── HANDOFF.md                            # 项目交接文档
└── README.md                             # 本文件
```

---

## 四、运行方式

### 4.1 环境准备

```bash
# 1. 克隆仓库
git clone https://github.com/B4QAQ/contest2026_309_MingchengNetworkStudio.git
cd contest2026_309_MingchengNetworkStudio

# 2. 拉取 openvela 源码
repo init -u https://github.com/open-vela/contest2026_309_MingchengNetworkStudio \
  -b dev-ai-contest-2026 -m contest2026_309_MingchengNetworkStudio.xml
repo sync -c -j8

# 3. 配置工具链
export CCACHE_DIR=/tmp/ccache_dir
```

### 4.2 编译固件

```bash
cd openvela
rm -rf cmake_out/hd-rk3506-evm_nsh
CCACHE_DIR=/tmp/ccache_dir \
PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j$(nproc)
```

**成功标志**: `#### build completed successfully`

### 4.3 打包镜像

```bash
bash nand_firmware/pack_nand.sh
```

**产物**: `nand_firmware/update.img`

### 4.4 烧录运行

```bash
# 首次烧录（Loader 模式）
sudo upgrade_tool uf nand_firmware/update.img

# 板上 OTA 升级
nsh> ota update /data/boot.fit
nsh> reboot
nsh> ota confirm
```

### 4.5 功能验证

```bash
nsh> ping 192.168.10.1                    # 网络连通性
nsh> curl -v https://stdl.b4qaq.cn/fwtb/info.json  # HTTPS 请求
nsh> mount -t vfat /dev/sda /mnt/usb      # U 盘挂载
nsh> rpmsgtest                             # rpmsg 多核测试
nsh> ota status                            # OTA 状态
```

---

## 五、AI Coding 使用说明

### 5.1 协作模式

本项目采用 **人机协作** 模式：

- **AI 负责**：代码实现、调试分析、文档生成
- **选手负责**：硬件测试、方案决策、最终验收

### 5.2 AI 工具链

| 工具                            | 用途        | 使用场景           |
| ------------------------------- | ----------- | ------------------ |
| **MiMoCode**              | AI 编程助手 | 代码生成、调试分析 |
| **DSH**                   | 对话管理    | 会话记录、日志导出 |
| **contest-log-collector** | 日志归集    | 自动收集 AI 对话   |

### 5.3 关键 AI 贡献

1. **curl 挂死根因定位**

   - 现象：所有 curl 命令卡死，需 Ctrl+C
   - AI 分析：通过 `bt` 命令抓调用栈，定位到 `Curl_socketpair → accept4`
   - 根因：TCP socketpair 在 loopback 未配置时 accept 永久阻塞
   - 修复：`#define CURL_DISABLE_SOCKETPAIR 1`
2. **rpmsg 通信失败排查**

   - 现象：A7↔M0 通信超时
   - AI 分析：时钟门控写反（SET_TO_DISABLE 模式）
   - 修复：正确配置 PCLK_MAILBOX 门控
3. **USB bulk 传输卡死**

   - 现象：U 盘枚举成功，读写卡死
   - AI 分析：DWC2 RX FIFO 太小（128 words）
   - 修复：提到 SDK 值 512/256/224

### 5.4 AI Coding 日志

完整对话记录见 `logs/B4QAQ/` 目录，包含：

- 116 个提交的开发过程
- 37 个 bug 的调试细节
- 方案决策的讨论记录

---

## 六、技术亮点

### 6.1 异构多核支持

- **Cortex-A7**: 运行 openvela 主系统
- **Cortex-M0**: 运行 rpmsg 服务端
- **邮箱中断**: A7↔M0 零拷贝通信
- **停核机制**: 两级停核 + 金丝雀验证

### 6.2 完整网络栈

- **GMAC 驱动**: DMA 描述符 + PHY 管理
- **DHCP 客户端**: 自动 IP 配置 + link-up 重试
- **DNS 解析**: 同步/异步双模式
- **curl HTTPS**: TLS 1.2 + CA 证书验证

### 6.3 存储子系统

- **SPI NAND**: FSPI 控制器 + MTD 驱动
- **dhara**: 磨损均衡层
- **littlefs**: 嵌入式文件系统
- **A/B OTA**: 双分区热升级

### 6.4 显示子系统

- **VOP 控制器**: RGB LCD 输出
- **ST7701S**: 480×854 面板初始化
- **LVGL**: 嵌入式 GUI 框架
- **帧缓冲**: /dev/fb0 设备

### 6.5 USB Host

- **DWC2 控制器**: 高速 USB 2.0
- **MSC 类**: U 盘自动枚举
- **FAT 文件系统**: vfat 挂载
- **热插拔**: link-change 检测

---

## 七、已完成功能

### 7.1 基础系统

- [X] NSH 命令行 (115200 波特率)
- [X] 内存管理 (128MB DDR3)
- [X] 进程调度 (Cortex-A7)
- [X] 中断控制器 (GIC)
- [X] 系统定时器 (Generic Timer)

### 7.2 外设驱动

- [X] UART0/1/2/4 (串口)
- [X] I2C0/1/2 (传感器)
- [X] SPI1 (Flash)
- [X] FSPI (SPI NAND)
- [X] GMAC0 (以太网)
- [X] USB Host (U 盘)
- [X] VOP (LCD)
- [X] SARADC (ADC)
- [X] PWM (脉冲宽度调制)
- [X] Watchdog (看门狗)
- [X] RTC (实时时钟)

### 7.3 网络功能

- [X] DHCP 自动配置
- [X] DNS 域名解析
- [X] NTP 时间同步
- [X] curl HTTPS 请求
- [X] ping 连通测试
- [X] iperf 网络性能

### 7.4 文件系统

- [X] ROMFS (/etc)
- [X] tmpfs (/tmp)
- [X] littlefs (/data)
- [X] FAT (U 盘)

### 7.5 应用支持

- [X] Lua 解释器
- [X] QuickJS (JavaScript)
- [X] MiniBASIC
- [X] LVGL (GUI)

### 7.6 OTA 升级

- [X] A/B 双分区
- [X] bootcheck 开机检查
- [X] ota NSH 命令
- [X] 自动回滚

---

## 八、已知限制

### 8.1 硬件限制

| 项目        | 状态        | 说明              |
| ----------- | ----------- | ----------------- |
| GT911 触摸  | ⚠️ 未验证 | 台架未接面板      |
| ST7701S LCD | ⚠️ 未验证 | 台架未接面板      |
| USB Hub     | ❌ 不支持   | 驱动未实现 asynch |
| 音频        | ❌ 未实现   | I2S 驱动待开发    |

### 8.2 软件限制

| 项目           | 状态          | 说明                              |
| -------------- | ------------- | --------------------------------- |
| M0 停核        | ⚠️ 需验证   | 温复位后 M0 状态不确定            |
| OTA 端到端     | ⚠️ 部分验证 | bootcheck 已验证，update 流程待验 |
| 网络长时间运行 | ⚠️ 需测试   | DHCP 租约续期待验                 |

## 九、移植方法论

本项目总结了完整的芯片移植方法论，详见：

**`.claude/skills/openvela-chip-porting/SKILL.md`**

核心要点：

1. **参考资料优先级**: Linux SDK > 已有 NuttX 移植 > 数据手册
2. **分阶段验证**: 最小启动 → 基础外设 → 网络 → 存储 → 显示
3. **调试技巧**: 串口日志 + GDB + 逻辑分析仪
4. **常见陷阱**: 时钟门控、日志宏、FIFO 配置、内存映射

---

## 十、提交记录

| 版本 | 日期       | 主要变更                |
| ---- | ---------- | ----------------------- |
| v8a  | 2026-09-01 | 初始 BSP，NSH 启动      |
| v8b  | 2026-09-05 | rpmsg + USB 诊断        |
| v8c  | 2026-09-08 | USB 传输修复 + 日志规范 |
| v8d  | 2026-09-10 | USB FIFO + rpmsg 修复   |
| v8e  | 2026-09-12 | rpmsg 时钟门控修复      |
| v8f  | 2026-09-13 | M0 停核 + 竞态修复      |
| v8g  | 2026-09-14 | M0 时基时钟补开         |
| v8h  | 2026-09-15 | INTMUX 握手 + 探针      |
| v8i  | 2026-09-16 | 日志清理                |
| v8j  | 2026-09-16 | M0 停核 + NSH 行长      |
| v8k  | 2026-09-17 | iomux 日志删除          |
| v8l  | 2026-09-17 | FSPI 日志 + curl 修复   |

**统计**：

- 总提交：116 个
- Bug 修复：37 个
- 驱动代码：21237 行
- 板级文件：9 个
- 芯片驱动：24 个

---

## 十一、致谢

- **openvela 团队**: 提供 RTOS 框架和技术支持
- **小米老师**: 远程指导 curl 调试
- **组委会**: 组织比赛和资源支持

---

## 十二、联系方式

- **队伍**: MingchengNetworkStudio
- **编号**: 309
- **仓库**: https://github.com/B4QAQ/contest2026_309_MingchengNetworkStudio

---

*本作品参加 2026 首届 openvela AI 硬件开发者大赛，新硬件适配赛道。*
