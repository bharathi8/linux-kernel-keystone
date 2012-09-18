/*
 * TI DaVinci PLL definitions
 *
 * Copyright (C) 2006-2012 Texas Instruments.
 * Copyright (C) 2008-2009 Deep Root Systems, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef __ASM_ARCH_PLL_H
#define __ASM_ARCH_PLL_H

#define DAVINCI_PLL1_BASE 0x01c40800
#define DAVINCI_PLL2_BASE 0x01c40c00
#define MAX_PLL 2

/* PLL/Reset register offsets */
#define PLLCTL          0x100
#define PLLCTL_PLLEN    BIT(0)
#define PLLCTL_PLLPWRDN	BIT(1)
#define PLLCTL_PLLRST	BIT(3)
#define PLLCTL_PLLDIS	BIT(4)
#define PLLCTL_PLLENSRC	BIT(5)
#define PLLCTL_CLKMODE  BIT(8)

#define PLLCTL_CLKMODE_SHIFT	8
#define PLLCTL_CLKMODE_WIDTH	1
#define PLLCTL_PLLEN_SHIFT	0
#define PLLCTL_PLLEN_WIDTH	1

#define PLLDIV1         0x118
#define PLLDIV2         0x11c
#define PLLDIV3         0x120
#define OSCDIV1         0x124
#define BPDIV           0x12c
#define PLLCMD		0x138
#define PLLSTAT		0x13c
#define PLLALNCTL	0x140
#define PLLDCHANGE	0x144
#define PLLCKEN		0x148
#define PLLCKSTAT	0x14c
#define PLLSYSTAT	0x150
#define PLLDIV4         0x160
#define PLLDIV5         0x164
#define PLLDIV6         0x168
#define PLLDIV7         0x16c
#define PLLDIV8         0x170
#define PLLDIV9         0x174
#define PLLDIV10        0x178
#define PLLDIV11        0x17c
#define PLLDIV12        0x180
#define PLLDIV13        0x184
#define PLLDIV14        0x188
#define PLLDIV15        0x18c
#define PLLDIV16        0x190
#define PLLDIV_RATIO_MASK 0x1f
#define PLLDIV_EN       BIT(15)

/*
 * OMAP-L138 system reference guide recommends a wait for 4 OSCIN/CLKIN
 * cycles to ensure that the PLLC has switched to bypass mode. Delay of 1us
 * ensures we are good for all > 4MHz OSCIN/CLKIN inputs. Typically the input
 * is ~25MHz. Units are micro seconds.
 */
#define PLL_BYPASS_TIME		1
/* From OMAP-L138 datasheet table 6-4. Units are micro seconds */
#define PLL_RESET_TIME		1
/*
 * From OMAP-L138 datasheet table 6-4; assuming prediv = 1, sqrt(pllm) = 4
 * Units are micro seconds.
 */
#define PLL_LOCK_TIME		20
#define PLLSTAT_GOSTAT		BIT(0)
#define PLLCMD_GOSET		BIT(0)

#endif /* __ASM_ARCH_PLL_H */
