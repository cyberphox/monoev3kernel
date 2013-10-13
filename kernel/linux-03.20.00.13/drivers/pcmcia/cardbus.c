/*
 * cardbus.c -- 16-bit PCMCIA core support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * The initial developer of the original code is David A. Hinds
 * <dahinds@users.sourceforge.net>.  Portions created by David A. Hinds
 * are Copyright (C) 1999 David A. Hinds.  All Rights Reserved.
 *
 * (C) 1999		David A. Hinds
 */

/*
 * Cardbus handling has been re-written to be more of a PCI bridge thing,
 * and the PCI code basically does all the resource handling.
 *
 *		Linus, Jan 2000
 */


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <asm/irq.h>

#include <pcmcia/cs_types.h>
#include <pcmcia/ss.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include "cs_internal.h"

/*====================================================================*/

/* Offsets in the Expansion ROM Image Header */
#define ROM_SIGNATURE		0x0000	/* 2 bytes */
#define ROM_DATA_PTR		0x0018	/* 2 bytes */

/* Offsets in the CardBus PC Card Data Structure */
#define PCDATA_SIGNATURE	0x0000	/* 4 bytes */
#define PCDATA_VPD_PTR		0x0008	/* 2 bytes */
#define PCDATA_LENGTH		0x000a	/* 2 bytes */
#define PCDATA_REVISION		0x000c
#define PCDATA_IMAGE_SZ		0x0010	/* 2 bytes */
#define PCDATA_ROM_LEVEL	0x0012	/* 2 bytes */
#define PCDATA_CODE_TYPE	0x0014
#define PCDATA_INDICATOR	0x0015

/*=====================================================================

    Expansion ROM's have a special layout, and pointers specify an
    image number and an offset within that image.  xlate_rom_addr()
    converts an image/offset address to an absolute offset from the
    ROM's base address.

=====================================================================*/

static u_int xlate_rom_addr(void __iomem *b, u_int addr)
{
	u_int img = 0, ofs = 0, sz;
	u_short data;
	while ((readb(b) == 0x55) && (readb(b + 1) == 0xaa)) {
		if (img == (addr >> 28))
			return (addr & 0x0fffffff) + ofs;
		data = readb(b + ROM_DATA_PTR) + (readb(b + ROM_DATA_PTR + 1) << 8);
		sz = 512 * (readb(b + data + PCDATA_IMAGE_SZ) +
			    (readb(b + data + PCDATA_IMAGE_SZ + 1) << 8));
		if ((sz == 0) || (readb(b + data + PCDATA_INDICATOR) & 0x80))
			break;
		b += sz;
		ofs += sz;
		img++;
	}
	return 0;
}

/*=====================================================================

    These are similar to setup_cis_mem and release_cis_mem for 16-bit
    cards.  The "result" that is used externally is the cb_cis_virt
    pointer in the struct pcmcia_socket structure.

=====================================================================*/

static void cb_release_cis_mem(struct pcmcia_socket *s)
{
	if (s->cb_cis_virt) {
		dev_dbg(&s->dev, "cb_release_cis_mem()\n");
		iounmap(s->cb_cis_virt);
		s->cb_cis_virt = NULL;
		s->cb_cis_res = NULL;
	}
}

static int cb_setup_cis_mem(struct pcmcia_socket *s, struct resource *res)
{
	unsigned int start, size;

	if (res == s->cb_cis_res)
		return 0;

	if (s->cb_cis_res)
		cb_release_cis_mem(s);

	start = res->start;
	size = res->end - start + 1;
	s->cb_cis_virt = ioremap(start, size);

	if (!s->cb_cis_virt)
		return -1;

	s->cb_cis_res = res;

	return 0;
}

/*=====================================================================

    This is used by the CIS processing code to read CIS information
    from a CardBus device.

=====================================================================*/

int read_cb_mem(struct pcmcia_socket *s, int space, u_int addr, u_int len,
		void *ptr)
{
	struct pci_dev *dev;
	struct resource *res;

	dev_dbg(&s->dev, "read_cb_mem(%d, %#x, %u)\n", space, addr, len);

	dev = pci_get_slot(s->cb_dev->subordinate, 0);
	if (!dev)
		goto fail;

	/* Config space? */
	if (space == 0) {
		if (addr + len > 0x100)
			goto failput;
		for (; len; addr++, ptr++, len--)
			pci_read_config_byte(dev, addr, ptr);
		return 0;
	}

	res = dev->resource + space - 1;

	pci_dev_put(dev);

	if (!res->flags)
		goto fail;

	if (cb_setup_cis_mem(s, res) != 0)
		goto fail;

	if (space == 7) {
		addr = xlate_rom_addr(s->cb_cis_virt, addr);
		if (addr == 0)
			goto fail;
	}

	if (addr + len > res->end - res->start)
		goto fail;

	memcpy_fromio(ptr, s->cb_cis_virt + addr, len);
	return 0;

failput:
	pci_dev_put(dev);
fail:
	memset(ptr, 0xff, len);
	return -1;
}

/*=====================================================================

    cb_alloc() and cb_free() allocate and free the kernel data
    structures for a Cardbus device, and handle the lowest level PCI
    device setup issues.

=====================================================================*/

static void cardbus_config_irq_and_cls(struct pci_bus *bus, int irq)
{
	struct pci_dev *dev;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		u8 irq_pin;

		/*
		 * Since there is only one interrupt available to
		 * CardBus devices, all devices downstream of this
		 * device must be using this IRQ.
		 */
		pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &irq_pin);
		if (irq_pin) {
			dev->irq = irq;
			pci_write_config_byte(dev, PCI_INTERRUPT_LINE, dev->irq);
		}

		/*
		 * Some controllers transfer very slowly with 0 CLS.
		 * Configure it.  This may fail as CLS configuration
		 * is mandatory only for MWI.
		 */
		pci_set_cacheline_size(dev);

		if (dev->subordinate)
			cardbus_config_irq_and_cls(dev->subordinate, irq);
	}
}

int __ref cb_alloc(struct pcmcia_socket *s)
{
	struct pci_bus *bus = s->cb_dev->subordinate;
	struct pci_dev *dev;
	unsigned int max, pass;

	s->functions = pci_scan_slot(bus, PCI_DEVFN(0, 0));
	pci_fixup_cardbus(bus); 

	max = bus->secondary;
	for (pass = 0; pass < 2; pass++)
		list_for_each_entry(dev, &bus->devices, bus_list)
			if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE ||
			    dev->hdr_type == PCI_HEADER_TYPE_CARDBUS)
				max = pci_scan_bridge(bus, dev, max, pass);

	/*
	 * Size all resources below the CardBus controller.
	 */
	pci_bus_size_bridges(bus);
	pci_bus_assign_resources(bus);
	cardbus_config_irq_and_cls(bus, s->pci_irq);

	/* socket specific tune function */
	if (s->tune_bridge)
		s->tune_bridge(s, bus);

	pci_enable_bridges(bus);
	pci_bus_add_devices(bus);

	s->irq.AssignedIRQ = s->pci_irq;
	return 0;
}

void cb_free(struct pcmcia_socket *s)
{
	struct pci_dev *bridge = s->cb_dev;

	cb_release_cis_mem(s);

	if (bridge)
		pci_remove_behind_bridge(bridge);
}
