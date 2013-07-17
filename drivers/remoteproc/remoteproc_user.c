/*
 * User-space remoteproc loader interface
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com
 * Contact: Cyril Chemparathy <cyril@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/of_gpio.h>
#include <linux/remoteproc.h>
#include <linux/uio_driver.h>
#include <linux/remoteproc_user.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include <linux/io.h>

#include "remoteproc_internal.h"

#define DRIVER_NAME		"rproc-user"
#define DRIVER_VERSION		"0.1"
#define UPROC_MAX_NOTIFIES	8
#define UPROC_MAX_RSC_TABLE	SZ_64K


/**
 * struct uproc_addr_map - remote processor address map entry
 */
struct uproc_addr_map {
	phys_addr_t	addr;
	unsigned long	length;
	int		index;
	struct list_head	node;
};

/**
 * struct uproc_info - local information for remoteproc user
 */
struct uproc_info {
	struct uio_info		uio;
	struct rproc		*rproc;
	struct device		*dev;
	struct miscdevice	misc;
	struct clk		*clk;
	unsigned long		flags;
	spinlock_t		lock;
	int			irq_ctl, irq_ring;
	int			kick_gpio;
	struct resource_table	*rsc_table;
	int			rsc_table_size;
	unsigned int		vring_addr;
	int			start_offset;
	struct list_head	addr_map;
	struct work_struct	workqueue;
};

static int uproc_set_rsc_table(struct uproc_info *uproc,
			       void __user *data)
{
	unsigned long len = 0;
	void *rsc_table = NULL;

	if (data) {
		if (copy_from_user(&len, data, sizeof(len)))
			return -EFAULT;
		data += sizeof(len);

		if (len >= UPROC_MAX_RSC_TABLE)
			return -EOVERFLOW;

		rsc_table = kmalloc(len, GFP_KERNEL);
		if (!rsc_table)
			return -ENOMEM;

		if (copy_from_user(rsc_table, data, len))
			return -EFAULT;
	}

	spin_lock(&uproc->lock);

	if (uproc->rsc_table) {
		kfree(uproc->rsc_table);
		uproc->rsc_table_size = 0;
		uproc->rsc_table = NULL;
	}

	uproc->rsc_table = rsc_table;
	uproc->rsc_table_size = len;

	spin_unlock(&uproc->lock);

	return 0;
}

static int uproc_open(struct uio_info *uio, struct inode *inode)
{
	struct uproc_info *uproc = uio->priv;
	return clk_prepare_enable(uproc->clk);
}

static int uproc_release(struct uio_info *uio, struct inode *inode)
{
	struct uproc_info *uproc = uio->priv;
	struct rproc *rproc = uproc->rproc;

	if (rproc->state != RPROC_OFFLINE) {
		rproc_shutdown(rproc);
		WARN_ON(rproc->state != RPROC_OFFLINE);
	}

	clk_disable_unprepare(uproc->clk);
	return 0;
}

static irqreturn_t uproc_handler(int irq, struct uio_info *uio)
{
	struct uproc_info *uproc = uio->priv;

	if (!test_and_set_bit(0, &uproc->flags))
		disable_irq_nosync(irq);

	return IRQ_HANDLED;
}

static int uproc_irqcontrol(struct uio_info *uio, s32 irq_on)
{
	struct uproc_info *uproc = uio->priv;
	unsigned long flags;

	spin_lock_irqsave(&uproc->lock, flags);
	if (irq_on) {
		if (test_and_clear_bit(0, &uproc->flags))
			enable_irq(uio->irq);
	} else {
		if (!test_and_set_bit(0, &uproc->flags))
			disable_irq(uio->irq);
	}
	spin_unlock_irqrestore(&uproc->lock, flags);

	return 0;
}

static int uproc_set_state(struct uproc_info *uproc,
			   enum rproc_user_state state)
{
	struct rproc *rproc = uproc->rproc;
	int error = 0;

	if (state == RPROC_USER_RUNNING)
		error = rproc_boot(rproc);
	else if (state == RPROC_USER_OFFLINE)
		rproc_shutdown(rproc);
	else
		error = -ENOTSUPP;

	return error;
}

static int uproc_set_vring_addr(struct uproc_info *uproc,
			   unsigned int dma_addr)
{
	uproc->vring_addr = dma_addr;
	uproc->start_offset = 0;
	return 0;
}

/**
 * uproc_dev_fop_open() for the rproc-user driver
 */
static int uproc_dev_fop_open(struct inode *inode, struct file *file)
{
	/* Need an empty open so that file->private_data gets populated */
	return 0;
}

/**
 * uproc_dev_fop_mmap() - provided mmap support for
 * rproc memory. This checks if user request is in valid range before providing
 * mmap access. The valid range can be configured using device tree or platform
 * data.
 */
static int uproc_dev_fop_mmap(struct file *file, struct vm_area_struct *vma)
{
	bool is_allowed = false;
	struct uproc_addr_map *pmap;
	size_t size = vma->vm_end - vma->vm_start;
	phys_addr_t addr = vma->vm_pgoff << PAGE_SHIFT;
	struct miscdevice *misc = file->private_data;
	struct uproc_info *uproc =
		container_of(misc, struct uproc_info, misc);

	/* Check if the requested address and length is in the
	range configured duing probe */
	list_for_each_entry(pmap, &uproc->addr_map, node) {
		if (pmap->addr <= addr &&
			pmap->addr + pmap->length >= addr + size) {
				is_allowed = true;
				break;
		}
	}

	if (!is_allowed) {
		dev_err(uproc->dev,
			"mmap offset 0x%lx is outside the allowed range\n",
			(long unsigned int) addr);
		return -EINVAL;
	}

	if (vma->vm_start & ~PAGE_MASK) {
		dev_err(uproc->dev, "must mmap at page boundary\n");
		return -EINVAL;
	}

	vma->vm_page_prot = phys_mem_access_prot(file, vma->vm_pgoff, size,
						 vma->vm_page_prot);

	if (remap_pfn_range(vma,
			    vma->vm_start,
			    vma->vm_pgoff,
			    size,
			    vma->vm_page_prot)) {
		return -EAGAIN;
	}
	return 0;

}

static long uproc_ioctl(struct uio_info *uio, unsigned cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct uproc_info *uproc = uio->priv;

	switch (cmd) {
	case RPROC_USER_IOC_SET_RSC:
		return uproc_set_rsc_table(uproc, argp);
	case RPROC_USER_IOC_SET_STATE:
		return uproc_set_state(uproc, arg);
	case RPROC_USER_IOC_SET_VRING_ADDR:
		return uproc_set_vring_addr(uproc, arg);
	default:
		return -ENOTSUPP;
	}
}

static void uproc_kick(struct rproc *rproc, int vqid)
{
	struct uproc_info *uproc = rproc->priv;

	dev_dbg(uproc->dev, "kick vqid: %d\n", vqid);
	if (uproc->kick_gpio < 0)
		return;

	/* interrupt the remote core */
	gpio_set_value(uproc->kick_gpio, 1);
}

static void *uproc_alloc(struct device *dev, size_t size,
		dma_addr_t *dma_handle, gfp_t flag)
{
	struct uproc_info *uproc = dev_get_drvdata(dev);
	struct rproc *rproc = uproc->rproc;
	void *va;

	if((uproc->start_offset + 0x4000) > 0x8000)
		return(NULL);

	dev_dbg(uproc->dev, "\n uproc->start offset %x uproc->vring_addr %x",
		 uproc->start_offset, uproc->vring_addr);

	/* feed address set through IOCTL */
	*dma_handle = uproc->vring_addr + uproc->start_offset;
	uproc->start_offset += 0x4000;
        /* Convert to virtual address */
	va = devm_ioremap_nocache(&rproc->dev, *dma_handle, size);

	return(va);
}

static void uproc_free(struct device *dev, size_t size, void *cpu_addr,
		    dma_addr_t dma_handle)
{
	struct uproc_info *uproc = dev_get_drvdata(dev);
	struct rproc *rproc = uproc->rproc;

	devm_iounmap(&rproc->dev, cpu_addr);
}

/**
 * handle_event() - inbound virtqueue message workqueue function
 *
 * This funciton is registered with 'workqueue' and is scheduled by the
 * ISR handler.
 *
 * There is no "payload" message indicating the virtqueue index as is the
 * case with mailbox-based implementations on OMAP4.  As such, this
 * handler "polls" each known virtqueue index for every invocation.
 *
 * A payload could be added by using some of the source bits in the IPC
 * generation registers, but we would need to change the logic in
 * drivers/misc/keystone-ipc-int.c to avoid interpreting these as extra
 * interrupts.
 */
static void handle_event(struct work_struct *work)
{
	struct uproc_info *uproc =
		container_of(work, struct uproc_info, workqueue);

	dev_dbg(uproc->dev, "Calling rproc_vq_interrupt...\n");

	/* Process incoming buffers on our vring */
	while (IRQ_HANDLED == rproc_vq_interrupt(uproc->rproc, 0))
                ;

	/* Must allow wakeup of potenitally blocking senders: */
	rproc_vq_interrupt(uproc->rproc, 1);
}

static irqreturn_t uproc_interrupt(int irq, void *dev_id)
{
	struct uproc_info *uproc = dev_id;

	dev_dbg(uproc->dev, "Scheduling_work...\n");

	schedule_work(&uproc->workqueue);

	return IRQ_HANDLED;
}

static int uproc_start(struct rproc *rproc)
{
	struct uproc_info *uproc = rproc->priv;
	int error;

	dev_dbg(uproc->dev, "start\n");

	INIT_WORK(&uproc->workqueue, handle_event);

	error = request_irq(uproc->irq_ring, uproc_interrupt, 0,
			    dev_name(uproc->dev), uproc);
	if (error)
		dev_err(uproc->dev, "failed to grab ring irq\n");

	return error;
}

static int uproc_stop(struct rproc *rproc)
{
	struct uproc_info *uproc = rproc->priv;

	dev_dbg(uproc->dev, "stop\n");

	free_irq(uproc->irq_ring, uproc);
	/* Flush any pending work: */
	flush_work(&uproc->workqueue);

	return 0;
}

static struct rproc_ops uproc_ops = {
	.start		= uproc_start,
	.stop		= uproc_stop,
	.kick		= uproc_kick,
	.alloc		= uproc_alloc,
	.free		= uproc_free,
};

static struct resource_table *
uproc_find_rsc_table(struct rproc *rproc, const struct firmware *fw,
		     int *tablesz)
{
	struct uproc_info *uproc = rproc->priv;

	if (tablesz)
		*tablesz = uproc->rsc_table_size;
	return uproc->rsc_table;
}

static int uproc_load_segments(struct rproc *rproc, const struct firmware *fw)
{
	/* nothing to do */
	return 0;
}

static const struct file_operations uproc_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= uproc_dev_fop_open,
	.mmap		= uproc_dev_fop_mmap,
};

static struct rproc_fw_ops uproc_fw_ops = {
	.find_rsc_table		= uproc_find_rsc_table,
	.load			= uproc_load_segments,
};

/**
 * uproc_populate_segments() - scan the configuration for "mem" and populate
 * polulate in uproc local structure. This information will be used to process
 * user mmap requests.
 */
#ifdef CONFIG_OF
static inline int uproc_populate_segments(struct device_node *np,
					struct uproc_info *uproc)
{
	int len, i;
	u32 *paddr_map;
	int num_maps = 0;
	struct uproc_addr_map *pmapentry;

	if (of_get_property(np, "mem", &len)) {

		paddr_map = devm_kzalloc(uproc->dev, len, GFP_KERNEL);
		if (!paddr_map) {
			dev_err(uproc->dev, "memory allocation failed\n");
			return -ENOMEM;
		}
		/* check if length even multiple of sizeof(u32), i.e.,
		    the dt bindings need to be of the form <addr length>
		 */
		len = len / sizeof(u32);
		if ((len % 2) != 0) {
			dev_err(uproc->dev, "invalid address map in dt binding\n");
			return -EINVAL;
		}
		num_maps = len / 2;
		if (of_property_read_u32_array(np, "mem",
					       paddr_map, len)) {
			dev_err(uproc->dev, "No addr-map array  in dt bindings\n");
			return -ENODEV;
		}

		pmapentry  = devm_kzalloc(uproc->dev,
				sizeof(struct uproc_addr_map) * num_maps,
				GFP_KERNEL);
		if (!pmapentry) {
			dev_err(uproc->dev, "devm_kzalloc mapping failed\n");
			return -ENOMEM;
		}

		/* populate the uproc structure for policing */
		for (i = 0; i < num_maps; i++) {
			pmapentry->index = i;
			pmapentry->addr = *paddr_map++;
			pmapentry->length = *paddr_map++;
			list_add_tail(&pmapentry->node, &uproc->addr_map);
			pmapentry++;
		}

	}
	return 0;
}
#else
static inline void uproc_populate_segments(struct device_node *np,
					struct uproc_info *uproc)
{
	return 0;
}
#endif

static int uproc_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int *pdata = dev->platform_data;
	struct device_node *np = dev->of_node;
	struct uproc_info *uproc;
	struct miscdevice *misc;
	struct uio_info *uio;
	struct rproc *rproc;
	struct resource *r;
	int error = 0;
	int i;

	rproc = rproc_alloc(dev, dev_name(dev), &uproc_ops, NULL,
			    sizeof(*uproc));
	if (!rproc)
		return -ENOMEM;

	rproc->fw_ops	= &uproc_fw_ops;

	uproc		= rproc->priv;
	uproc->rproc	= rproc;
	uproc->dev	= dev;
	spin_lock_init(&uproc->lock);

	uio		= &uproc->uio;
	uio->name	= dev_name(dev);
	uio->version	= DRIVER_VERSION;
	uio->priv		= uproc;
	uio->handler	= uproc_handler;
	uio->irqcontrol	= uproc_irqcontrol;
	uio->open	= uproc_open;
	uio->release	= uproc_release;
	uio->ioctl	= uproc_ioctl;

	uproc->irq_ctl	= platform_get_irq(pdev, 0);
	uproc->irq_ring	= platform_get_irq(pdev, 1);
	if (!(uproc->irq_ctl < 0))
		/* pass up control irq to user-space */
		uio->irq	= uproc->irq_ctl;

	uproc->kick_gpio = -1;
	if (pdata)
		uproc->kick_gpio = *pdata;
#ifdef CONFIG_OF
	if (dev->of_node) {
		error = of_get_named_gpio_flags(np, "kick-gpio", 0, NULL);
		if (error >= 0)
			uproc->kick_gpio = error;
	}
#endif
	if (uproc->kick_gpio < 0)
		dev_warn(dev, "kick gpio\n");

	for (i = 0; i < MAX_UIO_MAPS; ++i) {
		r = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!r)
			break;
		uio->mem[i].memtype = UIO_MEM_PHYS;
		uio->mem[i].addr    = r->start;
		uio->mem[i].size    = resource_size(r);
		uio->mem[i].name    = r->name;
	}

	uproc->clk = clk_get(dev, NULL);

	INIT_LIST_HEAD(&uproc->addr_map);
	error = uproc_populate_segments(np, uproc);
	if (error)
		goto fail_uio;

	if (uio->irq && uio->mem[0].memtype != UIO_MEM_NONE) {
		error = uio_register_device(dev, uio);
		if (error) {
			dev_err(dev, "failed to register uio device\n");
			goto fail_uio;
		}
	}
	if (!(uproc->irq_ctl < 0 || uproc->irq_ring < 0)) {
		error = rproc_add(rproc);
		if (error) {
			dev_err(dev, "error adding remoteproc device\n");
			goto fail_rproc;
		}
	}

	platform_set_drvdata(pdev, uproc);

	if (!list_empty(&uproc->addr_map)) {
		misc = &uproc->misc;

		misc->minor	= MISC_DYNAMIC_MINOR;
#ifdef CONFIG_OF
		if (of_property_read_string(np, "label", &misc->name) < 0)
			misc->name = dev_name(dev);
		if (!misc->name)
			misc->name = "unknown";
#else
		misc->name	= dev_name(dev);
#endif
		misc->fops	= &uproc_dev_fops;
		misc->parent	= dev;

		if (misc_register(misc)) {
			dev_err(dev, "could not register misc device\n");
			goto fail_rproc;
		}
		dev_info(dev, "registered misc device %s\n", misc->name);
	}

	return 0;

fail_rproc:
	uio_unregister_device(uio);
fail_uio:
	clk_put(uproc->clk);
	rproc_put(rproc);
	devm_kfree(dev, uproc);
	return error;
}

static int uproc_driver_remove(struct platform_device *pdev)
{
	struct uproc_info *uproc = platform_get_drvdata(pdev);
	struct uio_info *uio = &uproc->uio;

	if (uproc) {
		if (uio->irq && uio->mem[0].memtype != UIO_MEM_NONE)
			uio_unregister_device(&uproc->uio);
		if (!(uproc->irq_ctl < 0 || uproc->irq_ring < 0))
			rproc_put(uproc->rproc);
		clk_put(uproc->clk);
		if (!list_empty(&uproc->addr_map))
			misc_deregister(&uproc->misc);
	}
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static struct of_device_id uproc_of_match[] = {
	{ .compatible = "linux,rproc-user", },
	{},
};
MODULE_DEVICE_TABLE(of, uproc_of_match);

static struct platform_driver uproc_driver = {
	.driver	= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = uproc_of_match,
	},
	.probe	= uproc_driver_probe,
	.remove	= uproc_driver_remove,
};

module_platform_driver(uproc_driver);
MODULE_AUTHOR("Cyril Chemparathy");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("User-space driver for the Remote Processor Framework");
MODULE_ALIAS("platform:" DRIVER_NAME);
