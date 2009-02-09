/*
 *
 * v4l2 device driver for Techwell 6800 based video capture cards
 *
 * (c) 2009 William M. Brack <wbrack@mmm.com.hk>
 *
 * The design and coding of this driver is heavily based upon the cx88
 * driver originally written by Gerd Knorr and modified by Mauro Carvalho
 * Chehab, whose work is gratefully acknowledged.  Full credit goes to
 * them - any problems are mine.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/sound.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <linux/mutex.h>

#include "tw68.h"
#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>

MODULE_DESCRIPTION("v4l2 driver module for tw6800 based video capture cards");
MODULE_AUTHOR("William M. Brack <wbrack@mmm.com.hk>");
MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */

static unsigned int core_debug;
module_param(core_debug,int,0644);
MODULE_PARM_DESC(core_debug,"enable debug messages [core]");

static unsigned int nicam;
module_param(nicam,int,0644);
MODULE_PARM_DESC(nicam,"tv audio is nicam");

static unsigned int nocomb;
module_param(nocomb,int,0644);
MODULE_PARM_DESC(nocomb,"disable comb filter");

#define dprintk(level,fmt, arg...)	if (core_debug >= level)	\
	printk(KERN_DEBUG "%s: " fmt, core->name , ## arg)

static unsigned int tw68_devcount;	/* curr tot num of devices present */
static LIST_HEAD(tw68_devlist);
static DEFINE_MUTEX(devlist);

#define NO_SYNC_LINE (-1U)

/**
 *  @rp		pointer to current risc program position
 *  @sglist	pointer to "scatter-gather list" of buffer pointers
 *  @offset	offset to target memory buffer
 *  @sync_line	0 -> no sync, 1 -> odd sync, 2 -> even sync
 *  @bpl	number of bytes per scan line
 *  @padding	number of bytes of padding to add
 *  @lines	number of lines in field
 *  @lpi	lines per IRQ, or 0 to not generate irqs
 *  		Note: IRQ to be generated _after_ lpi lines are transferred
 */
static __le32* tw68_risc_field(__le32 *rp, struct scatterlist *sglist,
			    unsigned int offset, u32 sync_line,
			    unsigned int bpl, unsigned int padding,
			    unsigned int lines, unsigned int lpi)
{
	struct scatterlist *sg;
	unsigned int line,todo;

	/* sync instruction */
	if (sync_line != NO_SYNC_LINE) {
		if (sync_line == 1)
			*(rp++) = cpu_to_le32(RISC_SYNCO);
		else
			*(rp++) = cpu_to_le32(RISC_SYNCE);
		*(rp++) = 0;
	}
	/* scan lines */
	sg = sglist;
	for (line = 0; line < lines; line++) {
		/* calculate next starting position */
		while (offset && offset >= sg_dma_len(sg)) {
			offset -= sg_dma_len(sg);
			sg++;
		}
		if (bpl <= sg_dma_len(sg) - offset) {
			/* fits into current chunk */
			*(rp++) = cpu_to_le32(RISC_LINESTART |
					      (offset<<12) | bpl);
			*(rp++) = cpu_to_le32(sg_dma_address(sg) + offset);
			offset += bpl;
		} else {
			/*
			 * scanline needs to be split.  Put the start in
			 * whatever memory remains using RISC_LINESTART,
			 * then the remainder into following addresses
			 * given by the scatter-gather list.
			 */
			todo = bpl;	/* one full line to be done */
			/* first fragment */
			*(rp++) = cpu_to_le32(RISC_LINESTART |
					(offset<<12) |
					(sg_dma_len(sg) - offset));
			*(rp++) = cpu_to_le32(sg_dma_address(sg) + offset);
			todo -= (sg_dma_len(sg) - offset);
			sg++;
			/* succeeding fragments have no offset */
			while (todo > sg_dma_len(sg)) {
				*(rp++) = cpu_to_le32(RISC_INLINE |
					  sg_dma_len(sg));
				*(rp++) = cpu_to_le32(sg_dma_address(sg));
				todo -= sg_dma_len(sg);
				sg++;
			}
			/* final chunk - offset 0, count 'todo' */
			*(rp++) = cpu_to_le32(RISC_INLINE | todo);
			*(rp++) = cpu_to_le32(sg_dma_address(sg));
			offset = todo;
		}
		offset += padding;
		/* If this line needs an interrupt, put it in */
		if (lpi && line > 0 && !(line % lpi))
			*(rp-2) |= RISC_INT_BIT;
	}

	return rp;
}

/**
 * tw68_risc_buffer
 *
 * 	This routine is called by tw68-video.  It allocates
 * 	memory for the dma controller "program" and then fills in that
 * 	memory with the appropriate "instructions".
 *
 * 	@pci_dev	structure with info about the pci
 * 			slot which our device is in.
 * 	@risc		structure with info about the memory
 * 			used for our controller program.
 * 	@sglist		scatter-gather list entry
 * 	@top_offset	offset within the risc program area for the
 * 			first odd frame line
 * 	@bottom_offset	offset within the risc program area for the
 * 			first even frame line
 * 	@bpl		number of data bytes per scan line
 * 	@padding	number of extra bytes to add at end of line
 * 	@lines		number of scan lines
 */
int tw68_risc_buffer(struct pci_dev *pci, struct btcx_riscmem *risc,
		     struct scatterlist *sglist,
		     unsigned int top_offset, unsigned int bottom_offset,
		     unsigned int bpl, unsigned int padding,
		     unsigned int lines)
{
	u32 instructions, fields;
	__le32 *rp;
	int rc;

	fields = 0;
	if (UNSET != top_offset)
		fields++;
	if (UNSET != bottom_offset)
		fields++;

	/* estimate risc mem: worst case is one write per page border +
	   one write per scan line + syncs + jump (all 2 dwords).  Padding
	   can cause next bpl to start close to a page border.  First DMA
	   region may be smaller than PAGE_SIZE */
	instructions  = fields * (1 + (((bpl + padding) * lines) /
			 PAGE_SIZE) + lines) + 2;
	if ((rc = btcx_riscmem_alloc(pci,risc,instructions*8)) < 0)
		return rc;

	/* write risc instructions */
	rp = risc->cpu;
	if (UNSET != top_offset)	/* generates SYNCO */
		rp = tw68_risc_field(rp, sglist, top_offset, 1,
				     bpl, padding, lines, 0);
	if (UNSET != bottom_offset)	/* generates SYNCE */
		rp = tw68_risc_field(rp, sglist, bottom_offset, 2,
				     bpl, padding, lines, 0);

	/* save pointer to jmp instruction address */
	risc->jmp = rp;
	/* assure risc buffer hasn't overflowed */
	BUG_ON((risc->jmp - risc->cpu + 2) * sizeof (*risc->cpu) > risc->size);
	return 0;
}

/* ------------------------------------------------------------------ */
/* debug helper code                                                  */

static void tw68_risc_decode(u32 risc, u32 addr)
{
#define	RISC_OP(reg)	(((reg) >> 28) & 7)
	static struct instr_details {
		char *name;
		u8 has_data_type;
		u8 has_byte_info;
		u8 has_addr;
	} instr[8] = {
		[ RISC_OP(RISC_SYNCO) ] = { "syncOdd", 0, 0, 0 },
		[ RISC_OP(RISC_SYNCE) ] = { "syncEven", 0, 0, 0 },
		[ RISC_OP(RISC_JUMP)  ] = { "jump", 0, 0, 1 },
		[ RISC_OP(RISC_LINESTART) ] = { "lineStart", 1, 1, 1 },
		[ RISC_OP(RISC_INLINE) ]    = { "inline", 1, 1, 1 },
	};
	u32 p;

	p = RISC_OP(risc);
	if (!(risc & 0x80000000) || !instr[p].name) {
		printk("0x%08x [ INVALID ]\n", risc);
		return;
	}
	printk("0x%08x %-9s IRQ=%d",
		risc, instr[p].name, (risc >> 27) & 1);
	if (instr[p].has_data_type) {
		printk(" Type=%d", (risc >> 24) & 7);
	}
	if (instr[p].has_byte_info) {
		printk(" Start=0x%03x Count=%03u",
			(risc >> 12) & 0xfff, risc & 0xfff);
	}
	if (instr[p].has_addr) {
		printk(" StartAddr=0x%08x", addr);
	}
	printk("\n");
}

void tw68_risc_program_dump(struct tw68_core *core,
			    struct btcx_riscmem *risc)
{
	__le32 *addr;

	printk(KERN_DEBUG "%s: risc_program_dump: risc=%p, "
			  "risc->cpu=0x%p, risc->jmp=0x%p\n",
			  core->name, risc, risc->cpu, risc->jmp);
	for (addr = risc->cpu; addr <=risc->jmp; addr += 2) {
		tw68_risc_decode(*addr, *(addr+1));
	}
}
EXPORT_SYMBOL(tw68_risc_program_dump);

/*
 * tw68_risc_stopper
 * 	The 'risc_stopper' acts as a switch to direct the risc code
 * 	to the buffer at the head of the chain of active buffers.
 *
 * 	For the initial implementation, the "stopper" program is a
 * 	simple jump-to-self.
 */
int tw68_risc_stopper(struct pci_dev *pci, struct btcx_riscmem *risc)
{
	__le32 *rp;
	int rc;

	if ((rc = btcx_riscmem_alloc(pci, risc, 4*4)) < 0)
		return rc;

	/* write risc inststructions */
	rp = risc->cpu;
	*(rp++) = cpu_to_le32(RISC_JUMP);
	*(rp++) = cpu_to_le32(risc->dma);
	risc->jmp = risc->cpu;
	return 0;
}
EXPORT_SYMBOL(tw68_risc_stopper);

/*
 * tw68_free_buffer
 *
 * 	Called by tw68-video.
 *
 */
void
tw68_free_buffer(struct videobuf_queue *q, struct tw68_buffer *buf)
{
	struct videobuf_dmabuf *dma=videobuf_to_dma(&buf->vb);

	BUG_ON(in_interrupt());
	videobuf_waiton(&buf->vb,0,0);
	videobuf_dma_unmap(q, dma);
	videobuf_dma_free(dma);
	btcx_riscmem_free(to_pci_dev(q->dev), &buf->risc);
	buf->vb.state = VIDEOBUF_NEEDS_INIT;
}

void tw68_wakeup(struct tw68_core *core,
		 struct tw68_dmaqueue *q, u32 count)
{
	struct tw68_buffer *buf;
#if 0
	int bc;

printk(KERN_DEBUG "Entered %s\n", __func__);
	for (bc = 0;; bc++) {
		if (list_empty(&q->active))
{
printk(KERN_DEBUG "%s: q->active empty\n", __func__);
			break;
}
#endif
if (list_empty(&q->active)) {
	del_timer(&q->timeout);
	return;
}
		buf = list_entry(q->active.next,
				 struct tw68_buffer, vb.queue);
#if 0
		/* count comes from the hw and is 16 bits wide --
		 * this trick handles wrap-arounds correctly for
		 * up to 32767 buffers in flight... */
		if ((s16) (count - buf->count) < 0)
{
printk(KERN_DEBUG "%s: count is %d, buf->count is %d\n",
  __func__, count, buf->count);
			break;
}
#endif
		do_gettimeofday(&buf->vb.ts);
		dprintk(2,"[%p/%d] wakeup reg=%d buf=%d\n",buf,buf->vb.i,
			count, buf->count);
		buf->vb.state = VIDEOBUF_DONE;
		list_del(&buf->vb.queue);
		wake_up(&buf->vb.done);
#if 0
	}
#endif
	if (list_empty(&q->active)) {
		del_timer(&q->timeout);
	} else {
		mod_timer(&q->timeout, jiffies+BUFFER_TIMEOUT);
	}
#if 0
	if (bc != 1)
		dprintk(2, "%s: %d buffers handled (should be 1)\n",
			__func__, bc);
#endif
}

void tw68_shutdown(struct tw68_core *core)
{
	/* disable RISC controller + interrupts */
	tw_clear(TW68_DMAC, TW68_DMAP_EN | TW68_FIFO_EN);
	core->pci_irqmask &= ~TW68_VID_INTS;
	tw_write(TW68_INTMASK, 0x0);
}

int tw68_reset(struct tw68_core *core)
{
	tw68_shutdown(core);

	tw_write(TW68_ACNTL, 0x80);
	/* wait a bit */
	msleep(100);

	/* FIXME - check if any of the following need to be taken care of 
	 * 	agc enable
	 *	agc gain
	 *	adaptibe agc
	 *	chroma agc
	 *	ckillen
	 *	color control
	 */

	return 0;
}

/* ------------------------------------------------------------------ */

static unsigned int inline norm_swidth(v4l2_std_id norm)
{
	return (norm & (V4L2_STD_MN & ~V4L2_STD_PAL_Nc)) ? 754 : 922;
}

static unsigned int inline norm_hdelay(v4l2_std_id norm)
{
	return (norm & (V4L2_STD_MN & ~V4L2_STD_PAL_Nc)) ? 135 : 186;
}

static unsigned int inline norm_vdelay(v4l2_std_id norm)
{
	return (norm & V4L2_STD_625_50) ? 0x24 : 0x18;
}

static unsigned int inline norm_fsc8(v4l2_std_id norm)
{
	if (norm & V4L2_STD_PAL_M)
		return 28604892;      // 3.575611 MHz

	if (norm & (V4L2_STD_PAL_Nc))
		return 28656448;      // 3.582056 MHz

	if (norm & V4L2_STD_NTSC) // All NTSC/M and variants
		return 28636360;      // 3.57954545 MHz +/- 10 Hz

	/* SECAM have also different sub carrier for chroma,
	   but step_db and step_dr, at tw68_set_tvnorm already handles that.

	   The same FSC applies to PAL/BGDKIH, PAL/60, NTSC/4.43 and PAL/N
	 */

	return 35468950;      // 4.43361875 MHz +/- 5 Hz
}

static unsigned int inline norm_htotal(v4l2_std_id norm)
{

	unsigned int fsc4=norm_fsc8(norm)/2;

	/* returns 4*FSC / vtotal / frames per seconds */
	return (norm & V4L2_STD_625_50) ?
				((fsc4+312)/625+12)/25 :
				((fsc4+262)/525*1001+15000)/30000;
}

#if 0
static unsigned int inline norm_vbipack(v4l2_std_id norm)
{
	return (norm & V4L2_STD_625_50) ? 511 : 400;
}
#endif

int tw68_set_scale(struct tw68_core *core, unsigned int width, unsigned int height,
		   enum v4l2_field field)
{
	unsigned int swidth  = norm_swidth(core->tvnorm);
	unsigned int sheight = norm_maxh(core->tvnorm);

	/* FIXME - Should values be u8? */
	u32 value;
	u32 comb_val;

	dprintk(2, "set_scale: %dx%d [%s%s,%s] scaled %dx%d\n", width, height,
		V4L2_FIELD_HAS_TOP(field)    ? "T" : "",
		V4L2_FIELD_HAS_BOTTOM(field) ? "B" : "",
		v4l2_norm_to_name(core->tvnorm), swidth, sheight);
	if (!V4L2_FIELD_HAS_BOTH(field))
		height *= 2;

	// recalc H/V active and delay registers
	// FIXME - check why and with 0xfe

	comb_val = (width & 0x300) >> 8;
	tw_write(TW68_HACTIVE_LO, width & 0xff);
	value = (width * norm_hdelay(core->tvnorm)) / swidth;
	comb_val |= (value & 0x300) >> 6;
	tw_write(TW68_HDELAY_LO, value & 0xfe);

	comb_val = (sheight & 0x300) >> 4;
	tw_write(TW68_VACTIVE_LO, sheight & 0xff);
	value = norm_vdelay(core->tvnorm);
	comb_val |= (value & 0x300) >> 2;
	tw_write(TW68_VDELAY_LO, value & 0xff);

	tw_write(TW68_CROP_HI, comb_val);

#if 0
	value = (swidth * 4096 / width) - 4096;
	comb_val = value >> 8;
	tw_write(TW68_HSCALE_LO, value & 0xff);
	printk(KERN_DEBUG "set_scale: hscale  0x%04x\n", value);
	value = (0x10000 - (sheight * 512 / height - 512)) & 0x1fff;
	comb_val |= (value & 0xf00) >> 4;
	tw_write(TW68_VSCALE_LO, value & 0xff);
	printk(KERN_DEBUG "set_scale: vscale  0x%04x\n", value);
#endif
	value = (swidth * 256) / width;
	comb_val = value >> 8;
	tw_write(TW68_HSCALE_LO, value & 0xff);

	value = (sheight * 256) / height;
	comb_val |= (value & 0xf00) >> 4;
	tw_write(TW68_VSCALE_LO, value & 0xff);
	tw_write(TW68_SCALE_HI, comb_val);
printk(KERN_DEBUG "hscaleLo=%d, vscaleLo=%d, comb=%d(0x%04x)\n",
   (swidth*256)/width, value, comb_val, comb_val);

#if 0
	/* todo -  setup filters */
	value = (1 << 19);        // CFILT (default)
	if (core->tvnorm & V4L2_STD_SECAM) {
		value |= (1 << 15);
		value |= (1 << 16);
	}
	if (INPUT(core->input).type == TW68_VMUX_SVIDEO)
		value |= (1 << 13) | (1 << 5);
	if (V4L2_FIELD_INTERLACED == field)
		value |= (1 << 3); // VINT (interlaced vertical scaling)
	if (width < 385)
		value |= (1 << 0); // 3-tap interpolation
	if (width < 193)
		value |= (1 << 1); // 5-tap interpolation
	if (nocomb)
		value |= (3 << 5); // disable comb filter

	printk(KERN_DEBUG "set_scale: filter  0x%04x\n", value);
#endif

	return 0;
}

int tw68_set_tvnorm(struct tw68_core *core, v4l2_std_id norm)
{
	u32 twiformat;

printk(KERN_DEBUG "Entered %s\n", __func__);
	core->tvnorm = norm;

	if (norm & V4L2_STD_NTSC_M_JP) {
		twiformat = VideoFormatNTSCJapan;
	} else if (norm & V4L2_STD_NTSC_443) {
		twiformat = VideoFormatNTSC443;
	} else if (norm & V4L2_STD_PAL_M) {
		twiformat = VideoFormatPALM;
	} else if (norm & V4L2_STD_PAL_N) {
		twiformat = VideoFormatPALN;
	} else if (norm & V4L2_STD_PAL_Nc) {
		twiformat = VideoFormatPALNC;
	} else if (norm & V4L2_STD_PAL_60) {
		twiformat = VideoFormatPAL60;
	} else if (norm & V4L2_STD_NTSC) {
		twiformat = VideoFormatNTSC;
	} else if (norm & V4L2_STD_SECAM) {
		twiformat = VideoFormatSECAM;
	} else { /* PAL */
		twiformat = VideoFormatPAL;
	}

	printk(KERN_DEBUG "set_tvnorm: TW68_SDT  0x%08x [old=0x%08x]\n",
		twiformat, (tw_read(TW68_SDT) >> 4) & 0x07);

	tw_andor(TW68_SDT, 0x07, twiformat);

#if 0
	// FIXME: as-is from DScaler
	printk(KERN_DEBUG "set_tvnorm: MO_OUTPUT_FORMAT 0x%08x [old=0x%08x]\n",
		twoformat, tw_read(MO_OUTPUT_FORMAT));
//	tw_write(MO_OUTPUT_FORMAT, twoformat);

	// MO_SCONV_REG = adc clock / video dec clock * 2^17
	tmp64  = adc_clock * (u64)(1 << 17);
	do_div(tmp64, vdec_clock);
	printk(KERN_DEBUG "set_tvnorm: MO_SCONV_REG     0x%08x [old=0x%08x]\n",
		(u32)tmp64, tw_read(MO_SCONV_REG));
	tw_write(MO_SCONV_REG, (u32)tmp64);

	// MO_SUB_STEP = 8 * fsc / video dec clock * 2^22
	tmp64  = step_db * (u64)(1 << 22);
	do_div(tmp64, vdec_clock);
	printk(KERN_DEBUG "set_tvnorm: MO_SUB_STEP      0x%08x [old=0x%08x]\n",
		(u32)tmp64, tw_read(MO_SUB_STEP));
	tw_write(MO_SUB_STEP, (u32)tmp64);

	// MO_SUB_STEP_DR = 8 * 4406250 / video dec clock * 2^22
	tmp64  = step_dr * (u64)(1 << 22);
	do_div(tmp64, vdec_clock);
	printk(KERN_DEBUG "set_tvnorm: MO_SUB_STEP_DR   0x%08x [old=0x%08x]\n",
		(u32)tmp64, tw_read(MO_SUB_STEP_DR));
	tw_write(MO_SUB_STEP_DR, (u32)tmp64);

	// bdelay + agcdelay
	bdelay   = vdec_clock * 65 / 20000000 + 21;
	agcdelay = vdec_clock * 68 / 20000000 + 15;
	printk(KERN_DEBUG "set_tvnorm: MO_AGC_BURST     0x%08x [old=0x%08x,bdelay=%d,agcdelay=%d]\n",
		(bdelay << 8) | agcdelay, tw_read(MO_AGC_BURST), bdelay, agcdelay);
	tw_write(MO_AGC_BURST, (bdelay << 8) | agcdelay);

	// htotal
	tmp64 = norm_htotal(norm) * (u64)vdec_clock;
	do_div(tmp64, fsc8);
	htotal = (u32)tmp64 | (HLNotchFilter4xFsc << 11);
	printk(KERN_DEBUG "set_tvnorm: MO_HTOTAL        0x%08x [old=0x%08x,htotal=%d]\n",
		htotal, tw_read(MO_HTOTAL), (u32)tmp64);
	tw_write(MO_HTOTAL, htotal);

#endif

	// this is needed as well to set all tvnorm parameter
	tw68_set_scale(core, 320, 240, V4L2_FIELD_INTERLACED);

	// done
	return 0;
}

/* ------------------------------------------------------------------ */

struct video_device *tw68_vdev_init(struct tw68_core *core,
				    struct pci_dev *pci,
				    struct video_device *template,
				    char *type)
{
	struct video_device *vfd;

	vfd = video_device_alloc();
	if (NULL == vfd)
		return NULL;
	*vfd = *template;
	vfd->minor   = -1;
	vfd->parent  = &pci->dev;
	vfd->release = video_device_release;
	snprintf(vfd->name, sizeof(vfd->name), "%s %s (%s)",
		 core->name, type, core->board.name);
	return vfd;
}

struct tw68_core* tw68_core_get(struct pci_dev *pci)
{
	struct tw68_core *core;

printk(KERN_DEBUG "Entered %s\n", __func__);
	mutex_lock(&devlist);
	list_for_each_entry(core, &tw68_devlist, devlist) {
		if (pci->bus->number != core->pci_bus)
			continue;
		if (PCI_SLOT(pci->devfn) != core->pci_slot)
			continue;

		if (0 != tw68_get_resources(core, pci)) {
			mutex_unlock(&devlist);
			return NULL;
		}
		atomic_inc(&core->refcount);
		mutex_unlock(&devlist);
		return core;
	}

	core = tw68_core_create(pci, tw68_devcount);
	if (NULL != core) {
		tw68_devcount++;
		list_add_tail(&core->devlist, &tw68_devlist);
	}

	mutex_unlock(&devlist);
	return core;
}

void tw68_core_put(struct tw68_core *core, struct pci_dev *pci)
{
printk(KERN_DEBUG "Entered %s\n", __func__);
	release_mem_region(pci_resource_start(pci,0),
			   pci_resource_len(pci,0));

	if (!atomic_dec_and_test(&core->refcount))
		return;

	mutex_lock(&devlist);
	list_del(&core->devlist);
	iounmap(core->lmmio);
	tw68_devcount--;
	mutex_unlock(&devlist);
	kfree(core);
}

/* ------------------------------------------------------------------ */

EXPORT_SYMBOL(tw68_wakeup);
EXPORT_SYMBOL(tw68_reset);
EXPORT_SYMBOL(tw68_shutdown);

EXPORT_SYMBOL(tw68_risc_buffer);
EXPORT_SYMBOL(tw68_free_buffer);

EXPORT_SYMBOL(tw68_set_tvnorm);
EXPORT_SYMBOL(tw68_set_scale);

EXPORT_SYMBOL(tw68_vdev_init);
EXPORT_SYMBOL(tw68_core_get);
EXPORT_SYMBOL(tw68_core_put);
