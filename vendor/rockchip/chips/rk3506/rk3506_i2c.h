/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_i2c.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_I2C_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_I2C_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2C Register Offsets (RK3x I2C, compatible with RK3399) */

#define RK3506_I2C_REG_CON        0x00  /* Control register */
#define RK3506_I2C_REG_CLKDIV     0x04  /* Clock divisor register */
#define RK3506_I2C_REG_MRXADDR    0x08  /* Slave address for read */
#define RK3506_I2C_REG_MRXRADDR   0x0c  /* Slave register address */
#define RK3506_I2C_REG_MTXCNT     0x10  /* Number of bytes to transmit */
#define RK3506_I2C_REG_MRXCNT     0x14  /* Number of bytes to receive */
#define RK3506_I2C_REG_IEN        0x18  /* Interrupt enable */
#define RK3506_I2C_REG_IPD        0x1c  /* Interrupt pending */
#define RK3506_I2C_REG_FCNT       0x20  /* Finished count */
#define RK3506_I2C_REG_SCL_OE_DB  0x24  /* Slave hold SCL debounce */
#define RK3506_I2C_REG_CON1       0x228 /* Control register 1 (V5) */

/* Data buffer offsets */

#define RK3506_I2C_TXBUFFER_BASE  0x100
#define RK3506_I2C_RXBUFFER_BASE  0x200

/* REG_CON bits */

#define RK3506_I2C_CON_EN         (1 << 0)
#define RK3506_I2C_CON_MOD_TX     (0 << 1)
#define RK3506_I2C_CON_MOD_REG_TX (1 << 1)
#define RK3506_I2C_CON_MOD_RX     (2 << 1)
#define RK3506_I2C_CON_MOD_REG_RX (3 << 1)
#define RK3506_I2C_CON_MOD_MASK   (3 << 1)
#define RK3506_I2C_CON_START      (1 << 3)
#define RK3506_I2C_CON_STOP       (1 << 4)
#define RK3506_I2C_CON_LASTACK    (1 << 5)
#define RK3506_I2C_CON_ACTACK     (1 << 6)

/* CON[15:8] tuning bits - preserved when forcing STOP/EN */

#define RK3506_I2C_CON_TUNING_MASK (0xff << 8)

/* REG_MRXADDR bits */

#define RK3506_I2C_MRXADDR_VALID(x) (1 << (24 + (x)))

/* REG_IEN/REG_IPD bits */

#define RK3506_I2C_INT_BTF        (1 << 0)  /* Byte transmitted */
#define RK3506_I2C_INT_BRF        (1 << 1)  /* Byte received */
#define RK3506_I2C_INT_MBTF       (1 << 2)  /* Master TX finished */
#define RK3506_I2C_INT_MBRF       (1 << 3)  /* Master RX finished */
#define RK3506_I2C_INT_START      (1 << 4)  /* START generated */
#define RK3506_I2C_INT_STOP       (1 << 5)  /* STOP generated */
#define RK3506_I2C_INT_NAKRCV     (1 << 6)  /* NACK received */
#define RK3506_I2C_INT_SLV_HDSCL  (1 << 7)  /* Slave hold SCL */
#define RK3506_I2C_INT_ALL        0xff

/* REG_CON1 bits (V5) */

#define RK3506_I2C_CON1_AUTO_STOP       (1 << 0)
#define RK3506_I2C_CON1_TRANSFER_AUTO_STOP (1 << 1)
#define RK3506_I2C_CON1_NACK_AUTO_STOP  (1 << 2)

/* I2C buffer size */

#define RK3506_I2C_MAX_RXBUF     32
#define RK3506_I2C_MAX_TXBUF     32

/* I2C transfer timeout (ms) */

#define RK3506_I2C_TIMEOUT_MS    200

/* I2C bus numbers */

#define RK3506_I2C_BUS0          0
#define RK3506_I2C_BUS1          1
#define RK3506_I2C_BUS2          2

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_i2cbus_initialize
 *
 * Description:
 *   Initialize the I2C bus and return an I2C master device instance.
 *
 * Input Parameters:
 *   bus - I2C bus number (0, 1, or 2)
 *
 * Returned Value:
 *   Pointer to I2C master device on success; NULL on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3506_I2C
FAR struct i2c_master_s *rk3506_i2cbus_initialize(int bus);
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_I2C_H */
