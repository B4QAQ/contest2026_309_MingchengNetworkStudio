/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_fspi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip RK3506 FSPI (rockchip,fspi) serial-flash controller.
 *
 * The on-board SPI NAND (GD5F family) is wired to the dedicated FSPI
 * controller at 0xFF488000 (IRQ85), chip select 0, 1-bit command /
 * 4-bit data (verified in rk3506-u-boot.dtsi: &fspi -> spi_nand).
 * FSPI is a serial-flash command engine (CMD/ADDR/DATA phase model,
 * Linux spi-mem op style), not a byte-FIFO SPI host.
 *
 * This header exposes a minimal 1-1-1 (single-bit) command-mode bring-up:
 * controller init plus a READ_ID probe, enough to validate clocks,
 * pinmux and the register sequence against the SDK HAL before adding
 * quad/DLL and a full spi-nand MTD.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_FSPI_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_FSPI_H

#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_fspi_nand_op
 *
 * Description:
 *   Execute one SPI-NAND style command transaction on FSPI CS0 in
 *   single-bit (1-1-1) mode: one command opcode, an optional address
 *   phase (1..5 bytes), optional dummy cycles (buswidth 1), and an
 *   optional data phase.  This is a thin wrapper over the internal
 *   spi-mem-style op builder; it exists so the SPI NAND MTD driver
 *   (rk3506_spinand_fspi.c) can drive command sequences that require
 *   CMD+ADDR+DUMMY+DATA inside a single chip-select window, which a
 *   byte-FIFO emulation cannot provide.
 *
 * Input Parameters:
 *   cmd          - Command opcode byte.
 *   addr_nbytes  - Address phase size in bytes (0..5; 1 for feature
 *                  registers, 2 for column addresses, 3 for row
 *                  addresses).
 *   addr         - Address value (LSB first on the wire per SFC).
 *   dummy_nbytes - Number of dummy bytes (1 for the 0x0b fast read).
 *   write        - true: data phase writes to the device; false: reads.
 *   len          - Data phase length in bytes (0 = no data phase).
 *   buf          - Data buffer (in for read, out for write).
 *
 * Returned Value:
 *   0 on success, negative errno on failure.
 *
 ****************************************************************************/

int rk3506_fspi_nand_op(uint8_t cmd, uint8_t addr_nbytes, uint32_t addr,
                        uint8_t dummy_nbytes, bool write, uint32_t len,
                        FAR uint8_t *buf);

/****************************************************************************
 * Name: rk3506_fspi_hw_init
 *
 * Description:
 *   Reset and initialise the FSPI controller for command-mode PIO
 *   transfers.  Callers must enable the FSPI clock
 *   (rk3506_fspi_clock_init) and pin mux (rk3506_ioc_fspi_setup) first.
 *
 ****************************************************************************/

void rk3506_fspi_hw_init(void);

/****************************************************************************
 * Name: rk3506_fspi_read_id
 *
 * Description:
 *   Run the standard SPI NAND READ_ID operation (opcode 0x9F, one dummy
 *   address byte, then 4 ID bytes, all single-bit) on CS0 and return the
 *   raw ID bytes.  Returns 0 on success (4 bytes clocked out within the
 *   bounded timeouts), negated errno on a timeout/bus error.
 *
 *   For a GigaDevice GD5F part the manufacturer ID byte 0xC8 appears in
 *   the returned buffer (the SDK HAL prints idByte[0..2]).
 *
 ****************************************************************************/

int rk3506_fspi_read_id(uint8_t id[4]);

/****************************************************************************
 * Name: rk3506_fspi_probe
 *
 * Description:
 *   Full bring-up: hardware init, then sweep the FSPI DLL delay cells
 *   to find the valid sample window for the on-board SPI NAND (mirrors
 *   rockchip_sfc_delay_lines_tuning from the Linux kernel), then
 *   re-issue the READ_ID with the best cells locked in.  Returns the
 *   4-byte JEDEC ID (id[0] should be 0xC8 for a GigaDevice GD5F).
 *
 ****************************************************************************/

int rk3506_fspi_probe(uint8_t id[4]);

/****************************************************************************
 * Name: rk3506_fspi_set_sclk_x2_bypass
 *
 * Description:
 *   Tell the driver whether the DTS declares rockchip,sclk-x2-bypass.
 *   On RK3506 it does NOT, so the default is false (x2 path, controller
 *   halves SCLK_FSPI internally to produce sclk_out).
 *
 ****************************************************************************/

void rk3506_fspi_set_sclk_x2_bypass(bool enable);

/****************************************************************************
 * Name: rk3506_fspi_set_max_dll_cells
 *
 * Description:
 *   Override the DLL max cell count (from DTS rockchip,max-dll).
 *   RK3506 DTS sets this to 0x17F (383).
 *
 ****************************************************************************/

void rk3506_fspi_set_max_dll_cells(uint16_t cells);

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_FSPI_H */
