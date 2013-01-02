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
#include <linux/io.h>
#include <linux/module.h>

#include "core.h"
#include "io.h"

struct dwc3_keystone_phy_regs {
	unsigned int phy_utmi;
	unsigned int phy_pipe;
	unsigned int phy_param_ctrl_1;
	unsigned int phy_param_ctrl_2;
	unsigned int phy_clock;
	unsigned int phy_pll;
};

struct dwc_keystone_usbss_regs {
};

struct dwc3_keystone {
	spinlock_t		lock;
	struct platform_device	*dwc;
	struct device		*dev;
	struct clk		*clk;
	u32			dma_status:1;
	u32			phy_regs_base;
	u32			phy_regs_size;
	u32			usbss_regs_base;
	u32			usbss_regs_size;

	struct dwc3_keystone_phy_regs __iomem	*phy;
	struct dwc3_keystone_usbss_regs __iomem	*usbss;
};

/* TODO: calculate and/or parametrize */
#define PHY_SSC_REF_CLK_SEL		0
#define PHY_SSC_REF_CLK_SEL_SHIFT	4
#define PHY_MPLL_MULT			0x19
#define PHY_MPLL_MULT_SHIFT		13
#define PHY_REF_CLKDIV2			0

#define PHY_SSC_EN			BIT(31)
#define PHY_REF_USE_PAD			BIT(30)
#define PHY_REF_SSP_EN			BIT(29)
#define PHY_FSEL			0x27
#define PHY_FSEL_SHIFT			22
#define PHY_RETENABLEEN			BIT(21)
#define PHY_REFCLKSEL			0x2
#define PHY_REFCLKSEL_SHIFT		19
#define PHY_OTGDISABLE			BIT(15)

#define PHY_CLOCK_DEFAULT	(PHY_SSC_EN				| \
				 PHY_REF_USE_PAD			| \
				 PHY_REF_SSP_EN				| \
				 PHY_FSEL << PHY_FSEL_SHIFT		| \
				 PHY_RETENABLEEN			| \
				 PHY_REFCLKSEL << PHY_REFCLKSEL_SHIFT	| \
				 PHY_OTGDISABLE)
#define PHY_PLL_DEFAULT		(PHY_SSC_REF_CLK_SEL			| \
				 PHY_MPLL_MULT << PHY_MPLL_MULT_SHIFT)

static int __devinit dwc3_keystone_phy_init(struct dwc3_keystone *kdwc)
{
	int error;

	/* enable the phy refclk clock gate  */
	writel(PHY_REF_SSP_EN, &kdwc->phy->phy_clock);

	error = clk_prepare_enable(kdwc->clk);
	if (error < 0) {
		dev_dbg(kdwc->dev, "unable to enable usb clock, err %d\n", error);
		writel(0, &kdwc->phy->phy_clock);
		return error;
	}

	/* configure the phy */
	writel(PHY_CLOCK_DEFAULT, &kdwc->phy->phy_clock);
	writel(PHY_PLL_DEFAULT, &kdwc->phy->phy_pll);

	return 0;
}

static void dwc3_keystone_phy_exit(struct dwc3_keystone *kdwc)
{
	clk_disable_unprepare(kdwc->clk);

	/* disable the phy refclk clock gate */
	writel(0, &kdwc->phy->phy_clock);
}

static void dwc3_keystone_dev_exit(struct dwc3_keystone *kdwc)
{
	int id;

	if (!kdwc->dwc)
		return;
	id = kdwc->dwc->id;
	platform_device_unregister(kdwc->dwc);
	dwc3_put_device_id(id);
	kdwc->dwc = NULL;
}

static int __devinit dwc3_keystone_dev_init(struct dwc3_keystone *kdwc)
{
	struct device *dev = kdwc->dev;
	struct platform_device *pdev = to_platform_device(dev), *dwc;
	struct platform_device_info info = {
		.parent		= dev,
		.name		= "dwc",
		.res		= pdev->resource,
		.num_res	= pdev->num_resources,
		.dma_mask	= *dev->dma_mask,
	};

	info.id = dwc3_get_device_id();
	if (info.id < 0) {
		dev_err(dev, "failed to get dwc device id\n");
		return info.id;
	}

	dwc = platform_device_register_full(&info);
	if (IS_ERR(dwc)) {
		dev_err(dev, "failed to register dwc device\n");
		dwc3_put_device_id(info.id);
		return PTR_ERR(dwc);
	}

	kdwc->dwc = dwc;

	return 0;
}

static int __devinit dwc3_keystone_probe(struct platform_device *pdev)
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
	error = dwc3_keystone_phy_init(kdwc);
	if (error)
		return error;

	error = dwc3_keystone_dev_init(kdwc);
	if (error) {
		dwc3_keystone_phy_exit(kdwc);
		return error;
	}

	platform_set_drvdata(pdev, kdwc);

	return 0;
}

static int __devexit dwc3_keystone_remove(struct platform_device *pdev)
{
	struct dwc3_keystone *kdwc = platform_get_drvdata(pdev);

	if (kdwc) {
		dwc3_keystone_dev_exit(kdwc);
		dwc3_keystone_phy_exit(kdwc);
		platform_set_drvdata(pdev, NULL);
	}
	return 0;
}

static const struct of_device_id of_dwc3_match[] = {
	{ .compatible = "ti,keystone-dwc3", },
	{},
};
MODULE_DEVICE_TABLE(of, of_dwc3_matach);

static struct platform_driver dwc3_keystone_driver = {
	.probe		= dwc3_keystone_probe,
	.remove		= __devexit_p(dwc3_keystone_remove),
	.driver		= {
		.name	= "keystone-dwc3",
		.owner	        = THIS_MODULE,
		.of_match_table	= of_dwc3_match,
	},
};

module_platform_driver(dwc3_keystone_driver);

MODULE_ALIAS("platform:keystone-dwc3");
MODULE_AUTHOR("WingMan Kwok");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DesignWare USB3 KEYSTONE Glue Layer");
