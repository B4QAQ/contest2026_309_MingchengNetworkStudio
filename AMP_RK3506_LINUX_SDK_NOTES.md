# RK3506G2 AMP 实战笔记（M0 HAL 裸机 + 三核 Linux SMP）

> 本笔记记录在官方 SDK `rk3506_linux6.1_sdk_v1.2.0_iot_evm` 上从"纯 Linux"到
> "CPU0/1/2 跑 Linux SMP + M0 跑 HAL 裸机 + rpmsg 核间通信 pingpong 打通"的完整过程，
> 重点记录**踩过的坑与根因**，供 OpenVela 适配 AMP（v9 架构：U-Boot 放行 M0）参考。
>
> 验证平台：Vanxoak HD-RK3506G-EVM V1.0（NAND，128MB DDR3）。
> 最终现象：`nproc=3`、`/proc/cpuinfo` 三个 processor、
> 内核日志出现 `rockchip_rpmsg_test virtio0.rpmsg-mcu0-test.-1.16387: rx msg
> Rockchip rpmsg linux test! rx_count N(remote_ept_id: 0x4003)` 持续刷屏
> （= A7↔M0 pingpong 正常）。
>
> SDK 侧完整改动见 `RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/AMP_LINUX_SDK_NOTES.md`（同内容）。

---

## 0. 目标形态与资源划分（本次采用）

| 项 | 值 | 说明 |
|---|---|---|
| A 核 | CPU0/CPU1/CPU2 = Linux SMP | 内核编号 0/1/2（硬件 mpidr 0xf00/0xf01/0xf02） |
| M0 | HAL 裸机（无 RTOS） | 内核编号 **3**，rpmsg link-id **0x03** |
| M0 代码/数据 | **DRAM 0x03a00000，1MB** | 非 SRAM（SRAM 仅 32KB，实测只够 rpmsg 例程，加应用即栈溢出） |
| rpmsg 共享内存 | DRAM **0x03c00000，2MB** | M0 链接脚本 `LINUX_RPMSG` 与内核 DTS `rpmsg@3c00000` 必须一致 |
| M0 调试串口 | **UART1**（GPIO0_C0=RX / GPIO0_C1=TX，GPIO_BANK0），115200 8N1 | 见 §4 串口角色坑 |
| 核间中断 | mailbox3（A7→M0，A2B）/ mailbox0（M0→A7，B2A） | M0 侧 `rpmsg_platform.c` cpu_id=3 |

### 地址一致性三处核对（必须相等，烧录前自检）

| 出处 | 字段 | 值 |
|---|---|---|
| `amp_mcu_evm.its` | `load = <...>` | `0x03a00000` |
| `hal/project/rk3506-mcu/GCC/gcc_bus_m0_ddr.ld` | `RAM ORIGIN` | `0x03a00000` |
| 内核 DTSI reserved-memory | `mcu_reserved` | `0x03a00000`（size 0x100000） |

另：`gcc_bus_m0_ddr.ld` 的 `LINUX_RPMSG ORIGIN` − `RAM ORIGIN`
= 0x03c00000 − 0x03a00000 = **0x00200000 必须为正值**。
验证：`arm-none-eabi-readelf -h mcu.elf` 的 Entry point 必须是 `0x03a0xxxx`
（Thumb 态会带 bit0，如 0x3a00141 属正常）。

---

## 1. 总体改动清单（全部"复制后改名"，官方原件零改动）

SDK 目录按项目约定是"参考用"，本方案**不在任何官方文件上原地改**，
一律复制改名后让 defconfig 指向新文件：

| 文件 | 说明 |
|---|---|
| `device/rockchip/.chips/rk3506/vanxoak_hd_rk3506g_evm_nand_amp_mcu_defconfig` | 基于 `rockchip_rk3506_g_evm_nand_defconfig` 全量复制 + AMP 追加项 |
| `u-boot/configs/rk3506-amp-mcu.config` | 仅 `CONFIG_AMP=y`+`CONFIG_ROCKCHIP_AMP=y` 两行（merge-only 形态） |
| `kernel-6.1/arch/arm/configs/rk3506-amp-mcu.config` | `rockchip_amp.config` 副本 + `CONFIG_RPMSG_CHAR=y`+`CONFIG_RPMSG_ROCKCHIP_TEST=y` |
| `device/rockchip/.chips/rk3506/amp_mcu_evm.its` | 复制自 `amp_mcu.its`，**只含 mcu 一个 image**，load=0x03a00000，`loadables="mcu"` |
| `kernel-6.1/arch/arm/boot/dts/rk3506-amp-mcu-only.dtsi` | rockchip_amp 节点 + rpmsg + reserved-memory + mailbox0/3 + arm_pmu 亲和性 |
| `kernel-6.1/arch/arm/boot/dts/vanxoak-hd-rk3506g-evm-nand-amp-mcu.dts` | 原板级 dts + 上面 dtsi + `&uart1 { status="disabled"; }` |
| `hal/project/rk3506-mcu/GCC/gcc_bus_m0_ddr.ld` | 复制自 `gcc_bus_m0.ld`，RAM ORIGIN 改 0x03a00000 |
| `hal/project/rk3506-mcu/GCC/Makefile` | `LINKER_SCRIPT := gcc_bus_m0_ddr.ld` |
| `hal/project/rk3506-mcu/src/main.c` | `TEST_DEMO` 放开；UART1 + GPIO0_C0/C1 RMIO；波特率 115200 |
| `hal/project/rk3506-mcu/src/test_demo.c` | `RPMSG_LINUX_TEST` 放开（MASTER_ID=0/REMOTE_ID=3，端点 `rpmsg-mcu0-test` ept 0x4003） |
| `device/rockchip/.chips/rk3506/parameter-evm-nand-amp-mcu.txt` | 原 parameter 加 `0x00002000@0x00072800(amp)` 分区，userdata 顺延 grow |
| `device/rockchip/.chips/rk3506/package-file-amp-mcu` | 全分区清单 + `amp amp.img` 一行 |

**未改任何 U-Boot .c/.h**；官方 `rk3506-amp.dtsi`（它把 CPU2 让给 RTT，
与"三核 Linux"目标冲突）从未被 include。

---

## 2. 【核心坑】thunderboot 直启会绕过完整 U-Boot → M0 永远不被放行

这是本次排障的**主根因**，也是给 OpenVela 适配最重要的结论。

### 现象

配好了全部 AMP 配置、固件链接地址正确、amp 分区也打进了 update.img，
烧录后：M0 串口无输出、内核 rpmsg 只停在 probe（`rpmsg host is online`，
没有任何 `new channel`）。

### 定位过程

启动日志给出决定性证据：

```
U-Boot SPL 2017.09 ...
Trying to boot from MTD1
Trying fit image at 0x1800 sector          ← 加载 uboot.img
Trying kernel at 0x15800 sector from 'boot' part
Jumping to Kernel(0x01100000) via OP-TEE(0x00001000)   ← SPL 直接跳内核
```

**全程没有完整版 U-Boot 的启动横幅**（正常完整 U-Boot 会再打一次
`U-Boot 2017.09 ...`），也没有 `Handle standalone: 'mcu' ...` 字样。

### 机制

- Rockchip 的 AMP 加载在 **`u-boot/drivers/cpu/rockchip_amp.c` 的
  `amp_cpus_on()`**：从 GPT `amp` 分区读 FIT，`fit_standalone_release()`
  通过 SIP `ROCKCHIP_SIP_CONFIG_MCU_CODE_START_ADDR` 告知 TEE M0 入口地址并放行。
- 它在 `arch/arm/mach-rockchip/board.c` 的 **`board_late_init()`** 里、
  由 `#ifdef CONFIG_AMP` 包裹调用——**属于完整版 U-Boot 阶段**。
- 而 `rk3506_tb.config`（tb = thunderboot）里有 **`CONFIG_SPL_KERNEL_BOOT=y`**：
  SPL 的 FIT 加载器直接加载 kernel 并跳转，**完整版 U-Boot 整体被跳过**，
  `board_late_init→amp_cpus_on` 从未执行。
- 注意 `CONFIG_SPL_KERNEL_BOOT` 由 **U-Boot fragment（rk3506_tb.config）** 决定，
  与 SDK defconfig 的 `RK_BOOT_COMPRESSED` 无关——只改后者没用。
- SPL 的 FIT 加载器本身支持 "standalone" 子镜像（`common/spl/spl_fit.c:683-713`，
  加载后调 `spl_fit_standalone_release()`），rv1106/rv1126 都在 SPL 实现了该函数
  （直接写 CRU/GRF 寄存器复位+放行 MCU，不用 SIP）；**rk3506 没有实现**，
  走 weak 空实现 `return 0`（只加载到内存，不放行）。
- 对照官方 AMP 测试镜像 `HD-RK3506G-AMP.img` 的 strings：含完整 U-Boot 横幅
  （dtb=`rk3506-evb`，**不是** `rk3506-evb-tb`）+ `Handle standalone: '%s' at 0x%08x ...`
  + `[HAL INFO] Hello HD-RK3506G-EVM mcu`——证明官方 AMP 走的就是**完整 U-Boot 链**。

### 修复（方案 A：关掉 thunderboot 直启，走完整 U-Boot）

defconfig 三处改动：

```bash
# 1. 去掉 thunderboot 压缩直启
# RK_BOOT_COMPRESSED=y        ← 注释掉
# 2. U-Boot fragment 去掉 rk3506_tb（它含 CONFIG_SPL_KERNEL_BOOT=y），
#    用基础 rk3506_defconfig（dtb=rk3506-evb）+ amp fragment
RK_UBOOT_CFG_FRAGMENTS="rk3506-amp-mcu"
# 3. its 复用现有占位符模板（mk-fitimage.sh 会 sed 替换 @KERNEL_IMG@ 等；
#    不要指 kernel-6.1/boot.its——它是固定文件名格式，不适用此路径）
RK_BOOT_FIT_ITS_NAME="zboot.its"
RK_RECOVERY_FIT_ITS_NAME="zboot4recovery.its"
```

验证（烧录前）：

```bash
grep SPL_KERNEL_BOOT u-boot/.config
# 期望: # CONFIG_SPL_KERNEL_BOOT is not set
grep CONFIG_DEFAULT_DEVICE_TREE u-boot/.config
# 期望: "rk3506-evb"
strings u-boot/u-boot | grep -E "Handle standalone|Not fit"
# 期望能看到 amp_cpus_on 的字符串（证明真的编进了完整 U-Boot）
```

烧录后正确现象：SPL 之后出现**完整版 U-Boot 横幅**，
并有 `Handle standalone: 'mcu' at 0x03a00000 ... OK`。

### 备选方案 B（未采用，记录备查）

保留 thunderboot 直启，把 mcu 作为 standalone image 打进 thunderboot FIT，
并给 rk3506 实现 `spl_fit_standalone_release()`（参考 rv1106/rv1126 写寄存器）。
缺点：必须改 U-Boot 源码（违反"不动 U-Boot .c/.h"约束）；SPL 是 ARM32 无 PSCI，
不能复用完整版的 SIP 调用，寄存器序列需手工外推，风险高。

> **对 OpenVela v9 的启示**：v9 架构是"U-Boot 放行 M0 + NuttX 只跑协议"。
> 该架构成立的前提就是**启动链上完整 U-Boot 真的跑了 `amp_cpus_on`**。
> OpenVela 固件打包若用了 thunderboot/SPL 直启内核的 uboot 配置，
> 会出现与本笔记完全相同的症状（M0 毫无生命迹象）。检查方法同上：
> 看启动日志有没有完整 U-Boot 横幅 + `Handle standalone`。

---

## 3. U-Boot fragment 的 CONFIG_BASE_DEFCONFIG 陷阱

`u-boot/make.sh` 对 fragment 的处理：如果 `.config` 形态参数里**含有
`CONFIG_BASE_DEFCONFIG=`**，会执行 `make <base>_defconfig <frag>.config`
——**重建 .config，把之前 merge 进去的其他 fragment 全部冲掉**。

官方 `rk3506-amp.config` 含该行，若直接 `"rk3506_tb rk3506-amp"` 连用，
rk3506_tb 会被静默冲掉。官方多 fragment 先例（如 `rk3506_g_evm_sd_defconfig`
的 `"rk3506_tb rk-emmc"`）里 `rk-emmc.config` **没有**该行，是 merge-only 形态。

**做法**：AMP fragment 新建 `rk3506-amp-mcu.config`，只写两行
`CONFIG_AMP=y`、`CONFIG_ROCKCHIP_AMP=y`，不带 BASE_DEFCONFIG。
（本次最终方案里 rk3506_tb 整个不用了，见 §2，但该陷阱对其他 fragment 组合依然成立。）

---

## 4. M0 串口角色坑：UART1 是调试口，UART2 是收发测试口

万象奥科官方教程《RK3506-EVM-AMP/RK3506AMP功能测试.md》明确：

| 口 | 角色 | 引脚 | 说明 |
|---|---|---|---|
| **UART1** | M0 **调试口**（`HAL_DBG` / `Hello` 打印） | GPIO0_C0=RX / GPIO0_C1=TX（`RMIO_UART1_RX/TX`） | main.c `pUart=UART1`、`g_uart1Dev`，115200 |
| **UART2** | M0 **数据收发测试口**（`VANXOAK_UART_TEST`） | GPIO0_A0=RX / GPIO0_A2=TX（`RMIO_UART2_RX/TX`） | 独立测试宏，不是调试口 |
| UART0 | A 核控制台 | 板载调试串口 | — |

配套要求：
- main.c：`HAL_PINCTRL_SetRMIO(GPIO_BANK0, GPIO_PIN_C1, RMIO_UART1_TX)` +
  `(GPIO_BANK0, GPIO_PIN_C0, RMIO_UART1_RX)`，`HAL_UART_Init(&g_uart1Dev, ...)`。
- 内核板级 DTS：`&uart1 { status = "disabled"; }`（A/M 核外设互斥）。
- DTSI `rockchip_amp` 节点时钟用 `SCLK_UART1`/`PCLK_UART1`，
  pinctrl 用 `&rm_io16_uart1_rx &rm_io17_uart1_tx`。

常见翻车：把 UART2 当调试口、或 main.c 里 `HAL_UART_Init` 用了别的 `g_uartXDev`
而 RMIO 配的是另一组脚，都会导致"固件活着但串口哑"。本次还叠加过
"固件 1500000 vs 串口工具 115200"的波特率不匹配——**先对齐波特率再怀疑硬件**。

---

## 5. SDK 配置层的三个联动坑

### 5.1 `RK_AMP=y` 的 kconfig 软默认会关掉 rootfs/recovery/misc

`device/rockchip/common/configs/` 里这几项是 `default y if !RK_AMP`
（**软默认，不是 depends on**）。开 AMP 后它们被静默关掉，导致打包阶段
`afptool` 报 `AddFile open file failed, err=2`（package-file 列了但镜像没生成）。

**修法**：defconfig 显式写回，不动官方 Config.in、不改 output/.config（生成物）：

```bash
RK_ROOTFS=y
RK_RECOVERY=y
RK_MISC=y
```

### 5.2 RTT target 默认值的 scons 误触发

`Config.in.amp` 里 RTT target 默认 `"${RK_CHIP_FAMILY}-32"` / `"-mcu"`，
是普通 string，展开成 `-32`/`-mcu` **非空** → `check-amp.sh` 误判要求 scons。

**修法**（纯 HAL 方案，无 RT-Thread）：

```bash
RK_AMP_RTT_TARGET=""
RK_AMP_MCU_RTT_TARGET=""
RK_AMP_HAL_TARGET=""
```

### 5.3 WSL 下 buildroot 拒绝含空格/引号的 PATH

WSL 继承 Windows 的 `/mnt/c/Program Files/...` 路径，buildroot 报
`Your PATH contains spaces, TABs, and/or newline`。构建前清洗：

```bash
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -vE '[ "'"'"']' | grep -v '^$' | paste -sd:)
```

---

## 6. 内核 DTSI 关键配置（rk3506-amp-mcu-only.dtsi）

要点（本方案"三核 Linux + 单 M0"形态）：

- `rockchip_amp` 节点：`compatible="rockchip,amp"`，clocks 给全
  `HCLK_M0, STCLK_M0, SCLK_UART1, PCLK_UART1, PCLK_TIMER, CLK_TIMER0_CH5,
  PCLK_MAILBOX, PCLK_INTMUX`（对应 Linux `rockchip_amp` 驱动的
  `devm_clk_bulk_get_all`+`clk_bulk_prepare_enable`），pinctrl 指向 M0 串口引脚。
- `rpmsg@3c00000`：`compatible="rockchip,rpmsg"`，
  `mboxes = <&mailbox0 0 &mailbox3 0>`，`rockchip,link-id = <0x03>`，
  `memory-region` 指向 rpmsg-dma 保留区。
- `&mailbox0`/`&mailbox3` 均 `status="okay"`（基 dtsi 里默认 disabled）。
- reserved-memory（均 `no-map`）：`mcu@3a00000`(1MB)、`amp-shmem@3b00000`(1MB)、
  `rpmsg@3c00000`(1MB)、`rpmsg-dma@3d00000`(1MB, shared-dma-pool)。
- `&arm_pmu`：`interrupt-affinity = <&cpu0>, <&cpu1>, <&cpu2>`。
- **不要** include 官方 `rk3506-amp.dtsi`（它 `/delete-node/ cpu@f02`，
  把 CPU2 从 Linux 摘掉给 RTT，与三核目标冲突）；不要 `amp-irqs`、
  不要 `&mailbox2`、不要 `amp-cpu-aff-maskbits`（均为 RTT 方案的东西）。

启动日志里 `[ 1.02] rockchip_amp_get_gic_info: get amp gic cpu mask error`
是无 `amp-cpu-aff-maskbits` 时的预期告警，mcu-only 方案可忽略。

### 内存预留验证（烧录后）

```
/proc/iomem:  System RAM 在 039fffff 断开、03e00000 才续上
内核日志:     node 0: [0x00000000-0x039fffff]
              node 0: [0x03e00000-0x07ffffff]
```

即 0x03a00000~0x03dfffff 被正确从 Linux 摘出（`grep 3a000 /proc/iomem`
为空属正常——no-map 保留区不显示为独立条目，看 System RAM 断口即可）。

---

## 7. M0 固件（HAL）侧要点

- **链接脚本**：复制 `gcc_bus_m0.ld` → `gcc_bus_m0_ddr.ld`，
  `RAM ORIGIN = 0x03a00000`（DRAM 方案），`LINUX_RPMSG ORIGIN = 0x03c00000`。
  Makefile 的 `LINKER_SCRIPT` 指向新文件，原 ld 不动。
- **向量表自检**（`arm-none-eabi-objcopy -O binary mcu.elf mcu.bin` 后 `xxd -l 16`）：
  第 1 字=初始 SP（应 ≈ 0x03b00000 以内），第 2 字=Reset_Handler 入口
  （应 0x03a0xxxx | 1，Thumb）。
- **test_demo.c**：`RPMSG_LINUX_TEST` 打开后，M0 侧
  `MASTER_ID=0 / REMOTE_ID=3`（→ `RL_PLATFORM_SET_LINK_ID(0,3)` = link 0x03），
  端点 `0x4003 / "rpmsg-mcu0-test"`，与内核 `rockchip_rpmsg_test` 驱动互为对端。
- 官方教程的 rpmsg M0 侧实现用的就是 Rockchip `test_demo.c` 框架 +
  `VANXOAK_TEST_DEMO`/`RPMSG_LINUX_TEST` 宏（本 SDK 里直接用 `TEST_DEMO` 即可）。

---

## 8. rpmsg 刷屏是"成功"不是故障

验证通过后的典型刷屏：

```
rockchip_rpmsg_test virtio0.rpmsg-mcu0-test.-1.16387:
    rx msg Rockchip rpmsg linux test! rx_count N(remote_ept_id: 0x4003)
```

- 这是 `rockchip_rpmsg_test`（`CONFIG_RPMSG_ROCKCHIP_TEST=y`）驱动的行为：
  每条消息 `dev_info` 打印，直到 `MSG_LIMIT 10000` 才打印
  `Rockchip rpmsg test exit!` 停止。
- `remote_ept_id: 0x4003` 正是 M0 端点；`rx_count` 递增 = 双向 pingpong 活着。
- 想手动验证而不刷屏：保留 `CONFIG_RPMSG_CHAR=y`（`/dev/rpmsg0`，
  `echo hello > /dev/rpmsg0`），关掉 `CONFIG_RPMSG_ROCKCHIP_TEST` 即可；
  本次验证后按用户决定**保持现状**。

---

## 9. 排障方法论小结（给后来者的检查顺序）

1. **先确认 M0 有没有被放行**：启动日志找完整 U-Boot 横幅 +
   `Handle standalone: 'mcu' at 0x03a00000`。没有 → 启动链问题（§2），
   别去查固件。
2. **固件静态自检**：`readelf -h mcu.elf` 的 Entry、`xxd -l16` 向量表、
   ITS load 地址、DTS reserved-memory 四处一致（§0）。
3. **串口无输出的排查顺序**：波特率对齐 → 串口角色（UART1 vs UART2，§4）→
   RMIO 引脚与 `g_uartXDev` 配对 → 接线交叉（TX 接 RX）→ 才怀疑 M0 没跑。
4. **rpmsg 只 probe 无 new channel** = M0 没跑到端点宣告 = 回到第 1 步。
5. **内存预留是否生效**：看 System RAM 在 0x03a00000 处断开，而非
   `grep 3a000` 有没有输出（§6）。

## 10. 与 OpenVela v9 架构的对应关系

| Linux SDK（本笔记） | OpenVela（比赛仓） |
|---|---|
| 完整 U-Boot `amp_cpus_on()` 从 `amp` 分区读 FIT 放行 M0 | v9 同架构：U-Boot 放行，NuttX 只跑 mailbox/rpmsg 协议（`rk3506_rptun.c` 不再碰 reset/拷贝/放行） |
| `amp_mcu_evm.its` load=0x03a00000（DRAM） | `openvela/nand_firmware/amp/amp.its` load=0xfff84000（SRAM，32KB 约束见 §0） |
| parameter 加 `amp` 分区 + package-file 加 `amp amp.img` | `nand_firmware/parameter.txt` 加 amp 分区（0x10800+0x800）+ `pack_nand.sh` 打包 amp.img |
| kernel `rockchip_rpmsg` + `rockchip_rpmsg_test` | NuttX rptun + `hd_rk3506_rpmsgtest.c` |
| **前提**：启动链必须真的跑到完整 U-Boot（§2 是本笔记对 v9 最重要的输入） | v9 当前求助点"M0 收 kick 不回 B2A"排查时，先用 §9 第 1 步确认放行链路 |
