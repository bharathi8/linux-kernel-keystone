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

#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/eventfd.h>
#include <linux/mm_types.h>
#include <linux/virtio_ring.h>
#include <linux/udma.h>
#include <linux/dma-contiguous.h>
#include <linux/anon_inodes.h>
#include <linux/poll.h>
#include <linux/workqueue.h>
#include <linux/keystone-dma.h>

DEFINE_SPINLOCK(udma_lock);

struct udma_device {
	struct device		*dev;
	struct miscdevice	 misc;
	struct list_head	 users;
	struct kref		 refcount;
	const char		*name;
};
#define from_misc(misc)	container_of(misc, struct udma_device, misc)
#define to_misc(udma)	(&(udma)->misc)
#define udma_dev(udma)	((udma)->dev)

struct udma_user {
	struct udma_device	*udma;
	struct list_head	 node;
	struct list_head	 maps;
	struct file		*file;
};
#define udma_user_dev(user)	udma_dev((user)->udma)

struct udma_map {
	struct udma_user	*user;
	struct list_head	 node;
	size_t			 size;
	struct page		*page;
	void			*cpu_addr;
	struct kref		 refcount;
	struct list_head	 channels;
};
#define udma_map_dev(map)	udma_user_dev((map)->user)

struct udma_request {
	struct udma_chan		*chan;
	struct vring_desc		*desc;
	struct dma_async_tx_descriptor	*dma_desc;
	dma_cookie_t			 cookie;
	struct scatterlist		 sg[1];
};

struct udma_chan {
	struct vring			 vring;

	struct vm_area_struct		*last_vma;
	struct udma_user		*user;
	struct dma_chan			*chan;
	struct udma_request		*req;

	unsigned			 last_avail_idx;
	enum dma_data_direction		 data_dir;
	enum dma_transfer_direction	 xfer_dir;

	struct udma_map			*map;
	struct file			*file;
	struct list_head		 node;
	int				 id;
	struct udma_chan_data		 data;
	wait_queue_head_t		 wqh;
};

#define udma_chan_dev(chan)	udma_map_dev((chan)->map)
#define udma_chan_name(chan)	((chan)->data.name)

static void udma_device_release(struct kref *kref)
{
	struct udma_device *udma;

	udma = container_of(kref, struct udma_device, refcount);

	WARN_ON(!list_empty(&udma->users));

	dev_dbg(udma_dev(udma), "released udma device instance\n");
	kfree(udma);
}

static inline struct udma_device *udma_device_get(struct udma_device *udma)
{
	kref_get(&udma->refcount);
	return udma;
}

static inline void udma_device_put(struct udma_device *udma)
{
	kref_put(&udma->refcount, udma_device_release);
}

static void udma_add_user(struct udma_device *udma, struct udma_user *user)
{
	spin_lock(&udma_lock);
	list_add_tail(&user->node, &udma->users);
	spin_unlock(&udma_lock);
	udma_device_get(udma);
}

static void udma_del_user(struct udma_device *udma, struct udma_user *user)
{
	spin_lock(&udma_lock);
	list_del(&user->node);
	spin_unlock(&udma_lock);
	udma_device_put(udma);
}

static void udma_map_add_chan(struct udma_map *map, struct udma_chan *chan)
{
	spin_lock(&udma_lock);
	list_add_tail(&chan->node, &map->channels);
	spin_unlock(&udma_lock);
}

static void udma_map_del_chan(struct udma_map *map, struct udma_chan *chan)
{
	spin_lock(&udma_lock);
	list_del(&chan->node);
	spin_unlock(&udma_lock);
}

static struct udma_chan *udma_map_first_chan(struct udma_map *map)
{
	struct udma_chan *chan = NULL;

	spin_lock(&udma_lock);
	if (!list_empty(&map->channels))
		chan = list_first_entry(&map->channels, struct udma_chan, node);
	spin_unlock(&udma_lock);
	return chan;
}

static void udma_user_add_map(struct udma_user *user, struct udma_map *map)
{
	spin_lock(&udma_lock);
	list_add_tail(&map->node, &user->maps);
	spin_unlock(&udma_lock);
}

static void udma_user_del_map(struct udma_user *user, struct udma_map *map)
{
	spin_lock(&udma_lock);
	list_del(&map->node);
	spin_unlock(&udma_lock);
}

static struct udma_map *udma_user_first_map(struct udma_user *user)
{
	struct udma_map *map = NULL;

	spin_lock(&udma_lock);
	if (!list_empty(&user->maps))
		map = list_first_entry(&user->maps, struct udma_map, node);
	spin_unlock(&udma_lock);
	return map;
}

static struct udma_map *
__udma_find_map(struct udma_user *user, struct udma_chan *chan,
		unsigned long start, unsigned long end, unsigned long *offset)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->active_mm;
	struct udma_map *map = NULL;

	down_read(&mm->mmap_sem);

	vma = find_vma(mm, start);

	if (likely(vma && start >= vma->vm_start && end <= vma->vm_end &&
		   vma->vm_file == user->file)) {
		map = vma->vm_private_data;
		*offset = start - vma->vm_start;
		if (likely(chan))
			chan->last_vma = vma;
	}

	up_read(&mm->mmap_sem);

	return map;
}

static inline struct udma_map *
udma_find_map(struct udma_user *user, struct udma_chan *chan,
	      void * __user ptr, size_t size, unsigned long *offset)
{
	struct vm_area_struct *vma;
	unsigned long start = (unsigned long)ptr;
	unsigned long end = start + size;

	vma = likely(chan) ? chan->last_vma : NULL;
	if (likely(vma && start >= vma->vm_start && end <= vma->vm_end)) {
		if (offset)
			*offset = start - vma->vm_start;
		return vma->vm_private_data;
	}

	return __udma_find_map(user, chan, start, end, offset);
}

static inline bool is_valid_direction(enum dma_transfer_direction xfer_dir)
{
	return (xfer_dir == DMA_DEV_TO_MEM || xfer_dir == DMA_MEM_TO_DEV);
}

static void udma_chan_notify(struct dma_chan *dma_chan, void *arg)
{
	struct udma_chan *chan = arg;
	unsigned long flags;

	spin_lock_irqsave(&chan->wqh.lock, flags);
	dmaengine_pause(chan->chan);
	if (waitqueue_active(&chan->wqh)) {
		wake_up_locked_poll(&chan->wqh,
				    ((chan->xfer_dir == DMA_MEM_TO_DEV) ?
				     POLLOUT : POLLIN));
	}
	spin_unlock_irqrestore(&chan->wqh.lock, flags);

	return;
}

static void udma_chan_complete_rx(struct udma_chan *chan,
			       struct udma_request *req,
			       enum dma_status status)
{
	enum dma_data_direction dir = chan->data_dir;
	struct udma_user *user = chan->user;
	struct vring *vring = &chan->vring;
	int id = req->desc - vring->desc;
	__u16 used_idx;

	if (req->dma_desc)
		dma_unmap_sg(udma_chan_dev(chan), req->sg, 1, dir);

	/* return desc to the used list */
	used_idx = vring->used->idx & (vring->num - 1);
	vring->used->ring[used_idx].id = id;
	if (status == DMA_SUCCESS)
		vring->used->ring[used_idx].len = req->sg[0].length;
	else
		vring->used->ring[used_idx].len = -1;

	vring->used->idx++;

	dev_vdbg(udma_user_dev(user), "(%s) used %d, status %s\n",
		 udma_chan_name(chan), vring->used->idx,
		 (status == DMA_SUCCESS) ? "success" : "error");
}

static void udma_chan_complete_rx_cb(void *data)
{
	struct udma_request *req = data;
	struct udma_chan *chan = req->chan;

	udma_chan_complete_rx(chan, req, DMA_SUCCESS);
}

static struct dma_async_tx_descriptor *udma_rxpool_alloc(void *arg,
		unsigned q_num, unsigned bufsize)
{
	struct udma_chan *chan = arg;
	struct udma_user *user = chan->user;
	struct vring *vring = &chan->vring;
	struct vring_desc *desc;
	struct udma_request *req;
	unsigned long buf_size;
	void __user *buf_virt;
	struct udma_map *map;
	unsigned long offset = 0;
	int segs;
	__u16 idx, desc_idx;

	while (chan->last_avail_idx != vring->avail->idx) {
		idx = chan->last_avail_idx;
		desc_idx = vring->avail->ring[idx & (vring->num - 1)];
		desc = vring->desc + desc_idx;
		req = chan->req + desc_idx;
		buf_size = desc->len;
		buf_virt = (void __user *)(unsigned long)desc->addr;
		req->dma_desc	= NULL;

		dev_dbg(udma_chan_dev(chan), "(%s) rxpool_alloc idx %d: %d\n",
			udma_chan_name(chan), idx, vring->avail->idx);
		map = udma_find_map(user, chan, buf_virt, buf_size, &offset);
		if (unlikely(!map)) {
			dev_err(udma_user_dev(user), "(%s) chan do not"
				"belong to map\n", udma_chan_name(chan));
			idx++;
			chan->last_avail_idx = idx;
			vring_avail_event(&chan->vring) = idx;
			udma_chan_complete_rx(chan, req, DMA_ERROR);
			continue;
		}

		sg_set_buf(req->sg, map->cpu_addr + offset, buf_size);
		segs = dma_map_sg(udma_chan_dev(chan), req->sg, 1,
				  chan->data_dir);
		if (unlikely(segs != 1)) {
			dev_err(udma_user_dev(user), "(%s) failed to map"
				"dma buffer\n", udma_chan_name(chan));
			idx++;
			chan->last_avail_idx = idx;
			vring_avail_event(&chan->vring) = idx;
			udma_chan_complete_rx(chan, req, DMA_ERROR);
			continue;
		}

		req->dma_desc = dmaengine_prep_slave_sg(chan->chan, req->sg, 1,
							chan->xfer_dir, 0);
		if (unlikely(IS_ERR(req->dma_desc))) {
			dev_err(udma_user_dev(user), " (%s) fail to prep dma\n",
				udma_chan_name(chan));
			dma_unmap_sg(udma_chan_dev(chan), req->sg, 1,
				     chan->xfer_dir);
			return NULL;
		}

		req->dma_desc->callback = udma_chan_complete_rx_cb;
		req->dma_desc->callback_param = req;
		req->cookie = dmaengine_submit(req->dma_desc);

		idx++;
		chan->last_avail_idx = idx;
		vring_avail_event(&chan->vring) = idx;

		return req->dma_desc;
	}
	/* Nothing available in avail list, return NULL */
	return NULL;
}

static void udma_rxpool_free(void *arg, unsigned q_num, unsigned bufsize,
		struct dma_async_tx_descriptor *desc)
{
	struct udma_chan *chan = arg;

	/* Return to used list with error, so len = -1 */
	udma_chan_complete_rx(chan, chan->req, DMA_ERROR);
}

static int udma_chan_setup_dma(struct udma_chan *chan)
{
	struct device *dev = udma_chan_dev(chan);
	struct udma_chan_data *data = &chan->data;
	struct dma_keystone_info config;
	dma_cap_mask_t mask;
	int error;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	chan->chan = dma_request_channel_by_name(mask, data->name);

	if (IS_ERR_OR_NULL(chan->chan)) {
		dev_err(dev, "(%s) failed to open dmachan \n",
			udma_chan_name(chan));
		return -ENODEV;
	}

	memset(&config, 0, sizeof(config));
	if (chan->xfer_dir == DMA_MEM_TO_DEV) {
		config.direction	= DMA_MEM_TO_DEV;
		config.tx_queue_depth	= data->num_desc;

		error = dma_keystone_config(chan->chan, &config);
		if (error < 0) {
			dev_err(dev, "(%s) failed to set keystone_config\n",
				udma_chan_name(chan));
			dma_release_channel(chan->chan);
			return error;
		}
	}

	else if (chan->xfer_dir == DMA_DEV_TO_MEM) {
		config.direction		= DMA_DEV_TO_MEM;
		config.scatterlist_size		= 1;
		config.rxpool_allocator		= udma_rxpool_alloc;
		config.rxpool_destructor	= udma_rxpool_free;
		config.rxpool_param		= chan;
		config.rxpool_count		= 1;
		config.rxpool_thresh_enable	= DMA_THRESH_NONE;
		config.rxpools[0].pool_depth	= data->num_desc;
		config.rxpools[0].buffer_size	= 0;

		error = dma_keystone_config(chan->chan, &config);
		if (error < 0) {
			dev_err(dev, "(%s) failed to set keystone_config\n",
				udma_chan_name(chan));
			dma_release_channel(chan->chan);
			return error;
		}
	}
	dma_set_notify(chan->chan, udma_chan_notify, chan);

	dmaengine_pause(chan->chan);

	dma_rxfree_refill(chan->chan);

	return 0;
}

static void udma_chan_shutdown_dma(struct udma_chan *chan)
{
	dma_release_channel(chan->chan);
}

static void udma_chan_destroy(struct udma_chan *chan)
{
	if (!chan->file)
		return;
	chan->file->private_data = NULL;
	udma_map_del_chan(chan->map, chan);
	udma_chan_shutdown_dma(chan);
	kfree(chan->req);
	kfree(chan);
}

static void udma_chan_complete_tx(struct udma_chan *chan,
			       struct udma_request *req,
			       enum dma_status status)
{
	enum dma_data_direction dir = chan->data_dir;
	struct udma_user *user = chan->user;
	struct vring *vring = &chan->vring;
	int id = req->desc - vring->desc;
	__u16 used_idx;

	if (req->dma_desc)
		dma_unmap_sg(udma_chan_dev(chan), req->sg, 1, dir);

	/* return desc to the used list */
	used_idx = vring->used->idx & (vring->num - 1);
	vring->used->ring[used_idx].id = id;
	if (status == DMA_SUCCESS)
		vring->used->ring[used_idx].len = req->sg[0].length;
	else
		vring->used->ring[used_idx].len = -1;

	vring->used->idx++;

	dev_vdbg(udma_user_dev(user), "(%s) used %d, status %s\n",
		 udma_chan_name(chan), vring->used->idx,
		 (status == DMA_SUCCESS) ? "success" : "error");
}

static void udma_chan_complete_tx_cb(void *data)
{
	struct udma_request *req = data;
	struct udma_chan *chan = req->chan;
	enum dma_status status;

	status = dma_async_is_tx_complete(chan->chan, req->cookie,
					  NULL, NULL);
	udma_chan_complete_tx(chan, req, status);
}

static int udma_chan_submit_tx(struct udma_chan *chan, __u16 idx)
{
	struct udma_user *user = chan->user;
	struct vring *vring = &chan->vring;
	struct vring_desc *desc = vring->desc + idx;
	struct udma_request *req = chan->req + idx;
	unsigned long buf_size = desc->len;
	void __user *buf_virt;
	struct udma_map *map;
	unsigned long offset;
	int segs;

	buf_virt = (void __user *)(unsigned long)desc->addr;

	req->dma_desc	= NULL;

	map = udma_find_map(user, chan, buf_virt, buf_size, &offset);
	if (unlikely(!map)) {
		dev_err(udma_user_dev(user), "(%s) chan do not belong to map\n",
			udma_chan_name(chan));
		udma_chan_complete_tx(chan, req, DMA_ERROR);
		return 0;
	}

	sg_set_buf(req->sg, map->cpu_addr + offset, buf_size);
	segs = dma_map_sg(udma_chan_dev(chan), req->sg, 1,
			  chan->data_dir);
	if (unlikely(segs != 1)) {
		dev_err(udma_user_dev(user), "(%s) failed to map dma buffer\n",
			udma_chan_name(chan));
		udma_chan_complete_tx(chan, req, DMA_ERROR);
		return 0;
	}

	req->dma_desc = dmaengine_prep_slave_sg(chan->chan, req->sg, 1,
						chan->xfer_dir, 0);
	if (unlikely(IS_ERR(req->dma_desc))) {
		dev_err(udma_user_dev(user), " (%s) failed to prep dma\n",
			udma_chan_name(chan));
		udma_chan_complete_tx(chan, req, DMA_ERROR);
		return -ENOMEM;
	}

	req->dma_desc->callback = udma_chan_complete_tx_cb;
	req->dma_desc->callback_param = req;
	req->cookie = dmaengine_submit(req->dma_desc);

	return 0;
}

static int udma_chan_fop_release(struct inode *inode, struct file *file)
{
	struct udma_chan *chan = file->private_data;

	if (!chan)
		return 0;
	dev_dbg(udma_chan_dev(chan), "(%s) fd closed\n", udma_chan_name(chan));
	udma_chan_destroy(chan);
	return 0;
}

static unsigned int udma_chan_fop_poll(struct file *file, poll_table *wait)
{
	struct udma_chan *chan = file->private_data;
	struct device *dev = udma_chan_dev(chan);
	struct vring *vring = &chan->vring;
	__u16 idx, desc_idx;

	dev_dbg(dev, "(%s) udma_chan_fop_poll() called\n",
		udma_chan_name(chan));

	if (!is_valid_direction(chan->xfer_dir)) {
		dev_err(dev, "(%s) bad direction %d\n",
			udma_chan_name(chan), chan->xfer_dir);
		return -EINVAL;
	}

	if (chan->xfer_dir == DMA_MEM_TO_DEV) {
		for (idx = chan->last_avail_idx; idx != vring->avail->idx;
		     idx++) {
			desc_idx = vring->avail->ring[idx & (vring->num - 1)];

			if (udma_chan_submit_tx(chan, desc_idx) < 0)
				break;
		}
		chan->last_avail_idx = idx;
		vring_avail_event(&chan->vring) = idx;
	}

	dma_poll(chan->chan, -1);

	dma_rxfree_refill(chan->chan);

	poll_wait(file, &chan->wqh, wait);

	/* Check if the kernel's view of used index and the user's view are
	 * same. If not user has already got stuff to do */
	if (vring_used_event(&chan->vring) != vring->used->idx) {
		return ((chan->xfer_dir == DMA_MEM_TO_DEV) ?
			POLLOUT : POLLIN);
	}

	dev_dbg(dev, " (%s) about to block, ring used %d, kernel used %d\n",
		udma_chan_name(chan),
		vring_used_event(&chan->vring), vring->used->idx);

	dmaengine_resume(chan->chan);	

	return 0;
}

static const struct file_operations udma_chan_fops = {
	.release	= udma_chan_fop_release,
	.poll		= udma_chan_fop_poll,
};

static int udma_chan_init_fd(struct udma_chan *chan)
{
	struct device *dev = udma_chan_dev(chan);
	struct file *file;
	int fd;

	fd = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return fd;

	file = anon_inode_getfile(udma_chan_name(chan), &udma_chan_fops, chan,
				  O_RDWR | O_CLOEXEC);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		return PTR_ERR(file);
	}

	chan->file = file;

	fd_install(fd, file);
	dev_dbg(dev, "(%s) fd %d\n", udma_chan_name(chan), fd);

	return fd;
}

static struct udma_chan *
udma_chan_create(struct udma_user *user, struct udma_chan_data *data)
{
	struct device		*dev = udma_user_dev(user);
	size_t			 ring_size;
	void __user		*ring_user;
	struct udma_chan	*chan;
	struct udma_map		*map;
	struct udma_request	*req;
	void			*ring_virt;
	unsigned long		 offset = 0;
	int			 error, i;

	ring_size = vring_size(data->num_desc, data->align);
	dev_dbg(dev, "(%s) chan_create\n",
		data->name);
	if (ring_size != data->ring_size) {
		dev_err(dev, "(%s) bad chan size %d, expect %d\n",
			data->name,
			data->ring_size, ring_size);
		return ERR_PTR(-EOVERFLOW);
	}

	if (!is_valid_direction(data->direction)) {
		dev_err(dev, "(%s) bad direction %d\n",
			data->name, data->direction);
		return ERR_PTR(-EINVAL);
	}

	ring_user = (void __user *)data->ring_virt;
	map = udma_find_map(user, NULL, ring_user, ring_size, &offset);
	if (!map) {
		dev_err(dev, "(%s) chan does not belong to map\n",
			data->name);
		return ERR_PTR(-ENODEV);
	}

	ring_virt = map->cpu_addr + offset;
	memset(ring_virt, 0, ring_size);

	chan = kzalloc(sizeof(*chan), GFP_KERNEL);
	if (!chan) {
		dev_err(dev, "(%s) failed to allocate chan\n",
			data->name);
		return ERR_PTR(-ENOMEM);
	}

	chan->req = kzalloc(sizeof(*chan->req) * data->num_desc,
			    GFP_KERNEL);
	if (!chan->req) {
		dev_err(dev, "(%s) failed to allocate chan requests\n",
			data->name);
		kfree(chan);
		return ERR_PTR(-ENOMEM);
	}

	chan->data = *data;
	chan->user =  user;
	chan->map  =  map;

	vring_init(&chan->vring, data->num_desc, ring_virt, data->align);

	for (i = 0, req = chan->req; i < data->num_desc; i++, req++) {
		sg_init_table(req->sg, 1);
		req->chan = chan;
		req->desc = chan->vring.desc + i;
	}

	chan->xfer_dir = data->direction;

	if (chan->xfer_dir == DMA_DEV_TO_MEM)
		chan->data_dir = DMA_FROM_DEVICE;
	else
		chan->data_dir = DMA_TO_DEVICE;

	chan->id = udma_chan_init_fd(chan);
	if (chan->id < 0) {
		dev_err(dev, "(%s) failed to allocate chan id\n",
			udma_chan_name(chan));
		kfree(chan->req);
		kfree(chan);
		return ERR_PTR(-ENOMEM);
	}

	init_waitqueue_head(&chan->wqh);

	error = udma_chan_setup_dma(chan);
	if (error < 0) {
		put_unused_fd(chan->id);
		kfree(chan->req);
		kfree(chan);
		return ERR_PTR(error);
	}


	udma_map_add_chan(map, chan);

	dev_dbg(dev, "(%s) chan: usr %lx, kern %lx, ofs %lx, id %d\n",
		udma_chan_name(chan), (unsigned long)data->ring_virt,
		(unsigned long)(map->cpu_addr + offset), offset, chan->id);

	return chan;
}

static void udma_map_release(struct kref *kref)
{
	struct udma_map *map = container_of(kref, struct udma_map, refcount);
	struct udma_user *user = map->user;
	struct udma_device *udma = user->udma;
	struct udma_chan *chan;

	for (;;) {
		chan = udma_map_first_chan(map);
		if (!chan)
			break;
		udma_chan_destroy(chan);
	}

	dev_dbg(udma_map_dev(map), "closed map kern %lx, size %lx\n",
		(unsigned long)map->cpu_addr, (unsigned long)map->size);

	dma_release_from_contiguous(udma->dev, map->page,
				   map->size >> PAGE_SHIFT);
	udma_user_del_map(user, map);
	kfree(map);
}

static inline struct udma_map *udma_map_get(struct udma_map *map)
{
	kref_get(&map->refcount);
	return map;
}

static inline void udma_map_put(struct udma_map *map)
{
	kref_put(&map->refcount, udma_map_release);
}

static void udma_vma_open(struct vm_area_struct *vma)
{
	udma_map_get(vma->vm_private_data);
}

static void udma_vma_close(struct vm_area_struct *vma)
{
	udma_map_put(vma->vm_private_data);
}

static struct vm_operations_struct udma_vm_ops = {
	.open	= udma_vma_open,
	.close	= udma_vma_close,
};

static struct udma_map *udma_map_create(struct udma_user *user,
					struct vm_area_struct *vma)
{
	struct udma_device *udma = user->udma;
	struct udma_map *map;
	unsigned long order;
	size_t count;
	int ret;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		dev_err(udma_user_dev(user), "failed to allocate map\n");
		return ERR_PTR(-ENOMEM);
	}

	map->user = user;
	map->size = vma->vm_end - vma->vm_start;

	kref_init(&map->refcount);
	INIT_LIST_HEAD(&map->channels);

	count = map->size >> PAGE_SHIFT;
	order = get_order(map->size);
	map->page = dma_alloc_from_contiguous(udma->dev, count, order);
	if (!map->page) {
		dev_err(udma_map_dev(map), "failed to allocate dma memory\n");
		kfree(map);
		return ERR_PTR(-ENOMEM);
	}

	map->cpu_addr = page_address(map->page);
	memset(map->cpu_addr, 0, map->size);

	ret = remap_pfn_range(vma, vma->vm_start, page_to_pfn(map->page),
			      map->size, vma->vm_page_prot);
	if (ret) {
		dev_err(udma_map_dev(map), "failed to user map dma memory\n");
		dma_release_from_contiguous(udma->dev, map->page, count);
		kfree(map);
		return ERR_PTR(-ENOMEM);
	}

	vma->vm_private_data	= map;
	vma->vm_ops		= &udma_vm_ops;

	udma_user_add_map(user, map);

	dev_dbg(udma_map_dev(map), "opened map %lx..%lx, kern %lx\n",
		vma->vm_start, vma->vm_end - 1, (unsigned long)map->cpu_addr);

	return map;
}

static struct udma_user *
udma_user_create(struct udma_device *udma, struct file *file)
{
	struct udma_user *user;

	user = kzalloc(sizeof(*user), GFP_KERNEL);
	if (!user) {
		dev_err(udma_dev(udma), "failed to allocate user\n");
		return ERR_PTR(-ENOMEM);
	}

	user->udma = udma;
	INIT_LIST_HEAD(&user->maps);
	user->file = file;
	file->private_data = user;

	udma_add_user(udma, user);

	dev_dbg(udma_user_dev(user), "opened user\n");

	return user;
}

static int udma_dev_fop_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct udma_user *user = file->private_data;
	struct udma_map *map;

	if (vma->vm_end < vma->vm_start) {
		dev_err(udma_user_dev(user),
			"strangely inverted vm area\n");
		return -EINVAL;
	}

	if (vma->vm_pgoff) {
		dev_err(udma_user_dev(user),
			"cannot mmap from non-zero offset\n");
		return -EINVAL;
	}


	if (vma->vm_start & ~PAGE_MASK) {
		dev_err(udma_user_dev(user),
			"must mmap at page boundary\n");
		return -EINVAL;
	}

	map = udma_map_create(user, vma);

	return IS_ERR(map) ? PTR_ERR(map) : 0;
}

static long udma_dev_fop_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct udma_user *user = file->private_data;
	void __user *argp = (void __user *)arg;
	struct udma_chan_data data;
	struct udma_chan *chan;
	int ret;

	if (likely(cmd == UDMA_IOC_ATTACH)) {
		if (copy_from_user(&data, argp, sizeof(data)))
			return -EFAULT;

		chan = udma_chan_create(user, &data);
		ret = IS_ERR(chan) ? PTR_ERR(chan) : 0;
		if (ret)
			return ret;

		data.handle = chan->id;
		if (copy_to_user(argp, &data, sizeof(data)))
			return -EFAULT;
	} else
		return -EINVAL;
	return 0;
}

static int udma_dev_fop_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct udma_device *udma = from_misc(misc);
	struct udma_user *user;

	user = udma_user_create(udma, file);

	return IS_ERR(user) ? PTR_ERR(user) : 0;
}

static int udma_dev_fop_release(struct inode *inode, struct file *file)
{
	struct udma_user *user = file->private_data;
	struct udma_device *udma = user->udma;
	struct udma_map *map;

	for (;;) {
		map = udma_user_first_map(user);
		if (!map)
			break;
		udma_user_del_map(user, map);
		udma_map_put(map);
	}

	dev_dbg(udma_user_dev(user), "closed user\n");
	udma_del_user(udma, user);
	kfree(user);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations udma_dev_fops = {
	.owner          = THIS_MODULE,
	.open           = udma_dev_fop_open,
	.release        = udma_dev_fop_release,
	.mmap		= udma_dev_fop_mmap,
	.unlocked_ioctl = udma_dev_fop_ioctl,
};

static const char * udma_get_name(struct device_node *node)
{
	const char *name;

	if (of_property_read_string(node, "label", &name) < 0)
		name = node->name;
	if (!name)
		name = "unknown";
	return name;
}

static int keystone_udma_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct udma_device *udma;
	struct device *dev = &pdev->dev;
	struct miscdevice *misc;
	int ret;

	if (!node) {
		dev_err(dev, "could not find device info\n");
		return -EINVAL;
	}

	udma = devm_kzalloc(dev, sizeof(*udma), GFP_KERNEL);
	if (!udma) {
		dev_err(dev, "could not allocate driver mem\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, udma);
	misc = to_misc(udma);

	udma->dev = dev;
	udma->name = udma_get_name(node);
	INIT_LIST_HEAD(&udma->users);
	kref_init(&udma->refcount);

	misc->minor	= MISC_DYNAMIC_MINOR;
	misc->name	= udma->name;
	misc->fops	= &udma_dev_fops;
	misc->parent	= dev;

	ret = misc_register(misc);
	if (ret) {
		dev_err(dev, "could not register misc device\n");
		return ret;
	}

	dev_info(udma_dev(udma), "registered udma device %s\n", misc->name);

	return 0;
}

static int keystone_udma_remove(struct platform_device *pdev)
{
	struct udma_device *udma = platform_get_drvdata(pdev);
	struct miscdevice *misc = to_misc(udma);

	misc_deregister(misc);
	platform_set_drvdata(pdev, NULL);
	udma_device_put(udma);

	return 0;
}

static struct of_device_id of_match[] = {
	{ .compatible = "ti,keystone-udma", },
	{},
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver keystone_udma_driver = {
	.probe	= keystone_udma_probe,
	.remove	= keystone_udma_remove,
	.driver = {
		.name		= "keystone-udma",
		.owner		= THIS_MODULE,
		.of_match_table	= of_match,
	},
};

static int __init keystone_udma_init(void)
{
	return platform_driver_register(&keystone_udma_driver);
}
module_init(keystone_udma_init);

static void __exit keystone_udma_exit(void)
{
	platform_driver_unregister(&keystone_udma_driver);
}
module_exit(keystone_udma_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cyril Chemparathy <cyril@ti.com>");
MODULE_DESCRIPTION("TI Keystone User DMA driver");
