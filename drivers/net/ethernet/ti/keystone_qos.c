/*
 * Copyright (C) 2012 Texas Instruments Incorporated
 * Authors: Reece Pollack <reece@theptrgroup.com>
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

#include <linux/io.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/firmware.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/byteorder/generic.h>
#include <linux/platform_device.h>
#include <linux/keystone-dma.h>
#include <linux/errqueue.h>

#include "keystone_net.h"

#define	QOS_TXHOOK_ORDER	20

#define	MAX_CHANNELS	8

struct qos_channel {
	const char		*tx_chan_name;
	u32			 tx_queue_depth;
	struct netcp_tx_pipe	 tx_pipe;
};

struct qos_device {
	struct netcp_device		*netcp_device;
	struct net_device		*net_device;		/* FIXME */
	struct device			*dev;
	int				 num_channels;
	struct qos_channel		 channels[MAX_CHANNELS];
	struct device_node		*node;
	u32				 multi_if;
};

static int qos_tx_hook(int order, void *data, struct netcp_packet *p_info)
{
	struct qos_device *qos_dev = data;
	struct sk_buff *skb = p_info->skb;

	dev_dbg(qos_dev->dev,
		"priority: %u, queue_mapping: %04x\n",
		skb->priority, skb_get_queue_mapping(skb));

	if (skb->queue_mapping < qos_dev->num_channels)
		p_info->tx_pipe = &qos_dev->channels[skb->queue_mapping].tx_pipe;
	else {
		dev_warn(qos_dev->dev,
			"queue_mapping (%d) >= num_channels (%d),"
			" QoS bypassed\n",
			skb_get_queue_mapping(skb), qos_dev->num_channels);
	}

	return 0;
}


static int qos_close(void *intf_priv, struct net_device *ndev)
{
	struct qos_device *qos_dev = intf_priv;
	struct netcp_priv *netcp_priv = netdev_priv(ndev);
	int i;

	netcp_unregister_txhook(netcp_priv, QOS_TXHOOK_ORDER, qos_tx_hook, qos_dev);

	for (i = 0; i < qos_dev->num_channels; ++i) {
		struct qos_channel *qchan = &qos_dev->channels[i];

		netcp_txpipe_close(&qchan->tx_pipe);
	}

	return 0;
}

static int qos_open(void *intf_priv, struct net_device *ndev)
{
	struct qos_device *qos_dev = intf_priv;
	struct netcp_priv *netcp_priv = netdev_priv(ndev);
	int ret;
	int i;

	/* Open the QoS input queues */
	for (i = 0; i < qos_dev->num_channels; ++i) {
		struct qos_channel *qchan = &qos_dev->channels[i];

		ret = netcp_txpipe_open(&qchan->tx_pipe);
		if (ret)
			goto fail;
	}

	netcp_register_txhook(netcp_priv, QOS_TXHOOK_ORDER, qos_tx_hook, intf_priv);

	return 0;

fail:
	qos_close(intf_priv, ndev);
	return ret;
}


static int qos_attach(void *inst_priv, struct net_device *ndev, void **intf_priv)
{
	struct netcp_priv *netcp = netdev_priv(ndev);
	struct qos_device *qos_dev = inst_priv;
	char node_name[24];
	int i;

	snprintf(node_name, sizeof(node_name), "interface-%d",
		 (qos_dev->multi_if) ? (netcp->cpsw_port - 1) : 0);

	qos_dev->net_device = ndev;
	*intf_priv = qos_dev;

	if (of_find_property(qos_dev->node, node_name, NULL)) {
		/* Initialize the QoS input queues */
		for (i = 0; i < qos_dev->num_channels; ++i) {
			struct qos_channel *qchan = &qos_dev->channels[i];

			netcp_txpipe_init(&qchan->tx_pipe, netdev_priv(ndev),
					  qchan->tx_chan_name,
					  qchan->tx_queue_depth);

			qchan->tx_pipe.dma_psflags = netcp->cpsw_port;
		}
		return 0;
	} else
		return -ENODEV;
}

static int qos_release(void *intf_priv)
{
	struct qos_device *qos_dev = intf_priv;

	qos_dev->net_device = NULL;
	return 0;
}


static int qos_remove(struct netcp_device *netcp_device, void *inst_priv)
{
	struct qos_device *qos_dev = inst_priv;

	kfree(qos_dev);

	return 0;
}

static int init_channel(struct qos_device *qos_dev,
			int index,
			struct device_node *node)
{
	struct qos_channel *qchan = &qos_dev->channels[index];
	int ret;

	ret = of_property_read_string(node, "tx-channel", &qchan->tx_chan_name);
	if (ret < 0) {
		dev_err(qos_dev->dev, "missing \"tx-channel\" parameter, err %d\n", ret);
		qchan->tx_chan_name = "qos";
	}
	dev_dbg(qos_dev->dev, "tx-channel \"%s\"\n", qchan->tx_chan_name);

	ret = of_property_read_u32(node, "tx_queue_depth", &qchan->tx_queue_depth);
	if (ret < 0) {
		dev_err(qos_dev->dev, "missing \"tx_queue_depth\" parameter, err %d\n",	ret);
		qchan->tx_queue_depth = 16;
	}
	dev_dbg(qos_dev->dev, "tx_queue_depth %u\n", qchan->tx_queue_depth);

	return 0;
}

static int qos_probe(struct netcp_device *netcp_device,
		    struct device *dev,
		    struct device_node *node,
		    void **inst_priv)
{
	struct qos_device *qos_dev;
	struct device_node *channels, *channel;
	int ret = 0;

	if (!node) {
		dev_err(dev, "device tree info unavailable\n");
		return -ENODEV;
	}

	qos_dev = devm_kzalloc(dev, sizeof(struct qos_device), GFP_KERNEL);
	if (!qos_dev) {
		dev_err(dev, "memory allocation failed\n");
		return -ENOMEM;
	}
	*inst_priv = qos_dev;

	qos_dev->netcp_device = netcp_device;
	qos_dev->dev = dev;
	qos_dev->node = node;

	if (of_find_property(node, "multi-interface", NULL))
		qos_dev->multi_if = 1;

	channels = of_get_child_by_name(node, "input-channels");
	if (!channels) {
		dev_err(dev, "could not find \"input-channels\" in device tree\n");
		ret = -ENODEV;
		goto exit;
	}

	qos_dev->num_channels = 0;
	for_each_child_of_node(channels, channel) {
		if (qos_dev->num_channels >= MAX_CHANNELS) {
			dev_err(dev, "too many QoS input channels defined\n");
			break;
		}
		init_channel(qos_dev, qos_dev->num_channels, channel);
		++qos_dev->num_channels;
	}

	of_node_put(channels);

	return 0;

exit:
	qos_remove(netcp_device, qos_dev);
	*inst_priv = NULL;
	return ret;
}


static struct netcp_module qos_module = {
	.name		= "keystone-qos",
	.owner		= THIS_MODULE,
	.probe		= qos_probe,
	.open		= qos_open,
	.close		= qos_close,
	.remove		= qos_remove,
	.attach		= qos_attach,
	.release	= qos_release,
};

static int __init keystone_qos_init(void)
{
	return netcp_register_module(&qos_module);
}
module_init(keystone_qos_init);

static void __exit keystone_qos_exit(void)
{
	netcp_unregister_module(&qos_module);
}
module_exit(keystone_qos_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Reece Pollack <reece@theptrgroup.com");
MODULE_DESCRIPTION("Quality of Service driver for Keystone devices");