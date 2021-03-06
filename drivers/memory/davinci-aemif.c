/*
 * AEMIF support for DaVinci SoCs
 *
 * Copyright (C) 2010 Texas Instruments Incorporated. http://www.ti.com/
 * Copyright (C) Heiko Schocher <hs@denx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_data/mtd-davinci-aemif.h>
#include <linux/platform_device.h>
#include <linux/time.h>

#define TA_SHIFT	2
#define RHOLD_SHIFT	4
#define RSTROBE_SHIFT	7
#define RSETUP_SHIFT	13
#define WHOLD_SHIFT	17
#define WSTROBE_SHIFT	20
#define WSETUP_SHIFT	26
#define EW_SHIFT	30
#define SS_SHIFT	31

#define TA(x)		((x) << TA_SHIFT)
#define RHOLD(x)	((x) << RHOLD_SHIFT)
#define RSTROBE(x)	((x) << RSTROBE_SHIFT)
#define RSETUP(x)	((x) << RSETUP_SHIFT)
#define WHOLD(x)	((x) << WHOLD_SHIFT)
#define WSTROBE(x)	((x) << WSTROBE_SHIFT)
#define WSETUP(x)	((x) << WSETUP_SHIFT)
#define EW(x)		((x) << EW_SHIFT)
#define SS(x)		((x) << SS_SHIFT)

#define ASIZE_MAX	0x1
#define TA_MAX		0x3
#define RHOLD_MAX	0x7
#define RSTROBE_MAX	0x3f
#define RSETUP_MAX	0xf
#define WHOLD_MAX	0x7
#define WSTROBE_MAX	0x3f
#define WSETUP_MAX	0xf
#define EW_MAX		0x1
#define SS_MAX		0x1
#define NUM_CS		4

#define TA_VAL(x)	(((x) & TA(TA_MAX)) >> TA_SHIFT)
#define RHOLD_VAL(x)	(((x) & RHOLD(RHOLD_MAX)) >> RHOLD_SHIFT)
#define RSTROBE_VAL(x)	(((x) & RSTROBE(RSTROBE_MAX)) >> RSTROBE_SHIFT)
#define RSETUP_VAL(x)	(((x) & RSETUP(RSETUP_MAX)) >> RSETUP_SHIFT)
#define WHOLD_VAL(x)	(((x) & WHOLD(WHOLD_MAX)) >> WHOLD_SHIFT)
#define WSTROBE_VAL(x)	(((x) & WSTROBE(WSTROBE_MAX)) >> WSTROBE_SHIFT)
#define WSETUP_VAL(x)	(((x) & WSETUP(WSETUP_MAX)) >> WSETUP_SHIFT)
#define EW_VAL(x)	(((x) & EW(EW_MAX)) >> EW_SHIFT)
#define SS_VAL(x)	(((x) & SS(SS_MAX)) >> SS_SHIFT)


#define CONFIG_MASK	(TA(TA_MAX) | \
				RHOLD(RHOLD_MAX) | \
				RSTROBE(RSTROBE_MAX) |	\
				RSETUP(RSETUP_MAX) | \
				WHOLD(WHOLD_MAX) | \
				WSTROBE(WSTROBE_MAX) | \
				WSETUP(WSETUP_MAX) | \
				EW(EW_MAX) | SS(SS_MAX) | \
				ASIZE_MAX)

#define DRV_NAME "davinci-aemif"

struct aemif_device {
	struct davinci_aemif_pdata *cfg;
	void __iomem *base;
	struct clk *clk;
	/* clock rate in KHz */
	unsigned long clk_rate;
};

static struct aemif_device *aemif;
/**
 * aemif_calc_rate - calculate timing data.
 * @wanted: The cycle time needed in nanoseconds.
 * @clk: The input clock rate in kHz.
 * @max: The maximum divider value that can be programmed.
 *
 * On success, returns the calculated timing value minus 1 for easy
 * programming into AEMIF timing registers, else negative errno.
 */
static int aemif_calc_rate(int wanted, unsigned long clk, int max)
{
	int result;

	result = DIV_ROUND_UP((wanted * clk), NSEC_PER_MSEC) - 1;

	pr_debug("%s: result %d from %ld, %d\n", __func__, result, clk, wanted);

	/* It is generally OK to have a more relaxed timing than requested... */
	if (result < 0)
		result = 0;

	/* ... But configuring tighter timings is not an option. */
	else if (result > max)
		result = -EINVAL;

	return result;
}

/**
 * davinci_aemif_config_abus - configure async bus parameters given
 * AEMIF interface
 * @cs: chip-select to program the timing values for
 * @data: aemif chip select configuration
 * @base: aemif io mapped configuration base
 *
 * This function programs the given timing values (in real clock) into the
 * AEMIF registers taking the AEMIF clock into account.
 *
 * This function does not use any locking while programming the AEMIF
 * because it is expected that there is only one user of a given
 * chip-select.
 *
 * Returns 0 on success, else negative errno.
 */
static int davinci_aemif_config_abus(unsigned int cs,
				void __iomem *base,
				struct davinci_aemif_cs_data *data)
{
	int ta, rhold, rstrobe, rsetup, whold, wstrobe, wsetup;
	unsigned offset = A1CR_OFFSET + cs * 4;
	u32 set, val;

	if (!data)
		return -EINVAL;

	ta	= aemif_calc_rate(data->ta, aemif->clk_rate, TA_MAX);
	rhold	= aemif_calc_rate(data->rhold, aemif->clk_rate, RHOLD_MAX);
	rstrobe	= aemif_calc_rate(data->rstrobe, aemif->clk_rate, RSTROBE_MAX);
	rsetup	= aemif_calc_rate(data->rsetup, aemif->clk_rate, RSETUP_MAX);
	whold	= aemif_calc_rate(data->whold, aemif->clk_rate, WHOLD_MAX);
	wstrobe	= aemif_calc_rate(data->wstrobe, aemif->clk_rate, WSTROBE_MAX);
	wsetup	= aemif_calc_rate(data->wsetup, aemif->clk_rate, WSETUP_MAX);

	if (ta < 0 || rhold < 0 || rstrobe < 0 || rsetup < 0 ||
			whold < 0 || wstrobe < 0 || wsetup < 0) {
		pr_err("%s: cannot get suitable timings\n", __func__);
		return -EINVAL;
	}

	set = TA(ta) | RHOLD(rhold) | RSTROBE(rstrobe) | RSETUP(rsetup) |
		WHOLD(whold) | WSTROBE(wstrobe) | WSETUP(wsetup);

	set |= (data->asize & ACR_ASIZE_MASK);
	if (data->enable_ew)
		set |= ACR_EW_MASK;
	if (data->enable_ss)
		set |= ACR_SS_MASK;

	val = readl(aemif->base + offset);
	val &= ~CONFIG_MASK;
	val |= set;
	writel(val, aemif->base + offset);

	return 0;
}

inline int aemif_cycles_to_nsec(int val)
{
	return (val * NSEC_PER_MSEC) / aemif->clk_rate;
}

/**
 * davinci_aemif_get_hw_params - function to read hw register values
 * @cs: chip select
 * @data: ptr to cs data
 *
 * This function reads the defaults from the registers and update
 * the timing values. Required for get/set commands and also for
 * the case when driver needs to use defaults in hardware.
 */
static void davinci_aemif_get_hw_params(int cs,
		struct davinci_aemif_cs_data *data)
{
	u32 val, offset = A1CR_OFFSET + cs * 4;

	val = readl(aemif->base + offset);
	data->ta = aemif_cycles_to_nsec(TA_VAL(val));
	data->rhold = aemif_cycles_to_nsec(RHOLD_VAL(val));
	data->rstrobe = aemif_cycles_to_nsec(RSTROBE_VAL(val));
	data->rsetup = aemif_cycles_to_nsec(RSETUP_VAL(val));
	data->whold = aemif_cycles_to_nsec(WHOLD_VAL(val));
	data->wstrobe = aemif_cycles_to_nsec(WSTROBE_VAL(val));
	data->wsetup = aemif_cycles_to_nsec(WSETUP_VAL(val));
	data->enable_ew = EW_VAL(val);
	data->enable_ss = SS_VAL(val);
	data->asize = val & ASIZE_MAX;
}

/**
 * get_cs_data - helper function to get bus configuration data for a given cs
 * @cs: chip-select, values 2-5
 */
static struct davinci_aemif_cs_data *get_cs_data(int cs)
{
	int i;

	for (i = 0; i < aemif->cfg->num_cs; i++) {
		if (cs == aemif->cfg->cs_data[i].cs)
			break;
	}

	if (i == aemif->cfg->num_cs)
		return NULL;

	return &aemif->cfg->cs_data[i];
}

/**
 * davinci_aemif_set_abus_params - Set bus configuration data for a given cs
 * @cs: chip-select, values 2-5
 * @data: ptr to a struct to hold the configuration data to be set
 *
 * This function is called to configure emif bus parameters for a given cs.
 * Users call this function after calling davinci_aemif_get_abus_params()
 * to get current parameters, modify and call this function
 */
int davinci_aemif_set_abus_params(unsigned int cs,
			struct davinci_aemif_cs_data *data)
{
	struct davinci_aemif_cs_data *curr_cs_data;
	int ret = -EINVAL, chip_cs;

	if (data == NULL)
		return ret;

	if (aemif->base == NULL || aemif->cfg == NULL)
		return ret;

	/* translate to chip CS which starts at 2 */
	chip_cs = cs + 2;

	curr_cs_data = get_cs_data(chip_cs);
	if (curr_cs_data) {
		/* for configuration we use cs since it is used to index ACR */
		ret = davinci_aemif_config_abus(chip_cs, aemif->base, data);
		if (!ret) {
			*curr_cs_data = *data;
			return 0;
		}
	}

	return ret;
}
EXPORT_SYMBOL(davinci_aemif_set_abus_params);

/**
 * davinci_aemif_get_abus_params - Get bus configuration data for a given cs
 * @cs: chip-select, values 2-5
 * returns: ptr to a struct having the current configuration data
 */
struct davinci_aemif_cs_data *davinci_aemif_get_abus_params(unsigned int cs)
{
	if (aemif->base == NULL || aemif->cfg == NULL)
		return NULL;

	/* translate to chip CS which starts at 2 */
	return get_cs_data(cs + 2);
}
EXPORT_SYMBOL(davinci_aemif_get_abus_params);

#if defined(CONFIG_OF)
/**
 * dv_get_value - helper function to read a attribute valye
 * @np: device node ptr
 * @name: name of the attribute
 * returns: value of the attribute
 */
static int dv_get_value(struct device_node *np, const char *name)
{
	u32 data;
	int ret = -EINVAL;

	ret = of_property_read_u32(np, name, &data);
	if (ret != 0)
		return ret;

	return data;
}

/**
 * of_davinci_aemif_parse_abus_config - parse bus config data from a cs node
 * @np: device node ptr
 *
 * This function update the emif async bus configuration based on the values
 * configured in a cs device binding node.
 */
static int of_davinci_aemif_parse_abus_config(struct device_node *np)
{
	struct davinci_aemif_cs_data *data;
	unsigned long cs;
	int val;

	if (kstrtoul(np->name + 2, 10, &cs) < 0)
		return -EINVAL;

	if (cs < 2 || cs >= NUM_CS)
		return -EINVAL;

	if (aemif->cfg->num_cs >= NUM_CS)
		return -EINVAL;

	data = &aemif->cfg->cs_data[aemif->cfg->num_cs++];
	data->cs = cs;

	/* read the current value in the hw register */
	davinci_aemif_get_hw_params(cs - 2, data);

	/* override the values from device node */
	val = dv_get_value(np, "ti,davinci-cs-ta");
	if (val >= 0)
		data->ta = val;
	val = dv_get_value(np, "ti,davinci-cs-rhold");
	if (val >= 0)
		data->rhold	= val;
	val = dv_get_value(np, "ti,davinci-cs-rstrobe");
	if (val >= 0)
		data->rstrobe = val;
	val = dv_get_value(np, "ti,davinci-cs-rsetup");
	if (val >= 0)
		data->rsetup = val;
	val = dv_get_value(np, "ti,davinci-cs-whold");
	if (val >= 0)
		data->whold = val;
	val = dv_get_value(np, "ti,davinci-cs-wstrobe");
	if (val >= 0)
		data->wstrobe = val;
	val = dv_get_value(np, "ti,davinci-cs-wsetup");
	if (val >= 0)
		data->wsetup = val;
	val = dv_get_value(np, "ti,davinci-cs-asize");
	if (val >= 0)
		data->asize = val;
	val = dv_get_value(np, "ti,davinci-cs-ew");
	if (val >= 0)
		data->enable_ew	= val;
	val = dv_get_value(np, "ti,davinci-cs-ss");
	if (val >= 0)
		data->enable_ss	= val;
	return 0;
}
#endif

static const struct of_device_id davinci_aemif_of_match[] = {
	{ .compatible = "ti,davinci-aemif", },
	{},
};

static const struct of_device_id davinci_cs_of_match[] = {
	{ .compatible = "ti,davinci-cs", },
	{},
};

/**
 * of_davinci_aemif_cs_init - init cs data based on cs device nodes
 * @np: device node ptr
 *
 * For every controller device node, there is a cs device node that
 * describe the bus configuration parameters. This functions iterate
 * over these nodes and update the cs data array.
 */
static int of_davinci_aemif_cs_init(struct device_node *aemif_np)
{
	struct device_node *np = aemif_np;

	/* cs nodes are optional. So just return success */
	if (!np)
		return 0;

	for_each_matching_node(np, davinci_cs_of_match)
		of_davinci_aemif_parse_abus_config(np);
	return 0;
}

static int davinci_aemif_probe(struct platform_device *pdev)
{
	struct davinci_aemif_pdata *cfg;
	int ret  = -ENODEV, i;
	struct resource *res;

	aemif = devm_kzalloc(&pdev->dev, sizeof(*aemif), GFP_KERNEL);

	if (!aemif)
		return -ENOMEM;

	aemif->clk = clk_get(&pdev->dev, "aemif");
	if (IS_ERR(aemif->clk))
		return PTR_ERR(aemif->clk);

	clk_prepare_enable(aemif->clk);
	aemif->clk_rate = clk_get_rate(aemif->clk) / 1000;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("No IO memory address defined\n");
		goto error;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	aemif->base = devm_request_and_ioremap(&pdev->dev, res);
	if (!aemif->base) {
		ret = -EBUSY;
		pr_err("ioremap failed\n");
		goto error;
	}

	if (pdev->dev.platform_data == NULL) {
		/* Not platform data, we get the cs data from the cs nodes */
		cfg = devm_kzalloc(&pdev->dev, sizeof(*cfg), GFP_KERNEL);
		if (cfg == NULL)
			return -ENOMEM;

		aemif->cfg = cfg;
		if (of_davinci_aemif_cs_init(pdev->dev.of_node) < 0) {
			pr_err("No platform data or cs of node present\n");
			goto error;
		}
	} else {
		cfg = pdev->dev.platform_data;
		aemif->cfg = cfg;
	}

	for (i = 0; i < cfg->num_cs; i++) {
		/* cs is from 2-5. Internally we use cs-2 to access ACR */
		ret = davinci_aemif_config_abus(cfg->cs_data[i].cs - 2,
				aemif->base, &cfg->cs_data[i]);
		if (ret < 0) {
			pr_err("Error configuring chip select %d\n",
				cfg->cs_data[i].cs);
			goto error;
		}
	}
	return 0;
error:
	clk_disable_unprepare(aemif->clk);
	clk_put(aemif->clk);
	return ret;
}

static struct platform_driver davinci_aemif_driver = {
	.probe = davinci_aemif_probe,
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = davinci_aemif_of_match,
	},
};

static int __init davinci_aemif_init(void)
{
	return platform_driver_register(&davinci_aemif_driver);
}
subsys_initcall(davinci_aemif_init);

static void __exit davinci_aemif_exit(void)
{
	clk_disable_unprepare(aemif->clk);
	clk_put(aemif->clk);
	platform_driver_unregister(&davinci_aemif_driver);
}
module_exit(davinci_aemif_exit);

MODULE_AUTHOR("Murali Karicheri <m-karicheri2@ti.com>");
MODULE_DESCRIPTION("Texas Instruments AEMIF driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);
