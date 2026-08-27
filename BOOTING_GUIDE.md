# HD-RK3506-EVM 启动说明（自动进入 NSH，零交互）

> **已解决**：新镜像的 boot 分区是 **FIT 镜像**，U-Boot 默认 bootcmd 的 `boot_fit` 能直接识别并自动引导 NuttX。
> **烧录后上电即自动进入 NSH，不需要在 U-Boot 里输入任何命令。**

---

## 1. 烧录（一条命令）

```bash
sudo upgrade_tool uf /home/b4qaq/project/openvela/nand_firmware/update.img
```

## 2. 上电 → 自动进 NSH

上电后串口会看到：

```
DDR Version V1.06 ...
U-Boot 2018.09 ...
Hit any key to stop autoboot('CTRL+C'): 0
## Booting FIT Image ...
   Loading Kernel Image ... OK
Starting kernel ...

NuttX (with NuttX RTOS)
nsh>
```

**什么都不用按，什么都不用输。**

---

## 3. 原理（为什么能自动）

U-Boot 默认 `bootcmd = "boot_fit;boot_android ..."`（`CONFIG_BOOTDELAY=0`，上电立即执行）。

`boot_fit` 从 boot 分区开头读 FDT，要求：
1. FDT magic `0xd00dfeed`
2. FDT 结构 < 4KB（`fit_is_ext_type`）
3. 内核数据用 **external data** 形式追加在 FDT 之后（`mkimage -E`）

新 `update.img` 的 boot 分区就是按 Rockchip Linux SDK 打包内核的方式做的 FIT 镜像（`pack_nand.sh` 自动生成 `boot.fit`）：
- FDT 结构 1KB，magic `0xd00dfeed`
- kernel 节点：`load=0x02080000`，`entry=0x02080560`（真实地址，非占位符）
- 内核数据 external，在文件偏移 `0x1000` 处

`boot_fit` 成功后走 bootm 的 v7-A Linux 引导路径：
- 把内核加载到 `0x02080000`
- 跳到 `entry=0x02080560`，该地址 bit0=0 → **ARM 模式**跳转
  - 注：`go` 命令和 v7M bootm 路径会 `entry | 1` 强制 Thumb，但 RK3506 是 Cortex-A7（v7-A，`CONFIG_CPU_V7M` 未定义），走标准 v7-A 路径，不置 Thumb 位，与 NuttX 的 ARM 入口匹配。

→ NuttX 启动 → `nsh>`。

---

## 4. 改完代码后重新烧

改 NuttX 代码后，重新打包：

```bash
cd /home/b4qaq/project/openvela
CCACHE_DIR=/tmp/ccache_dir \
PATH="$PWD/prebuilts/build-tools/linux-x86_64/bin:$PWD/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH" \
./build.sh vendor/rockchip/boards/rk3506/hd-rk3506-evm/configs/nsh/ --cmake -j$(nproc)

bash nand_firmware/pack_nand.sh
```

然后只烧 boot 分区（快）：

```bash
sudo upgrade_tool di boot /home/b4qaq/project/openvela/nand_firmware/boot.img
sudo reset   # 或给板子重新上电
```

`boot.img` 现在也是 FIT 格式（开头 `d00d feed`），单分区烧也能自动引导。

---

## 5. 如果上电还是停在 `=>`

说明 `boot_fit` 没认出 FIT。排查（此时才需要手动）：

```bash
=> mtd read spi-nand0 0x02080000 0x2B00000 0x280000
=> md 0x02080000 4          # 应看到 d00d feed (FIT magic)
=> bootm 0x02080000         # 手动 FIT 引导
```

- 若 `md` 看到 `d00d feed` → FIT 已在，`boot_fit` 应自动成功
- 若看到 `18f0 9fe5`（raw reset vector）→ 烧的还是旧 raw 镜像，重烧 `update.img`

---

## 6. 产物速查

| 文件 | 说明 |
|------|------|
| `nand_firmware/update.img` | 全量固件（FIT boot，自动进 NSH）|
| `nand_firmware/boot.img` | boot 分区镜像 = FIT（`di boot` 用）|
| `nand_firmware/boot.fit` | 同上的 FIT 中间产物 |
| `nand_firmware/nuttx.its` | FIT 源描述（mkimage -E 输入）|
| `nand_firmware/MiniLoaderAll.bin` | SPL/loader（chip tag 350F）|
| `nand_firmware/uboot.img` | U-Boot FIT |

烧录：全量 `uf update.img`；调试内核只 `di boot boot.img`。
