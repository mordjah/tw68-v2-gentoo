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

#include <linux/pci.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
//#include <linux/videodev2.h>
#include <linux/kdev_t.h>

#include <media/tuner.h>
#include <media/tveeprom.h>
#include <media/v4l2-common.h>
#include <media/videobuf-dma-sg.h>
#include <media/v4l2-chip-ident.h>

#if 0
#include <media/cx2341x.h>
#if defined(CONFIG_VIDEO_CX88_DVB) || defined(CONFIG_VIDEO_CX88_DVB_MODULE)
#include <media/videobuf-dvb.h>
#endif
#endif

#include "btcx-risc.h"
#include "tw68-reg.h"
//#include "tuner-xc2028.h"

#include <linux/version.h>
#include <linux/mutex.h>

#define TW68_VERSION_CODE KERNEL_VERSION(0,0,0)

#define TW68_MAXBOARDS 8

#define UNSET (-1U)

/* Max number of inputs by card */
#define MAX_TW68_INPUT 4

/* ----------------------------------------------------------- */
/* defines and enums                                           */

/* Currently unsupported by the driver: PAL/H, NTSC/Kr, SECAM B/G/H/LC */
#define TW68_NORMS (\
	V4L2_STD_NTSC_M|  V4L2_STD_NTSC_M_JP|  V4L2_STD_NTSC_443 | \
	V4L2_STD_PAL_BG|  V4L2_STD_PAL_DK   |  V4L2_STD_PAL_I    | \
	V4L2_STD_PAL_M |  V4L2_STD_PAL_N    |  V4L2_STD_PAL_Nc   | \
	V4L2_STD_PAL_60|  V4L2_STD_SECAM_L  |  V4L2_STD_SECAM_DK )

#define FORMAT_FLAGS_PACKED       0x01
#define FORMAT_FLAGS_PLANAR       0x02

#define VBI_LINE_COUNT              17
#define VBI_LINE_LENGTH           2048

/* need "shadow" registers for some write-only ones ... */
#define SHADOW_AUD_VOL_CTL           1
#define SHADOW_AUD_BAL_CTL           2
#define SHADOW_MAX                   3

/* FM Radio deemphasis type */
enum tw68_deemph_type {
	FM_NO_DEEMPH = 0,
	FM_DEEMPH_50,
	FM_DEEMPH_75
};

enum tw68_board_type {
        TW68_BOARD_NONE = 0,
        TW68_MPEG_DVB,
        TW68_MPEG_BLACKBIRD
};

/* ----------------------------------------------------------- */
/* static data                                                 */

struct tw6800_fmt {
	char  *name;
	u32   fourcc;          /* v4l2 format id */
	int   depth;
	int   flags;
	u32   twformat;
};

struct tw68_ctrl {
	struct v4l2_queryctrl  v;
	u32			off;
	u32			reg;
	u32			mask;
	u32			shift;
	u32			reg2;
	u32			mask2;
	u32			shift2;
};

/* ----------------------------------------------------------- */
/* card configuration                                          */

#define TW68_BOARD_NOAUTO               UNSET
#define TW68_BOARD_UNKNOWN                  0
#define	TW68_BOARD_6801			    1
#define	TW68_BOARD_OTHER		    2

enum tw68_itype {
	TW68_VMUX_COMPOSITE1 = 1,
	TW68_VMUX_COMPOSITE2,
	TW68_VMUX_COMPOSITE3,
	TW68_VMUX_COMPOSITE4,
	TW68_VMUX_SVIDEO,
	TW68_VMUX_TELEVISION,
	TW68_VMUX_CABLE,
	TW68_VMUX_DVB,
	TW68_VMUX_DEBUG,
	TW68_RADIO,
};

struct tw68_input {
	enum tw68_itype type;
	u32             gpio0, gpio1, gpio2, gpio3;
	unsigned int    vmux:2;
	unsigned int    audioroute:4;
};

struct tw68_board {
	char                    *name;
	unsigned int            tuner_type;
	unsigned int		radio_type;
	unsigned char		tuner_addr;
	unsigned char		radio_addr;
	int                     tda9887_conf;
	struct tw68_input       input[MAX_TW68_INPUT];
	struct tw68_input       radio;
	enum tw68_board_type    mpeg;
	unsigned int            audio_chip;
	int			num_frontends;
};

struct tw68_subid {
	u16     subvendor;
	u16     subdevice;
	u32     card;
};

#define INPUT(nr) (core->board.input[nr])

/* ----------------------------------------------------------- */
/* Interrupts enabled and handled by the video module          */
#define	TW68_VID_INTS	(TW68_PABORT | TW68_DMAPERR | TW68_FDMIS | \
			 TW68_FFOF | TW68_DMAPI)

/* ----------------------------------------------------------- */
/* device / file handle status                                 */

#define RESOURCE_OVERLAY       1
#define RESOURCE_VIDEO         2
#define RESOURCE_VBI           4

#define BUFFER_TIMEOUT     msecs_to_jiffies(500)  /* 0.5 seconds */

/* buffer for one video frame */
struct tw68_norm;
struct tw68_buffer {
	/* common v4l buffer stuff -- must be first */
	struct videobuf_buffer vb;

	/* tw68 specific */
	unsigned int           bpl;
	struct btcx_riscmem    risc;
	struct tw6800_fmt      *fmt;
	u32                    count;
};

struct tw68_dmaqueue {
	struct list_head       active;
	struct list_head       queued;
	struct timer_list      timeout;
	struct btcx_riscmem    stopper;
	u32                    count;
};

struct tw68_core {
	struct list_head           devlist;
	atomic_t                   refcount;

	/* board name */
	int                        nr;
	char                       name[32];

	/* pci stuff */
	int                        pci_bus;
	int                        pci_slot;
	u32                        __iomem *lmmio;
	u8                         __iomem *bmmio;
	u32                        shadow[SHADOW_MAX];
	int                        pci_irqmask;

	/* i2c i/o */
	struct i2c_adapter         i2c_adap;
	struct i2c_algo_bit_data   i2c_algo;
	struct i2c_client          i2c_client;
	u32                        i2c_state, i2c_rc;

	/* config info -- analog */
	unsigned int               boardnr;
	struct tw68_board	   board;

	/* Supported V4L _STD_ tuner formats */
	unsigned int               tuner_formats;

	/* config info -- dvb */
#if defined(CONFIG_VIDEO_TW68_DVB) || defined(CONFIG_VIDEO_TW68_DVB_MODULE)
	int 			   (*prev_set_voltage)(struct dvb_frontend* fe, fe_sec_voltage_t voltage);
#endif

	/* state info */
	struct task_struct         *kthread;
	struct tw68_norm	   *tvnorm;
	u32                        tvaudio;
	u32                        audiomode_manual;
	u32                        audiomode_current;
	u32                        input;
	u32                        astat;
	u32			   use_nicam;

	/* IR remote control state */
	struct tw68_IR             *ir;

	struct mutex               lock;
	/* various v4l controls */
	u32                        freq;
	atomic_t		   users;
	atomic_t                   mpeg_users;

	/* tw68-video needs to access cx6802 for hybrid tuner pll access. */
	struct cx6802_dev          *dvbdev;
	enum tw68_board_type       active_type_id;
	int			   active_ref;
	int			   active_fe_id;
};

struct tw6800_dev;
//struct cx6802_dev;

/* ----------------------------------------------------------- */
/* function 0: video stuff                                     */

struct tw68_norm {
	v4l2_std_id		v4l2_id;
	u32			format;
	u16			swidth;
	u16			sheight;
	u16			hdelay;
	u16			vdelay;
};

struct tw6800_fh {
	struct tw6800_dev          *dev;
	enum v4l2_buf_type         type;
	int                        radio;
	unsigned int               resources;

	/* video overlay */
	struct v4l2_window         win;
	struct v4l2_clip           *clips;
	unsigned int               nclips;

	/* video capture */
	struct tw6800_fmt          *fmt;
	unsigned int               width;
	unsigned int		   height;
	struct videobuf_queue      vidq;

	/* vbi capture */
	struct videobuf_queue      vbiq;
};

struct tw6800_suspend_state {
	int                        disabled;
};

struct tw6800_dev {
	struct tw68_core           *core;
	struct list_head           devlist;
	spinlock_t                 slock;

	/* various device info */
	unsigned int               resources;
	struct video_device        *video_dev;
	struct video_device        *vbi_dev;
	struct video_device        *radio_dev;

	/* pci i/o */
	struct pci_dev             *pci;
	unsigned char              pci_rev,pci_lat;


	/* capture queues */
	struct tw68_dmaqueue       vidq;
	struct tw68_dmaqueue       vbiq;

	/* various v4l controls */

	/* other global state info */
	struct tw6800_suspend_state state;
};

#if 0
/* ----------------------------------------------------------- */
/* function 1: audio/alsa stuff                                */
/* =============> moved to tw68-alsa.c <====================== */


/* ----------------------------------------------------------- */
/* function 2: mpeg stuff                                      */

struct cx6802_fh {
	struct cx6802_dev          *dev;
	struct videobuf_queue      mpegq;
};

struct cx6802_suspend_state {
	int                        disabled;
};

struct cx6802_driver {
	struct tw68_core *core;

	/* List of drivers attached to device */
	struct list_head drvlist;

	/* Type of driver and access required */
	enum tw68_board_type type_id;
//	enum cx6802_board_access hw_access;

	/* MPEG 8802 internal only */
	int (*suspend)(struct pci_dev *pci_dev, pm_message_t state);
	int (*resume)(struct pci_dev *pci_dev);

	/* MPEG 8802 -> mini driver - Driver probe and configuration */
	int (*probe)(struct cx6802_driver *drv);
	int (*remove)(struct cx6802_driver *drv);

	/* MPEG 8802 -> mini driver - Access for hardware control */
	int (*advise_acquire)(struct cx6802_driver *drv);
	int (*advise_release)(struct cx6802_driver *drv);

	/* MPEG 8802 <- mini driver - Access for hardware control */
	int (*request_acquire)(struct cx6802_driver *drv);
	int (*request_release)(struct cx6802_driver *drv);
};

struct cx6802_dev {
	struct tw68_core           *core;
	spinlock_t                 slock;

	/* pci i/o */
	struct pci_dev             *pci;
	unsigned char              pci_rev,pci_lat;

	/* dma queues */
	struct tw68_dmaqueue       mpegq;
	u32                        ts_packet_size;
	u32                        ts_packet_count;

	/* other global state info */
	struct cx6802_suspend_state state;

	/* for blackbird only */
	struct list_head           devlist;

#if defined(CONFIG_VIDEO_TW68_DVB) || defined(CONFIG_VIDEO_TW68_DVB_MODULE)
	/* for dvb only */
	struct videobuf_dvb_frontends frontends;
#endif

	/* for switching modulation types */
	unsigned char              ts_gen_cntrl;

	/* List of attached drivers */
	struct list_head	   drvlist;
	struct work_struct	   request_module_wk;
};
#endif

/* ----------------------------------------------------------- */
/* TODO - probably should use byte access for non-PCI regs     */
#define tw_readl(reg)             readl(core->lmmio + ((reg)>>2))
#define	tw_readb(reg)		 readb(core->bmmio + (reg))
#define tw_writel(reg,value)      writel((value), core->lmmio + ((reg)>>2))
#define	tw_writeb(reg,value)	 writeb((value), core->bmmio + (reg))

#define tw_andorl(reg,mask,value) \
  writel((readl(core->lmmio+((reg)>>2)) & ~(mask)) |\
  ((value) & (mask)), core->lmmio+((reg)>>2))
#define	tw_andorb(reg,mask,value) \
  writeb((readb(core->bmmio+(reg)) & ~(mask)) |\
  ((value) & (mask)), core->bmmio+(reg))
#define tw_setl(reg,bit)          tw_andorl((reg),(bit),(bit))
#define	tw_setb(reg,bit)	  tw_andorb((reg),(bit),(bit))
#define tw_clearl(reg,bit)        tw_andorl((reg),(bit),0)
#define	tw_clearb(reg,bit)	  tw_andorl((reg),(bit),0)

/* ----------------------------------------------------------- */
/* tw68-core.c                                                 */

extern void tw68_print_irqbits(char *name, char *tag, char **strings,
			       int len, u32 bits, u32 mask);

extern int tw68_core_irq(struct tw68_core *core, u32 status);
extern void tw68_wakeup(struct tw68_core *core,
			struct tw68_dmaqueue *q, u32 count);
extern void tw68_shutdown(struct tw68_core *core);
extern int tw68_reset(struct tw68_core *core);

extern int
tw68_risc_buffer(struct pci_dev *pci, struct btcx_riscmem *risc,
		 struct scatterlist *sglist,
		 unsigned int top_offset, unsigned int bottom_offset,
		 unsigned int bpl, unsigned int padding, unsigned int lines);
extern int
tw68_risc_databuffer(struct pci_dev *pci, struct btcx_riscmem *risc,
		     struct scatterlist *sglist, unsigned int bpl,
		     unsigned int lines, unsigned int lpi);
extern int
tw68_risc_stopper(struct pci_dev *pci, struct btcx_riscmem *risc);
extern void
tw68_free_buffer(struct videobuf_queue *q, struct tw68_buffer *buf);

extern void tw68_risc_disasm(struct tw68_core *core,
			     struct btcx_riscmem *risc);

#if 0
extern int tw68_sram_channel_setup(struct tw68_core *core,
				   struct sram_channel *ch,
				   unsigned int bpl, u32 risc);
extern void tw68_sram_channel_dump(struct tw68_core *core,
				   struct sram_channel *ch);
#endif

extern void tw68_risc_program_dump(struct tw68_core *core,
				   struct btcx_riscmem *risc);

extern int tw68_set_scale(struct tw68_core *core, unsigned int width,
			  unsigned int height, enum v4l2_field field);
extern int tw68_set_tvnorm(struct tw68_core *core, v4l2_std_id norm);

extern struct video_device *tw68_vdev_init(struct tw68_core *core,
					   struct pci_dev *pci,
					   struct video_device *template,
					   char *type);
extern struct tw68_core* tw68_core_get(struct pci_dev *pci);
extern void tw68_core_put(struct tw68_core *core,
			  struct pci_dev *pci);

extern int tw68_start_audio_dma(struct tw68_core *core);
extern int tw68_stop_audio_dma(struct tw68_core *core);


/* ----------------------------------------------------------- */
/* tw68-vbi.c                                                  */

/* Can be used as g_vbi_fmt, try_vbi_fmt and s_vbi_fmt */
int tw6800_vbi_fmt (struct file *file, void *priv,
					struct v4l2_format *f);

/*
int tw6800_start_vbi_dma(struct tw6800_dev    *dev,
			 struct tw68_dmaqueue *q,
			 struct tw68_buffer   *buf);
*/
int tw6800_stop_vbi_dma(struct tw6800_dev *dev);
int tw6800_restart_vbi_queue(struct tw6800_dev    *dev,
			     struct tw68_dmaqueue *q);
void tw6800_vbi_timeout(unsigned long data);

extern struct videobuf_queue_ops tw6800_vbi_qops;

/* ----------------------------------------------------------- */
/* tw68-i2c.c                                                  */

extern int tw68_i2c_init(struct tw68_core *core, struct pci_dev *pci);
extern void tw68_call_i2c_clients(struct tw68_core *core,
				  unsigned int cmd, void *arg);


/* ----------------------------------------------------------- */
/* tw68-cards.c                                                */

extern int tw68_tuner_callback(void *dev, int component, int command/*, int arg*/);
extern int tw68_get_resources(const struct tw68_core *core,
			      struct pci_dev *pci);
extern struct tw68_core *tw68_core_create(struct pci_dev *pci, int nr);
//extern void tw68_setup_xc3028(struct tw68_core *core, struct xc2028_ctrl *ctl);

/* ----------------------------------------------------------- */
/* tw68-tvaudio.c                                              */

#define WW_NONE		 1
#define WW_BTSC		 2
#define WW_BG		 3
#define WW_DK		 4
#define WW_I		 5
#define WW_L		 6
#define WW_EIAJ		 7
#define WW_I2SPT	 8
#define WW_FM		 9
#define WW_I2SADC	 10

void tw68_set_tvaudio(struct tw68_core *core);
void tw68_newstation(struct tw68_core *core);
void tw68_get_stereo(struct tw68_core *core, struct v4l2_tuner *t);
void tw68_set_stereo(struct tw68_core *core, u32 mode, int manual);
int tw68_audio_thread(void *data);

#if 0
int cx6802_register_driver(struct cx6802_driver *drv);
int cx6802_unregister_driver(struct cx6802_driver *drv);
struct cx6802_dev * cx6802_get_device(struct inode *inode);
struct cx6802_driver * cx6802_get_driver(struct cx6802_dev *dev, enum tw68_board_type btype);

/* ----------------------------------------------------------- */
/* tw68-input.c                                                */

int tw68_ir_init(struct tw68_core *core, struct pci_dev *pci);
int tw68_ir_fini(struct tw68_core *core);
void tw68_ir_irq(struct tw68_core *core);
void tw68_ir_start(struct tw68_core *core, struct tw68_IR *ir);
void tw68_ir_stop(struct tw68_core *core, struct tw68_IR *ir);

/* ----------------------------------------------------------- */
/* tw68-mpeg.c                                                 */

int cx6802_buf_prepare(struct videobuf_queue *q,struct cx6802_dev *dev,
			struct tw68_buffer *buf, enum v4l2_field field);
void cx6802_buf_queue(struct cx6802_dev *dev, struct tw68_buffer *buf);
void cx6802_cancel_buffers(struct cx6802_dev *dev);
#endif

/* ----------------------------------------------------------- */
/* tw68-video.c*/
extern const u32 tw68_user_ctrls[];
extern int tw6800_ctrl_query(struct tw68_core *core,
			     struct v4l2_queryctrl *qctrl);
int tw68_enum_input (struct tw68_core  *core,struct v4l2_input *i);
int tw68_set_freq (struct tw68_core  *core,struct v4l2_frequency *f);
int tw68_get_control(struct tw68_core *core, struct v4l2_control *ctl);
int tw68_set_control(struct tw68_core *core, struct v4l2_control *ctl);
int tw68_video_mux(struct tw68_core *core, unsigned int input);

