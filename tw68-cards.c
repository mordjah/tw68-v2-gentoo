/*
 *
 * device driver for Techwell 6800 based video capture cards
 * card-specific stuff.
 *
 * (c) 2009 William M. Brack <wbrack@mmm.com.hk>
 *
 * The design and coding of this driver is heavily based upon the
 * cx88 driver originally written by Gerd Knorr and modified by
 * Mauro Carvalho Chehab, whose work is gratefully acknowledged.
 * Full credit goes to them - any problems are mine.
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
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>

#include "tw68.h"

static unsigned int card[] = {[0 ... (TW68_MAXBOARDS - 1)] = UNSET };

module_param_array (card, int, NULL, 0444);

MODULE_PARM_DESC (card, "card type");

static unsigned int latency = UNSET;
module_param (latency, int, 0444);
MODULE_PARM_DESC (latency, "pci latency timer");

#define info_printk(core, fmt, arg...) \
		printk(KERN_INFO "%s: " fmt, core->name , ## arg)

#define warn_printk(core, fmt, arg...) \
		printk(KERN_WARNING "%s: " fmt, core->name , ## arg)

#define err_printk(core, fmt, arg...) \
		printk(KERN_ERR "%s: " fmt, core->name , ## arg)


/* ------------------------------------------------------------------ */
/* board config info */

static const struct tw68_board tw68_boards[] = {
	[TW68_BOARD_UNKNOWN] = {
		.name = "UNKNOWN/GENERIC",
		.tuner_type = UNSET,
		.radio_type = UNSET,
		.tuner_addr = ADDR_UNSET,
		.radio_addr = ADDR_UNSET,
		.input = { {
			.type =
			TW68_VMUX_COMPOSITE1,
			.vmux = 0,
		}, {
			.type =
			TW68_VMUX_COMPOSITE2,
			.vmux = 1,
		}, {
			.type =
			TW68_VMUX_COMPOSITE3,
			.vmux = 2,
		}, {
			.type =
			TW68_VMUX_COMPOSITE4,
			.vmux = 3,
		}, },
	},
	[TW68_BOARD_6801] = {
		.name = "TW6801/GENERIC",
		.tuner_type = UNSET,
		.radio_type = UNSET,
		.tuner_addr = ADDR_UNSET,
		.radio_addr = ADDR_UNSET,
		.input = { {
			.type =
			TW68_VMUX_COMPOSITE1,
			.vmux = 0,
		}, {
			.type =
			TW68_VMUX_COMPOSITE2,
			.vmux = 1,
		}, {
			.type =
			TW68_VMUX_COMPOSITE3,
			.vmux = 2,
		}, {
			.type =
			TW68_VMUX_COMPOSITE4,
			.vmux = 3,
		} },
	},
};

/* ------------------------------------------------------------------*/
/* PCI subsystem IDs */

static const struct tw68_subid tw68_subids[] = {
	{
		.subvendor = 0x0000,
		.subdevice = 0x0000,
		.card = TW68_BOARD_6801,
	},
};

/* -----------------------------------------------------------------------*/

static void
tw68_card_list (struct tw68_core *core, struct pci_dev *pci)
{
	int     i;

	if (0 == pci->subsystem_vendor && 0 == pci->subsystem_device) {
		printk (KERN_ERR
			"%s: Your board has no valid PCI Subsystem ID "
			"and thus can't\n"
			"%s: be autodetected.  Please pass card=<n> "
			"insmod option to\n"
			"%s: workaround that.  Redirect complaints to "
			"the vendor of\n"
			"%s: the TV card.  Best regards,\n"
			"%s:         -- tw6800\n",
			core->name, core->name, core->name,
			core->name, core->name);
	} else {
		printk (KERN_ERR
			"%s: Your board isn't known (yet) to the "
			"driver.  You can\n"
			"%s: try to pick one of the existing card "
			"configs via\n"
			"%s: card=<n> insmod option.  Updating to the "
			"latest\n"
			"%s: version might help as well.\n",
			core->name, core->name, core->name,
			core->name);
	}
	err_printk (core,
		"Here is a list of valid choices for the card=<n> "
		"insmod option:\n");
	for (i = 0; i < ARRAY_SIZE (tw68_boards); i++)
		printk (KERN_ERR "%s:    card=%d -> %s\n",
			core->name, i, tw68_boards[i].name);
}

/* ------------------------------------------------------------------*/

static int tw68_pci_quirks (const char *name, struct pci_dev *pci)
{
	unsigned int lat = UNSET;

	/* check insmod options */
	if (UNSET != latency)
		lat = latency;

	if (UNSET != lat) {
		printk (KERN_INFO
			"%s: setting pci latency timer to %d\n", name,
			latency);
		pci_write_config_byte (pci, PCI_LATENCY_TIMER,
			latency);
	}
	return 0;
}

int
tw68_get_resources (const struct tw68_core *core, struct pci_dev *pci)
{
	if (request_mem_region (pci_resource_start (pci, 0),
			pci_resource_len (pci, 0), core->name))
		return 0;
	printk (KERN_ERR
		"%s/%d: Can't get MMIO memory @ 0x%llx, subsystem: %04x:%04x\n",
		core->name, PCI_FUNC (pci->devfn),
		(unsigned long long) pci_resource_start (pci, 0),
		pci->subsystem_vendor, pci->subsystem_device);
	return -EBUSY;
}

	/* Allocate and initialize the tw68 core struct.  One should
	 * hold the devlist mutex before calling this.  */
struct tw68_core *tw68_core_create (struct pci_dev *pci, int nr)
{
	struct tw68_core *core;
	int     i;

	core = kzalloc (sizeof (*core), GFP_KERNEL);

	atomic_inc (&core->refcount);
	core->pci_bus = pci->bus->number;
	core->pci_slot = PCI_SLOT (pci->devfn);
	core->pci_irqmask = 0;	/*TODO - initial impl has no non-video */
	mutex_init (&core->lock);

	core->nr = nr;
	sprintf (core->name, "tw68[%d]", core->nr);
	if (0 != tw68_get_resources (core, pci)) {
		kfree (core);
		return NULL;
	}

	/* PCI stuff */
	tw68_pci_quirks (core->name, pci);
	core->lmmio = ioremap (pci_resource_start (pci, 0),
		pci_resource_len (pci, 0));
	core->bmmio = (u8 __iomem *) core->lmmio;

	/* board config */
	if (card[core->nr] < ARRAY_SIZE (tw68_boards))
		core->boardnr = card[core->nr];
	else
		core->boardnr = UNSET;
	for (i = 0;
		UNSET == core->boardnr &&
		i < ARRAY_SIZE (tw68_subids); i++) {
		if (pci->subsystem_vendor == tw68_subids[i].subvendor
			&& pci->subsystem_device ==
			tw68_subids[i].subdevice) {
			core->boardnr = tw68_subids[i].card;
			break;
		}
	}
	if (UNSET == core->boardnr) {
		core->boardnr = TW68_BOARD_UNKNOWN;
		tw68_card_list (core, pci);
	}

	memcpy (&core->board, &tw68_boards[core->boardnr],
		sizeof (core->board));
	info_printk (core,
		"subsystem: %04x:%04x, board: %s [card=%d,%s], "
		"frontend(s): %d\n",
		pci->subsystem_vendor, pci->subsystem_device,
		core->board.name, core->boardnr,
		card[core->nr] ==
		core->boardnr ? "insmod option" : "autodetected",
		core->board.num_frontends);

	/* init hardware */
	tw68_reset (core);

	return core;
}
