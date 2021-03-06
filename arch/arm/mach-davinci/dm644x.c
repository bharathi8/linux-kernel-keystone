/*
 * TI DaVinci DM644x chip specific setup
 *
 * Author: Kevin Hilman, Deep Root Systems, LLC
 *
 * 2007 (c) Deep Root Systems, LLC. This file is licensed under
 * the terms of the GNU General Public License version 2. This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/serial_8250.h>
#include <linux/platform_device.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/platform_data/clk-davinci-pll.h>
#include <linux/platform_data/clk-davinci-psc.h>
#include <linux/platform_data/davinci-clock.h>
#include <mach/pll.h>

#include <asm/mach/map.h>

#include <mach/cputype.h>
#include <mach/edma.h>
#include <mach/irqs.h>
#include <mach/psc.h>
#include <mach/mux.h>
#include <mach/time.h>
#include <mach/serial.h>
#include <mach/common.h>
#include <mach/gpio-davinci.h>

#include "davinci.h"
#include "mux.h"
#include "asp.h"

#define PLLM		0x110
#define PREDIV          0x114
#define POSTDIV         0x128
#define PLLM_PLLM_MASK  0xff

/*
 * Device specific clocks
 */
#define DM644X_REF_FREQ		27000000

#define DM644X_EMAC_BASE		0x01c80000
#define DM644X_EMAC_MDIO_BASE		(DM644X_EMAC_BASE + 0x4000)
#define DM644X_EMAC_CNTRL_OFFSET	0x0000
#define DM644X_EMAC_CNTRL_MOD_OFFSET	0x1000
#define DM644X_EMAC_CNTRL_RAM_OFFSET	0x2000
#define DM644X_EMAC_CNTRL_RAM_SIZE	0x2000

static struct clk_davinci_pll_data pll1_data = {
	.phy_pllm	= DAVINCI_PLL1_BASE + PLLM,
	.phy_prediv	= DAVINCI_PLL1_BASE + PREDIV,
	.phy_postdiv	= DAVINCI_PLL1_BASE + POSTDIV,
	.pllm_mask	= PLLM_PLLM_MASK,
	.prediv_mask	= PLLDIV_RATIO_MASK,
	.postdiv_mask	= PLLDIV_RATIO_MASK,
	.num		= 1,
};

static struct clk_fixed_rate_data clkin_data = {
	.rate		= DM644X_REF_FREQ,
	.flags		= CLK_IS_ROOT,
};

static struct davinci_clk ref_clk_clkin = {
	.name		= "clkin",
	.type		=  DAVINCI_FIXED_RATE_CLK,
	.clk_data	=  {
		.data	= &clkin_data,
	},
};

static struct clk_fixed_rate_data oscin_data = {
	.rate		= DM644X_REF_FREQ,
	.flags		= CLK_IS_ROOT,
};

static struct davinci_clk ref_clk_oscin = {
	.name		= "oscin",
	.type		=  DAVINCI_FIXED_RATE_CLK,
	.clk_data	=  {
		.data	= &oscin_data,
	},
};

static const char *ref_clk_mux_parents[] = {"clkin", "oscin"};

static struct clk_mux_data ref_clk_mux_data = {
	.shift		= PLLCTL_CLKMODE_SHIFT,
	.width		= PLLCTL_CLKMODE_WIDTH,
	.num_parents	= ARRAY_SIZE(ref_clk_mux_parents),
	.parents	= ref_clk_mux_parents,
	.phys_base	= DAVINCI_PLL1_BASE + PLLCTL,
};

static struct davinci_clk ref_clk_mux = {
	.name		= "ref_clk_mux",
	.parent		= &ref_clk_clkin,
	.type		= DAVINCI_MUX_CLK,
	.clk_data	=  {
		.data	= &ref_clk_mux_data,
	}
};

static struct davinci_clk pll1_clk = {
	.name		= "pll1",
	.parent		= &ref_clk_mux,
	.type		= DAVINCI_MAIN_PLL_CLK,
	.clk_data = {
		.data	= &pll1_data,
	},
};

static const char *pll1_plldiv_clk_mux_parents[] = {
						"ref_clk_mux", "pll1"};

static struct clk_mux_data pll1_plldiv_clk_mux_data = {
	.shift		= PLLCTL_PLLEN_SHIFT,
	.width		= PLLCTL_PLLEN_WIDTH,
	.num_parents	= ARRAY_SIZE(pll1_plldiv_clk_mux_parents),
	.parents	= pll1_plldiv_clk_mux_parents,
	.phys_base	= DAVINCI_PLL1_BASE + PLLCTL,
};

static struct davinci_clk pll1_plldiv_clk_mux = {
	.name		= "pll1_plldiv_clk_mux",
	.parent		= &pll1_clk,
	.type		= DAVINCI_MUX_CLK,
	.clk_data	= {
		.data	= &pll1_plldiv_clk_mux_data,
	},
};

#define define_pll1_div_clk(__pll, __div, __name)		\
	static struct clk_divider_data pll1_div_data##__div = {	\
		.div_reg	= DAVINCI_PLL1_BASE + PLLDIV##__div,	\
		.width		= 5,				\
	};							\
								\
	static struct davinci_clk __name = {			\
		.name		= #__name,			\
		.parent		= &__pll,			\
		.type		= DAVINCI_PRG_DIV_CLK,		\
		.clk_data	= {				\
			.data	=  &pll1_div_data##__div,	\
		},						\
	};

define_pll1_div_clk(pll1_plldiv_clk_mux, 1, pll1_sysclk1);
define_pll1_div_clk(pll1_plldiv_clk_mux, 2, pll1_sysclk2);
define_pll1_div_clk(pll1_plldiv_clk_mux, 3, pll1_sysclk3);
define_pll1_div_clk(pll1_plldiv_clk_mux, 4, pll1_sysclk4);
define_pll1_div_clk(pll1_plldiv_clk_mux, 5, pll1_sysclk5);

static struct clk_divider_data pll1_sysclkbp_data = {
	.div_reg	= BPDIV,
};

static struct davinci_clk pll1_sysclkbp = {
	.name		= "pll1_sysclkbp",
	.parent		= &ref_clk_mux,
	.type		= DAVINCI_PRG_DIV_CLK,
	.clk_data	= {
		.data	= &pll1_sysclkbp_data,
	},
};

static struct clk_davinci_pll_data pll2_data = {
	.phy_pllm	= DAVINCI_PLL2_BASE + PLLM,
	.phy_prediv	= DAVINCI_PLL2_BASE + PREDIV,
	.phy_postdiv	= DAVINCI_PLL2_BASE + POSTDIV,
	.pllm_mask	= PLLM_PLLM_MASK,
	.prediv_mask	= PLLDIV_RATIO_MASK,
	.postdiv_mask	= PLLDIV_RATIO_MASK,
	.num = 2,
};

static struct davinci_clk pll2_clk = {
	.name		= "pll2",
	.type		= DAVINCI_MAIN_PLL_CLK,
	.parent		= &ref_clk_mux,
	.clk_data	= {
		.data	= &pll2_data,
	},
};

#define define_pll2_div_clk(__pll, __div, __name)	\
	static struct clk_divider_data pll2_div_data##__div = {	\
		.div_reg	= DAVINCI_PLL2_BASE + PLLDIV##__div,	\
		.width		= 5,				\
	};							\
								\
	static struct davinci_clk __name = {			\
		.name		= #__name,			\
		.parent		= &__pll,			\
		.type		= DAVINCI_PRG_DIV_CLK,		\
		.clk_data	= {				\
			.data	=  &pll2_div_data##__div,	\
		},						\
	};

static const char *pll2_plldiv_clk_mux_parents[] = {
						"ref_clk_mux", "pll2"};

static struct clk_mux_data pll2_plldiv_clk_mux_data = {
	.shift		= PLLCTL_PLLEN_SHIFT,
	.width		= PLLCTL_PLLEN_WIDTH,
	.num_parents	= ARRAY_SIZE(pll2_plldiv_clk_mux_parents),
	.parents	= pll2_plldiv_clk_mux_parents,
	.phys_base	= DAVINCI_PLL2_BASE + PLLCTL,
};

static struct davinci_clk pll2_plldiv_clk_mux = {
	.name		= "pll2_plldiv_clk_mux",
	.parent		= &pll2_clk,
	.type		= DAVINCI_MUX_CLK,
	.clk_data	= {
		.data	= &pll2_plldiv_clk_mux_data,
	},
};

define_pll2_div_clk(pll2_plldiv_clk_mux, 1, pll2_sysclk1);
define_pll2_div_clk(pll2_plldiv_clk_mux, 2, pll2_sysclk2);

static struct clk_divider_data pll2_sysclkbp_data = {
	.div_reg	= DAVINCI_PLL2_BASE + BPDIV,
	.width		= 5,
};

static struct davinci_clk pll2_sysclkbp = {
	.name		= "pll2_sysclkbp",
	.parent		= &ref_clk_mux,
	.type		= DAVINCI_PRG_DIV_CLK,
	.clk_data	= {
		.data	= &pll2_sysclkbp_data,
	},
};

#define __lpsc_clk(cname, _parent, mod, flgs, _flgs, dom)	\
	static struct clk_davinci_psc_data clk_psc_data##cname = {	\
		.domain	= DAVINCI_GPSC_##dom,			\
		.lpsc	= DAVINCI_LPSC_##mod,			\
		.flags	= flgs,					\
	};							\
								\
	static struct davinci_clk clk_##cname = {		\
		.name		= #cname,			\
		.parent		= &_parent,			\
		.flags		= _flgs,			\
		.type		= DAVINCI_PSC_CLK,		\
		.clk_data	= {				\
			.data	= &clk_psc_data##cname		\
		},						\
	};

#define lpsc_clk_enabled(cname, parent, mod)		\
	__lpsc_clk(cname, parent, mod, 0, ALWAYS_ENABLED, ARMDOMAIN)

#define lpsc_clk(cname, flgs, parent, mod, dom)		\
	__lpsc_clk(cname, parent, mod, flgs, 0, dom)

lpsc_clk_enabled(arm, pll1_sysclk2, ARM);
lpsc_clk(dsp, CLK_IGNORE_UNUSED, pll1_sysclk1, GEM, DSPDOMAIN);
lpsc_clk(vicp, CLK_IGNORE_UNUSED, pll1_sysclk2, IMCOP, DSPDOMAIN);
lpsc_clk(vpss_master, 0, pll1_sysclk3, VPSSMSTR, ARMDOMAIN);
lpsc_clk(vpss_slave, 0, pll1_sysclk3, VPSSSLV, ARMDOMAIN);
lpsc_clk(uart0, 0, ref_clk_mux, UART0, ARMDOMAIN);
lpsc_clk(uart1, 0, ref_clk_mux, UART1, ARMDOMAIN);
lpsc_clk(uart2, 0, ref_clk_mux, UART2, ARMDOMAIN);
lpsc_clk(emac, 0, pll1_sysclk5, EMAC_WRAPPER, ARMDOMAIN);
lpsc_clk(i2c, 0, ref_clk_mux, I2C, ARMDOMAIN);
lpsc_clk(ide, 0, pll1_sysclk5, ATA, ARMDOMAIN);
lpsc_clk(asp0, 0, pll1_sysclk5, McBSP, ARMDOMAIN);
lpsc_clk(mmcsd, 0, pll1_sysclk5, MMC_SD, ARMDOMAIN);
lpsc_clk(spi, 0, pll1_sysclk5, SPI, ARMDOMAIN);
lpsc_clk(gpio, 0, pll1_sysclk5, GPIO, ARMDOMAIN);
lpsc_clk(usb, 0, pll1_sysclk5, USB, ARMDOMAIN);
lpsc_clk(vlynq, 0, pll1_sysclk5, VLYNQ, ARMDOMAIN);
lpsc_clk(aemif, 0, pll1_sysclk5, AEMIF, ARMDOMAIN);
lpsc_clk(pwm0, 0, ref_clk_mux, PWM0, ARMDOMAIN);
lpsc_clk(pwm1, 0, ref_clk_mux, PWM1, ARMDOMAIN);
lpsc_clk(pwm2, 0, ref_clk_mux, PWM2, ARMDOMAIN);
lpsc_clk(timer0, 0, ref_clk_mux, TIMER0, ARMDOMAIN);
lpsc_clk(timer1, 0, ref_clk_mux, TIMER1, ARMDOMAIN);
lpsc_clk(timer2, CLK_IGNORE_UNUSED, ref_clk_mux, TIMER2, ARMDOMAIN);

static struct davinci_clk_lookup dm644x_clks[] = {
	CLK(NULL, "clkin", &ref_clk_clkin),
	CLK(NULL, "oscin", &ref_clk_oscin),
	CLK(NULL, "ref_clk_mux", &ref_clk_mux),
	CLK(NULL, "pll1", &pll1_clk),
	CLK(NULL, "pll1_plldiv_clk_mux", &pll1_plldiv_clk_mux),
	CLK(NULL, "pll1_sysclk1", &pll1_sysclk1),
	CLK(NULL, "pll1_sysclk2", &pll1_sysclk2),
	CLK(NULL, "pll1_sysclk3", &pll1_sysclk3),
	CLK(NULL, "pll1_sysclk4", &pll1_sysclk4),
	CLK(NULL, "pll1_sysclk5", &pll1_sysclk5),
	CLK(NULL, "pll1_sysclkbp", &pll1_sysclkbp),
	CLK(NULL, "pll2", &pll2_clk),
	CLK(NULL, "pll2_plldiv_clk_mux", &pll2_plldiv_clk_mux),
	CLK(NULL, "pll2_sysclk1", &pll2_sysclk1),
	CLK(NULL, "pll2_sysclk2", &pll2_sysclk2),
	CLK(NULL, "pll2_sysclkbp", &pll2_sysclkbp),
	CLK(NULL, "dsp", &clk_dsp),
	CLK(NULL, "arm", &clk_arm),
	CLK(NULL, "vicp", &clk_vicp),
	CLK(NULL, "vpss_master", &clk_vpss_master),
	CLK(NULL, "vpss_slave", &clk_vpss_slave),
	CLK(NULL, "uart0", &clk_uart0),
	CLK(NULL, "uart1", &clk_uart1),
	CLK(NULL, "uart2", &clk_uart2),
	CLK("davinci_emac.1", NULL, &clk_emac),
	CLK("i2c_davinci.1", NULL, &clk_i2c),
	CLK("palm_bk3710", NULL, &clk_ide),
	CLK("davinci-mcbsp", NULL, &clk_asp0),
	CLK("davinci_mmc.0", NULL, &clk_mmcsd),
	CLK(NULL, "spi", &clk_spi),
	CLK(NULL, "gpio", &clk_gpio),
	CLK(NULL, "usb", &clk_usb),
	CLK(NULL, "vlynq", &clk_vlynq),
	CLK(NULL, "aemif", &clk_aemif),
	CLK(NULL, "pwm0", &clk_pwm0),
	CLK(NULL, "pwm1", &clk_pwm1),
	CLK(NULL, "pwm2", &clk_pwm2),
	CLK(NULL, "timer0", &clk_timer0),
	CLK(NULL, "timer1", &clk_timer1),
	CLK("watchdog", NULL, &clk_timer2),
	CLK(NULL, NULL, NULL),
};

static struct emac_platform_data dm644x_emac_pdata = {
	.ctrl_reg_offset	= DM644X_EMAC_CNTRL_OFFSET,
	.ctrl_mod_reg_offset	= DM644X_EMAC_CNTRL_MOD_OFFSET,
	.ctrl_ram_offset	= DM644X_EMAC_CNTRL_RAM_OFFSET,
	.ctrl_ram_size		= DM644X_EMAC_CNTRL_RAM_SIZE,
	.version		= EMAC_VERSION_1,
};

static struct resource dm644x_emac_resources[] = {
	{
		.start	= DM644X_EMAC_BASE,
		.end	= DM644X_EMAC_BASE + SZ_16K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.start = IRQ_EMACINT,
		.end   = IRQ_EMACINT,
		.flags = IORESOURCE_IRQ,
	},
};

static struct platform_device dm644x_emac_device = {
       .name		= "davinci_emac",
       .id		= 1,
       .dev = {
	       .platform_data	= &dm644x_emac_pdata,
       },
       .num_resources	= ARRAY_SIZE(dm644x_emac_resources),
       .resource	= dm644x_emac_resources,
};

static struct resource dm644x_mdio_resources[] = {
	{
		.start	= DM644X_EMAC_MDIO_BASE,
		.end	= DM644X_EMAC_MDIO_BASE + SZ_4K - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device dm644x_mdio_device = {
	.name		= "davinci_mdio",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(dm644x_mdio_resources),
	.resource	= dm644x_mdio_resources,
};

/*
 * Device specific mux setup
 *
 *	soc	description	mux  mode   mode  mux	 dbg
 *				reg  offset mask  mode
 */
static const struct mux_config dm644x_pins[] = {
#ifdef CONFIG_DAVINCI_MUX
MUX_CFG(DM644X, HDIREN,		0,   16,    1,	  1,	 true)
MUX_CFG(DM644X, ATAEN,		0,   17,    1,	  1,	 true)
MUX_CFG(DM644X, ATAEN_DISABLE,	0,   17,    1,	  0,	 true)

MUX_CFG(DM644X, HPIEN_DISABLE,	0,   29,    1,	  0,	 true)

MUX_CFG(DM644X, AEAW,		0,   0,     31,	  31,	 true)
MUX_CFG(DM644X, AEAW0,		0,   0,     1,	  0,	 true)
MUX_CFG(DM644X, AEAW1,		0,   1,     1,	  0,	 true)
MUX_CFG(DM644X, AEAW2,		0,   2,     1,	  0,	 true)
MUX_CFG(DM644X, AEAW3,		0,   3,     1,	  0,	 true)
MUX_CFG(DM644X, AEAW4,		0,   4,     1,	  0,	 true)

MUX_CFG(DM644X, MSTK,		1,   9,     1,	  0,	 false)

MUX_CFG(DM644X, I2C,		1,   7,     1,	  1,	 false)

MUX_CFG(DM644X, MCBSP,		1,   10,    1,	  1,	 false)

MUX_CFG(DM644X, UART1,		1,   1,     1,	  1,	 true)
MUX_CFG(DM644X, UART2,		1,   2,     1,	  1,	 true)

MUX_CFG(DM644X, PWM0,		1,   4,     1,	  1,	 false)

MUX_CFG(DM644X, PWM1,		1,   5,     1,	  1,	 false)

MUX_CFG(DM644X, PWM2,		1,   6,     1,	  1,	 false)

MUX_CFG(DM644X, VLYNQEN,	0,   15,    1,	  1,	 false)
MUX_CFG(DM644X, VLSCREN,	0,   14,    1,	  1,	 false)
MUX_CFG(DM644X, VLYNQWD,	0,   12,    3,	  3,	 false)

MUX_CFG(DM644X, EMACEN,		0,   31,    1,	  1,	 true)

MUX_CFG(DM644X, GPIO3V,		0,   31,    1,	  0,	 true)

MUX_CFG(DM644X, GPIO0,		0,   24,    1,	  0,	 true)
MUX_CFG(DM644X, GPIO3,		0,   25,    1,	  0,	 false)
MUX_CFG(DM644X, GPIO43_44,	1,   7,     1,	  0,	 false)
MUX_CFG(DM644X, GPIO46_47,	0,   22,    1,	  0,	 true)

MUX_CFG(DM644X, RGB666,		0,   22,    1,	  1,	 true)

MUX_CFG(DM644X, LOEEN,		0,   24,    1,	  1,	 true)
MUX_CFG(DM644X, LFLDEN,		0,   25,    1,	  1,	 false)
#endif
};

/* FIQ are pri 0-1; otherwise 2-7, with 7 lowest priority */
static u8 dm644x_default_priorities[DAVINCI_N_AINTC_IRQ] = {
	[IRQ_VDINT0]		= 2,
	[IRQ_VDINT1]		= 6,
	[IRQ_VDINT2]		= 6,
	[IRQ_HISTINT]		= 6,
	[IRQ_H3AINT]		= 6,
	[IRQ_PRVUINT]		= 6,
	[IRQ_RSZINT]		= 6,
	[7]			= 7,
	[IRQ_VENCINT]		= 6,
	[IRQ_ASQINT]		= 6,
	[IRQ_IMXINT]		= 6,
	[IRQ_VLCDINT]		= 6,
	[IRQ_USBINT]		= 4,
	[IRQ_EMACINT]		= 4,
	[14]			= 7,
	[15]			= 7,
	[IRQ_CCINT0]		= 5,	/* dma */
	[IRQ_CCERRINT]		= 5,	/* dma */
	[IRQ_TCERRINT0]		= 5,	/* dma */
	[IRQ_TCERRINT]		= 5,	/* dma */
	[IRQ_PSCIN]		= 7,
	[21]			= 7,
	[IRQ_IDE]		= 4,
	[23]			= 7,
	[IRQ_MBXINT]		= 7,
	[IRQ_MBRINT]		= 7,
	[IRQ_MMCINT]		= 7,
	[IRQ_SDIOINT]		= 7,
	[28]			= 7,
	[IRQ_DDRINT]		= 7,
	[IRQ_AEMIFINT]		= 7,
	[IRQ_VLQINT]		= 4,
	[IRQ_TINT0_TINT12]	= 2,	/* clockevent */
	[IRQ_TINT0_TINT34]	= 2,	/* clocksource */
	[IRQ_TINT1_TINT12]	= 7,	/* DSP timer */
	[IRQ_TINT1_TINT34]	= 7,	/* system tick */
	[IRQ_PWMINT0]		= 7,
	[IRQ_PWMINT1]		= 7,
	[IRQ_PWMINT2]		= 7,
	[IRQ_I2C]		= 3,
	[IRQ_UARTINT0]		= 3,
	[IRQ_UARTINT1]		= 3,
	[IRQ_UARTINT2]		= 3,
	[IRQ_SPINT0]		= 3,
	[IRQ_SPINT1]		= 3,
	[45]			= 7,
	[IRQ_DSP2ARM0]		= 4,
	[IRQ_DSP2ARM1]		= 4,
	[IRQ_GPIO0]		= 7,
	[IRQ_GPIO1]		= 7,
	[IRQ_GPIO2]		= 7,
	[IRQ_GPIO3]		= 7,
	[IRQ_GPIO4]		= 7,
	[IRQ_GPIO5]		= 7,
	[IRQ_GPIO6]		= 7,
	[IRQ_GPIO7]		= 7,
	[IRQ_GPIOBNK0]		= 7,
	[IRQ_GPIOBNK1]		= 7,
	[IRQ_GPIOBNK2]		= 7,
	[IRQ_GPIOBNK3]		= 7,
	[IRQ_GPIOBNK4]		= 7,
	[IRQ_COMMTX]		= 7,
	[IRQ_COMMRX]		= 7,
	[IRQ_EMUINT]		= 7,
};

/*----------------------------------------------------------------------*/

static const s8
queue_tc_mapping[][2] = {
	/* {event queue no, TC no} */
	{0, 0},
	{1, 1},
	{-1, -1},
};

static const s8
queue_priority_mapping[][2] = {
	/* {event queue no, Priority} */
	{0, 3},
	{1, 7},
	{-1, -1},
};

static struct edma_soc_info edma_cc0_info = {
	.n_channel		= 64,
	.n_region		= 4,
	.n_slot			= 128,
	.n_tc			= 2,
	.n_cc			= 1,
	.queue_tc_mapping	= queue_tc_mapping,
	.queue_priority_mapping	= queue_priority_mapping,
	.default_queue		= EVENTQ_1,
};

static struct edma_soc_info *dm644x_edma_info[EDMA_MAX_CC] = {
	&edma_cc0_info,
};

static struct resource edma_resources[] = {
	{
		.name	= "edma_cc0",
		.start	= 0x01c00000,
		.end	= 0x01c00000 + SZ_64K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "edma_tc0",
		.start	= 0x01c10000,
		.end	= 0x01c10000 + SZ_1K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "edma_tc1",
		.start	= 0x01c10400,
		.end	= 0x01c10400 + SZ_1K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "edma0",
		.start	= IRQ_CCINT0,
		.flags	= IORESOURCE_IRQ,
	},
	{
		.name	= "edma0_err",
		.start	= IRQ_CCERRINT,
		.flags	= IORESOURCE_IRQ,
	},
	/* not using TC*_ERR */
};

static struct platform_device dm644x_edma_device = {
	.name			= "edma",
	.id			= 0,
	.dev.platform_data	= dm644x_edma_info,
	.num_resources		= ARRAY_SIZE(edma_resources),
	.resource		= edma_resources,
};

/* DM6446 EVM uses ASP0; line-out is a pair of RCA jacks */
static struct resource dm644x_asp_resources[] = {
	{
		.start	= DAVINCI_ASP0_BASE,
		.end	= DAVINCI_ASP0_BASE + SZ_8K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.start	= DAVINCI_DMA_ASP0_TX,
		.end	= DAVINCI_DMA_ASP0_TX,
		.flags	= IORESOURCE_DMA,
	},
	{
		.start	= DAVINCI_DMA_ASP0_RX,
		.end	= DAVINCI_DMA_ASP0_RX,
		.flags	= IORESOURCE_DMA,
	},
};

static struct platform_device dm644x_asp_device = {
	.name		= "davinci-mcbsp",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm644x_asp_resources),
	.resource	= dm644x_asp_resources,
};

#define DM644X_VPSS_BASE	0x01c73400

static struct resource dm644x_vpss_resources[] = {
	{
		/* VPSS Base address */
		.name		= "vpss",
		.start		= DM644X_VPSS_BASE,
		.end		= DM644X_VPSS_BASE + 0xff,
		.flags		= IORESOURCE_MEM,
	},
};

static struct platform_device dm644x_vpss_device = {
	.name			= "vpss",
	.id			= -1,
	.dev.platform_data	= "dm644x_vpss",
	.num_resources		= ARRAY_SIZE(dm644x_vpss_resources),
	.resource		= dm644x_vpss_resources,
};

static struct resource dm644x_vpfe_resources[] = {
	{
		.start          = IRQ_VDINT0,
		.end            = IRQ_VDINT0,
		.flags          = IORESOURCE_IRQ,
	},
	{
		.start          = IRQ_VDINT1,
		.end            = IRQ_VDINT1,
		.flags          = IORESOURCE_IRQ,
	},
};

static u64 dm644x_video_dma_mask = DMA_BIT_MASK(32);
static struct resource dm644x_ccdc_resource[] = {
	/* CCDC Base address */
	{
		.start          = 0x01c70400,
		.end            = 0x01c70400 + 0xff,
		.flags          = IORESOURCE_MEM,
	},
};

static struct platform_device dm644x_ccdc_dev = {
	.name           = "dm644x_ccdc",
	.id             = -1,
	.num_resources  = ARRAY_SIZE(dm644x_ccdc_resource),
	.resource       = dm644x_ccdc_resource,
	.dev = {
		.dma_mask               = &dm644x_video_dma_mask,
		.coherent_dma_mask      = DMA_BIT_MASK(32),
	},
};

static struct platform_device dm644x_vpfe_dev = {
	.name		= CAPTURE_DRV_NAME,
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm644x_vpfe_resources),
	.resource	= dm644x_vpfe_resources,
	.dev = {
		.dma_mask		= &dm644x_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
	},
};

#define DM644X_OSD_BASE		0x01c72600

static struct resource dm644x_osd_resources[] = {
	{
		.start	= DM644X_OSD_BASE,
		.end	= DM644X_OSD_BASE + 0x1ff,
		.flags	= IORESOURCE_MEM,
	},
};

static struct osd_platform_data dm644x_osd_data = {
	.vpbe_type     = VPBE_VERSION_1,
};

static struct platform_device dm644x_osd_dev = {
	.name		= VPBE_OSD_SUBDEV_NAME,
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm644x_osd_resources),
	.resource	= dm644x_osd_resources,
	.dev		= {
		.dma_mask		= &dm644x_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
		.platform_data		= &dm644x_osd_data,
	},
};

#define DM644X_VENC_BASE		0x01c72400

static struct resource dm644x_venc_resources[] = {
	{
		.start	= DM644X_VENC_BASE,
		.end	= DM644X_VENC_BASE + 0x17f,
		.flags	= IORESOURCE_MEM,
	},
};

#define DM644X_VPSS_MUXSEL_PLL2_MODE          BIT(0)
#define DM644X_VPSS_MUXSEL_VPBECLK_MODE       BIT(1)
#define DM644X_VPSS_VENCLKEN                  BIT(3)
#define DM644X_VPSS_DACCLKEN                  BIT(4)

static int dm644x_venc_setup_clock(enum vpbe_enc_timings_type type,
				   unsigned int pclock)
{
	int ret = 0;
	u32 v = DM644X_VPSS_VENCLKEN;

	switch (type) {
	case VPBE_ENC_STD:
		v |= DM644X_VPSS_DACCLKEN;
		writel(v, DAVINCI_SYSMOD_VIRT(SYSMOD_VPSS_CLKCTL));
		break;
	case VPBE_ENC_CUSTOM_TIMINGS:
		if (pclock <= 27000000) {
			v |= DM644X_VPSS_DACCLKEN;
			writel(v, DAVINCI_SYSMOD_VIRT(SYSMOD_VPSS_CLKCTL));
		} else {
			/*
			 * For HD, use external clock source since
			 * HD requires higher clock rate
			 */
			v |= DM644X_VPSS_MUXSEL_VPBECLK_MODE;
			writel(v, DAVINCI_SYSMOD_VIRT(SYSMOD_VPSS_CLKCTL));
		}
		break;
	default:
		ret  = -EINVAL;
	}

	return ret;
}

static struct resource dm644x_v4l2_disp_resources[] = {
	{
		.start	= IRQ_VENCINT,
		.end	= IRQ_VENCINT,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device dm644x_vpbe_display = {
	.name		= "vpbe-v4l2",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm644x_v4l2_disp_resources),
	.resource	= dm644x_v4l2_disp_resources,
	.dev		= {
		.dma_mask		= &dm644x_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
	},
};

static struct venc_platform_data dm644x_venc_pdata = {
	.venc_type	= VPBE_VERSION_1,
	.setup_clock	= dm644x_venc_setup_clock,
};

static struct platform_device dm644x_venc_dev = {
	.name		= VPBE_VENC_SUBDEV_NAME,
	.id		= -1,
	.num_resources	= ARRAY_SIZE(dm644x_venc_resources),
	.resource	= dm644x_venc_resources,
	.dev		= {
		.dma_mask		= &dm644x_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
		.platform_data		= &dm644x_venc_pdata,
	},
};

static struct platform_device dm644x_vpbe_dev = {
	.name		= "vpbe_controller",
	.id		= -1,
	.dev		= {
		.dma_mask		= &dm644x_video_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
	},
};

/*----------------------------------------------------------------------*/

static struct map_desc dm644x_io_desc[] = {
	{
		.virtual	= IO_VIRT,
		.pfn		= __phys_to_pfn(IO_PHYS),
		.length		= IO_SIZE,
		.type		= MT_DEVICE
	},
};

/* Contents of JTAG ID register used to identify exact cpu type */
static struct davinci_id dm644x_ids[] = {
	{
		.variant	= 0x0,
		.part_no	= 0xb700,
		.manufacturer	= 0x017,
		.cpu_id		= DAVINCI_CPU_ID_DM6446,
		.name		= "dm6446",
	},
	{
		.variant	= 0x1,
		.part_no	= 0xb700,
		.manufacturer	= 0x017,
		.cpu_id		= DAVINCI_CPU_ID_DM6446,
		.name		= "dm6446a",
	},
};

static u32 dm644x_psc_bases[] = { DAVINCI_PWR_SLEEP_CNTRL_BASE };

/*
 * T0_BOT: Timer 0, bottom:  clockevent source for hrtimers
 * T0_TOP: Timer 0, top   :  clocksource for generic timekeeping
 * T1_BOT: Timer 1, bottom:  (used by DSP in TI DSPLink code)
 * T1_TOP: Timer 1, top   :  <unused>
 */
static struct davinci_timer_info dm644x_timer_info = {
	.timers		= davinci_timer_instance,
	.clockevent_id	= T0_BOT,
	.clocksource_id	= T0_TOP,
};

static struct plat_serial8250_port dm644x_serial_platform_data[] = {
	{
		.mapbase	= DAVINCI_UART0_BASE,
		.irq		= IRQ_UARTINT0,
		.flags		= UPF_BOOT_AUTOCONF | UPF_SKIP_TEST |
				  UPF_IOREMAP,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
	},
	{
		.mapbase	= DAVINCI_UART1_BASE,
		.irq		= IRQ_UARTINT1,
		.flags		= UPF_BOOT_AUTOCONF | UPF_SKIP_TEST |
				  UPF_IOREMAP,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
	},
	{
		.mapbase	= DAVINCI_UART2_BASE,
		.irq		= IRQ_UARTINT2,
		.flags		= UPF_BOOT_AUTOCONF | UPF_SKIP_TEST |
				  UPF_IOREMAP,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
	},
	{
		.flags		= 0
	},
};

static struct platform_device dm644x_serial_device = {
	.name			= "serial8250",
	.id			= PLAT8250_DEV_PLATFORM,
	.dev			= {
		.platform_data	= dm644x_serial_platform_data,
	},
};

struct clk_lookup vpss_master_lookups[] = {
	{ .dev_id = "dm644x_ccdc", .con_id = "master", },
};

struct clk_lookup vpss_slave_lookups[] = {
	{ .dev_id = "dm644x_ccdc", .con_id = "slave", },
};

static struct davinci_dev_lookup dev_clk_lookups[] = {
	{
		.con_id		= "vpss_master",
		.num_devs	= ARRAY_SIZE(vpss_master_lookups),
		.lookups	= vpss_master_lookups,
	},
	{
		.con_id		= "vpss_slave",
		.num_devs	= ARRAY_SIZE(vpss_slave_lookups),
		.lookups	= vpss_slave_lookups,
	},
	{
		.con_id		= NULL,
	},
};

static struct davinci_soc_info davinci_soc_info_dm644x = {
	.io_desc		= dm644x_io_desc,
	.io_desc_num		= ARRAY_SIZE(dm644x_io_desc),
	.jtag_id_reg		= 0x01c40028,
	.ids			= dm644x_ids,
	.ids_num		= ARRAY_SIZE(dm644x_ids),
	.cpu_clks		= dm644x_clks,
	.dev_clk_lookups	= dev_clk_lookups,
	.psc_bases		= dm644x_psc_bases,
	.psc_bases_num		= ARRAY_SIZE(dm644x_psc_bases),
	.pinmux_base		= DAVINCI_SYSTEM_MODULE_BASE,
	.pinmux_pins		= dm644x_pins,
	.pinmux_pins_num	= ARRAY_SIZE(dm644x_pins),
	.intc_base		= DAVINCI_ARM_INTC_BASE,
	.intc_type		= DAVINCI_INTC_TYPE_AINTC,
	.intc_irq_prios 	= dm644x_default_priorities,
	.intc_irq_num		= DAVINCI_N_AINTC_IRQ,
	.timer_info		= &dm644x_timer_info,
	.gpio_type		= GPIO_TYPE_DAVINCI,
	.gpio_base		= DAVINCI_GPIO_BASE,
	.gpio_num		= 71,
	.gpio_irq		= IRQ_GPIOBNK0,
	.serial_dev		= &dm644x_serial_device,
	.emac_pdata		= &dm644x_emac_pdata,
	.sram_dma		= 0x00008000,
	.sram_len		= SZ_16K,
};

void __init dm644x_init_asp(struct snd_platform_data *pdata)
{
	davinci_cfg_reg(DM644X_MCBSP);
	dm644x_asp_device.dev.platform_data = pdata;
	platform_device_register(&dm644x_asp_device);
}

void __init dm644x_init(void)
{
	davinci_common_init(&davinci_soc_info_dm644x);
	davinci_map_sysmod();
}

int __init dm644x_init_video(struct vpfe_config *vpfe_cfg,
				struct vpbe_config *vpbe_cfg)
{
	if (vpfe_cfg || vpbe_cfg)
		platform_device_register(&dm644x_vpss_device);

	if (vpfe_cfg) {
		dm644x_vpfe_dev.dev.platform_data = vpfe_cfg;
		platform_device_register(&dm644x_ccdc_dev);
		platform_device_register(&dm644x_vpfe_dev);
	}

	if (vpbe_cfg) {
		dm644x_vpbe_dev.dev.platform_data = vpbe_cfg;
		platform_device_register(&dm644x_osd_dev);
		platform_device_register(&dm644x_venc_dev);
		platform_device_register(&dm644x_vpbe_dev);
		platform_device_register(&dm644x_vpbe_display);
	}

	return 0;
}

static int __init dm644x_init_devices(void)
{
	if (!cpu_is_davinci_dm644x())
		return 0;

	platform_device_register(&dm644x_edma_device);

	platform_device_register(&dm644x_mdio_device);
	platform_device_register(&dm644x_emac_device);
	clk_add_alias(NULL, dev_name(&dm644x_mdio_device.dev),
		      NULL, &dm644x_emac_device.dev);

	return 0;
}
postcore_initcall(dm644x_init_devices);
