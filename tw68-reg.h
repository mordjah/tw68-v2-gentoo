/*

    tw68-reg.h - TW6800 register offsets

    Copyright (C) William M. Brack <wbrack@mmm.com.hk>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef _TW68_REG_H_
#define _TW68_REG_H_

/* ---------------------------------------------------------------------- */
/* PCI IDs and config space                                               */

#ifndef PCI_VENDOR_ID_JUMPTEC
# define PCI_VENDOR_ID_JUMPTEC		0x1797
#endif
#ifndef PCI_DEVICE_ID_TW6801_VID
# define PCI_DEVICE_ID_TW6801_VID	0x6801
#endif

/* ---------------------------------------------------------------------- */
#define	TW68_DMAC		0x000
#define	TW68_DMAP_SA		0x004
#define	TW68_DMAP_EXE		0x008
#define	TW68_DMAP_PP		0x00c
#define	TW68_VBIC		0x010
#define	TW68_SBUSC		0x014
#define	TW68_SBUSD		0x018
#define	TW68_INTSTAT		0x01C
#define	TW68_INTMASK		0x020
#define	TW68_GPIOC		0x024
#define	TW68_GPOE		0x028
#define	TW68_TESTREG		0x02C
#define	TW68_VBIINST		0x06C
/* define bits in FIFO and DMAP Control reg */
#define	TW68_DMAP_EN		(1 << 0)
#define	TW68_FIFO_EN		(1 << 1)
/* define the Interrupt Status Register bits */
#define	TW68_SBDONE		(1 << 0)
#define	TW68_DMAPI		(1 << 1)
#define	TW68_GPINT		(1 << 2)
#define	TW68_FFOF		(1 << 3)
#define	TW68_FDMIS		(1 << 4)
#define	TW68_DMAPERR		(1 << 5)
#define	TW68_PABORT		(1 << 6)
#define	TW68_PPERR		(1 << 14)
#define	TW68_FFERR		(1 << 15)
#define	TW68_DET50		(1 << 16)
#define	TW68_FLOCK		(1 << 17)
#define	TW68_CCVALID		(1 << 18)
#define	TW68_VLOCK		(1 << 19)
#define	TW68_FIELD		(1 << 20)
#define	TW68_SLOCK		(1 << 21)
#define	TW68_HLOCK		(1 << 22)
#define	TW68_VDLOSS		(1 << 23)
#define	TW68_SBERR		(1 << 24)
#define	TW68_GPDATA		0x100
#define	TW68_DSTATUS		0x204
#define	TW68_INFORM		0x208
#define	TW68_RESERV1		0x20C
#define	TW68_HSYNC		0x210
#define	TW68_POLARITY		0x214
#define	TW68_ACNTL		0x218
#define	TW68_CROP_HI		0x21C
#define	TW68_VDELAY_LO		0x220
#define	TW68_VACTIVE_LO		0x224
#define	TW68_HDELAY_LO		0x228
#define	TW68_HACTIVE_LO		0x22C
#define	TW68_CNTRL1		0x230
#define	TW68_VSCALE_LO		0x234
#define	TW68_SCALE_HI		0x238
#define	TW68_HSCALE_LO		0x23C
#define	TW68_BRIGHT		0x240
#define	TW68_CONTRAST		0x244
#define	TW68_SHARPNESS		0x248
#define	TW68_SAT_U		0x24C
#define	TW68_SAT_V		0x250
#define	TW68_HUE		0x254
#define	TW68_SHARP2		0x258
#define	TW68_RESERV2		0x25C
#define	TW68_CORING		0x260
#define	TW68_VBICNTL		0x264
#define	TW68_CC_STATUS		0x268
#define	TW68_CC_DATA		0x26C
#define	TW68_SDT		0x270
#define	TW68_SDTR		0x274
#define	TW68_RESERV3		0x278
#define	TW68_RESERV4		0x27C
#define	TW68_CLMPG		0x280
#define	TW68_IAGC		0x284
#define	TW68_AGCGAIN		0x288
#define	TW68_PEAKWT		0x28C
#define	TW68_CLMPL		0x290
#define	TW68_SYNCT		0x294
#define	TW68_MISSCNT		0x298
#define	TW68_PCLAMP		0x29C
#define	TW68_VERTCTL		0x2A0
#define	TW68_VERTCTL2		0x2A4
#define	TW68_COLORKILL		0x2A8
#define	TW68_COMBFILT		0x2AC
#define	TW68_LUMA_HSYNC		0x2B0
#define	TW68_MISC1		0x2B4
#define	TW68_MISC2		0x2B8
#define	TW68_MISC3		0x2BC
#define	TW68_MACROVISION	0x2C0
#define	TW68_CLMPCTL2		0x2C4
#define	TW68_FILLDATA		0x2C8
#define	TW68_VBICNTL1		0x2CC
#define	TW68_VBICNTL2		0x2D0
#define	TW68_MISC4		0x2D4
#define	TW68_SLICELEVEL		0x2D8
#define	TW68_WSS1		0x2DC
#define	TW68_WSS2		0x2E0
#define	TW68_CSTATUS3		0x2E4
#define	TW68_HFREF		0x2E8

#define	RISC_INT_BIT		0x08000000
#define	RISC_SYNCO		0xC0000000
#define	RISC_SYNCE		0xD0000000
#define	RISC_JUMP		0xB0000000
#define	RISC_LINESTART		0x90000000
#define	RISC_INLINE		0xA0000000

#define VideoFormatAuto		 0x7
#define VideoFormatNTSC		 0x0
#define VideoFormatNTSCJapan	 0x0
#define VideoFormatNTSC443	 0x3
#define VideoFormatPAL		 0x1	/* ?? */
#define VideoFormatPALB		 0x1
#define VideoFormatPALD		 0x1
#define VideoFormatPALG		 0x1
#define VideoFormatPALH		 0x1
#define VideoFormatPALI		 0x1
#define VideoFormatPALBDGHI	 0x1
#define VideoFormatPALM		 0x4
#define VideoFormatPALN		 0x5
#define VideoFormatPALNC	 0x5
#define VideoFormatPAL60	 0x6
#define VideoFormatSECAM	 0x2

#define ColorFormatRGB32	 0x00
#define ColorFormatRGB24	 0x10
#define ColorFormatRGB16	 0x20
#define ColorFormatRGB15	 0x30
#define ColorFormatYUY2		 0x40
#if 0
#define ColorFormatBTYUV	 0x0055
#define ColorFormatY8		 0x0066
#define ColorFormatRGB8		 0x0077
#define ColorFormatPL422	 0x0088
#define ColorFormatPL411	 0x0099
#define ColorFormatYUV12	 0x00AA
#define ColorFormatYUV9		 0x00BB
#define ColorFormatRAW		 0x00EE
#endif
#define ColorFormatBSWAP         0x04
#define ColorFormatWSWAP         0x08
#if 0
#define ColorFormatEvenMask      0x050f
#define ColorFormatOddMask       0x0af0
#endif
#define ColorFormatGamma         0x1000
#endif
