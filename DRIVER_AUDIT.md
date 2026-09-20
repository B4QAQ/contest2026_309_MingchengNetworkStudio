# RK3506 驱动标准规范审查报告

> 审查基准：nuttx-driver-development skill（dm90x0 以太网范式 / coding_rules / checklist）、
> NuttX 主线同框架驱动、Linux SDK v1.2.0 + U-Boot（寄存器/时序唯一事实源）。
> 审查日期：2026-09-19。构建：clean `--cmake` 全量 + `pack_nand.sh` 验证。

## ★ 实施状态（2026-09-19，用户已批准全部 5 项设计修复 + 清理）

每项单独 clean 构建 + pack + 独立提交（vendor/rockchip 分支）：

| 提交 | 内容 |
|---|---|
| 107aaa2 | GMAC0 netdev 标准修复（DR-001/002/004/005/006/009~012） |
| a0c9005 | FSPI/SPI-NAND/I2C 标准 arch 头 + 告警（修 up_udelay 隐式声明真缺陷） |
| 51547f6 | **I2C/SPI/GT911 串行锁 spin_lock_irqsave→nxmutex**（I2C-001/SPI-001/GT911-001） |
| de3998e | **CAN bus-off 恢复**（RCY bit19 丢帧释放+carrier，INT_MASK 全 32 位）+ irq_attach 检查（CAN-001/002） |
| b34feb4 | **RPTUN mbox kick 锁→nxmutex**（RPTUN-001） |
| 40a91c7 | **DR-003 ifup 非阻塞**（去 5s 持锁 sleep，链路轮询给 carrier）+ **DR-008 netcfg 唯一 DHCP**（关 NETINIT_DHCPC，SIOCGIFADDR 守卫+有界等 carrier） |
| 13f34cf | **VOP updatearea 做 fb D-cache clean**（VOP/LV-001，RK3506_VOP select FB_UPDATE） |
| ccf9ac0 | 删 spinand/cru boot 噪声日志 + 清 vop/spi/saradc/lowputc unused 告警 |

- 最终 update.img md5 `5351d6b9912e8515368bd2bd027689fd`；vendor/rockchip 告警仅剩 1 条
  （fspi sfc_irq_unmask，verbatim 对称保留），总告警 50→40（其余全在 nuttx/apps 上游，非本仓）。
- **待台架**：①开机自动拿 IP/拔插重 DHCP/无网线不卡死（DR-003/008）；②iperf 双向、坏帧/拔插、长稳（GMAC）；
  ③rpmsgtest ping/echo（RPTUN 锁）；④CAN 需收发器、显示/触摸无件——代码就绪，仅检测模式。

---

## 一、本轮已修复并提交

### commit 107aaa2 `chip: GMAC0 按 NuttX netdev 标准修复 TX/RX 资源回收与错误路径`
对照 `nuttx/drivers/net/dm90x0.c` 范式与 DWMAC4/stmmac 恢复语义：
- **DR-001 RX 坏帧钉死环**：新增 `HAL_GMAC_RxFrameReady()`，receive 循环对 `Recv()` 拒绝的
  CRC/超长帧 `CleanRX` 回收并计 `NETDEV_RXERRORS`；中断在 `DMA_RX_ERROR` 也排空。
- **DR-002 TX 覆盖在途帧**：`transmit` 先 `HAL_GMAC_TxReady()` 判环空再拷贝；环满 -EBUSY；
  `NETDEV_TXPACKETS` 移到 Send 成功后；Send 失败计 TXERRORS。
- **DR-004/005 超时/work**：TX 超时独立 `txtimeoutwork`（不再与链路轮询共用），超时做真实
  `HAL_GMAC_TxRecover()`（回收 TX 环 + tail-poke 恢复挂起 DMA）+ repoll，计 TXTIMEOUTS；
  linktimer 改在看门狗 expiry 内重装（worker 尾部重装可能静默停摆）+ bifup 守卫。
- **DR-006 错误路径**：ifup 改 goto 回滚（disable/detach IRQ + Stop），检查 PHYStartup /
  netdev_register 返回值。
- **DR-009** SIOCMIINOTIFY pid/event 读写加临界区防撕裂。
- **DR-010** 删除 Start() 残留 `syslog(LOG_INFO)`（v8i 静默）。
- **DR-011** 删除无效 `select ARCH_PHY_INTERRUPT`（无 PHY 中断线；SIOCMIINOTIFY 由
  `NETDEV_PHY_IOCTL` 提供，不依赖它）。
- **DR-012** 修正 yt8512b_init 与 EXTCLK 实际行为矛盾的旧注释。
- 修正原样移植笔误 `HAL_GMAC_GetRXIndex()`（返回了 txDescIdx）。
- 验证：clean 构建 3344 目标通过，GMAC 零 warning；update.img md5 `8f2b77ee…`。

### commit a0c9005 `chip: FSPI/SPI-NAND/I2C 使用标准 arch 头并消除编译告警`
- spinand `up_udelay` 隐式声明（-Wimplicit-function-declaration，64 位会指针截断）→ 补
  `#include <nuttx/arch.h>`；fspi 手写 extern 改标准头；i2c 两处 -Wformat + reset #ifdef 守卫。
- 验证：`make distclean` + clean 构建通过；update.img md5 `13b2f0f1…`。

---

## 二、其余驱动分级结论

图例：✅PASS（无实质问题）／⚠️低危（建议）／🔶中危（设计级，待 3.6）。

| 驱动 | 结论 | 说明 |
|---|---|---|
| rk3506_fspi | ✅ | spi-rockchip-sfc.c 逐行移植；等待全部有界超时，done 超时复位控制器，nand_op 全局互斥锁平衡；`0xFFFFFFFFF` 与 SDK 原文逐字一致（曾疑笔误，已核实自纠） |
| rk3506_spinand_fspi | ✅ | ECC 语义（-EUCLEAN/-EBADMSG）、页编程/擦除失败检查、坏块 raw 编程 ECC 开关平衡、MTD 边界/OOM 回滚齐全。仅余 894/974 两条 boot syslog（待打印决策，内含 2 条 %u 告警） |
| rk3506_serial | ✅ | 教科书 uart_ops：LSR 回调、IER 缓存、attach/enable 顺序、256 趟有界 ISR、up_putc 标准忙等 |
| rk3506_i2c | 🔶 I2C-001 | 见下 |
| rk3506_spi | 🔶 SPI-001 | 见下 |
| rk3506_can | 🔶 CAN-001 / ⚠️ CAN-002 | 见下 |
| rk3506_pwm | ✅ | uint64 ns/100MHz 换算、period<2 ERANGE、duty 钳位、HAL 序列正确（背光，面板未接） |
| rk3506_saradc | ✅ | 标准 adc_ops，ISR+au_receive 异步交付，attach 检查返回，trigger 临界区+EBUSY。⚠️ 1 个 unused `clksel` 告警 |
| rk3506_wdt | ✅ | watchdog_lowerhalf_s 标准；TOP 搜索/32 位溢出边界核对通过；start/stop/keepalive/capture 正确。⚠️ stop 未 irq_detach（重 attach 覆盖不累积，低危） |
| rk3506_iomux | ✅ | 纯 GRF 写，v8k 已清日志 |
| rk3506_cru | ✅ | 纯时钟门控初始化，无循环/锁；仅 boot 期 _info 噪声 |
| rk3506_timerisr | ✅ | ARM generic physical timer PPI30 one-shot 标准范式（irq_attach 返回未查为 NuttX 惯例） |
| rk3506_vop | 🔶 VOP/LV-001 | setpower spinlock 只护短 RMW（正确）；见下。⚠️ 1 个 unused `val` 告警 |
| rk3506_rptun | ⚠️ RPTUN-001 | mbox_isr 短而正确；mcu_boot/hold 为 v8f/h/j 用户已拍板序列 |
| rk3506_usbhost | ✅（已知限制） | 无 spinlock+睡眠反模式，等待有界，PHY 序列在 init 上下文；**已知**：select HAVE_ASYNCH 但未实现 asynch，插 USB hub 会 NULL 调用，U 盘/HID 直插正常（待 3.6 功能增强） |
| hd_rk3506_bringup | ✅ | /data 分区页单位数学正确（0x10800×512，first_page=16896）、失败不阻断、OTA 用整片 mtd、初始化在 late init |
| hd_rk3506_boardinit/appinit | ✅ | early 为空不阻塞；late 跑 bringup；appinit 在 LATE_INITIALIZE 下直接 OK，不会双重初始化 |
| hd_rk3506_gt911 | 🔶 GT911-001 | 见下；设备不在时 verify 失败即退出不空轮询，init goto 链正确 |
| hd_rk3506_st7701s | 无法验证 | 9-bit SPI 初始化、复位/上电 mdelay 均在线程上下文、无 spinlock；面板未接，时序需上台与手册核对 |
| hd_rk3506_lv_port_disp | 🔶 VOP/LV-001 | flush 按 stride 逐行 memcpy、边界检查齐全；cache 一致性见下 |
| rk3506_ota / hd_rk3506_ota | ✅ | AvbABData 布局/CRC32 大端/BCB 保留读-改-写、坏块中止；板级命令双缓冲回读校验，app 上下文裸 open/read/write 正确。update e2e 为台架验收项 |
| hd_rk3506_netcfg | ✅（DR-008 待定） | sigwait/EINTR/退避/settle 设计正确；开机双 DHCP 归属为待决策项 |
| hd_rk3506_rpmsgtest | ✅ | strlcpy/snprintf 有界、poll+超时、错误检查齐全 |

---

## 三、待用户 3.6 拍板的设计项

### DR-003 ifup 持锁 sleep（GMAC，中危，既有）
`rk3506_gmac0_ifup` 在 net_lock 下用 50×100ms `nxsig_usleep` 等链路（最长 5s），
期间冻结全部网络处理。
- **方案 A（推荐，dm90x0/现代 MAC 范式）**：ifup 只起 MAC/DMA/PHY、置 bifup、启动 linktimer
  即返回；首次 carrier 由既有的 2s 链路轮询 worker 给出。
- **方案 B**：保留等待但缩短/移出锁（ifup 恒在 net_lock 内被调，实际只能走异步，等同 A）。
- 影响：必须与 DR-008 一起设计，否则开机 DHCP 时序变化。

### DR-008 开机双 DHCP 归属（GMAC/netcfg/netinit，中危，既有）
netinit 线程（NETINIT_DHCPC）与 netcfg 的无条件首次 DHCP 并发跑两个客户端。
- **方案 A（推荐）**：netcfg 首次 DHCP 前先查 SIOCGIFADDR，已租约则跳过；仅做链路事件兜底。
- **方案 B**：关 CONFIG_NETINIT_DHCPC，netcfg 作为唯一 DHCP 所有者（改 defconfig，开机时序后移）。
- 注意：ifup 首次 carrier_on 不发 MIINOTIFY，netcfg 注册晚，必须保留一次无条件（或带 IP 判定的）
  首次尝试，否则会漏掉初始 up。

### I2C-001 / SPI-001 / GT911-001 自旋锁内长忙等（中危，同一族）
- I2C：`rk3506_i2c_transfer` 用 `spin_lock_irqsave` 包住整个轮询传输，纯轮询从不 irq_attach，
  总线卡死时关中断最长 200ms。
- SPI：`spi_ops.lock` 用 irqsave，上层在整个消息序列持锁，exchange 每字 udelay。
- GT911：worker 在 irqsave 临界区内做 I2C 读（gt911.c:681→693）。
- **建议修法**：总线锁改 `nxmutex`（轮询在开中断下进行），寄存器级短临界区才用 spinlock；
  GT911 读到本地缓冲再短锁发布。正常传输亚毫秒无感，故障时不再长时间关中断。
- 风险：锁策略改动需重测 I2C/SPI 设备（当前总线上无外接 I2C 设备、面板未接，影响面小但需验证）。

### CAN-001 无 bus-off 恢复（中危）
ISR 仅把 INT_BUSOFF 当唤醒标志；总线关闭后在途 tx_pkt 永不完成、TX 配额耗尽，发送永久卡死
直到 ifdown/ifup。建议：检测 BUSOFF → carrier_off → 复位/重初始化模式 → carrier_on
（对齐内核 rk3576_canfd restart）。CAN 当前无收发器/未上台，属预防性修复。
附带 CAN-002（低危）：ifup 的 irq_attach 返回值未检查。

### VOP/LV-001 framebuffer cache 一致性（中危，显示未接无法验证）
VOP fbmem 是 `kmm_zalloc` 的可缓存堆，vop.c 无任何 `up_clean_dcache`，defconfig 未开
`CONFIG_FB_UPDATE`（lv_port 的 FBIO_UPDATE flush 块被编译掉）。D-cache 开启（GMAC 显式
维护可证），LVGL memcpy 进 mmap fb 的数据可能滞留 cache → VOP DMA 读到旧帧。
- **方案 A**：vop 实现 updatearea 对脏区 up_clean_dcache + 开 CONFIG_FB_UPDATE。
- **方案 B**：fbmem 从 non-cacheable 区分配。
- 必须接面板后实测才能确认/验收。

### RPTUN-001（低-中危）
`rk3506_mbox_send` irqsave 内对邮箱忙最多自旋 10000 次 MMIO 读（最坏关中断约 200~500µs，
仅 M0 卡顿时）。该路径只在线程/work 上下文，可改 nxmutex+有界轮询。已台架 PASS，可不急动。

### 打印清理（待你一句话）
- spinand_fspi.c:894 / :974 两条 boot syslog（894 是 v8l 挂起项；删则 976 的两条 %u 告警一并消失）。
- cru.c 3 条 boot _info（SPI1/FSPI 时钟）。
GMAC 静默策略不动；这些是其它子系统，删/留请示下。

---

## 四、纯编译告警清单（均为既有、未改，清理可单独成一个 commit）
- rk3506_vop.c unused variable `val`
- rk3506_spi.c `g_spi_irq` unused const（轮询不用，预留 IRQ 表）
- rk3506_saradc.c unused `clksel`
- rk3506_lowputc.c `rk3506_uart_divisor` unused function
- rk3506_fspi.c `rk3506_sfc_irq_unmask` unused（verbatim 对称保留，AGENTS 已记录）
- nuttx 上游 usbhost_storage.c 6× -Wformat、dhara/gd5f 少量 %d（非本仓代码，不动）

## 五、待台架验证（修复后）
1. GMAC：iperf 双向吞吐；坏帧/CRC 注入（或拔插/拥塞）下 RX 不挂、ifconfig 统计自增；
   网线拔插链路事件；长稳 curl/HTTPS。
2. 若批准 I2C/SPI 锁修复：接外设验证传输；CAN（需收发器）bus-off 恢复。
3. 显示：接 ST7701S 面板验证 VOP/LV cache 一致性与上电时序。
4. OTA update 端到端（/data littlefs 已就绪）。
