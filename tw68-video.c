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
#include <linux/kmod.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <asm/div64.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>

#include "tw68.h"

MODULE_DESCRIPTION("v4l2 driver module for tw6800 based video capture cards");
MODULE_AUTHOR("William M. Brack <wbrack@mmm.com.hk>");
MODULE_LICENSE("GPL");

/* ------------------------------------------------------------------ */

/*
 *  FIXME - need to cater for cross-register values, e.g. AGCGAIN
 *  	with routines used by 'get' and 'set' controls.
 */
static unsigned int video_nr[] = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };

module_param_array(video_nr, int, NULL, 0444);

MODULE_PARM_DESC(video_nr,"video device numbers");

static unsigned int video_debug;
module_param(video_debug,int,0644);
MODULE_PARM_DESC(video_debug,"enable debug messages [video]");

static unsigned int irq_debug;
module_param(irq_debug,int,0644);
MODULE_PARM_DESC(irq_debug,"enable debug messages [IRQ handler]");

static unsigned int vid_limit = 16;
module_param(vid_limit,int,0644);
MODULE_PARM_DESC(vid_limit,"capture memory limit in megabytes");

#define	dprintk(level, fmt, arg...)	if (video_debug >= level) \
	printk(KERN_DEBUG "%s/0: " fmt, core->name , ## arg)
#define iprintk(level, fmt, arg...)	if (irq_debug >= level) \
	printk(KERN_DEBUG "%s/0: " fmt, core->name , ## arg)

/* ------------------------------------------------------------------ */

static LIST_HEAD(tw6800_devlist);

/* ------------------------------------------------------------------- */
/* static data                                                         */

/*
 * The TW6801 video block supports the following formats:
 *	RGB32 RGB24 RGB16 RGB15 YUY2
 * Additionally, the four bytes of DWORDs containing the pixels can be
 * swapped in various manners.
 */
static struct tw6800_fmt formats[] = {
	{	/* TODO - confirm the next two aren't reversed */
		.name     = "32 bpp RGB, le",
		.fourcc   = V4L2_PIX_FMT_BGR32,
		.twformat = ColorFormatRGB32,
		.depth    = 32,
		.flags    = FORMAT_FLAGS_PACKED,
	},{
		.name     = "32 bpp RGB, be",
		.fourcc   = V4L2_PIX_FMT_RGB32,
		.twformat = ColorFormatRGB32 | ColorFormatBSWAP |
			    ColorFormatWSWAP,
		.depth    = 32,
		.flags    = FORMAT_FLAGS_PACKED,
	},{
		.name     = "24 bpp RGB, le",
		.fourcc   = V4L2_PIX_FMT_BGR24,
		.twformat = ColorFormatRGB24,
		.depth    = 24,
		.flags    = FORMAT_FLAGS_PACKED,
	},{
		.name     = "16 bpp RGB, le",
		.fourcc   = V4L2_PIX_FMT_RGB565,
		.twformat = ColorFormatRGB16,
		.depth    = 16,
		.flags    = FORMAT_FLAGS_PACKED,
	},{
		.name     = "16 bpp RGB, be",
		.fourcc   = V4L2_PIX_FMT_RGB565X,
		.twformat = ColorFormatRGB16 | ColorFormatBSWAP,
		.depth    = 16,
		.flags    = FORMAT_FLAGS_PACKED,
	},{
		.name     = "15 bpp RGB, le",
		.fourcc   = V4L2_PIX_FMT_RGB555,
		.twformat = ColorFormatRGB15,
		.depth    = 16,
		.flags    = FORMAT_FLAGS_PACKED,
	},{
		.name     = "15 bpp RGB, be",
		.fourcc   = V4L2_PIX_FMT_RGB555X,
		.twformat = ColorFormatRGB15 | ColorFormatBSWAP,
		.depth    = 16,
		.flags    = FORMAT_FLAGS_PACKED,
	},{
		.name     = "4:2:2, packed, YUYV",
		.fourcc   = V4L2_PIX_FMT_YUYV,
		.twformat = ColorFormatYUY2,
		.depth    = 16,
		.flags    = FORMAT_FLAGS_PACKED,
	},{
		.name     = "4:2:2, packed, UYVY",
		.fourcc   = V4L2_PIX_FMT_UYVY,
		.twformat = ColorFormatYUY2 | ColorFormatBSWAP,
		.depth    = 16,
		.flags    = FORMAT_FLAGS_PACKED,
	},
};

/*
 * The settings for HDELAY, HACTIVE, VDELAY and VACTIVE don't seem to
 * be very obvious between different norms.  To simplify the logic, we
 * use the following table to allow their settings to be easily determined.
 * The elements of each entry are as follows:
 * 	v4l2_id		V4L2 standard - bit significant, multiple norms
 * 			can be described with a single entry.  The table
 * 			is searched in order, so the first matching entry
 * 			is the one which is used.
 * 	format		Setting for the TW6800 SDT register
 * 	swidth		Width of active video
 * 	totwidth	Total line width
 * 	hdelay		Start of active video, relative to edge of HSYNC
 * 	hactive		Number of bytes of active video
 * 	vdelay		Start of active video, relative to edge of VSYNC
 */
static struct tw68_norm norms[] = {
	{
		.v4l2_id  = V4L2_STD_NTSC_M_JP,
		.format   = VideoFormatNTSCJapan,
		.swidth   = 640,
		.sheight  = 480,
		.hdelay   = 135,
		.vdelay   = 0x16,
	},{
		.v4l2_id  = V4L2_STD_NTSC,
		.format   = VideoFormatNTSC,
		.swidth   = 768,
		.sheight  = 480,
		.hdelay   = 128,
		.vdelay   = 0x1a,
	},{
		.v4l2_id  = V4L2_STD_PAL_M,
		.format   = VideoFormatPALM,
		.swidth   = 640,
		.sheight  = 480,
		.hdelay   = 135,
		.vdelay   = 0x1a,
	},{
		.v4l2_id  = V4L2_STD_PAL_N,
		.format   = VideoFormatPALN,
//		.swidth   = 768,
		.swidth   = 720,
		.sheight  = 576,
//		.sheight  = 576,
//		.hdelay   = 186,
		.hdelay   = 15,
//		.vdelay   = 0x20,
		.vdelay   = 23,
	},{
		.v4l2_id  = V4L2_STD_PAL_Nc,
		.format   = VideoFormatPALNC,
		.swidth   = 640,
		.sheight  = 576,
		.hdelay   = 130,
		.vdelay   = 0x1a,
	},{
		.v4l2_id  = V4L2_STD_PAL_60,
		.format   = VideoFormatPAL60,
		.swidth   = 924,
		.sheight  = 480,
		.hdelay   = 186,
		.vdelay   = 0x1a,
	},{
		.v4l2_id  = V4L2_STD_PAL,
		.format   = VideoFormatPAL,
//		.swidth   = 924,
		.swidth   = 720,
		.sheight  = 576,
//		.hdelay   = 186,
		.hdelay   = 15,
//		.vdelay   = 0x20,
		.vdelay   = 0x17,
	},{
		.v4l2_id  = V4L2_STD_SECAM,
		.format   = VideoFormatSECAM,
		.swidth   = 924,
		.sheight  = 576,
		.hdelay   = 186,
		.vdelay   = 0x20,
	}
};

static struct tw6800_fmt* format_by_fourcc(unsigned int fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++)
		if (formats[i].fourcc == fourcc)
			return formats+i;
	return NULL;
}

/* ------------------------------------------------------------------- */

static const struct v4l2_queryctrl no_ctl = {
	.name  = "42",
	.flags = V4L2_CTRL_FLAG_DISABLED,
};

static struct tw68_ctrl tw6800_ctls[] = {
	/* --- video --- */
	{
		.v = {
			.id            = V4L2_CID_BRIGHTNESS,
			.name          = "Brightness",
			.minimum       = -128,
			.maximum       = 127,
			.step          = 2,
			.default_value = 0,
			.type          = V4L2_CTRL_TYPE_INTEGER,
		},
		.off                   = 0,
		.reg                   = TW68_BRIGHT,
		.mask                  = ~0,	/* 32-bit signed */
		.shift                 = 0,
	},{
		.v = {
			.id            = V4L2_CID_CONTRAST,
			.name          = "Contrast",
			.minimum       = 0,
			.maximum       = 0xff,
			.step          = 2,
			.default_value = 84,
			.type          = V4L2_CTRL_TYPE_INTEGER,
		},
		.off                   = 0,
		.reg                   = TW68_CONTRAST,
		.mask                  = 0xff,
		.shift                 = 0,
	},{
		.v = {
			.id            = V4L2_CID_HUE,
			.name          = "Hue",
			.minimum       = -90,
			.maximum       = 90,
			.step          = 1,
			.default_value = 0,
			.type          = V4L2_CTRL_TYPE_INTEGER,
		},
		.off                   = 128,
		.reg                   = TW68_HUE,
		.mask                  = 0xff,
		.shift                 = 0,
	},{
		/* strictly, this only describes only U saturation.
		 * V saturation is handled specially through code.
		 */
		.v = {
			.id            = V4L2_CID_SATURATION,
			.name          = "Saturation",
			.minimum       = 0,
			.maximum       = 0xff,
			.step          = 1,
			.default_value = 0x7f,
			.type          = V4L2_CTRL_TYPE_INTEGER,
		},
		.off                   = 0,
		.reg                   = TW68_SAT_U,
		.mask                  = 0x00ff,
		.shift                 = 0,
	},{
		.v = {
			.id            = V4L2_CID_CHROMA_AGC,
			.name          = "Chroma AGC",
			.minimum       = 0,
			.maximum       = 1,
			.default_value = 0x1,
			.type          = V4L2_CTRL_TYPE_BOOLEAN,
		},
		.off                   = 0,
		.reg                   = TW68_ACNTL,
		.mask                  = 0x10,
		.shift                 = 4,
	}, {
		.v = {
			.id            = V4L2_CID_COLOR_KILLER,
			.name          = "Color killer",
			.minimum       = 0,
			.maximum       = 1,
			.default_value = 0x1,
			.type          = V4L2_CTRL_TYPE_BOOLEAN,
		},
		.reg                   = TW68_LDLY, /* FIXME */
		.mask                  = 0x80,
		.shift                 = 7,
	}
};
static const int TW6800_CTLS = ARRAY_SIZE(tw6800_ctls);

const u32 tw68_user_ctrls[] = {
	V4L2_CID_USER_CLASS,
	V4L2_CID_BRIGHTNESS,
	V4L2_CID_CONTRAST,
	V4L2_CID_SATURATION,
	V4L2_CID_HUE,
	V4L2_CID_AUDIO_VOLUME,
	V4L2_CID_AUDIO_BALANCE,
	V4L2_CID_AUDIO_MUTE,
	V4L2_CID_CHROMA_AGC,
	V4L2_CID_COLOR_KILLER,
	0
};
EXPORT_SYMBOL_GPL(tw68_user_ctrls);

static const u32 *ctrl_classes[] = {
	tw68_user_ctrls,
	NULL
};

#if 0
/* ----------------------------------------------------------- */
/* Debugging code                                              */

static void dump_vregs (int start, int end, struct tw68_core *core)
{
	int ix, nchar, pos;
	char line[80];

	pos = 0;
	nchar = 0;
	for (ix = start; ix < end; ix += 4) {
		if (nchar == 0)
			pos = snprintf (line, 10, "0x%04x  ", ix);
		if (!(ix % 16))
			line[pos + nchar++] = ' ';
		snprintf (&line[pos + nchar], 4, "%02x ", tw_readb(ix));
		nchar += 3;
		if (nchar >= 50) {
			printk("%s\n", line);
			nchar = 0;
		}
	}
	if (nchar)
		printk("%s\n", line);
}

static void dump_pci_regs (int start, int end, struct tw68_core *core)
{
	int ix, nchar, pos;
	char line[80];

	pos = 0;
	nchar = 0;
	for (ix = start; ix < end; ix += 4) {
		if (nchar == 0)
			pos = snprintf (line, 10, "0x%04x   ", ix);
		snprintf (&line[pos + nchar], 10, "%08x ", tw_readl(ix));
		nchar += 9;
		if (nchar >= 72) {
			printk("%s\n", line);
			nchar = 0;
		}
	}
	if (nchar)
		printk("%s\n", line);
}
#endif
/* ----------------------------------------------------------- */
/* tv norms                                                    */

/*
 * tw68_set_scale
 *
 * Scaling and Cropping for video decoding
 *
 * We are working with 3 values for horizontal and vertical - scale,
 * delay and active.  The TW6802 datasheet says the unscaled image is:
 * 		Total pixels	HDELAY	HACTIVE
 * 	NTSC	   858		  106	  720
 * 	PAL	   864		  108	  720
 *
 * HACTIVE represent the actual number of pixels in the "usable" image,
 * before scaling.  HDELAY represents the number of pixels skipped
 * between the start of the horizontal sync and the start of the image.
 * HSCALE is calculated using the formula
 * 	HSCALE = (720 / HACTIVE) * 256
 *
 * The vertical registers are similar, except based upon the total number
 * of lines in the image, and the first line of the image (i.e. ignoring
 * vertical sync and VBI).
 *
 * Note that the number of bytes reaching the FIFO (and hence needing
 * to be processed by the DMAP program) is completely dependent upon
 * these values, especially HSCALE.
 *
 * Parameters:
 * 	@core		pointer to the core structure, needed for
 * 			getting current norm (as well as debug print)
 * 	@width		actual image width (from user buffer)
 * 	@height		actual image height
 * 	@field		indicates Top, Bottom or Interlaced
 */
int tw68_set_scale(struct tw68_core *core, unsigned int width,
		unsigned int height, enum v4l2_field field)
{

	/* set indidually for debugging clarity */
	int hactive, hdelay, hscale;
	int vactive, vdelay, vscale;
	int comb;

	if (!V4L2_FIELD_HAS_BOTH(field))
		height *= 2;

	hactive = core->tvnorm->swidth;
	hdelay = core->tvnorm->hdelay;
	hscale = (hactive * 256) / width;

	vactive = core->tvnorm->sheight;
	vdelay = core->tvnorm->vdelay;
	vscale = (vactive * 256) / height;

	dprintk(2, "set_scale: %dx%d [%s%s,%s]\n", width, height,
		V4L2_FIELD_HAS_TOP(field)    ? "T" : "",
		V4L2_FIELD_HAS_BOTTOM(field) ? "B" : "",
		v4l2_norm_to_name(core->tvnorm->v4l2_id));
	dprintk(2, "set_scale: hactive=%d, hdelay=%d, hscale=%d; "
		   "vactive=%d, vdelay=%d, vscale=%d\n",
		   hactive, hdelay, hscale, vactive, vdelay, vscale);

	comb =	((vdelay & 0x300)  >> 2) |
		((vactive & 0x300) >> 4) |
		((hdelay & 0x300)  >> 6) |
		((hactive & 0x300) >> 8);	
	tw_writeb(TW68_CROP_HI, comb);
	tw_writeb(TW68_F2CROP_HI, comb);
	tw_writeb(TW68_VDELAY_LO, vdelay & 0xff);
	tw_writeb(TW68_F2VDELAY_LO, vdelay & 0xff);
	tw_writeb(TW68_VACTIVE_LO, vactive & 0xff);
	tw_writeb(TW68_F2VACTIVE_LO, vactive & 0xff);
	tw_writeb(TW68_HDELAY_LO, hdelay & 0xff);
	tw_writeb(TW68_F2HDELAY_LO, hdelay & 0xff);
	tw_writeb(TW68_HACTIVE_LO, hactive & 0xff);
	tw_writeb(TW68_F2HACTIVE_LO, hactive & 0xff);

	comb = ((vscale & 0xf00) >> 4) | ((hscale & 0xf00) >> 8);
	tw_writeb(TW68_SCALE_HI, comb);
	tw_writeb(TW68_F2SCALE_HI, comb);
	tw_writeb(TW68_VSCALE_LO, vscale);
	tw_writeb(TW68_F2VSCALE_LO, vscale);
	tw_writeb(TW68_HSCALE_LO, hscale);
	tw_writeb(TW68_F2HSCALE_LO, hscale);

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
EXPORT_SYMBOL_GPL(tw68_set_scale);

int tw68_set_tvnorm(struct tw68_core *core, v4l2_std_id norm)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(norms); i++) {
		if (norms[i].v4l2_id & norm)
			break;
	}
	if (i == ARRAY_SIZE(norms))
		return -EINVAL;

	core->tvnorm = &norms[i];
	tw_andorb(TW68_SDT, 0x07, norms[i].format);
	tw_andorb(TW68_RESERV2, 0x07, norms[i].format);

	// this is needed as well to set all tvnorm parameter
//	tw68_set_scale(core, 320, 240, V4L2_FIELD_INTERLACED);

	// done
	return 0;
}
EXPORT_SYMBOL_GPL(tw68_set_tvnorm);

int tw6800_ctrl_query(struct tw68_core *core, struct v4l2_queryctrl *qctrl)
{
	int i;

	if (qctrl->id < V4L2_CID_BASE ||
	    qctrl->id >= V4L2_CID_LASTP1)
		return -EINVAL;
	for (i = 0; i < TW6800_CTLS; i++)
		if (tw6800_ctls[i].v.id == qctrl->id)
			break;
	if (i == TW6800_CTLS) {
		*qctrl = no_ctl;
		return 0;
	}
	*qctrl = tw6800_ctls[i].v;

	/* Report chroma AGC as inactive when SECAM is selected */
	if (tw6800_ctls[i].v.id == V4L2_CID_CHROMA_AGC &&
	    core->tvnorm->v4l2_id & V4L2_STD_SECAM)
		qctrl->flags |= V4L2_CTRL_FLAG_INACTIVE;

	return 0;
}
EXPORT_SYMBOL_GPL(tw6800_ctrl_query);

/* ------------------------------------------------------------------- */
/* resource management                                                 */

static int res_get(struct tw6800_dev *dev, struct tw6800_fh *fh,
		   unsigned int bit)
{
	struct tw68_core *core = dev->core;

	if (fh->resources & bit)
		/* have it already allocated */
		return 1;

	/* is it free? */
	mutex_lock(&core->lock);
	if (dev->resources & bit) {
		/* no, someone else uses it */
		mutex_unlock(&core->lock);
		return 0;
	}
	/* it's free, grab it */
	fh->resources  |= bit;
	dev->resources |= bit;
	dprintk(1,"res: get %d\n",bit);
	mutex_unlock(&core->lock);
	return 1;
}

static
int res_check(struct tw6800_fh *fh, unsigned int bit)
{
	return (fh->resources & bit);
}

static
int res_locked(struct tw6800_dev *dev, unsigned int bit)
{
	return (dev->resources & bit);
}

static
void res_free(struct tw6800_dev *dev, struct tw6800_fh *fh, unsigned int bits)
{
	struct tw68_core *core = dev->core;

	BUG_ON((fh->resources & bits) != bits);

	mutex_lock(&core->lock);
	fh->resources  &= ~bits;
	dev->resources &= ~bits;
	dprintk(1,"res: put %d\n",bits);
	mutex_unlock(&core->lock);
}

/* ------------------------------------------------------------------ */

int tw68_video_mux(struct tw68_core *core, unsigned int input)
{
/*
 * As a first try, we will do the minimum - position the input value
 * into position for setting into the INFORM register
 */
	if (input > 3)
		return -EINVAL;
	core->input = input;	/* save the value into control struct */
	dprintk(1, "tw6800: video_mux: input=%d\n", input);
	input <<= 2;	/* position value into b3-2 */
	tw_andorb(TW68_INFORM, 0x03 << 2, input);

	return 0;
}
EXPORT_SYMBOL_GPL(tw68_video_mux);

/* ------------------------------------------------------------------ */

static int start_video_dma(struct tw6800_dev    *dev,
			   struct tw68_dmaqueue *q,
			   struct tw68_buffer   *buf)
{
	struct tw68_core *core = dev->core;
	/* setup fifo + format */

	tw68_set_scale(core, buf->vb.width, buf->vb.height, buf->vb.field);
	q->count = 1;
	/* set risc starting address */
	tw_writel(TW68_DMAP_SA, cpu_to_le32(buf->risc.dma));
	/* start risc processor plus fifo and set format */
	tw_andorl(TW68_DMAC, 0x7f, buf->fmt->twformat |
		 ColorFormatGamma | TW68_DMAP_EN | TW68_FIFO_EN);
	/* enable irqs */
	core->pci_irqmask |= TW68_VID_INTS;
	tw_setl(TW68_INTMASK, core->pci_irqmask);
	return 0;
}

/* Code only included if power management is being used */
#ifdef CONFIG_PM
static int stop_video_dma(struct tw6800_dev    *dev)
{
	struct tw68_core *core = dev->core;
	core->pci_irqmask &= ~TW68_VID_INTS;
	tw_clearl(TW68_INTMASK, TW68_VID_INTS);
	tw_clearl(TW68_DMAC, TW68_DMAP_EN | TW68_FIFO_EN);
	return 0;
}
#endif

static int restart_video_queue(struct tw6800_dev    *dev,
			       struct tw68_dmaqueue *q)
{
	struct tw68_core *core = dev->core;
	struct tw68_buffer *buf, *prev;

	if (!list_empty(&q->active)) {
		buf = list_entry(q->active.next, struct tw68_buffer, vb.queue);
		dprintk(10,"restart_queue [%p/%d]: restart dma\n",
			buf, buf->vb.i);
		start_video_dma(dev, q, buf);
		list_for_each_entry(buf, &q->active, vb.queue)
			buf->count = q->count++;
		mod_timer(&q->timeout, jiffies+BUFFER_TIMEOUT);
		return 0;
	}

	prev = NULL;
	for (;;) {
		if (list_empty(&q->queued))
			return 0;
		buf = list_entry(q->queued.next, struct tw68_buffer, vb.queue);
		if (NULL == prev) {
			list_move_tail(&buf->vb.queue, &q->active);
			start_video_dma(dev, q, buf);
			buf->vb.state = VIDEOBUF_ACTIVE;
			buf->count    = q->count++;
			mod_timer(&q->timeout, jiffies+BUFFER_TIMEOUT);
			dprintk(10,"[%p/%d] restart_queue - first active\n",
				buf,buf->vb.i);

		} else if (prev->vb.width  == buf->vb.width  &&
			  prev->vb.height == buf->vb.height &&
			   prev->fmt       == buf->fmt) {
			list_move_tail(&buf->vb.queue, &q->active);
			buf->vb.state = VIDEOBUF_ACTIVE;
			buf->count    = q->count++;
			prev->risc.jmp[1] = cpu_to_le32(buf->risc.dma);
			dprintk(10,"[%p/%d] restart_queue - move to active\n",
				buf,buf->vb.i);
		} else {
			return 0;
		}
		prev = buf;
	}
}

/* ------------------------------------------------------------------ */

static int
buffer_setup(struct videobuf_queue *q, unsigned int *count, unsigned int *size)
{
	struct tw6800_fh *fh = q->priv_data;

	*size = fh->fmt->depth*fh->width*fh->height >> 3;
	if (0 == *count)
		*count = 32;
	while (*size * *count > vid_limit * 1024 * 1024)
		(*count)--;
	return 0;
}

static int
buffer_prepare(struct videobuf_queue *q, struct videobuf_buffer *vb,
	       enum v4l2_field field)
{
	struct tw6800_fh   *fh  = q->priv_data;
	struct tw6800_dev  *dev = fh->dev;
	struct tw68_core *core = dev->core;
	struct tw68_buffer *buf = container_of(vb,struct tw68_buffer,vb);
	struct videobuf_dmabuf *dma=videobuf_to_dma(&buf->vb);
	int rc, init_buffer = 0;

	BUG_ON(NULL == fh->fmt);
	if (fh->width  < 48 || fh->width  > core->tvnorm->swidth ||
	    fh->height < 32 || fh->height > core->tvnorm->sheight)
		return -EINVAL;
	buf->vb.size = (fh->width * fh->height * fh->fmt->depth) >> 3;
	if (0 != buf->vb.baddr  &&  buf->vb.bsize < buf->vb.size)
		return -EINVAL;

	if (buf->fmt       != fh->fmt    ||
	    buf->vb.width  != fh->width  ||
	    buf->vb.height != fh->height ||
	    buf->vb.field  != field) {
		buf->fmt       = fh->fmt;
		buf->vb.width  = fh->width;
		buf->vb.height = fh->height;
		buf->vb.field  = field;
		init_buffer = 1;
	}

	if (VIDEOBUF_NEEDS_INIT == buf->vb.state) {
		init_buffer = 1;
		if (0 != (rc = videobuf_iolock(q,&buf->vb,NULL)))
			goto fail;
	}

	if (init_buffer) {
		buf->bpl = buf->vb.width * buf->fmt->depth >> 3;
		switch (buf->vb.field) {
		case V4L2_FIELD_TOP:
			tw68_risc_buffer(dev->pci, &buf->risc,
					 dma->sglist, 0, UNSET,
					 buf->bpl, 0, buf->vb.height);
			break;
		case V4L2_FIELD_BOTTOM:
			tw68_risc_buffer(dev->pci, &buf->risc,
					 dma->sglist, UNSET, 0,
					 buf->bpl, 0, buf->vb.height);
			break;
		case V4L2_FIELD_INTERLACED:
			tw68_risc_buffer(dev->pci, &buf->risc,
					 dma->sglist, 0, buf->bpl,
					 buf->bpl, buf->bpl,
					 buf->vb.height >> 1);
			break;
		case V4L2_FIELD_SEQ_TB:
			tw68_risc_buffer(dev->pci, &buf->risc,
					 dma->sglist,
					 0, buf->bpl * (buf->vb.height >> 1),
					 buf->bpl, 0,
					 buf->vb.height >> 1);
			break;
		case V4L2_FIELD_SEQ_BT:
			tw68_risc_buffer(dev->pci, &buf->risc,
					 dma->sglist,
					 buf->bpl * (buf->vb.height >> 1), 0,
					 buf->bpl, 0,
					 buf->vb.height >> 1);
			break;
		default:
			BUG();
		}
	}
	dprintk(10,"[%p/%d] buffer_prepare - %dx%d %dbpp \"%s\" - dma=0x%08lx\n",
		buf, buf->vb.i,
		fh->width, fh->height, fh->fmt->depth, fh->fmt->name,
		(unsigned long)buf->risc.dma);

	buf->vb.state = VIDEOBUF_PREPARED;
	return 0;

 fail:
	tw68_free_buffer(q,buf);
	return rc;
}

static void
buffer_queue(struct videobuf_queue *vq, struct videobuf_buffer *vb)
{
	struct tw68_buffer    *buf = container_of(vb,struct tw68_buffer,vb);
	struct tw68_buffer    *prev;
	struct tw6800_fh      *fh   = vq->priv_data;
	struct tw6800_dev     *dev  = fh->dev;
	struct tw68_core      *core = dev->core;
	struct tw68_dmaqueue  *q    = &dev->vidq;

	/* append a 'JUMP to stopper' to the buffer risc program */
	buf->risc.jmp[0] = cpu_to_le32(RISC_JUMP | RISC_INT_BIT);
	buf->risc.jmp[1] = cpu_to_le32(q->stopper.dma);

	/* if the 'queued' chain is empty, append this buffer to it */
	if (!list_empty(&q->queued)) {
		list_add_tail(&buf->vb.queue,&q->queued);
		buf->vb.state = VIDEOBUF_QUEUED;
		dprintk(10,"[%p/%d] buffer_queue - append to queued\n",
			buf, buf->vb.i);

	/* else if the 'active' chain doesn't exist put on this one */
	} else if (list_empty(&q->active)) {
		list_add_tail(&buf->vb.queue,&q->active);
		start_video_dma(dev, q, buf);
		buf->vb.state = VIDEOBUF_ACTIVE;
		buf->count    = q->count++;
		mod_timer(&q->timeout, jiffies+BUFFER_TIMEOUT);
		dprintk(10,"[%p/%d] buffer_queue - first active\n",
			buf, buf->vb.i);

	/*
	 * else we would like to put this buffer on the tail of the
	 * active chain.
	 */
	} else {
		prev = list_entry(q->active.prev, struct tw68_buffer,
				  vb.queue);
		/*
		 * If the width, height and format for this buffer are
		 * the same as the active chain, we can add it.
		 */
		if (prev->vb.width  == buf->vb.width  &&
		    prev->vb.height == buf->vb.height &&
		    prev->fmt       == buf->fmt) {
			list_add_tail(&buf->vb.queue,&q->active);
			buf->vb.state = VIDEOBUF_ACTIVE;
			buf->count    = q->count++;
			prev->risc.jmp[1] = cpu_to_le32(buf->risc.dma);
			dprintk(10,"[%p/%d] buffer_queue - append to active\n",
				buf, buf->vb.i);
		/* Otherwise we put it onto the 'queued' chain */
		} else {
			list_add_tail(&buf->vb.queue,&q->queued);
			buf->vb.state = VIDEOBUF_QUEUED;
			dprintk(10,"[%p/%d] buffer_queue - first queued\n",
				buf, buf->vb.i);
		}
	}
}

static void buffer_release(struct videobuf_queue *q, struct videobuf_buffer *vb)
{
	struct tw68_buffer *buf = container_of(vb,struct tw68_buffer,vb);

	tw68_free_buffer(q,buf);
}

static struct videobuf_queue_ops tw6800_video_qops = {
	.buf_setup    = buffer_setup,
	.buf_prepare  = buffer_prepare,
	.buf_queue    = buffer_queue,
	.buf_release  = buffer_release,
};

/* ------------------------------------------------------------------ */

static struct videobuf_queue* get_queue(struct tw6800_fh *fh)
{
	switch (fh->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &fh->vidq;
	default:
		BUG();
		return NULL;
	}
}

static int get_resource(struct tw6800_fh *fh)
{
	switch (fh->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return RESOURCE_VIDEO;
	case V4L2_BUF_TYPE_VBI_CAPTURE:
		return RESOURCE_VBI;
	default:
		BUG();
		return 0;
	}
}

static int video_open(struct inode *inode, struct file *file)
{
	int minor = iminor(inode);
	struct tw6800_dev *h,*dev = NULL;
	struct tw68_core *core;
	struct tw6800_fh *fh;
	enum v4l2_buf_type type = 0;
	int radio = 0;

	lock_kernel();
	list_for_each_entry(h, &tw6800_devlist, devlist) {
		if (h->video_dev->minor == minor) {
			dev  = h;
			type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		}
	}
	if (NULL == dev) {
		unlock_kernel();
		return -ENODEV;
	}

	core = dev->core;

	dprintk(1,"open minor=%d radio=%d type=%s\n",
		minor,radio,v4l2_type_names[type]);

	/* allocate + initialize per filehandle data */
	fh = kzalloc(sizeof(*fh),GFP_KERNEL);
	if (NULL == fh) {
		unlock_kernel();
		return -ENOMEM;
	}
	file->private_data = fh;
	fh->dev      = dev;
	fh->radio    = radio;
	fh->type     = type;
	fh->width    = 320;
	fh->height   = 240;
	fh->fmt      = format_by_fourcc(V4L2_PIX_FMT_BGR24);

	videobuf_queue_sg_init(&fh->vidq, &tw6800_video_qops,
			    &dev->pci->dev, &dev->slock,
			    V4L2_BUF_TYPE_VIDEO_CAPTURE,
			    V4L2_FIELD_INTERLACED,
			    sizeof(struct tw68_buffer),
			    fh);

	unlock_kernel();

	atomic_inc(&core->users);

	return 0;
}

static ssize_t
video_read(struct file *file, char __user *data, size_t count, loff_t *ppos)
{
	struct tw6800_fh *fh = file->private_data;

	switch (fh->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		if (res_locked(fh->dev,RESOURCE_VIDEO))
			return -EBUSY;
		return videobuf_read_one(&fh->vidq, data, count, ppos,
					 file->f_flags & O_NONBLOCK);
	default:
		BUG();
		return 0;
	}
}

static unsigned int
video_poll(struct file *file, struct poll_table_struct *wait)
{
	struct tw6800_fh *fh = file->private_data;
	struct tw68_buffer *buf;

	if (res_check(fh,RESOURCE_VIDEO)) {
		/* streaming capture */
		if (list_empty(&fh->vidq.stream))
			return POLLERR;
		buf = list_entry(fh->vidq.stream.next,
				 struct tw68_buffer, vb.stream);
	} else {
		/* read() capture */
		buf = (struct tw68_buffer*)fh->vidq.read_buf;
		if (NULL == buf)
			return POLLERR;
	}
	poll_wait(file, &buf->vb.done, wait);
	if (buf->vb.state == VIDEOBUF_DONE ||
	    buf->vb.state == VIDEOBUF_ERROR)
		return POLLIN|POLLRDNORM;
	return 0;
}

static int video_release(struct inode *inode, struct file *file)
{
	struct tw6800_fh  *fh  = file->private_data;
	struct tw6800_dev *dev = fh->dev;

	/* turn off overlay */
	if (res_check(fh, RESOURCE_OVERLAY)) {
		/* FIXME */
		res_free(dev,fh,RESOURCE_OVERLAY);
	}

	/* stop video capture */
	if (res_check(fh, RESOURCE_VIDEO)) {
		videobuf_queue_cancel(&fh->vidq);
		res_free(dev,fh,RESOURCE_VIDEO);
	}
	if (fh->vidq.read_buf) {
		buffer_release(&fh->vidq,fh->vidq.read_buf);
		kfree(fh->vidq.read_buf);
	}

	videobuf_mmap_free(&fh->vidq);
	file->private_data = NULL;
	kfree(fh);

	return 0;
}

static int
video_mmap(struct file *file, struct vm_area_struct * vma)
{
	struct tw6800_fh *fh = file->private_data;

	return videobuf_mmap_mapper(get_queue(fh), vma);
}

/* ------------------------------------------------------------------ */
/* VIDEO CTRL IOCTLS                                                  */

int tw68_get_control (struct tw68_core  *core, struct v4l2_control *ctl)
{
	struct tw68_ctrl  *c    = NULL;
	int i;
	s32 sval;

	for (i = 0; i < TW6800_CTLS; i++) {
		if (tw6800_ctls[i].v.id == ctl->id) {
			c = &tw6800_ctls[i];
			break;
		}
	}
	if (unlikely(NULL == c))
		return -EINVAL;
#if 0
	value = tw_readb(c->reg);
	switch (ctl->id) {
	case V4L2_CID_AUDIO_BALANCE:
		ctl->value = ((value & 0x7f) < 0x40) ?
			((value & 0x7f) + 0x40) :
			(0x7f - (value & 0x7f));
		break;
	case V4L2_CID_AUDIO_VOLUME:
		ctl->value = 0x3f - (value & 0x3f);
		break;
	default:
		ctl->value = ((value + (c->off << c->shift)) &
			      c->mask) >> c->shift;
		break;
	}
#endif
dprintk(1, "get_control regval=0x%02x\n", tw_readb(c->reg));
//	ctl->value = ((tw_readb(c->reg) & c->mask) >> c->shift) + c->off;
	sval = (s8)tw_readb(c->reg) + (c->off << c->shift);
dprintk(1, "get_control sval=%d\n", sval);
	ctl->value = (sval & c->mask) >> c->shift;
#if 0
	ctl->value = ((tw_readb(c->reg) + (c->off << c->shift)) &
			c->mask) >> c->shift;
#endif
	if (c->reg2)
		ctl->value |=
		    ((tw_readb(c->reg2) & c->mask2) >> c->shift2) << 8;
	dprintk(1,"get_control id=0x%X(%s) ctrl=0x%x, "
		"reg=0x%02x (mask 0x%02x)\n",
		ctl->id, c->v.name, ctl->value, c->reg, c->mask);
	return 0;
}
EXPORT_SYMBOL_GPL(tw68_get_control);

int tw68_set_control(struct tw68_core *core, struct v4l2_control *ctl)
{
	struct tw68_ctrl *c = NULL;
	u32 value, vvalue, mask;
	int i;

	for (i = 0; i < TW6800_CTLS; i++) {
		if (tw6800_ctls[i].v.id == ctl->id) {
			c = &tw6800_ctls[i];
			break;
		}
	}
	if (unlikely(NULL == c))
		return -EINVAL;

	/* limit value to be within allowable range */
	dprintk(1,"set_control id=0x%X(%s) val=%d\n",
		ctl->id, c->v.name, ctl->value);
	if (ctl->value < c->v.minimum) {
		dprintk(1,"too small, setting to %d\n", c->v.minimum);
		ctl->value = c->v.minimum;
	} else if (ctl->value > c->v.maximum) {
		dprintk(1,"too large, setting to %d\n", c->v.maximum);
		ctl->value = c->v.maximum;
	}
	mask=c->mask;
	switch (ctl->id) {
#if 0
	case V4L2_CID_AUDIO_BALANCE:
		value = (ctl->value < 0x40) ?
			(0x7f - ctl->value) : (ctl->value - 0x40);
		break;
	case V4L2_CID_AUDIO_VOLUME:
		value = 0x3f - (ctl->value & 0x3f);
		break;
#endif
	case V4L2_CID_SATURATION:
		/* special v_sat handling */

		value = ((ctl->value - c->off) << c->shift) & c->mask;

		if (core->tvnorm->v4l2_id & V4L2_STD_SECAM) {
			/* For SECAM, both U and V sat should be equal */
			vvalue = value;
		} else {
			/* Keeps U Saturation proportional to V Sat */
			vvalue=(value*0x5a)/0x7f;
		}
		tw_writeb(TW68_SAT_V, vvalue);
		tw_writeb(TW68_SAT_U, value);
		break;
	case V4L2_CID_CHROMA_AGC:
		/* Do not allow chroma AGC to be enabled for SECAM */
		value = ((ctl->value - c->off) << c->shift) & c->mask;
		if (core->tvnorm->v4l2_id & V4L2_STD_SECAM && value)
			return -EINVAL;
		tw_andorb(c->reg, c->mask, value);
		break;
	case V4L2_CID_COLOR_KILLER:
		if (ctl->value)
			value = 0xe0;
		else
			value = 0x00;
		tw_andorb(c->reg, 0xe0, value);
		break;
	default:
		value = ((ctl->value - c->off) << c->shift) & c->mask;
		dprintk(1,"set_control writing 0x%02x\n", value);
		tw_writeb(c->reg, value);
		break;
	}
	dprintk(1,"set_control id=0x%X(%s) ctrl=0x%02x, "
		  "reg=0x%02x val=0x%02x (mask 0x%02x)\n",
		  ctl->id, c->v.name, ctl->value, c->reg, value,
		  mask);
	return 0;
}
EXPORT_SYMBOL_GPL(tw68_set_control);

static void init_controls(struct tw68_core *core)
{
	struct v4l2_control ctrl;
	int i;

	for (i = 0; i < TW6800_CTLS; i++) {
		ctrl.id=tw6800_ctls[i].v.id;
		ctrl.value=tw6800_ctls[i].v.default_value;

		tw68_set_control(core, &ctrl);
	}
}

/* ------------------------------------------------------------------ */
/* VIDEO IOCTLS                                                       */

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct tw6800_fh  *fh   = priv;

	f->fmt.pix.width        = fh->width;
	f->fmt.pix.height       = fh->height;
	f->fmt.pix.field        = fh->vidq.field;
	f->fmt.pix.pixelformat  = fh->fmt->fourcc;
	f->fmt.pix.bytesperline =
		(f->fmt.pix.width * fh->fmt->depth) >> 3;
	f->fmt.pix.sizeimage =
		f->fmt.pix.height * f->fmt.pix.bytesperline;
	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
			struct v4l2_format *f)
{
	struct tw68_core  *core = ((struct tw6800_fh *)priv)->dev->core;
	struct tw6800_fmt *fmt;
	enum v4l2_field   field;
	unsigned int      maxw, maxh;

	fmt = format_by_fourcc(f->fmt.pix.pixelformat);
	if (NULL == fmt)
		return -EINVAL;

	field = f->fmt.pix.field;
	maxw  = core->tvnorm->swidth;
	maxh  = core->tvnorm->sheight;

	if (V4L2_FIELD_ANY == field) {
		field = (f->fmt.pix.height > maxh/2)
			? V4L2_FIELD_INTERLACED
			: V4L2_FIELD_BOTTOM;
	}

	switch (field) {
	case V4L2_FIELD_TOP:
	case V4L2_FIELD_BOTTOM:
		maxh = maxh / 2;
		break;
	case V4L2_FIELD_INTERLACED:
		break;
	default:
		return -EINVAL;
	}

	f->fmt.pix.field = field;
	if (f->fmt.pix.height < 32)
		f->fmt.pix.height = 32;
	if (f->fmt.pix.height > maxh)
		f->fmt.pix.height = maxh;
	if (f->fmt.pix.width < 48)
		f->fmt.pix.width = 48;
	if (f->fmt.pix.width > maxw)
		f->fmt.pix.width = maxw;
	f->fmt.pix.width &= ~0x03;
	f->fmt.pix.bytesperline =
		(f->fmt.pix.width * fmt->depth) >> 3;
	f->fmt.pix.sizeimage =
		f->fmt.pix.height * f->fmt.pix.bytesperline;
	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	struct tw6800_fh  *fh   = priv;
	int err = vidioc_try_fmt_vid_cap (file,priv,f);

	if (0 != err)
		return err;
	fh->fmt        = format_by_fourcc(f->fmt.pix.pixelformat);
	fh->width      = f->fmt.pix.width;
	fh->height     = f->fmt.pix.height;
	fh->vidq.field = f->fmt.pix.field;
	return 0;
}

static int vidioc_querycap (struct file *file, void  *priv,
					struct v4l2_capability *cap)
{
	struct tw6800_dev *dev  = ((struct tw6800_fh *)priv)->dev;
	struct tw68_core  *core = dev->core;

	strcpy(cap->driver, "tw6800");
	strlcpy(cap->card, core->board.name, sizeof(cap->card));
	sprintf(cap->bus_info,"PCI:%s",pci_name(dev->pci));
	cap->version = TW68_VERSION_CODE;
	cap->capabilities =
		V4L2_CAP_VIDEO_CAPTURE |
		V4L2_CAP_READWRITE     |
#if 0
		V4L2_CAP_VBI_CAPTURE   |
#endif
		V4L2_CAP_STREAMING;
	if (UNSET != core->board.tuner_type)
		cap->capabilities |= V4L2_CAP_TUNER;
	return 0;
}

static int vidioc_enum_fmt_vid_cap (struct file *file, void  *priv,
					struct v4l2_fmtdesc *f)
{
	if (unlikely(f->index >= ARRAY_SIZE(formats)))
		return -EINVAL;

	strlcpy(f->description,formats[f->index].name,
		sizeof(f->description));
	f->pixelformat = formats[f->index].fourcc;

	return 0;
}

static int vidioc_reqbufs (struct file *file, void *priv,
			   struct v4l2_requestbuffers *p)
{
	struct tw6800_fh  *fh   = priv;
	return (videobuf_reqbufs(get_queue(fh), p));
}

static int vidioc_querybuf (struct file *file, void *priv,
			    struct v4l2_buffer *p)
{
	struct tw6800_fh  *fh   = priv;
	return (videobuf_querybuf(get_queue(fh), p));
}

static int vidioc_qbuf (struct file *file, void *priv,
			struct v4l2_buffer *p)
{
	struct tw6800_fh  *fh   = priv;
	return (videobuf_qbuf(get_queue(fh), p));
}

static int vidioc_dqbuf (struct file *file, void *priv,
			 struct v4l2_buffer *p)
{
	struct tw6800_fh  *fh   = priv;
	return (videobuf_dqbuf(get_queue(fh), p,
				file->f_flags & O_NONBLOCK));
}

static int vidioc_streamon(struct file *file, void *priv,
			   enum v4l2_buf_type i)
{
	struct tw6800_fh  *fh   = priv;
	struct tw6800_dev *dev  = fh->dev;

	/* We should remember that this driver also supports teletext,  */
	/* so we have to test if the v4l2_buf_type is VBI capture data. */
	if (unlikely((fh->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) &&
		     (fh->type != V4L2_BUF_TYPE_VBI_CAPTURE)))
		return -EINVAL;

	if (unlikely(i != fh->type))
		return -EINVAL;

	if (unlikely(!res_get(dev,fh,get_resource(fh))))
		return -EBUSY;
#if 0
/* ****** Debugging - dump all registers ****** */
{
struct tw68_core  *core = dev->core;
  printk("PCI Registers:\n");
  dump_pci_regs(0, 0x200, core);
  printk("Video Registers:\n");
  dump_vregs(0x200, 0x300, core);
}
#endif
	return videobuf_streamon(get_queue(fh));
}

static int vidioc_streamoff(struct file *file, void *priv,
			    enum v4l2_buf_type i)
{
	struct tw6800_fh  *fh   = priv;
	struct tw6800_dev *dev  = fh->dev;
	int               err, res;

	if ((fh->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) &&
	    (fh->type != V4L2_BUF_TYPE_VBI_CAPTURE))
		return -EINVAL;

	if (i != fh->type)
		return -EINVAL;

	res = get_resource(fh);
	err = videobuf_streamoff(get_queue(fh));
	if (err < 0)
		return err;
	res_free(dev,fh,res);
	return 0;
}

static int vidioc_s_std (struct file *file, void *priv,
			 v4l2_std_id *tvnorms)
{
	struct tw68_core  *core = ((struct tw6800_fh *)priv)->dev->core;

	mutex_lock(&core->lock);
	tw68_set_tvnorm(core,*tvnorms);
	mutex_unlock(&core->lock);

	return 0;
}

int tw68_enum_input (struct tw68_core  *core,struct v4l2_input *i)
{
	static const char *iname[] = {
		[ TW68_VMUX_COMPOSITE1 ] = "Composite1",
		[ TW68_VMUX_COMPOSITE2 ] = "Composite2",
		[ TW68_VMUX_COMPOSITE3 ] = "Composite3",
		[ TW68_VMUX_COMPOSITE4 ] = "Composite4",
		[ TW68_VMUX_SVIDEO     ] = "S-Video",
		[ TW68_VMUX_TELEVISION ] = "Television",
		[ TW68_VMUX_CABLE      ] = "Cable TV",
		[ TW68_VMUX_DVB        ] = "DVB",
		[ TW68_VMUX_DEBUG      ] = "for debug only",
	};
	unsigned int n;

	n = i->index;
	if (n >= 4)
		return -EINVAL;
	if (0 == INPUT(n).type)
		return -EINVAL;
	memset(i,0,sizeof(*i));
	i->index = n;
	i->type  = V4L2_INPUT_TYPE_CAMERA;
	strcpy(i->name,iname[INPUT(n).type]);
	if ((TW68_VMUX_TELEVISION == INPUT(n).type) ||
	    (TW68_VMUX_CABLE      == INPUT(n).type))
		i->type = V4L2_INPUT_TYPE_TUNER;
		i->std = TW68_NORMS;
	return 0;
}
EXPORT_SYMBOL_GPL(tw68_enum_input);

static int vidioc_enum_input (struct file *file, void *priv,
				struct v4l2_input *i)
{
	struct tw68_core  *core = ((struct tw6800_fh *)priv)->dev->core;
	return tw68_enum_input (core,i);
}

static int vidioc_g_input (struct file *file, void *priv,
			   unsigned int *i)
{
	struct tw68_core  *core = ((struct tw6800_fh *)priv)->dev->core;

	*i = core->input;
	return 0;
}

static int vidioc_s_input (struct file *file, void *priv,
			   unsigned int i)
{
	struct tw68_core  *core = ((struct tw6800_fh *)priv)->dev->core;

	if (i >= 4)
		return -EINVAL;

	mutex_lock(&core->lock);
//	tw68_newstation(core);
	tw68_video_mux(core,i);
	mutex_unlock(&core->lock);
	return 0;
}



static int vidioc_queryctrl (struct file *file, void *priv,
			     struct v4l2_queryctrl *qctrl)
{
	struct tw68_core *core = ((struct tw6800_fh *)priv)->dev->core;

	qctrl->id = v4l2_ctrl_next(ctrl_classes, qctrl->id);
	if (unlikely(qctrl->id == 0))
		return -EINVAL;
	return tw6800_ctrl_query(core, qctrl);
}

static int vidioc_g_ctrl (struct file *file, void *priv,
			  struct v4l2_control *ctl)
{
	struct tw68_core  *core = ((struct tw6800_fh *)priv)->dev->core;
	return tw68_get_control(core,ctl);
}

static int vidioc_s_ctrl (struct file *file, void *priv,
				struct v4l2_control *ctl)
{
	struct tw68_core  *core = ((struct tw6800_fh *)priv)->dev->core;
	return tw68_set_control(core,ctl);
}

static int vidioc_g_frequency (struct file *file, void *priv,
				struct v4l2_frequency *f)
{
	struct tw6800_fh  *fh   = priv;
	struct tw68_core  *core = fh->dev->core;

	if (unlikely(UNSET == core->board.tuner_type))
		return -EINVAL;

	f->type = fh->radio ? V4L2_TUNER_RADIO : V4L2_TUNER_ANALOG_TV;
	f->frequency = core->freq;

	return 0;
}

int tw68_set_freq (struct tw68_core  *core,
				struct v4l2_frequency *f)
{
	if (unlikely(UNSET == core->board.tuner_type))
		return -EINVAL;
	if (unlikely(f->tuner != 0))
		return -EINVAL;

	mutex_lock(&core->lock);
	core->freq = f->frequency;
//	tw68_newstation(core);
//	tw68_call_i2c_clients(core,VIDIOC_S_FREQUENCY,f);

	mutex_unlock(&core->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(tw68_set_freq);

static int vidioc_s_frequency (struct file *file, void *priv,
				struct v4l2_frequency *f)
{
	struct tw6800_fh  *fh   = priv;
	struct tw68_core  *core = fh->dev->core;

	if (unlikely(0 == fh->radio && f->type != V4L2_TUNER_ANALOG_TV))
		return -EINVAL;
	if (unlikely(1 == fh->radio && f->type != V4L2_TUNER_RADIO))
		return -EINVAL;

	return
		tw68_set_freq (core,f);
}

/* ----------------------------------------------------------- */

static void tw6800_vid_timeout(unsigned long data)
{
	struct tw6800_dev *dev = (struct tw6800_dev*)data;
	struct tw68_core *core = dev->core;
	struct tw68_dmaqueue *q = &dev->vidq;
	struct tw68_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&dev->slock,flags);
	while (!list_empty(&q->active)) {
		buf = list_entry(q->active.next, struct tw68_buffer, vb.queue);
		list_del(&buf->vb.queue);
		buf->vb.state = VIDEOBUF_ERROR;
		wake_up(&buf->vb.done);
		printk("%s/0: [%p/%d] timeout - dma=0x%08lx\n", core->name,
		       buf, buf->vb.i, (unsigned long)buf->risc.dma);
	}
	restart_video_queue(dev,q);
	spin_unlock_irqrestore(&dev->slock,flags);
}

/*
 * tw6800_vid_irq
 *
 * 	Handle a PCI interrupt from the video circuitry
 */
static void tw6800_vid_irq(struct tw6800_dev *dev, u32 status)
{
	struct tw68_core *core = dev->core;
	u32 reg;
	struct tw68_dmaqueue *q = &dev->vidq;

	if (0 == (status & TW68_VID_INTS))
		return;		/* if not a video interrupt, return */

	/* reset interrupts handled by this routine */
	tw_writel(TW68_INTSTAT, TW68_VID_INTS);

	if (status & TW68_PABORT) {	/* TODO - what should we do? */
		iprintk(2, "PABORT interrupt\n");
	}
	if (status & TW68_DMAPERR) {
		iprintk(2, "DMAPERR interrupt\n");
		/* Stop risc & fifo */
		tw_clearl(TW68_DMAC, TW68_DMAP_EN | TW68_FIFO_EN);
		tw_clearl(TW68_INTMASK, TW68_VID_INTS);
		core->pci_irqmask &= ~TW68_VID_INTS;
		return;
	}
	if (status & TW68_FDMIS) {	/* logic error somewhere */
		iprintk(2, "FDMIS interrupt\n");
		/* Stop risc & fifo */
		tw_clearl(TW68_DMAC, TW68_DMAP_EN | TW68_FIFO_EN);
		tw_clearl(TW68_INTMASK, TW68_VID_INTS);
		core->pci_irqmask &= ~TW68_VID_INTS;
		return;
	}
	if (status & TW68_FFOF) {	/* probably a logic error */
		iprintk(2, "FFOF interrupt\n");
		/* Stop risc & fifo */
//		tw_clearl(TW68_DMAC, TW68_DMAP_EN | TW68_FIFO_EN);
//		tw_clearl(TW68_INTMASK, TW68_VID_INTS);
//		core->pci_irqmask &= ~TW68_VID_INTS;
//		return;
	}

	if (status & TW68_DMAPI) {
		iprintk(2, "DMAPI interrupt\n");
		spin_lock(&dev->slock);
		/*
		 * DMAPI shows we have reached the end of the risc code
		 * for the current buffer.  tw68_wakeup will take care
		 * of the buffer handling, plus any non-video requirements.
		 */
		tw68_wakeup(core, q, 2);
		spin_unlock(&dev->slock);
		/* Check whether we have gotten into 'stopper' code */
		reg = tw_readl(TW68_DMAP_PP);
		if ((reg >= q->stopper.dma) &&
		    (reg < q->stopper.dma + q->stopper.size)) {
			/* Yes - stop risc & fifo */
			tw_clearl(TW68_DMAC, TW68_DMAP_EN | TW68_FIFO_EN);
			tw_clearl(TW68_INTMASK, TW68_VID_INTS);
			core->pci_irqmask &= ~TW68_VID_INTS;
			dprintk(10, "stopper risc code entered\n");
			return;
		}
	}
}

/*
 * tw6800_irq
 *
 * 	This routine is called on any PCI interrupt.  It must assure the
 * 	handling of a video interrupt is performed, but also any other
 * 	devices which may be present.
 *
 * 	TODO: Check whether there is anything in core->pci_irqmask which
 * 	might need to be processed and, if not, re-write this.
 */
static irqreturn_t tw6800_irq(int irq, void *dev_id)
{
	struct tw6800_dev *dev = dev_id;
	struct tw68_core *core = dev->core;
	u32 status;
	int loop, handled = 0;

	status = tw_readl(TW68_INTSTAT);
	/* Check if anything to do */
	if (0 == status)
		return IRQ_RETVAL(0);	/* No - return */
	for (loop = 0; loop < 10; loop++) {
		/* check for all anticipated interrupts */
		status = tw_readl(TW68_INTSTAT);
		if (0 == (status & core->pci_irqmask))
			goto out;	/* all interrupts handled */
		handled = 1;
		if (status & TW68_VID_INTS)	/* video interrupt */
			tw6800_vid_irq(dev, status);
		
	};
	if (10 == loop) {
		printk(KERN_WARNING "%s/0: irq loop -- clearing mask\n",
		       core->name);
		tw_writel(TW68_INTMASK,0);
	}

 out:
	if (0 == handled)
  		printk(KERN_DEBUG "%s: Interrupt not handled - "
			"status=0x%08x\n", __func__, status);
	return IRQ_RETVAL(handled);
}

/* ----------------------------------------------------------- */
/* exported stuff                                              */

static const struct file_operations video_fops =
{
	.owner	       = THIS_MODULE,
	.open	       = video_open,
	.release       = video_release,
	.read	       = video_read,
	.poll          = video_poll,
	.mmap	       = video_mmap,
	.ioctl	       = video_ioctl2,
	.compat_ioctl  = v4l_compat_ioctl32,
	.llseek        = no_llseek,
};

static const struct v4l2_ioctl_ops video_ioctl_ops = {
	.vidioc_querycap      = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap  = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap     = vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap   = vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap     = vidioc_s_fmt_vid_cap,
//	.vidioc_g_fmt_vbi_cap     = tw6800_vbi_fmt,
//	.vidioc_try_fmt_vbi_cap   = tw6800_vbi_fmt,
//	.vidioc_s_fmt_vbi_cap     = tw6800_vbi_fmt,
	.vidioc_reqbufs       = vidioc_reqbufs,
	.vidioc_querybuf      = vidioc_querybuf,
	.vidioc_qbuf          = vidioc_qbuf,
	.vidioc_dqbuf         = vidioc_dqbuf,
	.vidioc_s_std         = vidioc_s_std,
	.vidioc_enum_input    = vidioc_enum_input,
	.vidioc_g_input       = vidioc_g_input,
	.vidioc_s_input       = vidioc_s_input,
	.vidioc_queryctrl     = vidioc_queryctrl,
	.vidioc_g_ctrl        = vidioc_g_ctrl,
	.vidioc_s_ctrl        = vidioc_s_ctrl,
	.vidioc_streamon      = vidioc_streamon,
	.vidioc_streamoff     = vidioc_streamoff,
//	.vidioc_g_tuner       = vidioc_g_tuner,
//	.vidioc_s_tuner       = vidioc_s_tuner,
	.vidioc_g_frequency   = vidioc_g_frequency,
	.vidioc_s_frequency   = vidioc_s_frequency,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.vidioc_g_register    = vidioc_g_register,
	.vidioc_s_register    = vidioc_s_register,
#endif
};

static struct video_device tw6800_video_template = {
	.name                 = "tw6800-video",
	.fops                 = &video_fops,
	.minor                = -1,
	.ioctl_ops 	      = &video_ioctl_ops,
	.tvnorms              = TW68_NORMS,
	.current_norm         = V4L2_STD_PAL_M,
//	.current_norm         = V4L2_STD_NTSC_M,
};

/* ----------------------------------------------------------- */

static void tw6800_unregister_video(struct tw6800_dev *dev)
{
	if (dev->video_dev) {
		if (-1 != dev->video_dev->minor)
			video_unregister_device(dev->video_dev);
		else
			video_device_release(dev->video_dev);
		dev->video_dev = NULL;
	}
}

static int __devinit tw6800_initdev(struct pci_dev *pci_dev,
				    const struct pci_device_id *pci_id)
{
	struct tw6800_dev *dev;
	struct tw68_core *core;

	int err;

	dev = kzalloc(sizeof(*dev),GFP_KERNEL);
	if (NULL == dev)
		return -ENOMEM;

	/* pci init */
	dev->pci = pci_dev;
	if (pci_enable_device(pci_dev)) {
		err = -EIO;
		goto fail_free;
	}
	core = tw68_core_get(dev->pci);
	if (NULL == core) {
		err = -EINVAL;
		goto fail_free;
	}
	dev->core = core;

	/* print pci info */
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER,  &dev->pci_lat);
	printk(KERN_INFO "%s/0: found at %s, rev: %d, irq: %d, "
	       "latency: %d, mmio: 0x%llx\n", core->name,
	       pci_name(pci_dev), dev->pci_rev, pci_dev->irq,
	       dev->pci_lat,(unsigned long long)pci_resource_start(pci_dev,0));

	pci_set_master(pci_dev);
	if (!pci_dma_supported(pci_dev,DMA_32BIT_MASK)) {
		printk("%s/0: Oops: no 32bit PCI DMA ???\n",core->name);
		err = -EIO;
		goto fail_core;
	}

	/* initialize driver struct */
	spin_lock_init(&dev->slock);

	/* init video dma queues */
	INIT_LIST_HEAD(&dev->vidq.active);
	INIT_LIST_HEAD(&dev->vidq.queued);
	dev->vidq.timeout.function = tw6800_vid_timeout;
	dev->vidq.timeout.data     = (unsigned long)dev;
	init_timer(&dev->vidq.timeout);
	tw68_risc_stopper(dev->pci, &dev->vidq.stopper);

	/* get irq */
	err = request_irq(pci_dev->irq, tw6800_irq,
			  IRQF_SHARED | IRQF_DISABLED, core->name, dev);
	if (err < 0) {
		printk(KERN_ERR "%s/0: can't get IRQ %d\n",
		       core->name,pci_dev->irq);
		goto fail_core;
	}
	tw_setl(TW68_INTMASK, core->pci_irqmask);

#if 0
	/* load and configure helper modules */

	if (core->board.audio_chip == V4L2_IDENT_WM8775)
		request_module("wm8775");

	switch (core->boardnr) {
	case TW68_BOARD_DVICO_FUSIONHDTV_5_GOLD:
	case TW68_BOARD_DVICO_FUSIONHDTV_7_GOLD:
		request_module("rtc-isl1208");
		/* break intentionally omitted */
	case TW68_BOARD_DVICO_FUSIONHDTV_5_PCI_NANO:
		request_module("ir-kbd-i2c");
	}
#endif

	/* register v4l devices */
	dev->video_dev = tw68_vdev_init(core,dev->pci,
					&tw6800_video_template,"video");
	err = video_register_device(dev->video_dev,VFL_TYPE_GRABBER,
				    video_nr[core->nr]);
	if (err < 0) {
		printk(KERN_ERR "%s/0: can't register video device\n",
		       core->name);
		goto fail_unreg;
	}
	printk(KERN_INFO "%s/0: registered device video%d [v4l2]\n",
	       core->name, dev->video_dev->minor);

#if 0
	dev->vbi_dev = tw68_vdev_init(core,dev->pci,&tw6800_vbi_template,"vbi");
	err = video_register_device(dev->vbi_dev,VFL_TYPE_VBI,
				    vbi_nr[core->nr]);
	if (err < 0) {
		printk(KERN_ERR "%s/0: can't register vbi device\n",
		       core->name);
		goto fail_unreg;
	}
	printk(KERN_INFO "%s/0: registered device vbi%d\n",
	       core->name, dev->vbi_dev->minor);

	if (core->board.radio.type == TW68_RADIO) {
		dev->radio_dev = tw68_vdev_init(core,dev->pci,
						&tw6800_radio_template,"radio");
		err = video_register_device(dev->radio_dev,VFL_TYPE_RADIO,
					    radio_nr[core->nr]);
		if (err < 0) {
			printk(KERN_ERR "%s/0: can't register radio device\n",
			       core->name);
			goto fail_unreg;
		}
		printk(KERN_INFO "%s/0: registered device radio%d\n",
		       core->name, dev->radio_dev->minor);
	}
#endif

	/* everything worked */
	list_add_tail(&dev->devlist,&tw6800_devlist);
	pci_set_drvdata(pci_dev,dev);

	/* initial device configuration */
	mutex_lock(&core->lock);
	tw68_set_tvnorm(core, tw6800_video_template.current_norm);
	init_controls(core);
	tw68_video_mux(core,0);
	mutex_unlock(&core->lock);

#if 0
	/* start tvaudio thread */
	if (core->board.tuner_type != TUNER_ABSENT) {
		core->kthread = kthread_run(tw68_audio_thread, core, "tw68 tvaudio");
		if (IS_ERR(core->kthread)) {
			err = PTR_ERR(core->kthread);
			printk(KERN_ERR "%s/0: failed to create tw68 audio thread, err=%d\n",
			       core->name, err);
		}
	}
#endif
	return 0;

fail_unreg:
	tw6800_unregister_video(dev);
	free_irq(pci_dev->irq, dev);
fail_core:
	tw68_core_put(core,dev->pci);
fail_free:
	kfree(dev);
	return err;
}

static void __devexit tw6800_finidev(struct pci_dev *pci_dev)
{
	struct tw6800_dev *dev = pci_get_drvdata(pci_dev);
	struct tw68_core *core = dev->core;

	/* stop thread */
	if (core->kthread) {
		kthread_stop(core->kthread);
		core->kthread = NULL;
	}
	tw68_shutdown(core); /* FIXME */
	pci_disable_device(pci_dev);

	/* unregister stuff */
	free_irq(pci_dev->irq, dev);
	tw6800_unregister_video(dev);
	pci_set_drvdata(pci_dev, NULL);

	/* free memory */
	btcx_riscmem_free(dev->pci,&dev->vidq.stopper);
	list_del(&dev->devlist);
	tw68_core_put(core,dev->pci);
	kfree(dev);
}

#ifdef CONFIG_PM
static int tw6800_suspend(struct pci_dev *pci_dev, pm_message_t state)
{
	struct tw6800_dev *dev = pci_get_drvdata(pci_dev);
	struct tw68_core *core = dev->core;

	/* stop video+vbi capture */
	spin_lock(&dev->slock);
	if (!list_empty(&dev->vidq.active)) {
		printk("%s/0: suspend video\n", core->name);
		stop_video_dma(dev);
		del_timer(&dev->vidq.timeout);
	}
	spin_unlock(&dev->slock);


	/* FIXME -- shutdown device */
	tw68_shutdown(core);

	pci_save_state(pci_dev);
	if (0 != pci_set_power_state(pci_dev, pci_choose_state(pci_dev, state))) {
		pci_disable_device(pci_dev);
		dev->state.disabled = 1;
	}
	return 0;
}

static int tw6800_resume(struct pci_dev *pci_dev)
{
	struct tw6800_dev *dev = pci_get_drvdata(pci_dev);
	struct tw68_core *core = dev->core;
	int err;

	if (dev->state.disabled) {
		err=pci_enable_device(pci_dev);
		if (err) {
			printk(KERN_ERR "%s/0: can't enable device\n",
			       core->name);
			return err;
		}

		dev->state.disabled = 0;
	}
	err= pci_set_power_state(pci_dev, PCI_D0);
	if (err) {
		printk(KERN_ERR "%s/0: can't set power state\n", core->name);
		pci_disable_device(pci_dev);
		dev->state.disabled = 1;

		return err;
	}
	pci_restore_state(pci_dev);

	/* FIXME: re-initialize hardware */
	tw68_reset(core);

	tw_setl(TW68_INTMASK, core->pci_irqmask);

	/* restart video+vbi capture */
	spin_lock(&dev->slock);
	if (!list_empty(&dev->vidq.active)) {
		printk("%s/0: resume video\n", core->name);
		restart_video_queue(dev,&dev->vidq);
	}
	spin_unlock(&dev->slock);

	return 0;
}
#endif

/* ----------------------------------------------------------- */

static struct pci_device_id tw6800_pci_tbl[] = {
	{
		.vendor       = 0x1797,
		.device       = 0x6801,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
	}, {
		/* --- end of list --- */
	}
};
MODULE_DEVICE_TABLE(pci, tw6800_pci_tbl);

static struct pci_driver tw6800_pci_driver = {
	.name     = "tw6800",
	.id_table = tw6800_pci_tbl,
	.probe    = tw6800_initdev,
	.remove   = __devexit_p(tw6800_finidev),
#ifdef CONFIG_PM
	.suspend  = tw6800_suspend,
	.resume   = tw6800_resume,
#endif
};

static int tw6800_init(void)
{
	printk(KERN_INFO "tw6800: tw6800 v4l2 driver version %d.%d.%d loaded\n",
	       (TW68_VERSION_CODE >> 16) & 0xff,
	       (TW68_VERSION_CODE >>  8) & 0xff,
	       TW68_VERSION_CODE & 0xff);
	return pci_register_driver(&tw6800_pci_driver);
}

static void tw6800_fini(void)
{
	pci_unregister_driver(&tw6800_pci_driver);
}

module_init(tw6800_init);
module_exit(tw6800_fini);

/* ----------------------------------------------------------- */
