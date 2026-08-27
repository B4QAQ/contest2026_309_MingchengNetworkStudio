# HD-RK3506-EVM U-Boot 启动操作指南（最终版）

> **关键发现**：`mtd list` 只显示一个 128MB 的 spi-nand0 设备，**没有看到分区**。
> 这是因为 U-Boot 通过 GPT 找分区，但需要主动查询。
> 或者直接用 mtd read 读指定偏移。

---

## 1. 直接读 boot 分区

`boot` 分区在 NAND 上的位置（按 `parameter.txt`）：
- 起始 LBA: `0x15800`（每 LBA = 512 字节）
- 起始字节: `0x15800 * 0x200 = 0xAC0000`
- 大小（字节）: `0x5000 * 0x200 = 0x280000`（2.5MB）
- 大小（块）: 20 blocks（每块 128KB）

**block size 0x20000 = 128KB**，起始 `0xAC0000` 已经是 block 对齐的。

### 命令

```bash
=> mtd read spi-nand0 0x02080000 0xAC0000 0x280000
```

然后跳转：

```bash
=> go 0x02080560
```

---

## 2. 如果上面的命令不工作

### 2.1 试简化版

```bash
# 读最小 256KB（足够启动 NuttX）
=> mtd read spi-nand0 0x02080000 0xAC0000 0x40000
```

### 2.2 看 part 命令

```bash
=> part list spi-nand0
```

可能会列出 GPT 分区。

### 2.3 用 mtd_blk

```bash
# mtd_blk 把 MTD 设备映射成块设备
=> mtd_blk dev 0
=> part list mtd 0
```

### 2.4 完整读取整个 boot 区域（不到 1MB 但保险）

```bash
=> mtd read spi-nand0 0x02080000 0xAC0000 0x200000  # 读 2MB
=> md 0x02080000 10  # 验证内存内容
```

如果 `md` 显示前几行是 `e59ff018`（reset vector），说明加载成功。

---

## 3. 跳转到 NuttX

```bash
=> go 0x02080560
```

预期看到：
```
NuttX (with NuttX RTOS)
nsh>
```

如果看不到 `nsh>` 但不报 "undefined instruction"，可能 NSH 还没初始化好（等了 1-2 秒）。

---

## 4. 让 U-Boot 自动启动

成功后：

```bash
=> setenv bootcmd 'mtd read spi-nand0 0x02080000 0xAC0000 0x280000; go 0x02080560'
=> saveenv
```

下次上电会自动启动 NuttX。

---

## 5. 关键提示

- **`spi-nand0` 是 U-Boot 给这块 NAND 的设备名**
- `mtd read` 的参数是：`<设备名> <目标内存地址> <NAND偏移字节> <大小字节>`
- `go` 的参数是入口点地址 `0x02080560`（从 ELF 头读取）
- 必须先 `mtd read` 再 `go`，否则 `go` 跳到没初始化的内存会乱跑

---

## 6. 命令速查

```bash
=> mtd list                        # MTD 设备列表
=> mtd read spi-nand0 ADDR OFFSET SIZE  # 读 NAND
=> md ADDR LEN                     # 显示内存
=> go ADDR                         # 跳转
=> setenv bootcmd '...'            # 设置启动命令
=> saveenv                         # 保存环境
=> printenv                        # 看所有环境变量
```

---

## 7. 期望的命令序列

```bash
=> mtd read spi-nand0 0x02080000 0xAC0000 0x280000
Reading 2621440 bytes from spi-nand0 at 0xAC0000...
... (read OK)

=> go 0x02080560
## Starting application at 0x02080560 ...
NuttX ...
nsh>
```

把每一步的输出贴给我。