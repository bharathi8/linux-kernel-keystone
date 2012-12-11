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

struct xhci_phy {
	unsigned int phy_utmi;
	unsigned int phy_pipe;
	unsigned int phy_param_ctrl_1;
	unsigned int phy_param_ctrl_2;
	unsigned int phy_clock;
	unsigned int phy_pll;
};

struct dwc3_keystone {
	spinlock_t		lock;
	struct platform_device	*dwc3;
	struct device		*dev;
	struct clk		*clk;
	u32			dma_status:1;
	u32			boot_cfg_usb_regs_base;
	u32			boot_cfg_usb_regs_size;
	struct xhci_phy		*phy;
};

#define PHY_SSC_EN			BIT(31)
#define PHY_REF_USE_PAD			BIT(30)
#define PHY_REF_SSP_EN			BIT(29)
#define PHY_FSEL			0x27
#define PHY_FSEL_SHIFT			22
#define PHY_RETENABLEEN			BIT(21)
#define PHY_REFCLKSEL			0x2
#define PHY_REFCLKSEL_SHIFT		19
#define PHY_OTGDISABLE			BIT(15)

#define PHY_SSC_REF_CLK_SEL		0
#define PHY_SSC_REF_CLK_SEL_SHIFT	4
#define PHY_MPLL_MULT			0x19
#define PHY_MPLL_MULT_SHIFT		13
#define PHY_REF_CLKDIV2			0

#define PHY_CLOCK_DEFAULT	(PHY_SSC_EN				| \
				 PHY_REF_USE_PAD			| \
				 PHY_REF_SSP_EN				| \
				 PHY_FSEL << PHY_FSEL_SHIFT		| \
				 PHY_RETENABLEEN			| \
				 PHY_REFCLKSEL << PHY_REFCLKSEL_SHIFT	| \
				 PHY_OTGDISABLE)
#define PHY_PLL_DEFAULT		(PHY_SSC_REF_CLK_SEL			| \
				 PHY_MPLL_MULT << PHY_MPLL_MULT_SHIFT)

static void keystone_usb_phy_init(struct xhci_phy *phy)
{
	/* Configure the PHY */
	writel(PHY_CLOCK_DEFAULT, &phy->phy_clock);
	writel(PHY_PLL_DEFAULT, &phy->phy_pll);
}

static void keystone_usb_phy_exit(struct xhci_phy *phy)
{
	/* Disable the PHY REFCLK clock gate */
	writel(0, &phy->phy_clock);
}

static int __devinit dwc3_keystone_probe(struct platform_device *pdev)
{
	struct dwc3_keystone	*keystone;
	struct platform_device	*dwc3;
	struct resource		*res;
	int devid, ret = -ENOMEM;

	keystone = kzalloc(sizeof(*keystone), GFP_KERNEL);
	if (!keystone) {
		dev_err(&pdev->dev, "not enough memory\n");
		goto err0;
	}

	platform_set_drvdata(pdev, keystone);

	devid = dwc3_get_device_id();
	if (devid < 0)
		goto err0;

	dwc3 = platform_device_alloc("dwc3", devid);
	if (!dwc3) {
		dev_err(&pdev->dev, "couldn't allocate dwc3 device\n");
		goto err2;
	}

	keystone->clk = clk_get(&pdev->dev, "usb");
	if (IS_ERR(keystone->clk)) {
		dev_err(&pdev->dev, "Unable to get Keystone USB clock\n");
		goto err3;
	}

	spin_lock_init(&keystone->lock);
	dma_set_coherent_mask(&dwc3->dev, pdev->dev.coherent_dma_mask);

	dwc3->dev.parent = &pdev->dev;
	dwc3->dev.dma_mask = pdev->dev.dma_mask;
	dwc3->dev.dma_parms = pdev->dev.dma_parms;
	keystone->dev	= &pdev->dev;
	keystone->dwc3	= dwc3;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(&pdev->dev, "missing memory resource 1\n");
		goto err4;
	}

	keystone->boot_cfg_usb_regs_base = res->start;
	keystone->boot_cfg_usb_regs_size = resource_size(res);

	res = devm_request_mem_region(&pdev->dev, res->start,
				      resource_size(res), dev_name(&pdev->dev));
	if (!res) {
		dev_err(&pdev->dev, "can't request mem region\n");
		goto err4;
	}

	keystone->phy = devm_ioremap(&pdev->dev, res->start,
				     resource_size(res));
	if (!keystone->phy) {
		dev_err(&pdev->dev, "ioremap failed\n");
		goto err4;
	}

	dev_dbg(&pdev->dev, "mem res 1 start=%08x size=%d mapped=%08x\n",
		(u32)(res->start), (int)resource_size(res),
		(u32)(keystone->phy));

	/* Enable the PHY REFCLK clock gate with phy_ref_ssp_en = 1 */
	writel(PHY_REF_SSP_EN, &(keystone->phy->phy_clock));

	ret = clk_prepare_enable(keystone->clk);
	if (ret < 0) {
		dev_dbg(&pdev->dev, "unable to enable USB clock, err %d\n",
			ret);
		goto err4;
	}

	/* Initialize usb phy */
	keystone_usb_phy_init(keystone->phy);

	ret = platform_device_add_resources(dwc3, pdev->resource,
					    pdev->num_resources);
	if (ret) {
		dev_err(&pdev->dev, "couldn't add resources to dwc3 device\n");
		goto err5;
	}

	ret = platform_device_add(dwc3);
	if (ret) {
		dev_err(&pdev->dev, "failed to register dwc3 device\n");
		goto err5;
	}

	return 0;

err5:
	keystone_usb_phy_exit(keystone->phy);
	clk_disable_unprepare(keystone->clk);
err4:
	clk_put(keystone->clk);
err3:
	platform_device_put(dwc3);
err2:
	dwc3_put_device_id(devid);
err0:
	return ret;
}

static int __devexit dwc3_keystone_remove(struct platform_device *pdev)
{
	struct dwc3_keystone	*keystone = platform_get_drvdata(pdev);

	platform_device_unregister(keystone->dwc3);
	dwc3_put_device_id(keystone->dwc3->id);
	keystone_usb_phy_exit(keystone->phy);

	if (keystone->clk) {
		clk_disable_unprepare(keystone->clk);
		clk_put(keystone->clk);
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
