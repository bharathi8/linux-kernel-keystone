/**
 * io.h - DesignWare USB3 DRD IO Header
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
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

#ifndef __DRIVERS_USB_DWC3_IO_H
#define __DRIVERS_USB_DWC3_IO_H

#include <linux/io.h>

#include "core.h"

#define dwc3_readl(dwc, offset)			\
	__dwc3_readl(dwc, offset, #offset, __FILE__, __LINE__)
#define dwc3_writel(dwc, offset, value)		\
	__dwc3_writel(dwc, offset, value, #offset, __FILE__, __LINE__)

/*
 * We requested the mem region starting from the Globals address
 * space, see dwc3_probe in core.c.
 * However, the offsets are given starting from xHCI address space.
 */
#define reg_addr(dwc, offset)			\
	((dwc)->regs + ((offset) - DWC3_GLOBALS_REGS_START))

static inline u32 __dwc3_readl(struct dwc3 *dwc, u32 offset, const char *reg,
			       const char *file, int line)
{
	u32 value = readl(reg_addr(dwc, offset));

	dev_vdbg(dwc->dev, "[%s (%05x)] --> %08x @%s:%d\n",
		 reg, offset, value, file, line);
	return value;
}

static inline void __dwc3_writel(struct dwc3 *dwc, u32 offset, u32 value,
				 const char *reg, const char *file, int line)
{
	dev_vdbg(dwc->dev, "[%s (%05x)] <-- %08x @%s:%d\n",
		 reg, offset, value, file, line);
	writel(value, reg_addr(dwc, offset));
}

#endif /* __DRIVERS_USB_DWC3_IO_H */
