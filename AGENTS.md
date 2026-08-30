# AGENTS.md — OpenVela RK3506G2 移植项目

> **面向 AI 代理的协作规范**。本文件定义在本项目工作时必须遵守的规则。
> 配合 `docs/zh-cn/skills/openvela-fast-chip-porting/SKILL.md` 使用。

---

## 1. 项目背景

- **目标平台**：Rockchip RK3506G2 (3×Cortex-A7 + 1×Cortex-M0, 128MB DDR3)
- **开发板**：HD-RK3506-EVM (480x854 ST7701S RGB LCD, GT911 触摸)
- **仓库结构**：本仓库是 openvela 的"比赛"模板（`app/` `board/` `quickapp/` 为空），主代码在 `openvela/` 子目录（git submodule 形式）。

---

## 2. 工作目录约定

| 目录                                            | 用途                    | 注意事项                                   |
| ----------------------------------------------- | ----------------------- | ------------------------------------------ |
| `/home/b4qaq/project/`                        | 仓库根目录              | 比赛模板，不要乱改                         |
| `/home/b4qaq/project/openvela/`               | 主代码（openvela 仓库） | 实际修改这里                               |
| `/home/b4qaq/project/RK3506G2/`               | 厂商 SDK（参考用）      | 不修改                                     |
| `/home/b4qaq/project/HD-RK3506-EVM`           | 厂商 文档（参考用）     | 不修改                                     |
| `/home/b4qaq/project/nanopi_m4_rk3399`        | rk3399参考代码          | 不修改                                     |
| `/home/b4qaq/project/OpenVelaDocs`            | OpenVela官方文档        | 不修改                                     |
| `/home/b4qaq/project/openvela/cmake_out/`     | 构建输出                | 可以 `rm -rf`                            |
| `/home/b4qaq/project/openvela/nand_firmware/` | 烧录用镜像              | 提交 `parameter.txt` 和 `pack_nand.sh` |

---

## 3. 关键规则（必读）

### 3.1 构建相关

1. **必须使用 `build.sh`，不要直接 cmake**。它会自动设置 PATH、ccache、lunch 流程。
2. **必须设置 `CCACHE_DIR=/tmp/ccache_dir`**。默认 `~/.cache/ccache` 不可写。
3. **必须把 prebuilt 工具加到 PATH**：
   ```bash
   PATH="$PWD/prebuilts/build-tools/linux-x86_64/bin:$PWD/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH"
   ```
4. **完整命令模板**：
   ```bash
   cd /home/b4qaq/project/openvela
   rm -rf cmake_out/hd-rk3506-evm_nsh
   CCACHE_DIR=/tmp/ccache_dir \
   PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
   ./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j$(nproc)
   ```

### 3.2 defconfig 修改

1. **不要手动编辑 defconfig 超过必要范围**。改动流程：
   - `cd` 到 defconfig 目录
   - `make menuconfig`（用 prebuilt 的 kconfig-mconf）
   - 选好后退出（保存到 `.config`）
   - `make savedefconfig`（重新生成 defconfig）
2. **必须保留的项**：
   - `CONFIG_RAW_BINARY=y`
   - `CONFIG_INIT_ENTRYPOINT="nsh_main"`
   - `CONFIG_ARCH_CHIP_CUSTOM=y`
   - `CONFIG_RAM_START=0x02080000`
   - `CONFIG_RAM_SIZE=134217728`（128MB）

### 3.3 路径处理

1. **禁止硬编码绝对路径**。用相对路径或 `${NUTTX_BOARD_ABS_DIR}/../..` 风格。
2. **`NUTTX_TOP_DIR` 不存在**，要用 `get_filename_component(${NUTTX_BOARD_ABS_DIR}/../../../.. ABSOLUTE)`。
3. **`find_program(... NO_DEFAULT_PATH)`** 找交叉工具链，否则会找到系统的 gcc。
4. **pack_nand.sh 必须用环境变量**（`RK3506_SDK_DIR`, `NUTTX_BIN`, `PARAM_FILE`），不硬编码。

### 3.4 驱动开发

1. **不要碰 `arch/`、`nuttx/boards/` 下的通用代码**（除非必要）。只改 `vendor/rockchip/` 下的内容。
2. **所有 RK3506 驱动在** `vendor/rockchip/chips/rk3506/<chip>_<driver>.c`。
3. **所有板级代码在** `vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/<board>_<file>.c`。
4. **新驱动必须满足**：
   - 有 `Kconfig` 选项
   - 在 `Make.defs` 和 `CMakeLists.txt` 中注册
   - 寄存器操作封装为 `putreg32` / `getreg32`（除非 NuttX 已提供）
   - 错误路径用 `ierr()`/`iinfo()`/`iwarn()` 三个日志级别
5. **参考代码优先级（强制）**：写任何 RK3506 外设驱动时，必须**只**以下列两处为事实基准，**不得凭芯片手册记忆或猜测寄存器/时序**：
   - **Linux SDK**：`/home/b4qaq/project/RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/`
     - 寄存器位域/基址：`hal/lib/CMSIS/Device/RK3506/Include/rk3506.h`
     - HAL 驱动序列：`hal/lib/hal/src/<periph>.c`
     - 时钟门控/mux/复位：`kernel-6.1/drivers/clk/rockchip/clk-rk3506.c`
     - 引脚复用：`u-boot/arch/arm/dts/rk3506-pinctrl.dtsi` + `u-boot/drivers/pinctrl/rockchip/pinctrl-rk3506.c`
     - 外设/板级：`u-boot/arch/arm/dts/rk3506*.dtsi`
   - **已适配 A7 的 R258 移植**：`openvela/vendor/allwinnertech/chips/r528/`
     - NuttX 侧驱动组织结构、`spi_ops`/MTD/bringup 的接法、HAL→NuttX 的封装套路（`drv/`、`drivers/rtos-hal/hal/`）。
   - 寄存器地址/位/时序一律以 **Linux SDK** 为准；NuttX 集成方式（如何挂 `spi_ops`、`mtd_dev_s`、`netdev`、`audio` 等）优先照 **R258**。
6. **遇到“两处都没有参考代码”的点，必须停下来询问用户是否继续**，不要自行发明实现。典型场景：
   - Linux SDK 与 R258 都没覆盖某寄存器/时序/PHY 校准/DLL/延迟线调优；
   - 需要在 NuttX 里自研一个 R258 没有对应物的上层框架（如 spi-mem/spi-nand MTD 桥）；
   - SDK 只有 DMA/中断路径而要改轮询、或要做未经验证的时序简化。
   - 询问时给出：(a) 卡在哪一步、(b) SDK/R258 各有什么、(c) 我打算怎么实现及风险。

### 3.5 提交前检查

每次改完代码，**必须**：

1. `rm -rf cmake_out/hd-rk3506-evm_nsh`（彻底清空）
2. 用 3.1 的命令重新构建
3. 验证产物：
   - `cmake_out/hd-rk3506-evm_nsh/vela.bin` 存在
   - `nand_firmware/update.img` 存在
4. 跑 `bash nand_firmware/pack_nand.sh` 看是否成功

### 3.6 方案决策（用户强制参与）

**除非用户特意说明放行，任何方案层面的决定都必须先让用户参与**：

- 技术路线选择（如：自己造轮子 vs 用 NuttX/SDK 现成机制、轮询 vs 中断、兜底 hack vs 标准接口）
- 架构与配置取舍（新增/修改 Kconfig、defconfig 选项的取舍）
- 驱动实现方式的重大改动

做法：先给出 (a) 备选方案、(b) 各自代价/风险、(c) 我的倾向及理由，等用户确认或拍板后再实施。用户直接指示的事项（"实现 X"）按指示做，但由该指示派生的子决策仍需说明并征求意见。

---

## 4. 已知限制 / 待修复

| 项目                                                                       | 状态                        | 备注                                                                                                                                                                                   |
| -------------------------------------------------------------------------- | --------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `nuttx/arch/arm/Kconfig` 增加 `ARCH_CHIP_RK3506`                       | ✅**必需保留**        | 之前 AI 修改                                                                                                                                                                           |
| `vendor/rockchip/boards/rk3506/hd-rk3506-evm/CMakeLists.txt` 重写        | ✅**必需保留**        | 修复了 34MB 零填充 bug                                                                                                                                                                 |
| `vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/parameter.txt` 重写 | ✅**必需保留 (v5)**   | v5: A/B 双槽布局 — vnvm/uboot/misc/boot_a(10M)/boot_b(10M)/userdata(grow)，删 recovery/system/vendor/oem/data；**与旧布局不兼容，需全量刷**                                                                                                                            |
| `nand_firmware/pack_nand.sh` 重写                                        | ✅**必需保留 (v5)**   | v5: A/B package-file（boot_a/boot_b 同 FIT 镜像 + misc 8KB 零占位，不含 rootfs/recovery/oem/userdata）；PARAM_FILE 优先级 环境 > 板级 v5 > SDK；v4 遗产：FIT external-data (`mkimage -E`)、`-RK350F` |
| `nand_firmware/nuttx.its` + `boot.fit`                                 | ✅**必需**            | FIT 源/产物：kernel `load=0x02080000 entry=0x02080560`（真实地址，bit0=0 → v7-A ARM 模式）；boot.fit 同时覆盖 boot.img                                                              |
| `nand_firmware/uboot.img` (预编译 813KB)                                 | ✅**必需**            | MiniLoader 链式加载的 U-Boot FIT                                                                                                                                                       |
| `nand_firmware/boot.uimg` (uImage 格式)                                  | ✅**必需**            | U-Boot bootm 加载的 kernel 镜像                                                                                                                                                        |
| `rk3506_i2c.c` (v2)                                                      | ✅**重写完成 + 修复** | 基于 Linux i2c-rk3x.c，clock divider 公式从 `(pclk/8/scl)-1` 修正为 `DIV_ROUND_UP(pclk, 8*scl) - 2`                                                                                |
| `rk3506_lowputc.c`                                                       | ✅**已修复**          | UART 时钟从 1.8432 MHz 修正为 24 MHz                                                                                                                                                   |
| `rk3506_serial.c`                                                        | ✅**已修复**          | UART_SCLK 从 1.8432 MHz 修正为 24 MHz                                                                                                                                                  |
| `hd_rk3506_bringup.c`                                                    | ✅**已重写**          | 不再因单个驱动失败中断后续初始化；添加 I2C 控制器初始化                                                                                                                                |
| `rk3506_vop.c`                                                           | ✅**已修复**          | WIN1_CTRL0 format 字段位域修正、GRF HIWORD_UPDATE 模式修正、dsp_layer_sel 修正                                                                                                         |
| GMAC0 PHY ioctl + netinit monitor                                        | ⚠️**monitor 已回退**  | 驱动侧 SIOCMIINOTIFY/SIOCGMIIPHY/SIOCGMIIREG/SIOCSMIIREG 保留（休眠，无消费者）；**NETINIT_MONITOR 实测有害已关**：监控线程的 ifup/ifdown + MDIO ioctl 与驱动 5s 链路等待/ifup 持锁互相卡死，导致 dhcp/ping/curl 全部冻结。重开前提：ifup 去掉持锁 sleep（链路等待挪出锁外）+ 通知改 PHY 中断驱动，方案需 3.6 确认 |
| GMAC0 日志策略                                                           | ✅**按用户要求**      | 只保留 link up/down（用户拍板不算错误）和错误日志（nerr）；"no link after Nms" 为 nerr；其余 info 全删。任何再增打印先过 3.6                                                                    |
| 启动 DHCP 重试                                                           | ✅**根因已定位**      | 根因：交换机/路由器端口 STP listening/learning（10~30s）丢弃 BOOTP 广播，PHY 5s 就 up 但 boot DHCP 默认 3 重试（9s）全被吞；且 `netinit_net_bringup()` 的 DHCP 失败路径**静默 return 不打日志**（这就是"开了 DEBUG_ERROR 也没看到 ERROR"的谜底）。`NETUTILS_DHCPC_RETRIES=10`（~30s）覆盖 STP 窗口。曾误把它当 22:34 全卡死的元凶回退过——实际卡死是 NETINIT_MONITOR 锁竞争，重试只是拉长失败窗口 |
| 系统时间 / TLS BADCERT_FUTURE (curl: 60)                                 | ✅**已修复**          | 根因：无电池 RTC，boot 时钟停在 1970，所有证书 notBefore 都"来自未来"。已开 `SYSTEM_NTPC=y`（ntpcstart/stop/status 命令）+ rcS 里 `ntpcstart`（daemon 后台指数退避重试 1s→2s→…→120s，60 次才退；网络一通即同步，之后每 60s 重同步）；netinit 在 DHCP 成功后也会启动（状态锁防重复）。`date` 可验证，强制立即重试用 `ntpcstop`+`ntpcstart` |
| DNS 解析失败 / 无 nameserver (curl: 6 Could not resolve)                 | ✅**已修复**          | 根因：`NETINIT_DNS` 未开，netinit 从不设置 DNS；解析器里有没有 nameserver **完全取决于 DHCP 回包是否带 option 6**——某次 boot 的 ACK 没带（boot DHCP 已成功拿到 IP/网关，DNS 却是空），curl/ping 按名字立即失败。已开 `NETINIT_DNS=y`+`NETINIT_DNSIPADDR=0xc0a80a01`（netinit 预设网关 192.168.10.1 作 DNS 兜底，DHCP 带了就覆盖）；`NETDB_DNSCLIENT_RECV_TIMEOUT` 30→5s（死 nameserver 时解析 ≤15s 而非 90s）。**注意**：boot DHCP 成功后手动再跑 `ifconfig eth0 dhcp`，服务器对重复 DISCOVER 常不应答 → 10×3s=30s 才报错，不是死锁，也不需要再跑 |
| 控制台 Ctrl+C（curl 卡终端）                                             | ✅**已修复**          | 根因：`CONFIG_TTY_SIGINT` 未开，串口 Ctrl+C 无法给前台任务发 SIGINT，NSH 等卡死的 curl 无法打断。已开 `TTY_SIGINT=y`（NSH 前台运行子命令时自动 TIOCSCTTY 绑定子任务 pid）                     |
| curl DNS "getaddrinfo() thread failed to start" (curl: 6)                | ✅**已修复**          | 根因：threaded resolver（`curl_config.h` 的 `USE_THREADS_POSIX`）要起 pthread + 用环回 TCP 手搓 socketpair 自检，任一环失败就报这条误导性错误。已注释掉该宏改走 `CURLRES_SYNCH` 同步解析（getaddrinfo 直接在 curl 任务里跑，真实 EAI_* 错误可见）。apps/external/curl/curl_config.h 属 openvela 自带的移植配置文件，改它不算动上游 curl 源码 |
| TLS 熵源 "CTR_DRBG - entropy source failed" (-0x0034)                    | ✅**已修复**          | 根因：mbedtls NuttX 熵 poll 走 `getrandom()` → `/dev/urandom`，板上没这个设备。已开 `CRYPTO=y`+`CRYPTO_RANDOM_POOL=y`+`DEV_URANDOM=y`+`DEV_URANDOM_RANDOM_POOL=y`（BLAKE2s 熵池 + IRQ 喂熵，不用裸 xorshift128）。注意 `CRYPTO_RANDOM_POOL` 在 `if CRYPTO` 块内，只加它不加 `CRYPTO` 会被 kconfig 静默丢弃；choice 算法切换需显式写 `DEV_URANDOM_RANDOM_POOL=y` |
| TLS CA 证书 "Error reading ca cert file" (curl: 77)                      | ✅**已修复**          | 根因：curl 默认信任库路径 `/etc/ssl/curl/ca-certificates.crt`（`curl_config.h` 的 `CURL_CA_BUNDLE`）在板上是空目录。已把宿主机 ca-certificates 包的 bundle（121 个根证书，178KB）放进板级 romfs `src/etc/ssl/curl/`，并在 `src/CMakeLists.txt` 的 `nuttx_add_romfs(RCRAWS ...)` 里登记（RAW 文件原样打包，romfs 大小 +178KB） |
| `rk3506_usbhost.c`                                                       | ⚠️**可能有 bug**    | 8000+ 行复杂驱动                                                                                                                                                                       |
| `hd_rk3506_gt911.c`                                                      | ⚠️**可能有 bug**    | 2000+ 行                                                                                                                                                                               |
| `hd_rk3506_st7701s.c`                                                    | ⚠️**可能有 bug**    | 1300+ 行                                                                                                                                                                               |
| `{etc/init.d}` 空目录（残留）                                            | ❌**已删除**          | 上一个 AI 笔误                                                                                                                                                                         |
| `/data 分区位置错误（潜伏 bug）`                                          | ✅**已修复**          | bringup 把擦块索引当 mtd_partition 的页索引传，/data 实际映射 3.7MB 处 384KB（在 uboot 分区内！）。没炸是因为每次全量刷都重写 uboot 分区。已改为页单位传参 + userdata 0x14800 扇区（v5 布局）。mtd.h 的 "offset in bytes" 注释是过时的（实际单位=geo.blocksize） |
| A/B 双分区 OTA                                                            | ✅**代码完成，未上板** | `rk3506_ota.c` /dev/ota（AvbABData @ misc+2048，U-Boot CONFIG_ANDROID_AB 协议）+ `ota` NSH 命令 + rcS bootcheck（try 递减/回滚）+ parameter.txt v5（boot_a/boot_b 双 10MB 槽）。**分区布局变更，旧镜像不兼容，需全量刷**。验收见 `MORNING_CHECKLIST.md` §2 |
| FSPI 并发锁                                                               | ✅**已修复**          | 多 MTD 分区消费者（dhara /data 与 OTA misc/boot_a/boot_b）可并发进 FSPI 控制器；`rk3506_fspi_nand_op` 已加互斥锁串行化                                                                 |

**`vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/{etc/` 目录** ❌ 这是上一个 AI 用错误的 `cp` 命令创建的（文件名有 `{` 和 `}`），目录是空的，已删除。

---

## 5. 关键文件速查

| 任务                    | 看这里                                                                               |
| ----------------------- | ------------------------------------------------------------------------------------ |
| 修改 NSH 配置           | `vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/defconfig`                |
| 修改 U-Boot/Loader 流程 | `nand_firmware/parameter.txt` + `nand_firmware/pack_nand.sh`                     |
| 修改 boot 流程          | `nuttx/arch/arm/src/armv7-a/arm_head.S`（慎改）                                    |
| 修改中断                | `vendor/rockchip/chips/rk3506/rk3506_irq.c`                                        |
| 添加外设                | `vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_appinit.c`              |
| 修改 Kconfig            | `nuttx/arch/arm/Kconfig`（顶层）+ `vendor/rockchip/chips/rk3506/Kconfig`（芯片） |
| 改链接脚本              | `vendor/rockchip/boards/rk3506/hd-rk3506-evm/scripts/ld.script`                    |

---

## 6. 测试矩阵

| 测试     | 命令                   | 通过标准                          |
| -------- | ---------------------- | --------------------------------- |
| 干净编译 | 见 3.1                 | 退出码 0，无 warning              |
| 启动 NSH | 烧录后重启             | `nsh>` 提示符                   |
| 内存检测 | `nsh> free`          | ~27.5MB（RAM 缩到 0x1B80000，顶部 2MB 让给 rpmsg 共享窗，属预期）  |
| 文件系统 | `nsh> mount`         | 至少 `/etc` 挂载                |
| 进程列表 | `nsh> ps`            | 至少 NSH 进程                     |
| LCD 显示 | `nsh> lvgl_homepage` | 屏幕显示 UI（需 menuconfig 启用） |
| USB 设备 | 插 U 盘                | `/dev/sda` 出现                 |

---

## 7. 提交规范

1. **每次提交前**：跑 3.5 的检查清单。
2. **提交信息格式**：
   ```
   <scope>: <summary>

   <详细说明>
   - 修改了哪些文件
   - 为什么这样改
   - 验证方法
   ```
3. **scope** 用 `chip`/`board`/`build`/`pack`/`doc`/`fix`/`test` 等。
4. **避免大改**：每次 commit 只做一件事。重构和功能分开。

---

## 8. 紧急情况

### 8.1 烧录失败 / 板子变砖

- 短接 RECOVERY 后插入 USB，进入 Loader 模式
- 用 `upgrade_tool lf` 看 Loader 是否识别
- 用 `upgrade_tool uf update.img` 全量刷
- 如果 Loader 都没了，从 `nand_firmware/MiniLoaderAll.bin` 重新烧

### 8.2 编译过但启动黑屏

1. 串口是否正常？（换 USB-TTL 适配器）
2. 波特率？115200 8N1
3. 时钟配置对吗？看 `rk3506_lowputc.c` 的 baud rate 设置
4. `CONFIG_DEBUG_FULLOPT=y` 重新编译看 log

### 8.3 ccache 总是 Permission denied

```bash
# 清空 ccache 缓存
rm -rf /tmp/ccache_dir
# 重新构建
CCACHE_DIR=/tmp/ccache_dir ./build.sh ...
```

---

## 9. 更多信息

- 详细移植步骤：`docs/zh-cn/skills/openvela-fast-chip-porting/SKILL.md`
- 官方文档：`docs/zh-cn/chip_porting/porting_guide.md`
- 比赛规则：仓库根目录的 `README.md`
- 比赛代码提交：`docs/zh-cn/contest_2026/code_submission_guide.md`
- AI 协作日志：`docs/zh-cn/contest_2026/ai_coding_log_guide.md`
