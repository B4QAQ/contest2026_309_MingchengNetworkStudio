# HANDOFF — 交接给下一个 AI

> 生成时间：2026-09-17（v8l 之后，curl 挂死根因定位进行中）
> 配套文件：`AGENTS.md`（项目宪法，必读）、`MORNING_CHECKLIST.md` §14–§16（curl 调查逐日记录）
> 读这份文档能让你零上下文接手。看完再动任何代码。

---

## 0. 三句话现状

1. RK3506G2 / HD-RK3506-EVM 的 NuttX(openvela) 板级移植主干已完成（v8a→v8l），最新固件 v8l 干净构建通过、可打包。
2. **当前头号任务**：`curl` 任何"传输类"命令都在**干活之前**挂死在一个不超时的等待上，必须 Ctrl+C 才继续并正常跑完（exit 0）。根因**未定位，未改任何代码**，小米老师（openvela 官方）在远程指导，下一步实验已列出（见 §1）。
3. **比赛代码尚未推送**：外层比赛仓有 135 个未推送提交；真正的源码在 repo 工具管理的多个子项目里（vendor/rockchip 108 个本地提交等），GitHub 从本机不可达，推送要选手自己做。详见 §2。

---

## 1. 当前任务：curl 挂死根因定位（最高优先级）

### 1.1 现象（板上 100% 复现）

板子：HD-RK3506-EVM，单串口 NSH 115200，扁平构建，网络已通（DHCP/DNS/NTP 正常）。

| 命令 | 结果 |
|---|---|
| `curl --version` | **秒退**，正常 |
| `curl http://`（URL 非法，应秒报错 3） | 报错了，**但要 Ctrl+C 才退出** |
| `curl file:///nonexistent_xyz`（应秒报错 37） | 报错了，**但要 Ctrl+C 才退出** |
| `curl --max-time 10 file:///etc/passwd -o /tmp/test` | **要 Ctrl+C 才返回**（小米老师 2026-09-17 新增证据） |
| `file://` 真实 177KB 文件 | 传完，挂，Ctrl+C 后出全部输出 |
| `http://192.168.10.1/`（无 DNS 无 TLS） | 传完 1770B，挂，Ctrl+C |
| `https://stdl.b4qaq.cn/fwtb/info.json`（115B，keep-alive） | 传完，挂，Ctrl+C；`--max-time 20` 从不触发 |
| `curl -s ...`（全程零输出） | 照挂 |
| 普通按键 | **叫不醒**（只有 Ctrl+C 能） |

Ctrl+C 后：全部该有的输出一次性出现、文件完整、`echo DONE=$?` = **0**。后台挂死时 `ps` 显示 curl：`STATE=Waiting  EVENT=Semaphore`，STACK 1824/8024 (22.7%)，SIGMASK=0。

### 1.2 已钉死的结论（每条都有源码依据，勿重复怀疑）

1. **挂点在第一次 `multi_perform` 之前**，即第一次 `curl_multi_poll` 里：
   - meter 的 Time Spent 列打印 `--:--:--`，而 `lib/progress.c` 的 `time2str` 对 **seconds≤0** 才打这个串 ⇒ 从 `Curl_pgrsStartNow`（`Curl_pretransfer` 末尾，`lib/transfer.c:1415`）到传输结束 <1s ⇒ 挂点在 pretransfer 之前。
   - `-o` 文件是**惰性创建**的（第一个响应字节到达才 fopen：`src/tool_cb_wrt.c:220`、`src/tool_cb_hdr.c:184`）。后台 curl 10 秒后 `/tmp/info3.json` 不存在（stat failed:2）⇒ 连响应都没收到。
   - "`-v` 输出憋到 Ctrl+C"**不是缓冲问题**：`-s`（零输出）照挂；真相是**输出当时还没产生**——Ctrl+C 踢醒后 0.3s 跑完全程，输出全是踢醒之后打的。
2. **挂点唯一候选 = 第一次 `Curl_wait_ms(1000)` 的 `select(0,…,1s)` 没有按超时返回**。源码链（curl 8.4.0-DEV，原版，路径 `openvela/external/curl/curl/`）：
   - tool 主循环：`src/tool_operate.c:2549` 附近 `while(!mcode && (still_running||more_transfers))`，先 `curl_multi_poll(multi,NULL,0,1000,NULL)` 再 perform。
   - `lib/multi.c` `multi_wait()`（1156–1453）：首次轮询时 easy handle 在 MSTATE_INIT、无 socket（curlfds=0），wakeup_pair 失败（见下），故 nfds=0，`if(nfds)` 的 `Curl_poll` 块被跳过（1323 行非 Winsock 分支）；尾部（**1437–1452**）：
     ```c
     if(extrawait && !nfds) {
       long sleep_ms = 0;
       if(!curl_multi_timeout(multi, &sleep_ms) && sleep_ms) {
         if(sleep_ms > timeout_ms) sleep_ms = timeout_ms;
         else if(sleep_ms < 0)    sleep_ms = timeout_ms; /* 无内部定时器 →1000 */
         Curl_wait_ms(sleep_ms);
     }}
     ```
   - `lib/select.c:67 Curl_wait_ms()`：`HAVE_POLL_FINE` 未定义（`apps/external/curl/curl_config.h:408`），走 `select(0, NULL,NULL,NULL, {1s})`；EINTR 被吞成返回 0（这就是 Ctrl+C 能放行的原因）。
   - **wakeup_pair 必失败**：`ENABLE_WAKEUP` 有定义，`lib/multi.c:430 wakeup_create()` 调 `Curl_socketpair(AF_UNIX,…)`（`HAVE_SOCKETPAIR=1`，走真 socketpair）；板子 `# CONFIG_NET_LOCAL is not set`，`nuttx/net/socket/socketpair.c:62 psock_socketpair()` 对 AF_UNIX 返回 `-EAFNOSUPPORT` ⇒ pair=BAD，nfds 不含它。
   - NuttX 侧：`select(0,…)` → `nuttx/fs/vfs/fs_select.c:218 poll(pollset, npfds=0, msec=1000)` → `nuttx/fs/vfs/fs_poll.c:416 poll()` → `poll_setup(0 fd)` count=0 → **495 行 `nxsem_tickwait(sem, MSEC2TICK(1000))`**。
   - `nuttx/sched/semaphore/sem_tickwait.c nxsem_tickwait_slow()`：`wd_start(&rtcb->waitdog, delay, nxsem_timeout, rtcb)` + `nxsem_wait_slow(sem)`；超时应由看门狗在 tick 中断里 `nxsem_timeout` 投信号量唤醒，返回 -ETIMEDOUT→OK。
3. **纸面全对，板子不超时**：`CONFIG_USEC_PER_TICK=1000`（1ms tick），无 TICKLESS。`sleep 1`/`sleep 10` 在板上一直正常（实验里天天用），说明看门狗/tick 大体正常。**这正是最大的矛盾点**：同样靠 wd_start 唤醒，nanosleep 醒、poll 的 nxsem_tickwait 不醒。
4. 已排除（勿再走回头路）：DNS（file:// 无 DNS 也挂；且挂点在 resolve 的 infof 之前）、TLS/mbedtls、keep-alive、socket 非阻塞（cf-socket.c:1079 无条件 nonblock，FIONBIO 路径通）、控制台写阻塞（`-s` 反证；`help -v`/`--version` 串口输出正常，小米老师也据此排除 UART TX）、tmpfs/fclose（`tool_operate.c:649` fclose 有返回值检查，失败会报 (23)，DONE=0）、curl 自旋锁（atomic，不占信号量）、pthread_mutex（NuttX 对 EINTR 重试，SIGINT 踢不醒；待复核但可能性低）、堆锁（其他任务健康、IDLE Ready）、UART 驱动 TX 中断（同上反证）。
5. 小米老师明确指示：**先不要恢复 mbedTLS close 补丁，不要关 keep-alive/降 HTTP1.0 绕过**。另一 AI 早先提的 FORBID_REUSE/HTTP1.0 与"改 tcp_*.c POLLHUP"方案均已否决（见 AGENTS.md curl 行）。
6. curl 源码必须保持 pristine（`external/curl/curl` @ `8c2a01f3e`，"我什么时候说过要删 curl"）；可改的只有 openvela 胶水 `apps/external/curl/curl_config.h`（=`external/curl/curl_config.h`，已在 external 项目提交 72adf42）。

### 1.3 下一步实验（按顺序，前 3 个零成本）

**A. 小米老师指定，待选手跑（还没回报结果）：**
```
nsh> date
nsh> sleep 1
nsh> date
```
验证系统定时器（预期正常；sleep 一直可用）。

**B. file:// 超时测试 —— 已做：照挂**（`--max-time 10` 10 秒不触发，需 Ctrl+C）。这条本身就强力支持"第一次等待不超时"。

**C. 重定向到文件，区分"启动前挂"vs"退出路径挂"（待跑）：**
```
nsh> curl -v --max-time 10 https://stdl.b4qaq.cn/fwtb/info.json > /tmp/body 2> /tmp/trace
nsh> ls -l /tmp/body /tmp/trace
```
（NSH 重定向在任务启动前就创建文件，所以看**大小**：body=115、trace 完整 ⇒ 退出路径挂；body=0、trace=0 ⇒ 启动前挂。按现有证据预测：两者都 0 字节/极小。）

**D. 最小 poll 超时测试（小米老师建议，需要出一个临时诊断固件）：**
在**板级自己的目录**加一个 NSH 命令（仿照现有 `rpmsgtest`/`ota` 的 `nuttx_add_application` 注册，`vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/CMakeLists.txt` 有现成模板），建议命名 `timetest`，一次测全：
```c
/* 1 */ poll(NULL, 0, 1000);          /* 怀疑对象：预期 1s 返回，若永久挂=根因在 NuttX poll 超时链 */
/* 2 */ select(0, NULL,NULL,NULL,&tv);/* Curl_wait_ms 实际调的，同上 */
/* 3 */ nanosleep(&ts, NULL);         /* 对照组：板上已知正常 */
/* 4 */ sem_timedwait / pthread_cond_timedwait 各 1s（如方便，区分是否只有 waitdog 路径坏） */
```
每步前后打 tick 计数，直接显示实际耗时。

**E. 挂死现场抓调用栈（强烈建议和 D 合一个固件）：**
- defconfig 加 `CONFIG_SCHED_BACKTRACE=y`（ARM EHABI unwinder，默认选中，不需 frame pointer；之前 dataabort 的栈就是它解的）。
- 板级新增 `bt <pid>` 命令，函数体就一句 `sched_dumpstack(pid);`（声明在 `nuttx/include/sched.h:278`，被 SCHED_BACKTRACE 门控）。
- 用法：`curl ... &; sleep 3; ps; bt <curl的pid>` → 直接打出卡在哪个函数，一锤定音。
- 纯新增、在我们自己的板级目录，不碰 curl/上游；属临时诊断手段，**动手前按 AGENTS §3.6 向选手确认**（我已提过方案，选手当时转去问小米老师，尚未点头）。

### 1.4 如果 D 正常返回（poll 1s 能醒）怎么办

说明挂点不是 poll 超时，而是另一个信号量等待——此时 **E 的 `bt <pid>` 栈就是唯一路径**，别再静态猜。拿到栈后按子系统（VFS/串口/堆/pthread/任务退出）定位。注意：`curl http://` 这种"错误在第一次 multi_perform 内、出错前唯一经过的等待就是首次 multi_poll"的事实，使 poll 路径仍是概率最大的候选；若 D 正常，重点复查 multi_wait 的 timeout 计算在真实二进制里是否被优化/配置改变（必要时反汇编 vela.bin 里的 multi_wait）。

### 1.5 修复约束（找到根因后）

- 若根因在 NuttX 核心（poll/sem/看门狗/timer）：**先给选手 (a) 备选 (b) 代价/风险 (c) 推荐，过 §3.6 才能改**；改动尽量限制在 `vendor/rockchip/`（板级/芯片级），不碰 arch/、nuttx 通用代码（除非必要且单独提交）。
- 不许通过改 curl、关 keep-alive、加 close 补丁"绕过去"。
- 改完走 AGENTS §3.5：清空构建→全量构建→vela.bin/update.img→pack→板上复验 curl 真实 URL 秒退 + rpmsgtest 等回归。
- 顺手验证 v8j M0 停核的板子表现（见 §4 待办）。

---

## 2. 比赛仓库提交（选手已开口要求，**尚未执行，等他确认**）

### 2.1 规则要点（`README.md` + `openvela/docs/zh-cn/contest_2026/code_submission_guide.md`）

- 比赛只认 **GitHub**（Gitee 只是镜像源）；截止 **9 月 20 日**，之后收 push 权限。
- 自己的作品 → fork 自己的专属仓 → 推分支 → PR → **可自行 review 合入**；首次 PR 要先在 openvela 官网签 CLA，PR 里评论 `/check-cla` 复检。
- 改了公共仓（nuttx/vendor/apps/external 等）→ fork 对应公共仓，PR 到其 `dev-ai-contest-2026` 分支（组委会 review）。
- AI Coding 日志要**主动导出**到外层仓 `logs/B4QAQ/` 再提交（当前只有 7 月的 mimocode 日志，8–9 月 DSH 会话——也就是主力开发过程——**一份都没导**；用 `contest-log-collector` skill 导出）。
- 提交前 README.md 要从模板换成作品说明（现在还是模板，同内容另存了一份 README.contest.md）。

### 2.2 仓库实际地图（已核实）

外层 `/home/b4qaq/project/` 就是比赛仓（git 根，分支 `dev-ai-contest-2026`）：
- origin = `github.com/B4QAQ/contest2026_309_MingchengNetworkStudio`（选手 fork）
- upstream = `github.com/open-vela/contest2026_309_...`
- **135 个提交未推送**（origin 缓存停在 `19af3b7` 初始提交）：内容是各版 `doc:` 记录 + `pack:` 刷机包（update.img≈9.8MB、nuttx.elf/bin、boot.fit/img/uimg、MiniLoaderAll.bin，路径 `openvela/nand_firmware/`），以及 31 个 repo 项目的 gitlink SHA。
- `openvela/` 是 repo 工具工作区（openvela 根目录**没有** .git；各项目在 `openvela/.repo/projects/`，工作目录里是 `.git` 符号链接）。外层仓用 gitlink（160000，无 .gitmodules）钉住各项目 SHA。
- 未跟踪垃圾目录（**不要 add**，.gitignore 也没盖它们）：`.dsh/`、`JsFeatureDocs/`、`VelaDocs/`、`Vela_Application_Documentation-main/`、`luoxe_docs/`、`veladocsOffial/`。

各子项目本地独有工作（全部只在本机，remote 只有 gitee 只读镜像 `https://gitee.com/open-vela/<proj>`，manifest 相对路径在 GitHub 下解析为 `github.com/open-vela/<proj>`）：

| 项目路径 | 状态 | 本地工作 |
|---|---|---|
| `openvela/vendor/rockchip` | 分支 `openvela-rk3506-scripting`，**领先 openvela/dev-ai-contest-2026 共 108 个提交**（基线 27886b4，首个本地提交 c83a2be BSP 初版） | **全部芯片+板级驱动**，比赛核心作品 |
| `openvela/nuttx` | **detached HEAD** @ `bc7e01ef`，12 个本地提交 | 净改动实际只有两处：`arch/arm/Kconfig` 的 `ARCH_CHIP_RK3506` 选择块（771b9f9，必需保留）+ `mtd/dhara` 补 `<nuttx/mutex.h>`（bc7e01ef）；中间 9 个是 GIC/timer/TZ/.Lvstart 实验，已被 458f984 回退（历史可 squash） |
| `openvela/external` | 分支 dev-ai-contest-2026，1 个本地提交 `72adf42`（curl_config 关线程 DNS→SYNCH） | 外层 gitlink 已钉 72adf42 |
| `openvela/apps` | 0 本地提交；`examples/lvgl_homepage/`（28K，LCD demo，defconfig 引用）**未跟踪** | 需决定：提交到 apps fork，或挪到外层仓 `app/` 走 linkfile |
| `openvela/vendor`（不含 rockchip） | gitlink 432dfd2，无本地改动 | — |

注意：`vendor/rockchip` 是嵌套在 `vendor` 项目里的独立项目（`.repo/projects/vendor/rockchip.git`），外层比赛仓的 gitlink **只钉了 `openvela/vendor`，钉不到 rockchip**；它由 `openvela.xml:228 <project path="vendor/rockchip" name="vendor_rockchip"/>` 控制，评委 repo sync 默认拉 open-vela 上游——**不推送 fork 并改 manifest，评委拿不到源码，只能拿到我们打包的二进制**。

### 2.3 建议的提交方案（过选手确认后再做）

1. **本机准备**（我能做）：补齐各项目提交（lvgl_homepage 归属先问选手）；如需，把 nuttx 12 提交压成 2 个干净提交（另起分支，不动 detached 现场）；导出 8–9 月 DSH 日志到 `logs/B4QAQ/`；按 README §6 起草作品 README（选手审定）。
2. **GitHub 侧**（网络不可达，需选手自己在有网环境做，或给可用代理）：
   - fork：`vendor_rockchip`、`nuttx`、`external`（如保持单提交也可直接 PR）、必要时 `nuttx-apps`；分支推到自己 fork；
   - 向各公共仓 `dev-ai-contest-2026` 发 PR；
   - 改本队 manifest（`contest2026_309_...xml` 里 override vendor/rockchip 等项目指向自己 fork 的分支），保证评委 repo sync 可复现构建；
   - 外层仓：推 135 提交 + 新增日志/README → 发 PR → 签 CLA → `/check-cla` → 自行合入。
3. 选手上次说"提交前让我确认一遍"——**任何 push/PR 动作前把清单给他过目**。

---

## 3. 工程环境与铁律（摘要，全文看 AGENTS.md）

- 工作目录：`/home/b4qaq/project`；主代码 `openvela/`；SDK 只读参考 `RK3506G2/rk3506_linux6.1_sdk_v1.2.0_iot_evm/`；R258 参考 `openvela/vendor/allwinnertech/`；文档 `OpenVelaDocs/`、`openvela/docs/`。
- 构建（必须照抄，别直接 cmake）：
  ```bash
  cd /home/b4qaq/project/openvela
  rm -rf cmake_out/hd-rk3506-evm_nsh
  CCACHE_DIR=/tmp/ccache_dir \
  PATH="$(pwd)/prebuilts/build-tools/linux-x86_64/bin:$(pwd)/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
  ./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j$(nproc)
  ```
  打包：`bash nand_firmware/pack_nand.sh`（产物 `nand_firmware/update.img`）。不要把构建输出 pipe 给 tail。
- 提交：每个子任务一个独立 commit，中文信息用 `git commit -F /tmp/file`；外层仓与各 repo 项目分开提交；scope 用 chip/board/build/pack/doc/fix/test。
- 驱动事实基准只允许两处：Linux SDK（寄存器/HAL/时钟/pinctrl）+ R258（NuttX 集成套路）；两处都没有的东西**停下来问**（§3.4.6），不许发明。
- 日志级别用 `_err/_warn/_info`，禁用 `ierr/iinfo`（那是 input 子系统宏）。已按选手要求删日志：USB（留 ERROR）、GMAC0（全静默）、rpmsg（留 err）、iomux（v8k commit 25f2aed）、fspi（v8l commit 9d60ef5）。
- 沙箱：workspace-write，需要更多权限会弹审批。

### 版本/产物索引

- 最新固件 **v8l**：update.img md5 `f915312cb69a9717be9ae2076184bc1e`（v8j=`45982493…`，v8k=`699c8695…`）。
- nxstyle 警告基线：rptun=15、gmac0=47、rpmsgtest=4、iomux 24、fspi 143；全局 v8l=50。
- 关键 vendor 提交（新→旧）：9d60ef5 fspi 删日志 / 25f2aed iomux 删日志 / b544287 NSH 256/16 / 26822bb M0 停核（rk3506_mcu_hold 两级停核+金丝雀）/ 9f3f034 rpmsg 探针拆除。完整 108 条在 `git -C openvela/vendor/rockchip log`。

## 4. 其他待办 / 开放项

1. **v8j M0 停核板上复验（重要）**：首刷不应出现 `ERROR: M0 still running with HRESETN_M0 asserted`；反复温复位不再概率性 arm_dataabort；`rpmsgtest` 仍 PASS（v8h 曾 PASS）。v8j 之后板子主要在测 curl，这项还没正式回归。
2. `rk3506_spinand_fspi.c:894` 还有一条 MTD 层 `syslog(LOG_INFO, "FSPI: SPI NAND READ_ID …")`，选手没点名，删前要问。
3. `nuttx/drivers/usbhost/usbhost_storage.c` 6 条既有 `-Wformat` 警告（DEBUG_USB_INFO 关后暴露的死代码），修上游还是留着——待选手定。
4. USB hub：`rk3506_usbhost.c` 未实现 `asynch`，插集线器会空指针崩（直插 U 盘正常，v8f 验收过 238GB FAT32）；补 asynch 要过 §3.6。
5. st7701s/gt911：台架面板/触摸未接，gt911 读 ID -110 属预期；接上后再验收（st7701s 1300 行可能有 bug）。
6. OTA：`ota` 命令 + A/B 逻辑代码完成、bootcheck 部分验证；`ota update` 端到端待验（/data littlefs 已就绪）。
7. curl 根因结案后：如果用到 `bt`/`timetest` 诊断命令，去留问选手。
8. 比赛提交清单见 §2（日志导出、README 重写、fork/PR/manifest、CLA、9/20 截止）。

## 5. 选手协作风格（重要）

- 中文、极简、零容忍偷工减料/猜测/擅自删除或替换；curl 绝不能动、绝不能删。
- 每个假设被推翻要**明确写"自我更正"并记录**（MORNING_CHECKLIST 里有大量范例），他会逐条核对。
- 方案级决定先给选项+代价+推荐，等他拍板；他会跑板上实验、回贴结果，响应很快。
- 他现在背后还有小米老师的官方意见，官方建议与本文档结论一致（timer/poll 唤醒方向），按 §1.3 推进即可。
