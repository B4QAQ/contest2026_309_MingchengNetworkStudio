# HD-RK3506-EVM U-Boot 启动操作指南（修正 v3）

> **重大发现！** 我之前算错了 boot 分区的字节偏移！
> - 错误：`0x15800 * 512 = 0xAC0000`（错）
> - 正确：`0x15800 * 512 = 0x2B00000`（对）
> 
> 因为 `0x15800 = 88064 decimal`，`88064 * 512 = 0x2B00000`。
> 所以您读的是空数据（0xff），没读到 NuttX kernel！

---

## 1. 立即尝试：正确的偏移

```bash
=> mtd read spi-nand0 0x02080000 0x2B00000 0x280000
=> go 0x02080560
```

---

## 2. 如果还是不行

### 2.1 看内存

```bash
=> mtd read spi-nand0 0x02080000 0x2B00000 0x280000
=> md 0x02080000 20
```

如果显示 `e59ff018 02080560 02080240 ...`（reset vector），说明加载成功。
如果显示全 `ff`，说明这个位置还是空数据。

### 2.2 试别的偏移

如果 `0x2B00000` 还是空，**boot.img 可能在别的位置**。但根据 parameter.txt 这应该是对的。

---

## 3. 串口自动进 NSH

要让 U-Boot 自动启动 NuttX，设置 bootcmd：

```bash
=> setenv bootcmd 'mtd read spi-nand0 0x02080000 0x2B00000 0x280000; go 0x02080560'
=> saveenv
```

下次上电就自动启动。**不需要每次手动输入。**

---

## 4. 烧录方式

**用 update.img 一把全烧**（推荐）：

```bash
sudo upgrade_tool uf nand_firmware/update.img
```

这会把所有分区（MiniLoader + U-Boot + boot.img + 各种占位）一起烧写。

或者单分区烧：

```bash
# 只烧 boot 分区（最常用，比如改了 kernel）
sudo upgrade_tool di boot nand_firmware/boot.img

# 烧 MiniLoader（如果 loader 损坏了才用）
sudo upgrade_tool ul nand_firmware/MiniLoaderAll.bin

# 烧 U-Boot
sudo upgrade_tool di uboot nand_firmware/uboot.img
```

`update.img` 是最安全的，因为会自动写 GPT + 所有分区。单分区烧只在你知道要更新什么的时候用。

---

## 5. 完整启动流程

1. 上电
2. BootROM → MiniLoader（280KB）
3. MiniLoader → U-Boot（813KB）
4. U-Boot 读 `mtd read spi-nand0 0x02080000 0x2B00000 0x280000`
5. U-Boot 跳转 `go 0x02080560`
6. NuttX 启动 → NSH 提示符

---

## 6. 期望的命令序列

```bash
=> setenv bootcmd 'mtd read spi-nand0 0x02080000 0x2B00000 0x280000; go 0x02080560'
=> saveenv
=> reset

# 之后每次上电:
# U-Boot bootcmd 自动执行
# 看到 NSH 提示符
```

**先把 `md 0x02080000 20` 的输出贴给我**，确认数据是否正确加载。