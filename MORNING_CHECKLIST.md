# 隔夜工作验收清单 (2026-02-28 夜间 → 早晨)

> 本轮目标: "将所有 Vela 支持的能力都进行适配" 收尾 — **A/B 双分区 OTA** 全套实现。
> 上一轮已交付: CAN / WDT / SARADC / PWM / AMP rpmsg (A7 侧 rptun + M0 启动 + rpmsgtest)。
> 本轮新增: OTA A/B 双槽 + ota NSH 命令 + FSPI 并发锁 + /data 分区位置修复。
> 音频 / RTC / WiFi-BT 按你的决定跳过。

---

## 0. 产物

| 文件 | 大小 | 说明 |
|------|------|------|
| `openvela/nand_firmware/update.img` | md5 f915312cb69a9717be9ae2076184bc1e | **全量刷机包 v8l** (v8k 全部 + **FSPI 启动日志已删**) |
| `openvela/cmake_out/hd-rk3506-evm_nsh/vela.bin` | 2,889,724 B | NuttX 固件 (含 /dev/ota + USB host v8c+v8d **已板上验收** + littlefs/FAT + 驱动日志全量可见) |
| `openvela/nand_firmware/boot.fit` | 4,194,304 B | 单槽 FIT 镜像 (ota update 用它) |
| `openvela/nand_firmware/parameter.txt` | — | v5 A/B 分区表 |

提交记录:
- vendor/rockchip 独立仓 (openvela-rk3506-scripting 分支):
  - `5d41b78` chip: SPI NAND A/B OTA 槽位管理器 /dev/ota
  - `341927d` board: OTA NSH 命令 + A/B 分区表 + bootcheck 开机钩子
  - `fb92df9` fix(chip): USB host 根因修复 — conn/drvr 接口重叠 + 切换 OTG1 + INNO PHY 上电 (v7)
  - `215a262` fix(board): /data userdata 起始扇区修正 0x14800→0x10800 (v7)
  - `7db9a88` board: /data 切换 dhara+littlefs, 修复 mount failed:25 (v8)
  - `8c745bd` board: defconfig 开 FAT/LFN, 补齐 U 盘挂载配置缺口 (v8)
  - `cb40931` board: rpmsgtest 阻塞读改 poll+超时, 端点改名避开 NS 冲突 (v8)
  - `bcc74cc` fix(chip,board): rpmsg 共享窗改 uncached 映射 (根因) + USB/rpmsg 诊断 (v8b)
  - `3ff636a` board: defconfig 开 DEBUG_USB_WARN/INFO 诊断跟踪 (v8b)
  - `6fc7ffb` fix(chip): USB host 传输完成机制修复 — ACK 判据 + 进展感知等待 (v8c)
  - `dbb8b4b` fix(chip,board): 驱动日志宏误用 INPUT 子系统 ierr/iwarn/iinfo, 全部静默 (v8c)
  - `97b96d1` fix(chip): DWC2 RX FIFO 128 卡死 HS bulk, 提到 SDK 值 512/256/224 (v8d)
  - `f4e7c4b` board: rpmsgtest 超时探针加邮箱寄存器转储, 定位 M0 kick 链断点 (v8d)
  - `1d25a3d` fix(chip): rk3506_rptun mbox 时钟门控写反, PCLK_MAILBOX 被关死致 rpmsg 全链路失效 (v8e)
  - `0f757ef` board: U 盘挂载点改 /mnt/usb (mount ENOTDIR 根因) + rpmsgtest 邮箱探针 v2 (v8e)
  - `3e2e503` fix(chip): mcu_boot 等 M0 武装邮箱接收后再放行 kick, 修复丢边沿竞态 (v8f)
  - `48aa2fc` fix(chip): 补开 M0 时基时钟 STCLK_M0/PCLK_TIMER/CLK_TIMER0_CH5 (v8g)
  - `ae86676` fix(chip): 用 INTMUX 使能位做 M0 就绪握手, 补向量表/INTMUX/复位探针 (v8h) — **rpmsg 板上 PASS**
  - `2577148` build(board): 关掉 USB DEBUG_INFO/WARN, 只保留 ERROR (v8i)
  - `afed146` chip(gmac0): 按用户要求移除全部网络日志 (v8i)
  - `9f3f034` chip(rptun)+board: 删除 rpmsg 探针块与 info/warn 日志, 保留 err (v8i)
  - `26822bb` chip(rptun): 加载固件前先把 M0 停住 (v8j) — **修复概率性 Data abort**
  - `b544287` build(board): 放宽 NSH 命令行长度与参数个数 (v8j)
  - `25f2aed` chip(iomux): 删除 iomux 日志 (v8k)
  - `9d60ef5` chip(fspi): 删除 FSPI 启动日志 (v8l)
  - external/curl/curl `f1a6fef21` fix: mbedtls_close 仅在 close_notify 已到达时读 (v8c) — **v8e 已按用户要求回退**
  - external/curl/curl `9a4601247` test: P1-P6 无缓冲定位探针 (v8d) — **v8e 已按用户要求回退**
  - external/curl/curl **v8e: 回退到仓库原始版本 `8c2a01f3e`**（curl 源码不再有任何本地改动）
- 主仓 dev-ai-contest-2026:
  - `5d6f269` pack: A/B 双分区 OTA 打包 (v5) + OTA 固件二进制
  - `ca08796` chore: gitignore .mcu_build

⚠️ **分区布局变了 (v5)**, 与旧镜像不兼容, 必须 `upgrade_tool uf update.img` 全量刷, 不能只刷 boot。
⚠️ **v7 的 /data 起始扇区更正为 0x10800** (boot_b 结束处, 33MiB; v6 误写 0x14800)。
旧 /data 数据丢失, 首启 rcS 会自动格式化 (v8: littlefs -o autoformat), 无需手动干预。

---

## 1. 新分区表 (v5, A/B)

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| vnvm | 1MB | 2MB | NV (MAC 等, 不变) |
| uboot | 3MB | 8MB | U-Boot (不变) |
| misc | 11MB | 2MB | BCB(偏移0) + **AvbABData(偏移2048)** 槽位元数据 |
| boot_a | 13MB | 10MB | NuttX FIT — 槽 A |
| boot_b | 23MB | 10MB | NuttX FIT — 槽 B |
| userdata | 33MB | 223MB | /data (dhara+littlefs, v8 改; v7 误配 SmartFS)。**v7 更正: 起始 0x10800 扇区 = 33MiB (boot_b 结束处); v6 误写 0x14800=41MiB 留了 8MiB 空洞** |

recovery/system/vendor/oem/data 已删除 (无 Android 恢复流程, 不需要)。

---

## 2. OTA A/B 验收步骤 (本轮核心)

> **前置条件 (2026-08-30 更新)**: 预编译 U-Boot 无 `CONFIG_ANDROID_AB`, 只认字面
> `boot` 分区 → v5 首刷卡死 `FIT: No boot partition`。已用 SDK 源码重编出
> 支持 A/B 的 U-Boot (见 `nand_firmware/UBOOT_BUILD.md`), 已打进当前
> `update.img`。上电时 U-Boot 应打印 `A/B-slot: _a, successful: 0, tries-remain: 7`
> ——看到这行说明新 U-Boot 在正常选槽。

### 2.1 首刷后基础检查

```bash
nsh> ota status
```
预期 (misc 全零 → U-Boot 首次启动自动写默认元数据):
```
OTA: metadata valid, last_boot=a
  slot_a: priority=15 tries=7 successful=1 fit=yes   ← 首刷后可能是 0 tries 烧掉几次, 见 2.2
  slot_b: priority=14 tries=7 successful=0 fit=yes
  active: slot_a (U-Boot would boot this now)
```
(首刷时 boot_a/boot_b 烧的是同一个镜像, 所以两个槽 fit=yes 是正常的。)

### 2.2 ota confirm (固化当前槽)

每次新槽首次启动后, rcS 的 `ota bootcheck` 会烧掉一个 try; 7 次内不
`ota confirm` 就会回滚。健康检查完就固化:

```bash
nsh> ota bootcheck   # 手动跑一次看行为: "slot_x not confirmed yet, try burned (N left)"
nsh> ota confirm     # → "slot_x marked successful (permanent)"
nsh> ota status      # successful=1, 之后 bootcheck 不再烧 try
```

### 2.3 完整 OTA 升级流程 (核心验收)

前提: 新固件镜像 `boot.fit` 放到 /data (U 盘或网络):

```bash
# 方式 A: U 盘
mkdir -p /tmp/usb && mount -t vfat /dev/sda /tmp/usb
cp /tmp/usb/boot.fit /data/boot.fit

# 方式 B: 网络 (TFTP/HTTP 皆可)
curl -o /data/boot.fit http://192.168.10.100:8000/boot.fit

# 执行 A/B 升级 (自动写"非当前槽", 全量擦除→流式写→逐字节回读校验→激活)
nsh> ota update /data/boot.fit
```
预期输出顺序:
1. `ota: writing /data/boot.fit (4194304 bytes) to slot_b` (当前 A → 写 B)
2. `ota: wrote 4194304 bytes into slot_b`
3. `ota: readback verify OK (4194304 bytes)` ← 逐字节校验必须 OK
4. `ota: slot_b activated (priority 15, tries 7). Run reboot...`

```bash
nsh> reboot
# 重启后:
nsh> ota status    # active: slot_b, successful=0
nsh> ota confirm   # 固化 B; 若不固化, 7 次重启后自动回滚到 A
```

### 2.4 回滚验收 (可选但建议)

刷一个"故意坏的"镜像验证保护逻辑 — 注意: 镜像内容损坏会被**回读校验**拦下
(不会 activate), 所以回滚测试要模拟"镜像合法但启动后异常":

```bash
# 让新槽跑着但不 confirm, 连续重启 7 次 (或手动 ota boot-a 切回旧槽)
# 第 7 次启动时 bootcheck 输出: "ROLLBACK: slot_b tries exhausted,
#   marked unbootable; slot_a restored as boot target"
nsh> ota boot-a      # 也可手动随时切回 A
```

### 2.5 ota 命令速查

| 命令 | 作用 |
|------|------|
| `ota status` | 元数据 + 两槽状态 + 当前 active |
| `ota bootcheck` | 烧 try / 耗尽回滚 (rcS 每次开机自动跑) |
| `ota confirm` | 当前槽 successful=1 (永久) |
| `ota boot-a` / `ota boot-b` | 手动切换启动槽 (校验 FIT magic) |
| `ota update <file> [-f]` | 擦非活动槽 → 写 → 校验 → 激活; `-f` 强制 (不推荐) |

---

## 3. v7 USB host 修复验收 (2026-08-30 追加)

**v6 首刷崩溃根因** (`rk3506_usbhost.c:2136` DEBUGASSERT, usbhost_waiter 线程):
三层叠加 bug, 已全部修复:
1. **接口结构重叠 (真正的崩溃元凶)**: 驱动把整个 `rk3506_usbhost_s` 强转成
   `usbhost_connection_s`, 但结构体第一个成员是 `usbhost_driver_s` —
   `conn->wait` 别名到 `drvr->ep0configure`。waiter 线程首次 CONN_WAIT 就带着
   垃圾参数调进 ep0configure 触发断言 (崩溃发生在任何 USB 连接事件之前)。
2. **初始化了错误的控制器**: 板上 host 口是 OTG1 (USB-A, DTS `dr_mode=host`),
   旧代码 host 初始化 OTG0 — 那是 Type-C 烧录口 (`dr_mode=peripheral`)。
3. **INNO USB2 PHY 从未上电**: POR 后 GRF phy_sus=0x1d1 (suspend), UTMI 线态
   全是垃圾 → 即使接了真设备也枚举不到/读到垃圾描述符。

已验证 (二进制级): conn.wait/enumerate 指向真正的 rk3506_wait/rk3506_enumerate;
drvr->ep0configure 增加 maxpacketsize>64 clamp + funcaddr 校验 (垃圾枚举优雅失败
不再 panic); DEBUG_USB_ERROR 打开 (USB 错误日志此前全部静默)。

### 3.1 U 盘验收 (重要: 插对口!)

- **插板边缘的 USB-A 大口 (host 口)**, ❌ 不是 Type-C (那是烧录口, 对面是 PC)。
- 开机日志预期: `USB Host (OTG1, USB-A) initialized, waiter started`
- 插 U 盘:
  ```bash
  nsh> ls /dev          # 预期出现 sda
  nsh> mkdir -p /tmp/usb && mount -t vfat /dev/sda /tmp/usb
  nsh> ls /tmp/usb      # 看到 U 盘文件即通过
  ```
- 若枚举失败: 现在 DEBUG_USB_ERROR 已开, 会打 `ERROR: ...` 具体
  原因 (此前崩溃前一行日志都没有就是因为这个被静默)。

### 3.2 GT911 触摸 -110: 属预期, 不是 bug

你确认过台架上面板没接。SDK 里 480×800 ST7701S 面板的 dtsi 本来就没有
gt911 节点。product ID 读 -110 ETIMEDOUT 忽略即可; 接上面板后仍失败再查。

### 3.3 已知限制 (记录在案, 不阻塞验收)

- **USB 集线器不可用**: defconfig 的 USBHOST_HUB=y 此前被 Kconfig 依赖
  静默丢弃, v7 修复 select 后真正生效, 但驱动未实现 asynch 接口 —
  插 USB hub 会崩。U 盘/HID 直插走同步路径, 不受影响。hub 支持要做的话
  需补驱动 asynch 实现 (按 3.6 走)。

---



**/data 分区位置错误 (潜伏 bug, 修复 + v7 更正)**

- 旧 bringup 把擦块索引当 mtd_partition 的页索引传, 导致 /data 实际映射在
  **3.7MB 处的 384KB** (落在 uboot 分区内!), 而不是 229MB 的 userdata 区。
- 之前没炸是因为: 每次全量刷 update.img 都会重写 uboot 分区, 把 dhara
  格式化造成的破坏"治愈"了; 且 U-Boot 已在内存里运行, 当次不受影响。
- v6 修复页单位后起始扇区误写 0x14800 (41MB); **v7 更正为 0x10800 (33MB,
  boot_b 结束处, 与 parameter.txt 一致)**。旧 /data 数据会丢, 首次启动
  rcS 检测 mount 失败会自动重新格式化 (v8: littlefs -o autoformat), 无需手动干预。
  KVDB 需重新配置。
- 顺带: mtd.h 里 mtd_partition 的 "firstblock - offset in bytes" 注释是
  过时的 (实际单位 = geo.blocksize 块), 代码里已写注释说明。

**FSPI 并发锁**

- 修复前多个 MTD 分区消费者 (/data 的 dhara 与 OTA 的 misc/boot_a/boot_b)
  可并发进 FSPI 控制器, 操作交错会损坏传输。已在 `rk3506_fspi_nand_op`
  加互斥锁串行化。

---

## 4. OTA 设计决策记录 (3.6 要求, 夜间自主决策, 早晨可否决)

| 决策 | 选择 | 理由 / 备选 |
|------|------|------------|
| OTA 路线 | 完整 A/B 双槽 (你已拍板, "做完整") | 备选单槽+recovery 已否决 |
| 元数据格式 | U-Boot 原生 AvbABData @ misc+2048 | 与 prebuilt U-Boot (CONFIG_ANDROID_AB) 兼容, 不改 U-Boot; 备选自造格式需换 U-Boot |
| try 递减 | 用户态 rcS `ota bootcheck` | U-Boot boot_fit 只选槽不递减 (反汇编+源码确认); 备选改 U-Boot 违反"不重编 U-Boot"现状 |
| 回滚语义 | tries 耗尽 → 槽 priority=0, 另一槽恢复 15/7 | 与 Android bootctl 语义一致 |
| 确认方式 | 手动 `ota confirm` | 自动确认会失去重试保护; 你可否决为 rcS 里 health-check 后自动 confirm |
| 写入保护 | 拒写 active 槽 + 回读校验通过才 activate + -f 强制 | 防呆优先 |
| 坏块策略 | 槽窗口含 factory bad block → 拒绝更新 (-EIO) | U-Boot 线性读 FIT, 无法跳块; 运行期写坏 → 中止, 槽保持未激活 (安全) |
| misc 擦除粒度 | 只擦 misc 块 0, 回写页 0-1 | 保住 BCB (偏移 0) 与其他元数据 |

**已接受的残余风险** (你睡前确认过 brick 风险):
1. rcS 跑 bootcheck **之前** kernel panic → try 不递减 → 新槽无限重试
   (不会伤旧槽数据, 手动 `ota boot-a` 或重刷可救)。
2. `upgrade_tool uf` 后 misc 被清零 → U-Boot 重置默认元数据 → 启动 boot_a
   (boot_a 始终保持 confirm 过的镜像即可视为"出厂槽")。
3. U-Boot 的 `android_slotsufix=_x` bootargs 对 NuttX 无效 (仅 U-Boot 内部用)。

---

## 5. 上轮遗留验收提醒 (未变, 再贴一遍)

1. **CAN 引脚映射**: J9 CAN0/CAN1 ↔ RM_IO 的对应关系需你确认 (我按
   pinctrl dtsi + 原理图推的, 万用表量一下 TX 引脚有无波形即可)。
2. **AMP link-id 0x03**: M0 demo 的 MASTER_ID=0/REMOTE_ID=3; 内核 dtsi
   里 0x02 是 CPU2 场景, 不要混淆。`nsh> rpmsgtest` 期望
   `Rockchip rpmsg linux test!` 回包。
3. **M0 启动路径是本夜唯一未经硬件验证的 AMP 环节** (NuttX SMC
   0x82000028 + 内嵌固件; 预编译 U-Boot 无 ROCKCHIP_AMP, 嵌入 TEE 有
   SIP handler, 静态确认可走)。若 rpmsgtest FAIL: `ota` 无关, 先看
   `dmesg` 式日志里 "M0 booted" 是否出现。
4. **RAM 缩到 27.5MB** (CONFIG_RAM_SIZE=0x1B80000): `free` 显示 ~27.5MB
   是预期的 (顶部 2MB 让给 rpmsg 共享窗 0x03c00000)。
5. **无 RTC**: 时间来自 NTP (`ntpcstart` 已在 rcS); `date` 验证。
6. `wtdog` 看门狗 / `cansend can0` CAN 回环 / `pwm` / `adc` 命令不变。

---

## 6. 快速冒烟序列 (建议顺序)

```bash
nsh> free && ps && mount          # 基线
nsh> ota status                   # A/B 元数据 (本轮核心)
nsh> ota confirm                  # 固化
nsh> rpmsgtest                    # AMP 链路
nsh> candump can0 &  cansend can0 123#DEADBEEF   # CAN 回环
nsh> date                         # NTP 时间
# U 盘拷 boot.fit 后:
nsh> ota update /data/boot.fit && nsh> reboot
# 重启后:
nsh> ota status && nsh> ota confirm
```

---

## 7. v8 修复 (2026-08-30 白天追加): /data littlefs + U盘 FAT + rpmsgtest 超时

> 对应你晨间反馈的四个问题: ① rpmsgtest 卡死 ② DHCP 未起 ③ /data mount failed: 25
> ④ U 盘灯绿不会测。③ 已修, ① 已修, ②④ 是使用姿势 (见下), 另修了 U 盘配置缺口。

### 7.1 /data mount failed: 25 — 根因 + 修复 (你已拍板方案 B)

**根因 (两层叠加)**:
1. **架构层**: SmartFS 绑定块设备时必须拿到 SMART 层的 BIOC_GETFORMAT ioctl
   (`smartfs_utils.c:199`), 而这些 BIOC_* ioctl 只有 `drivers/mtd/smart.c`
   (SMART FTL) 实现。我们的栈是 raw SPI NAND MTD → mtd_partition → **dhara**
   → /dev/mtdblock0 (普通块设备), dhara 的 ioctl 全部透传给 NAND MTD
   (`dhara.c:465`), MTD 不认识 BIOC_* → **ENOTTY = 25**。即 "dhara+SmartFS"
   这个 v5 起的设计从未真正执行过, v7 是第一次暴露。
2. **rcS 参数错误**: `mksmartfs /dev/mtdblock0 2048` 的第二位置参数是
   nrootdirs (1-8, MULTI_ROOT_DIRS 开启时), 2048 直接报
   "Invalid number of root directories" 且**跳过格式化**。就算 ioctl 通了这行也是错的。

**修复 (方案 B, 你拍板)**: dhara 保留 (磨损均衡) + **littlefs** 挂 /data:
- R258 参考就是 NAND FTL + littlefs (r528 `nuttx_nand_init.c:2082`), 符合 3.4.5
- KVDB 只写 `/data/kvdb.db` 普通文件, 与文件系统类型无关 ✓
- littlefs 原生支持块设备 + `mount -o autoformat` (首启自动格式化, 之后直接挂载)
- rcS: `mount -t littlefs -o autoformat /dev/mtdblock0 /data`
- defconfig: +CONFIG_FS_LITTLEFS=y (原 smartfs 相关保留不动, mksmartfs 不再被 rcS 调用)

### 7.2 U 盘 "灯绿但不会测" — v7 缺 FAT 配置!

**v7 的 defconfig 没开 CONFIG_FS_FAT** — U 盘枚举出 /dev/sda 没问题 (LED 绿
说明供电/枚举大概率 OK), 但 `mount -t vfat` 会报错 (vfat 类型未注册)。
v8 已加 `CONFIG_FS_FAT=y` + `FAT_LCNAMES` + `FAT_LFN` (长文件名)。

**测试步骤 (v8 固件)**:
```bash
nsh> ls /dev                                  # 预期出现 sda
nsh> mount -t vfat /dev/sda /tmp/usb          # /tmp/usb rcS 已预建
nsh> ls /tmp/usb                              # 看到 U 盘文件即通过
```
- LED 绿 = U 盘拿到电, 不代表枚举完成; 以 `ls /dev` 出现 sda 为准。
- 若 sda 不出现: DEBUG_USB_ERROR 已开, 看串口 ERROR 日志 (枚举失败会打原因)。

**v8b 实测新情况 (2026-08-30)**: U 盘枚举成功（控制传输全过）但 MSC 的
bulk IN 挂死, /dev/sda 不出现; 拔线才报 `rk3506_chan_wait failed: -32`。

**v8c 根因修复（v8b INFO 跟踪直接定位, 三层）**:
1. **NAK 循环被误杀**: 设备上电后对首个请求长时间 NAK（JMicron 桥初始化
   慢）, 驱动 NAK 重试在 ISR 里静默跑（无日志）; v8b 的 5s 超时把"正在
   重试"误判为"停滞"强拆——每个控制 IN 首次尝试都白等 5s。
2. **XFRC 不置位**: NAK 重试序列后 DWC2 核心偶发不报 XFRC——数据已完整
   收到（rxflvl 已 pop, xfrd==buflen）但 HCINT 只剩未 mask 的 ACK 位,
   通道空转直到超时; 且 HCINTMSK 基线不含 CHH, 核心 self-halt 完全不可见
   （偏离 u-boot dwc2 恒使能 CHHLTD 的参考行为）。
3. **ep0 mask 方向残留**: 控制通道 IN/OUT 交替复用, mask 在 configure
   时按当时方向写死, 反方向传输用错（OUT NYET/IN BBERR 错位）。

**v8c 修复**: mask 每笔传输现算（恒含 CHH, ctrl/bulk IN 含 ACK）; ACK
判据——数据收满或收到短包即按 XFRC 完成传输; NYET→EAGAIN 重发;
chan_wait 改 500ms 切片轮询 progress——有中断进展就续期（3s 窗口/30s
硬上限）, 真停滞 3s 才转储+取消（转储含 GINTMSK/progress, 可分辨
"ISR 在跑但 halt 不生效" vs "中断未交付"）。

**复测 (v8c 固件)**: 插 U 盘 → 串口会先出现枚举跟踪（含若干 NAK 期间
的静默重试, 属正常）→ 预期 /dev/sda 出现 → `mount -t vfat /dev/sda
/tmp/usb` → `ls /tmp/usb`。若仍失败: 把完整日志发我, 重点看
"transfer stalled" 转储（现在含 GINTMSK/progress, 能定位到层）。

### 7.3 rpmsgtest 卡死 — 阻塞读 bug 已修 + v8b 根因修复 (缓存一致性)

**Bug 1 (v8 修)**: 主循环用阻塞 `read()`, 超时判定写在 read **之后** — M0 不回包时
read 永久阻塞。v8 改为 `poll()(剩余超时) + read`, FAIL 会退出而不是挂死。

**根因 (v8b 修): rpmsg 共享窗缓存一致性**。rk3506_start.c 的 MMU 身份映射把
128MiB DDR 全部映射为 **cached**, rpmsg 窗 0x03c00000-0x03e00000 (vring0/vring1
+ 缓冲池) 也在其中。但 **Cortex-M0 没有 cache 且与 A7 无硬件一致性**: A7 写的
vring 描述符 / avail-used idx 停在 A7 dcache 里, M0 从 DDR 读到的是陈旧数据;
两边各持一套失同步的 vring 视图 → 数据面完全断流 (mailbox kick 走寄存器
不受影响, 所以 M0 可能已完成 link-up 但收不到任何报文)。open-amp 用普通
store/load 访问 vring 元数据, 必须靠映射本身保证一致性 → 窗口改映射
**device/uncached** (`rk3506_start.c`: 60 段 cached + 2 段 device + 66 段 cached)。

**端点改名**: "rpmsg-mcu0-test" → "rpmsg-mcu0-echo" (避开 M0 NS announce
同名节点冲突; 路由靠 dst 0x4003)。

**v8b 板上实测 (探针数据)**: `vring probe A7->M0 (avail=1 used=0)
M0->A7 (avail=64 used=0)` —
- ping 已入 vring1 (avail=1), A7 的 64 缓冲种子在 DDR 可见 (avail=64)
  → **uncached 映射修复生效, A7 侧数据面正常**;
- used 全 0 → **M0 从未消费 ping、从未发送任何报文** = M0 未启动或
  卡在 link-up 之前 (kick 未到达 / rpmsg init 未跑)。

**v8c 重大发现 — 驱动日志宏全部静默 (根因级)**: NuttX debug.h 的
`ierr/iwarn/iinfo` 是 INPUT（输入子系统）模块宏（CONFIG_DEBUG_INPUT_*
门控, 本工程未开）, 不是"中断上下文日志"; 全部 12 个驱动文件的错误/
信息日志从第一天起编译为空——这就是 boot 日志里**从未出现 "M0
firmware booted"（rptun 启动成功标志）也从未出现任何 rptun 报错**的
原因。v8c 已全量改为 `_err/_warn/_info`（DEBUG_ERROR/WARN/INFO=y 已开）。

**复测 (v8c 固件)**: 看 boot 日志（这次 rptun 的日志第一次真正可见）:
- 出现 `M0 firmware booted at ...` → M0 启动成功, 若 rpmsgtest 仍
  FAIL 且探针 used=0 → 问题在 mailbox3 kick 未到达 M0 / M0 rpmsg init
  卡住, 带日志回来;
- 出现 `SIP_MCU_CFG failed` 或其他 SMC/时钟报错 → M0 根本没启动成,
  按报错修启动链;
- 什么都没有 → boot 路径没执行到（bringup 顺序问题）, 带日志回来。

**端点改名**: "rpmsg-mcu0-test" → "rpmsg-mcu0-echo" (避开 M0 NS announce
同名节点冲突; 路由靠 dst 0x4003)。

**A7/M0 协议链路静态核对结论** (rk3506_rptun.c 与 SDK rk3506-mcu test_demo
/ rpmsg-lite RK3506 platform): 地址一致 (link_id 0x03, ept 0x4003); A7
master init 尾部 set_status(DRIVER_OK)→notify→mailbox3 kick {CMD=0x03,
DATA=RMSG} 释放 M0 link_state; A7 收包 isr 以 RPTUN_NOTIFY_ALL 上报,
rproc_virtio_notified(RSC_NOTIFY_ID_ANY) 两 vring 全处理。

### 7.4 curl HTTPS "卡住" — 根因: mbedtls_close 阻塞读 (v8c 已修; v8e 已回退原始 curl)

> **v8e 注意**: 用户要求 curl 使用仓库原始版本, 外部 curl 源码仓已回退到
> `8c2a01f3e`, 本节的 mbedtls_close 修复已不在固件中, 仅作历史记录。

**现象 (v8b 板上实测)**: `curl https://stdl.b4qaq.cn/fwtb/info.json` —
响应体其实早已完整收到, 但 curl 不退出、无输出; Ctrl+C 终止后 JSON
一次性打印。

**根因**: `external/curl/curl/lib/vtls/mbedtls.c` 的 `mbedtls_close()`
在连接关闭时无条件 `mbedtls_ssl_read()` 一次（本意是消费已到达的
close_notify、避免 RST）。上游 socket 非阻塞, 该 read 立即返回
WANT_READ; NuttX socket 是阻塞的且 curl 未设 O_NONBLOCK——keep-alive
服务器在响应后保持 TLS 会话不发 close_notify → 该 read 永久阻塞:
easy_cleanup 卡死、stdout 缓冲不刷新, Ctrl+C 终止 curl 后退出 flush
才"显示结果"。v8c 修复: `Curl_socket_check(fd,..,0)` 零超时探测,
仅当 close_notify 已到达才读。OTA 的 `curl -o /data/boot.fit` 同样
受益（否则下载完成也会挂）。

**复测 (v8c 固件)**:
```bash
nsh> curl https://stdl.b4qaq.cn/fwtb/info.json    # 应立即返回提示符+JSON
nsh> curl -v --max-time 15 https://stdl.b4qaq.cn/fwtb/info.json
```
- 若仍有异常: 用第二条（`-v` 看停在响应哪一步, `--max-time 15` 保证
  自己退出）, 把完整 -v 输出发我。

**DHCP/网络排查（仍有效的通用步骤）**: `ifconfig eth0`（开机等 40s,
有 inet addr 才算 DHCP 成功; 日志应见 `netcfg: dhcp on eth0 ok`）→
`ping 192.168.10.1` → 再 curl。手动补跑 `ifconfig eth0 dhcp` 注意:
服务器对重复 DISCOVER 常不应答, 30s 后报错不算死锁。

### 7.5 slot_b 元数据数字异常 — 重刷后自然消除

v7 板上 `ota status` 显示 slot_b {priority 0, tries 14, successful 7}
(默认应为 {14,7,0})。v8 全量刷会清 misc, 元数据回到默认, 此疑点作废;
若刷完 v8 后再出现, 再查。

### 7.6 v8 刷机后 /data 快速验收

```bash
nsh> mount                              # 应有 /data type littlefs
nsh> echo hello > /data/t.txt && cat /data/t.txt && rm /data/t.txt
nsh> reboot                             # 重启
nsh> ls /data                           # t.txt 已删 (autoformat 没有重新格式化, 数据保持)
```

---

## 8. v8d 修复 (2026-08-30 追加): U 盘 bulk 零事件根因 + rpmsg kick 探针 + curl 定位探针

### 8.1 U 盘 bulk IN 零事件 — 真根因: DWC2 RX FIFO 太小 (v8d 已修)

**v8c 板上数据回顾**: 枚举（mps=8/64 的控制传输）全部通过, 但 CSW 的
bulk IN (mps=512) 零事件挂死: 通道 armed 正确（HCCHAR/HCTSIZ 无误）
但 HCINT=0、核心一次 token 都不发。

**根因**: DWC2 slave 模式下, 核心在发 IN token 前要求 RX FIFO 能同时
容纳"该通道一个 max packet + 状态字"。HS bulk mps=512B → 512/4=128
words + ~3 状态字 ≈ **131 words > 旧默认 GRXFSIZ=128 words** → 核心
静默拒绝发 token（CHENA=1、HCINT=0、零中断）。mps=8/64 的控制传输只需
~19 words, 所以枚举全过——完美解释"枚举过、bulk 死"。

**修复**: `vendor/rockchip/chips/rk3506/Kconfig` 三个 FIFO 默认值
128/96/96 → **512/256/224 words**, 与 SDK `hal_usb_core.c` 完全一致
(GRXFSIZ=0x200, NPTX FIFO 0x100 @0x200, PTX FIFO 0xE0 @0x300)。
v8c 的 ACK 判据/进展等待修复仍然有效（那是完成机制层, 这是能力层）。

**复测 (v8d 固件)**: 同 7.2——插 U 盘 → /dev/sda → `mount -t vfat
/dev/sda /tmp/usb` → `ls /tmp/usb`。这次 bulk IN 应该有事件了。

### 8.2 rpmsg M0: 已确认启动, kick 链源码级核对完成, 加探针定位 (v8d)

**v8c 板上数据**: boot 日志首次出现 `M0 firmware booted at 0xfff84000
(27120 bytes)` → **M0 已被释放启动**（SIP_MCU_CFG 返回 0）。但
rpmsgtest 仍 FAIL、探针仍 `avail=1/used=0 + avail=64/used=0` = M0
从未消费 ping、从未发过任何报文。

**本轮完成的源码级核对（结论: A7 侧 kick 协议完全正确）**:
- **rk3506 mailbox 是 V2 语义**: dts compatible
  `"rockchip,rk3506-mailbox", "rockchip,rk3576-mailbox"` → 内核
  rk3576_drv_data。V2: 每方向一组寄存器 (INTEN@0x00/STATUS@0x04/CMD@0x08/
  DATA@0x0c, B2A 在 +0x10), bit0=消息到达中断/STATUS, bit8=trigger
  mode, 写需 HIWORD 写使能 (bits31:16)。
- **SDK M0 HAL 的使能写法是对的**（此前怀疑"裸写无 WE 位"是我看漏）:
  `MBOX_ChanEnable` V2 分支写 `A2B_INTEN = (1<<16|1)` — 带 WE ✓。
  M0 在 rpmsg init 时会对 MBOX0/MBOX3 各写一次 A2B_INTEN=WE|1。
- **kick 消息格式一致**: A7 写 MBOX3 A2B_CMD=0x03(link_id)/
  A2B_DATA=0x524D5347(magic) == Linux 内核 rockchip_rpmsg_mbox 的
  {cmd=link_id, data=magic} == M0 rpmsg_remote_cb 的期望。
- **M0 回调链一致**: 第一次 kick → env_isr(6)=tvq → link_state=1 →
  wait_for_link_up 释放 → ns_bind/create_ept/ns_announce。M0 的反向
  kick: q0(vq6)→MBOX0 B2A, q1(vq7)→MBOX3 B2A, 我们 A7 两个 isr 都挂了。
- **A7 上层链一致**: master init 尾部 set_status(DRIVER_OK) →
  rproc_virtio_set_status 写资源表并 notify → 我们的 mbox3 kick。
  确认 kick 一定发出。

**剩余未知 = M0 是否活到 rpmsg init / kick 是否被 M0 中断消费**。
静态分析到此无法再收窄, v8d 在 rpmsgtest 超时探针里加了**邮箱寄存器
转储**（A7 侧直接读 MBOX0/MBOX3 寄存器, 一次刷机判四个问题）:

```
rpmsgtest: mbox probe MBOX3 A2B (inten=0x???????? status=0x????????)
           B2A (status=0x????????) MBOX0 A2B (inten=0x????????)
           B2A (status=0x????????)
```

判读表:
| 现象 | 结论 |
|------|------|
| MBOX3 A2B_INTEN bit0=1（或 MBOX0 A2B_INTEN bit0=1） | **M0 活着**且跑完了 rpmsg init（它使能了自己的收中断）|
| MBOX3 A2B_INTEN = 0x100（只有我们写的 bit8） | M0 **没活到** rpmsg init——卡在 rpmsg 之前（此时建议接 M0 串口）|
| MBOX3 A2B_INTEN = 0 | 连 A7 自己的写都没了——A7 mailbox pclk 被门控/地址错 |
| MBOX3 A2B_STATUS bit0=1 | 我们的 kick **还在路上没人收**——M0 的 INTMUX/NVIC 中断路径断 |
| MBOX3 A2B_STATUS bit0=0 | kick 已被 M0 ISR 消费——若 vring 仍 used=0, 问题在 M0 rpmsg 内部（DATA 校验/发送路径）|
| MBOX0/MBOX3 B2A_STATUS bit0=1 | **M0 反向 kick 过我们**而 A7 没服务——A7 收路径断（不太可能, 但探针能证）|

**复测 (v8d 固件)**: `rpmsgtest`, 把 vring probe + **mbox probe 两段
完整日志**发我。

### 8.3 curl: v8c 修复后仍挂 → 加 P1-P6 定位探针 (v8d; v8e 已回退原始 curl)

> **v8e 注意**: 用户要求 curl 使用仓库原始版本, P1-P6 探针与 mbedtls_close
> 修复一并回退到 `8c2a01f3e`, 本节仅作历史记录。

**v8c 板上数据**: `-v` 完整走完（握手、200、body、
"Connection #0 ... left intact"）后仍挂, Ctrl+C 才 flush。注意 v8c
分析里有个误判已纠正: teardown 的 `Closing connection` 本来就不会打
（close_all 用非 verbose 的 closure_handle）, 所以"没这行"不是证据。

**静态分析已到头**: cleanup 链 easy_cleanup → Curl_close → multi_cleanup
→ conncache_close_all → conn_shutdown → Curl_conn_close → ssl_cf_close
→ mbedtls_close(已有 0 超时防护) → cf_socket_close → sclose()。除
mbedtls_close 的 read（已防护）和最后的 close() 系统调用外无 I/O。
**剩余嫌疑只有两个**: ① 防护后仍有一条 read 路径阻塞（极小概率）;
② NuttX `close()` 本身阻塞。无法从源码进一步区分 → 上探针。

**v8d 探针**（全部 `fprintf(stderr)+fflush`, 无缓冲, 一定按顺序出现）:
- P1 `easy_perform returned` — 传输层正常返回
- P2 `Curl_close` — teardown 开始
- P3 `conn_shutdown` ×3 — 进入 / secondary 关完 / first 关完
- P4 `mbedtls_close sockfd=? readable=?` + `done` — TLS 层防护判定
- P5 `socket_close fd=?` + `done` — OS close() 前后
- P6 `easy_cleanup returned` — 全部完成

**复测 (v8d 固件)**: `curl https://stdl.b4qaq.cn/fwtb/info.json`,
若还挂, Ctrl+C 后把**从 P1 开始的所有 P 行**发我。最后一行停在哪,
根因就在哪个区间:
- 停在 P4 readable=1 → close_notify 读仍阻塞（改删 close_notify read）
- 停在 P5（无 done）→ **NuttX close() 阻塞**（查 net_close/tcp_close）
- P1 没出现 → 问题不在 cleanup, 在别处（再分析）

---

## 9. v8e 修复 (追加): U 盘 mount failed:20 根因 + rpmsg mbox 时钟门控根因

### 9.1 U 盘 `mount failed: 20` — 根因: 挂载点不能在挂载点之下 (v8e 已修)

**v8d 板上数据**: `mount -t vfat /dev/sda /tmp/usb` → `mount failed: 20`
(ENOTDIR), 但 sda 已出现（v8d FIFO 修复让 MSC bulk 通了）。

**根因 (NuttX fs_mount.c 源码级)**: `mount()` 先 `inode_find(target)`,
而 `_inode_search()` 对"路径下落到某个挂载点时"直接停在那个 MOUNTPT
inode 返回 OK（relpath=剩余段）。`/tmp` 已被 bringup 挂了 tmpfs, 所以
`inode_find("/tmp/usb")` 命中的是 **/tmp 挂载点 inode**; 而
`FSNODEFLAG_TYPE_MOUNTPT(3) != FSNODEFLAG_TYPE_PSEUDODIR(0)`,
fs_mount.c 的 `!INODE_IS_PSEUDODIR(mountpt_inode)` 于是返回 **-ENOTDIR**。
即: **NuttX mount() 不能在另一文件系统的挂载点之下再创建挂载点**。
rcS 里的 `mkdir /tmp/usb` 建的是 tmpfs 目录, mount 根本不看它。

**修复**: 挂载点改到伪根下 `/mnt/usb`（`mount -t vfat /dev/sda /mnt/usb`）。
mount 对不存在的目标会用 `inode_reserve_path()` 在伪根里自动创建, 无需
mkdir。注意 `/data/usb` 也不行（/data 是 littlefs 挂载点, 同样 ENOTDIR）。

**复测 (v8e 固件)**: 插 U 盘 → `ls /dev` 见 sda → `mount -t vfat
/dev/sda /mnt/usb` → `ls /mnt/usb` 看到文件即通过。

### 9.2 rpmsg 一直 FAIL — 真根因: A7 把 PCLK_MAILBOX 写门控了 (v8e 已修)

**v8d 板上数据**: mbox 探针全零（MBOX3/MBOX0 的 A2B INTEN/STATUS、
B2A STATUS 全 0）, 但 boot 日志有 "M0 firmware booted" = M0 已释放。
A7 侧源码级核对过 kick 协议、V2 语义、M0 HAL 使能写法、回调链, 全部
一致——就是找不到 M0 不响应的原因。

**根因 (v8e 定位)**: `rk3506_mbox_init()` 原来有一行"开 PCLK_MAILBOX":
```c
putreg32(RK3506_MBOX_WE(13) | (1u << 13), CRU + 0x800 + 6*4);
```
问题在 **Rockchip 门控是 SET_TO_DISABLE 语义**: 内核 `GFLAGS =
CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE`（写 1 = 关时钟, 写 0
带 WE = 开时钟）; 本仓 SPI1 驱动 `rk3506_spi1_clock_init()` 也是
`putreg32(gates << 16, reg)`（数据位=0 才开门）。所以上面这行写的是
**关时钟**的值——**v8b 起每次 boot A7 都亲手把 PCLK_MAILBOX 关死了**:
之后所有 mbox 寄存器读=0、写被丢, v8d 探针全零、kick 永不落地、M0
永远收不到; `mbox_send` 的忙等读 A2B_STATUS 恒 0 于是从不等待、静默
"成功", 假象完整。

**修复 (rk3506_rptun.c)**: 写 0 带 WE 开 PCLK_MAILBOX + PCLK_INTMUX
(CON6 bits13,14), mcu_boot 里补开 PCLK_UART4 + SCLK_UART4 (CON11
bits8,13) 给 M0 调试串口。

**探针 v2 (v8e)**: INTEN 可能只写不可读, 判读改以 **CRU 门控原始值 +
MBOX3 A2B_CMD/DATA 回读**为准:
```
rpmsgtest: mbox probe MBOX3 A2B (inten=.. status=.. cmd=0x? data=0x?)
           B2A (status=..)
rpmsgtest: mbox probe MBOX0 A2B (inten=..) B2A (status=..)
           CRU gates CON5=0x? CON6=0x? CON11=0x?
```
判读:
| 现象 | 结论 |
|------|------|
| CON6 bit13=1 | PCLK_MAILBOX 仍被门控（v8b-v8d 的 bug）|
| MBOX3 A2B_CMD=0x03 且 DATA=0x524d5347 | A7 kick 落地 |
| A2B_STATUS bit0=1（且 kick 落地）| M0 的 mailbox/INTMUX 中断路径断 |
| A2B_STATUS bit0=0（且 kick 落地）| M0 ISR 已消费, 但无 ns_announce → M0 卡 rpmsg 内部, 接 M0 串口 GPIO1_C2/C3 @1500000 |

**复测 (v8e 固件)**: `rpmsgtest`, 把 vring probe + mbox probe v2 两段
完整日志发我。若 CON6 bit13=1 说明还有别的代码在关它; 若 kick 落地
且被消费但仍 FAIL, 则需要接 M0 调试串口看 M0 侧日志。

## 10. v8f 修复: rpmsg kick 丢边沿竞态 (v8e 板上数据定位)

### 10.1 v8e 板上实测: 时钟修复生效, M0 活着, kick 落地, 但仍不通

```
CRU gates CON5=0x00000000 CON6=0x00000000 CON11=0x00000000
MBOX3 A2B (inten=0x00000101 status=0x00000001
           cmd=0x00000003 data=0x524d5347) B2A (status=0x00000000)
MBOX0 A2B (inten=0x00000101) B2A (status=0x00000000)
vring A7->M0 (avail=1 used=0)  M0->A7 (avail=64 used=0)
```
逐条判读:
| 观测 | 结论 |
|------|------|
| CON6 bit13/14 = 0 | **v8e 时钟门控修复生效**, mailbox/intmux pclk 已开 |
| **MBOX0 A2B_INTEN=0x101** | A7 侧代码对 MBOX0 只写 B2A_STATUS/B2A_INTEN, **从不写 A2B_INTEN** → 这个值全部是 M0 写的 ⇒ **M0 活着且执行到了 rpmsg/mailbox init** |
| MBOX3 A2B_CMD=0x03 DATA=0x524d5347 | **kick 物理落地**（v8d 时寄存器全 0） |
| MBOX3 A2B_STATUS bit0=1 | kick 永久 pending, **M0 的 ISR 从未运行** |
| B2A_STATUS 全 0 | M0 从未反向 kick |

### 10.2 根因: A7 kick 早于 M0 武装接收中断, 上升沿丢失

时序证据（boot 日志紧邻两行）:
```
rk3506_mcu_boot: M0 firmware booted at 0xfff84000 (27120 bytes)
rk3506_mbox_send: ERROR: mailbox3 A2B busy, kick dropped (cmd=0x03)
```
A7 在释放 M0 后**几微秒**就发出 DRIVER_OK kick, 而 M0 此刻还在跑
HAL_Init / UART / INTMUX / rpmsg init, `A2B_INTEN bit0` 仍是 0。
A2B_STATUS 的 0→1 边沿在**接收中断被屏蔽**时发生 → M0 收不到 IRQ →
不会 w1c 清状态 → 之后每一发 kick 都被 `A2B busy` 拒绝（那两条
"kick dropped" 是修复后的**正确行为**, 不是新 bug）→ rpmsg 永远
link 不上。

### 10.3 修复 (方案 A, 用户拍板): mcu_boot 就绪握手

`rk3506_mcu_boot()` 释放 M0 后轮询 `MBOX3.A2B_INTEN bit0`
（该位**只有 M0 会写**, A7 只写 bit8, 因此是天然的"远端接收已武装"
握手信号）, 最多 1 s / 每 1 ms 一轮; 就绪后 w1c 清一次 A2B_STATUS,
保证首发 kick 产生干净的 0→1 边沿。`mcu_boot` 由 rptun `start` 调用,
**早于框架首次 notify**, 所以覆盖所有 kick, 且不存在重复投递。
超时打 ERROR 后继续, 不影响其他功能。

### 10.4 复测 (v8f 固件)

正常应看到 boot 日志多出一行:
```
rk3506_mcu_boot: M0 mailbox rx armed after N ms
```
（N 通常个位数 ms）且**不再出现** "kick dropped"。然后 `rpmsgtest`
应当 PASS。若仍 FAIL, 看新的探针判读:
- `M0 rx IS armed ... yet the kick is STILL PENDING` → 握手成功但 M0
  的 **INTMUX/NVIC 投递通路**断, 其 ISR 不运行 → 必须接 M0 UART4
  (GPIO1_C2/C3 @1500000, 该串口时钟 v8e 已打开) 看 M0 侧日志
- `M0 never armed its rx (A2B_INTEN bit0=0)` + boot 里有 "rx not armed
  after 1000 ms" → M0 没起来或卡在 rpmsg init 之前

## 11. v8g 真根因: M0 从来没有时基, 卡死在 HAL_Init()

### 11.1 v8f 板上数据把问题逼到了死角

```
rk3506_mcu_boot: M0 mailbox rx armed after 0 ms      ← 不可能
rk3506_mbox_send: ERROR: mailbox3 A2B busy, kick dropped   x3
MBOX3 A2B (inten=0x101 status=0x1 cmd=0x03 data=0x524d5347)
```
- `armed after 0 ms`: M0 不可能在 0ms 内跑完 HAL/UART/INTMUX/rpmsg init
  → `A2B_INTEN bit0=1` 是**上次遗留的陈旧值**（mailbox 不随 A7 温复位而
  复位），v8f 的握手实际是空操作
- 3 条 dropped = kick #1 静默成功(mbox_send 只在失败时打日志) + #2#3#4
  被拒；清 A2B_STATUS 那步是有效的（内核 rockchip-mailbox.c:176 证明
  STATUS 是纯 w1c，不需要 HIWORD）
- ⇒ **我们其实从来没有任何证据证明 M0 执行过一条指令**

### 11.2 逐项源码核对（把所有"我们可能配错"的假设一一排除）

| 检查项 | 结论 |
|---|---|
| A7 启动序列 | 与 u-boot `fit_standalone_release()` **逐寄存器一致**：CRU_BASE 0xff9a0000 + GATE_CON5 0x814、0x0c000000、GRF 0xff288090=0x0bcd3d80、PMU 0xff90000c=0x00060004 ✓ |
| link-id / mailbox | `test_demo.c` MASTER_ID=0 / REMOTE_ID=3 → link 0x03 + mailbox3 ✓ 我们是对的 |
| 共享内存窗 | 工程链接脚本 `hal/project/rk3506-mcu/GCC/gcc_bus_m0.ld`: `LINUX_RPMSG ORIGIN=0x03c00000 LENGTH=0x200000` ✓ 与我们一致（CMSIS 模板里的 0xa0000000 被工程覆盖，虚警） |
| 加载地址 | `Image/amp.its` `load = <0xfff84000>` ✓ |
| 固件内容 | blob 里含 `rpmsg-mcu0-test` / `Rockchip rpmsg linux test!` → RPMSG_LINUX_TEST 确实编进去了 ✓ |
| M0 SRAM | dtsi `mcu_reserved 0xfff80000 + 0xc000`，镜像在 0xfff84000 → 可用 32KiB，与链接脚本 RAM=0x8000 / SP=0x7c00 吻合 ✓ |

### 11.3 真根因: dtsi 列的 6 个 AMP 时钟, 我们只开了 4 个

`rk3506-amp.dtsi` 的 `rockchip_amp` 节点**明确列出 A 侧必须替 AMP 核
使能的时钟**：
```
clocks = <&cru HCLK_M0>, <&cru STCLK_M0>,
         <&cru SCLK_UART4>, <&cru PCLK_UART4>,
         <&cru PCLK_TIMER>, <&cru CLK_TIMER0_CH5>;
```
Linux 由 `rockchip-amp` 驱动把这 6 个全部 `clk_prepare_enable`；
**u-boot 只开 HCLK_M0**（`CLKGATE_CON5=0x0c000000`，即 bit10；bit11 在
`clk-rk3506.c` 里查无此物）。我们照抄了 u-boot，于是漏了三个：

| 时钟 | 门控位 (clk-rk3506.c) | v8f 前 |
|---|---|---|
| `PCLK_TIMER` | `CLKGATE_CON(6)` bit 2 | ❌ 未开 |
| `CLK_TIMER0_CH5` | `CLKGATE_CON(6)` bit 8 | ❌ 未开 |
| `STCLK_M0` | `CLKGATE_CON(8)` bit 2 | ❌ 未开 |

而 M0 的 `hal_conf.h` 里 **`SYS_TIMER` 就是 `TIMER5`**（=
CLK_TIMER0_CH5）且 **`HAL_SYSTICK_MODULE_ENABLED`** 打开（SysTick 时钟
= STCLK_M0）；`main.c` 第一件事就是 `HAL_Init()`。**时基三个时钟全被
门控 → 计数器永不前进 → 任何 HAL 延时都是死循环 → M0 卡死在
HAL_Init()，压根到不了 rpmsg init。**

这一条同时解释 v8f 全部三个现象：INTEN bit0 是陈旧值(M0 从未写过)、
A2B_STATUS 永久 pending(ISR 从不运行)、vring 从不被消费。

### 11.4 v8g 改了什么

1. `mcu_boot` 步骤 1b：`CLKGATE_CON(6) <- WE(2)|WE(8)`、
   `CLKGATE_CON(8) <- WE(2)`（SET_TO_DISABLE，写 WE + 数据 0 = 开门），
   在拷贝固件和放行 M0**之前**完成
2. 握手去污染：放行 M0 前先 `WE(0)` 清掉 MBOX3/MBOX0 的 A2B_INTEN bit0，
   于是"armed after N ms"只可能是 M0 本次启动写的
3. **执行金丝雀**：M0 偏移 `0x7000..0x7c00`（3KiB，bss/heap + 下行栈，
   远高于 0x69f0 镜像末尾）填 `0xa5a5a5a5`，握手超时后回读
4. 探针 v3：加 `CON8`/`SEL23` 转储 + M0 时基三位判读

### 11.5 复测 (v8g, md5 3837fc10ce05c8a131e6dc3592ab29ae)

预期 boot 日志：
```
rk3506_mcu_boot: M0 firmware booted at 0xfff84000 (27120 bytes)
rk3506_mcu_boot: M0 mailbox rx armed after N ms (M0 is alive this boot)
```
N 应该是**真实的个位/十位 ms**（不再是 0），且**不再有 kick dropped**，
然后 `rpmsgtest` 应 PASS。

若仍失败，日志现在能直接二分，不需要接串口：
| 日志 | 含义 | 下一步 |
|---|---|---|
| `M0 never executed a single instruction: canary ... fully intact` | M0 一条指令都没跑 | 查 SMC code-start 重映射 / M0 核门控 |
| `M0 ran (canary dirtied N/768 words) but never armed its mailbox rx` | M0 跑过但死在 rpmsg init 之前 | 接 M0 UART4 GPIO1_C2/C3 @1500000 看它停在哪 |
| `armed after N ms` 但 rpmsgtest 仍 FAIL 且探针报 `M0 rx IS armed ... STILL PENDING` | M0 已武装但 INTMUX/NVIC 投递断 | 查 M0 侧 INTMUX/NVIC |
| 探针报 `the M0 has NO TIMEBASE` | 时基门控没写进去 | 查 CON6/CON8 写入是否被覆盖 |

---

## 12. v8h 复测（rpmsg 决定性取数）

**镜像**：`update.img` md5 `d4c903a465753dcb4a03c7970a5aa5b0`（9,785,898 B），分区布局未变但**建议仍全量刷**。

### 12.1 先纠正一条我自己的错误结论

v8g 我把"缺 M0 时基时钟"写成了**真根因**，这是过头了。逐行走 M0 初始化路径后：

| 我当时的假设 | 源码事实 |
|---|---|
| `HAL_Init()` 会因为没有时基而死循环 | `hal_base.c:162-183` 四步全是纯写寄存器，**无任何轮询** |
| `HAL_TIMER_SysTimerInit()` 会等计数器 | `hal_timer.c:87-100` 读一次 CONTROLREG、写 LOAD_COUNT、置 ENABLE，不等 |
| `HAL_UART_Init()`/打印会用 HAL 延时 | `hal_uart.c:331-367` 无延时；`SerialOutChar` 轮询的是 UART 自己的 `USR` |
| SysTick 开着所以要 `STCLK_M0` | SysTick 只由 `HAL_SystemCoreClockUpdate()` 初始化（`hal_base.c:192-200`），`main.c` 从不调用 |
| assert 可能死循环 | `HAL_ASSERT` 在本工程是**空宏**（无 `HAL_ASSERT_ON`） |
| — | demo 里**第一个 `HAL_DelayMs()` 在 link-up 之后**（`test_demo.c:278-280`） |

⇒ **v8g 该修（否则 link-up 后第一次 `HAL_DelayMs(1)` 必死循环），但它解释不了"M0 一条指令没执行"。根因仍未定。**

### 12.2 v8h 的取数逻辑：三分判读，不需要接串口

mailbox 中断是**电平敏感**的（`rk3502.dtsi:803` `IRQ_TYPE_LEVEL_HIGH`；`rockchip-mailbox.c:255-291` 是典型"共享电平线"写法：STATUS 为 0 就 `IRQ_NONE`，处理完末尾 w1c 撤电平）。因此 `A2B_STATUS b0=1` 与 `A2B_INTEN b0=1` **长期共存在电平语义下不可能是"丢边沿"**，只剩三种可能，v8h 用两个新寄存器把它们分开：

| `0xfff840bc`（IRQ31 向量槽） | `0xff2a000c` bit21（INTMUX EN） | 结论与下一步 |
|---|---|---|
| `0x00000000` | 任意 | **M0 零执行，或地址 0 对它不可写**。后者更阴险：Cortex-M0 无 VTOR，向量表固定在 0，镜像里 64 个 IRQ 向量是 `.space (64*4)` 全零（`start_rk3506_mcu.S:41`），靠 `hal_nvic.c:55` 运行时写 `(uint32_t*)0x0U` 填。写不进 ⇒ 首个 IRQ 跳 0（thumb 位=0）⇒ HardFault ⇒ 现象与"ISR 从不运行"一模一样。查 SMC `MCU_CODE_START_ADDR` 重映射与 `HRESETN_M0`(CON05 b10) |
| 非 0（应为奇数=thumb） | `0` | M0 跑起来了，但**死在 `HAL_MBOX_RegisterClient()` 与 `HAL_INTMUX_EnableIRQ()` 之间**（`rpmsg_platform.c:178..295`）。这时才值得接 M0 UART4 GPIO1_C2/C3 @1500000 |
| 非 0 | `1` | 整条 mailbox→INTMUX→NVIC 已武装。再看 `0xff2a008c` bit21：为 0 ⇒ 电平根本没进 INTMUX（查 `PCLK_INTMUX` CON6 b14、`PRESETN_INTMUX`/`PRESETN_MAILBOX` CON6 b14/13、`MCU_ISO_CON1/3`）；为 1 而仍不进 ISR ⇒ 回到向量表/NVIC |

### 12.3 握手信号升级（为什么 INTMUX 位严格优于 `A2B_INTEN bit0`）

`MAILBOX_BB_3_IRQn` = 117 + `NUM_INTERRUPTS`(32) = 149（`soc.h:271`），经 `hal_intmux.c:323-328` 换算＝INTMUX 输入 117 ＝ group 3 / bit 21 → `INTMUX_OUT3` → M0 NVIC IRQ 31。

- **唯一写者**是 M0 自己的 `rpmsg_platform.c:295`，而那是 `rpmsg_lite_remote_init()` 的**最后一步**
- A7 / u-boot / OP-TEE / Linux **全 SDK 无一处访问 INTMUX**（只有 `clk-rk3506.c:359` 的 `PCLK_INTMUX` 门控）
- **POR 值为 0** ⇒ 不可能像 `A2B_INTEN bit0` 那样是上次 boot 的陈旧值
- 位置在 `HAL_MBOX_RegisterClient()` 之后，而后者的 `MBOX_ChanEnable()`（`hal_mbox.c:74-84`）**先 w1c 清 `A2B_STATUS` 再开 INTEN** —— 这才是真正吃掉早到 kick 的地方，也证明 v8f 的握手方向没错

实现为两阶段且**不可能回退到 v8f 之下**：先等 `A2B_INTEN bit0`（≤1s），再等 INTMUX bit21（+100ms）；后者超时只 `_warn` 并继续，以防 INTMUX 万一在 A7 侧读不到反而变差。

### 12.4 复测步骤

```
# 1) 全量刷 v8h 后，看 bringup 里 mcu_boot 的这一行（三种之一）
#    成功：  M0 ready after N ms: mailbox rx armed and INTMUX group3 bit21 set M ms later
#    警告：  WARNING: M0 armed its mailbox rx after N ms but INTMUX group3 bit21 stayed clear ...
#    失败：  ERROR: M0 never executed a single instruction: canary ... AND IRQ31 vector slot still 0
#            ERROR: M0 dirtied N/768 canary words but its IRQ31 vector slot is STILL 0 ...
#            ERROR: M0 is alive (canary N/768 dirty, IRQ31 vector 0x...) but never armed ...
nsh> rpmsgtest
# 2) 重点抄这一行的全部字段
#    rpmsgtest: M0 delivery path INTMUX EN=0x... FLAG=0x... bit21 en=? pending=? |
#               IRQ31 vector=0x... | resets CON0=.. CON5=.. CON6=.. | iso CON1=.. CON3=..
```

`iso CON1/CON3` 全 SDK 无写者、复位值未知，这次只是**采数**，不做判断。

### 12.5 顺带记录的 SDK 事实（省下将来重查）

- **不存在 mailbox 中断路由寄存器**：GRF `SOC_CON13`（`0xff288034`）的 `*_INTR_MCU_SEL` / `*_INTR_INTMUX_SEL` 只覆盖 GPIO1_1/2/3 与 TIMER1CH5/DSMC；8 根 mailbox 线（AP_0..3→GIC SPI 138+x，BB_0..3→INTMUX 输入 114+x）是**硬连线**
- `MAILBOX_8MUX1_IRQn`（M0 NVIC #22）全 SDK 无任何使用，也找不到选择它的寄存器，**不要当备用通路**
- A 核唯一"不配 M0 就永远收不到中断"的寄存器是 `PMU_INT_MASK_CON` `0xff90000c` = `0x00060004`（`glb_int_mask_mcu=0`），我们已照抄；`rk3506.h` 里没有 PMU 这一块的位定义，位序无法交叉验证
- CON5 bit11 = `SWCLKTCK_M0_EN`（`rk3506.h:18671`），u-boot 开它有依据（v8g 注释里"查无此物"的说法已改）
- SDK 出厂 `main.c:10` 的 `TEST_DEMO` 和 `test_demo.c:13` 的 `RPMSG_LINUX_TEST` **都是注释掉的**；我们的 blob 已手工打开（`strings` 可见 `rpmsg-mcu0-test`）
- `amp.its:20` 的 `udelay = <1000000>` ⇒ 厂商在放行 M0 后等整整 1 秒，与我们 1s 轮询上限同量级

---

## 13. v8i 日志清理（rpmsg 已验收后收尾）

**镜像**：`update.img` md5 `601f6b7f2a6413474415a9b247bf61f1`（9,785,898 B）；`vela.bin` 2,889,724 → **2,877,436 B**（-12,288）。

### 13.1 rpmsg 结案

```
nsh> rpmsgtest
rpmsgtest: endpoint /dev/rpmsg-rpmsg-mcu0-echo (src 0x30 -> dst 0x4003) created, sending payload...
rpmsgtest: sent "ping from vela" (15 bytes), waiting for reply (10s timeout)...
rpmsgtest: reply (26 bytes): "Rockchip rpmsg linux test!"
rpmsgtest: PASS
```

**v8g 与 v8h 缺一不可**：v8h 的 INTMUX 握手保证首个 kick 落在 M0 武装之后；v8g 补的时基时钟让 M0 在 link-up 之后 `test_demo.c:278-280` 的 `while (cb_sta != 1) HAL_DelayMs(1)` 不再死循环——这正是 §12.1 自我更正时指出的、时基唯一真正致命的位置。

### 13.2 按用户拍板删除的日志

| 范围 | 删了什么 | 保留什么 |
|---|---|---|
| **USB** | defconfig 去掉 `DEBUG_USB_WARN`/`DEBUG_USB_INFO`（v8b 为查 MSC bulk 挂死临时开的，也是 `ls /mnt/usb` 乱码的原因） | `CONFIG_DEBUG_USB=y` + `DEBUG_USB_ERROR=y` |
| **网络** | GMAC0 **完全静默**：8 处 `nerr`（TX 超时/DMA 错误/Init/Start/irq_attach/PHYInit/PHYStartup 失败/no link after Nms）+ 2 处 `syslog(LOG_INFO)` link up/down；连带移除删空的 if/else、改 `ret = HAL_GMAC_PHYStartup()` 为裸调用、移除 `#include <syslog.h>` | 错误处理路径本身：返回值、`NETDEV_TXERRORS`、`up_disable_irq`、`netdev_carrier_on/off` 一个没动 |
| **rpmsg** | `hd_rk3506_rpmsgtest.c` **603→272 行**：`rpmsgtest_mbox_probe()`/`rpmsgtest_vring_probe()` 两个函数（约 18 条 printf）、两个 helper、30 多个寄存器宏及其注释、超时分支的两个调用、文件头 6 行描述；`rk3506_rptun.c` 删 3 条 `_info` + 2 条 `_warn` | rpmsgtest 的结果输出（endpoint/sent/reply/PASS）与全部 `fprintf(stderr)` 真实失败原因；rptun 的 **8 条 `_err`**；v8h 的 INTMUX 等待循环本身（那是握手逻辑不是日志） |

`mbox_isr` 删掉 warn 后 `cmd`/`data` 变成只写不读，但这两个寄存器读取与内核 `rockchip-mailbox.c` 的 ISR 一致、且 kick 只当门铃用不解析载荷，故**保留读取并加 `UNUSED()`**——零行为变化。

### 13.3 验证

- 干净构建 `exit 0`，**我改的三个文件零告警**
- nxstyle：rptun 16→**15**、gmac0 63→**47**、rpmsgtest 4→**4**（全部不劣于基线）
- 固件字符串核对：11 条被删日志**全部消失**，4 条应保留的**仍在**
- `cmake_out/.config` 确认 `# CONFIG_DEBUG_USB_WARN is not set` / `# CONFIG_DEBUG_USB_INFO is not set`
- `pack exit 0`

### 13.4 ⚠️ 一个我引入的副作用，需要你拍板

关掉 `DEBUG_USB_INFO` 后，全局 warning 从 45 → **51**，新增的 6 条全部来自**上游文件** `nuttx/drivers/usbhost/usbhost_storage.c` 的 `usbhost_dumpcbw()`/`usbhost_dumpcsw()`：

```
usbhost_storage.c:482:9: warning: format '%x' expects argument of type 'unsigned int',
                         but argument 3 has type 'uint32_t' {aka 'long unsigned int'}
```

**机理**：这两个函数的守卫是 `CONFIG_DEBUG_USB && CONFIG_DEBUG_INFO`（全局 INFO，不是 USB_INFO），所以函数体一直在编译。`uinfo` 在 USB_INFO=y 时展开成 `_info`，关掉后展开成 `debug.h:108` 的 `_none`：

```c
#  define _none(format, ...)     do { if (0) syslog(LOG_ERR, format, ##__VA_ARGS__); } while (0)
```

注释写着 "don't call syslog while performing the compiler's format check" —— 它**故意**保留格式检查，于是把上游一直存在的 `%08x` vs `uint32_t`（本 ABI 上是 `unsigned long`）不匹配暴露出来了。

**性质**：`if (0)` 死代码，零运行时影响；是上游的既有缺陷，不是我们的新 bug。

**要不要修**：修法就是把那 4 处 `%08x` 改成 `%08" PRIx32 "`（外加 `#include <inttypes.h>`）。但这要动 `nuttx/drivers/` 上游通用代码，**AGENTS 3.4.1 明确要求"不要碰"**，所以我没擅自改。你说改我就改，一个小提交；你说不改就挂在这儿记录着。

---

## 14. v8j —— M0 停核 + NSH 长度 + curl 记录纠正

**镜像**：`update.img` md5 `459824931f434d63f619e32cc8524be5`；`vela.bin` 2,877,436 B。

### 14.1 概率性 Data abort：根因是我的 bug

板上 v8h 概率复现。用 git 里的 v8h 源码重建带符号镜像解出调用栈：

```
rpmsg_virtio_start_worker → rpmsg_init_vdev_with_config+0x412
  → mm_memalign+0x3a → mm_malloc+0x17c        Data abort DFAR=0x3d
```

崩溃指令 `ldr r3,[r1,#8]`（`r1 = node->blink = 0x35`，`0x35+8 = 0x3d` ✅），是 `mm_malloc` 的自由链表完整性校验。分配请求本身正常（对齐是常量 `#8`）⇒ **堆在这次分配之前就已被写坏**，rpmsg 的 malloc 只是第一个踩到污染节点的**探测者**，不是元凶。

**根因**：`rk3506_mcu_boot()` **从来没让 M0 进过复位**。A7 热重启（含崩溃时 `Reset board on recursive assert` 自触发的那次）不复位 M0 ⇒ 上一代固件仍在 `0xfff84000` 执行，而我们直接 `memcpy` 覆盖它正在跑的代码/向量表/.data/.bss。跑飞的 Cortex-M0 拥有完整 4 GiB 地址空间，可以写进 A7 DRAM。这是唯一能同时解释「堆里出现 0x35」和「概率出现」的机制，也解释了崩溃→重启→M0 还活着→再崩的自我延续。

### 14.2 同时更正 v8h 的一个错误断言

那行日志本身自相矛盾：

```
M0 ready after 0 ms: ... INTMUX group3 bit21 set 0 ms later
(M0 finished rpmsg init this boot, IRQ31 vector 0x00000000)
```

按 §12 v8h 自己的三分判读表，**向量槽 = 0 ⇒ M0 零执行**。我当时断言"INTMUX bit21 POR=0 所以不可能是陈旧值，比 A2B_INTEN bit0 严格更好"——**错了**。POR=0 只对冷上电成立。真相不是"寄存器残留"，而是**上一代 M0 还活着在驱动这两个信号**。`A2B_INTEN bit0`（v8f）同理。我把 v8f 的错误换了个寄存器又犯了一遍。

### 14.3 修复（用户按 3.4.6 拍板）

新增 `rk3506_mcu_hold()`，两级停核、弱假设在先：

| 步 | 动作 | 依据 |
|---|---|---|
| 1 | `0xff90000c <- 0x00060000`（`mcu_rst_dis_cfg=0`） | u-boot 把这位置 1 作为释放的**最后**一步，清它即原厂序列倒着走。必须排第一：它读作 1 时 CRU 复位请求可能整个被屏蔽 |
| 2 | `0xff9a0a14 <- 0x0c000c00`（`HRESETN_M0`+`RESETN_M0_JTAG`） | rk3506.h:19111/19113；外推 |
| 3 | **金丝雀实测自证**：填充 → 等 2ms → 回读 | 停住了不可能被碰，没停住几乎必被脏化（M0 的 bss/heap 就在那）。脏了报 `_err` |
| 4 | `0xff9a0a18` 脉冲 `PRESETN_MAILBOX`+`PRESETN_INTMUX` | 让两个握手信号不可能来自上一次会话 |
| 5→ | memcpy → SMC → 解除 CRU 复位 → 写 PMU_INT_MASK_VAL | 收尾寄存器状态与 u-boot `fit_standalone_release()` 完全一致 |

**极性陷阱**（已写进宏注释）：SOFTRST 数据位 1 = **断言复位**；CLKGATE（SET_TO_DISABLE）数据位 1 = **关时钟**。两套寄存器极性相反，跨族抄错就是灾难。子代理报告里给的 `writel(0x01040104, 0xff9a0818)` 就是这么错的（正确是只写 WE 的 `0x01040000`，我现有代码本来就对）。

**安全红线**：不碰 `CON05` b6/b7（`ARESETN_SYSRAM`/`HRESETN_SYSRAM`，正是要加载的那块 SRAM），不碰 `CON00`（A7 自己的核复位组）。

**未获 SDK 背书**（已在代码注释标注）：全 SDK **无一处**断言过 M0 复位；rk3506 更是这一族里唯一连释放都不走 CRU 的（rk3562/rk3576/rk3528 都写 SOFTRST 解除，rk3506 的 `CRU_SOFTRST_CON5` 宏是死代码）；释放归 TEE 还是归 `mcu_rst_dis_cfg` 无法判定（只有预编译 `rk3506_tee_v2.10.bin`）；SDK 未规定任何 softreset 脉宽（取 20us）。**故第 2 级停核是外推，由金丝雀在板上自证**——这正是保留金丝雀（本来准备删）的意义。

**首刷要看**：如果出现 `ERROR: M0 still running with HRESETN_M0 asserted`，说明 CRU 停核在 rk3506 上无效，需要改走别的路子；没有这条错误就说明停住了，外推被证实。

### 14.4 NSH 命令行长度

根因是配置不是缺陷：defconfig 从未写过这两项，一直吃默认 `LINELEN=80` / `MAXARGUMENTS=7`。已改 **256 / 16**。

### 14.5 curl：我的 v8c 记录作废

原记录的机理**站不住**，逐条查证：

| 证据 | 位置 |
|---|---|
| curl 无条件把 socket 设非阻塞 | `cf-socket.c:1079` `curlx_nonblock(ctx->sock, TRUE)` |
| 走 fcntl 路径且非 lwip | `curl_config.h:194 HAVE_FCNTL_O_NONBLOCK`；`curl_setup_once.h:220 sfcntl=fcntl` |
| NuttX 认 O_NONBLOCK | `fs_fcntl.c:99-113` → `file_ioctl(FIONBIO)` |
| 传到 TCP 连接标志 | `netdev_ioctl.c:1756` 置 `_SF_NONBLOCK`；`tcp_recvfrom.c:721` 据此判断 |

socket 本来就是非阻塞的，`mbedtls_close()` 那个读应立即返回 WANT_READ，**不该挂**。而且补丁**已不存在**（curl 回原始 `8c2a01f3e`，`mbedtls.c:1050` 原样还在）。另排除：本仓 curl 8.4.0-DEV，`Curl_conn_shutdown` 优雅关闭超时是 8.8+ 才有的。

**下一步先取证，不改码**（用户已同意），三个板上实验不用重烧：

1. `curl -o /tmp/o https://... ; echo DONE=$?`
   - 文件完整但 `DONE` 迟迟不出 ⇒ 卡在退出/清理
   - `DONE` 立刻出 ⇒ **根本没卡死**，是 stdout 全缓冲未刷
2. `curl -v ...` —— 停住前的最后一行属于哪个阶段
3. `curl --max-time 20` —— 能被打断=卡在传输状态机，打不断=卡在系统调用

**否决记录**（另一 AI 的建议）：
- `CURLOPT_FORBID_REUSE`+`HTTP/1.0`：机制上**能绕开**（服务器主动关 → close_notify 或 FIN 都让读返回），但不治本、全局牺牲 keep-alive/HTTP1.1，且 NSH 的 curl 自建 easy handle，"在初始化代码里设置"等于改 curl 源码，与"用仓库原始版本"冲突。
- 「改 `nuttx/drivers/net/tcp_*.c` 让 FIN 映射 POLLHUP」：**三重错误**——该路径不存在（TCP socket poll 在 `nuttx/net/tcp/tcp_netpoll.c`）；NuttX 早已在 4 处设 `POLLERR|POLLHUP`；卡点根本没走 `poll()`，而我们的场景服务器压根**没发 FIN**。

### 14.6 验证

- 干净构建 `exit 0`，`rk3506_rptun.c` **零告警**；全局 51 = v8i 基线，无新增
- nxstyle `rk3506_rptun.c` **15 = 基线**
- `.config` 确认 `CONFIG_NSH_LINELEN=256` / `CONFIG_NSH_MAXARGUMENTS=16`
- 固件字符串 + 反汇编确认 `0x0c000c00` / `0x60006000` / `0x60000000` 均已落地
- `pack exit 0`

---

## 15. v8k —— iomux 日志删除 + curl 排查中段

**镜像**：`update.img` md5 `699c8695efbc5334991fa49ce46a2ab5`。

### 15.1 iomux 日志

用户点名 `rk3506_ioc_mux_set: IOC mux: ...` 让去掉。`rk3506_iomux.c` 共 4 条 `_info()`、无错误路径日志可保留，**全删**；连带删掉只被日志用到的 `inttypes.h`/`debug.h`/`syslog.h`。顺带修掉既有 `RK3506_GRF_PMU_ADDR` 重复定义告警（本地与 `hardware/rk3506_memorymap.h:99` 同值重复，删本地版）——全局告警 51→50。nxstyle 28→24。固件中 `IOC mux`/`IOMUX` 字符串计数 0。

### 15.2 curl 实验 1 的解读（等实验 2/3）

用户跑 `curl -H "Connection: close" -o /tmp/info.json ... ; echo DONE=$?`，按 Ctrl+C 后才打印 meter + `DONE=0`。三个硬事实：

- `TimeSpent = --:--:--` ⇒ 传输 0.3s 就完成了
- meter 行 Ctrl+C 才出现 ⇒ 挂点在 `progress_finalize` 之前 = 还在 multi loop 里
- `DONE=0` ⇒ SIGINT 唤醒后**成功**退出，排除失败/中止路径

源码侧已逐行排除整条清理链的阻塞点（multi_done → Curl_disconnect → conn_shutdown → Curl_conn_close → `do_close`：`mbedtls_close` 的无条件读在非阻塞 socket 立即返回 WANT_READ；`sclose`=`close()`；NuttX `tcp_close`/`tcp_shutdown`/`inet_close` 均非阻塞）。剩两种可能：

- (a) **真阻塞**：卡在 poll 信号量（ps 显示 `Waiting Semaphore`）
- (b) **假阻塞真自旋**：循环空转（ps 显示 `Running`/`Ready` 且占 CPU）

待用户跑实验 2（`curl -v ...` 看挂在哪个阶段）和实验 3（`curl ... &` + `sleep 3` + `ps` 看 STATE/EVENT）二分。

**源码排查中顺带确认的既有正确行为**（避免再被误导）：NuttX `tcp_netpoll.c:86-88` 把 `TCP_RXCLOSE` 映射为 `POLLIN`（peer 干净 FIN 会唤醒 poll）；`tcp_pollsetup` 在 `conn->readahead != NULL || backlog || (shutdown & SHUT_RD)` 时同步报 `POLLRDNORM`。**注意对方 AI 说的"NuttX poll 不处理 FIN"是错的**——RXCLOSE→POLLIN 一直存在。

---

## 16. v8l —— FSPI 日志删除 + curl 排查：锁定两个候选

**镜像**：`update.img` md5 `f915312cb69a9717be9ae2076184bc1e`。

### 16.1 FSPI 日志

8 条 `_info` 全删（无 `_err`/`_warn` 可保留），连带 3 个 include；DLL 调谐控制流未动。注意 `rk3506_spinand_fspi.c:894` 的 `syslog(LOG_INFO, "FSPI: SPI NAND READ_ID ...")` 是 MTD 层同名打印，**本次未动**。

### 16.2 curl 实验 2（-v）的新证据与两个候选

用户跑 `curl -v -o /tmp/info.json ...`，全部输出（含 stderr 的 verbose 行）都在 Ctrl+C 后才出现，最后一行是 `* Connection #0 to host stdl.b4qaq.cn left intact`。

源码侧新确认：

- **"left intact" 是普通 `infof`**（`multi.c:803`，非 DEBUGF）——在 `multi_done()` 收尾、连接**还回连接缓存**时打印。它后面紧跟 `Curl_safefree(data->state.buffer)` 就 return。所以**挂点在 multi_done 结束之后**。
- **NuttX 的 stderr 是行缓冲**（`task_initinfo.c:64-86`：stdin/stdout/stderr 三个流统一给 64B 缓冲 + `__FS_FLAG_LBF`，因 `CONFIG_STDIO_LINEBUFFER=y`）。按常理 `-v` 行带 `
` 应该实时刷出来——但用户两次实验都是 Ctrl+C 前**零输出**，这一点尚未解释，是待用户确认的关键事实问题。
- **`Curl_poll` 把 EINTR 吞掉返回 0**（`select.c`："make EINTR from select or poll not a lethal error"）——所以一次 Ctrl+C 不是"杀死"curl，而是给 poll 喂了一次超时式唤醒；若此后的同步状态检查能看到成功（连接其实已在协议栈里建好），curl 就顺势走完，exit 0。这解释了 DONE=0。

两个候选（病根不同，必须靠实验二分）：

- **(T1) 挂在 connect-wait 的 poll**：TCP 三次握手在协议栈里已完成但唤醒丢失，curl 干等；一次 SIGINT → poll 返回 0 → `getsockopt(SO_ERROR)=0` 已连接 → 顺势跑完 0.3s 传输 → exit 0。疑点：`Curl_pgrsStartNow` 在 `Curl_pretransfer`（`transfer.c:1415`）就启动计时，若挂在 connect，`TimeSpent` 应该含挂起时长，但 meter 显示 `<1s`。
- **(T2) 挂在传输完成之后**（left intact 之后）的某个单次阻塞调用——cache 归还 / msg 处理 / `curl_multi_cleanup` 链路；SIGINT → EINTR → 该调用报错返回 → 继续清理 → exit 0。与 meter `<1s` 吻合（传输 0.3s 就完了，挂在其后）。

**决定性实验（一个串口就够，用户之前以为不行）**：

```
curl -v -o /tmp/info.json https://stdl.b4qaq.cn/fwtb/info.json 2>/tmp/v.log &
sleep 5
cat /tmp/v.log
ps
```

- `&` 把 curl 放后台，shell 立即可用（一个串口完全够，不需要第二个）。
- stderr 重定向到文件后，按行缓冲规则 `-v` 轨迹**实时**写进 `/tmp/v.log`；`cat` 看到的最后一行 = 挂死的精确位置。
- `ps` 看 curl 的 STATE/EVENT：`Waiting Semaphore` = 真阻塞（T1/T2 的 poll 或单次调用）；`Running/Ready` = 自旋。

辅助实验：`curl --max-time 20 ...` —— 若 ~20s 报 `Operation timed out ... with 115 out of 115 bytes`（DONE=28）⇒ 挂在 multi loop 内（T1 类）；若 20s 到了毫无反应 ⇒ 挂在 loop 外的阻塞调用（T2 类）。

### 16.3 curl 实验批 2（2026-09-12）—— 全部传输都能完成，挂点在传输后

板上四连测：`curl --version` 秒退；`file://`(177KB)、`http://192.168.10.1/`(无 DNS 无 TLS)、`https://... --max-time 20` 三个传输**全部完整完成**（收到全部字节、打印 `left intact`/`Closing connection`），但**除 --version 外全部要 Ctrl+C 才出输出**。--max-time 20 未触发（传输 0.3s 就完成，挂在其后）。

已用源码钉死的事实：

- `tool_operate.c:649` 的 `fclose(outs->stream)` **检查了返回值**，失败会报 `CURLE_WRITE_ERROR(23)` 并打印 `Failed writing body`——DONE=0 证明 **fclose 成功**，tmpfs-close 挂死理论排除
- `Curl_infof`（curl_trc.c:118-133）每行**显式补 `
`** 后经 `Curl_debug` 写 stderr；NuttX stderr 行缓冲（task_initinfo.c）→ 每行本应实时上屏——但实际全部憋到 Ctrl+C，**输出去向是独立疑点**
- NSH curl 是**原版 tool**（tool_main.c/tool_operate.c，Makefile MAINSRC 证实），无 fmultidone，无 setvbuf
- 之前"后台实验挂在很早期"的判读**可能错了**：sleep 5 < DNS 重试预算（5s×RETRIES），那次也许只是 DNS 慢，不是挂——"挂点漂移"可能不存在，真实现象只有一个：**传输完成后进程不退出**
- `--max-time` 不触发 + ps 显示 `Waiting Semaphore` ⇒ 挂在 multi loop 之后的尾部（cleanup/exit 段）的某个信号量等待，SIGINT→EINTR→继续→exit 0

待跑判读实验：
```
curl -v -o /tmp/info3.json https://stdl.b4qaq.cn/fwtb/info.json &
sleep 10; ls -l /tmp/info3.json; ps        # 文件尺寸+状态定位挂点(115=传完/64=卡在fclose/无=更早)
curl -s -o /tmp/info4.json https://... ; echo DONE=$?      # 静默:若正常退出⇒输出行参与
curl -o /tmp/info5.json https://... 2>/dev/null ; echo DONE=$?  # stderr 去 null
```

