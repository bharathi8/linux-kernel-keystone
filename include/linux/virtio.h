#ifndef _LINUX_VIRTIO_H
#define _LINUX_VIRTIO_H
/* Everything a virtio driver needs to work with any particular virtio
 * implementation. */
#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/gfp.h>

struct virtqueue;

struct virtqueue_ops {
	int	(*add_buf)(struct virtqueue *vq, struct scatterlist sg[],
			   unsigned int out_num, unsigned int in_num,
			   void *data, unsigned flags, gfp_t gfp);
	bool	(*kick_prepare)(struct virtqueue *vq);
	void	(*notify)(struct virtqueue *vq);
	void	*(*get_buf)(struct virtqueue *vq, unsigned int *len);
	void	(*disable_cb)(struct virtqueue *vq);
	bool	(*enable_cb)(struct virtqueue *vq, bool delayed);
	void	*(*detach_unused_buf)(struct virtqueue *vq);
	unsigned (*get_vring_size)(struct virtqueue *vq);
	int	(*get_queue_index)(struct virtqueue *vq);
};

/**
 * virtqueue - a queue to register buffers for sending or receiving.
 * @list: the chain of virtqueues for this device
 * @callback: the function to call when buffers are consumed (can be NULL).
 * @name: the name of this virtqueue (mainly for debugging)
 * @vdev: the virtio device this queue was created for.
 * @priv: a pointer for the virtqueue implementation to use.
 * @index: the zero-based ordinal number for this queue.
 * @num_free: number of elements we expect to be able to fit.
 * @driver_data: a pointer for the virtqueue user to use.
 *
 * A note on @num_free: with indirect buffers, each buffer needs one
 * element in the queue, otherwise a buffer will need one element per
 * sg element.
 */
struct virtqueue {
	struct list_head list;
	void (*callback)(struct virtqueue *vq);
	const char *name;
	struct virtio_device *vdev;
	unsigned int index;
	unsigned int num_free;
	void *priv;
	void *driver_data;
	struct virtqueue_ops *ops;
};

/**
 * virtqueue_add_buf_flags - expose buffer to other end
 * @vq: the struct virtqueue we're talking about.
 * @sg: the description of the buffer(s).
 * @out_num: the number of sg readable by other side
 * @in_num: the number of sg which are writable (after readable ones)
 * @data: the token identifying the buffer.
 * @flags: optional flags to pass in to virtqueue
 * @gfp: how to do memory allocations (if necessary).
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 *
 * Returns remaining capacity of queue or a negative error
 * (ie. ENOSPC).  Note that it only really makes sense to treat all
 * positive return values as "available": indirect buffers mean that
 * we can put an entire sg[] array inside a single queue entry.
 */
static inline int virtqueue_add_buf_flags(struct virtqueue *vq,
					  struct scatterlist sg[],
					  unsigned int out_num,
					  unsigned int in_num,
					  void *data, unsigned flags,
					  gfp_t gfp)
{
	if (vq && vq->ops && vq->ops->add_buf)
		return vq->ops->add_buf(vq, sg, out_num, in_num, data, flags,
					gfp);
	return -ENOTSUPP;
}

/**
 * virtqueue_add_buf - expose buffer to other end
 * @vq: the struct virtqueue we're talking about.
 * @sg: the description of the buffer(s).
 * @out_num: the number of sg readable by other side
 * @in_num: the number of sg which are writable (after readable ones)
 * @data: the token identifying the buffer.
 * @gfp: how to do memory allocations (if necessary).
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 *
 * Returns remaining capacity of queue or a negative error
 * (ie. ENOSPC).  Note that it only really makes sense to treat all
 * positive return values as "available": indirect buffers mean that
 * we can put an entire sg[] array inside a single queue entry.
 */
static inline int virtqueue_add_buf(struct virtqueue *vq,
				    struct scatterlist sg[],
				    unsigned int out_num, unsigned int in_num,
				    void *data, gfp_t gfp)
{
	return virtqueue_add_buf_flags(vq, sg, out_num, in_num, data, 0, gfp);
}

/**
 * virtqueue_kick_prepare - first half of split virtqueue_kick call.
 * @vq: the struct virtqueue
 *
 * Instead of virtqueue_kick(), you can do:
 *	if (virtqueue_kick_prepare(vq))
 *		virtqueue_notify(vq);
 *
 * This is sometimes useful because the virtqueue_kick_prepare() needs
 * to be serialized, but the actual virtqueue_notify() call does not.
 */
static inline bool virtqueue_kick_prepare(struct virtqueue *vq)
{
	if (vq && vq->ops && vq->ops->kick_prepare)
		return vq->ops->kick_prepare(vq);
	return true;
}

/**
 * virtqueue_notify - second half of split virtqueue_kick call.
 * @vq: the struct virtqueue
 *
 * This does not need to be serialized.
 */
static inline void virtqueue_notify(struct virtqueue *vq)
{
	if (vq && vq->ops && vq->ops->notify)
		vq->ops->notify(vq);
}

/**
 * virtqueue_kick - update after add_buf
 * @vq: the struct virtqueue
 *
 * After one or more virtqueue_add_buf calls, invoke this to kick
 * the other side.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 */
static inline void virtqueue_kick(struct virtqueue *vq)
{
	if (virtqueue_kick_prepare(vq))
		virtqueue_notify(vq);
}

/**
 * virtqueue_get_buf - get the next used buffer
 * @vq: the struct virtqueue we're talking about.
 * @len: the length written into the buffer
 *
 * If the driver wrote data into the buffer, @len will be set to the
 * amount written.  This means you don't need to clear the buffer
 * beforehand to ensure there's no data leakage in the case of short
 * writes.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 *
 * Returns NULL if there are no used buffers, or the "data" token
 * handed to virtqueue_add_buf().
 */
static inline void *virtqueue_get_buf(struct virtqueue *vq, unsigned int *len)
{
	if (vq && vq->ops && vq->ops->get_buf)
		return vq->ops->get_buf(vq, len);
	return NULL;
}

/**
 * virtqueue_disable_cb - disable callbacks
 * @vq: the struct virtqueue we're talking about.
 *
 * Note that this is not necessarily synchronous, hence unreliable and only
 * useful as an optimization.
 *
 * Unlike other operations, this need not be serialized.
 */
static inline void virtqueue_disable_cb(struct virtqueue *vq)
{
	if (vq && vq->ops && vq->ops->disable_cb)
		vq->ops->disable_cb(vq);
}

/**
 * virtqueue_enable_cb - restart callbacks after disable_cb.
 * @vq: the struct virtqueue we're talking about.
 *
 * This re-enables callbacks; it returns "false" if there are pending
 * buffers in the queue, to detect a possible race between the driver
 * checking for more work, and enabling callbacks.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 */
static inline bool virtqueue_enable_cb(struct virtqueue *vq)
{
	if (vq && vq->ops && vq->ops->enable_cb)
		return vq->ops->enable_cb(vq, false);
	return false;
}

/**
 * virtqueue_enable_cb_delayed - restart callbacks after disable_cb.
 * @vq: the struct virtqueue we're talking about.
 *
 * This re-enables callbacks but hints to the other side to delay
 * interrupts until most of the available buffers have been processed;
 * it returns "false" if there are many pending buffers in the queue,
 * to detect a possible race between the driver checking for more work,
 * and enabling callbacks.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 */
static inline bool virtqueue_enable_cb_delayed(struct virtqueue *vq)
{
	if (vq && vq->ops && vq->ops->enable_cb)
		return vq->ops->enable_cb(vq, true);
	return false;
}

/**
 * virtqueue_detach_unused_buf - detach first unused buffer
 * @vq: the struct virtqueue we're talking about.
 *
 * Returns NULL or the "data" token handed to virtqueue_add_buf().
 * This is not valid on an active queue; it is useful only for device
 * shutdown.
 */
static inline void *virtqueue_detach_unused_buf(struct virtqueue *vq)
{
	if (vq && vq->ops && vq->ops->detach_unused_buf)
		return vq->ops->detach_unused_buf(vq);
	return NULL;
}

/**
 * virtqueue_get_vring_size - return the size of the virtqueue's vring
 * @vq: the struct virtqueue containing the vring of interest.
 *
 * Returns the size of the vring.  This is mainly used for boasting to
 * userspace.  Unlike other operations, this need not be serialized.
 */
static inline unsigned virtqueue_get_vring_size(struct virtqueue *vq)
{
	if (vq && vq->ops && vq->ops->get_vring_size)
		return vq->ops->get_vring_size(vq);
	return 0;
}

/* FIXME: Obsolete accessor, but required for virtio_net merge. */
static inline unsigned int virtqueue_get_queue_index(struct virtqueue *vq)
{
	return vq->index;
}

static inline void *virtqueue_get_drvdata(const struct virtqueue *vq)
{
	return vq->driver_data;
}

static inline void virtqueue_set_drvdata(struct virtqueue *vq, void *data)
{
	vq->driver_data = data;
}

/**
 * virtio_device - representation of a device using virtio
 * @index: unique position on the virtio bus
 * @dev: underlying device.
 * @id: the device type identification (used to match it with a driver).
 * @config: the configuration ops for this device.
 * @vqs: the list of virtqueues for this device.
 * @features: the features supported by both driver and device.
 * @priv: private pointer for the driver's use.
 */
struct virtio_device {
	int index;
	struct device dev;
	struct virtio_device_id id;
	struct virtio_config_ops *config;
	struct list_head vqs;
	/* Note that this is a Linux set_bit-style bitmap. */
	unsigned long features[1];
	void *priv;
};

static inline struct virtio_device *dev_to_virtio(struct device *_dev)
{
	return container_of(_dev, struct virtio_device, dev);
}

int register_virtio_device(struct virtio_device *dev);
void unregister_virtio_device(struct virtio_device *dev);

/**
 * virtio_driver - operations for a virtio I/O driver
 * @driver: underlying device driver (populate name and owner).
 * @id_table: the ids serviced by this driver.
 * @feature_table: an array of feature numbers supported by this driver.
 * @feature_table_size: number of entries in the feature table array.
 * @probe: the function to call when a device is found.  Returns 0 or -errno.
 * @remove: the function to call when a device is removed.
 * @config_changed: optional function to call when the device configuration
 *    changes; may be called in interrupt context.
 */
struct virtio_driver {
	struct device_driver driver;
	const struct virtio_device_id *id_table;
	const unsigned int *feature_table;
	unsigned int feature_table_size;
	int (*probe)(struct virtio_device *dev);
	void (*scan)(struct virtio_device *dev);
	void (*remove)(struct virtio_device *dev);
	void (*config_changed)(struct virtio_device *dev);
#ifdef CONFIG_PM
	int (*freeze)(struct virtio_device *dev);
	int (*restore)(struct virtio_device *dev);
#endif
};

static inline struct virtio_driver *drv_to_virtio(struct device_driver *drv)
{
	return container_of(drv, struct virtio_driver, driver);
}

int register_virtio_driver(struct virtio_driver *drv);
void unregister_virtio_driver(struct virtio_driver *drv);

#define module_virtio_driver(__virtio_driver)	\
	module_driver(__virtio_driver,		\
		      register_virtio_driver,	\
		      unregister_virtio_driver)

static inline void *virtio_get_drvdata(const struct virtio_device *vdev)
{
	return dev_get_drvdata(&vdev->dev);
}

static inline void virtio_set_drvdata(struct virtio_device *vdev, void *data)
{
	dev_set_drvdata(&vdev->dev, data);
}

#endif /* _LINUX_VIRTIO_H */
