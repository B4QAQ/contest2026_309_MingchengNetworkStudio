/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_serial.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SERIAL_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SERIAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* UART Register Offsets (DesignWare 8250 compatible) */

#define RK3506_UART_THR_OFFSET   0x00  /* Transmit Holding Register */
#define RK3506_UART_RBR_OFFSET   0x00  /* Receive Buffer Register */
#define RK3506_UART_DLL_OFFSET   0x00  /* Divisor Latch Low */
#define RK3506_UART_DLH_OFFSET   0x04  /* Divisor Latch High */
#define RK3506_UART_IER_OFFSET   0x04  /* Interrupt Enable Register */
#define RK3506_UART_IIR_OFFSET   0x08  /* Interrupt Identity Register */
#define RK3506_UART_FCR_OFFSET   0x08  /* FIFO Control Register */
#define RK3506_UART_LCR_OFFSET   0x0c  /* Line Control Register */
#define RK3506_UART_MCR_OFFSET   0x10  /* Modem Control Register */
#define RK3506_UART_LSR_OFFSET   0x14  /* Line Status Register */
#define RK3506_UART_MSR_OFFSET   0x18  /* Modem Status Register */
#define RK3506_UART_SCR_OFFSET   0x1c  /* Scratch Register */
#define RK3506_UART_USR_OFFSET   0x7c  /* UART Status Register */

/* UART Register Bit Definitions */

#define RK3506_UART_LSR_DR       (1 << 0)  /* Data Ready */
#define RK3506_UART_LSR_OE       (1 << 1)  /* Overrun Error */
#define RK3506_UART_LSR_PE       (1 << 2)  /* Parity Error */
#define RK3506_UART_LSR_FE       (1 << 3)  /* Framing Error */
#define RK3506_UART_LSR_BI       (1 << 4)  /* Break Interrupt */
#define RK3506_UART_LSR_THRE     (1 << 5)  /* TX Holding Register Empty */
#define RK3506_UART_LSR_TEMT     (1 << 6)  /* Transmitter Empty */

#define RK3506_UART_IER_ERBFI    (1 << 0)  /* Enable RX Data Interrupt */
#define RK3506_UART_IER_ETBEI    (1 << 1)  /* Enable TX Empty Interrupt */

#define RK3506_UART_LCR_DLAB     (1 << 7)  /* Divisor Latch Access Bit */

#define RK3506_UART_USR_BUSY     (1 << 0)  /* UART Busy */

/* UART Interrupt Identity Register */

#define RK3506_UART_IIR_IID_SHIFT        0
#define RK3506_UART_IIR_IID_MASK         (15 << RK3506_UART_IIR_IID_SHIFT)
#define RK3506_UART_IIR_IID_NONE         (1 << RK3506_UART_IIR_IID_SHIFT)
#define RK3506_UART_IIR_IID_TXEMPTY      (2 << RK3506_UART_IIR_IID_SHIFT)
#define RK3506_UART_IIR_IID_RECV         (4 << RK3506_UART_IIR_IID_SHIFT)
#define RK3506_UART_IIR_IID_TIMEOUT      (12 << RK3506_UART_IIR_IID_SHIFT)

#define RK3506_UART_IIR_FEFLAG_SHIFT     6
#define RK3506_UART_IIR_FEFLAG_MASK      (3 << RK3506_UART_IIR_FEFLAG_SHIFT)

/* UART FIFO Control Register */

#define RK3506_UART_FCR_FIFOE    (1 << 0)  /* FIFO Enable */
#define RK3506_UART_FCR_RFIFOR   (1 << 1)  /* Receiver FIFO Reset */
#define RK3506_UART_FCR_XFIFOR   (1 << 2)  /* Transmitter FIFO Reset */

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_SERIAL_H */
