# 隔夜工作验收清单 (2026-02-28 夜间 → 早晨)

> 本轮目标: "将所有 Vela 支持的能力都进行适配" 收尾 — **A/B 双分区 OTA** 全套实现。
> 上一轮已交付: CAN / WDT / SARADC / PWM / AMP rpmsg (A7 侧 rptun + M0 启动 + rpmsgtest)。
> 本轮新增: OTA A/B 双槽 + ota NSH 命令 + FSPI 并发锁 + /data 分区位置修复。
> 音频 / RTC / WiFi-BT 按你的决定跳过。

---

## 0. 产物

| 文件 | 大小 | 说明 |
|------|------|------|
| `openvela/nand_firmware/update.img` | 9,785,898 B (md5 4defe2eb6941c9fbc4c89849d07e8525) | **全量刷机包 v8b** (v8 + rpmsg 缓存一致性根因修复 + USB 诊断超时转储) |
| `openvela/cmake_out/hd-rk3506-evm_nsh/vela.bin` | 2,881,436 B | NuttX 固件 (含 /dev/ota + USB host 修复 + littlefs/FAT + USB INFO 跟踪) |
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

**v8b 实测新情况 (2026-08-30)**: U 盘枚举成功 (控制传输全过) 但 MSC 的
bulk IN 挂死, /dev/sda 不出现; 拔线才报 `rk3506_chan_wait failed: -32`
(-32 = 拔线中止, 非 STALL)。v8b 已加两层诊断:
1. 驱动通道等待加 5s 超时 + 寄存器转储 (GINTSTS/HAINT/HCINT/HCCHAR/
   HCTSIZ) — 设备沉默时不再永久挂死, 转储直接指出卡在哪一层
   (XFRC 未处理 / NAK 静默 / 完全无中断)。
2. defconfig 开 DEBUG_USB_WARN/INFO — 串口将出现每笔传输的
   "ch%d start" / "rxflvl: ch%d INRECVD" / "ch%d done" 跟踪。
**复测**: 插 U 盘等 ~10s, 把串口完整日志发我 (重点: CBW dump、
ch 跟踪、超时转储)。

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

**诊断 (rpmsgtest FAIL 时自动执行 vring 探针)** — 读共享窗 vring idx 判断断点:
- `A7->M0 (avail used)`: avail≥1 = 我们的 ping 已入 vring1; used≥1 = M0 消费了 ping
- `M0->A7 (avail used)`: avail 起点 64 (A7 播种 64 个 rx 缓冲), used≥1 = A7
  收到过 M0 的消息 (NS announce) = M0 活着且完成握手
- 判读: avail=1,used=0 → M0 未启动或卡 link-up 前; used≥1 但无回包 → 回包
  通道断; 全 0 → 检查 rptun/SMC 启动链

**A7/M0 协议链路静态核对结论** (rk3506_rptun.c 与 SDK rk3506-mcu test_demo
/ rpmsg-lite RK3506 platform): 地址一致 (link_id 0x03, ept 0x4003); A7
master init 尾部 set_status(DRIVER_OK)→notify→mailbox3 kick {CMD=0x03,
DATA=RMSG} 释放 M0 link_state; A7 收包 isr 以 RPTUN_NOTIFY_ALL 上报,
rproc_virtio_notified(RSC_NOTIFY_ID_ANY) 两 vring 全处理。

### 7.4 curl "卡死" / DHCP — 先确认网络通不通

**curl 现象解读**: "只有 Ctrl+C 才显示结果" 最可能是 curl 卡在 TCP 连接
(网络没通时 SYN 一直重试, NuttX connect 无超时), Ctrl+C 中止后 curl 打印
错误信息 — 那个"结果"是报错。先按顺序确认网络:

```bash
nsh> ifconfig eth0           # 等 40s 再看! 有 inet addr 才算 DHCP 成功
nsh> ping 192.168.10.1       # 先 ping 网关 (IP 以 ifconfig 为准)
nsh> curl -v --connect-timeout 8 http://192.168.10.1/     # 局域网 http, 无 DNS 无 TLS
nsh> curl -v --connect-timeout 8 http://example.com/      # 再试公网 (DNS+TLS)
```
- ifconfig 无 IP: DHCP 没完成 — 看 `netcfg: dhcp on eth0 ok/failed` 是否
  出现在串口 (GMAC link up 晚于 nsh>, DHCP 重试窗 ~30s, 开机要等够 40s)。
- ping 网关通但 curl 公网卡 → DNS/TLS 问题, 把 -v 输出发我。
- curl 全部超时报错但网络通 → 服务器/防火墙侧问题。
- **--connect-timeout 8** 让 curl 自己退出, 不再需要 Ctrl+C。

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
