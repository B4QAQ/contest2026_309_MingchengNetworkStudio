# HD-RK3506-EVM 硬件验证测试计划

> **目的**：当拿到 HD-RK3506-EVM 开发板后，按本计划逐项验证 BSP 的功能。
> **前提**：已完成编译，生成 `update.img` (7.4MB)，使用 `upgrade_tool` 烧录。

---

## 1. 烧录流程

### 1.1 准备

1. 一台 Linux x86_64 主机
2. USB-C 数据线（接开发板 J2 USB Device）
3. USB-TTL 串口线（接 DEBUG 串口，115200 8N1）
4. `upgrade_tool`（从 `RK3506G2/.../tools/linux/Linux_Upgrade_Tool/upgrade_tool` 获取）
5. 文件：`nand_firmware/update.img` (7.4MB)

### 1.2 烧录步骤

```bash
# 1. 短接 RECOVERY 按键（丝印 RECOVERY 在板子丝印层有标注）
# 2. 插入 USB-C 到 PC
# 3. 短接松开后，PC 应检测到 Loader 设备（lsusb 看到 "USB Device 0x2207:0x3506"）
# 4. 全量烧录：
sudo upgrade_tool uf nand_firmware/update.img
# 5. 烧录完成后，拔掉 USB-C 重新上电（或短接 RESET）
```

预期耗时：30-60 秒（128MB NAND）。

### 1.3 验证烧录结果

```bash
# 1. 重启开发板
# 2. 串口应该看到类似输出：

# 阶段 1: BootROM
# (无输出，BootROM 直接跳到 MiniLoader)

# 阶段 2: MiniLoader (SPL)
DDR Version V1.06 ...
ChipType: RK3506
...

# 阶段 3: U-Boot
U-Boot SPL ...
U-Boot 2018.09 (Build time...)
Hit any key to stop autoboot: 3
=>
```

如果只看到 U-Boot 但没看到 NuttX 启动：
- 说明 U-Boot 还没加载 kernel
- 在 U-Boot 提示符下手动执行：
  ```
  => ext4load mmc 0:5 0x02080000 boot.uimg
  => bootm 0x02080000
  ```
  (注意：实际 mmc 设备号和分区号取决于 U-Boot env)

---

## 2. 基础功能验证（NSH 启动后）

### 2.1 串口和 NSH

```bash
# 串口应能输入并回显
nsh> help
# 预期输出 NSH 命令列表（至少 20 条命令）

nsh> uname -a
# 预期: NuttX X.Y.Z ... arm ... HD-RK3506-EVM

nsh> free
# 预期: 显示总内存约 128MB
#        total       used       free
#        Mem:      131072       4096      126976
```

### 2.2 文件系统

```bash
nsh> ls /dev/
# 预期看到 /dev/console, /dev/null, /dev/zero, /dev/ttyS0, /dev/fb0, /dev/input0

nsh> mount
# 预期看到 /etc 挂载点 (romfs)
```

### 2.3 进程

```bash
nsh> ps
# 预期: 看到 nsh_main 进程 (PID 1)
```

---

## 3. 设备驱动验证

### 3.1 UART0 (115200 8N1)

**测试命令**：
```bash
nsh> stty -a /dev/ttyS0
# 预期: 显示当前 UART 配置

# 短接 RX-TX 验证回环
nsh> cat /dev/ttyS0 &
# 然后在另一个终端 (或通过 USB-TTL 连接到 PC 串口) 发送字符
# 应该看到字符被回显到 NSH
```

**通过标准**：
- 字符不丢
- 波特率正确
- 没有"乱码"（验证 UART 时钟是 24 MHz 而非 1.8432 MHz）

### 3.2 Framebuffer (VOP)

**测试命令**：
```bash
nsh> ls /dev/fb0
# 预期: /dev/fb0 存在

nsh> cat /dev/fb0 | xxd | head
# 预期: 显示一些十六进制数据（framebuffer 内容）
```

**LVGL 启动测试**：
```bash
nsh> lvgl_homepage &
# 预期: LCD 屏幕上出现 LVGL 主页 UI
```

**通过标准**：
- LCD 屏亮起（不是黑屏/白屏）
- 显示 LVGL 主页
- 触摸坐标能反馈（结合 3.3）

### 3.3 Touch (GT911)

**测试命令**：
```bash
nsh> ls /dev/input0
# 预期: /dev/input0 存在

nsh> cat /dev/input0 &
# 然后触摸屏幕
# 预期: 终端上看到 touch 事件（BEGIN/MOVE/END）
```

**通过标准**：
- 触摸有响应
- 坐标范围合理（0-479 / 0-853）
- 多点触摸可选支持

### 3.4 USB Host

**测试命令**：
```bash
# 插入 U 盘
nsh> ls /dev/sda
# 预期: /dev/sda 出现

nsh> mount -t vfat /dev/sda /mnt
# 预期: 挂载成功

nsh> ls /mnt
# 预期: 看到 U 盘内容
```

**通过标准**：
- 插入 U 盘后 /dev/sda 出现
- 文件系统可挂载
- 读写文件成功

---

## 4. 调试技巧

### 4.1 串口无输出（黑屏）

1. **检查串口线**：TX/RX 是否接反？GND 是否接上？
2. **检查波特率**：115200 8N1，是否是 8M/16M 晶振误判？
3. **检查 UART 时钟**：`rk3506_lowputc.c` 中 `UART_CLK` 应该是 24000000
4. **重烧 MiniLoader**：用 `upgrade_tool ul MiniLoaderAll.bin`

### 4.2 启动停在某阶段

```bash
# 1. 停在 BootROM：检查 U-Boot 烧写是否成功
# 2. 停在 U-Boot：检查 parameter.txt 和 boot 分区
# 3. 停在 NuttX "NuttShell" 之前：检查 early init
# 4. 停在 NSH 提示符：检查 NSH_ARCHINIT
```

### 4.3 I2C 设备无响应

```bash
# 1. 检查 I2C 总线时钟：24 MHz (Pclk)
# 2. 检查 GT911 复位时序：RST 高 → 至少 10ms → INT 高 → 55ms
# 3. 用示波器/逻辑分析仪看 I2C SCL/SDA 波形
# 4. 检查 GT911 INT 引脚：reset 期间应保持低（选 0x5D 地址）
```

### 4.4 LCD 不亮

```bash
# 1. 检查 VOP 寄存器：DSP_CTRL0[0] (RGB_EN) 应为 1
# 2. 检查 ST7701S reset 序列：120ms 等待
# 3. 检查 RGB 接线：HSYNC/VSYNC/DE/DATA0-23/PCLK
# 4. 检查背光：PWMI/BL_EN 信号
```

---

## 5. 关键文件参考

| 文件 | 用途 |
|------|------|
| `vendor/rockchip/chips/rk3506/rk3506_lowputc.c` | 早期串口（UART 时钟） |
| `vendor/rockchip/chips/rk3506/rk3506_i2c.c` | I2C 控制器（v2 重写） |
| `vendor/rockchip/chips/rk3506/rk3506_vop.c` | 显示控制器（Round 7 修复） |
| `vendor/rockchip/chips/rk3506/rk3506_usbhost.c` | DWC2 USB Host |
| `vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_st7701s.c` | LCD 面板初始化 |
| `vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_gt911.c` | 触摸初始化 |
| `nand_firmware/parameter.txt` | 分区表 |
| `nand_firmware/pack_nand.sh` | 打包脚本 |
| `docs/zh-cn/skills/openvela-fast-chip-porting/SKILL.md` | 移植指南 |
| `docs/zh-cn/skills/openvela-fast-chip-porting/RK3506_DRIVER_ISSUES.md` | 驱动问题清单 |

---

## 6. 提交 bug 报告

发现 bug 后，请：

1. 记录复现步骤（精确到命令）
2. 记录预期行为 vs 实际行为
3. 提供串口完整 log
4. 在 GitHub Issues 提 ticket
5. 修复后更新 `RK3506_DRIVER_ISSUES.md` 的状态
