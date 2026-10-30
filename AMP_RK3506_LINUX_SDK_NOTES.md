# RK3506G2 AMP 实施方案（Linux SDK 落地版）

> **平台**：Vanxoak HD-RK3506G-EVM V1.0（NAND，128MB DDR3）
> **SDK**：`rk3506_linux6.1_sdk_v1.2.0_iot_evm`
> **目标**：CPU0/CPU1/CPU2 跑 Linux SMP（三核），M0 跑 HAL 裸机，A7↔M0 rpmsg 双向通信
> **状态**：已上板验证通过
>
> 本文是**可直接照做的实施规格**：每个文件的完整内容、每处取值为什么这么定、
> 以及不这么做会怎样。给 OpenVela 适配 AMP 参考。

---

## 1. 最终形态总览

```
┌─────────────────────────────────────────────────────────────┐
│  NAND (update.img)                                          │
│  bootloader → uboot → misc → boot → recovery → rootfs       │
│              → oem → amp(0x72800) → userdata                │
└─────────────────────────────────────────────────────────────┘
启动链（关键）：
  BootROM → SPL(MiniLoaderAll) → 完整版 U-Boot → [amp_cpus_on 放行 M0] → Kernel
                                                  ↓
                                    读 amp 分区 FIT，SIP 告知 TEE
                                    M0 入口 0x03a00000 并放行

Linux 侧：CPU0/1/2 SMP，0x03a00000~0x03dfffff 从 System RAM 摘出
M0 侧：跑在 DRAM 0x03a00000，串口 UART1，rpmsg link-id 0x03
```

### 资源划分表（本文所有取值的总纲）

| 项目 | 取值 | 依据 |
|---|---|---|
| Linux CPU | CPU0/CPU1/CPU2 | 板级 mpidr 0xf00/0xf01/0xf02 |
| M0 内核编号 | **3** | `test_demo.c` `REMOTE_ID=3`，rpmsg link-id = 0x03 |
| M0 固件加载地址 | **0x03a00000**（1MB） | DRAM 方案；SRAM 仅 32KB 不够跑应用 |
| M0 栈顶 | 0x03affc00 | 链接脚本 `ORIGIN+LENGTH-0x400` |
| rpmsg 共享内存 | **0x03c00000**（2MB） | M0 链接脚本 `LINUX_RPMSG` + 内核 DTS `rpmsg@3c00000` |
| M0 访问共享区偏移 | 0x00200000 | = 0x03c00000 − 0x03a00000 |
| M0 调试串口 | **UART1**，GPIO0_C0=RX/GPIO0_C1=TX | 官方教程：UART1 调试口，UART2 是数据测试口 |
| M0 串口波特率 | 115200 8N1 | 官方教程 + 串口工具对齐 |
| A7→M0 中断 | mailbox3（A2B） | rk3506 mailbox 编号 |
| M0→A7 中断 | mailbox0（B2A） | 同上 |
| rpmsg 端点 | 0x4003 / `rpmsg-mcu0-test` | M0 通告，内核 `rockchip_rpmsg_test` 配对 |

### ⚠️ 地址一致性硬约束（三处必须相等，改任一必须同步改另两处）

| # | 文件 | 字段 | 值 |
|---|---|---|---|
| ① | `device/rockchip/.chips/rk3506/amp_mcu_evm.its` | `load = <...>` | `0x03a00000` |
| ② | `hal/project/rk3506-mcu/GCC/gcc_bus_m0_ddr.ld` | `RAM(rxw) : ORIGIN` | `0x03a00000` |
| ③ | `kernel-6.1/arch/arm/boot/dts/rk3506-amp-mcu-only.dtsi` | `mcu_reserved: mcu@3a00000 { reg }` | `0x03a00000` |

---

## 2. 改动文件总清单（12 个，全部"复制后改名"）

> **原则**：官方 SDK 文件一个不原地改。所有改动 = 新建文件 + defconfig 指过去。
> 这样官方配置可随时对照、可回滚。

| # | 文件 | 新/改 | 作用 |
|---|---|---|---|
| 1 | `device/rockchip/.chips/rk3506/vanxoak_hd_rk3506g_evm_nand_amp_mcu_defconfig` | 新 | 总开关，指向下面所有新文件 |
| 2 | `u-boot/configs/rk3506-amp-mcu.config` | 新 | U-Boot 打开 CONFIG_AMP |
| 3 | `kernel-6.1/arch/arm/configs/rk3506-amp-mcu.config` | 新 | 内核打开 AMP + rpmsg + rpmsg_test |
| 4 | `device/rockchip/.chips/rk3506/amp_mcu_evm.its` | 新 | M0 固件 FIT（load=0x03a00000，只含 mcu） |
| 5 | `kernel-6.1/arch/arm/boot/dts/rk3506-amp-mcu-only.dtsi` | 新 | AMP 节点 + rpmsg + 内存预留 |
| 6 | `kernel-6.1/arch/arm/boot/dts/vanxoak-hd-rk3506g-evm-nand-amp-mcu.dts` | 新 | 板级 DTS（原 dts + 上面 dtsi + 关 uart1） |
| 7 | `hal/project/rk3506-mcu/GCC/gcc_bus_m0_ddr.ld` | 新 | M0 链接到 DRAM 0x03a00000 |
| 8 | `hal/project/rk3506-mcu/GCC/Makefile` | **改** | `LINKER_SCRIPT` 指向新 ld |
| 9 | `hal/project/rk3506-mcu/src/main.c` | **改** | UART1 + TEST_DEMO |
| 10 | `hal/project/rk3506-mcu/src/test_demo.c` | **改** | 打开 RPMSG_LINUX_TEST |
| 11 | `device/rockchip/.chips/rk3506/parameter-evm-nand-amp-mcu.txt` | 新 | 加 amp 分区 |
| 12 | `device/rockchip/.chips/rk3506/package-file-amp-mcu` | 新 | 打包清单加 amp |

**未改动**：任何 U-Boot `.c/.h`；官方 `rk3506-amp.dtsi`、`amp_mcu.its`、
`rockchip_amp.config`、`rk3506-amp.config`、`parameter-evm-nand.txt`、
`package-file`、原 defconfig、`gcc_bus_m0.ld`。

---

## 3. 逐文件详细方案

### 3.1 defconfig（总开关）

**文件**：`device/rockchip/.chips/rk3506/vanxoak_hd_rk3506g_evm_nand_amp_mcu_defconfig`
**来源**：`rockchip_rk3506_g_evm_nand_defconfig` 全量复制，保留全部原项，再追加/修改

```bash
# ===== 原 defconfig 保留项（节选，实际全量保留）=====
RK_BUILDROOT_BASE_CFG="hd_rk3506g_evm_nand"
RK_ROOTFS_UBI=y
RK_ROOTFS_INSTALL_MODULES=y
RK_WIFIBT=y
RK_WIFIBT_ON_BOARD=y
RK_WIFIBT_CHIP="AIC8800DC"
RK_UBOOT_SPL=y
RK_KERNEL_CFG="vanxoak_hd_rk3506g_evm_nand_defconfig"
RK_FLASH_SIZE=2048
RK_EXTRA_PARTITION_1_SRC="rk3506_oem"
RK_USE_FIT_IMG=y

# ===== 改动 1：关闭 thunderboot 直启（本方案最关键的一处）=====
# 原值 RK_BOOT_COMPRESSED=y 被注释掉
# RK_BOOT_COMPRESSED=y
RK_BOOT_FIT_ITS_NAME="zboot.its"
RK_RECOVERY_FIT_ITS_NAME="zboot4recovery.its"

# ===== 改动 2：package-file 指向新清单 =====
RK_PACKAGE_FILE_CUSTOM=y
RK_PACKAGE_FILE="package-file-amp-mcu"

# ===== 追加：AMP 开关 =====
RK_AMP=y
RK_AMP_ARM32=y

# ===== 追加：重开被 RK_AMP 联动关掉的项（原因见 §4.1）=====
RK_ROOTFS=y
RK_RECOVERY=y
RK_MISC=y

# ===== 追加：AMP FIT 与 M0 HAL 工程 =====
RK_AMP_FIT_ITS="amp_mcu_evm.its"
RK_AMP_MCU_HAL_TARGET="rk3506-mcu"

# ===== 追加：置空 RTT/HAL-AP target（原因见 §4.2）=====
RK_AMP_RTT_TARGET=""
RK_AMP_MCU_RTT_TARGET=""
RK_AMP_HAL_TARGET=""

# ===== 改动 3：U-Boot fragment 去掉 rk3506_tb（原因见 §5）=====
RK_UBOOT_CFG_FRAGMENTS="rk3506-amp-mcu"

# ===== 改动 4：内核 fragment 原值 + 追加 amp =====
RK_KERNEL_CFG_FRAGMENTS="rk3506-display.config rk3506-usb-otg.config rk3506-amp-mcu.config"

# ===== 追加：板级 DTS / parameter 指向新文件 =====
RK_KERNEL_DTS_NAME="vanxoak-hd-rk3506g-evm-nand-amp-mcu"
RK_PARAMETER="parameter-evm-nand-amp-mcu.txt"
```

**为什么要注释 `RK_BOOT_COMPRESSED`**：见 §5（这是本次排障主根因）。

---

### 3.2 U-Boot fragment

**文件**：`u-boot/configs/rk3506-amp-mcu.config`（新建，全文只有 2 行有效配置）

```
# 刻意不含 CONFIG_BASE_DEFCONFIG，理由见 §6
CONFIG_AMP=y
CONFIG_ROCKCHIP_AMP=y
```

**必须产出的 `.config` 结果（烧录前自检）**：

```bash
grep -E "CONFIG_AMP=|CONFIG_ROCKCHIP_AMP=|SPL_KERNEL_BOOT|CONFIG_DEFAULT_DEVICE_TREE" u-boot/.config
```
期望：
```
CONFIG_AMP=y
CONFIG_ROCKCHIP_AMP=y
# CONFIG_SPL_KERNEL_BOOT is not set     ← 必须 not set，否则 M0 不被放行
CONFIG_DEFAULT_DEVICE_TREE="rk3506-evb" ← 完整 U-Boot 用，不是 rk3506-evb-tb
```

---

### 3.3 内核 fragment

**文件**：`kernel-6.1/arch/arm/configs/rk3506-amp-mcu.config`
**来源**：官方 `rockchip_amp.config` 原样复制，**尾部追加 2 行**

```bash
# （前面是 rockchip_amp.config 原文，未改动）
...
# CONFIG_VIRTIO_BLK is not set

# ===== 追加 =====
CONFIG_RPMSG_CHAR=y            # 生成 /dev/rpmsg0，供 echo hello > /dev/rpmsg0 验证
CONFIG_RPMSG_ROCKCHIP_TEST=y   # 注册 "rpmsg-mcu0-test"，与 M0 侧 demo 配对打 pingpong
```

---

### 3.4 AMP FIT（M0 固件打包脚本）

**文件**：`device/rockchip/.chips/rk3506/amp_mcu_evm.its`
**来源**：官方 `amp_mcu.its` 复制后改名，**只改 load 地址**

```dts
/dts-v1/;
/ {
	description = "Rockchip AMP FIT Image";
	#address-cells = <1>;

	images {
		mcu {
			description  = "mcu";
			data         = /incbin/("./mcu.bin");
			type         = "standalone";	// 必须是 standalone
			compression  = "none";
			arch         = "arm";		// 与 U-Boot 状态一致
			load         = <0x03a00000>;	// ★ 与 .ld ORIGIN / DTS mcu_reserved 一致
			udelay       = <1000000>;
			compile {
				sys      = "hal";
				core     = "mcu";
			};
			hash { algo = "sha256"; };
		};
	};

	share {
		rpmsg_base = <0x03c00000>;	// = 0x00200000 偏移 + 0x03a00000 基址
		rpmsg_size = <0x00200000>;
	};

	configurations {
		default = "conf";
		conf {
			description = "Rockchip AMP images";
			rollback-index = <0x0>;
			loadables = "mcu";		// 只放 mcu，无 amp2/linux
			signature {
				algo = "sha256,rsa2048";
				padding = "pss";
				key-name-hint = "dev";
				sign-images = "loadables";
			};
		};
	};
};
```

**要点**：
- 官方 `amp_mcu.its` 里 `load = <0xfff84000>`（SRAM），本方案改 `0x03a00000`（DRAM）。
- **只有 `mcu` 一个 image**，不 include 官方 `amp_linux.its` 的 amp2/linux 节点
  （那是 CPU2 跑 RTOS 的方案）。
- `loadables = "mcu"`，不能写别的。
- `compile { sys = "hal"; core = "mcu"; }` 会被 `mk-amp.sh` 读出来决定编哪个 HAL 工程
  （`hal/project/rk3506-mcu`）。

**构建产出验证**：

```bash
./build.sh amp
# 日志应含：Building CPU mcu: HAL-->rk3506-mcu / Load Address: 0x03a00000
file output/firmware/amp.img
# 应含：FIT image, Load Address 0x03a00000, type "standalone", arch ARM
```

> 已知无害告警：`25-amp.sh: line 175: ((: > 0xff : syntax error` ——
> mk-amp.sh 解析 ITS 的 `cpu` 字段，mcu image 无该字段导致，编译照常成功。

---

### 3.5 内核 DTSI（AMP 节点）

**文件**：`kernel-6.1/arch/arm/boot/dts/rk3506-amp-mcu-only.dtsi`（新建，91 行）

**与官方 `rk3506-amp.dtsi` 的根本区别**：

| 项 | 官方 rk3506-amp.dtsi | 本方案 |
|---|---|---|
| `/delete-node/ cpu@f02` | **有**（CPU2 让给 RTOS） | **无** → Linux 保留三核 |
| `amp-irqs` / `amp-cpu-aff-maskbits` | 有（给 CPU2 做 GIC 路由） | **无** |
| rpmsg 邮箱 | mailbox2/0 + link-id 0x02 | **mailbox0/3 + link-id 0x03**（M0） |
| 固件地址 | SRAM 0xfff80000 | **DRAM 0x03a00000** |

**关键内容**：

```dts
#include <dt-bindings/soc/rockchip-amp.h>

/ {
	rockchip_amp: rockchip-amp {
		compatible = "rockchip,amp";
		/* 这 8 个时钟由 Linux rockchip_amp 驱动 clk_bulk_prepare_enable 全部打开，
		 * 缺任何一个 M0 都可能跑不起来（时基/串口/邮箱死掉） */
		clocks = <&cru HCLK_M0>, <&cru STCLK_M0>,
			<&cru SCLK_UART1>, <&cru PCLK_UART1>,	// M0 调试串口
			<&cru PCLK_TIMER>, <&cru CLK_TIMER0_CH5>,// hal_conf.h SYS_TIMER=TIMER5 时基
			<&cru PCLK_MAILBOX>, <&cru PCLK_INTMUX>;	// 邮箱 + 中断转发
		pinctrl-names = "default";
		pinctrl-0 = <&rm_io16_uart1_rx &rm_io17_uart1_tx>;	// UART1 引脚
		status = "okay";
	};

	/* M0 rpmsg：rx=mailbox0(M0→A7)，tx=mailbox3(A7→M0) */
	rpmsg: rpmsg@3c00000 {
		compatible = "rockchip,rpmsg";
		mbox-names = "rpmsg-rx", "rpmsg-tx";
		mboxes = <&mailbox0 0 &mailbox3 0>;
		rockchip,vdev-nums = <1>;
		rockchip,link-id = <0x03>;			// ★ M0 = 3
		reg = <0x3c00000 0x20000>;
		memory-region = <&rpmsg_dma_reserved>;
		status = "okay";
	};
};

&arm_pmu {
	interrupt-affinity = <&cpu0>, <&cpu1>, <&cpu2>;		// 三核
};

&mailbox0 { rockchip,txpoll-period-ms = <1>; status = "okay"; };
&mailbox3 { rockchip,txpoll-period-ms = <1>; status = "okay"; };

&reserved_memory {
	mcu_reserved: mcu@3a00000 {				// ★ 与 ITS/.ld 一致
		reg = <0x03a00000 0x100000>;			// 1MB
		no-map;
	};
	amp_shmem_reserved: amp-shmem@3b00000 {
		reg = <0x03b00000 0x100000>;
		no-map;
	};
	rpmsg_reserved: rpmsg@3c00000 {
		reg = <0x03c00000 0x100000>;
		no-map;
	};
	rpmsg_dma_reserved: rpmsg-dma@3d00000 {
		compatible = "shared-dma-pool";
		reg = <0x03d00000 0x100000>;
		no-map;
	};
};
```

**注意**：`&mailbox0`/`&mailbox3` 在基线 `rk3502.dtsi` 里默认 `disabled`，
必须显式 `okay`，否则 rpmsg probe 失败。

---

### 3.6 板级 DTS

**文件**：`kernel-6.1/arch/arm/boot/dts/vanxoak-hd-rk3506g-evm-nand-amp-mcu.dts`（新建，全文 17 行）

```dts
/dts-v1/;

#include "vanxoak-hd-rk3506g-evm-nand-v1.dts"	// 原板级 dts，不动
#include "rk3506-amp-mcu-only.dtsi"		// 上面新建的 AMP dtsi

/* A 核与 M 核外设互斥：M0 用 UART1，Linux 必须让出 */
&uart1 {
	status = "disabled";
};
```

**不需要**改 `arch/arm/boot/dts/Makefile`：SDK 的 `%.img` 通配规则会自动处理 `<dts>.img`。

---

### 3.7 M0 链接脚本（DRAM 方案）

**文件**：`hal/project/rk3506-mcu/GCC/gcc_bus_m0_ddr.ld`
**来源**：官方 `gcc_bus_m0.ld` 复制后改名（原文件不动），**只改 MEMORY 段**

```ld
MEMORY
{
    /* 官方：RAM ORIGIN = 0x00000000, LENGTH = 0x8000   （SRAM 32KB） */
    RAM(rxw) : ORIGIN = 0x03a00000, LENGTH = 0x00100000   /* DRAM 1MB */
    /* 官方：LINUX_RPMSG ORIGIN = 0x03c00000, LENGTH = 0x200000（此项不变） */
    LINUX_RPMSG (rxw) : ORIGIN = 0x03c00000, LENGTH = 0x00200000
}
```

其余段布局与官方完全一致。栈顶由脚本算出：
`__STACK_START = ORIGIN(RAM) + LENGTH(RAM) - 0x400 = 0x03affc00`。

**为什么必须换 DRAM**：SRAM 只有 32KB，实测只够跑 rpmsg 例程，加自己的应用就栈溢出。
DRAM 方案给 1MB，充裕。

**Makefile 配套**（`hal/project/rk3506-mcu/GCC/Makefile` 第 14 行）：

```makefile
LINKER_SCRIPT := gcc_bus_m0_ddr.ld    # 原为 gcc_bus_m0.ld
```

**静态验证（编译后必做）**：

```bash
cd hal/project/rk3506-mcu/GCC
arm-none-eabi-readelf -h mcu.elf | grep Entry
# 期望 0x3a00141（0x03a00140 | Thumb bit，属正常）

arm-none-eabi-objcopy -O binary mcu.elf /tmp/m.bin && xxd -l 16 /tmp/m.bin
# 期望前两字：[SP=0x03affc00] [Reset=0x03a00141]
# 实际输出：00fc af03  4101 a003 → 正确

arm-none-eabi-nm mcu.elf | grep -E "Reset_Handler|__StackTop|linux_share_rpmsg"
# 期望：03a00140 T Reset_Handler / 03affc00 B __StackTop / 03c00000 B __linux_share_rpmsg_start__
```

---

### 3.8 M0 main.c

**文件**：`hal/project/rk3506-mcu/src/main.c`（改 2 处）

```c
/* 改动 1：放开 demo（第 10 行，原来是注释掉的） */
#define TEST_DEMO

/* 改动 2：串口 = UART1（第 19 行） */
static struct UART_REG *pUart = UART1;      // 原为 UART4 或其它

/* main() 里的串口初始化（第 79-86 行） */
    /* UART Init: M0 调试串口 = UART1（GPIO0_C0=RX / GPIO0_C1=TX，GPIO_BANK0，115200） */
    HAL_PINCTRL_SetRMIO(GPIO_BANK0, GPIO_PIN_C1, RMIO_UART1_TX);
    HAL_PINCTRL_SetRMIO(GPIO_BANK0, GPIO_PIN_C0, RMIO_UART1_RX);
    HAL_UART_Init(&g_uart1Dev, &hal_uart_config);

    HAL_DBG("Hello RK3506 mcu\n");
```

`hal_uart_config` 的波特率 = `UART_BR_115200`（第 64 行）。

**三个必须配对的东西**（错一个就"固件活着但串口哑"）：
1. `pUart`（`_write` 打印走这个）
2. `HAL_UART_Init(&g_uartXDev, ...)`（初始化哪个 UART 控制器）
3. `HAL_PINCTRL_SetRMIO(...RMIO_UARTx_TX/RX)`（哪组引脚复用）

三者必须都是同一个 UART 号。

---

### 3.9 M0 test_demo.c

**文件**：`hal/project/rk3506-mcu/src/test_demo.c`（改 1 处）

```c
/* 第 13 行：放开 rpmsg 测试（原来注释掉的） */
#define RPMSG_LINUX_TEST
```

配套常量（文件内已有，无需改，仅确认）：

```c
#define MASTER_ID ((uint32_t)0)      // A7 是 master
#define REMOTE_ID ((uint32_t)3)      // M0 = 3 → link-id 0x03
#define RPMSG_HAL_REMOTE_TEST_EPT      0x4003U
#define RPMSG_HAL_REMOTE_TEST_EPT_NAME "rpmsg-mcu0-test"
```

这三个值必须与内核侧对得上：
- `REMOTE_ID=3` ↔ DTS `rockchip,link-id = <0x03>`
- 端点名 `rpmsg-mcu0-test` ↔ 内核 `rockchip_rpmsg_test` 驱动注册的名字
- 端点号 `0x4003` ↔ 内核日志里的 `remote_ept_id: 0x4003`

---

### 3.10 parameter（加 amp 分区）

**文件**：`device/rockchip/.chips/rk3506/parameter-evm-nand-amp-mcu.txt`
**来源**：官方 `parameter-evm-nand.txt` 复制后改名，只在 CMDLINE 里插入 amp 分区

```
CMDLINE:mtdparts=:0x00001000@0x00000800(vnvm),0x00004000@0x00001800(uboot),0x00001000@0x00005800(misc),0x0000f000@0x00006800(recovery),0x00005000@0x00015800(boot),0x00050000@0x0001a800(rootfs),0x00008000@0x0006a800(oem),0x00002000@0x00072800(amp),-@0x00074800(userdata:grow)
```

分区变化对照：

| 分区 | 官方 offset | 本方案 offset | 说明 |
|---|---|---|---|
| vnvm…oem | 不变 | 不变 | 全部原样 |
| **amp** | 无 | **0x00072800，size 0x00002000（4MB）** | 新增，插在 oem 之后 |
| userdata | 0x00072800 | **0x00074800**（顺延），`grow` | 给 amp 让位 |

**注意**：amp 分区大小 4MB（0x2000 扇区），实际 amp.img 只 32KB，留足余量。

---

### 3.11 package-file（打包清单）

**文件**：`device/rockchip/.chips/rk3506/package-file-amp-mcu`
**来源**：官方 `package-file` 复制后改名，**只加一行 `amp amp.img`**

```
package-file	package-file
parameter	parameter.txt
bootloader	MiniLoaderAll.bin
uboot	uboot.img
misc	misc.img
boot	boot.img
recovery	recovery.img
rootfs	rootfs.img
oem	oem.img
amp	amp.img          ← 新增
userdata	userdata.img
```

配合 defconfig `RK_PACKAGE_FILE_CUSTOM=y` + `RK_PACKAGE_FILE="package-file-amp-mcu"`
（相对 `$RK_CHIP_DIR` 解析）。官方 `package-file` 不动。

> **前提**：`RK_ROOTFS/RK_RECOVERY/RK_MISC` 必须已重开（§3.1），
> 否则这些镜像不生成、afptool 报 `AddFile open file failed, err=2`。

---

## 4. SDK 配置层的三个联动坑（必须知道，否则编译/打包直接失败）

### 4.1 `RK_AMP=y` 的软默认会静默关掉 rootfs/recovery/misc

**表现**：打包时 `afptool` 报 `AddFile open file failed, err=2`（package-file 列了但文件不存在）。

**原因**：`device/rockchip/common/configs/` 里这几项是 **`default y if !RK_AMP`**
（软默认，不是 `depends on`）。开 AMP 后它们被静默关掉。

**修法**：defconfig 显式写回（不动官方 `Config.in`，不改 `output/.config`——那是生成物）：

```bash
RK_ROOTFS=y
RK_RECOVERY=y
RK_MISC=y
```

### 4.2 RTT target 默认值导致误要求 scons

**表现**：`./build.sh amp` 报找不到 `scons`。

**原因**：`Config.in.amp` 里 `RK_AMP_RTT_TARGET` 默认 `"${RK_CHIP_FAMILY}-32"`、
`RK_AMP_MCU_RTT_TARGET` 默认 `"-mcu"`，都是**普通 string**，变量展开后成
`-32`/`-mcu`（非空），`check-amp.sh` 用 `[ "$RK_AMP_RTT_TARGET" ]` 判断 →
非空就要求编 RT-Thread。

**修法**（纯 HAL 方案无 RT-Thread）：

```bash
RK_AMP_RTT_TARGET=""
RK_AMP_MCU_RTT_TARGET=""
RK_AMP_HAL_TARGET=""
```

### 4.3 WSL 下 buildroot 拒绝含空格/引号的 PATH

**表现**：buildroot 报 `Your PATH contains spaces, TABs, and/or newline (\n) characters`。

**原因**：WSL 继承 Windows 的 `/mnt/c/Program Files/...`（甚至带裸引号）。

**修法**：构建前清洗 PATH

```bash
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -vE '[ "'"'"']' | grep -v '^$' | paste -sd:)
```

---

## 5. ★ 核心问题：thunderboot 直启让 M0 永远不被放行

这是本次从"配好了一切但 M0 毫无动静"到"打通"的**唯一根因**，
也是给 OpenVela 适配最重要的结论。

### 5.1 现象

所有 AMP 配置就绪、`amp.img` 正确打进 update.img、内存预留生效
（`/proc/iomem` 里 System RAM 在 0x03a00000 处断开）、内核 rpmsg 也 probe 成功：

```
rockchip-rpmsg 3c00000.rpmsg: rockchip rpmsg platform probe.
rockchip-rpmsg 3c00000.rpmsg: rpdev vdev0: vring0 0x3c00000, vring1 0x3c08000
virtio_rpmsg_bus virtio0: rpmsg host is online
```

**但**：M0 串口无任何输出，rpmsg 停在 probe，**没有 `new channel`**（M0 从未宣告端点）。

### 5.2 决定性证据

启动日志：

```
U-Boot SPL 2017.09 ...
Trying to boot from MTD1
Trying fit image at 0x1800 sector
Trying kernel at 0x15800 sector from 'boot' part
Jumping to Kernel(0x01100000) via OP-TEE(0x00001000)   ← SPL 直接跳内核
```

**全程没有完整版 U-Boot 的第二次横幅**，也没有 `Handle standalone` 字样。

### 5.3 机制链（四步）

1. Rockchip 加载 AMP 核的代码在 `u-boot/drivers/cpu/rockchip_amp.c` 的 `amp_cpus_on()`：
   从 GPT `amp` 分区读 FIT → `fit_standalone_release()` → 通过 SIP
   `ROCKCHIP_SIP_CONFIG_MCU_CODE_START_ADDR` 把入口地址交给 TEE 并放行 M0。
2. 它在 `u-boot/arch/arm/mach-rockchip/board.c` 的 **`board_late_init()`** 里被调用，
   由 `#ifdef CONFIG_AMP` 包裹 —— **属于完整版 U-Boot 阶段**。
3. 而本板原配置用了 **thunderboot**：`RK_UBOOT_CFG_FRAGMENTS` 含 `rk3506_tb`，
   该 fragment 里有 **`CONFIG_SPL_KERNEL_BOOT=y`** → SPL 加载完 kernel 直接跳转，
   **完整版 U-Boot 整个被跳过** → `board_late_init` → `amp_cpus_on` 从未执行。
4. **注意**：`CONFIG_SPL_KERNEL_BOOT` 由 **U-Boot fragment** 决定，
   与 SDK defconfig 的 `RK_BOOT_COMPRESSED` **不是一回事**（两个都要改）。

### 5.4 为什么不用"方案 B"（保留 thunderboot + SPL 里放行）

SPL 的 FIT 加载器本身支持 standalone 子镜像（`common/spl/spl_fit.c:683-713`
加载后调 `spl_fit_standalone_release()`）。rv1106/rv1126 都实现了该函数
（直接写 CRU/GRF 寄存器复位并放行 MCU，不用 SIP，因为 SPL 是 ARM32 无 PSCI）。
**但 rk3506 没实现**，走 weak 空函数 `return 0`（只加载到内存，不放行）。

方案 B 要改 U-Boot 源码（违反"不动 U-Boot .c/.h"约束），且寄存器序列需手工外推，
风险高，未采用。

### 5.5 官方 AMP 镜像的佐证

`HD-RK3506G-AMP.img` 的 strings 里有：

```
U-Boot 2017.09-g1efd40dc40-250310 #will (Sep 15 2025 ...)   ← 完整 U-Boot 横幅
Handle standalone: '%s' at 0x%08x ...
AMP: Brought up primary cpu[%x, self] with state 0x%x, entry 0x%08x ...
rk3506-evb                            ← 完整 U-Boot 的 dtb（不是 rk3506-evb-tb）
[HAL INFO] Hello HD-RK3506G-EVM mcu
rpmsg-mcu0-test
```

证明官方 AMP 走的就是**完整 U-Boot 链**（dtb=`rk3506-evb`），与本方案一致。

### 5.6 修复（三处，缺一不可）

```bash
# ① defconfig：注释掉 RK_BOOT_COMPRESSED=y
# RK_BOOT_COMPRESSED=y
RK_BOOT_FIT_ITS_NAME="zboot.its"
RK_RECOVERY_FIT_ITS_NAME="zboot4recovery.its"

# ② defconfig：U-Boot fragment 去掉 rk3506_tb（它带 SPL_KERNEL_BOOT=y）
RK_UBOOT_CFG_FRAGMENTS="rk3506-amp-mcu"

# ③ 清理旧 u-boot 产物后重编（配置变了必须强制重建）
rm -f u-boot/.config u-boot/uboot.img
./build.sh uboot
```

### 5.7 烧录前自检（三条）

```bash
grep SPL_KERNEL_BOOT u-boot/.config
# 必须：# CONFIG_SPL_KERNEL_BOOT is not set

grep CONFIG_DEFAULT_DEVICE_TREE u-boot/.config
# 必须：CONFIG_DEFAULT_DEVICE_TREE="rk3506-evb"

strings u-boot/u-boot | grep -E "Handle standalone|AMP Error: Not fit"
# 必须能搜到（证明 amp_cpus_on 真的编进了完整 U-Boot）
```

### 5.8 烧录后正确现象

SPL 之后会出现**完整版 U-Boot 横幅**，并有：

```
Handle standalone: 'mcu' at 0x03a00000 ...
AMP: Brought up primary cpu[...] with state 0x..., entry 0x03a00141 ...
```

---

## 6. U-Boot fragment 的 `CONFIG_BASE_DEFCONFIG` 陷阱

`u-boot/make.sh`（约 241-256 行）对 `.config` 形态参数的处理：

- 若 fragment 文件里**含 `CONFIG_BASE_DEFCONFIG=` 行**：
  执行 `make <base>_defconfig <frag>.config` → **重建 .config**，
  把之前 merge 进去的其他 fragment 全部冲掉。
- 若**不含**：走 `make <frag>.config`（merge 到现有 .config 上）—— merge-only 形态。

官方 `rk3506-amp.config` **含**该行。若直接写
`RK_UBOOT_CFG_FRAGMENTS="rk3506_tb rk3506-amp"`，rk3506_tb 会被静默冲掉。

官方多 fragment 先例（`rk3506_g_evm_sd_defconfig` 的 `"rk3506_tb rk-emmc"`）里，
`rk-emmc.config` **没有**该行。

**本方案做法**：新建 `rk3506-amp-mcu.config`，只写两行 `CONFIG_AMP=y`/`CONFIG_ROCKCHIP_AMP=y`，
不带 BASE_DEFCONFIG。（最终方案里 rk3506_tb 整个不用了，见 §5，
但该陷阱对其他 fragment 组合依然成立。）

> 无害告警：make.sh 处理 merge-only fragment 时会打
> `sed: read error on configs/: Is a directory`（`BASE1_DEFCONFIG` 为空导致），
> 不影响编译。

---

## 7. 构建与打包流程

### 7.1 完整构建

```bash
cd /home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm

# WSL 必须：清洗含空格的 PATH（§4.3）
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -vE '[ "'"'"']' | grep -v '^$' | paste -sd:)

./build.sh vanxoak_hd_rk3506g_evm_nand_amp_mcu_defconfig   # lunch
./build.sh                                                 # 全量编译 + 打包
```

### 7.2 改配置后的增量重建（重要）

改了 u-boot fragment / DTS / HAL 时，**必须清对应产物**，否则不生效：

```bash
# 改 U-Boot 配置后
rm -f u-boot/.config u-boot/uboot.img && ./build.sh uboot

# 改 M0 固件后
./build.sh amp

# 改内核 DTS 后
./build.sh kernel

# 最后必须重新打包
./build.sh firmware
```

### 7.3 产物验证清单

| 检查项 | 命令 | 期望 |
|---|---|---|
| M0 入口 | `arm-none-eabi-readelf -h hal/project/rk3506-mcu/GCC/mcu.elf \| grep Entry` | `0x3a00141` |
| FIT 加载地址 | `file output/firmware/amp.img` | `Load Address: 0x03a00000`、`standalone` |
| amp 分区打进包 | 打包日志 grep `amp,Add file` | `flash_address=0x00072800`，与 parameter 一致 |
| rootfs 存在 | `grep ^RK_ROOTFS=y output/.config` | `RK_ROOTFS=y` |
| U-Boot 直启关闭 | `grep SPL_KERNEL_BOOT u-boot/.config` | `# CONFIG_SPL_KERNEL_BOOT is not set` |
| 固件最终产物 | `ls -laL rockdev/update.img` | 存在，含完整 rootfs（约 125MB+） |

---

## 8. 板上验证

### 8.1 分步验证顺序（先易后难，别一上来追 rpmsg）

**第 1 步：U-Boot 有没有放行 M0**

接 A 核调试串口（UART0，板载那个），上电看日志：

```
U-Boot SPL ...                     ← SPL 横幅
U-Boot 2017.09 ... (完整版)         ← ★ 必须有第二次横幅
Handle standalone: 'mcu' at 0x03a00000 ...   ← ★ 放行成功
```

两条都在 = M0 已被放行。**缺任一条 → 回到 §5**。

**第 2 步：Linux 是不是三核**

```bash
nproc                                # 期望 3
cat /proc/cpuinfo | grep processor   # 期望 processor 0 / 1 / 2
dmesg | grep "smp: Brought up"       # 期望 ... 3 CPUs
```

**第 3 步：内存预留对不对**

```bash
cat /proc/iomem
# 期望 System RAM 分成两段：
#   00000000-039fffff : System RAM
#   03e00000-07ffffff : System RAM
# 即 0x03a00000~0x03dfffff 被摘出给 M0
```

> `grep 3a000 /proc/iomem` 为空是**正常的** —— `no-map` 保留区不会显示成
> 独立条目。判断依据是 System RAM 在 0x039fffff 断开、0x03e00000 续上。

**第 4 步：M0 串口有没有输出**

接 **UART1**（GPIO0_C0=RX / GPIO0_C1=TX）115200 8N1，上电应在**内核日志之前**
（M0 由 U-Boot 放行，比 Linux 早）看到：

```
[HAL INFO] Hello RK3506 mcu
```

**第 5 步：rpmsg 通没通**

```bash
dmesg | grep -i rpmsg
# 期望看到：
#   rockchip-rpmsg 3c00000.rpmsg: rockchip rpmsg platform probe.
#   rockchip-rpmsg 3c00000.rpmsg: rpdev vdev0: vring0 0x3c00000, vring1 0x3c08000
#   virtio_rpmsg_bus virtio0: rpmsg host is online
#   rockchip_rpmsg_test virtio0.rpmsg-mcu0-test.-1.16387:
#       rx msg Rockchip rpmsg linux test! rx_count N(remote_ept_id: 0x4003)
```

`remote_ept_id: 0x4003` + `rx_count` 递增 = **A7↔M0 双向 pingpong 正常**。

### 8.2 通过后的样子（实测）

```
root@rk3506-buildroot:/# nproc
3
root@rk3506-buildroot:/# cat /proc/cpuinfo | grep processor
processor       : 0
processor       : 1
processor       : 2
```

内核日志持续刷：
```
[   4.249809] rockchip_rpmsg_test virtio0.rpmsg-mcu0-test.-1.16387: rx msg Rockchip rpmsg linux test! rx_count 5(remote_ept_id: 0x4003)
```

### 8.3 刷屏说明（不是故障）

`CONFIG_RPMSG_ROCKCHIP_TEST=y` 的驱动对每条消息 `dev_info` 打印，
直到 `MSG_LIMIT 10000` 才停（会打 `Rockchip rpmsg test exit!`）。
这是**验证成功的标志**，不是 bug。

想不刷屏：保留 `CONFIG_RPMSG_CHAR=y`（`/dev/rpmsg0` 仍可
`echo hello > /dev/rpmsg0` 手动测），把 `CONFIG_RPMSG_ROCKCHIP_TEST` 关掉即可。

---

## 9. 排障检查表（按顺序查，别跳步）

| 顺序 | 症状 | 先查什么 | 结论 |
|---|---|---|---|
| 1 | M0 毫无动静（串口哑 + rpmsg 无 new channel） | 启动日志有没有完整 U-Boot 横幅 + `Handle standalone` | 没有 → §5 启动链问题，**别查固件** |
| 2 | `./build.sh amp` 报 `scons` 找不到 | defconfig 的 `RK_AMP_RTT_TARGET` 等三个是否置空 | §4.2 |
| 3 | 打包 `afptool` 报 `err=2` | `RK_ROOTFS/RECOVERY/MISC` 是否显式 `=y` | §4.1 |
| 4 | buildroot 报 PATH 含空格 | WSL PATH 清洗 | §4.3 |
| 5 | 串口无输出（但 M0 已被放行） | ① 波特率（115200）② UART 号（UART1 调试口 vs UART2 数据口）③ `pUart`/`g_uartXDev`/RMIO 三者是否同一 UART ④ 接线 TX↔RX 交叉 | §3.8 |
| 6 | rpmsg probe 成功但无 `new channel` | M0 没跑到端点宣告 → 回第 1 步 | §5 |
| 7 | 内存预留看起来"没生效" | 看 System RAM 断口，不是 grep 0x03a00000 | §8.1 第 3 步 |

### 关键判读技巧

- **`/proc/iomem` 里 `grep 3a000` 空 ≠ 预留失败**：`no-map` 区不显示为条目，
  要看 System RAM 是否在 0x039fffff 断开。
- **`Rockchip rpmsg test!` 刷屏 = 成功**，不是错误。
- **`[1.02] rockchip_amp_get_gic_info: get amp gic cpu mask error`**：
  无 `amp-cpu-aff-maskbits`（mcu-only 方案不需要）的预期告警，可忽略。

---

## 10. 给 OpenVela 适配的关键结论

| 本方案（Linux SDK） | OpenVela（比赛仓 v9 架构） | 注意 |
|---|---|---|
| 完整 U-Boot `amp_cpus_on()` 从 `amp` 分区读 FIT 放行 M0 | v9 同样是"U-Boot 放行 + NuttX 只跑协议" | **前提一致**：启动链必须真的走到完整 U-Boot |
| `amp_mcu_evm.its` load=**0x03a00000**（DRAM 1MB） | `openvela/nand_firmware/amp/amp.its` load=**0xfff84000**（SRAM） | SRAM 仅 32KB，跑得动 rpmsg 例程但加应用会栈溢出 |
| parameter 加 `amp` 分区（0x72800, 4MB） | `nand_firmware/parameter.txt` 加 amp（0x10800, 1MB） | 分区大小按固件实际体积给余量 |
| package-file 加 `amp amp.img` 一行 | `pack_nand.sh` 打包 amp.img | — |
| 内核 `rockchip_rpmsg` + `rockchip_rpmsg_test` | NuttX rptun + `hd_rk3506_rpmsgtest.c` | 协议侧等价 |
| M0 串口 UART1（调试口） | v9 用 UART2（J9） | 官方教程里 UART1 是调试口、UART2 是数据测试口，两者角色别混 |
| `REMOTE_ID=3` → link-id 0x03 + mailbox0/3 | 同 | v9 已有此配置 |

**最有价值的一条**：OpenVela v9 当前的现象
（M0 收 kick 但不回 B2A / 串口无输出）**首先要用 §8.1 第 1 步排除放行链路** ——
确认启动日志里有完整 U-Boot 横幅 + `Handle standalone`。
若这条不成立，后面所有 rpmsg/mailbox 排查都是白费。

---

## 附：本次实测记录（可作为基线对照）

| 项 | 实测值 |
|---|---|
| `mcu.elf` Entry | `0x3a00141` |
| 向量表前 16 字节 | `00fc af03 4101 a003 8501 a003 8501 a003`（SP=0x03affc00，Reset=0x03a00141） |
| `__StackTop` | `0x03affc00` |
| `__linux_share_rpmsg_start__` | `0x03c00000`，end `0x03e00000` |
| `amp.img` | 32256 B，FIT Load Address 0x03a00000，Standalone，ARM，loadables=mcu |
| amp 分区 flash_address | `0x00072800`（与 parameter 完全一致） |
| `update.img` | 125,547,082 B（含完整 rootfs.ubi 95MB） |
| 板上 `nproc` | 3 |
| 板上 System RAM | `00000000-039fffff` + `03e00000-07ffffff` |
| rpmsg 端点 | `virtio0.rpmsg-mcu0-test`，`remote_ept_id 0x4003`，rx_count 持续递增 |
