/*
 * Copyright (C) 2012 Texas Instruments Incorporated
 * Author: Cyril Chemparathy <cyril@ti.com>
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

#ifndef _LINUX_REMOTEPROC_USER_H
#define _LINUX_REMOTEPROC_USER_H

#include <linux/types.h>

enum rproc_user_state {
	RPROC_USER_OFFLINE,
	RPROC_USER_RUNNING,
};

#define RPROC_USER_IOC_MAGIC	 'I'

#define RPROC_USER_IOC_SET_RSC	 _IOW(RPROC_USER_IOC_MAGIC, 0,	\
				      void *)
#define RPROC_USER_IOC_SET_STATE _IOW(RPROC_USER_IOC_MAGIC, 1,	\
				      enum rproc_user_state)
#define RPROC_USER_IOC_SET_VRING_ADDR _IOW(RPROC_USER_IOC_MAGIC, 2,	\
				      unsigned int)

#endif /* _LINUX_REMOTEPROC_USER_H */
