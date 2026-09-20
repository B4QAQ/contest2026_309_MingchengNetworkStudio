# KICKPI-K7（RK3576）开发板 — openvela 适配指引

[ [English](README.md) | 简体中文 ]

> ⚠️ **状态：尚未适配**
>
> openvela **尚未** 适配 KICKPI-K7（Rockchip RK3576）开发板。本目录当前
> **不包含任何板级支持代码**，仅汇总下列官方硬件与文档资料，供开发者自行
> 完成适配。
>
> 欢迎提交可用的板级适配 PR。

## 开发板

- **开发板**：KICKPI-K7
- **芯片**：Rockchip **RK3576**

## 参考资料

| 资料 | 链接 |
|------|------|
| 上手指南 / WIKI 文档 | https://doc.kickpi.cn/Products/Beginner-Guide/KICKPI-K7/ |
| 硬件资料 | https://doc.kickpi.cn/Products/Introduction/KICKPI-K7/ |
| 外设接口（CAN 等） | https://doc.kickpi.cn/Products/Peripherals-and-Interfaces/CAN/ |
| K7 RK3576 网盘（百度网盘） | https://pan.baidu.com/s/1cMKQt06pWdxZcsOp1XIvQA?pwd=kpcd （提取码：`kpcd`） |

## 如何贡献适配

1. 先阅读上述硬件资料，了解 RK3576 的内存映射、时钟、引脚复用，以及
   KICKPI-K7 板上引出的外设。
2. 在本目录（`boards/rk3576/kickpi-k7/`）下添加板级支持包：`Kconfig`、
   `include/board.h`、`src/`、`configs/<名称>/defconfig` 等，参照 openvela
   其他 vendor 板子的目录结构。
3. 建议先把串口控制台跑通，再逐步适配存储、显示及其他外设。
4. 将适配以 PR 形式提交到本分支。
