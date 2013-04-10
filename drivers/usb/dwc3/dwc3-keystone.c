/**
 * dwc3-keystone.c - Keystone Specific Glue layer
 *
 * Copyright (C) 2010-2012 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: WingMan Kwok <w-kwok2@ti.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/of.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/ioport.h>
#include <linux/irqdomain.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>

#include "core.h"
#include "io.h"

#define BITS(n)			(BIT(n) - 1)
#define BITFIELD(x, s, n)	(((x) & BITS(n)) << (s))
#define MASK			0xffffffff

struct kdwc3_phy_regs {
	u32	phy_utmi;		/* USB_PHY_CTL0 */
	u32	phy_pipe;		/* USB_PHY_CTL1 */
	u32	phy_param_ctrl_1;	/* USB_PHY_CTL2 */
	u32	phy_param_ctrl_2;	/* USB_PHY_CTL3 */
	u32	phy_clock;		/* USB_PHY_CTL4 */
#define PHY_REF_SSP_EN(x)		BITFIELD(x, 29, 1)
	u32	phy_pll;		/* USB_PHY_CTL5 */
};

struct kdwc3_irq_regs {
	u32		revision;	/* 0x000 */
	u32		_rsvd0[3];
	u32		sysconfig;	/* 0x010 */
	u32		_rsvd1[1];
	u32		irq_eoi;
	u32		_rsvd2[1];
	struct {
		u32	raw_status;
		u32	status;
		u32	enable_set;
		u32	enable_clr;
	} irqs[16];
};

struct dwc3_keystone {
	spinlock_t		lock;
	struct platform_device	*dwc;
	struct device		*dev;
	struct clk		*clk;
	struct irq_domain	*domain;
	u32			dma_status:1;
	u32			phy_regs_base;
	u32			phy_regs_size;
	u32			usbss_regs_base;
	u32			usbss_regs_size;
	int			virq;

	struct kdwc3_phy_regs __iomem	*phy;
	struct kdwc3_irq_regs __iomem	*usbss;
};

static int kdwc3_phy_init(struct dwc3_keystone *kdwc)
{
	int error;
	u32 val;

	val  = readl(&kdwc->phy->phy_clock);
	writel(val | PHY_REF_SSP_EN(1), &kdwc->phy->phy_clock);
	udelay(20);

	error = clk_prepare_enable(kdwc->clk);
	if (error < 0) {
		dev_dbg(kdwc->dev, "unable to enable usb clock, err %d\n",
			error);
		writel(val, &kdwc->phy->phy_clock);
		return error;
	}

	/* soft reset usbss */
	writel(1, &kdwc->usbss->sysconfig);
	while (readl(&kdwc->usbss->sysconfig) & 1)
		;

	val = readl(&kdwc->usbss->revision);
	dev_info(kdwc->dev, "usbss revision %x\n", val);

	return 0;
}

static void kdwc3_phy_exit(struct dwc3_keystone *kdwc)
{
	u32 val;

	clk_disable_unprepare(kdwc->clk);

	val  = readl(&kdwc->phy->phy_clock);
	val &= ~PHY_REF_SSP_EN(MASK);
	writel(val, &kdwc->phy->phy_clock);
}

static void kdwc3_dev_exit(struct dwc3_keystone *kdwc)
{
	if (kdwc && kdwc->dwc) {
		platform_device_unregister(kdwc->dwc);
		kdwc->dwc = NULL;
	}
}

static void kdwc3_irq_mask(struct irq_data *d)
{
	struct dwc3_keystone *kdwc = d->chip_data;
	unsigned int irq = d->hwirq;
	writel(1, &kdwc->usbss->irqs[irq].enable_clr);
}

static void kdwc3_irq_unmask(struct irq_data *d)
{
	struct dwc3_keystone *kdwc = d->chip_data;
	unsigned int irq = d->hwirq;
	writel(1, &kdwc->usbss->irqs[irq].enable_set);
}

static void kdwc3_irq_ack(struct irq_data *d)
{
	struct dwc3_keystone *kdwc = d->chip_data;
	unsigned int irq = d->hwirq;
	writel(1, &kdwc->usbss->irqs[irq].status);
}

static inline void chained_irq_enter(struct irq_chip *chip,
				     struct irq_desc *desc)
{
	/* FastEOI controllers require no action on entry. */
	if (chip->irq_eoi)
		return;

	if (chip->irq_mask_ack) {
		chip->irq_mask_ack(&desc->irq_data);
	} else {
		chip->irq_mask(&desc->irq_data);
		if (chip->irq_ack)
			chip->irq_ack(&desc->irq_data);
	}
}

static inline void chained_irq_exit(struct irq_chip *chip,
				    struct irq_desc *desc)
{
	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
	else
		chip->irq_unmask(&desc->irq_data);
}

static void kdwc3_irq_handler(unsigned int irq, struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irq_data *d = irq_desc_get_irq_data(desc);
	struct dwc3_keystone *kdwc = d->handler_data;
	int virq, dwcirq = 0;

	chained_irq_enter(chip, desc);
	virq = irq_linear_revmap(kdwc->domain, dwcirq);
	dev_vdbg(kdwc->dev, "irq: dispatching virq %d\n", virq);
	generic_handle_irq(virq);
	writel(BIT(dwcirq), &kdwc->usbss->irq_eoi);
	chained_irq_exit(chip, desc);
}

static struct irq_chip kdwc3_irq_chip = {
	.name		= "DWC3",
	.irq_ack	= kdwc3_irq_ack,
	.irq_mask	= kdwc3_irq_mask,
	.irq_unmask	= kdwc3_irq_unmask,
};

static void kdwc3_irq_exit(struct dwc3_keystone *kdwc)
{
	struct device *dev = kdwc->dev;
	struct platform_device *pdev = to_platform_device(dev);
	int i, irq;

	for (i = 0; i < 256; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq <= 0)
			break;
		irq_set_handler_data(irq, NULL);
		irq_set_chained_handler(irq, NULL);
		dev_info(kdwc->dev, "unmapped irq %d\n", irq);
	}

	if (kdwc->virq >= 0) {
		irq_dispose_mapping(kdwc->virq);
		kdwc->virq = -1;
	}

	if (kdwc->domain) {
		irq_domain_remove(kdwc->domain);
		kdwc->domain = NULL;
	}
}

static int kdwc3_irq_init(struct dwc3_keystone *kdwc)
{
	struct device *dev = kdwc->dev;
	struct platform_device *pdev = to_platform_device(dev);
	int i, irq;

	kdwc->domain = irq_domain_add_linear(kdwc->dev->of_node, 1,
					     &irq_domain_simple_ops, kdwc);
	kdwc->virq = irq_create_mapping(kdwc->domain, 0);
	irq_set_chip_and_handler(kdwc->virq, &kdwc3_irq_chip, handle_level_irq);
	irq_set_chip_data(kdwc->virq, kdwc);
	set_irq_flags(kdwc->virq, IRQF_VALID);

	for (i = 0; i < 256; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq <= 0)
			break;
		irq_set_handler_data(irq, kdwc);
		irq_set_chained_handler(irq, kdwc3_irq_handler);
		dev_info(kdwc->dev, "mapped irq %d to virq %d\n", irq,
			 kdwc->virq);
	}

	if (!i)
		kdwc3_irq_exit(kdwc);

	return i ? 0 : -ENODEV;
}

static int kdwc3_dev_init(struct dwc3_keystone *kdwc)
{
	struct device *dev = kdwc->dev;
	struct platform_device *pdev = to_platform_device(dev), *dwc;
	struct resource resources[2], *res;
	struct platform_device_info info = {
		.parent		= dev,
		.name		= "dwc3",
		.res		= resources,
		.num_res	= ARRAY_SIZE(resources),
		.dma_mask	= dma_get_mask(dev),
	};

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "failed to find xhci resource\n");
		return -ENODEV;
	}
	
	memset(resources, 0, sizeof(resources));

	resources[0].start = res->start;
	resources[0].end   = res->end;
	resources[0].name  = res->name;
	resources[0].flags = res->flags;

	resources[1].start = kdwc->virq;
	resources[1].end   = kdwc->virq;
	resources[1].name  = "irq0";
	resources[1].flags = IORESOURCE_IRQ;

	dev_dbg(dev, "registering %s.%d, irq=%d, mem=%08lx-%08lx\n",
		info.name, 0 , (int)resources[1].start,
		(unsigned long)resources[0].start,
		(unsigned long)resources[0].end);

	dwc = platform_device_register_full(&info);
	if (IS_ERR(dwc)) {
		dev_err(dev, "failed to register dwc device\n");
		return PTR_ERR(dwc);
	}

	kdwc->dwc = dwc;

	return 0;
}

static int kdwc3_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dwc3_keystone *kdwc;
	struct resource *res;
	int error;

	kdwc = devm_kzalloc(dev, sizeof(*kdwc), GFP_KERNEL);
	if (!kdwc)
		return -ENOMEM;

	spin_lock_init(&kdwc->lock);
	kdwc->dev = dev;

	kdwc->clk = devm_clk_get(dev, "usb");
	if (IS_ERR_OR_NULL(kdwc->clk)) {
		dev_err(dev, "unable to get kdwc usb clock\n");
		return -ENODEV;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(dev, "missing usbss resource\n");
		return -ENODEV;
	}

	kdwc->usbss_regs_base = res->start;
	kdwc->usbss_regs_size = resource_size(res);

	res = devm_request_mem_region(dev, res->start,
				      resource_size(res), dev_name(dev));
	if (!res) {
		dev_err(dev, "can't request usbss region\n");
		return -ENODEV;
	}

	kdwc->usbss = devm_ioremap(dev, res->start, resource_size(res));
	if (!kdwc->usbss) {
		dev_err(dev, "ioremap failed on usbss region\n");
		return -ENODEV;
	}

	dev_dbg(dev, "usbss control start=%08x size=%d mapped=%08x\n",
		(u32)(res->start), (int)resource_size(res),
		(u32)(kdwc->usbss));

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!res) {
		dev_err(dev, "missing usb phy resource\n");
		return -ENODEV;
	}

	kdwc->phy_regs_base = res->start;
	kdwc->phy_regs_size = resource_size(res);

	res = devm_request_mem_region(dev, res->start,
				      resource_size(res), dev_name(dev));
	if (!res) {
		dev_err(dev, "can't request usb phy region\n");
		return -ENODEV;
	}

	kdwc->phy = devm_ioremap(dev, res->start, resource_size(res));
	if (!kdwc->phy) {
		dev_err(dev, "ioremap failed on usb phy region\n");
		return -ENODEV;
	}

	dev_dbg(dev, "phy control start=%08x size=%d mapped=%08x\n",
		(u32)(res->start), (int)resource_size(res),
		(u32)(kdwc->phy));

	/* Initialize usb phy */
	error = kdwc3_phy_init(kdwc);
	if (error)
		return error;

	error = kdwc3_irq_init(kdwc);
	if (error) {
		kdwc3_phy_exit(kdwc);
		return error;
	}

	error = kdwc3_dev_init(kdwc);
	if (error) {
		kdwc3_irq_exit(kdwc);
		kdwc3_phy_exit(kdwc);
		return error;
	}

	platform_set_drvdata(pdev, kdwc);

	return 0;
}

static int kdwc3_remove(struct platform_device *pdev)
{
	struct dwc3_keystone *kdwc = platform_get_drvdata(pdev);

	if (kdwc) {
		kdwc3_dev_exit(kdwc);
		kdwc3_irq_exit(kdwc);
		kdwc3_phy_exit(kdwc);
		platform_set_drvdata(pdev, NULL);
	}
	return 0;
}

static const struct of_device_id kdwc3_of_match[] = {
	{ .compatible = "ti,keystone-dwc3", },
	{},
};
MODULE_DEVICE_TABLE(of, kdwc3_of_match);

static struct platform_driver kdwc3_driver = {
	.probe		= kdwc3_probe,
	.remove		= kdwc3_remove,
	.driver		= {
		.name	= "keystone-dwc3",
		.owner	        = THIS_MODULE,
		.of_match_table	= kdwc3_of_match,
	},
};

module_platform_driver(kdwc3_driver);

MODULE_ALIAS("platform:keystone-dwc3");
MODULE_AUTHOR("WingMan Kwok");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DesignWare USB3 KEYSTONE Glue Layer");
