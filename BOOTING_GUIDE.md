# HD-RK3506-EVM 自动进入 NSH 完整指南

> **新方法：用 bootm 代替 go 启动 NuttX**
> 原因：U-Boot 的 `go` 命令强制 Thumb 模式（`entry | 1`），但 NuttX 入口是 ARM 代码，所以会 "undefined instruction"。
> 解决方法：用 uImage + `bootm` 启动。

---

## 1. 烧新镜像

新的 `update.img` 已经把 `boot.uimg`（uImage 格式）烧到 boot 分区了。

```bash
sudo upgrade_tool uf /home/b4qaq/project/openvela/nand_firmware/update.img
```

---

## 2. 进 U-Boot 提示符

按 `Ctrl+C` 或在 3 秒倒计时内按任意键

---

## 3. 设置自动启动

### 3.1 立即启动 NSH（先验证）

```bash
# 1. 读 uImage 到内存
=> mtd read spi-nand0 0x02080000 0x2B00000 0x280000

# 2. 用 bootm 启动 uImage
=> bootm 0x02080000
```

如果成功，应该看到：
```
## Booting kernel from Legacy Image at 02080000 ...
   Image Name:   openvela-rk3506
   ...
   Loading Kernel Image ... OK
OK

NuttX (with NuttX RTOS)
nsh>
```

### 3.2 设置自动启动 bootcmd

成功后执行：

```bash
=> setenv bootcmd 'mtd read spi-nand0 0x02080000 0x2B00000 0x280000; bootm 0x02080000'
=> saveenv
=> reset
```

---

## 4. 之后每次上电自动进 NSH

```bash
上电 -> U-Boot -> 自动执行 bootcmd -> bootm -> NuttX -> nsh>
```

---

## 5. 故障排查

### 5.1 bootm 失败

可能原因：
- uImage 损坏：重新烧 `boot` 分区
  ```bash
  sudo upgrade_tool di boot /home/b4qaq/project/openvela/nand_firmware/boot.img
  ```
- 内存地址不对：检查 `md 0x02080000 20`

### 5.2 串口没输出

- 检查接线（TX/RX/GND）
- 确认是 UART0（debug port）

### 5.3 reset 后还是停在 U-Boot

检查 bootcmd 是否正确：
```bash
=> printenv bootcmd
```

---

## 6. 关键命令速查

| 命令 | 用途 |
|------|------|
| `mtd read spi-nand0 ADDR OFFSET SIZE` | 从 NAND 读数据 |
| `bootm ADDR` | 启动 uImage |
| `setenv bootcmd '...'` | 设置启动命令 |
| `saveenv` | 保存到 NAND |
| `printenv` | 查所有环境变量 |
| `reset` | 重启板子 |

---

## 7. 烧录方式（再强调一遍）

**用 `update.img` 一把全烧**（最简单）：

```bash
sudo upgrade_tool uf /home/b4qaq/project/openvela/nand_firmware/update.img
```

**单分区烧**（调试时用）：

```bash
# 只烧 boot 分区（修改 kernel 后）
sudo upgrade_tool di boot /home/b4qaq/project/openvela/nand_firmware/boot.img
# 上面这个 boot.img 现在是 uImage 格式
```

---

## 8. 完整启动流程

1. **Loader 阶段**：BootROM → MiniLoader (rk3506_spl_loader_v1.06.111.bin, 273KB)
2. **U-Boot 阶段**：MiniLoader → U-Boot FIT (uboot.itb, 813KB)
3. **Kernel 阶段**：U-Boot `bootm` → NuttX (boot.uimg/uImage, ~500KB)
4. **Shell 阶段**：NuttX → NSH 提示符

---

## 9. 期望结果

```
# reset 后自动执行:
=> mtd read spi-nand0 0x02080000 0x2B00000 0x280000
Reading 2621440 byte(s) (1280 page(s)) at offset 0x02b00000
=> bootm 0x02080000
## Booting kernel from Legacy Image at 02080000 ...
   Image Name:   openvela-rk3506
   Image Type:   ARM Linux Kernel Image (uncompressed)
   Data Size:    510504 Bytes = 498.54 KiB = 0.49 MiB
   Load Address: 02080000
   Entry Point:  02080560
   Loading Kernel Image ... OK
OK
Starting kernel ...

NuttX (with NuttX RTOS)
nsh>
```

**请按这个步骤操作。烧新镜像后立即输入 `setenv bootcmd '...'` 让它自动启动！**
