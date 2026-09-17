/****************************************************************************
 * vendor/rockchip/chips/rk3506/hardware/rk3506_dwc2.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_DWC2_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_DWC2_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Endpoint type definitions */

#define DWC2_EPTYPE_CTRL               (0) /* Control */
#define DWC2_EPTYPE_ISOC               (1) /* Isochronous */
#define DWC2_EPTYPE_BULK               (2) /* Bulk */
#define DWC2_EPTYPE_INTR               (3) /* Interrupt */

#define DWC2_PID_DATA0                 (0)
#define DWC2_PID_DATA2                 (1)
#define DWC2_PID_DATA1                 (2)
#define DWC2_PID_MDATA                 (3) /* Non-control */
#define DWC2_PID_SETUP                 (3) /* Control */

/* Register Offsets *********************************************************/

/* Core global control and status registers */

#define DWC2_GOTGCTL_OFFSET            0x0000 /* Control and status register */
#define DWC2_GOTGINT_OFFSET            0x0004 /* Interrupt register */
#define DWC2_GAHBCFG_OFFSET            0x0008 /* AHB configuration register */
#define DWC2_GUSBCFG_OFFSET            0x000c /* USB configuration register */
#define DWC2_GRSTCTL_OFFSET            0x0010 /* Reset register */
#define DWC2_GINTSTS_OFFSET            0x0014 /* Core interrupt register */
#define DWC2_GINTMSK_OFFSET            0x0018 /* Interrupt mask register */
#define DWC2_GRXSTSR_OFFSET            0x001c /* Receive status debug read */
#define DWC2_GRXSTSP_OFFSET            0x0020 /* Receive status debug pop */
#define DWC2_GRXFSIZ_OFFSET            0x0024 /* Receive FIFO size register */
#define DWC2_HNPTXFSIZ_OFFSET          0x0028 /* Host non-periodic Tx FIFO size */
#define DWC2_HNPTXSTS_OFFSET           0x002c /* Non-periodic Tx FIFO/queue status */
#define DWC2_GCCFG_OFFSET              0x0038 /* General core configuration */
#define DWC2_CID_OFFSET                0x003c /* Core ID register */
#define DWC2_HPTXFSIZ_OFFSET           0x0100 /* Host periodic Tx FIFO size */

/* Host-mode control and status registers */

#define DWC2_HCFG_OFFSET               0x0400 /* Host configuration register */
#define DWC2_HFIR_OFFSET               0x0404 /* Host frame interval register */
#define DWC2_HFNUM_OFFSET              0x0408 /* Host frame number register */
#define DWC2_HPTXSTS_OFFSET            0x0410 /* Host periodic Tx FIFO/queue status */
#define DWC2_HAINT_OFFSET              0x0414 /* Host all channels interrupt */
#define DWC2_HAINTMSK_OFFSET           0x0418 /* Host all channels interrupt mask */
#define DWC2_HPRT_OFFSET               0x0440 /* Host port control and status */

/* Host channel registers (n = 0..11 for RK3506) */

#define DWC2_HCCHAR_OFFSET(n)          (0x0500 + ((n) << 5))
#define DWC2_HCSPLT_OFFSET(n)          (0x0504 + ((n) << 5))
#define DWC2_HCINT_OFFSET(n)           (0x0508 + ((n) << 5))
#define DWC2_HCINTMSK_OFFSET(n)        (0x050c + ((n) << 5))
#define DWC2_HCTSIZ_OFFSET(n)          (0x0510 + ((n) << 5))
#define DWC2_HCDMA_OFFSET(n)           (0x0514 + ((n) << 5))

/* Power and clock gating registers */

#define DWC2_PCGCCTL_OFFSET            0x0e00

/* Data FIFO access registers */

#define DWC2_DFIFO_OFFSET(n)           (0x1000 + ((n) << 12))

/* Register Bitfield Definitions ********************************************/

/* GOTGCTL - Control and status register */

#define DWC2_GOTGCTL_HNGSCS            (1 << 8)  /* Host negotiation success */
#define DWC2_GOTGCTL_HNPRQ             (1 << 9)  /* HNP request */
#define DWC2_GOTGCTL_HSHNPEN           (1 << 10) /* Host set HNP enable */
#define DWC2_GOTGCTL_CIDSTS            (1 << 16) /* Connector ID status */
#define DWC2_GOTGCTL_ASVLD             (1 << 18) /* A-session valid */
#define DWC2_GOTGCTL_BSVLD             (1 << 19) /* B-session valid */

/* GAHBCFG - AHB configuration register */

#define DWC2_GAHBCFG_GINTMSK           (1 << 0)  /* Global interrupt mask */
#define DWC2_GAHBCFG_TXFELVL           (1 << 7)  /* TxFIFO empty level */
#define DWC2_GAHBCFG_PTXFELVL          (1 << 8)  /* Periodic TxFIFO empty level */

/* GUSBCFG - USB configuration register */

#define DWC2_GUSBCFG_TOCAL_SHIFT       (0)
#define DWC2_GUSBCFG_TOCAL_MASK        (7 << DWC2_GUSBCFG_TOCAL_SHIFT)
#define DWC2_GUSBCFG_PHYSEL            (1 << 6)  /* FS serial transceiver select */
#define DWC2_GUSBCFG_TRDT_SHIFT        (10)
#define DWC2_GUSBCFG_TRDT_MASK         (15 << DWC2_GUSBCFG_TRDT_SHIFT)
#define DWC2_GUSBCFG_TRDT(n)           ((n) << DWC2_GUSBCFG_TRDT_SHIFT)
#define DWC2_GUSBCFG_FHMOD             (1 << 29) /* Force host mode */
#define DWC2_GUSBCFG_FDMOD             (1 << 30) /* Force device mode */

/* GRSTCTL - Reset register */

#define DWC2_GRSTCTL_CSRST             (1 << 0)  /* Core soft reset */
#define DWC2_GRSTCTL_HSRST             (1 << 1)  /* HCLK soft reset */
#define DWC2_GRSTCTL_FCRST             (1 << 2)  /* Host frame counter reset */
#define DWC2_GRSTCTL_RXFFLSH           (1 << 4)  /* RxFIFO flush */
#define DWC2_GRSTCTL_TXFFLSH           (1 << 5)  /* TxFIFO flush */
#define DWC2_GRSTCTL_TXFNUM_SHIFT      (6)
#define DWC2_GRSTCTL_TXFNUM_MASK       (31 << DWC2_GRSTCTL_TXFNUM_SHIFT)
#define DWC2_GRSTCTL_TXFNUM_HALL       (16 << DWC2_GRSTCTL_TXFNUM_SHIFT)
#define DWC2_GRSTCTL_AHBIDL            (1 << 31) /* AHB master idle */

/* GINTSTS / GINTMSK - Core interrupt register / mask */

#define DWC2_GINT_CMOD                 (1 << 0)  /* Current mode of operation */
#define DWC2_GINT_MMIS                 (1 << 1)  /* Mode mismatch interrupt */
#define DWC2_GINT_SOF                  (1 << 3)  /* Start of frame */
#define DWC2_GINT_RXFLVL               (1 << 4)  /* RxFIFO non-empty */
#define DWC2_GINT_NPTXFE               (1 << 5)  /* Non-periodic TxFIFO empty */
#define DWC2_GINT_ESUSP                (1 << 10) /* Early suspend */
#define DWC2_GINT_USBSUSP              (1 << 11) /* USB suspend */
#define DWC2_GINT_USBRST               (1 << 12) /* USB reset */
#define DWC2_GINT_ENUMDNE              (1 << 13) /* Enumeration done */
#define DWC2_GINT_HPRT                 (1 << 24) /* Host port interrupt */
#define DWC2_GINT_HC                   (1 << 25) /* Host channels interrupt */
#define DWC2_GINT_PTXFE                (1 << 26) /* Periodic TxFIFO empty */
#define DWC2_GINT_IPXFR                (1 << 21) /* Incomplete periodic transfer */
#define DWC2_GINT_DISC                 (1 << 29) /* Disconnect detected */
#define DWC2_GINT_WKUP                 (1 << 31) /* Resume/remote wakeup */

/* GRXSTSH - Receive status (host mode) */

#define DWC2_GRXSTSH_CHNUM_SHIFT       (0)
#define DWC2_GRXSTSH_CHNUM_MASK        (15 << DWC2_GRXSTSH_CHNUM_SHIFT)
#define DWC2_GRXSTSH_BCNT_SHIFT        (4)
#define DWC2_GRXSTSH_BCNT_MASK         (0x7ff << DWC2_GRXSTSH_BCNT_SHIFT)
#define DWC2_GRXSTSH_DPID_SHIFT        (15)
#define DWC2_GRXSTSH_DPID_MASK         (3 << DWC2_GRXSTSH_DPID_SHIFT)
#define DWC2_GRXSTSH_PKTSTS_SHIFT      (17)
#define DWC2_GRXSTSH_PKTSTS_MASK       (15 << DWC2_GRXSTSH_PKTSTS_SHIFT)
#define DWC2_GRXSTSH_PKTSTS_INRECVD    (2 << DWC2_GRXSTSH_PKTSTS_SHIFT)
#define DWC2_GRXSTSH_PKTSTS_INDONE     (3 << DWC2_GRXSTSH_PKTSTS_SHIFT)
#define DWC2_GRXSTSH_PKTSTS_DTOGERR    (5 << DWC2_GRXSTSH_PKTSTS_SHIFT)
#define DWC2_GRXSTSH_PKTSTS_HALTED     (7 << DWC2_GRXSTSH_PKTSTS_SHIFT)

/* GRXFSIZ - Receive FIFO size register */

#define DWC2_GRXFSIZ_MASK              (0xffff)

/* HNPTXFSIZ - Host non-periodic Tx FIFO size register */

#define DWC2_HNPTXFSIZ_NPTXFSA_SHIFT   (0)
#define DWC2_HNPTXFSIZ_NPTXFSA_MASK    (0xffff)
#define DWC2_HNPTXFSIZ_NPTXFD_SHIFT    (16)
#define DWC2_HNPTXFSIZ_NPTXFD_MASK     (0xffff << DWC2_HNPTXFSIZ_NPTXFD_SHIFT)

/* HNPTXSTS - Non-periodic Tx FIFO/queue status register */

#define DWC2_HNPTXSTS_NPTXFSAV_SHIFT   (0)
#define DWC2_HNPTXSTS_NPTXFSAV_MASK    (0xffff)

/* GCCFG - General core configuration register */

#define DWC2_GCCFG_PWRDWN              (1 << 16) /* Power down */
#define DWC2_GCCFG_VBUSASEN            (1 << 18) /* VBUS sensing A device */
#define DWC2_GCCFG_VBUSBSEN            (1 << 19) /* VBUS sensing B device */
#define DWC2_GCCFG_NOVBUSSENS          (1 << 21) /* VBUS sensing disable */

/* HCFG - Host configuration register */

#define DWC2_HCFG_FSLSPCS_SHIFT        (0)
#define DWC2_HCFG_FSLSPCS_MASK         (3 << DWC2_HCFG_FSLSPCS_SHIFT)
#define DWC2_HCFG_FSLSPCS_FS48MHz      (1 << DWC2_HCFG_FSLSPCS_SHIFT)
#define DWC2_HCFG_FSLSS                (1 << 2)  /* FS/LS-only support */

/* HFNUM - Host frame number register */

#define DWC2_HFNUM_FRNUM_SHIFT         (0)
#define DWC2_HFNUM_FRNUM_MASK          (0xffff)
#define DWC2_HFNUM_FTREM_SHIFT         (16)
#define DWC2_HFNUM_FTREM_MASK          (0xffff << DWC2_HFNUM_FTREM_SHIFT)

/* HPTXSTS - Host periodic Tx FIFO/queue status register */

#define DWC2_HPTXSTS_PTXFSAVL_SHIFT    (0)
#define DWC2_HPTXSTS_PTXFSAVL_MASK     (0xffff)

/* HAINT - Host all channels interrupt register */

#define DWC2_HAINT(n)                  (1 << (n))

/* HPRT - Host port control and status register */

#define DWC2_HPRT_PCSTS                (1 << 0)  /* Port connect status */
#define DWC2_HPRT_PCDET                (1 << 1)  /* Port connect detected */
#define DWC2_HPRT_PENA                 (1 << 2)  /* Port enable */
#define DWC2_HPRT_PENCHNG              (1 << 3)  /* Port enable/disable change */
#define DWC2_HPRT_POCA                 (1 << 4)  /* Port overcurrent active */
#define DWC2_HPRT_POCCHNG              (1 << 5)  /* Port overcurrent change */
#define DWC2_HPRT_PRES                 (1 << 6)  /* Port resume */
#define DWC2_HPRT_PSUSP                (1 << 7)  /* Port suspend */
#define DWC2_HPRT_PRST                 (1 << 8)  /* Port reset */
#define DWC2_HPRT_PLSTS_SHIFT          (10)
#define DWC2_HPRT_PLSTS_MASK           (3 << DWC2_HPRT_PLSTS_SHIFT)
#define DWC2_HPRT_PPWR                 (1 << 12) /* Port power */
#define DWC2_HPRT_PSPD_SHIFT           (17)
#define DWC2_HPRT_PSPD_MASK            (3 << DWC2_HPRT_PSPD_SHIFT)
#define DWC2_HPRT_PSPD_FS              (1 << DWC2_HPRT_PSPD_SHIFT)
#define DWC2_HPRT_PSPD_LS              (2 << DWC2_HPRT_PSPD_SHIFT)

/* HCCHAR - Host channel characteristics register */

#define DWC2_HCCHAR_MPSIZ_SHIFT        (0)
#define DWC2_HCCHAR_MPSIZ_MASK         (0x7ff << DWC2_HCCHAR_MPSIZ_SHIFT)
#define DWC2_HCCHAR_EPNUM_SHIFT        (11)
#define DWC2_HCCHAR_EPNUM_MASK         (15 << DWC2_HCCHAR_EPNUM_SHIFT)
#define DWC2_HCCHAR_EPDIR              (1 << 15) /* Endpoint direction */
#define DWC2_HCCHAR_EPDIR_OUT          (0)
#define DWC2_HCCHAR_EPDIR_IN           DWC2_HCCHAR_EPDIR
#define DWC2_HCCHAR_LSDEV              (1 << 17) /* Low-speed device */
#define DWC2_HCCHAR_EPTYP_SHIFT        (18)
#define DWC2_HCCHAR_EPTYP_MASK         (3 << DWC2_HCCHAR_EPTYP_SHIFT)
#define DWC2_HCCHAR_EPTYP_CTRL         (0 << DWC2_HCCHAR_EPTYP_SHIFT)
#define DWC2_HCCHAR_EPTYP_ISOC         (1 << DWC2_HCCHAR_EPTYP_SHIFT)
#define DWC2_HCCHAR_EPTYP_BULK         (2 << DWC2_HCCHAR_EPTYP_SHIFT)
#define DWC2_HCCHAR_EPTYP_INTR         (3 << DWC2_HCCHAR_EPTYP_SHIFT)
#define DWC2_HCCHAR_MCNT_SHIFT         (20)
#define DWC2_HCCHAR_MCNT_MASK          (3 << DWC2_HCCHAR_MCNT_SHIFT)
#define DWC2_HCCHAR_DAD_SHIFT          (22)
#define DWC2_HCCHAR_DAD_MASK           (0x7f << DWC2_HCCHAR_DAD_SHIFT)
#define DWC2_HCCHAR_ODDFRM             (1 << 29) /* Odd frame */
#define DWC2_HCCHAR_CHDIS              (1 << 30) /* Channel disable */
#define DWC2_HCCHAR_CHENA              (1 << 31) /* Channel enable */

/* HCINT - Host channel interrupt register */

#define DWC2_HCINT_XFRC                (1 << 0)  /* Transfer completed */
#define DWC2_HCINT_CHH                 (1 << 1)  /* Channel halted */
#define DWC2_HCINT_STALL               (1 << 3)  /* STALL response */
#define DWC2_HCINT_NAK                 (1 << 4)  /* NAK response */
#define DWC2_HCINT_ACK                 (1 << 5)  /* ACK response */
#define DWC2_HCINT_NYET                (1 << 6)  /* NYET response */
#define DWC2_HCINT_TXERR               (1 << 7)  /* Transaction error */
#define DWC2_HCINT_BBERR               (1 << 8)  /* Babble error */
#define DWC2_HCINT_FRMOR               (1 << 9)  /* Frame overrun */
#define DWC2_HCINT_DTERR               (1 << 10) /* Data toggle error */

/* HCTSIZ - Host channel transfer size register */

#define DWC2_HCTSIZ_XFRSIZ_SHIFT       (0)
#define DWC2_HCTSIZ_XFRSIZ_MASK        (0x7ffff << DWC2_HCTSIZ_XFRSIZ_SHIFT)
#define DWC2_HCTSIZ_PKTCNT_SHIFT       (19)
#define DWC2_HCTSIZ_PKTCNT_MASK        (0x3ff << DWC2_HCTSIZ_PKTCNT_SHIFT)
#define DWC2_HCTSIZ_DPID_SHIFT         (29)
#define DWC2_HCTSIZ_DPID_MASK          (3 << DWC2_HCTSIZ_DPID_SHIFT)
#define DWC2_HCTSIZ_DPID_DATA0         (0 << DWC2_HCTSIZ_DPID_SHIFT)
#define DWC2_HCTSIZ_DPID_DATA1         (2 << DWC2_HCTSIZ_DPID_SHIFT)
#define DWC2_HCTSIZ_PID_SETUP          (3 << DWC2_HCTSIZ_DPID_SHIFT)

/* Trace IDs for USB host debug *********************************************/

#define DWC2_TRACE1_IRQATTACH          0x0001
#define DWC2_TRACE1_DEVDISCONN         0x0002
#define DWC2_TRACE1_SENDSETUP          0x0003
#define DWC2_TRACE1_RECVDATA           0x0004
#define DWC2_TRACE1_ENQUEUE            0x0005
#define DWC2_TRACE1_SENDNAK            0x0006

#define DWC2_VTRACE1_CONNECTED         0x1001
#define DWC2_VTRACE1_DISCONNECTED      0x1002
#define DWC2_VTRACE1_GINT_SOF          0x1003
#define DWC2_VTRACE1_GINT_RXFLVL       0x1004
#define DWC2_VTRACE1_GINT_NPTXFE       0x1005
#define DWC2_VTRACE1_GINT_HC           0x1006
#define DWC2_VTRACE1_GINT_HPRT         0x1007
#define DWC2_VTRACE1_GINT_DISC         0x1008
#define DWC2_VTRACE1_GINT_IPXFR        0x1009

#define DWC2_VTRACE2_CTRLIN            0x2001
#define DWC2_VTRACE2_CTRLOUT           0x2002
#define DWC2_VTRACE2_CHANCONF_CTRL_IN  0x2003
#define DWC2_VTRACE2_CHANCONF_CTRL_OUT 0x2004
#define DWC2_VTRACE2_CHANCONF_BULK_IN  0x2005
#define DWC2_VTRACE2_CHANCONF_BULK_OUT 0x2006
#define DWC2_VTRACE2_CHANCONF_INTR_IN  0x2007
#define DWC2_VTRACE2_CHANCONF_INTR_OUT 0x2008
#define DWC2_VTRACE2_CHANHALT          0x2009

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_DWC2_H */
