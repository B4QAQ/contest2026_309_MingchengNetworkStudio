/****************************************************************************
 * vendor/rockchip/chips/rk3506/include/irq.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_INCLUDE_IRQ_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_INCLUDE_IRQ_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3506G2 GIC SPI (Shared Peripheral Interrupt) numbers.
 * GIC SPI base = 32 (IRQ 0-31 are SGI/PPI, reserved)
 * SPI number + 32 = GIC IRQ number
 */

#define RK3506_IRQ_SPI_BASE      32

/* GPIO Interrupts (SPI 0-19) */

#define RK3506_IRQ_GPIO0_0       (RK3506_IRQ_SPI_BASE + 0)
#define RK3506_IRQ_GPIO0_1       (RK3506_IRQ_SPI_BASE + 1)
#define RK3506_IRQ_GPIO0_2       (RK3506_IRQ_SPI_BASE + 2)
#define RK3506_IRQ_GPIO0_3       (RK3506_IRQ_SPI_BASE + 3)
#define RK3506_IRQ_GPIO1_0       (RK3506_IRQ_SPI_BASE + 4)
#define RK3506_IRQ_GPIO1_1       (RK3506_IRQ_SPI_BASE + 5)
#define RK3506_IRQ_GPIO1_2       (RK3506_IRQ_SPI_BASE + 6)
#define RK3506_IRQ_GPIO1_3       (RK3506_IRQ_SPI_BASE + 7)
#define RK3506_IRQ_GPIO2_0       (RK3506_IRQ_SPI_BASE + 8)
#define RK3506_IRQ_GPIO2_1       (RK3506_IRQ_SPI_BASE + 9)
#define RK3506_IRQ_GPIO2_2       (RK3506_IRQ_SPI_BASE + 10)
#define RK3506_IRQ_GPIO2_3       (RK3506_IRQ_SPI_BASE + 11)
#define RK3506_IRQ_GPIO3_0       (RK3506_IRQ_SPI_BASE + 12)
#define RK3506_IRQ_GPIO3_1       (RK3506_IRQ_SPI_BASE + 13)
#define RK3506_IRQ_GPIO3_2       (RK3506_IRQ_SPI_BASE + 14)
#define RK3506_IRQ_GPIO3_3       (RK3506_IRQ_SPI_BASE + 15)
#define RK3506_IRQ_GPIO4_0       (RK3506_IRQ_SPI_BASE + 16)
#define RK3506_IRQ_GPIO4_1       (RK3506_IRQ_SPI_BASE + 17)
#define RK3506_IRQ_GPIO4_2       (RK3506_IRQ_SPI_BASE + 18)
#define RK3506_IRQ_GPIO4_3       (RK3506_IRQ_SPI_BASE + 19)

/* PWM Interrupts (SPI 22-33) */

#define RK3506_IRQ_PWM0_4CH_0    (RK3506_IRQ_SPI_BASE + 22)
#define RK3506_IRQ_PWM0_4CH_1    (RK3506_IRQ_SPI_BASE + 23)
#define RK3506_IRQ_PWM0_4CH_2    (RK3506_IRQ_SPI_BASE + 24)
#define RK3506_IRQ_PWM0_4CH_3    (RK3506_IRQ_SPI_BASE + 25)
#define RK3506_IRQ_PWM1_8CH_0    (RK3506_IRQ_SPI_BASE + 26)
#define RK3506_IRQ_PWM1_8CH_1    (RK3506_IRQ_SPI_BASE + 27)
#define RK3506_IRQ_PWM1_8CH_2    (RK3506_IRQ_SPI_BASE + 28)
#define RK3506_IRQ_PWM1_8CH_3    (RK3506_IRQ_SPI_BASE + 29)
#define RK3506_IRQ_PWM1_8CH_4    (RK3506_IRQ_SPI_BASE + 30)
#define RK3506_IRQ_PWM1_8CH_5    (RK3506_IRQ_SPI_BASE + 31)
#define RK3506_IRQ_PWM1_8CH_6    (RK3506_IRQ_SPI_BASE + 32)
#define RK3506_IRQ_PWM1_8CH_7    (RK3506_IRQ_SPI_BASE + 33)

/* UART Interrupts (SPI 34-39) */

#define RK3506_IRQ_UART0         (RK3506_IRQ_SPI_BASE + 34)  /* 66 */
#define RK3506_IRQ_UART1         (RK3506_IRQ_SPI_BASE + 35)  /* 67 */
#define RK3506_IRQ_UART2         (RK3506_IRQ_SPI_BASE + 36)  /* 68 */
#define RK3506_IRQ_UART3         (RK3506_IRQ_SPI_BASE + 37)  /* 69 */
#define RK3506_IRQ_UART4         (RK3506_IRQ_SPI_BASE + 38)  /* 70 */
#define RK3506_IRQ_UART5         (RK3506_IRQ_SPI_BASE + 39)  /* 71 */

/* I2C Interrupts (SPI 40-42) */

#define RK3506_IRQ_I2C0          (RK3506_IRQ_SPI_BASE + 40)  /* 72 */
#define RK3506_IRQ_I2C1          (RK3506_IRQ_SPI_BASE + 41)  /* 73 */
#define RK3506_IRQ_I2C2          (RK3506_IRQ_SPI_BASE + 42)  /* 74 */

/* SPI Interrupts (SPI 43-44) */

#define RK3506_IRQ_SPI0          (RK3506_IRQ_SPI_BASE + 43)  /* 75 */
#define RK3506_IRQ_SPI1          (RK3506_IRQ_SPI_BASE + 44)  /* 76 */

/* CAN Interrupts (SPI 45-46) */

#define RK3506_IRQ_CAN0          (RK3506_IRQ_SPI_BASE + 45)  /* 77 */
#define RK3506_IRQ_CAN1          (RK3506_IRQ_SPI_BASE + 46)  /* 78 */

/* Display Interrupts (SPI 59-60) */

#define RK3506_IRQ_VOP           (RK3506_IRQ_SPI_BASE + 59)  /* 91 */
#define RK3506_IRQ_DSI           (RK3506_IRQ_SPI_BASE + 60)  /* 92 */

/* Ethernet Interrupts (SPI 66-73) */

#define RK3506_IRQ_GMAC0_0       (RK3506_IRQ_SPI_BASE + 66)  /* 98 */
#define RK3506_IRQ_GMAC0_1       (RK3506_IRQ_SPI_BASE + 69)  /* 101 */
#define RK3506_IRQ_GMAC1_0       (RK3506_IRQ_SPI_BASE + 70)  /* 102 */
#define RK3506_IRQ_GMAC1_1       (RK3506_IRQ_SPI_BASE + 73)  /* 105 */

/* USB Interrupts (SPI 74-82) */

#define RK3506_IRQ_USBOTG0       (RK3506_IRQ_SPI_BASE + 74)  /* 106 */
#define RK3506_IRQ_USBOTG0_BVALID (RK3506_IRQ_SPI_BASE + 75) /* 107 */
#define RK3506_IRQ_USBOTG0_ID    (RK3506_IRQ_SPI_BASE + 76)  /* 108 */
#define RK3506_IRQ_USBOTG0_LS    (RK3506_IRQ_SPI_BASE + 77)  /* 109 */
#define RK3506_IRQ_USBOTG1       (RK3506_IRQ_SPI_BASE + 79)  /* 111 */
#define RK3506_IRQ_USBOTG1_BVALID (RK3506_IRQ_SPI_BASE + 80) /* 112 */
#define RK3506_IRQ_USBOTG1_ID    (RK3506_IRQ_SPI_BASE + 81)  /* 113 */
#define RK3506_IRQ_USBOTG1_LS    (RK3506_IRQ_SPI_BASE + 82)  /* 114 */

/* FSPI Interrupt (SPI 85) */

#define RK3506_IRQ_FSPI          (RK3506_IRQ_SPI_BASE + 85)  /* 117 */

/* SDMMC Interrupt (SPI 86) */

#define RK3506_IRQ_SDMMC         (RK3506_IRQ_SPI_BASE + 86)  /* 118 */

/* SARADC/TSADC Interrupts (SPI 57-58) */

#define RK3506_IRQ_SARADC        (RK3506_IRQ_SPI_BASE + 57)  /* 89 */
#define RK3506_IRQ_TSADC         (RK3506_IRQ_SPI_BASE + 58)  /* 90 */

/* Watchdog Interrupts (SPI 107-108) */

#define RK3506_IRQ_WDT0          (RK3506_IRQ_SPI_BASE + 107) /* 139 */
#define RK3506_IRQ_WDT1          (RK3506_IRQ_SPI_BASE + 108) /* 140 */

/* DMA Interrupts (SPI 116-119) */

#define RK3506_IRQ_DMAC0_0       (RK3506_IRQ_SPI_BASE + 116) /* 148 */
#define RK3506_IRQ_DMAC0_1       (RK3506_IRQ_SPI_BASE + 117) /* 149 */
#define RK3506_IRQ_DMAC1_0       (RK3506_IRQ_SPI_BASE + 118) /* 150 */
#define RK3506_IRQ_DMAC1_1       (RK3506_IRQ_SPI_BASE + 119) /* 151 */

/* Other Interrupts */

#define RK3506_IRQ_RGA2          (RK3506_IRQ_SPI_BASE + 61)  /* 93 */
#define RK3506_IRQ_CRYPTO        (RK3506_IRQ_SPI_BASE + 111) /* 143 */
#define RK3506_IRQ_RNG           (RK3506_IRQ_SPI_BASE + 114) /* 146 */
#define RK3506_IRQ_MAILBOX0      (RK3506_IRQ_SPI_BASE + 138) /* 170 */
#define RK3506_IRQ_MAILBOX1      (RK3506_IRQ_SPI_BASE + 139) /* 171 */
#define RK3506_IRQ_MAILBOX2      (RK3506_IRQ_SPI_BASE + 140) /* 172 */
#define RK3506_IRQ_MAILBOX3      (RK3506_IRQ_SPI_BASE + 141) /* 173 */

/* ARM PMU Interrupts (SPI 121-123) */

#define RK3506_IRQ_PMU0          (RK3506_IRQ_SPI_BASE + 121) /* 153 */
#define RK3506_IRQ_PMU1          (RK3506_IRQ_SPI_BASE + 122) /* 154 */
#define RK3506_IRQ_PMU2          (RK3506_IRQ_SPI_BASE + 123) /* 155 */

/* FIQ Debugger (SPI 115) */

#define RK3506_IRQ_FIQ_DEBUGGER (RK3506_IRQ_SPI_BASE + 115) /* 147 */

/* ARM Generic Timer IRQs (PPI, IRQ 0-31) */

#define RK3506_IRQ_STIMER        29  /* Non-secure physical timer (PPI 13) */

/* Total number of IRQs */

#define RK3506_IRQ_NINT          192

/* NR_IRQS required by NuttX kernel */

#define NR_IRQS                  RK3506_IRQ_NINT

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_INCLUDE_IRQ_H */
