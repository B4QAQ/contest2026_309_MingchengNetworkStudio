# HD-RK3506-EVM U-Boot 启动操作指南（修正版）

> **重要发现**：板子用的是 **SPI NAND**（不是 eMMC/SD），所以 `mmc 0:5` 会失败。
> 您的 U-Boot log 也确认了：`boot mode: None`（mmc 检测失败后没找到其他 boot device）。
> U-Boot 字符串里 `rksfc`、`spinand`、`mtd` 都有，但默认 `bootcmd` 只试 `boot_fit;boot_android ${devtype} ${devnum}`。
> 我们需要手动操作。

---

## 1. 重新进入 U-Boot 提示符

按 `Ctrl+C` 或在 3 秒倒计时内按任意键。

---

## 2. 探索存储设备

在 U-Boot 提示符下输入：

```bash
=> help
# 看一下有没有 rksfc, mtd, nand 相关命令
```

### 2.1 试 SPI NAND

```bash
# 初始化 SPI NAND
=> rksfc dev 0
# 扫描
=> rksfc scan
# 列出找到的设备
```

### 2.2 试 mtdparts

```bash
=> mtdparts
# 应该会显示 NAND 分区表 (来自 SPL 传给 U-Boot)
# 类似:
# device 0: 10000000.nand (NOR/NAND), 128MB
# - 0x00000000-0x00100000 : "vnvm"
# - 0x00100000-0x00500000 : "uboot"
# - ...
# - 0x01580000-0x01a80000 : "boot"      <-- 我们的 kernel 在这里
# - ...
```

### 2.3 试 nand 命令

```bash
=> nand info
# 显示 NAND 设备信息
=> nand read 0x02080000 boot 0x50000
# 从 boot 分区读 0x50000 字节到内存 0x02080000
```

---

## 3. 加载 boot.img 到内存 0x02080000

### 方案 A：用 mtdparts (推荐)

```bash
# 列出 mtd 分区
=> mtdparts
# 用 mtd 读 boot 分区
=> mtd read 0x02080000 boot
# 或指定大小
=> mtd read 0x02080000 boot 0x400000
```

### 方案 B：用 nand 命令

```bash
# 直接读 NAND 的 boot 分区
=> nand read 0x02080000 boot
# 或指定起始地址
=> nand read 0x02080000 0x1580000 0x50000
# (0x1580000 = boot 分区起始, 0x50000 = 大小)
```

### 方案 C：用 rksfc (如果 SPI NOR)

```bash
=> rksfc dev 0
=> rksfc read 0x02080000 boot
```

---

## 4. 跳转到 NuttX

加载完成后，验证内存内容并跳转：

```bash
# 查看内存前几行（应该是 reset vector 0xe59ff018）
=> md 0x02080000 10

# 跳转到入口点 (从 nuttx.elf 的 Entry point 读出来 = 0x02080560)
=> go 0x02080560
```

---

## 5. 如果上面的命令不工作，试试这些

### 5.1 初始化 SPI NAND

```bash
# 看看 rksfc 命令
=> rksfc

# 试 sf (SPI Flash)
=> sf probe 0

# 试 mtd
=> mtd list
```

### 5.2 看 U-Boot 帮助

```bash
=> ?
# 列出所有命令
# 找 storage / load 相关
```

### 5.3 设置 devtype 然后重新试

```bash
# 强制设置 devtype 为 mtd
=> setenv devtype mtd
=> setenv devnum 0
=> saveenv
=> reset
```

---

## 6. 让 U-Boot 自动启动 NuttX

一旦找到正确的加载命令，可以设置为 bootcmd：

```bash
=> setenv bootcmd 'mtd read 0x02080000 boot 0x400000; go 0x02080560'
=> saveenv
# 下次上电会自动执行
```

---

## 7. 完全诊断流程

如果完全卡住，按这个流程来：

```bash
# 1. 看 U-Boot 版本和功能
=> version
=> help

# 2. 看环境变量
=> env print
# 重点看:
#   - bootcmd
#   - devtype, devnum
#   - mtdparts
#   - partitions

# 3. 试所有存储设备
=> mmc list
=> rksfc dev 0 && rksfc info
=> nand info
=> sf probe 0

# 4. 找到设备后列出分区
=> mtdparts
=> part list mtd 0
=> part list spinand 0

# 5. 读 kernel
# (用上面找到的命令)
```

---

## 8. 关键问题列表

### Q: 为什么 mmc 0 失败？
A: 板子用的是 SPI NAND，不是 eMMC/SD。`mmc 0` 是 SD 卡接口，板子没插卡。

### Q: 那 boot.img 怎么读？
A: 用 `mtd read` 或 `nand read` 从 SPI NAND 读。NAND 已经在烧录时写入了。

### Q: 怎么知道 boot 分区的偏移？
A: 看 `parameter.txt`：
- `uboot` 分区：起始 0x1800 块（1 块 = 512 字节）= 0xC0000
- `boot` 分区：起始 0x15800 块 = 0xAC0000，大小 0x5000 块 = 0x280000

但这个偏移是按 512 字节扇区算的，不是字节地址。

### Q: 怎么用地址读 NAND？
A: 优先用分区名（`boot`），不要用绝对地址。`mtd read 0x02080000 boot` 这样。

---

## 9. 期望的最终操作

```bash
=> mtdparts
# 应该看到 boot 分区
=> mtd read 0x02080000 boot
# 读取 boot 分区
=> go 0x02080560
# 跳转到 NuttX 入口
# 应该看到 NSH 提示符
```

如果 `mtd read` 不工作，告诉我 `mtdparts` 的输出，我帮您调整。

---

## 10. 附：parameter.txt 中的分区偏移

```
vnvm:      0x00000800 - 0x000017FF  (1MB)
uboot:     0x00001800 - 0x000057FF  (8MB)
misc:      0x00005800 - 0x000067FF  (2MB)
recovery:  0x00006800 - 0x000157FF  (30MB)
boot:      0x00015800 - 0x0001A7FF  (10MB)  <-- NuttX kernel here
rootfs:    0x0001A800 - 0x0006A7FF  (80MB)
oem:       0x0006A800 - 0x000727FF  (2MB)
userdata:  0x00072800 - END          (remaining)
```

注意：这是 LBA 地址 (扇区号，每个扇区 512 字节)：
- boot 起始 LBA: 0x15800
- boot 起始字节: 0x15800 * 512 = 0xAC0000
- boot 大小: 0x5000 块 = 0x280000 字节 = 2.5MB
