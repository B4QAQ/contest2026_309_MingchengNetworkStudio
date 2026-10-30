/****************************************************************************
 * vendor/rockchip/chips/rk3506/hardware/rk3506_spi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI register definitions for the Rockchip RK3506.
 * Taken from the Linux spi-rockchip.c driver and the RK3506 TRM.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_SPI_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_SPI_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SPI register offsets (from the SPI base) */

#define SPI_REG_CTRLR0           0x00u
#define SPI_REG_CTRLR1           0x04u
#define SPI_REG_ENR              0x08u
#define SPI_REG_SER              0x0cu
#define SPI_REG_BAUDR            0x10u
#define SPI_REG_TXFTLR           0x14u
#define SPI_REG_RXFTLR           0x18u
#define SPI_REG_TXFLR            0x1cu
#define SPI_REG_RXFLR            0x20u
#define SPI_REG_SR               0x24u
#define SPI_REG_IPR              0x28u
#define SPI_REG_IMR              0x2cu
#define SPI_REG_ISR              0x30u
#define SPI_REG_RISR             0x34u
#define SPI_REG_ICR              0x38u
#define SPI_REG_DMACR            0x3cu
#define SPI_REG_DMATDLR          0x40u
#define SPI_REG_DMARDLR          0x44u
#define SPI_REG_TXDR             0x400u
#define SPI_REG_RXDR             0x800u

/* CTRLR0 bits */

#define SPI_CTRLR0_DFS_MASK      0x3u
#define SPI_CTRLR0_DFS_8BIT      0x1u
#define SPI_CTRLR0_DFS_16BIT     0x2u
#define SPI_CTRLR0_FRF_MASK      (0x3u << 4)
#define SPI_CTRLR0_FRF_SPI       (0x0u << 4)
#define SPI_CTRLR0_SCPH          (1u << 6)
#define SPI_CTRLR0_SCPOL         (1u << 7)
#define SPI_CTRLR0_TMOD_MASK     (0x3u << 8)
#define SPI_CTRLR0_TMOD_TXRX     (0x0u << 8)
#define SPI_CTRLR0_TMOD_TX       (0x1u << 8)
#define SPI_CTRLR0_TMOD_RX       (0x2u << 8)
#define SPI_CTRLR0_SSD           (1u << 10)
#define SPI_CTRLR0_EM_BIG        (1u << 12)
#define SPI_CTRLR0_BHT_8BIT      (1u << 13)

/* SR bits */

#define SPI_SR_BUSY              (1u << 0)
#define SPI_SR_TFNF              (1u << 1)
#define SPI_SR_TFE               (1u << 2)
#define SPI_SR_RFNE              (1u << 3)
#define SPI_SR_RFF               (1u << 4)

/* SER bits */

#define SPI_SER_SLAVE(n)         (1u << (n))

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_SPI_H */
