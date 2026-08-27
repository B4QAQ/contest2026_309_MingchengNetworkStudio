# HD-RK3506-EVM U-Boot 启动操作指南（修正版 v2）

> **找到了！** U-Boot 有 `mtd` 命令（不是 `mtdparts`，不是 `rksfc`）。
> 您的 U-Boot 实际支持的命令：`mmc`, `mtd`, `mtd_blk`, `ext4load`, `ext2load`, `load`, `part` 等。

---

## 1. 重新进入 U-Boot 提示符

按 `Ctrl+C` 或在 3 秒倒计时内按任意键。

---

## 2. 列出 MTD 设备

```bash
=> mtd list
```

预期输出类似：
```
List of MTD devices:
spi-nand0  - 128 MiB
  - 0x000000000000-0x000000100000 : "vnvm"
  - 0x000000100000-0x000000500000 : "uboot"
  - 0x000000500000-0x000000700000 : "misc"
  - 0x000000700000-0x000001900000 : "recovery"
  - 0x000001900000-0x000001c00000 : "boot"        <-- NuttX kernel 在这里
  - 0x000001c00000-0x000006000000 : "rootfs"
  - 0x000006000000-0x000006200000 : "oem"
  - 0x000006200000-0x00001ec00000 : "userdata"
```

把输出贴给我。

---

## 3. 加载 boot 分区

如果 `mtd list` 显示有 `boot` 分区，直接读：

```bash
=> mtd read boot 0x02080000
# 或者
=> mtd read boot 0x02080000 0 0x400000
```

---

## 4. 跳转到 NuttX

```bash
=> go 0x02080560
```

应该看到：
```
NuttX (with NuttX RTOS)
nsh>
```

---

## 5. 如果 mtd list 显示 raw NAND 而不是 spi-nand

有些板子用 `rknand0` 或 `mtd0` 这样的设备名。用 `mtd list` 看。

---

## 6. 设置自动启动

成功后可以保存 bootcmd：

```bash
=> setenv bootcmd 'mtd read boot 0x02080000; go 0x02080560'
=> saveenv
```

---

## 7. 快速参考

| 命令 | 用途 |
|------|------|
| `mtd list` | 列出所有 MTD 设备和分区 |
| `mtd read <name> <addr>` | 读分区到内存 |
| `mtd read <name> <addr> <off> <size>` | 读指定范围 |
| `go <addr>` | 跳转到地址执行 |
| `md <addr> <len>` | 显示内存内容（验证加载）|
| `part list <dev>` | 列出块设备分区 |
| `setenv bootcmd '...'` | 设置启动命令 |
| `saveenv` | 保存环境到 flash |

---

## 8. 期望的命令序列

```bash
=> mtd list
# (看到 boot 分区)
=> mtd read boot 0x02080000
# 输出类似: "Read 4194304 bytes from 'boot' to 0x02080000"
=> go 0x02080560
# 看到 NSH 提示符
```

请把 `mtd list` 的输出贴给我！