/*
 * Driver for Marvell PHY 88X2242
 *
 * Partially based on drivers/net/phy/bcm87xx.c
 *
 * Copyright (C) 2013 Texas Instruments Incorporated
 * Authors: WingMan Kwok <w-kwok2@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/gpio.h>

#define MARVELL_PHY_ID_88X2242	0x01410f12
#define MARVELL_PHY_ID_MASK	0xfffffc00

/* chip level regs */
#define MV88X2242_TX_SRC_N		(MII_ADDR_C45 | 0x1ff400)
#define MV88X2242_TX_SRC_M		(MII_ADDR_C45 | 0x1ff401)
#define MV88X2242_HOST_LANE_MUX		(MII_ADDR_C45 | 0x1ff402)
#define MV88X2242_HW_RESET		(MII_ADDR_C45 | 0x1ff404)

/* port level regs */
#define MV88X2242_PCS_CONFIG		(MII_ADDR_C45 | 0x1ff002)
#define MV88X2242_PORT_RESET		(MII_ADDR_C45 | 0x1ff003)
#define MV88X2242_GPIO_TRI_CTRL		(MII_ADDR_C45 | 0x1ff013)
#define MV88X2242_GPIO_INTR_TYPE3	(MII_ADDR_C45 | 0x1ff016)

/* SFI PMA regs */
#define MV88X2242_PMD_RX_SIGNAL_DETECT	(MII_ADDR_C45 | 0x01000a)

/* SFI 10BASE-R PCS regs */
#define MV88X2242_10GBASER_PCS_CTRL	(MII_ADDR_C45 | 0x030000)
#define MV88X2242_10GBASER_PCS_STATUS	(MII_ADDR_C45 | 0x030020)
#define MV88X2242_LASI_CONTROL		(MII_ADDR_C45 | 0x038000)
#define MV88X2242_LASI_STATUS		(MII_ADDR_C45 | 0x038001)

/* XFI 10BASE-R PCS regs */
#define MV88X2242_BASER_PCS_STATUS	(MII_ADDR_C45 | 0x040020)

/* FIXME */
static int mv88x2242_chip_cfg;

#if IS_ENABLED(CONFIG_OF_MDIO)
static int mv88x2242_chip_level_config(struct phy_device *phydev)
{
	int val;

	/* chip hw reset */
	phy_write(phydev, MV88X2242_HW_RESET, 0x4000);

	mdelay(10);

	/* Map ports */
	/* Added by TI for port mapping 0123:0123,
	   The default is M0-to-N0, M2-to-N1,
	   so setting bit 9 makes M1-to-N1 instead.
	   Must be done prior to the reset!
	   BEGR: I believe only one write is required for this,
		 as 31.F4xx registers are accessible through any of the
		 4 PHY addresses.
	*/
	phy_write(phydev, MV88X2242_HOST_LANE_MUX, BIT(9));

	/* Shut down unused lanes */
	val = phy_read(phydev, MV88X2242_TX_SRC_N);
	val &= 0x00ff;
	phy_write(phydev, MV88X2242_TX_SRC_N, val);

	val = phy_read(phydev, MV88X2242_TX_SRC_M);
	val &= 0x00ff;
	phy_write(phydev, MV88X2242_TX_SRC_M, val);

	return 0;
}

/* Set and/or override some configuration registers based on the
 * marvell,c45-reg-init property stored in the of_node for the phydev.
 *
 * marvell,c45-reg-init = <devid reg mask value>,...;
 *
 * There may be one or more sets of <devid reg mask value>:
 *
 * devid: which sub-device to use.
 * reg: the register.
 * mask: if non-zero, ANDed with existing register value.
 * value: ORed with the masked value and written to the regiser.
 *
 */
static int mv88x2242_of_reg_init(struct phy_device *phydev)
{
	int val;

	if (phydev->priv) {
		dev_info(&phydev->dev, "phy %d already configured\n",
			phydev->addr);
		return 0;
	}

	/*10gx2, 6G */
	phy_write(phydev, MV88X2242_PCS_CONFIG, 0x7171);

	/* pcs reset */
	phy_write(phydev, MV88X2242_PORT_RESET, 0x8080);

	mdelay(10);

	/* Required to Enable Optical module transmitter */
	/* For Fiber Tx Disable to enable optics (REQUIRED) */
	val = phy_read(phydev, MV88X2242_GPIO_INTR_TYPE3);
	val = (val & ~0x0018) | 0x0010;
	phy_write(phydev, MV88X2242_GPIO_INTR_TYPE3, val);

	/* Enable Marvell I2C SCL/SDA operations */
	val = phy_read(phydev, MV88X2242_GPIO_INTR_TYPE3);
	val = (val & ~0x0800) | 0x0800;
	phy_write(phydev, MV88X2242_GPIO_INTR_TYPE3, val);

	val = phy_read(phydev, MV88X2242_GPIO_INTR_TYPE3);
	val = (val & ~0x8000) | 0x8000;
	phy_write(phydev, MV88X2242_GPIO_INTR_TYPE3, val);

	val = phy_read(phydev, MV88X2242_GPIO_TRI_CTRL);
	val = (val & ~0x0800) | 0x0800;
	phy_write(phydev, MV88X2242_GPIO_TRI_CTRL, val);

	mdelay(10);

	phydev->priv = (void *)1;

	/* Read Each Lane Optical traceiver name? */

	return 0;
}
#else
static int mv88x2242_chip_level_config(struct phy_device *phydev)
{
	return 0;
}

static int mv88x2242_of_reg_init(struct phy_device *phydev)
{
	return 0;
}
#endif /* CONFIG_OF_MDIO */

static int mv88x2242_gpiochip_match_by_label(struct gpio_chip *chip, void *data)
{
	return !strcmp(chip->label, data);
}

static int mv88x2242_reset_phy(struct phy_device *phydev)
{
	struct gpio_chip *gc;

	gc = gpiochip_find("pca9555", mv88x2242_gpiochip_match_by_label);
	if (!gc) {
		dev_err(&phydev->dev,
			"Can't find gpio chip with label pca9555\n");
		return -ENODEV;
	} else
		dev_info(&phydev->dev,
			"Found gpio chip with label pca9555, gpio base = %d\n",
			gc->base);

	gpio_set_value_cansleep(gc->base + 8, 0);
	gpio_set_value_cansleep(gc->base + 9, 0);
	gpio_set_value_cansleep(gc->base + 10, 0);

	gpio_set_value_cansleep(gc->base + 8, 1);
	gpio_set_value_cansleep(gc->base + 9, 1);
	gpio_set_value_cansleep(gc->base + 10, 1);

	return 0;
}

static int mv88x2242_config_init(struct phy_device *phydev)
{
	if (!mv88x2242_chip_cfg) {
		/* chip level config */
		mv88x2242_reset_phy(phydev);
		mv88x2242_chip_level_config(phydev);
		mv88x2242_chip_cfg = 1;
	}

	/* port level config */
	mv88x2242_of_reg_init(phydev);

	phydev->supported = SUPPORTED_10000baseR_FEC;
	phydev->advertising = ADVERTISED_10000baseR_FEC;
	phydev->state = PHY_NOLINK;
	phydev->autoneg = AUTONEG_DISABLE;

	return 0;
}

static int mv88x2242_config_aneg(struct phy_device *phydev)
{
	return -EINVAL;
}

static int mv88x2242_read_status(struct phy_device *phydev)
{
	int rx_signal_detect;
	int pcs_status;

	rx_signal_detect = phy_read(phydev, MV88X2242_PMD_RX_SIGNAL_DETECT);
	if (rx_signal_detect < 0)
		return rx_signal_detect;

	if ((rx_signal_detect & BIT(0)) == 0)
		goto no_link;

	pcs_status = phy_read(phydev, MV88X2242_10GBASER_PCS_STATUS);
	if (pcs_status < 0)
		return pcs_status;

	if ((pcs_status & BIT(0)) == 0)
		goto no_link;

	pcs_status = phy_read(phydev, MV88X2242_BASER_PCS_STATUS);
	if (pcs_status < 0)
		return pcs_status;

	if ((pcs_status & BIT(12)) == 0) {
		dev_warn(&phydev->dev,
			"WARN: host side receive link down\n");
		goto no_link;
	}

	phydev->speed = 10000;
	phydev->link = 1;
	phydev->duplex = 1;
	return 0;

no_link:
	phydev->link = 0;
	return 0;
}

static int mv88x2242_config_intr(struct phy_device *phydev)
{
	int reg, err;

	reg = phy_read(phydev, MV88X2242_LASI_CONTROL);

	if (reg < 0)
		return reg;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		reg |= 1;
	else
		reg &= ~1;

	err = phy_write(phydev, MV88X2242_LASI_CONTROL, reg);
	return err;
}

static int mv88x2242_did_interrupt(struct phy_device *phydev)
{
	int reg;

	reg = phy_read(phydev, MV88X2242_LASI_STATUS);

	if (reg < 0) {
		dev_err(&phydev->dev,
			"Error: Read of MV88X2242_LASI_STATUS failed: %d\n",
			 reg);
		return 0;
	}
	return (reg & 1) != 0;
}

static int mv88x2242_ack_interrupt(struct phy_device *phydev)
{
	/* Reading the LASI status clears it. */
	mv88x2242_did_interrupt(phydev);
	return 0;
}

static int mv88x2242_match_phy_device(struct phy_device *phydev)
{
	return (phydev->c45_ids.device_ids[4] & MARVELL_PHY_ID_MASK) ==
		(MARVELL_PHY_ID_88X2242 & MARVELL_PHY_ID_MASK);
}

static struct phy_driver mv88x2242_driver[] = {
{
	.phy_id		= MARVELL_PHY_ID_88X2242,
	.phy_id_mask	= MARVELL_PHY_ID_MASK,
	.name		= "Marvell 88x2242",
	.flags		= PHY_HAS_INTERRUPT,
	.config_init	= mv88x2242_config_init,
	.config_aneg	= mv88x2242_config_aneg,
	.read_status	= mv88x2242_read_status,
	.ack_interrupt	= mv88x2242_ack_interrupt,
	.config_intr	= mv88x2242_config_intr,
	.did_interrupt	= mv88x2242_did_interrupt,
	.match_phy_device = mv88x2242_match_phy_device,
	.driver		= { .owner = THIS_MODULE },
}
};

static int __init mv88x2242_init(void)
{
	mv88x2242_chip_cfg = 0;

	return phy_drivers_register(mv88x2242_driver,
		ARRAY_SIZE(mv88x2242_driver));
}
module_init(mv88x2242_init);

static void __exit mv88x2242_exit(void)
{
	phy_drivers_unregister(mv88x2242_driver,
		ARRAY_SIZE(mv88x2242_driver));
}
module_exit(mv88x2242_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("WingMan Kwok <w-kwok2@ti.com>");
MODULE_DESCRIPTION("Driver For Marvell PHY 88X2242");
