# HD-RK3506-EVM 启动流程与进入 NSH 指南

> **当前状态**：固件已烧录成功，MiniLoader + U-Boot 已运行，但 NuttX 还没自动启动。
> **原因**：U-Boot 的 `boot_fit` 和 `boot_android` 命令都需要特定格式（FIT 或 Android boot.img），而我们的 `boot.img` 是原始 nuttx.bin。
> **解决**：在 U-Boot 提示符下手工执行加载和启动命令。

---

## 1. 串口连接

| 接线 | 说明 |
|------|------|
| USB-TTL GND | → HD-RK3506-EVM GND |
| USB-TTL TX  | → HD-RK3506-EVM DEBUG RX |
| USB-TTL RX  | → HD-RK3506-EVM DEBUG TX |
| 串口参数 | 115200 8N1，无流控 |

工具：minicom / picocom / PuTTY / MobaXterm

```bash
# Linux 推荐用 picocom
sudo picocom -b 115200 /dev/ttyUSB0

# 或 minicom
sudo minicom -D /dev/ttyUSB0 -b 115200
```

---

## 2. 上电后看到的现象

上电后，串口会依次输出：

```
# 阶段 1: BootROM (无输出)
# 阶段 2: MiniLoader (SPL)
DDR Version V1.06 ...
DDR4
... (DDR 初始化)

# 阶段 3: U-Boot
U-Boot 2018.09 ... (Build time...)
Hit any key to stop autoboot: 2   <-- 2 秒内按任意键

=>
```

**3 秒后**，U-Boot 会尝试自动启动：
```
## Error: FIT image not found or invalid    <-- boot_fit 失败 (我们的 boot.img 不是 FIT)
## Error: android boot failed                <-- boot_android 失败 (不是 Android 格式)
=>
```

进入 U-Boot 命令提示符 `=>`。

---

## 3. 在 U-Boot 提示符下手动启动 NuttX

### 3.1 加载 boot.img 到内存

我们的 NuttX 内核起始地址是 `0x02080000`（在 defconfig 中 `CONFIG_RAM_START=0x02080000`）。
`boot.img` 是原始的 nuttx.bin，4KB 对齐。

```bash
# 查看 boot 分区在哪
=> mmc list
# 输出类似: mmc@fe330000: 0 (eMMC)  或  mmc@fe310000: 0 (SD)

# 查看 boot 分区布局
=> part list mmc 0
# 输出类似:
# Partition Map for MMC device 0  --   Partition Type: EFI
# Part    Start LBA     End LBA        Name
#   1     0x00000800    0x000017ff    "vnvm"
#   2     0x00001800    0x000057ff    "uboot"
#   3     0x00005800    0x000067ff    "misc"
#   4     0x00006800    0x000157ff    "recovery"
#   5     0x00015800    0x0001a7ff    "boot"     <-- 我们的 kernel 在这里
#   6     0x0001a800    0x0006a7ff    "rootfs"
#   7     0x0006a800    0x000727ff    "oem"
#   8     0x00072800    ...            "userdata"

# 用 ext4load (假设 boot 分区是 ext4 格式) 或 fatload 加载 boot.img
=> ext4load mmc 0:5 0x02080000 /boot.img
# 或
=> load mmc 0:5 0x02080000 boot.img
```

### 3.2 启动 NuttX

加载完后用 `go` 命令跳到入口点（`0x02080560`，从 ELF Entry point 读取）：

```bash
# go 命令 - 直接跳转到地址，不解析任何格式
=> go 0x02080560
```

或用 `bootm` 启动 uImage（如果有）：

```bash
# 如果用 mkimage 包装过的 boot.uimg
=> ext4load mmc 0:5 0x02080000 /boot.uimg
=> bootm 0x02080000
```

---

## 4. 看到 NSH 提示符

启动成功后，串口会输出：

```
NuttX (with NuttX RTOS)
nsh> help
nsh>
```

常见的 nsh 命令：
```bash
nsh> help         # 帮助
nsh> uname -a     # 系统信息
nsh> free         # 内存
nsh> ps           # 进程
nsh> mount        # 文件系统
nsh> ls /dev/     # 设备列表
nsh> ls /         # 根目录
```

---

## 5. 让 U-Boot 记住手动启动命令

如果每次都要手动加载太麻烦，可以设置 bootcmd：

```bash
# 设置自动 bootcmd
=> setenv bootcmd 'ext4load mmc 0:5 0x02080000 /boot.img; go 0x02080560'
=> saveenv
# 之后每次上电会自动执行
```

如果板子有 `bootcmd` 默认行为，也可以用 `env` 命令查看：
```bash
=> env print
```

---

## 6. 常见问题

### 6.1 U-Boot 看不到任何东西

- 串口线接错（TX/RX 反了）
- 波特率不对
- USB-TTL 没共地

### 6.2 U-Boot 报 "boot_fit" 错误

正常。我们的 `boot.img` 是 raw bin，不是 FIT 格式。
按任意键停在 U-Boot 提示符，手动加载即可。

### 6.3 U-Boot 报 "android boot failed"

正常。我们的 `boot.img` 没有 Android 2KB header。

### 6.4 加载后启动黑屏

- 检查 NSH 串口是否有输出（连接正确）
- 检查 `rk3506_lowputc.c` 时钟是否 24 MHz
- 检查 defconfig 中 `CONFIG_RAM_START=0x02080000` 和 `CONFIG_INIT_ENTRYPOINT="nsh_main"`

### 6.5 想看到内核启动 log

```bash
# 在 defconfig 中启用
CONFIG_DEBUG_FULLOPT=y
CONFIG_DEBUG_ERROR=y
CONFIG_DEBUG_WARN=y
CONFIG_DEBUG_INFO=y
# 然后重新编译
```

### 6.6 boot.img 加载后挂死

- 检查 `0x02080560` 是不是正确的入口（看 `arm-none-eabi-readelf -h nuttx`）
- 检查内存是否够用（我们的 defconfig 用 128MB）
- 用示波器/逻辑分析仪看 UART0 TX pin（PA0）有无波形

---

## 7. 进一步修改 U-Boot bootcmd

要让 U-Boot 自动启动 NuttX，最简单的办法是改 U-Boot 配置。

`/home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/u-boot/include/configs/evb_rk3506.h`:

```c
#undef CONFIG_BOOTCOMMAND
#define CONFIG_BOOTCOMMAND \
    "load mmc 0:5 0x02080000 boot.img;" \
    "go 0x02080560"
```

然后重新编译 U-Boot：
```bash
cd /home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/u-boot
make rk3506_defconfig
make -j$(nproc)
# 生成的 u-boot.itb 复制到 nand_firmware/uboot.img
```

---

## 8. 不进入 NSH 时的快速诊断

1. **U-Boot 是否正常？** - 看到 `=>` 提示符就说明 OK
2. **boot.img 能否加载？** - 用 `ext4load` 或 `load` 命令，看是否有错误
3. **内存中是否正确？** - 用 `md 0x02080000 10` 看内存
4. **入口地址对吗？** - 用 `arm-none-eabi-readelf -h nuttx.elf` 查看 Entry
5. **go 命令是否成功？** - 应该立即看到 NuttX 启动 log

如果 `go 0x02080560` 之后无任何输出，可能原因：
- 入口地址错误（应该用 ELF 入口 = 0x02080560）
- 内存访问失败
- 内核二进制损坏（重新烧 boot.img）

---

## 9. 当前可用的镜像

```bash
ls -la /home/b4qaq/project/openvela/nand_firmware/
# MiniLoaderAll.bin  (280KB) - SPL
# uboot.img          (813KB) - U-Boot FIT
# boot.img           (4MB)   - NuttX 内核 raw bin
# boot.uimg          (4MB)   - NuttX 内核 uImage
# update.img         (5.6MB) - 完整烧录包
```

如果 boot.img 不工作，可以尝试 boot.uimg：
```bash
=> load mmc 0:5 0x02080000 boot.uimg
=> bootm 0x02080000
```
