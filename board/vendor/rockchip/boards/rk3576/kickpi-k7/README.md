# KICKPI-K7 (RK3576) Board — openvela Porting Guide

[ English | [简体中文](README_zh-cn.md) ]

> ⚠️ **Status: NOT yet ported**
>
> openvela has **not** been ported to the KICKPI-K7 (Rockchip RK3576) board
> yet. This directory currently contains **no board support code** — it only
> collects the official hardware and documentation references below so that
> developers can carry out the port themselves.
>
> Contributions of a working board port are welcome.

## Board

- **Board**: KICKPI-K7
- **SoC**: Rockchip **RK3576**

## Reference Material

| Resource | Link |
|----------|------|
| Getting-started / WIKI guide | https://doc.kickpi.cn/Products/Beginner-Guide/KICKPI-K7/ |
| Hardware documentation | https://doc.kickpi.cn/Products/Introduction/KICKPI-K7/ |
| Peripherals & interfaces (CAN, etc.) | https://doc.kickpi.cn/Products/Peripherals-and-Interfaces/CAN/ |
| K7 RK3576 netdisk (Baidu Pan) | https://pan.baidu.com/s/1cMKQt06pWdxZcsOp1XIvQA?pwd=kpcd (extraction code: `kpcd`) |

## How to Contribute a Port

1. Study the hardware docs above to understand the RK3576 memory map, clocks,
   pinmux and the on-board peripherals exposed by the KICKPI-K7.
2. Add the board support package under this directory
   (`boards/rk3576/kickpi-k7/`): `Kconfig`, `include/board.h`, `src/`,
   `configs/<name>/defconfig`, etc., following the layout of other vendor
   boards in openvela.
3. Bring up the serial console first, then storage, display and other
   peripherals incrementally.
4. Submit the port as a pull request to this branch.
