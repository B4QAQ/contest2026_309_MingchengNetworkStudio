/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_lowputc.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "arm_internal.h"
#include "hardware/rk3506_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* UART Register Offsets (DesignWare 8250 compatible) */

#define UART_THR_OFFSET   0x00  /* Transmit Holding Register */
#define UART_RBR_OFFSET   0x00  /* Receive Buffer Register */
#define UART_DLL_OFFSET   0x00  /* Divisor Latch Low */
#define UART_DLH_OFFSET   0x04  /* Divisor Latch High */
#define UART_IER_OFFSET   0x04  /* Interrupt Enable Register */
#define UART_IIR_OFFSET   0x08  /* Interrupt Identity Register */
#define UART_FCR_OFFSET   0x08  /* FIFO Control Register */
#define UART_LCR_OFFSET   0x0c  /* Line Control Register */
#define UART_MCR_OFFSET   0x10  /* Modem Control Register */
#define UART_LSR_OFFSET   0x14  /* Line Status Register */
#define UART_MSR_OFFSET   0x18  /* Modem Status Register */
#define UART_SCR_OFFSET   0x1c  /* Scratch Register */
#define UART_USR_OFFSET   0x7c  /* UART Status Register */

/* UART Register Bit Definitions */

#define UART_LSR_DR       (1 << 0)  /* Data Ready */
#define UART_LSR_THRE     (1 << 5)  /* Transmitter Holding Register Empty */
#define UART_LSR_TEMT     (1 << 6)  /* Transmitter Empty */

#define UART_LCR_DLAB     (1 << 7)  /* Divisor Latch Access Bit */
#define UART_LCR_8N1      0x03      /* 8 data bits, no parity, 1 stop bit */

#define UART_FCR_FIFOE    (1 << 0)  /* FIFO Enable */
#define UART_FCR_RFIFOR   (1 << 1)  /* Receiver FIFO Reset */
#define UART_FCR_XFIFOR   (1 << 2)  /* Transmitter FIFO Reset */

/* UART Clock and Baud Rate
 *
 * RK3506 UART source clock is 24 MHz (XIN24M) by default.
 * The BootROM/MiniLoader does not change the UART clock before loading
 * NuttX (verified by U-Boot's empty board_debug_uart_init()).
 *
 * If you need a different UART clock, configure it in the CRU at boot
 * time before calling arm_lowputc().
 */

#define UART_CLK          24000000
#define DEFAULT_BAUD      115200

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_uart_divisor
 *
 * Description:
 *   Calculate the UART divisor for the given baud rate.
 *   DL = UART_CLK / (16 * baud)
 *
 ****************************************************************************/

static uint32_t rk3506_uart_divisor(uint32_t baud)
{
  return UART_CLK / (baud << 4);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_lowputc
 *
 * Description:
 *   Output a single character to the UART0 (debug console).
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_EARLY_PRINT
void arm_lowputc(char ch)
{
  uintptr_t base = RK3506_UART0_ADDR;

  /* Wait for the transmit holding register to be empty */

  while (!(getreg32(base + UART_LSR_OFFSET) & UART_LSR_THRE))
    {
    }

  /* Write the character to the transmit holding register */

  putreg32((uint32_t)ch, base + UART_THR_OFFSET);
}
#endif

/****************************************************************************
 * Name: rk3506_lowsetup
 *
 * Description:
 *   Initialize UART0 for early console output.
 *   This is called very early in the boot sequence before the full serial
 *   driver is initialized.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_EARLY_PRINT
void rk3506_lowsetup(void)
{
  uintptr_t base = RK3506_UART0_ADDR;
  uint32_t dl;

  /* Calculate divisor for default baud rate */

  dl = rk3506_uart_divisor(DEFAULT_BAUD);

  /* Set DLAB to access divisor latch registers */

  putreg32(UART_LCR_DLAB, base + UART_LCR_OFFSET);

  /* Set divisor latch low and high bytes */

  putreg32(dl & 0xff, base + UART_DLL_OFFSET);
  putreg32((dl >> 8) & 0xff, base + UART_DLH_OFFSET);

  /* Clear DLAB and set 8N1 format */

  putreg32(UART_LCR_8N1, base + UART_LCR_OFFSET);

  /* Enable and reset FIFOs */

  putreg32(UART_FCR_FIFOE | UART_FCR_RFIFOR | UART_FCR_XFIFOR,
           base + UART_FCR_OFFSET);
}
#endif
