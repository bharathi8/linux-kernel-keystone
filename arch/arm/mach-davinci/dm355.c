/*
 * TI DaVinci DM355 chip specific setup
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
#include <linux/dma-mapping.h>
#include <linux/spi/spi.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/platform_data/clk-davinci-pll.h>
#include <linux/platform_data/clk-davinci-psc.h>
#include <linux/platform_data/davinci-clock.h>
#include <mach/pll.h>

#include <asm/mach/map.h>

#include <mach/cputype.h>
#include <mach/edma.h>
#include <mach/psc.h>
#include <mach/mux.h>
#include <mach/irqs.h>
#include <mach/time.h>
#include <mach/serial.h>
#include <mach/common.h>
#include <linux/platform_data/spi-davinci.h>
#include <mach/gpio-davinci.h>

#include "davinci.h"
#include "mux.h"
#include "asp.h"

#define PLLM		0x110
#define PREDIV          0x114
#define POSTDIV         0x128
#define PLLM_PLLM_MASK  0xff

#define DM355_UART2_BASE	(IO_PHYS + 0x206000)
/*
 * Device specific clocks
 */
#define DM355_REF_FREQ		24000000	/* 24 or 36 MHz */

static struct clk_davinci_pll_data pll1_data = {
	.phy_pllm	= DAVINCI_PLL1_BASE + PLLM,
	.phy_prediv	= DAVINCI_PLL1_BASE + PREDIV,
	.phy_postdiv	= DAVINCI_PLL1_BASE + POSTDIV,
	.pllm_mask	= PLLM_PLLM_MASK,
	.prediv_mask	= PLLDIV_RATIO_MASK,
	.postdiv_mask	= PLLDIV_RATIO_MASK,
	.num		= 1,
	.pll_flags	= CLK_DAVINCI_PLL_HAS_PREDIV |
				 CLK_DAVINCI_PLL_HAS_POSTDIV,
	.fixed_prediv	= 8,
};

static struct clk_fixed_rate_data clkin_data = {
	/* FIXME -- crystal rate is board-specific */
	.rate		= DM355_REF_FREQ,
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
	/* FIXME -- crystal rate is board-specific */
	.rate		= DM355_REF_FREQ,
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
	}

define_pll1_div_clk(pll1_plldiv_clk_mux, 1, pll1_sysclk1);
define_pll1_div_clk(pll1_plldiv_clk_mux, 2, pll1_sysclk2);
define_pll1_div_clk(pll1_plldiv_clk_mux, 3, pll1_sysclk3);
define_pll1_div_clk(pll1_plldiv_clk_mux, 4, pll1_sysclk4);

static struct clk_fixed_factor_data fixed_clk_data = {
	.mult		= 1,
	.div		= 1,
};

static struct davinci_clk pll1_aux_clk = {
	.name			= "pll1_aux_clk",
	.parent			= &ref_clk_mux,
	.type			= DAVINCI_FIXED_FACTOR_CLK,
	.clk_data		= {
		.fixed_factor	=  &fixed_clk_data,
	},
};

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
	}

#define lpsc_clk_enabled(cname, parent, mod)		\
	__lpsc_clk(cname, parent, mod, 0, ALWAYS_ENABLED, ARMDOMAIN)

#define lpsc_clk(cname, flgs, parent, mod, dom)		\
	__lpsc_clk(cname, parent, mod, flgs, 0, dom)

#define __dm355_lpsc_clk(cname, _parent, mod, flgs, _flgs, dom)	\
	static struct clk_davinci_psc_data clk_psc_data##cname = {	\
		.domain	= DAVINCI_GPSC_##dom,			\
		.lpsc	= DM355_LPSC_##mod,			\
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
	}

#define dm355_lpsc_clk(cname, flgs, parent, mod, dom)		\
	__dm355_lpsc_clk(cname, parent, mod, flgs, 0, dom)

dm355_lpsc_clk(vpss_dac, 0, pll1_sysclk3, VPSS_DAC, ARMDOMAIN);
lpsc_clk(vpss_master, 0, pll1_sysclk4, VPSSMSTR, ARMDOMAIN);
lpsc_clk(vpss_slave, 0, pll1_sysclk4, VPSSSLV, ARMDOMAIN);
lpsc_clk_enabled(arm, pll1_sysclk1, ARM);
lpsc_clk(mjcp, 0, pll1_sysclk1, IMCOP, ARMDOMAIN);
lpsc_clk(uart0, 0, ref_clk_mux, UART0, ARMDOMAIN);
lpsc_clk(uart1, 0, ref_clk_mux, UART1, ARMDOMAIN);
lpsc_clk(uart2, 0, pll1_sysclk2, UART2, ARMDOMAIN);
lpsc_clk(i2c, 0, ref_clk_mux, I2C, ARMDOMAIN);
lpsc_clk(asp0, 0, pll1_sysclk2, McBSP, ARMDOMAIN);
dm355_lpsc_clk(asp1, 0, pll1_sysclk2, McBSP1, ARMDOMAIN);
lpsc_clk(mmcsd0, 0, pll1_sysclk2, MMC_SD, ARMDOMAIN);
dm355_lpsc_clk(mmcsd1, 0, pll1_sysclk2, MMC_SD1, ARMDOMAIN);
lpsc_clk(spi0, 0, pll1_sysclk2, SPI, ARMDOMAIN);
dm355_lpsc_clk(spi1, 0, pll1_sysclk2, SPI1, ARMDOMAIN);
dm355_lpsc_clk(spi2, 0, pll1_sysclk2, SPI2, ARMDOMAIN);
lpsc_clk(gpio, 0, pll1_sysclk2, GPIO, ARMDOMAIN);
lpsc_clk(aemif, 0, pll1_sysclk2, AEMIF, ARMDOMAIN);
lpsc_clk(pwm0, 0, ref_clk_mux, PWM0, ARMDOMAIN);
lpsc_clk(pwm1, 0, ref_clk_mux, PWM1, ARMDOMAIN);
lpsc_clk(pwm2, 0, ref_clk_mux, PWM2, ARMDOMAIN);
dm355_lpsc_clk(pwm3, 0, ref_clk_mux, PWM3, ARMDOMAIN);
lpsc_clk(timer0, 0, ref_clk_mux, TIMER0, ARMDOMAIN);
lpsc_clk(timer1, 0, ref_clk_mux, TIMER1, ARMDOMAIN);
/* REVISIT: why can't this be disabled? */
lpsc_clk(timer2, CLK_IGNORE_UNUSED, ref_clk_mux, TIMER2, ARMDOMAIN);
dm355_lpsc_clk(timer3, 0, ref_clk_mux, TIMER3, ARMDOMAIN);
dm355_lpsc_clk(rto, 0, ref_clk_mux, RTO, ARMDOMAIN);
lpsc_clk(usb, 0, pll1_sysclk2, USB, ARMDOMAIN);

static struct clk_davinci_pll_data pll2_data = {
	.phy_pllm	= DAVINCI_PLL2_BASE + PLLM,
	.phy_prediv	= DAVINCI_PLL2_BASE + PREDIV,
	.phy_postdiv	= DAVINCI_PLL2_BASE + POSTDIV,
	.pllm_mask	= PLLM_PLLM_MASK,
	.prediv_mask	= PLLDIV_RATIO_MASK,
	.postdiv_mask	= PLLDIV_RATIO_MASK,
	.num = 2,
	.pll_flags	= CLK_DAVINCI_PLL_HAS_PREDIV,
};

static struct davinci_clk pll2_clk = {
	.name		= "pll2",
	.parent		= &ref_clk_mux,
	.type		= DAVINCI_MAIN_PLL_CLK,
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
	}

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

static struct davinci_clk_lookup dm355_clks[] = {
	CLK(NULL, "clkin", &ref_clk_clkin),
	CLK(NULL, "oscin", &ref_clk_oscin),
	CLK(NULL, "ref_clk_mux", &ref_clk_mux),
	CLK(NULL, "pll1", &pll1_clk),
	CLK(NULL, "pll1_plldiv_clk_mux", &pll1_plldiv_clk_mux),
	CLK(NULL, "pll1_sysclk1", &pll1_sysclk1),
	CLK(NULL, "pll1_sysclk2", &pll1_sysclk2),
	CLK(NULL, "pll1_sysclk3", &pll1_sysclk3),
	CLK(NULL, "pll1_sysclk4", &pll1_sysclk4),
	CLK(NULL, "pll1_aux", &pll1_aux_clk),
	CLK(NULL, "pll1_sysclkbp", &pll1_sysclkbp),
	CLK(NULL, "vpss_dac", &clk_vpss_dac),
	CLK(NULL, "vpss_master", &clk_vpss_master),
	CLK(NULL, "vpss_slave", &clk_vpss_slave),
	CLK(NULL, "pll2", &pll2_clk),
	CLK(NULL, "pll2_plldiv_clk_mux", &pll2_plldiv_clk_mux),
	CLK(NULL, "pll2_sysclk1", &pll2_sysclk1),
	CLK(NULL, "pll2_sysclkbp", &pll2_sysclkbp),
	CLK(NULL, "arm", &clk_arm),
	CLK(NULL, "mjcp", &clk_mjcp),
	CLK(NULL, "uart0", &clk_uart0),
	CLK(NULL, "uart1", &clk_uart1),
	CLK(NULL, "uart2", &clk_uart2),
	CLK("i2c_davinci.1", NULL, &clk_i2c),
	CLK("davinci-mcbsp.0", NULL, &clk_asp0),
	CLK("davinci-mcbsp.1", NULL, &clk_asp1),
	CLK("davinci_mmc.0", NULL, &clk_mmcsd0),
	CLK("davinci_mmc.1", NULL, &clk_mmcsd1),
	CLK("spi_davinci.0", NULL, &clk_spi0),
	CLK("spi_davinci.1", NULL, &clk_spi1),
	CLK("spi_davinci.2", NULL, &clk_spi2),
	CLK(NULL, "gpio", &clk_gpio),
	CLK(NULL, "aemif", &clk_aemif),
	CLK(NULL, "pwm0", &clk_pwm0),
	CLK(NULL, "pwm1", &clk_pwm1),
	CLK(NULL, "pwm2", &clk_pwm2),
	CLK(NULL, "pwm3", &clk_pwm3),
	CLK(NULL, "timer0", &clk_timer0),
	CLK(NULL, "timer1", &clk_timer1),
	CLK("watchdog", NULL, &clk_timer2),
	CLK(NULL, "timer3", &clk_timer3),
	CLK(NULL, "rto", &clk_rto),
	CLK(NULL, "usb", &clk_usb),
	CLK(NULL, NULL, NULL),
};

/*----------------------------------------------------------------------*/

static u64 dm355_spi0_dma_mask = DMA_BIT_MASK(32);

static struct resource dm355_spi0_resources[] = {
	{
		.start = 0x01c66000,
		.end   = 0x01c667ff,
		.flags = IORESOURCE_MEM,
	},
	{
		.start = IRQ_DM355_SPINT0_0,
		.flags = IORESOURCE_IRQ,
	},
	{
		.start = 17,
		.flags = IORESOURCE_DMA,
	},
	{
		.start = 16,
		.flags = IORESOURCE_DMA,
	},
};

static struct davinci_spi_platform_data dm355_spi0_pdata = {
	.version 	= SPI_VERSION_1,
	.num_chipselect = 2,
	.cshold_bug	= true,
	.dma_event_q	= EVENTQ_1,
};
static struct platform_device dm355_spi0_device = {
	.name = "spi_davinci",
	.id = 0,
	.dev = {
		.dma_mask = &dm355_spi0_dma_mask,
		.coherent_dma_mask = DMA_BIT_MASK(32),
		.platform_data = &dm355_spi0_pdata,
	},
	.num_resources = ARRAY_SIZE(dm355_spi0_resources),
	.resource = dm355_spi0_resources,
};

void __init dm355_init_spi0(unsigned chipselect_mask,
		const struct spi_board_info *info, unsigned len)
{
	/* for now, assume we need MISO */
	davinci_cfg_reg(DM355_SPI0_SDI);

	/* not all slaves will be wired up */
	if (chipselect_mask & BIT(0))
		davinci_cfg_reg(DM355_SPI0_SDENA0);
	if (chipselect_mask & BIT(1))
		davinci_cfg_reg(DM355_SPI0_SDENA1);

	spi_register_board_info(info, len);

	platform_device_register(&dm355_spi0_device);
}

/*----------------------------------------------------------------------*/

#define INTMUX		0x18
#define EVTMUX		0x1c

/*
 * Device specific mux setup
 *
 *	soc	description	mux  mode   mode  mux	 dbg
 *				reg  offset mask  mode
 */
static const struct mux_config dm355_pins[] = {
#ifdef CONFIG_DAVINCI_MUX
MUX_CFG(DM355,	MMCSD0,		4,   2,     1,	  0,	 false)

MUX_CFG(DM355,	SD1_CLK,	3,   6,     1,	  1,	 false)
MUX_CFG(DM355,	SD1_CMD,	3,   7,     1,	  1,	 false)
MUX_CFG(DM355,	SD1_DATA3,	3,   8,     3,	  1,	 false)
MUX_CFG(DM355,	SD1_DATA2,	3,   10,    3,	  1,	 false)
MUX_CFG(DM355,	SD1_DATA1,	3,   12,    3,	  1,	 false)
MUX_CFG(DM355,	SD1_DATA0,	3,   14,    3,	  1,	 false)

MUX_CFG(DM355,	I2C_SDA,	3,   19,    1,	  1,	 false)
MUX_CFG(DM355,	I2C_SCL,	3,   20,    1,	  1,	 false)

MUX_CFG(DM355,	MCBSP0_BDX,	3,   0,     1,	  1,	 false)
MUX_CFG(DM355,	MCBSP0_X,	3,   1,     1,	  1,	 false)
MUX_CFG(DM355,	MCBSP0_BFSX,	3,   2,     1,	  1,	 false)
MUX_CFG(DM355,	MCBSP0_BDR,	3,   3,     1,	  1,	 false)
MUX_CFG(DM355,	MCBSP0_R,	3,   4,     1,	  1,	 false)
MUX_CFG(DM355,	MCBSP0_BFSR,	3,   5,     1,	  1,	 false)

MUX_CFG(DM355,	SPI0_SDI,	4,   1,     1,    0,	 false)
MUX_CFG(DM355,	SPI0_SDENA0,	4,   0,     1,    0,	 false)
MUX_CFG(DM355,	SPI0_SDENA1,	3,   28,    1,    1,	 false)

INT_CFG(DM355,  INT_EDMA_CC,	      2,    1,    1,     false)
INT_CFG(DM355,  INT_EDMA_TC0_ERR,     3,    1,    1,     false)
INT_CFG(DM355,  INT_EDMA_TC1_ERR,     4,    1,    1,     false)

EVT_CFG(DM355,  EVT8_ASP1_TX,	      0,    1,    0,     false)
EVT_CFG(DM355,  EVT9_ASP1_RX,	      1,    1,    0,     false)
EVT_CFG(DM355,  EVT26_MMC0_RX,	      2,    1,    0,     false)

MUX_CFG(DM355,	VOUT_FIELD,	1,   18,    3,	  1,	 false)
MUX_CFG(DM355,	VOUT_FIELD_G70,	1,   18,    3,	  0,	 false)
MUX_CFG(DM355,	VOUT_HVSYNC,	1,   16,    1,	  0,	 false)
MUX_CFG(DM355,	VOUT_COUTL_EN,	1,   0,     0xff, 0x55,  false)
MUX_CFG(DM355,	VOUT_COUTH_EN,	1,   8,     0xff, 0x55,  false)

MUX_CFG(DM355,	VIN_PCLK,	0,   14,    1,    1,	 false)
MUX_CFG(DM355,	VIN_CAM_WEN,	0,   13,    1,    1,	 false)
MUX_CFG(DM355,	VIN_CAM_VD,	0,   12,    1,    1,	 false)
MUX_CFG(DM355,	VIN_CAM_HD,	0,   11,    1,    1,	 false)
MUX_CFG(DM355,	VIN_YIN_EN,	0,   10,    1,    1,	 false)
MUX_CFG(DM355,	VIN_CINL_EN,	0,   0,   0xff, 0x55,	 false)
MUX_CFG(DM355,	VIN_CINH_EN,	0,   8,     3,    3,	 false)
#endif
};

static u8 dm355_default_priorities[DAVINCI_N_AINTC_IRQ] = {
	[IRQ_DM355_CCDC_VDINT0]		= 2,
	[IRQ_DM355_CCDC_VDINT1]		= 6,
	[IRQ_DM355_CCDC_VDINT2]		= 6,
	[IRQ_DM355_IPIPE_HST]		= 6,
	[IRQ_DM355_H3AINT]		= 6,
	[IRQ_DM355_IPIPE_SDR]		= 6,
	[IRQ_DM355_IPIPEIFINT]		= 6,
	[IRQ_DM355_OSDINT]		= 7,
	[IRQ_DM355_VENCINT]		= 6,
	[IRQ_ASQINT]			= 6,
	[IRQ_IMXINT]			= 6,
	[IRQ_USBINT]			= 4,
	[IRQ_DM355_RTOINT]		= 4,
	[IRQ_DM355_UARTINT2]		= 7,
	[IRQ_DM355_TINT6]		= 7,
	[IRQ_CCINT0]			= 5,	/* dma */
	[IRQ_CCERRINT]			= 5,	/* dma */
	[IRQ_TCERRINT0]			= 5,	/* dma */
	[IRQ_TCERRINT]			= 5,	/* dma */
	[IRQ_DM355_SPINT2_1]		= 7,
	[IRQ_DM355_TINT7]		= 4,
	[IRQ_DM355_SDIOINT0]		= 7,
	[IRQ_MBXINT]			= 7,
	[IRQ_MBRINT]			= 7,
	[IRQ_MMCINT]			= 7,
	[IRQ_DM355_MMCINT1]		= 7,
	[IRQ_DM355_PWMINT3]		= 7,
	[IRQ_DDRINT]			= 7,
	[IRQ_AEMIFINT]			= 7,
	[IRQ_DM355_SDIOINT1]		= 4,
	[IRQ_TINT0_TINT12]		= 2,	/* clockevent */
	[IRQ_TINT0_TINT34]		= 2,	/* clocksource */
	[IRQ_TINT1_TINT12]		= 7,	/* DSP timer */
	[IRQ_TINT1_TINT34]		= 7,	/* system tick */
	[IRQ_PWMINT0]			= 7,
	[IRQ_PWMINT1]			= 7,
	[IRQ_PWMINT2]			= 7,
	[IRQ_I2C]			= 3,
	[IRQ_UARTINT0]			= 3,
	[IRQ_UARTINT1]			= 3,
	[IRQ_DM355_SPINT0_0]		= 3,
	[IRQ_DM355_SPINT0_1]		= 3,
	[IRQ_DM355_GPIO0]		= 3,
	[IRQ_DM355_GPIO1]		= 7,
	[IRQ_DM355_GPIO2]		= 4,
	[IRQ_DM355_GPIO3]		= 4,
	[IRQ_DM355_GPIO4]		= 7,
	[IRQ_DM355_GPIO5]		= 7,
	[IRQ_DM355_GPIO6]		= 7,
	[IRQ_DM355_GPIO7]		= 7,
	[IRQ_DM355_GPIO8]		= 7,
	[IRQ_DM355_GPIO9]		= 7,
	[IRQ_DM355_GPIOBNK0]		= 7,
	[IRQ_DM355_GPIOBNK1]		= 7,
	[IRQ_DM355_GPIOBNK2]		= 7,
	[IRQ_DM355_GPIOBNK3]		= 7,
	[IRQ_DM355_GPIOBNK4]		= 7,
	[IRQ_DM355_GPIOBNK5]		= 7,
	[IRQ_DM355_GPIOBNK6]		= 7,
	[IRQ_COMMTX]			= 7,
	[IRQ_COMMRX]			= 7,
	[IRQ_EMUINT]			= 7,
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

static struct edma_soc_info *dm355_edma_info[EDMA_MAX_CC] = {
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
	/* not using (or muxing) TC*_ERR */
};

static struct platform_device dm355_edma_device = {
	.name			= "edma",
	.id			= 0,
	.dev.platform_data	= dm355_edma_info,
	.num_resources		= ARRAY_SIZE(edma_resources),
	.resource		= edma_resources,
};

static struct resource dm355_asp1_resources[] = {
	{
		.start	= DAVINCI_ASP1_BASE,
		.end	= DAVINCI_ASP1_BASE + SZ_8K - 1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.start	= DAVINCI_DMA_ASP1_TX,
		.end	= DAVINCI_DMA_ASP1_TX,
		.flags	= IORESOURCE_DMA,
	},
	{
		.start	= DAVINCI_DMA_ASP1_RX,
		.end	= DAVINCI_DMA_ASP1_RX,
		.flags	= IORESOURCE_DMA,
	},
};

static struct platform_device dm355_asp1_device = {
	.name		= "davinci-mcbsp",
	.id		= 1,
	.num_resources	= ARRAY_SIZE(dm355_asp1_resources),
	.resource	= dm355_asp1_resources,
};

static void dm355_ccdc_setup_pinmux(void)
{
	davinci_cfg_reg(DM355_VIN_PCLK);
	davinci_cfg_reg(DM355_VIN_CAM_WEN);
	davinci_cfg_reg(DM355_VIN_CAM_VD);
	davinci_cfg_reg(DM355_VIN_CAM_HD);
	davinci_cfg_reg(DM355_VIN_YIN_EN);
	davinci_cfg_reg(DM355_VIN_CINL_EN);
	davinci_cfg_reg(DM355_VIN_CINH_EN);
}

static struct resource dm355_vpss_resources[] = {
	{
		/* VPSS BL Base address */
		.name		= "vpss",
		.start          = 0x01c70800,
		.end            = 0x01c70800 + 0xff,
		.flags          = IORESOURCE_MEM,
	},
	{
		/* VPSS CLK Base address */
		.name		= "vpss",
		.start          = 0x01c70000,
		.end            = 0x01c70000 + 0xf,
		.flags          = IORESOURCE_MEM,
	},
};

static struct platform_device dm355_vpss_device = {
	.name			= "vpss",
	.id			= -1,
	.dev.platform_data	= "dm355_vpss",
	.num_resources		= ARRAY_SIZE(dm355_vpss_resources),
	.resource		= dm355_vpss_resources,
};

static struct resource vpfe_resources[] = {
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

static u64 vpfe_capture_dma_mask = DMA_BIT_MASK(32);
static struct resource dm355_ccdc_resource[] = {
	/* CCDC Base address */
	{
		.flags          = IORESOURCE_MEM,
		.start          = 0x01c70600,
		.end            = 0x01c70600 + 0x1ff,
	},
};
static struct platform_device dm355_ccdc_dev = {
	.name           = "dm355_ccdc",
	.id             = -1,
	.num_resources  = ARRAY_SIZE(dm355_ccdc_resource),
	.resource       = dm355_ccdc_resource,
	.dev = {
		.dma_mask               = &vpfe_capture_dma_mask,
		.coherent_dma_mask      = DMA_BIT_MASK(32),
		.platform_data		= dm355_ccdc_setup_pinmux,
	},
};

static struct platform_device vpfe_capture_dev = {
	.name		= CAPTURE_DRV_NAME,
	.id		= -1,
	.num_resources	= ARRAY_SIZE(vpfe_resources),
	.resource	= vpfe_resources,
	.dev = {
		.dma_mask		= &vpfe_capture_dma_mask,
		.coherent_dma_mask	= DMA_BIT_MASK(32),
	},
};

void dm355_set_vpfe_config(struct vpfe_config *cfg)
{
	vpfe_capture_dev.dev.platform_data = cfg;
}

/*----------------------------------------------------------------------*/

static struct map_desc dm355_io_desc[] = {
	{
		.virtual	= IO_VIRT,
		.pfn		= __phys_to_pfn(IO_PHYS),
		.length		= IO_SIZE,
		.type		= MT_DEVICE
	},
};

/* Contents of JTAG ID register used to identify exact cpu type */
static struct davinci_id dm355_ids[] = {
	{
		.variant	= 0x0,
		.part_no	= 0xb73b,
		.manufacturer	= 0x00f,
		.cpu_id		= DAVINCI_CPU_ID_DM355,
		.name		= "dm355",
	},
};

static u32 dm355_psc_bases[] = { DAVINCI_PWR_SLEEP_CNTRL_BASE };

/*
 * T0_BOT: Timer 0, bottom:  clockevent source for hrtimers
 * T0_TOP: Timer 0, top   :  clocksource for generic timekeeping
 * T1_BOT: Timer 1, bottom:  (used by DSP in TI DSPLink code)
 * T1_TOP: Timer 1, top   :  <unused>
 */
static struct davinci_timer_info dm355_timer_info = {
	.timers		= davinci_timer_instance,
	.clockevent_id	= T0_BOT,
	.clocksource_id	= T0_TOP,
};

static struct plat_serial8250_port dm355_serial_platform_data[] = {
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
		.mapbase	= DM355_UART2_BASE,
		.irq		= IRQ_DM355_UARTINT2,
		.flags		= UPF_BOOT_AUTOCONF | UPF_SKIP_TEST |
				  UPF_IOREMAP,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
	},
	{
		.flags		= 0
	},
};

static struct platform_device dm355_serial_device = {
	.name			= "serial8250",
	.id			= PLAT8250_DEV_PLATFORM,
	.dev			= {
		.platform_data	= dm355_serial_platform_data,
	},
};

static struct clk_lookup vpss_master_lookups[] = {
	{ .dev_id = "dm355_ccdc", .con_id = "master", },
};

static struct clk_lookup vpss_slave_lookups[] = {
	{ .dev_id = "dm355_ccdc", .con_id = "slave", },
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

static struct davinci_soc_info davinci_soc_info_dm355 = {
	.io_desc		= dm355_io_desc,
	.io_desc_num		= ARRAY_SIZE(dm355_io_desc),
	.jtag_id_reg		= 0x01c40028,
	.ids			= dm355_ids,
	.ids_num		= ARRAY_SIZE(dm355_ids),
	.cpu_clks		= dm355_clks,
	.dev_clk_lookups	= dev_clk_lookups,
	.psc_bases		= dm355_psc_bases,
	.psc_bases_num		= ARRAY_SIZE(dm355_psc_bases),
	.pinmux_base		= DAVINCI_SYSTEM_MODULE_BASE,
	.pinmux_pins		= dm355_pins,
	.pinmux_pins_num	= ARRAY_SIZE(dm355_pins),
	.intc_base		= DAVINCI_ARM_INTC_BASE,
	.intc_type		= DAVINCI_INTC_TYPE_AINTC,
	.intc_irq_prios		= dm355_default_priorities,
	.intc_irq_num		= DAVINCI_N_AINTC_IRQ,
	.timer_info		= &dm355_timer_info,
	.gpio_type		= GPIO_TYPE_DAVINCI,
	.gpio_base		= DAVINCI_GPIO_BASE,
	.gpio_num		= 104,
	.gpio_irq		= IRQ_DM355_GPIOBNK0,
	.serial_dev		= &dm355_serial_device,
	.sram_dma		= 0x00010000,
	.sram_len		= SZ_32K,
};

void __init dm355_init_asp1(u32 evt_enable, struct snd_platform_data *pdata)
{
	/* we don't use ASP1 IRQs, or we'd need to mux them ... */
	if (evt_enable & ASP1_TX_EVT_EN)
		davinci_cfg_reg(DM355_EVT8_ASP1_TX);

	if (evt_enable & ASP1_RX_EVT_EN)
		davinci_cfg_reg(DM355_EVT9_ASP1_RX);

	dm355_asp1_device.dev.platform_data = pdata;
	platform_device_register(&dm355_asp1_device);
}

void __init dm355_init(void)
{
	davinci_common_init(&davinci_soc_info_dm355);
	davinci_map_sysmod();
}

static int __init dm355_init_devices(void)
{
	if (!cpu_is_davinci_dm355())
		return 0;

	/* Add ccdc clock aliases */
#ifndef CONFIG_COMMON_CLK
	clk_add_alias("master", dm355_ccdc_dev.name, "vpss_master", NULL);
	clk_add_alias("slave", dm355_ccdc_dev.name, "vpss_master", NULL);
#endif
	davinci_cfg_reg(DM355_INT_EDMA_CC);
	platform_device_register(&dm355_edma_device);
	platform_device_register(&dm355_vpss_device);
	platform_device_register(&dm355_ccdc_dev);
	platform_device_register(&vpfe_capture_dev);

	return 0;
}
postcore_initcall(dm355_init_devices);
