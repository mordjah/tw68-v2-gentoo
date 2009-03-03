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
 *		Note: IRQ to be generated _after_ lpi lines are transferred
 */
static __le32* tw68_risc_field(__le32 *rp, struct scatterlist *sglist,
			    unsigned int offset, u32 sync_line,
			    unsigned int bpl, unsigned int padding,
			    unsigned int lines, unsigned int lpi)
{
	struct scatterlist *sg;
	unsigned int line,todo,done;

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
					      /* (offset<<12) |*/  bpl);
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
			done = (sg_dma_len(sg) - offset);
			*(rp++) = cpu_to_le32(RISC_LINESTART |
						(7 << 24) |
						done);
			*(rp++) = cpu_to_le32(sg_dma_address(sg) + offset);
			todo -= done;
			sg++;
			/* succeeding fragments have no offset */
			while (todo > sg_dma_len(sg)) {
				*(rp++) = cpu_to_le32(RISC_INLINE |
						(done << 12) |
						sg_dma_len(sg));
				*(rp++) = cpu_to_le32(sg_dma_address(sg));
				todo -= sg_dma_len(sg);
				sg++;
				done += sg_dma_len(sg);
			}
			/* final chunk - offset 0, count 'todo' */
			*(rp++) = cpu_to_le32(RISC_INLINE |
						(done << 12) |
						todo);
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
int tw68_risc_buffer(struct pci_dev *pci,
			struct btcx_riscmem *risc,
			struct scatterlist *sglist,
			unsigned int top_offset,
			unsigned int bottom_offset,
			unsigned int bpl,
			unsigned int padding,
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

	/*
	 * estimate risc mem: worst case is one write per page border +
	 * one write per scan line + syncs + jump (all 2 dwords).
	 * Padding can cause next bpl to start close to a page border.
	 * First DMA region may be smaller than PAGE_SIZE
	 */
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

#if 0
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
EXPORT_SYMBOL_GPL(tw68_risc_program_dump);
#endif

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
EXPORT_SYMBOL_GPL(tw68_risc_stopper);

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

/*
 * I just didn't understand what some of the original code was doing
 * here, so I commented out those parts.
 */
void tw68_wakeup(struct tw68_core *core,
		 struct tw68_dmaqueue *q, u32 count)
{
	struct tw68_buffer *buf;
#if 0
	int bc;

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
	buf = list_entry(q->active.next, struct tw68_buffer, vb.queue);
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
	if (list_empty(&q->active)) {
		del_timer(&q->timeout);
	} else {
#endif
	mod_timer(&q->timeout, jiffies+BUFFER_TIMEOUT);
#if 0
	}
	if (bc != 1)
		dprintk(2, "%s: %d buffers handled (should be 1)\n",
			__func__, bc);
#endif
}

void tw68_shutdown(struct tw68_core *core)
{
	/* disable RISC controller + interrupts */
	tw_clearl(TW68_DMAC, TW68_DMAP_EN | TW68_FIFO_EN);
	core->pci_irqmask &= ~TW68_VID_INTS;
	tw_writel(TW68_INTMASK, 0x0);
}

int tw68_reset(struct tw68_core *core)
{
	tw68_shutdown(core);
	/* clear any pending interrupts */
	tw_writel(TW68_INTSTAT, 0xffffffff);
	/* disable GPIO outputs */
//	tw_writel(TW68_GPOE, 0);
	tw_writeb(TW68_ACNTL, 0x80);	/* device reset */
	/* wait a bit */
	msleep(100);

	tw_writeb(TW68_INFORM, 0x40);
	tw_writeb(TW68_OPFORM, 0x04);
	tw_writeb(TW68_HSYNC, 0);
	tw_writeb(TW68_ACNTL, 0x42);
	tw_writeb(TW68_CNTRL1, 0xcc);

	tw_writeb(TW68_CROP_HI, 0x02);
	tw_writeb(TW68_VDELAY_LO, 0x18);
	tw_writeb(TW68_VACTIVE_LO, 0xf0);
	tw_writeb(TW68_HDELAY_LO, 0x0f);
	tw_writeb(TW68_HACTIVE_LO, 0xd0);
	tw_writeb(TW68_VSCALE_LO, 0);
	tw_writeb(TW68_SCALE_HI, 0x11);
	tw_writeb(TW68_HSCALE_LO, 0);

	/*
	 *  Following the bttv patches, we use the separate registers
	 *  for the second field.  However, we initialize them exactly
	 *  the same as the primary ones, since that's what's done
	 *  when they are modified at run-time.
	 */
	tw_writeb(TW68_F2CNT, 0x01);
	tw_writeb(TW68_F2CROP_HI, 0x02);
	tw_writeb(TW68_F2VDELAY_LO, 0x18);
	tw_writeb(TW68_F2VACTIVE_LO, 0xf0);
	tw_writeb(TW68_F2HDELAY_LO, 0x0f);
	tw_writeb(TW68_F2HACTIVE_LO, 0xd0);
	tw_writeb(TW68_F2VSCALE_LO, 0);
	tw_writeb(TW68_F2SCALE_HI, 0x11);
	tw_writeb(TW68_F2HSCALE_LO, 0);

	tw_writeb(TW68_BRIGHT, 0);
	tw_writeb(TW68_CONTRAST, 0x5c);
	tw_writeb(TW68_SHARPNESS, 0x98);
	tw_writeb(TW68_SAT_U, 0x80);
	tw_writeb(TW68_SAT_V, 0x80);
	tw_writeb(TW68_HUE, 0);
	tw_writeb(TW68_SHARP2, 0xc6);
	tw_writeb(TW68_VSHARP, 0x84);
	tw_writeb(TW68_CORING, 0x44);
	tw_writeb(TW68_CC_STATUS, 0x0a);
	tw_writeb(TW68_SDT, 0x07);
	tw_writeb(TW68_SDTR, 0x7f);
	tw_writeb(TW68_RESERV2, 0x07);	/* FIXME - why? */
	tw_writeb(TW68_RESERV3, 0x7f);	/* FIXME - why? */
	tw_writeb(TW68_CLMPG, 0x50);
	tw_writeb(TW68_IAGC, 0x42);
	tw_writeb(TW68_AGCGAIN, 0xf0);
	tw_writeb(TW68_PEAKWT, 0xd8);
	tw_writeb(TW68_CLMPL, 0xbc);
	tw_writeb(TW68_SYNCT, 0xb8);
	tw_writeb(TW68_MISSCNT, 0x44);
	tw_writeb(TW68_PCLAMP, 0x2a);
	tw_writeb(TW68_VERTCTL, 0);
	tw_writeb(TW68_VERTCTL2, 0);
	tw_writeb(TW68_COLORKILL, 0x78);
	tw_writeb(TW68_COMB, 0x44);
	tw_writeb(TW68_LDLY, 0x30);
	tw_writeb(TW68_MISC1, 0x14);
	tw_writeb(TW68_LOOP, 0xa5);
	tw_writeb(TW68_MISC2, 0xe0);
	tw_writeb(TW68_MACROVISION, 0);
	tw_writeb(TW68_CLMPCTL2, 0);
	tw_writeb(TW68_FILLDATA, 0xa0);
	tw_writeb(TW68_CLMD, 0x05);
	tw_writeb(TW68_IDCNTL, 0);
	tw_writeb(TW68_CLCNTL1, 0);
	tw_writeb(TW68_SLICELEVEL, 0);
	tw_writel(TW68_VBIC, 0x03);
	tw_writel(TW68_CAP_CTL, 0x43);
	tw_writel(TW68_DMAC, 0x2000);	/* patch set had 0x2080 */
	tw_writel(TW68_TESTREG, 0);

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

EXPORT_SYMBOL_GPL(tw68_wakeup);
EXPORT_SYMBOL_GPL(tw68_reset);
EXPORT_SYMBOL_GPL(tw68_shutdown);

EXPORT_SYMBOL_GPL(tw68_risc_buffer);
EXPORT_SYMBOL_GPL(tw68_free_buffer);

EXPORT_SYMBOL_GPL(tw68_vdev_init);
EXPORT_SYMBOL_GPL(tw68_core_get);
EXPORT_SYMBOL_GPL(tw68_core_put);
