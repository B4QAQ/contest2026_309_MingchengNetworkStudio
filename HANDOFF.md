# HANDOFF — RK3506G2 OpenVela 适配交接

> 更新时间：2026-09-20。给下一位 AI/人工接手用。工作规范见 `AGENTS.md`（3.5 干净构建/打包门禁、3.6 方案决策必须用户拍板）。

## 0. 一句话现状

系统各功能（网络 DHCP/DNS/NTP/curl HTTPS、USB U盘、SPI NAND+littlefs、OTA A/B 元数据、LVGL、PWM/ADC/CAN 回环/WDT）均已板上验证；**唯一未收尾主线：rpmsg A7↔M0 在改为"U-Boot 从 amp 分区加载 M0"的 SDK 标准流程后，M0 已确认被 U-Boot 加载放行但不响应邮箱 kick，正在定位 M0 侧死因（需要 M0 串口）**。

## 1. 仓库与推送

| 仓库 | 路径 | 说明 |
|---|---|---|
| 比赛外层仓 | `/home/b4qaq/project` (git) | remote `origin` = git@github.com:B4QAQ/contest2026_309_MingchengNetworkStudio.git，分支 `dev-ai-contest-2026` |
| openvela 主体 | `openvela/`（repo 管理，`.repo/`） | 外层仓只追踪其中 60 个文件（符号链接 + `nand_firmware/*` 产物/脚本），不是 submodule |
| vendor/rockchip | `openvela/vendor/rockchip`（独立 git，`.git -> ../../.repo/projects/vendor/rockchip.git`） | HEAD `5400e73`，**rpmsg 重构改动未提交** |
| nuttx | 冻结在 `bc7e01ef0d1`，不要动 |
| U-Boot 工作树 | `/home/b4qaq/project/uboot_build/u-boot`（仓外，不随仓提交） | 产物 `fit/uboot.itb` 拷入 `openvela/nand_firmware/uboot.itb` 随仓提交 |

**推送必须走 443**（22 被墙）：
```bash
GIT_SSH_COMMAND="ssh -p 443 -o Hostname=ssh.github.com -o StrictHostKeyChecking=accept-new" \
  git push origin dev-ai-contest-2026
```
中文提交信息用 `git commit -F /tmp/msg.txt`。rpmsg 相关改动在 vendor 仓与外层仓**分别提交**，不要把无关脏改（netcfg/gmac、cru/can/i2c/spi/vop/gt911/saradc cleanup、gmacstat.c 删除等）混进 rpmsg 提交。

外层仓已推送的最新文档提交：技术报告 `e41f274`（按大赛模板重写）、演示视频脚本 `164ec67`、vendor 快照 `abcc3f0`、openvela 追踪恢复 `bba57c7`。

## 2. 当前 rpmsg 架构（用户拍板：完全照搬 SDK，M0 交 U-Boot 加载放行）

- M0 固件 = SDK HAL `TestDemo.bin`（rpmsg remote test）→ `mkimage -f amp.its -E -p 0xe00` 打成 standalone FIT（`load=0xfff84000`，sha256 无签名），烧入独立 GPT 分区 **amp**。
- U-Boot（已开 `CONFIG_AMP=y` + `CONFIG_ROCKCHIP_AMP=y`）在 `board_late_init → amp_cpus_on()` 复位态拷固件入 M0 SRAM，`fit_standalone_release()` 放行（SMC 0x82000028 → CRU_GATE_CON5 → GRF_SOC_CON36 → PMU_INT_MASK_CON）。
- NuttX 侧 `rk3506_rptun.c` 已精简（1070→约 650 行）：只开 A 侧时钟（CON5 M0、CON6 mailbox/intmux/timer、CON8 STCLK、CON11 UART4）+ 邮箱收发/ISR，**不再拷贝/放行/握手写**。
- 分区表 v6：`...boot_b 0x5000@0xB800, amp 0x800@0x10800, userdata -@0x11000`（两份 parameter.txt 已同步）；bringup /data 偏移 0x11000。**改分区必须全量烧**（首启 littlefs 重格式化）。

### 2.1 v8n 板上结果（2026-09-20，最新镜像 md5 `8bd45ec312584fa187699e71b0b0aefa`）

U-Boot 打印：
```
## Loading loadables ... mcu ... 27120 Bytes ... Load Address 0xfff84000 ... sha256 OK
Loading loadables from ... to 0xfff84000
Handle standalone: 'mcu' at 0xfff84000 ...OK
```
加载链完全正确。但进 NuttX 后：首条 kick 即 `mailbox3 A2B busy, kick dropped`，rpmsgtest 10s 超时 FAIL——症状与旧的 NuttX 自行加载时代完全一致。**结论：问题不在 M0 加载方式，而在更底层：M0 不服务邮箱 ISR（或根本没跑到/跑飞）。**

### 2.2 下一步（按此顺序，不要跳步）

1. **接 M0 串口看它死在哪**。M0 原生调试口 UART4（GPIO1_C2/C3 = 核心板邮票孔 pin 91/92/100/101 区域，底板未引出）。**底板 J9 40Pin 只引出了 UART1/2/3（pin 23~28）**，方案：改 M0 固件 `hal/project/rk3506-mcu/src/main.c` 的 `pUart=UART4`→`UART2`，RMIO 复用改到 J9 的 UART2_TX(pin26)/RX(pin25) 对应 RM_IO 引脚（RMIO function ID：UART2_TX=18、UART2_RX=19，见 `u-boot/arch/arm/dts/rk3506-pinctrl-rmio.dtsi`；**J9 默认 UART2 焊盘具体落在 RM_IO 几还没从 RM_IO 功能复用表图确认，接手先做完这步映射**，图片在 `HD-RK3506-EVM/产品介绍/img/9cn4pcWm2YkGmwKv/1761809001730-*.png`），同时 NuttX 侧 CON11 门控换成 UART2（PCLK/SCLK UART2 门控位查 `clk-rk3506.c`，别凭记忆）。波特率 1500000 8N1。
   - 有 `Hello RK3506 mcu` + rpmsg init 日志 → M0 活着，问题在 INTMUX→NVIC IRQ31 投递；
   - 无输出/半句 → 早期 HardFault 或时基/PMU 问题。
2. 备选：示波器/逻辑分析仪量 GPIO1_C2（不焊线，探针点 UART4 焊盘）确认有无字符波形。
3. 历史探针结论（AGENTS.md rpmsg 行长表，先读再动）：v8h 曾在"NuttX 加载 + INTMUX 就绪握手 + 时基时钟"版本 PASS（镜像 md5 `d4c903a465753dcb4a03c7970a5aa5b0`，vendor 仓历史可查），但 v8j 发现温重启概率 DFAR=0x3d 堆损坏（不复位 M0 覆盖活核所致）。U-Boot 流程消除了覆盖问题却仍 FAIL，说明还缺某个"M0 真正跑起来"的条件——重点怀疑 PMU 隔离/GRF 默认值差异（U-Boot release 与旧手工序列看似一致但没有板上对照过 CON36/PMU_INT_MASK 回读）。
4. PASS 后验收：冷启动 + `reboot` 温启动各跑一次 `rpmsgtest`，期望 `reply (26 bytes): "Rockchip rpmsg linux test!"`，且温启动无 dataabort。

## 3. 未提交改动清单

vendor/rockchip 仓（`git -C openvela/vendor/rockchip status`）当前脏改中属于 rpmsg SDK 流程、待板上 PASS 后提交的：
- `chips/rk3506/rk3506_rptun.c`（重写：U-Boot 加载模型）
- `chips/rk3506/Kconfig`（删 RK3506_RPTUN_BOOT_M0）
- `chips/rk3506/rk3506_ota.h`（amp 分区注释）
- 删除 `chips/rk3506/rk3506_mcu_fw.h`
- `boards/.../configs/parameter.txt`（v6）、`boards/.../src/hd_rk3506_bringup.c`（0x11000）

外层仓对应待提交：`openvela/nand_firmware/` 下 parameter.txt（两份之一在外层）、pack_nand.sh、build_mcu_fw.sh、amp/amp.its、amp/amp.img、amp/mcu.bin、uboot.itb（md5 `4745297063db0f266e85eb419e58ed64`）、UBOOT_BUILD.md。

其余脏改（netcfg/gmac defconfig、gmacstat.c 删除、cru/can/i2c/spi/vop/gt911/saradc）是独立任务，单独提交。

## 4. 构建 / 打包 / 烧录（AGENTS.md 3.5 门禁，一步不能省）

```bash
cd /home/b4qaq/project/openvela
# M0 固件 + amp.img（需要 SDK；只读引用）
RK3506_SDK_DIR=/home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm \
  bash nand_firmware/build_mcu_fw.sh
# NuttX 干净构建
rm -rf cmake_out/hd-rk3506-evm_nsh && mkdir -p /tmp/ccache-tmp
CCACHE_DIR=/tmp/ccache_dir CCACHE_TEMPDIR=/tmp/ccache-tmp \
PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
  ./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j$(nproc) \
  > /tmp/build.log 2>&1; echo EXIT=$?   # 禁止 tail 管道吞退出码
bash nand_firmware/pack_nand.sh        # 产物 nand_firmware/update.img
# 烧录（分区变过必须 uf 全量）
sudo upgrade_tool uf /home/b4qaq/project/openvela/nand_firmware/update.img
```
U-Boot 重编（一般不用，已编好）：见 `nand_firmware/UBOOT_BUILD.md`，fragment 含 AB 六行 + `CONFIG_AMP=y`/`CONFIG_ROCKCHIP_AMP=y`。

NSH 无 grep；用户会直接贴串口原文。控制台 UART0 115200。

## 5. 已板上验证的功能（回归时别改坏）

- 启动/NSH、free≈27.5MB；SPI NAND 256MiB + littlefs /data（autoformat）
- 网络：`netcfg: dhcp on eth0 ok`；ping 网关≈2ms；域名解析；NTP 后 `date` 正确；`curl --max-time 20 -o /tmp/o https://stdl.b4qaq.cn/fwtb/info.json` 成功（四个独立根因都修了：时间/DNS/CA 包/urandom，见 AGENTS.md）
- USB：OTG1 Host，U 盘 /dev/sda，`mount -t vfat /dev/sda /mnt/usb`（238GB 盘实测过）；Hub 不支持（缺 asynch）
- VOP /dev/fb0 + LVGL 480×854 init OK（面板/GT911 台架未接，-110 属预期）
- PWM0 ch2、SARADC、CAN0 回环 cansend/candump、watchdog0/1（喂狗/停喂复位）
- OTA：`ota status/confirm`、rcS bootcheck 正常；`ota update` 端到端待验

## 6. Backlog（rpmsg 收尾之后）

1. M0 UART 改 RMIO 映射 → 定位 rpmsg 根因（本文件 §2）
2. st7701s / GT911 接面板后的验收
3. USB hub：给 rk3506_usbhost.c 补 asynch（方案过 3.6）
4. OTA `ota update /data/nuttx.fit` 端到端 + 回滚实测
5. curl HTTPS 传完不退出：先取证不改码（AGENTS.md 该行有完整命令：`curl -o /tmp/o URL; echo DONE=$?` / `-v` / `--max-time`）
6. 72h 长稳测试；I2S 音频
7. 上游 usbhost_storage 6 条 `-Wformat` 死代码警告的处理决策
