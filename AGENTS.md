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
   - 错误路径用 `_err()`/`_warn()`/`_info()` 三个日志级别。**禁止用 `ierr()`/`iwarn()`/`iinfo()`**——它们是 INPUT（输入子系统）的模块宏（CONFIG_DEBUG_INPUT_* 门控，本工程未开），用了全部静默；v8c 已修复 12 个文件的此错误（USB 驱动用 uerr/uwarn/uinfo 等模块宏的除外）
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
| `nand_firmware/uboot.img` (自建 AB 版 823KB)                             | ✅**必需 (v6)**       | MiniLoader 链式加载的 U-Boot FIT。**预编译版无 CONFIG_ANDROID_AB 只认字面 `boot` 分区**，v5 首刷卡死根因；已用 SDK 源码 + rk3588-ab.config 配方重编（见 `nand_firmware/UBOOT_BUILD.md`），源 `uboot.itb` 随仓库提交，pack 优先取本地 `uboot.itb`（`UBOOT_IMG` 环境变量可覆盖） |
| `nand_firmware/boot.uimg` (uImage 格式)                                  | ✅**必需**            | U-Boot bootm 加载的 kernel 镜像                                                                                                                                                        |
| `rk3506_i2c.c` (v2)                                                      | ✅**重写完成 + 修复** | 基于 Linux i2c-rk3x.c，clock divider 公式从 `(pclk/8/scl)-1` 修正为 `DIV_ROUND_UP(pclk, 8*scl) - 2`                                                                                |
| `rk3506_lowputc.c`                                                       | ✅**已修复**          | UART 时钟从 1.8432 MHz 修正为 24 MHz                                                                                                                                                   |
| `rk3506_serial.c`                                                        | ✅**已修复**          | UART_SCLK 从 1.8432 MHz 修正为 24 MHz                                                                                                                                                  |
| `hd_rk3506_bringup.c`                                                    | ✅**已重写**          | 不再因单个驱动失败中断后续初始化；添加 I2C 控制器初始化                                                                                                                                |
| `rk3506_vop.c`                                                           | ✅**已修复**          | WIN1_CTRL0 format 字段位域修正、GRF HIWORD_UPDATE 模式修正、dsp_layer_sel 修正                                                                                                         |
| GMAC0 PHY ioctl + netinit monitor                                        | ⚠️**monitor 已回退**  | 驱动侧 SIOCMIINOTIFY/SIOCGMIIPHY/SIOCGMIIREG/SIOCSMIIREG 保留（休眠，无消费者）；**NETINIT_MONITOR 实测有害已关**：监控线程的 ifup/ifdown + MDIO ioctl 与驱动 5s 链路等待/ifup 持锁互相卡死，导致 dhcp/ping/curl 全部冻结。重开前提：ifup 去掉持锁 sleep（链路等待挪出锁外）+ 通知改 PHY 中断驱动，方案需 3.6 确认 |
| GMAC0 日志策略                                                           | ✅**按用户要求（v8i 全删）** | **v8i 用户拍板：link up/down 和 nerr 全删，GMAC0 完全静默**。删了 8 处 nerr（TX 超时/DMA 错误/Init/Start/irq_attach/PHYInit/PHYStartup 失败/no link after Nms）和 2 处 `syslog(LOG_INFO)` link up/down；错误处理路径（返回值、`NETDEV_TXERRORS`、`up_disable_irq`、`netdev_carrier_on/off`）一个没动，只是不再打印。历史：v8i 之前是"只保留 link up/down + nerr"。任何再增打印先过 3.6                                                                    |
| 启动 DHCP 重试                                                           | ✅**根因已定位**      | 根因：交换机/路由器端口 STP listening/learning（10~30s）丢弃 BOOTP 广播，PHY 5s 就 up 但 boot DHCP 默认 3 重试（9s）全被吞；且 `netinit_net_bringup()` 的 DHCP 失败路径**静默 return 不打日志**（这就是"开了 DEBUG_ERROR 也没看到 ERROR"的谜底）。`NETUTILS_DHCPC_RETRIES=10`（~30s）覆盖 STP 窗口。曾误把它当 22:34 全卡死的元凶回退过——实际卡死是 NETINIT_MONITOR 锁竞争，重试只是拉长失败窗口 |
| 系统时间 / TLS BADCERT_FUTURE (curl: 60)                                 | ✅**已修复**          | 根因：无电池 RTC，boot 时钟停在 1970，所有证书 notBefore 都"来自未来"。已开 `SYSTEM_NTPC=y`（ntpcstart/stop/status 命令）+ rcS 里 `ntpcstart`（daemon 后台指数退避重试 1s→2s→…→120s，60 次才退；网络一通即同步，之后每 60s 重同步）；netinit 在 DHCP 成功后也会启动（状态锁防重复）。`date` 可验证，强制立即重试用 `ntpcstop`+`ntpcstart` |
| DNS 解析失败 / 无 nameserver (curl: 6 Could not resolve)                 | ✅**已修复**          | 根因：`NETINIT_DNS` 未开，netinit 从不设置 DNS；解析器里有没有 nameserver **完全取决于 DHCP 回包是否带 option 6**——某次 boot 的 ACK 没带（boot DHCP 已成功拿到 IP/网关，DNS 却是空），curl/ping 按名字立即失败。已开 `NETINIT_DNS=y`+`NETINIT_DNSIPADDR=0xc0a80a01`（netinit 预设网关 192.168.10.1 作 DNS 兜底，DHCP 带了就覆盖）；`NETDB_DNSCLIENT_RECV_TIMEOUT` 30→5s（死 nameserver 时解析 ≤15s 而非 90s）。**注意**：boot DHCP 成功后手动再跑 `ifconfig eth0 dhcp`，服务器对重复 DISCOVER 常不应答 → 10×3s=30s 才报错，不是死锁，也不需要再跑 |
| 控制台 Ctrl+C（curl 卡终端）                                             | ✅**已修复**          | 根因：`CONFIG_TTY_SIGINT` 未开，串口 Ctrl+C 无法给前台任务发 SIGINT，NSH 等卡死的 curl 无法打断。已开 `TTY_SIGINT=y`（NSH 前台运行子命令时自动 TIOCSCTTY 绑定子任务 pid）                     |
| curl DNS "getaddrinfo() thread failed to start" (curl: 6)                | ✅**已修复**          | 根因：threaded resolver（`curl_config.h` 的 `USE_THREADS_POSIX`）要起 pthread + 用环回 TCP 手搓 socketpair 自检，任一环失败就报这条误导性错误。已注释掉该宏改走 `CURLRES_SYNCH` 同步解析（getaddrinfo 直接在 curl 任务里跑，真实 EAI_* 错误可见）。apps/external/curl/curl_config.h 属 openvela 自带的移植配置文件，改它不算动上游 curl 源码 |
| TLS 熵源 "CTR_DRBG - entropy source failed" (-0x0034)                    | ✅**已修复**          | 根因：mbedtls NuttX 熵 poll 走 `getrandom()` → `/dev/urandom`，板上没这个设备。已开 `CRYPTO=y`+`CRYPTO_RANDOM_POOL=y`+`DEV_URANDOM=y`+`DEV_URANDOM_RANDOM_POOL=y`（BLAKE2s 熵池 + IRQ 喂熵，不用裸 xorshift128）。注意 `CRYPTO_RANDOM_POOL` 在 `if CRYPTO` 块内，只加它不加 `CRYPTO` 会被 kconfig 静默丢弃；choice 算法切换需显式写 `DEV_URANDOM_RANDOM_POOL=y` |
| TLS CA 证书 "Error reading ca cert file" (curl: 77)                      | ✅**已修复**          | 根因：curl 默认信任库路径 `/etc/ssl/curl/ca-certificates.crt`（`curl_config.h` 的 `CURL_CA_BUNDLE`）在板上是空目录。已把宿主机 ca-certificates 包的 bundle（121 个根证书，178KB）放进板级 romfs `src/etc/ssl/curl/`，并在 `src/CMakeLists.txt` 的 `nuttx_add_romfs(RCRAWS ...)` 里登记（RAW 文件原样打包，romfs 大小 +178KB） |
| curl HTTPS 传完不退出 (Ctrl+C 才"出结果")                                  | ✅**根因修复 (v8c)** | 根因：`external/curl/curl/lib/vtls/mbedtls.c` 的 `mbedtls_close()` 无条件 `mbedtls_ssl_read()` 一次以消费 close_notify——上游 socket 非阻塞立即返回 WANT_READ，NuttX socket 阻塞且 curl 未设 O_NONBLOCK，keep-alive 服务器响应后保持 TLS 会话不发 close_notify → read 永久阻塞：响应体早已收完，easy_cleanup 卡死，stdout 缓冲不刷新，Ctrl+C 终止后才一次性输出（板上 stdl.b4qaq.cn 复现）。修复：`Curl_socket_check(fd,..,0)` 零超时探测，仅当 close_notify 已到达才读。OTA 的 `curl -o` 同样受益（否则 boot.fit 下载完也挂） |
| `rk3506_usbhost.c`                                                       | ✅**根因修复 (v7)**  | v6 首刷 rk3506_usbhost.c:2136 DEBUGASSERT 崩溃的三层根因：① **conn/drvr 接口重叠**——`rk3506_usbhost_s` 整体强转 `usbhost_connection_s`，`conn->wait` 别名到 `drvr->ep0configure`，usbhost_waiter 首次 CONN_WAIT 即带垃圾参数调进 ep0configure 触发断言（崩溃在任何连接事件之前）；② 初始化了错误的控制器——板级 DTS otg0=peripheral（Type-C 烧录口）、otg1=host（USB-A 口），host 必须用 `initialize(1)`；③ INNO USB2 PHY 从 POR 处于 suspend（GRF phy_sus=0x1d1），不唤醒则 UTMI 线态垃圾→幻象连接。修复：conn 独立成员+container_of（上游 efm32 模式）、OTG1+`RK3506_USBHOST_OTG1=y`、`rk3506_usbphy_init()`（内核 inno-usb2 寄存器级序列）、ep0configure clamp/-EINVAL 防御（垃圾枚举不再 panic）、补 `usbhost_trformat1/2`（DEBUG_USB=y 需要）、Kconfig `select USBHOST_HAVE_ASYNCH`（HUB 依赖）、defconfig 开 DEBUG_USB_ERROR（此前 USB 错误日志全静默）。**v8i：DEBUG_USB_INFO/WARN 已关**（用户要求，且它们正是 `ls /mnt/usb` 输出被冲成乱码的原因），保留 DEBUG_USB+ERROR。**已知限制**：defconfig 的 `USBHOST_HUB=y` 此前被 HAVE_ASYNCH=n 静默丢弃，v7 select 后真正生效——但驱动未实现 `drvr->asynch`，hub class（usbhost_hub.c 轮询用 DRVR_ASYNCH）插上 USB 集线器会空指针崩；U 盘/HID 直插走同步 DRVR_TRANSFER 不受影响。若要支持 hub 需给驱动补 asynch 实现（方案过 3.6） |
| `hd_rk3506_gt911.c`                                                      | ⚠️**-110 属预期**    | 2000+ 行。用户确认台架上触摸未接（SDK 里 480×800 ST7701S 面板 dtsi 本就无 gt911 节点），product ID 读 -110 ETIMEDOUT 属预期；接上面板后若仍失败再查复位时序/引脚/地址                                                                                                                                                                                                                                                                 |
| `hd_rk3506_st7701s.c`                                                    | ⚠️**可能有 bug**    | 1300+ 行                                                                                                                                                                               |
| `{etc/init.d}` 空目录（残留）                                            | ❌**已删除**          | 上一个 AI 笔误                                                                                                                                                                         |
| `/data 分区位置错误（潜伏 bug）`                                          | ✅**已修复 (v7 更正)** | 两段历史：① bringup 把擦块索引当 mtd_partition 的页索引传，/data 实际映射 3.7MB 处 384KB（在 uboot 分区内！），已改为页单位传参；② **v6 时页单位修复却把起始扇区写成 0x14800（41MiB，错误记录），实际 v5 parameter 的 userdata 起始是 0x10800（boot_b 结束处，33MiB），/data 因此后移 8MiB 留空洞**，v7 已更正为 0x10800 并同步擦除块注释（block 264，剩约 183MiB）。mtd.h 的 "offset in bytes" 注释是过时的（实际单位=geo.blocksize） |
| `/data 文件系统栈（mount failed: 25）`                                    | ✅**已修复 (v8)** | v7 首刷暴露：SmartFS 绑定块设备要 SMART 层 BIOC_GETFORMAT ioctl（`smartfs_utils.c:199`），该 ioctl 只有 `drivers/mtd/smart.c` 实现；本板栈是 raw NAND MTD→mtd_partition→**dhara**→/dev/mtdblock0（普通块设备），dhara ioctl 全透传给 MTD → ENOTTY(25)。叠加 rcS bug：`mksmartfs /dev/mtdblock0 2048` 第二位置参数是 nrootdirs(1-8)，2048 报错且跳过格式化。**"dhara+SmartFS" 从 v5 起从未真正执行过**。v8 修复（用户拍板方案 B）：dhara 保留（磨损均衡）+ **littlefs** 挂 /data（R258 同构 r528 `nuttx_nand_init.c:2082`；KVDB 只写 /data/kvdb.db 普通文件与 fs 类型无关；littlefs 原生支持块设备 + `-o autoformat` 首启自动格式化）。rcS：`mount -t littlefs -o autoformat /dev/mtdblock0 /data`；defconfig +CONFIG_FS_LITTLEFS=y（FS_SMARTFS 保留但 rcS 不再用）。备选否决记录：A) smart.c+SmartFS 标准 NuttX 但 NAND 上无磨损均衡；C) 自写 block→SMART ioctl shim 违反 3.4.6 |
| U 盘 vfat 配置缺口                                                        | ✅**已修复 (v8)** | v7 defconfig 无 CONFIG_FS_FAT：USB MSC 枚举出 /dev/sda 没问题，但 `mount -t vfat` 因 vfat 类型未注册而失败。v8 加 `CONFIG_FS_FAT=y`+`FAT_LCNAMES`+`FAT_LFN`。**v8e 更正挂载点**：`mount -t vfat /dev/sda /mnt/usb`（不是 /tmp/usb——NuttX mount() 不能在另一挂载点之下建挂载点，/tmp 是 tmpfs 挂载点，fs_mount.c 对 MOUNTPT inode 返回 -ENOTDIR=20，即 v8d "mount failed: 20" 根因；/mnt/usb 在伪根下由 mount 自动创建） |
| `hd_rk3506_rpmsgtest.c` / rpmsg 起不来 | ✅**v8h 板上验收通过（rpmsgtest PASS）** | ①v8 改 poll+超时；v8b 共享窗 cached→uncached。②**v8e 时钟根因**：`rk3506_mbox_init()` 原写 `WE|bit13`（值含 1），Rockchip 门控是 SET_TO_DISABLE（内核 GFLAGS=HIWORD_MASK\|SET_TO_DISABLE；本仓 SPI1 用 `gates<<16` 即数据位=0 才开门）→ 实为**关死 PCLK_MAILBOX**，v8b 起每次 boot A7 自己关掉邮箱时钟，探针全零、kick 全丢。修复：写 0 带 WE 开 PCLK_MAILBOX+PCLK_INTMUX(CON6 b13,14)，mcu_boot 补开 PCLK_UART4+SCLK_UART4(CON11 b8,13)。③**v8f 竞态根因**（v8e 板上数据定位）：时钟修好后 kick 确实落地（A2B_CMD=0x03/DATA=0x524d5347 可回读，CRU CON5/6/11 全 0），且 ~~M0 活着并跑到 rpmsg init（MBOX0 A2B_INTEN=0x101 ⇒ 全是 M0 写的）~~ **该推断已被 v8g 推翻：mailbox 不随 A7 温复位而复位，0x101 是陈旧值，M0 其实从未执行**，但 A2B_STATUS bit0 永久 pending。原因：A7 在释放 M0 后**几微秒**就发 DRIVER_OK kick，M0 还在 HAL/UART/INTMUX/rpmsg init、A2B_INTEN bit0 仍为 0 → STATUS 0→1 边沿在接收中断被屏蔽时发生 → M0 收不到 IRQ、不清状态 → 后续 kick 全被 'A2B busy' 拒（那两条 kick dropped 是**正确行为**）。修复（用户拍板方案 A）：`mcu_boot()` 轮询 MBOX3 A2B_INTEN bit0（M0 独占写入＝天然就绪握手）最多 1s/1ms 一轮，就绪后 w1c 清 A2B_STATUS 保证首发干净边沿；mcu_boot 由 rptun start 调用早于首次 notify，覆盖所有 kick 且不重复投递。**探针判读**：kick pending 时按 A2B_INTEN bit0 细分 —— bit0=0 ⇒ M0 未起来/卡 rpmsg init 前；bit0=1 ⇒ M0 已武装但 INTMUX/NVIC 投递通路断（接 M0 UART4 GPIO1_C2/C3 @1500000）。④**v8g M0 时基缺陷（v8f 板上数据定位；必要修复，但*不是*本现象根因——见 ⑤）**：v8f 报 `M0 mailbox rx armed after 0 ms`——M0 不可能在 0ms 内跑完 init，⇒ `A2B_INTEN bit0=1` 是**上次遗留的陈旧值**（mailbox 不随 A7 温复位而复位），v8f 握手实为空操作，且**我们从来没有任何证据证明 M0 执行过一条指令**。逐项源码核对排除了所有配置嫌疑（A7 序列与 u-boot `fit_standalone_release()` 逐寄存器一致；`test_demo.c` MASTER_ID=0/REMOTE_ID=3 ⇒ link 0x03+mailbox3 正确；工程链接脚本 `hal/project/rk3506-mcu/GCC/gcc_bus_m0.ld` 的 `LINUX_RPMSG ORIGIN=0x03c00000 LENGTH=0x200000` 与我们一致，CMSIS 模板里的 0xa0000000 被工程覆盖属虚警；`amp.its` `load=<0xfff84000>` 一致；blob 内含 `rpmsg-mcu0-test` 字符串 ⇒ RPMSG_LINUX_TEST 确实编入）。**缺陷**：`rk3506-amp.dtsi` 的 `rockchip_amp` 节点列出 A 侧必须替 AMP 核使能的 6 个时钟 `HCLK_M0, STCLK_M0, SCLK_UART4, PCLK_UART4, PCLK_TIMER, CLK_TIMER0_CH5`，Linux 由 `rockchip-amp` 驱动全部 `clk_prepare_enable`，而 **u-boot 只开 HCLK_M0**（CON5 bit10 + bit11=`SWCLKTCK_M0_EN`，见 `rk3506.h:18669-18672`；`clk-rk3506.c` 没导出 bit11 但 header 有明确定义，u-boot 开它有依据）。我们照抄 u-boot ⇒ 漏了 `PCLK_TIMER`(CON6 b2)、`CLK_TIMER0_CH5`(CON6 b8)、`STCLK_M0`(**CON8 b2**，不在 CON5！)。而 M0 的 `hal_conf.h` 里 `SYS_TIMER` 就是 `TIMER5`(=CLK_TIMER0_CH5) 且 `HAL_SYSTICK_MODULE_ENABLED` 打开（SysTick 时钟=STCLK_M0），`main.c` 第一件事就是 `HAL_Init()` ⇒ 时基三时钟全被门控 ⇒ 计数器永不前进 ⇒ **`TimerDelayUs()`(`hal_base.c:133-146`) 会死循环**。修复：`mcu_boot` 在拷贝固件和放行 M0**之前**写 `CON6<-WE(2)|WE(8)`、`CON8<-WE(2)`；并在放行前用 `WE(0)` 清掉 MBOX3/MBOX0 的 A2B_INTEN bit0 使握手不受陈旧值污染。新增**执行金丝雀**：M0 偏移 `0x7000..0x7c00`（3KiB，bss/heap+下行栈，远高于 0x69f0 镜像末尾；SRAM 由 dtsi `mcu_reserved 0xfff80000+0xc000` 减去 0xfff84000 = 32KiB，与链接脚本 RAM=0x8000/SP=0x7c00 吻合）填 `0xa5a5a5a5`，握手超时后回读 —— 全然未动=M0 一条指令都没执行；被脏化=M0 跑过但死在 rpmsg init 之前。**无需接串口即可二分**。**⚠️ v8h 自我更正：逐行走 M0 初始化后，本条解释不了"M0 一条指令没执行"** —— `HAL_Init()` 四步全是纯写寄存器无轮询（`hal_base.c:162-183`）、`HAL_TIMER_SysTimerInit()` 不轮询（`hal_timer.c:87-100`）、`HAL_UART_Init()` 无延时、demo 里第一个 `HAL_DelayMs()` 在 link-up **之后**（`test_demo.c:278-280`）、`STCLK_M0` 只服务 SysTick 而 `main.c` 从不调 `HAL_SystemCoreClockUpdate()`、`HAL_ASSERT` 是空宏。所以 v8g 是**必要修复**（否则 link-up 后第一次 `HAL_DelayMs(1)` 必死循环）但**不是本现象根因**。⑤**v8h 决定性探针 + 握手升级**：(a) mailbox 中断是**电平敏感**（`rk3502.dtsi:803` `IRQ_TYPE_LEVEL_HIGH` + `rockchip-mailbox.c:255-291` 的 IRQ_NONE/末尾 w1c 写法），`A2B_INTEN bit8` 只是 `trigger_method`（哪次写触发）不是边沿/电平选择；真正吃掉早到 kick 的是 `hal_mbox.c:74-84` 的 `MBOX_ChanEnable()` —— 它**先 w1c 清 `A2B_STATUS` 再开 INTEN**，所以 v8f 的握手方向是对的。(b) 电平语义下 `STATUS b0=1` 与 `INTEN b0=1` 长期共存**不可能是丢边沿**，只剩三种可能：M0 零执行 / 死在 `rpmsg_platform.c:178..295` 之间 / 向量表没写进去导致取中断即 HardFault。(c) **握手信号换成 INTMUX `INT_ENABLE_GROUP[3]` bit21**（`0xff2a000c`）：`MAILBOX_BB_3_IRQn`=117+32=149（`soc.h:271`）经 `hal_intmux.c:323-328` 换算＝INTMUX 输入 117＝group3/bit21→`INTMUX_OUT3`→M0 NVIC IRQ31；该位**唯一写者是 M0 的 `rpmsg_platform.c:295`（rpmsg init 最后一步）、A7/u-boot/OP-TEE/Linux 全 SDK 无一处碰 INTMUX、且 POR=0** ⇒ 不可能是陈旧值，比 `A2B_INTEN bit0` 严格更好。实现为两阶段：先等 `A2B_INTEN bit0`(≤1s) 再等 INTMUX bit21(+100ms)，后者超时只 `_warn` 不放弃以防 INTMUX 在 A7 侧读不到而回退。(d) **向量表探针 `0xfff840bc`**（IRQ31 槽）：Cortex-M0 **无 VTOR**，向量表固定在地址 0，`start_rk3506_mcu.S:41` 把 64 个 IRQ 向量留成 `.space (64*4)` 全零，靠 `hal_nvic.c:55` 运行时写 `(uint32_t *)0x0U` 填入 ⇒ 该槽非零即证明 M0 执行过 `SystemInit()→HAL_INTMUX_Init()` **且地址 0 可写**；若不可写则 handler 存储静默失败、首个 IRQ 跳 0（thumb 位=0）直接 HardFault，外部现象与"ISR 从不运行"完全一致。(e) 另加只读探针：`INT_FLAG_GROUP[3]` bit21（`0xff2a008c`，电平是否进了 INTMUX）、softreset `CON00 b10`/`CON05 b10,11`/`CON06 b13,14`、`GRF_PMU MCU_ISO_CON1 b12-15`+`CON3 b3`（全 SDK 无写者，复位值待测）。**三分判读表**：向量槽=0 ⇒ M0 零执行或地址 0 不可写（查 SMC 重映射）；向量槽≠0 且 INTMUX b21=0 ⇒ M0 跑了但死在 `RegisterClient..EnableIRQ` 之间；两者都非 0 而 `FLAG b21=0` ⇒ 电平没进 INTMUX（查 `PCLK_INTMUX`/`PRESETN_INTMUX`/隔离位）。(f) 另确认：`MAILBOX_8MUX1_IRQn`(M0 NVIC #22) 全 SDK 无使用也找不到选择寄存器，别当备用通路；GRF `SOC_CON13` 的路由选择只管 GPIO1 与 TIMER1CH5/DSMC，**不存在 mailbox 中断路由寄存器**（8 根线硬连线）；A 核唯一必配的是 `PMU_INT_MASK_CON(0xff90000c)=0x00060004` 的 `glb_int_mask_mcu=0`，我们已做。端点 rpmsg-mcu0-echo（dst 0x4003；M0 侧 NS 名为 rpmsg-mcu0-test）。**⑥ v8h 板上验收通过**：`rpmsgtest` 输出 `endpoint /dev/rpmsg-rpmsg-mcu0-echo (src 0x30 -> dst 0x4003) created` → 发 `ping from vela` → `reply (26 bytes): "Rockchip rpmsg linux test!"` → **PASS**。**v8g 与 v8h 缺一不可**：v8h 的 INTMUX 握手保证首个 kick 落在 M0 武装之后；v8g 补的时基时钟让 M0 在 link-up 之后 `test_demo.c:278-280` 的 `while (cb_sta != 1) HAL_DelayMs(1)` 不再死循环（这正是 v8h 自我更正时指出的、时基唯一真正致命的位置）。**v8i 已删除全部诊断脚手架**（rpmsgtest 探针块 603→272 行、rptun 的 3 条 _info + 2 条 _warn），只保留 rpmsgtest 的结果输出与 rptun 的 8 条 _err |
| U 盘 MSC bulk 传输挂死                                                    | ✅**双层根因修复 + v8f 板上验收通过** | v8b INFO 跟踪定位三层完成机制问题（v8c 修）：①NAK 重试被 5s 超时误杀；②XFRC 不置位（ACK 判据替代）；③ep0 mask 方向残留（按 eptype+方向现算恒含 CHH）；chan_wait 改 500ms 切片轮询 progress。**v8d 补上能力层真根因：DWC2 RX FIFO 太小**——slave 模式核心发 IN token 前要求 RX FIFO 容纳"通道 max packet+状态字"，HS bulk mps=512B→128 words+~3 状态字≈131 words > 旧 GRXFSIZ 默认 128 → 核心静默拒绝发 token（CHENA=1、HCINT=0、零中断）；mps=8/64 控制传输只需 ~19 words 所以枚举全过——完美解释"枚举过、bulk 死"。修复：Kconfig 三 FIFO 默认 128/96/96→**512/256/224**（=SDK hal_usb_core.c GRXFSIZ=0x200/NPTX 0x100/PTX 0xE0）。**v8f 板上已验收**：`mount -t vfat /dev/sda /mnt/usb` 成功，`ls /mnt/usb` 正确列出 Ventoy 盘内容（grub/、EFI/、tool/、System Volume Information/、$RECYCLE.BIN/、debug.log、ENROLL_THIS_KEY_IN_MOKMANAGER.cer），CBW/CSW 与 512B bulk IN 全部 result=0；500118192 扇区（238GB）几何读取正常。日志里的乱码只是 USB 调试打印与 ls 输出交织，非缺陷 |
| A/B 双分区 OTA                                                            | ✅**代码完成，v7 板上部分验证** | `rk3506_ota.c` /dev/ota（AvbABData @ misc+2048，U-Boot CONFIG_ANDROID_AB 协议）+ `ota` NSH 命令 + rcS bootcheck（try 递减/回滚）+ parameter.txt v5（boot_a/boot_b 双 10MB 槽）。**U-Boot 已重编支持 AB（见 uboot.img v6 行）**，AvbABData 格式/默认值/CRC 大端已与 U-Boot avb_ab_flow.c 源码级核对一致。**分区布局变更，旧镜像不兼容，需全量刷**。v7 板上：bootcheck 烧 try、confirm、status 正常；`ota update` 流程待验（v8 前需 /data 可写——v8 已修）。验收见 `MORNING_CHECKLIST.md` §2 |
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
