/*
 * Keystone crypto accelerator driver
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com
 * Contact: Sandeep Nair <sandeep_n@ti.com>
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

/* TODO:
 *  - Add support for all algorithms supported by SA
 *  - Add support for ABLKCIPHER & AHASH algorithms
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/dmapool.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/rtnetlink.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/keystone-dma.h>

#include <linux/crypto.h>
#include <linux/hw_random.h>
#include <linux/cryptohash.h>
#include <crypto/algapi.h>
#include <crypto/aead.h>
#include <crypto/authenc.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>
#include <crypto/aes.h>
#include <crypto/des.h>
#include <crypto/sha.h>
#include <crypto/md5.h>
#include <crypto/scatterwalk.h>

#include "keystone-sa.h"

/* Enable the below macro for testing with run-time
 * self tests in the cryptographic algorithm manager
 * framework */
/* #define TEST */

/* For enabling debug prints */
/* #define DEBUG */

/* Algorithm constants */
#define MD5_BLOCK_SIZE    64
#define AES_XCBC_DIGEST_SIZE	16

/* Number of 32 bit words in EPIB  */
#define SA_DMA_NUM_EPIB_WORDS	4
/* Number of 32 bit words in PS data  */
#define SA_DMA_NUM_PS_WORDS	16

/* Maximum number of simultaeneous security contexts
 * supported by the driver */
#define SA_MAX_NUM_CTX	512

/* Encoding used to identify the typo of crypto operation
 * performed on the packet when the packet is returned
 * by SA
 */
#define SA_REQ_SUBTYPE_ENC	0x0001
#define SA_REQ_SUBTYPE_DEC	0x0002
#define SA_REQ_SUBTYPE_SHIFT	16
#define SA_REQ_SUBTYPE_MASK	0xffff

/* Maximum size of authentication tag
 * NOTE: update this macro as we start supporting
 * algorithms with bigger digest size
 */
#define SA_MAX_AUTH_TAG_SZ SHA1_DIGEST_SIZE

static struct device *keystone_dev;

/* DMA channel configuration */
struct sa_dma_data {
	struct dma_chan	*rx_chan;
	struct dma_chan	*tx_chan;
	const char	*rx_chan_name;
	const char	*tx_chan_name;
	u32		tx_queue_depth;
	u32		rx_queue_depths[KEYSTONE_QUEUES_PER_CHAN];
	u32		rx_buffer_sizes[KEYSTONE_QUEUES_PER_CHAN];
};

/* Memory map of the SA register set */
struct sa_mmr_regs {
	u32 PID;
	u32 EFUSE_EN;
	u32 CMD_STATUS;
	u32 BLKMGR_PA_BLKS;
	u32 PA_FLOWID;
	u32 CDMA_FLOWID;
	u32 PA_ENG_ID;
	u32 CDMA_ENG_ID;
	u8  RSVD0[224];
	u32 CTXCACH_CTRL;
	u32 CTXCACH_SC_PTR;
	u32 CTXCACH_SC_ID;
	u32 CTXCACH_MISSCNT;
};

/*
 * Register Overlay Structure for TRNG module
 */
struct sa_trng_regs {
	u32 TRNG_OUTPUT_L;
	u32 TRNG_OUTPUT_H;
	u32 TRNG_STATUS;
	u32 TRNG_INTMASK;
	u32 TRNG_INTACK;
	u32 TRNG_CONTROL;
	u32 TRNG_CONFIG;
	u32 TRNG_ALARMCNT;
	u32 TRNG_FROENABLE;
	u32 TRNG_FRODETUNE;
	u32 TRNG_ALARMMASK;
	u32 TRNG_ALARMSTOP;
	u32 TRNG_LFSR_L;
	u32 TRNG_LFSR_M;
	u32 TRNG_LFSR_H;
	u32 TRNG_COUNT;
	u32 TRNG_TEST;
};

struct sa_regs {
	struct sa_mmr_regs mmr;
};

/* Driver statistics */
struct sa_drv_stats {
	/* Number of data pkts dropped while submitting to CP_ACE */
	atomic_t tx_dropped;
	/* Number of tear-down pkts dropped while submitting to CP_ACE */
	atomic_t sc_tear_dropped;
	/* Number of crypto requests sent to CP_ACE */
	atomic_t tx_pkts;
	/* Number of crypto request completions received from CP_ACE */
	atomic_t rx_pkts;
};

/*
 * Minimum number of descriptors to be always
 * available in the Rx free queue
 */
#define SA_MIN_RX_DESCS	4

/* Crypto driver instance data */
struct keystone_crypto_data {
	struct platform_device	*pdev;
	struct clk		*clk;
	struct tasklet_struct	rx_task;
	struct dma_pool		*sc_pool;
	struct sa_regs		*regs;
	struct sa_trng_regs	*trng_regs;
	struct sa_dma_data	dma_data;
	struct hwrng		rng;

	/* lock for SC-ID allocation */
	spinlock_t		scid_lock;
	/* lock to prevent irq scheduling while dmaengine_submit() */
	spinlock_t		irq_lock;
	/* lock for reading random data from TRNG */
	spinlock_t		trng_lock;

	/* Kobjects */
	struct kobject		stats_kobj;

	/* Security context data */
	u16			sc_id_start;
	u16			sc_id_end;
	u16			sc_id;

	/* Bitmap to keep track of Security context ID's */
	unsigned long		ctx_bm[DIV_ROUND_UP(SA_MAX_NUM_CTX,
					BITS_PER_LONG)];
	/* Driver stats */
	struct sa_drv_stats	stats;

	/*
	 * Number of pkts pending crypto processing completion
	 * beyond which the driver will start dropping crypto
	 * requests.
	 */
	int			tx_thresh;

	/*
	 * Number of pkts pending crypto processing completion
	 */
	atomic_t		pend_compl;
};

/* Packet structure used in Rx */
#define SA_SGLIST_SIZE	(MAX_SKB_FRAGS + 2)
struct sa_packet {
	struct scatterlist		 sg[SA_SGLIST_SIZE];
	int				 sg_ents;
	struct keystone_crypto_data	*priv;
	struct dma_chan			*chan;
	struct dma_async_tx_descriptor	*desc;
	dma_cookie_t			 cookie;
	u32				 epib[SA_DMA_NUM_EPIB_WORDS];
	u32				 psdata[SA_DMA_NUM_PS_WORDS];
	struct completion		 complete;
	void				*data;
};

/* Command label updation info */
struct sa_cmdl_param_info {
	u16	index;
	u16	offset;
	u16	size;
};

/* Maximum length of Auxiliary data in 32bit words */
#define SA_MAX_AUX_DATA_WORDS	8

struct sa_cmdl_upd_info {
	u16	flags;
	u16	submode;
	struct sa_cmdl_param_info	enc_size;
	struct sa_cmdl_param_info	enc_size2;
	struct sa_cmdl_param_info	enc_offset;
	struct sa_cmdl_param_info	enc_iv;
	struct sa_cmdl_param_info	enc_iv2;
	struct sa_cmdl_param_info	aad;
	struct sa_cmdl_param_info	payload;
	struct sa_cmdl_param_info	auth_size;
	struct sa_cmdl_param_info	auth_size2;
	struct sa_cmdl_param_info	auth_offset;
	struct sa_cmdl_param_info	auth_iv;
	struct sa_cmdl_param_info	aux_key_info;
	u32				aux_key[SA_MAX_AUX_DATA_WORDS];
};

enum sa_submode {
	SA_MODE_GEN = 0,
	SA_MODE_CCM,
	SA_MODE_GCM,
	SA_MODE_GMAC
};

/* TFM Context info */

/* Number of 32bit words appended after the command label
 * in PSDATA to identify the crypto request context.
 * word-0: Request type
 * word-1: pointer to request
 */
#define SA_NUM_PSDATA_CTX_WORDS 2

/* Maximum size of Command label in 32 words */
#define SA_MAX_CMDL_WORDS (SA_DMA_NUM_PS_WORDS - SA_NUM_PSDATA_CTX_WORDS)

struct sa_ctx_info {
	u8		*sc;
	dma_addr_t	sc_phys;
	u16		sc_id;
	u16		cmdl_size;
	u32		cmdl[SA_MAX_CMDL_WORDS];
	struct sa_cmdl_upd_info cmdl_upd_info;
	/* Store Auxiliary data such as K2/K3 subkeys in AES-XCBC */
	u32		epib[SA_DMA_NUM_EPIB_WORDS];
	struct dma_chan *rx_chan;
};

struct sa_tfm_ctx {
	struct keystone_crypto_data *dev_data;
	struct sa_ctx_info enc;
	struct sa_ctx_info dec;
	struct sa_ctx_info auth;
};

/* Tx DMA callback param */
struct sa_dma_req_ctx {
	struct keystone_crypto_data *dev_data;
	u32		cmdl[SA_MAX_CMDL_WORDS];
	unsigned	map_idx;
	struct sg_table sg_tbl;
	dma_cookie_t	cookie;
	struct dma_chan *tx_chan;
	bool		pkt;
};

/************************************************************/
/* Security context utilities                               */
/************************************************************/

/* Encryption algorithms */
enum sa_ealg_id {
	SA_EALG_ID_NONE = 0,        /* No encryption */
	SA_EALG_ID_NULL,            /* NULL encryption */
	SA_EALG_ID_AES_CTR,         /* AES Counter mode */
	SA_EALG_ID_AES_F8,          /* AES F8 mode */
	SA_EALG_ID_AES_CBC,         /* AES CBC mode */
	SA_EALG_ID_DES_CBC,         /* DES CBC mode */
	SA_EALG_ID_3DES_CBC,        /* 3DES CBC mode */
	SA_EALG_ID_CCM,             /* Counter with CBC-MAC mode */
	SA_EALG_ID_GCM,             /* Galois Counter mode */
	SA_EALG_ID_LAST
};

/* Authentication algorithms */
enum sa_aalg_id {
	SA_AALG_ID_NONE = 0,               /* No Authentication  */
	SA_AALG_ID_NULL = SA_EALG_ID_LAST, /* NULL Authentication  */
	SA_AALG_ID_MD5,                    /* MD5 mode */
	SA_AALG_ID_SHA1,                   /* SHA1 mode */
	SA_AALG_ID_SHA2_224,               /* 224-bit SHA2 mode */
	SA_AALG_ID_SHA2_256,               /* 256-bit SHA2 mode */
	SA_AALG_ID_HMAC_MD5,               /* HMAC with MD5 mode */
	SA_AALG_ID_HMAC_SHA1,              /* HMAC with SHA1 mode */
	SA_AALG_ID_HMAC_SHA2_224,          /* HMAC with 224-bit SHA2 mode */
	SA_AALG_ID_HMAC_SHA2_256,          /* HMAC with 256-bit SHA2 mode */
	SA_AALG_ID_GMAC,                   /* Galois Message
					      Authentication Code mode */
	SA_AALG_ID_CMAC,                   /* Cipher-based Message
					      Authentication Code mode */
	SA_AALG_ID_CBC_MAC,                /* Cipher Block Chaining */
	SA_AALG_ID_AES_XCBC                /* AES Extended
					      Cipher Block Chaining */
};

/* Mode control engine algorithms used to index the
 * mode control instruction tables
 */
enum sa_eng_algo_id {
	SA_ENG_ALGO_ECB = 0,
	SA_ENG_ALGO_CBC,
	SA_ENG_ALGO_CFB,
	SA_ENG_ALGO_OFB,
	SA_ENG_ALGO_CTR,
	SA_ENG_ALGO_F8,
	SA_ENG_ALGO_GCM,
	SA_ENG_ALGO_GMAC,
	SA_ENG_ALGO_CCM,
	SA_ENG_ALGO_CMAC,
	SA_ENG_ALGO_CBCMAC,
	SA_NUM_ENG_ALGOS
};

struct sa_eng_info {
	u8	eng_id;
	u16	sc_size;
};

/************************************************************/
/* Begin: Encryption mode control instructions              */
/************************************************************/

/************************************************************
 * Note: The below tables are generated.
 * Do not update it manually.
 *
 * Note: This is a special version of MCI file with
 * 3GPP standard modes disabled.
************************************************************/

const uint8_t sa_eng_aes_enc_mci_tbl[11][3][27] = {
	{
		{
			0x21, 0x00, 0x00, 0x80, 0x8a, 0x04, 0xb7, 0x90, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x84, 0x8a, 0x04, 0xb7, 0x90, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x88, 0x8a, 0x04, 0xb7, 0x90, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x21, 0x00, 0x00, 0x18, 0x88, 0x0a, 0xaa, 0x4b, 0x7e,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x18, 0x88, 0x4a, 0xaa, 0x4b, 0x7e,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x18, 0x88, 0x8a, 0xaa, 0x4b, 0x7e,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x21, 0x00, 0x00, 0x80, 0x9a, 0x09, 0x94, 0x7c, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x84, 0x9a, 0x09, 0x94, 0x7c, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x88, 0x9a, 0x09, 0x94, 0x7c, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x21, 0x00, 0x00, 0x80, 0x9a, 0xa5, 0xb4, 0x60, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x84, 0x9a, 0xa5, 0xb4, 0x60, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x88, 0x9a, 0xa5, 0xb4, 0x60, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x21, 0x00, 0x00, 0x80, 0x9a, 0x8f, 0x54, 0x1b, 0x82,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x84, 0x9a, 0x8f, 0x54, 0x1b, 0x82,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x88, 0x9a, 0x8f, 0x54, 0x1b, 0x82,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x21, 0x00, 0x22, 0x3b, 0xa3, 0xfb, 0x19, 0x31, 0x91,
			0x80, 0xa5, 0xc3, 0xa8, 0x89, 0x9e, 0x10, 0x2c, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x22, 0x3b, 0xa3, 0xfb, 0x19, 0x31, 0x91,
			0x84, 0xa5, 0xc3, 0xa8, 0x89, 0x9e, 0x10, 0x2c, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x22, 0x3b, 0xa3, 0xfb, 0x19, 0x31, 0x91,
			0x88, 0xa5, 0xc3, 0xa8, 0x89, 0x9e, 0x10, 0x2c, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x61, 0x00, 0x44, 0x80, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x58, 0x2e, 0x0a, 0x90, 0x71, 0x41, 0x83, 0x9d, 0x63,
			0xaa, 0x0b, 0x7e, 0x9a, 0x78, 0x3a, 0xa3, 0x8b, 0x1e
		},
		{
			0x61, 0x00, 0x44, 0x84, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x58, 0x2e, 0x4a, 0x90, 0x71, 0x41, 0x83, 0x9d, 0x63,
			0xaa, 0x0b, 0x7e, 0x9a, 0x78, 0x3a, 0xa3, 0x8b, 0x1e
		},
		{
			0x61, 0x00, 0x44, 0x88, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x58, 0x2e, 0x8a, 0x90, 0x71, 0x41, 0x83, 0x9d, 0x63,
			0xaa, 0x0b, 0x7e, 0x9a, 0x78, 0x3a, 0xa3, 0x8b, 0x1e
		}
	},
	{
		{
			0x41, 0x00, 0x44, 0x80, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x14, 0x18, 0x39, 0xd4, 0xba, 0xa0, 0xb7, 0xe9, 0xa7,
			0x83, 0xaa, 0x38, 0xb5, 0xe0, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x41, 0x00, 0x44, 0x84, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x14, 0x18, 0x39, 0xd4, 0xba, 0xa0, 0xb7, 0xe9, 0xa7,
			0x83, 0xaa, 0x38, 0xb5, 0xe0, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x41, 0x00, 0x44, 0x88, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x14, 0x18, 0x39, 0xd4, 0xba, 0xa0, 0xb7, 0xe9, 0xa7,
			0x83, 0xaa, 0x38, 0xb5, 0xe0, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x61, 0x00, 0x66, 0x80, 0xa9, 0x8f, 0x80, 0xa9, 0xbe,
			0x80, 0xb9, 0x7e, 0x18, 0x28, 0x0a, 0x9b, 0xe5, 0xc3,
			0x80, 0xbd, 0x6c, 0x15, 0x1a, 0x8e, 0xb0, 0x00, 0x00
		},
		{
			0x61, 0x00, 0x66, 0x84, 0xa9, 0x8f, 0x84, 0xa9, 0xbe,
			0x84, 0xb9, 0x7e, 0x18, 0x28, 0x4a, 0x9b, 0xe5, 0xc3,
			0x84, 0xbd, 0x6c, 0x15, 0x1a, 0x8e, 0xb0, 0x00, 0x00
		},
		{
			0x61, 0x00, 0x66, 0x88, 0xa9, 0x8f, 0x88, 0xa9, 0xbe,
			0x88, 0xb9, 0x7e, 0x18, 0x28, 0x8a, 0x9b, 0xe5, 0xc3,
			0x88, 0xbd, 0x6c, 0x15, 0x1a, 0x8e, 0xb0, 0x00, 0x00
		}
	},
	{
		{
			0x41, 0x00, 0x00, 0xf1, 0x0d, 0x19, 0x10, 0x8d, 0x2c,
			0x12, 0x88, 0x08, 0xa6, 0x4b, 0x7e, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x41, 0x00, 0x00, 0xf1, 0x0d, 0x19, 0x10, 0x8d, 0x2c,
			0x12, 0x88, 0x48, 0xa6, 0x4b, 0x7e, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x41, 0x00, 0x00, 0xf1, 0x0d, 0x19, 0x10, 0x8d, 0x2c,
			0x12, 0x88, 0x88, 0xa6, 0x4b, 0x7e, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x01, 0x00, 0x11, 0x37, 0x91, 0x41, 0x80, 0x9a, 0x4c,
			0x97, 0xec, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x01, 0x00, 0x11, 0x37, 0x91, 0x41, 0x84, 0x9a, 0x4c,
			0x97, 0xec, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x01, 0x00, 0x11, 0x37, 0x91, 0x41, 0x88, 0x9a, 0x4c,
			0x97, 0xec, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	}
};

const uint8_t sa_eng_aes_dec_mci_tbl[11][3][27] = {
	{
		{
			0x31, 0x00, 0x00, 0x80, 0x8a, 0x04, 0xb7, 0x90, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x31, 0x00, 0x00, 0x84, 0x8a, 0x04, 0xb7, 0x90, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x31, 0x00, 0x00, 0x88, 0x8a, 0x04, 0xb7, 0x90, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x31, 0x00, 0x00, 0x80, 0x8a, 0xca, 0x98, 0xf4, 0x40,
			0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x31, 0x00, 0x00, 0x84, 0x8a, 0xca, 0x98, 0xf4, 0x40,
			0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x31, 0x00, 0x00, 0x88, 0x8a, 0xca, 0x98, 0xf4, 0x40,
			0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x21, 0x00, 0x00, 0x80, 0x9a, 0xc7, 0x44, 0x0b, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x84, 0x9a, 0xc7, 0x44, 0x0b, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x88, 0x9a, 0xc7, 0x44, 0x0b, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x21, 0x00, 0x00, 0x80, 0x9a, 0xa5, 0xb4, 0x60, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x84, 0x9a, 0xa5, 0xb4, 0x60, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x21, 0x00, 0x00, 0x88, 0x9a, 0xa5, 0xb4, 0x60, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x61, 0x00, 0x44, 0x80, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x58, 0x2e, 0x0a, 0x14, 0x19, 0x07, 0x83, 0x9d, 0x63,
			0xaa, 0x0b, 0x7e, 0x9a, 0x78, 0x3a, 0xa3, 0x8b, 0x1e
		},
		{
			0x61, 0x00, 0x44, 0x84, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x58, 0x2e, 0x4a, 0x14, 0x19, 0x07, 0x83, 0x9d, 0x63,
			0xaa, 0x0b, 0x7e, 0x9a, 0x78, 0x3a, 0xa3, 0x8b, 0x1e
		},
		{
			0x61, 0x00, 0x44, 0x88, 0xa9, 0xfe, 0x83, 0x99, 0x7e,
			0x58, 0x2e, 0x8a, 0x14, 0x19, 0x07, 0x83, 0x9d, 0x63,
			0xaa, 0x0b, 0x7e, 0x9a, 0x78, 0x3a, 0xa3, 0x8b, 0x1e
		}
	},
	{
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x61, 0x00, 0x66, 0x80, 0xa9, 0x8f, 0x80, 0xa9, 0xbe,
			0x80, 0xb9, 0x7e, 0x5c, 0x3e, 0x0b, 0x90, 0x71, 0x82,
			0x80, 0xaa, 0x88, 0x9b, 0xed, 0x7c, 0x14, 0xac, 0x00
		},
		{
			0x61, 0x00, 0x66, 0x84, 0xa9, 0x8f, 0x84, 0xa9, 0xbe,
			0x84, 0xb9, 0x7e, 0x5c, 0x3e, 0x4b, 0x90, 0x71, 0x82,
			0x84, 0xaa, 0x88, 0x9b, 0xed, 0x7c, 0x14, 0xac, 0x00
		},
		{
			0x61, 0x00, 0x66, 0x88, 0xa9, 0x8f, 0x88, 0xa9, 0xbe,
			0x88, 0xb9, 0x7e, 0x5c, 0x3e, 0x8b, 0x90, 0x71, 0x82,
			0x88, 0xaa, 0x88, 0x9b, 0xed, 0x7c, 0x14, 0xac, 0x00
		}
	},
	{
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	},
	{
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		},
		{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}
	}
};

const uint8_t sa_eng_3des_enc_mci_tbl[4][27] = {
	{
		0x20, 0x00, 0x00, 0x85, 0x0a, 0x04, 0xb7, 0x90, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	},
	{
		0x20, 0x00, 0x00, 0x18, 0x88, 0x52, 0xaa, 0x4b, 0x7e, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	},
	{
		0x20, 0x00, 0x00, 0x85, 0x1a, 0x09, 0x94, 0x7c, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	},
	{
		0x20, 0x00, 0x00, 0x85, 0x1a, 0xa5, 0xb4, 0x60, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	}
};

const uint8_t sa_eng_3des_dec_mci_tbl[4][27] = {
	{
		0x30, 0x00, 0x00, 0x85, 0x0a, 0x04, 0xb7, 0x90, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	},
	{
		0x30, 0x00, 0x00, 0x85, 0x0a, 0xca, 0x98, 0xf4, 0x40, 0xc0,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	},
	{
		0x20, 0x00, 0x00, 0x85, 0x1a, 0xc7, 0x44, 0x0b, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	},
	{
		0x20, 0x00, 0x00, 0x85, 0x1a, 0xa5, 0xb4, 0x60, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	}
};

/************************************************************/
/* End: Encryption mode control instructions                */
/************************************************************/

/************************************************************/
/* Utility functions                                        */
/************************************************************/

/* Perform 16 byte swizzling */
static void sa_swiz_128(u8 *in, u8 *out, u16 len)
{
	u8 data[16];
	int i, j;

	for (i = 0; i < len; i += 16) {
		memcpy(data, &in[i], 16);
		for (j = 0; j < 16; j++)
			out[i + j] = data[15 - j];
	}
}

/* Convert CRA name to internal algorithm ID */
static void sa_conv_calg_to_salg(const char *cra_name,
				int *ealg_id, int *aalg_id)
{
	*ealg_id = SA_EALG_ID_NONE;
	*aalg_id = SA_AALG_ID_NONE;

	if (!strcmp(cra_name, "authenc(hmac(sha1),cbc(aes))")) {
		*ealg_id = SA_EALG_ID_AES_CBC;
		*aalg_id = SA_AALG_ID_HMAC_SHA1;
	} else if (!strcmp(cra_name, "authenc(hmac(sha1),cbc(des3_ede))")) {
		*ealg_id = SA_EALG_ID_3DES_CBC;
		*aalg_id = SA_AALG_ID_HMAC_SHA1;
	} else if (!strcmp(cra_name, "authenc(xcbc(aes),cbc(aes))")) {
		*ealg_id = SA_EALG_ID_AES_CBC;
		*aalg_id = SA_AALG_ID_AES_XCBC;
	} else if (!strcmp(cra_name, "authenc(xcbc(aes),cbc(des3_ede))")) {
		*ealg_id = SA_EALG_ID_3DES_CBC;
		*aalg_id = SA_AALG_ID_AES_XCBC;
	} else if (!strcmp(cra_name, "cbc(aes)")) {
		*ealg_id = SA_EALG_ID_AES_CBC;
	} else if (!strcmp(cra_name, "cbc(des3_ede)")) {
		*ealg_id = SA_EALG_ID_3DES_CBC;
	} else if (!strcmp(cra_name, "hmac(sha1)")) {
		*aalg_id = SA_AALG_ID_HMAC_SHA1;
	} else if (!strcmp(cra_name, "xcbc(aes)")) {
		*aalg_id = SA_AALG_ID_AES_XCBC;
	}
	return;
}

/* Given an algorithm ID get the engine details */
static void sa_get_engine_info
(
	int			alg_id,
	struct sa_eng_info	*info
)
{
	switch (alg_id) {
	case SA_EALG_ID_AES_CBC:
	case SA_EALG_ID_3DES_CBC:
	case SA_EALG_ID_DES_CBC:
		info->eng_id = SA_ENG_ID_EM1;
		info->sc_size = SA_CTX_ENC_TYPE1_SZ;
		break;

	case SA_AALG_ID_HMAC_SHA1:
	case SA_AALG_ID_HMAC_MD5:
		info->eng_id = SA_ENG_ID_AM1;
		info->sc_size = SA_CTX_AUTH_TYPE2_SZ;
		break;

	case SA_AALG_ID_AES_XCBC:
	case SA_AALG_ID_CMAC:
		info->eng_id = SA_ENG_ID_EM1;
		info->sc_size = SA_CTX_AUTH_TYPE1_SZ;
		break;

	default:
		dev_err(keystone_dev, "%s: unsupported algo\n", __func__);
		info->eng_id = SA_ENG_ID_NONE;
		info->sc_size = 0;
		break;
	}
	return;
}

/* Given an algorithm get the hash size */
static int sa_get_hash_size(u16 aalg_id)
{
	int hash_size = 0;

	switch (aalg_id) {
	case SA_AALG_ID_MD5:
	case SA_AALG_ID_HMAC_MD5:
		hash_size = MD5_DIGEST_SIZE;
		break;

	case SA_AALG_ID_SHA1:
	case SA_AALG_ID_HMAC_SHA1:
		hash_size = SHA1_DIGEST_SIZE;
		break;

	case SA_AALG_ID_SHA2_224:
	case SA_AALG_ID_HMAC_SHA2_224:
		hash_size = SHA224_DIGEST_SIZE;
		break;

	case SA_AALG_ID_SHA2_256:
	case SA_AALG_ID_HMAC_SHA2_256:
		hash_size = SHA256_DIGEST_SIZE;
		break;

	case SA_AALG_ID_AES_XCBC:
	case SA_AALG_ID_CMAC:
		hash_size = AES_BLOCK_SIZE;
		break;

	default:
		dev_err(keystone_dev, "%s: unsupported hash\n", __func__);
		break;
	}

	return hash_size;
}

/* Initialize MD5 digest */
static inline void md5_init(u32 *hash)
{
	/* Load magic initialization constants */
	hash[0] = 0x67452301;
	hash[1] = 0xefcdab89;
	hash[2] = 0x98badcfe;
	hash[3] = 0x10325476;
}

/* Generate HMAC-MD5 intermediate Hash */
void sa_hmac_md5_get_pad(const u8 *key, u16 key_sz, u32 *ipad, u32 *opad)
{
	u8 k_ipad[MD5_MESSAGE_BYTES];
	u8 k_opad[MD5_MESSAGE_BYTES];
	int i;

	for (i = 0; i < key_sz; i++) {
		k_ipad[i] = key[i] ^ 0x36;
		k_opad[i] = key[i] ^ 0x5c;
	}
	/* Instead of XOR with 0 */
	for (; i < SHA_MESSAGE_BYTES; i++) {
		k_ipad[i] = 0x36;
		k_opad[i] = 0x5c;
	}

	/* SHA-1 on k_ipad */
	md5_init(ipad);
	md5_transform(ipad, (u32 *)k_ipad);

	/* SHA-1 on k_opad */
	md5_init(opad);
	md5_transform(ipad, (u32 *)k_opad);
	return;
}

/* Generate HMAC-SHA1 intermediate Hash */
void sa_hmac_sha1_get_pad(const u8 *key, u16 key_sz, u32 *ipad, u32 *opad)
{
	u32 ws[SHA_WORKSPACE_WORDS];
	u8 k_ipad[SHA_MESSAGE_BYTES];
	u8 k_opad[SHA_MESSAGE_BYTES];
	int i;

	for (i = 0; i < key_sz; i++) {
		k_ipad[i] = key[i] ^ 0x36;
		k_opad[i] = key[i] ^ 0x5c;
	}
	/* Instead of XOR with 0 */
	for (; i < SHA_MESSAGE_BYTES; i++) {
		k_ipad[i] = 0x36;
		k_opad[i] = 0x5c;
	}

	/* SHA-1 on k_ipad */
	sha_init(ipad);
	sha_transform(ipad, k_ipad, ws);

	for (i = 0; i < SHA_DIGEST_WORDS; i++)
		ipad[i] = cpu_to_be32(ipad[i]);

	/* SHA-1 on k_opad */
	sha_init(opad);
	sha_transform(opad, k_opad, ws);

	for (i = 0; i < SHA_DIGEST_WORDS; i++)
		opad[i] = cpu_to_be32(opad[i]);

	return;
}

/* Generate HMAC-SHA224 intermediate Hash */
void sa_hmac_sha224_get_pad(const u8 *key, u16 key_sz, u32 *ipad, u32 *opad)
{
}

/* Generate HMAC-SHA256 intermediate Hash */
void sa_hmac_sha256_get_pad(const u8 *key, u16 key_sz, u32 *ipad, u32 *opad)
{
}

/* Derive GHASH to be used in the GCM algorithm */
void sa_calc_ghash(const u8 *key, u16 key_sz, u8 *ghash)
{
}

/* Derive the inverse key used in AES-CBC decryption operation */
static inline int sa_aes_inv_key(u8 *inv_key, const u8 *key, u16 key_sz)
{
	struct crypto_aes_ctx ctx;
	int key_pos;

	if (crypto_aes_expand_key(&ctx, key, key_sz)) {
		dev_err(keystone_dev, "%s: bad key len(%d)\n",
					__func__, key_sz);
		return -1;
	}

	/* Refer the implementation of crypto_aes_expand_key()
	 * to understand the below logic
	 */
	switch (key_sz) {
	case AES_KEYSIZE_128:
	case AES_KEYSIZE_192:
		key_pos = key_sz + 24;
		break;

	case AES_KEYSIZE_256:
		key_pos = key_sz + 24 - 4;
		break;

	default:
		dev_err(keystone_dev, "%s: bad key len(%d)\n",
					__func__, key_sz);
		return -1;
	}

	memcpy(inv_key, &ctx.key_enc[key_pos], key_sz);
	return 0;
}

#define AES_MAXNR 14
struct asm_aes_key {
	unsigned int rd_key[4 * (AES_MAXNR + 1)];
	int rounds;
};

/* AES encryption functions defined in aes-armv4.S */
asmlinkage void AES_encrypt(const u8 *in, u8 *out, struct asm_aes_key *key);
asmlinkage int private_AES_set_encrypt_key(const unsigned char *user_key,
				const int bits, struct asm_aes_key *key);

/* Derive sub-key k1, k2 and k3 used in the AES XCBC MAC mode
 * detailed in RFC 3566
 */
static inline int sa_aes_xcbc_subkey(u8 *sub_key1, u8 *sub_key2,
					u8 *sub_key3, const u8 *key,
					u16 key_sz)
{
	struct asm_aes_key enc_key;
	if (private_AES_set_encrypt_key(key, (key_sz * 8),
				&enc_key) == -1) {
		dev_err(keystone_dev, "%s: failed to set enc key\n", __func__);
		return -1;
	}

	if (sub_key1) {
		memset(sub_key1, 0x01, AES_BLOCK_SIZE);
		AES_encrypt(sub_key1, sub_key1, &enc_key);
	}

	if (sub_key2) {
		memset(sub_key2, 0x02, AES_BLOCK_SIZE);
		AES_encrypt(sub_key2, sub_key2, &enc_key);
	}

	if (sub_key3) {
		memset(sub_key3, 0x03, AES_BLOCK_SIZE);
		AES_encrypt(sub_key3, sub_key3, &enc_key);
	}

	return 0;
}

/************************************************************/
/*		SG list utility functions		    */
/************************************************************/

/* Number of elements in scatterlist */
static int sg_count(struct scatterlist *sg, int len)
{
	int sg_nents = 0;

	while (len > 0) {
		sg_nents++;
		len -= sg->length;
		sg = scatterwalk_sg_next(sg);
	}
	return sg_nents;
}

/*
 * buffer capacity of scatterlist
 */
static int sg_len(struct scatterlist *sg)
{
	int len = 0;

	while (sg) {
		len += sg->length;
		sg = sg_next(sg);
	}
	return len;
}

/* Clone SG list without copying the buffer */
static inline void sa_clone_sg(struct scatterlist *src,
		struct scatterlist *dst, unsigned int nbytes)
{
	for (; (nbytes > 0) && src && dst; ) {
		struct page *pg = sg_page(src);
		unsigned int len = min(nbytes, src->length);
		sg_set_page(dst, pg, len, src->offset);
		src = sg_next(src);
		dst = sg_next(dst);
		nbytes -= len;
	}
}

/* Copy buffer content from SRC SG list to DST SG list */
static int sg_copy(struct scatterlist *src, struct scatterlist *dst,
		unsigned int src_offset, unsigned int dst_offset, int len)
{
	struct scatter_walk walk;
	int sglen, cplen;

	sglen = sg_len(src);
	if (unlikely(len + src_offset > sglen)) {
		dev_err(keystone_dev, "src len(%d) less than (%d)\n",
					sglen, len + src_offset);
		return -1;
	}

	sglen = sg_len(dst);
	if (unlikely(len + dst_offset > sglen)) {
		dev_err(keystone_dev, "dst len(%d) less than (%d)\n",
					sglen, len + dst_offset);
		return -1;
	}

	scatterwalk_start(&walk, dst);
	scatterwalk_advance(&walk, dst_offset);
	while (src && (len > 0)) {
		cplen = min(len, (int)(src->length - src_offset));
		if (likely(cplen))
			scatterwalk_copychunks(sg_virt(src) +
						src_offset, &walk, cplen, 1);
		len -= cplen;
		src = sg_next(src);
		src_offset = 0;
	}
	scatterwalk_done(&walk, 1, 0);
	return 0;
}

/************************************************************/
/*		DMA notifcation handlers			*/
/************************************************************/

/* Tx completion callback */
static void sa_tx_dma_cb(void *data)
{
	struct sa_dma_req_ctx *ctx = data;
	enum dma_status status;

	if (unlikely(ctx->cookie <= 0))
		WARN(1, "invalid dma cookie == %d", ctx->cookie);
	else {
		status = dma_async_is_tx_complete(ctx->tx_chan,
				ctx->cookie, NULL, NULL);
		WARN((status != DMA_SUCCESS),
				"dma completion failure, status == %d", status);
	}

	dma_unmap_sg(&ctx->dev_data->pdev->dev,
		&ctx->sg_tbl.sgl[ctx->map_idx],
		ctx->sg_tbl.nents, DMA_TO_DEVICE);

	if (likely(ctx->sg_tbl.sgl))
		sg_free_table(&ctx->sg_tbl);

	if (likely(ctx->pkt)) {
		atomic_inc(&ctx->dev_data->pend_compl);
		atomic_inc(&ctx->dev_data->stats.tx_pkts);
	}

	kfree(ctx);
}

/* Rx completion callback */
static void sa_desc_rx_complete(void *arg)
{
	struct keystone_crypto_data *dev_data = NULL;
	struct device *dev = keystone_dev;
	struct sa_packet *rx = arg;
	struct scatterlist *sg;
	unsigned int alg_type;
	unsigned int frags;
	u32 req_sub_type;
	u32 *psdata;

	frags = 0;
	sg = sg_next(&rx->sg[2]);

	while ((frags < (SA_SGLIST_SIZE - 3)) && sg) {
		++frags;
		sg = sg_next(sg);
	}

	dma_unmap_sg(dev, &rx->sg[2], frags + 1, DMA_FROM_DEVICE);

	psdata = rx->psdata;
	alg_type = psdata[0] & CRYPTO_ALG_TYPE_MASK;
	req_sub_type = psdata[0] >> SA_REQ_SUBTYPE_SHIFT;

	if (likely(alg_type == CRYPTO_ALG_TYPE_AEAD)) {
		int auth_words, auth_size, iv_size, enc_len, enc_offset, i;
		struct aead_request *req;
		struct crypto_aead *tfm;
		int enc, err = 0;

		req = (struct aead_request *)psdata[1];
		tfm = crypto_aead_reqtfm(req);
		dev_data =
		((struct sa_tfm_ctx *)(crypto_tfm_ctx(&tfm->base)))->dev_data;
		auth_size = crypto_aead_authsize(tfm);
		iv_size = crypto_aead_ivsize(tfm);
		enc_offset = req->assoclen + iv_size;

		if (req_sub_type == SA_REQ_SUBTYPE_ENC) {
			enc_len = req->cryptlen;
			enc = 1;
		} else if (req_sub_type == SA_REQ_SUBTYPE_DEC) {
			enc_len = req->cryptlen - auth_size;
			enc = 0;
		} else {
			err = -EBADMSG;
			goto aead_err;
		}

		/* NOTE: We receive the tag as host endian 32bit words */
		auth_words = auth_size/sizeof(u32);

		for (i = 2; i < (auth_words + SA_NUM_PSDATA_CTX_WORDS); i++)
			psdata[i] = htonl(psdata[i]);

		/* if encryption, copy the authentication tag */
		if (enc) {
			scatterwalk_map_and_copy(
					&psdata[SA_NUM_PSDATA_CTX_WORDS],
					req->dst, enc_len,
					auth_size, 1);
#ifdef DEBUG
			dev_info(dev, "computed tag:\n");
			print_hex_dump(KERN_CONT, "", DUMP_PREFIX_OFFSET,
			16, 1, &psdata[SA_NUM_PSDATA_CTX_WORDS],
			auth_size, false);
#endif
		} else  {
			/* Verify the authentication tag */
			u8 auth_tag[SA_MAX_AUTH_TAG_SZ];
			scatterwalk_map_and_copy(auth_tag, req->src, enc_len,
					auth_size, 0);
			err = memcmp(&psdata[SA_NUM_PSDATA_CTX_WORDS],
					auth_tag, auth_size) ? -EBADMSG : 0;
			if (unlikely(err))
				goto aead_err;
#ifdef DEBUG
			dev_info(dev, "expected tag:\n");
			print_hex_dump(KERN_CONT, "", DUMP_PREFIX_OFFSET,
			16, 1, auth_tag, auth_size, false);
			dev_info(dev, "computed tag:\n");
			print_hex_dump(KERN_CONT, "", DUMP_PREFIX_OFFSET,
			16, 1, &psdata[SA_NUM_PSDATA_CTX_WORDS],
			auth_size, false);
#endif
		}

		/* Copy the encrypted/decrypted data */
		if (unlikely(sg_copy(&rx->sg[2], req->dst, enc_offset, 0,
					enc_len)))
			err = -EBADMSG;

aead_err:
		aead_request_complete(req, err);
	}

	/* Free the Rx buffer */
	sg = sg_next(&rx->sg[2]);
	while (sg) {
		free_page((unsigned long)sg_virt(sg));
		sg = sg_next(sg);
	}
	kfree(rx);

	/* update completion pending count */
	if (dev_data) {
		atomic_dec(&dev_data->pend_compl);
		atomic_inc(&dev_data->stats.rx_pkts);
	}
	return;
}

static void sa_desc_rx_complete2nd(void *data)
{
	WARN(1, "keystone-sa: Attempt to complete secondary receive buffer!\n");
}

/* Allocate receive buffer for Rx descriptors */
static struct dma_async_tx_descriptor *sa_rxpool_alloc(void *arg,
		unsigned q_num, unsigned bufsize)
{
	struct keystone_crypto_data *dev_data = arg;
	struct device *dev = &dev_data->pdev->dev;
	struct dma_async_tx_descriptor *desc;
	u32 err = 0;

	if (q_num == 0) {
		struct sa_packet *p_info;

		/* Allocate a primary receive queue entry */
		p_info = kmalloc(sizeof(*p_info) + bufsize, GFP_ATOMIC);
		if (unlikely(!p_info)) {
			dev_err(dev, "rx packet alloc failed\n");
			return NULL;
		}

		p_info->priv = dev_data;
		p_info->data = p_info + 1;
		p_info->chan = dev_data->dma_data.rx_chan;

		sg_init_table(p_info->sg, SA_SGLIST_SIZE);
		sg_set_buf(&p_info->sg[0], p_info->epib, sizeof(p_info->epib));
		sg_set_buf(&p_info->sg[1], p_info->psdata,
				sizeof(p_info->psdata));
		sg_set_buf(&p_info->sg[2], p_info->data, bufsize);

		p_info->sg_ents = 2 + dma_map_sg(dev, &p_info->sg[2], 1,
							DMA_FROM_DEVICE);
		if (unlikely(p_info->sg_ents != 3)) {
			dev_err(dev, "dma map failed\n");
			kfree(p_info);
			return NULL;
		}

		desc = dmaengine_prep_slave_sg(p_info->chan, p_info->sg,
					       3, DMA_DEV_TO_MEM,
					       DMA_HAS_EPIB | DMA_HAS_PSINFO);
		if (unlikely(IS_ERR_OR_NULL(desc))) {
			dma_unmap_sg(dev, &p_info->sg[2], 1, DMA_FROM_DEVICE);
			kfree(p_info);
			err = PTR_ERR(desc);
			if (err != -ENOMEM)
				dev_err(dev, "dma prep failed, error %d\n",
					err);
			return NULL;
		}

		desc->callback_param = p_info;
		desc->callback = sa_desc_rx_complete;
		p_info->cookie = desc->cookie;

	} else {

		/* Allocate a secondary receive queue entry */
		struct scatterlist sg[1];
		void *bufptr;

		bufptr = (void *)__get_free_page(GFP_ATOMIC);
		if (unlikely(!bufptr)) {
			dev_warn(dev, "page alloc failed for pool %d\n", q_num);
			return NULL;
		}

		sg_init_table(sg, 1);
		sg_set_buf(&sg[0], bufptr, PAGE_SIZE);

		err = dma_map_sg(dev, sg, 1, DMA_FROM_DEVICE);
		if (unlikely(err != 1)) {
			dev_warn(dev, "map error for pool %d\n", q_num);
			free_page((unsigned long)bufptr);
			return NULL;
		}

		desc = dmaengine_prep_slave_sg(dev_data->dma_data.rx_chan,
						sg, 1, DMA_DEV_TO_MEM,
						q_num << DMA_QNUM_SHIFT);
		if (unlikely(IS_ERR_OR_NULL(desc))) {
			dma_unmap_sg(dev, sg, 1, DMA_FROM_DEVICE);
			free_page((unsigned long)bufptr);

			err = PTR_ERR(desc);
			if (err != -ENOMEM)
				dev_err(dev, "dma prep failed, error %d\n",
						err);
			return NULL;
		}

		desc->callback_param = bufptr;
		desc->callback = sa_desc_rx_complete2nd;
	}

	return desc;
}

/* Release free receive buffer */
static void sa_rxpool_free(void *arg, unsigned q_num, unsigned bufsize,
		struct dma_async_tx_descriptor *desc)
{
	struct keystone_crypto_data *dev_data = arg;

	if (q_num == 0) {
		struct sa_packet *p_info = desc->callback_param;
		dma_unmap_sg(&dev_data->pdev->dev,
				&p_info->sg[2], 1, DMA_FROM_DEVICE);
		kfree(p_info);
	} else {
		void *bufptr = desc->callback_param;
		struct scatterlist sg[1];

		sg_init_table(sg, 1);
		sg_set_buf(&sg[0], bufptr, PAGE_SIZE);
		sg_dma_address(&sg[0]) =
			virt_to_dma(&dev_data->pdev->dev, bufptr);
		dma_unmap_sg(&dev_data->pdev->dev, sg, 1, DMA_FROM_DEVICE);
		free_page((unsigned long)bufptr);
	}
}

/* DMA channel rx notify callback */
static void sa_dma_notify_rx_compl(struct dma_chan *dma_chan, void *arg)
{
	struct keystone_crypto_data *dev_data = arg;

	dmaengine_pause(dev_data->dma_data.rx_chan);
	tasklet_schedule(&dev_data->rx_task);
	return;
}

/* Rx tast tasklet code */
static void sa_chan_work_handler(unsigned long data)
{
	struct keystone_crypto_data *crypto =
		(struct keystone_crypto_data *)data;

	dma_poll(crypto->dma_data.rx_chan, -1);
	dma_rxfree_refill(crypto->dma_data.rx_chan);
	dmaengine_resume(crypto->dma_data.rx_chan);
	return;
}

/* Setup DMA configurations */
static int sa_setup_dma(struct keystone_crypto_data *dev_data)
{
	struct dma_keystone_info config;
	dma_cap_mask_t mask;
	int error, i;
	struct sa_dma_data *dma_data = &dev_data->dma_data;
	struct device *dev = &dev_data->pdev->dev;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	/* Setup Tx DMA channel */
	dma_data->tx_chan =
		dma_request_channel_by_name(mask, dma_data->tx_chan_name);
	if (IS_ERR_OR_NULL(dma_data->tx_chan)) {
		dev_err(dev, "(%s) failed to open dmachan\n",
			dma_data->tx_chan_name);
		error = -ENODEV;
		goto err_out;
	}

	memset(&config, 0, sizeof(config));
	config.direction	= DMA_MEM_TO_DEV;
	config.tx_queue_depth	= dma_data->tx_queue_depth;

	error = dma_keystone_config(dma_data->tx_chan, &config);
	if (error < 0) {
		dev_err(dev, "(%s) failed to set keystone_config\n",
			dma_data->tx_chan_name);
		goto err_out;
	}

	/* Setup Rx DMA channel */
	dma_data->rx_chan =
		dma_request_channel_by_name(mask, dma_data->rx_chan_name);
	if (IS_ERR_OR_NULL(dma_data->rx_chan)) {
		dev_err(dev, "(%s) failed to open dmachan\n",
			dma_data->rx_chan_name);
		error = -ENODEV;
		goto err_out;
	}

	memset(&config, 0, sizeof(config));
	config.direction		= DMA_DEV_TO_MEM;
	config.scatterlist_size		= SA_SGLIST_SIZE;
	config.rxpool_allocator		= sa_rxpool_alloc;
	config.rxpool_destructor	= sa_rxpool_free;
	config.rxpool_param		= dev_data;
	config.rxpool_thresh_enable	= DMA_THRESH_NONE;

	for (i = 0; i < KEYSTONE_QUEUES_PER_CHAN &&
		    dma_data->rx_queue_depths[i] &&
		    dma_data->rx_buffer_sizes[i]; ++i) {
		config.rxpools[i].pool_depth  = dma_data->rx_queue_depths[i];
		config.rxpools[i].buffer_size = dma_data->rx_buffer_sizes[i];
		dev_dbg(dev, "rx_pool[%d] depth %d, size %d\n", i,
				config.rxpools[i].pool_depth,
				config.rxpools[i].buffer_size);
	}
	config.rxpool_count = i;

	error = dma_keystone_config(dma_data->rx_chan, &config);
	if (error < 0) {
		dev_err(dev, "(%s) failed to set keystone_config\n",
			dma_data->rx_chan_name);
		goto err_out;
	}

	dma_set_notify(dma_data->rx_chan, sa_dma_notify_rx_compl, dev_data);
	dma_rxfree_refill(dma_data->rx_chan);

	return 0;

err_out:
	if (dma_data->tx_chan)
		dma_release_channel(dma_data->tx_chan);
	if (dma_data->rx_chan)
		dma_release_channel(dma_data->rx_chan);

	return error;
}

/* Teardown DMA channels */
static void sa_teardown_dma(struct keystone_crypto_data *dev_data)
{
	struct sa_dma_data *dma_data = &dev_data->dma_data;

	if (dma_data->tx_chan) {
		dmaengine_pause(dma_data->tx_chan);
		dma_release_channel(dma_data->tx_chan);
		dma_data->tx_chan = NULL;
	}

	if (dma_data->rx_chan) {
		dmaengine_pause(dma_data->rx_chan);
		dma_release_channel(dma_data->rx_chan);
		dma_data->rx_chan = NULL;
	}

	return;
}

/******************************************************************************
 * Command Label Definitions and utility functions
 ******************************************************************************/
struct sa_cmdl_cfg {
	int	enc1st;
	int	aalg;
	u8	enc_eng_id;
	u8	auth_eng_id;
	u8	iv_size;
	const u8 *akey;
	u16	akey_len;
};

#define SA_CMDL_UPD_ENC		0x0001
#define SA_CMDL_UPD_AUTH	0x0002
#define SA_CMDL_UPD_ENC_IV	0x0004
#define SA_CMDL_UPD_AUTH_IV	0x0008
#define SA_CMDL_UPD_AUX_KEY	0x0010


/* Format general command label */
static int sa_format_cmdl_gen(struct sa_cmdl_cfg *cfg, u8 *cmdl,
				struct sa_cmdl_upd_info *upd_info)
{
	u8 offset = 0;
	u32 *word_ptr = (u32 *)cmdl;
	int i;

	/* Clear the command label */
	memset(cmdl, 0, (SA_MAX_CMDL_WORDS * sizeof(u32)));

	/* Iniialize the command update structure */
	memset(upd_info, 0, sizeof(*upd_info));
	upd_info->enc_size.offset = 2;
	upd_info->enc_size.size = 2;
	upd_info->enc_offset.size = 1;
	upd_info->enc_size2.size = 4;
	upd_info->auth_size.offset = 2;
	upd_info->auth_size.size = 2;
	upd_info->auth_offset.size = 1;

	if (cfg->aalg == SA_AALG_ID_AES_XCBC) {

		/* Derive K2/K3 subkeys */
		if (sa_aes_xcbc_subkey(NULL, (u8 *)&upd_info->aux_key[0],
			(u8 *)&upd_info->aux_key[AES_BLOCK_SIZE/sizeof(u32)],
			cfg->akey,
			cfg->akey_len))
			return -1;

		/* Format the key into 32bit CPU words
		 * from a big-endian stream */
		for (i = 0; i < SA_MAX_AUX_DATA_WORDS; i++)
			upd_info->aux_key[i] =
				be32_to_cpu(upd_info->aux_key[i]);
	}

	if (cfg->enc1st) {
		if (cfg->enc_eng_id != SA_ENG_ID_NONE) {
			upd_info->flags |= SA_CMDL_UPD_ENC;
			upd_info->enc_size.index = 0;
			upd_info->enc_offset.index = 1;

			if ((cfg->enc_eng_id == SA_ENG_ID_EM1) &&
					(cfg->auth_eng_id == SA_ENG_ID_EM1))
				cfg->auth_eng_id = SA_ENG_ID_EM2;

			/* Encryption command label */
			if (cfg->auth_eng_id != SA_ENG_ID_NONE)
				cmdl[SA_CMDL_OFFSET_NESC] = cfg->auth_eng_id;
			else
				cmdl[SA_CMDL_OFFSET_NESC] = SA_ENG_ID_OUTPORT2;

			/* Encryption modes requiring IV */
			if (cfg->iv_size) {
				upd_info->flags |= SA_CMDL_UPD_ENC_IV;
				upd_info->enc_iv.index =
					SA_CMDL_HEADER_SIZE_BYTES >> 2;
				upd_info->enc_iv.size = cfg->iv_size;

				cmdl[SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES +
					cfg->iv_size;

				cmdl[SA_CMDL_OFFSET_OPTION_CTRL1] =
					(SA_CTX_ENC_AUX2_OFFSET |
					 (cfg->iv_size >> 3));

				offset = SA_CMDL_HEADER_SIZE_BYTES +
						cfg->iv_size;
			} else {
				cmdl[SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES;
				offset = SA_CMDL_HEADER_SIZE_BYTES;
			}
		}

		if (cfg->auth_eng_id != SA_ENG_ID_NONE) {
			upd_info->flags |= SA_CMDL_UPD_AUTH;
			upd_info->auth_size.index = offset >> 2;
			upd_info->auth_offset.index =
				upd_info->auth_size.index + 1;

			cmdl[offset + SA_CMDL_OFFSET_NESC] = SA_ENG_ID_OUTPORT2;

			/* Algorithm with subkeys */
			if ((cfg->aalg == SA_AALG_ID_AES_XCBC) ||
				(cfg->aalg == SA_AALG_ID_CMAC)) {
				upd_info->flags |= SA_CMDL_UPD_AUX_KEY;
				upd_info->aux_key_info.index =
				(offset + SA_CMDL_HEADER_SIZE_BYTES) >> 2;

				cmdl[offset + SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES + 16;
				cmdl[offset + SA_CMDL_OFFSET_OPTION_CTRL1] =
					(SA_CTX_ENC_AUX1_OFFSET | (16 >> 3));

				offset += SA_CMDL_HEADER_SIZE_BYTES + 16;
			} else {
				cmdl[offset + SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES;
				offset += SA_CMDL_HEADER_SIZE_BYTES;
			}
		}
	} else {
		/* Auth first */
		if (cfg->auth_eng_id != SA_ENG_ID_NONE) {
			upd_info->flags |= SA_CMDL_UPD_AUTH;
			upd_info->auth_size.index = 0;
			upd_info->auth_offset.index = 1;

			if ((cfg->auth_eng_id == SA_ENG_ID_EM1) &&
				(cfg->enc_eng_id == SA_ENG_ID_EM1))
				cfg->enc_eng_id = SA_ENG_ID_EM2;

			/* Authentication command label */
			if (cfg->enc_eng_id != SA_ENG_ID_NONE)
				cmdl[SA_CMDL_OFFSET_NESC] = cfg->enc_eng_id;
			else
				cmdl[SA_CMDL_OFFSET_NESC] = SA_ENG_ID_OUTPORT2;

			/* Algorithm with subkeys */
			if ((cfg->aalg == SA_AALG_ID_AES_XCBC) ||
				(cfg->aalg == SA_AALG_ID_CMAC)) {
				upd_info->flags |= SA_CMDL_UPD_AUX_KEY;
				upd_info->aux_key_info.index =
					(SA_CMDL_HEADER_SIZE_BYTES) >> 2;

				cmdl[SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES + 16;
				cmdl[offset + SA_CMDL_OFFSET_OPTION_CTRL1] =
					(SA_CTX_ENC_AUX1_OFFSET | (16 >> 3));

				offset = SA_CMDL_HEADER_SIZE_BYTES + 16;
			} else {
				cmdl[SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES;
				offset = SA_CMDL_HEADER_SIZE_BYTES;
			}
		}

		if (cfg->enc_eng_id != SA_ENG_ID_NONE) {
			upd_info->flags |= SA_CMDL_UPD_ENC;
			upd_info->enc_size.index = offset >> 2;
			upd_info->enc_offset.index =
				upd_info->enc_size.index + 1;

			cmdl[offset + SA_CMDL_OFFSET_NESC] = SA_ENG_ID_OUTPORT2;

			/* Encryption modes requiring IV */
			if (cfg->iv_size) {
				upd_info->flags |= SA_CMDL_UPD_ENC_IV;
				upd_info->enc_iv.index =
				(offset + SA_CMDL_HEADER_SIZE_BYTES) >> 2;
				upd_info->enc_iv.size = cfg->iv_size;

				cmdl[offset + SA_CMDL_OFFSET_LABEL_LEN] =
				SA_CMDL_HEADER_SIZE_BYTES + cfg->iv_size;

				cmdl[offset + SA_CMDL_OFFSET_OPTION_CTRL1] =
				(SA_CTX_ENC_AUX2_OFFSET | (cfg->iv_size >> 3));

				offset += SA_CMDL_HEADER_SIZE_BYTES +
						cfg->iv_size;
			} else {
				cmdl[offset + SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES;
				offset += SA_CMDL_HEADER_SIZE_BYTES;
			}
		}
	}

	/* Roundup command label size to multiple of 8 bytes */
	offset = roundup(offset, 8);

	/* Format the Command label into 32bit CPU words
	 * from a big-endian stream */
	for (i = 0; i < offset/4; i++)
		word_ptr[i] = be32_to_cpu(word_ptr[i]);

	return offset;
}

/* Make 32-bit word from 4 bytes */
#define SA_MK_U32(b0, b1, b2, b3) (((b0) << 24) | ((b1) << 16) | \
					((b2) << 8) | (b3))

/* Update Command label */
static inline void sa_update_cmdl
(
	struct device *dev,
	u8	enc_offset,
	u16	enc_size,
	u8	*enc_iv,
	u8	auth_offset,
	u16	auth_size,
	u8	*auth_iv,
	u8	aad_size,
	u8	*aad,
	struct sa_cmdl_upd_info	*upd_info,
	u32	*cmdl
)
{
	switch (upd_info->submode) {
	case SA_MODE_GEN:
		if (likely(upd_info->flags & SA_CMDL_UPD_ENC)) {
			cmdl[upd_info->enc_size.index] &= 0xffff0000;
			cmdl[upd_info->enc_size.index] |= enc_size;
			cmdl[upd_info->enc_offset.index] &= 0x00ffffff;
			cmdl[upd_info->enc_offset.index] |=
						((u32)enc_offset << 24);

			if (likely(upd_info->flags & SA_CMDL_UPD_ENC_IV)) {
				u32 *data = &cmdl[upd_info->enc_iv.index];

				data[0] = SA_MK_U32(enc_iv[0], enc_iv[1],
							enc_iv[2], enc_iv[3]);
				data[1] = SA_MK_U32(enc_iv[4], enc_iv[5],
							enc_iv[6], enc_iv[7]);

				if (upd_info->enc_iv.size > 8) {
					data[2] = SA_MK_U32(enc_iv[8],
					enc_iv[9], enc_iv[10], enc_iv[11]);
					data[3] = SA_MK_U32(enc_iv[12],
					enc_iv[13], enc_iv[14], enc_iv[15]);
				}
			}
		}

		if (likely(upd_info->flags & SA_CMDL_UPD_AUTH)) {
			cmdl[upd_info->auth_size.index] &= 0xffff0000;
			cmdl[upd_info->auth_size.index] |= auth_size;
			cmdl[upd_info->auth_offset.index] &= 0x00ffffff;
			cmdl[upd_info->auth_offset.index] |=
					((u32)auth_offset << 24);

			if (upd_info->flags & SA_CMDL_UPD_AUTH_IV) {
				u32 *data = &cmdl[upd_info->auth_iv.index];

				data[0] = SA_MK_U32(auth_iv[0], auth_iv[1],
							auth_iv[2], auth_iv[3]);
				data[1] = SA_MK_U32(auth_iv[4], auth_iv[5],
							auth_iv[6], auth_iv[7]);

				if (upd_info->auth_iv.size > 8) {
					data[2] = SA_MK_U32(auth_iv[8],
					auth_iv[9], auth_iv[10], auth_iv[11]);
					data[3] = SA_MK_U32(auth_iv[12],
					auth_iv[13], auth_iv[14], auth_iv[15]);
				}
			}

			if (upd_info->flags & SA_CMDL_UPD_AUX_KEY) {
				int offset = (auth_size & 0xF) ? 4 : 0;
				memcpy(&cmdl[upd_info->aux_key_info.index],
						&upd_info->aux_key[offset], 16);
			}
		}
		break;

	case SA_MODE_CCM:
	case SA_MODE_GCM:
	case SA_MODE_GMAC:
	default:
		dev_err(dev, "unsupported mode(%d)\n", upd_info->submode);
		break;

	}
	return;
}

struct sa_swinfo {
	u32 word[3];
};

/* Format SWINFO words to be sent to SA */
static void sa_set_swinfo(u8 eng_id, u16 sc_id, dma_addr_t sc_phys,
		u8 cmdl_present, u8 cmdl_offset, u8 flags, u16 queue_id,
		u8 flow_id, u8 hash_size, struct sa_swinfo *swinfo)
{
	swinfo->word[0] = sc_id;
	swinfo->word[0] |= (flags << 16);
	if (likely(cmdl_present))
		swinfo->word[0] |= ((cmdl_offset | 0x10)  << 20);
	swinfo->word[0] |= (eng_id << 25);
	swinfo->word[0] |= 0x40000000;
	swinfo->word[1] = sc_phys;
	swinfo->word[2] = (queue_id | (flow_id << 16) | (hash_size << 24));
}

/******************************************************************************
 * Security context creation functions
 ******************************************************************************/

/* Set Security context for the encryption engine */
static int sa_set_sc_enc(u16 alg_id, const u8 *key, u16 key_sz,
				u16 aad_len, u8 enc, u8 *sc_buf)
{
/* Byte offset for key in encryption security context */
#define SC_ENC_KEY_OFFSET (1 + 27 + 4)
/* Byte offset for Aux-1 in encryption security context */
#define SC_ENC_AUX1_OFFSET (1 + 27 + 4 + 32)

	u8 ghash[16]; /* AES block size */
	const u8 *mci = NULL;
	/* Convert the key size (16/24/32) to the key size index (0/1/2) */
	int key_idx = (key_sz >> 3) - 2;

	/* Set Encryption mode selector to crypto processing */
	sc_buf[0] = 0;

	/* Select the mode control instruction */
	switch (alg_id) {
	case SA_EALG_ID_AES_CBC:
		mci = (enc) ? sa_eng_aes_enc_mci_tbl[SA_ENG_ALGO_CBC][key_idx] :
			sa_eng_aes_dec_mci_tbl[SA_ENG_ALGO_CBC][key_idx];
		break;

	case SA_EALG_ID_CCM:
		mci = (enc) ? sa_eng_aes_enc_mci_tbl[SA_ENG_ALGO_CCM][key_idx] :
			sa_eng_aes_dec_mci_tbl[SA_ENG_ALGO_CCM][key_idx];
		break;

	case SA_EALG_ID_AES_F8:
		mci = sa_eng_aes_enc_mci_tbl[SA_ENG_ALGO_F8][key_idx];
		break;

	case SA_EALG_ID_AES_CTR:
		mci = sa_eng_aes_enc_mci_tbl[SA_ENG_ALGO_CTR][key_idx];
		break;

	case SA_EALG_ID_GCM:
		mci = (enc) ? sa_eng_aes_enc_mci_tbl[SA_ENG_ALGO_GCM][key_idx] :
			sa_eng_aes_dec_mci_tbl[SA_ENG_ALGO_GCM][key_idx];
		/* Set AAD length at byte offset 23 in Aux-1 */
		sc_buf[SC_ENC_AUX1_OFFSET + 23] = (aad_len << 3);
		/* fall through to GMAC */

	case SA_AALG_ID_GMAC:
		sa_calc_ghash(key, (key_sz << 3), ghash);
		/* copy GCM Hash in Aux-1 */
		memcpy(&sc_buf[SC_ENC_AUX1_OFFSET], ghash, 16);
		break;

	case SA_AALG_ID_AES_XCBC:
	case SA_AALG_ID_CMAC:
		mci = sa_eng_aes_enc_mci_tbl[SA_ENG_ALGO_CMAC][key_idx];
		break;

	case SA_AALG_ID_CBC_MAC:
		mci = sa_eng_aes_enc_mci_tbl[SA_ENG_ALGO_CBCMAC][key_idx];
		break;

	case SA_EALG_ID_3DES_CBC:
		mci = (enc) ? sa_eng_3des_enc_mci_tbl[SA_ENG_ALGO_CBC] :
			sa_eng_3des_dec_mci_tbl[SA_ENG_ALGO_CBC];
		break;
	}

	/* Set the mode control instructions in security context */
	if (mci)
		memcpy(&sc_buf[1], mci, 27);

	/* For AES-CBC decryption get the inverse key */
	if ((alg_id == SA_EALG_ID_AES_CBC) && !enc) {
		if (sa_aes_inv_key(&sc_buf[SC_ENC_KEY_OFFSET], key, key_sz))
			return -1;
	}
	/* For AES-XCBC-MAC get the subkey */
	else if (alg_id == SA_AALG_ID_AES_XCBC) {
		if (sa_aes_xcbc_subkey(&sc_buf[SC_ENC_KEY_OFFSET], NULL,
					NULL, key, key_sz))
			return -1;
	}
	/* For all other cases: key is used */
	else
		memcpy(&sc_buf[SC_ENC_KEY_OFFSET], key, key_sz);

	return 0;
}

/* Set Security context for the authentication engine */
static void sa_set_sc_auth(u16 alg_id, const u8 *key, u16 key_sz, u8 *sc_buf)
{
	u32 ipad[8], opad[8];
	u8 mac_sz, keyed_mac = 0;

	/* Set Authentication mode selector to hash processing */
	sc_buf[0] = 0;

	/* Auth SW ctrl word: bit[6]=1 (upload computed hash to TLR section) */
	sc_buf[1] = 0x40;

	switch (alg_id) {
	case SA_AALG_ID_MD5:
		/* Auth SW ctrl word: bit[4]=1 (basic hash)
		 * bit[3:0]=1 (MD5 operation)*/
		sc_buf[1] |= (0x10 | 0x1);
		break;

	case SA_AALG_ID_SHA1:
		/* Auth SW ctrl word: bit[4]=1 (basic hash)
		 * bit[3:0]=2 (SHA1 operation)*/
		sc_buf[1] |= (0x10 | 0x2);
		break;

	case SA_AALG_ID_SHA2_224:
		/* Auth SW ctrl word: bit[4]=1 (basic hash)
		 * bit[3:0]=3 (SHA2-224 operation)*/
		sc_buf[1] |= (0x10 | 0x3);
		break;

	case SA_AALG_ID_SHA2_256:
		/* Auth SW ctrl word: bit[4]=1 (basic hash)
		 * bit[3:0]=4 (SHA2-256 operation)*/
		sc_buf[1] |= (0x10 | 0x4);
		break;

	case SA_AALG_ID_HMAC_MD5:
		/* Auth SW ctrl word: bit[4]=0 (HMAC)
		 * bit[3:0]=1 (MD5 operation)*/
		sc_buf[1] |= 0x1;
		keyed_mac = 1;
		mac_sz = MD5_DIGEST_SIZE;
		sa_hmac_md5_get_pad(key, key_sz, ipad, opad);
		break;

	case SA_AALG_ID_HMAC_SHA1:
		/* Auth SW ctrl word: bit[4]=0 (HMAC)
		 * bit[3:0]=2 (SHA1 operation)*/
		sc_buf[1] |= 0x2;
		keyed_mac = 1;
		mac_sz = SHA1_DIGEST_SIZE;
		sa_hmac_sha1_get_pad(key, key_sz, ipad, opad);
		break;

	case SA_AALG_ID_HMAC_SHA2_224:
		/* Auth SW ctrl word: bit[4]=0 (HMAC)
		 * bit[3:0]=3 (SHA2-224 operation)*/
		sc_buf[1] |= 0x3;
		keyed_mac = 1;
		mac_sz = SHA224_DIGEST_SIZE;
		sa_hmac_sha224_get_pad(key, key_sz, ipad, opad);
		break;

	case SA_AALG_ID_HMAC_SHA2_256:
		/* Auth SW ctrl word: bit[4]=0 (HMAC)
		 * bit[3:0]=4 (SHA2-256 operation)*/
		sc_buf[1] |= 0x4;
		keyed_mac = 1;
		mac_sz = SHA256_DIGEST_SIZE;
		sa_hmac_sha256_get_pad(key, key_sz, ipad, opad);
		break;
	}

	/* Copy the keys or ipad/opad */
	if (keyed_mac) {
		/* Copy ipad to AuthKey */
		memcpy(&sc_buf[32], ipad, mac_sz);
		/* Copy opad to Aux-1 */
		memcpy(&sc_buf[64], opad, mac_sz);
	}
}

/* Dump the security context */
static void sa_dump_sc(u8 *buf, u32 dma_addr)
{
#ifdef DEBUG
	dev_info(keystone_dev, "Security context dump for %p:\n",
			(void *)dma_addr);
	print_hex_dump(KERN_CONT, "", DUMP_PREFIX_OFFSET,
			16, 1, buf, SA_CTX_MAX_SZ, false);
#endif
	return;
}

/* size of SCCTL structure in bytes */
#define SA_SCCTL_SZ 8

/* Initialize Security context */
static int sa_init_sc(struct sa_ctx_info *ctx, const u8 *enc_key,
			u16 enc_key_sz, const u8 *auth_key, u16 auth_key_sz,
			const char *cra_name, u8 enc,
			struct sa_swinfo *swinfo)
{
	struct sa_eng_info enc_eng, auth_eng;
	int ealg_id, aalg_id, use_enc = 0;
	int enc_sc_offset, auth_sc_offset;
	u8 php_f, php_e, eng0_f, eng1_f;
	u8 *sc_buf = ctx->sc;
	u16 sc_id = ctx->sc_id;
	u16 aad_len = 0; /* Currently not supporting AEAD algo */
	u8 first_engine;
	u16 queue_id;
	u8 flow_id, hash_size;

	memset(sc_buf, 0, SA_CTX_MAX_SZ);
	sa_conv_calg_to_salg(cra_name, &ealg_id, &aalg_id);
	sa_get_engine_info(ealg_id, &enc_eng);
	sa_get_engine_info(aalg_id, &auth_eng);

	if (!enc_eng.sc_size && !auth_eng.sc_size)
		return -1;

	if (auth_eng.eng_id <= SA_ENG_ID_EM2)
		use_enc = 1;

	/* Determine the order of encryption & Authentication contexts */
	if (enc || !use_enc) {
		eng0_f = SA_CTX_SIZE_TO_DMA_SIZE(enc_eng.sc_size);
		eng1_f = SA_CTX_SIZE_TO_DMA_SIZE(auth_eng.sc_size);
		enc_sc_offset = SA_CTX_PHP_PE_CTX_SZ;
		auth_sc_offset = enc_sc_offset + enc_eng.sc_size;
	} else {
		eng0_f = SA_CTX_SIZE_TO_DMA_SIZE(auth_eng.sc_size);
		eng1_f = SA_CTX_SIZE_TO_DMA_SIZE(enc_eng.sc_size);
		auth_sc_offset = SA_CTX_PHP_PE_CTX_SZ;
		enc_sc_offset = auth_sc_offset + auth_eng.sc_size;
	}

	php_f = php_e = SA_CTX_DMA_SIZE_64;

	/* SCCTL Owner info: 0=host, 1=CP_ACE */
	sc_buf[SA_CTX_SCCTL_OWNER_OFFSET] = 0;
	/* SCCTL F/E control */
	sc_buf[1] = SA_CTX_SCCTL_MK_DMA_INFO(php_f, eng0_f, eng1_f, php_e);
	memcpy(&sc_buf[2], &sc_id, 2); /*(optional)
					 Filled here for reference only */
	memcpy(&sc_buf[4], &ctx->sc_phys, 4); /*(optional)
					Filled here for reference only */

	/* Initialize the rest of PHP context */
	memset(sc_buf + SA_SCCTL_SZ, 0, SA_CTX_PHP_PE_CTX_SZ - SA_SCCTL_SZ);

	/* Prepare context for encryption engine */
	if (enc_eng.sc_size) {
		if (sa_set_sc_enc(ealg_id, enc_key, enc_key_sz, aad_len,
				enc, &sc_buf[enc_sc_offset]))
			return -1;
	}

	/* Prepare context for authentication engine */
	if (auth_eng.sc_size) {
		if (use_enc) {
			if (sa_set_sc_enc(aalg_id, auth_key, auth_key_sz,
					aad_len, 0, &sc_buf[auth_sc_offset]))
				return -1;
		} else
			sa_set_sc_auth(aalg_id, auth_key, auth_key_sz,
					&sc_buf[auth_sc_offset]);
	}

	/* Set the ownership of context to CP_ACE */
	sc_buf[SA_CTX_SCCTL_OWNER_OFFSET] = 0x80;

	/* swizzle the security context */
	sa_swiz_128(sc_buf, sc_buf, SA_CTX_MAX_SZ);

	/* Setup SWINFO */
	first_engine = enc ? enc_eng.eng_id : auth_eng.eng_id;
	queue_id = dma_get_rx_queue(ctx->rx_chan);
	flow_id = dma_get_rx_flow(ctx->rx_chan);
	/* TODO: take care of AEAD algorithms */
	hash_size = sa_get_hash_size(aalg_id);
	if (!hash_size)
		return -1;
	/* Round up the tag size to multiple of 8 */
	hash_size = roundup(hash_size, 8);

#ifndef TEST
	sa_set_swinfo(first_engine, ctx->sc_id, ctx->sc_phys, 1, 0,
			0, queue_id, flow_id, hash_size, swinfo);
#else
	/* For run-time self tests in the cryptographic
	 * algorithm manager framework */
	sa_set_swinfo(first_engine, ctx->sc_id, ctx->sc_phys, 1, 0,
			SA_SW_INFO_FLAG_EVICT, queue_id, flow_id,
			hash_size, swinfo);
#endif
	sa_dump_sc(sc_buf, ctx->sc_phys);

	return 0;
}

/* Tear down the Security Context */
#define SA_SC_TEAR_RETRIES	5
#define SA_SC_TEAR_DELAY	20 /* msecs */
static int sa_tear_sc(struct sa_ctx_info *ctx,
			struct keystone_crypto_data *pdata)
{
	int own_off, cnt = SA_SC_TEAR_RETRIES;
	struct dma_async_tx_descriptor *desc;
	struct sa_dma_req_ctx *dma_ctx;
	struct sa_swinfo swinfo;
	dma_cookie_t cookie;
	unsigned long flags;
	u16 queue_id;
	int ret = 0;
	u8 flow_id;

	dma_ctx = kmalloc(sizeof(struct sa_dma_req_ctx), 0);
	if (!dma_ctx)
		return -ENOMEM;

	if (sg_alloc_table(&dma_ctx->sg_tbl, 2, GFP_KERNEL))
		return -ENOMEM;

	queue_id = dma_get_rx_queue(ctx->rx_chan);
	flow_id = dma_get_rx_flow(ctx->rx_chan);

	sa_set_swinfo(SA_ENG_ID_OUTPORT2, ctx->sc_id, ctx->sc_phys, 0, 0,
		(SA_SW_INFO_FLAG_TEAR | SA_SW_INFO_FLAG_EVICT |
		SA_SW_INFO_FLAG_NOPD), queue_id, flow_id, 0, &swinfo);

	/* swinfo word 0 is epib[1] */
	ctx->epib[0] = 0;
	memcpy(&ctx->epib[1], &swinfo.word[0], sizeof(swinfo));

	sg_set_buf(&dma_ctx->sg_tbl.sgl[0], ctx->epib, sizeof(ctx->epib));

	/* NOTE: pktdma driver doesn't support 0 buffer DMA
	 * hence pass a dummy buffer */
	sg_set_buf(&dma_ctx->sg_tbl.sgl[1], dma_ctx, sizeof(dma_ctx));

	/* map the packet */
	dma_ctx->sg_tbl.nents = dma_map_sg(keystone_dev,
					&dma_ctx->sg_tbl.sgl[1],
					1, DMA_TO_DEVICE);

	if (dma_ctx->sg_tbl.nents != 1) {
		dev_warn(keystone_dev, "failed to map null pkt\n");
		ret = -ENXIO;
		goto err;
	}
	dma_ctx->map_idx = 1;

	desc = dmaengine_prep_slave_sg(pdata->dma_data.tx_chan,
					dma_ctx->sg_tbl.sgl, 2,
					DMA_MEM_TO_DEV, DMA_HAS_EPIB);

	if (IS_ERR_OR_NULL(desc)) {
		dev_warn(keystone_dev, "failed to prep slave dma\n");
		ret = -ENOBUFS;
		goto err;
	}

	dma_ctx->tx_chan = pdata->dma_data.tx_chan;
	dma_ctx->dev_data = pdata;
	dma_ctx->pkt = false;
	desc->callback = sa_tx_dma_cb;
	desc->callback_param = dma_ctx;

	spin_lock_irqsave(&pdata->irq_lock, flags);
	cookie = dmaengine_submit(desc);
	dma_ctx->cookie = cookie;
	spin_unlock_irqrestore(&pdata->irq_lock, flags);

	if (dma_submit_error(cookie)) {
		dev_warn(keystone_dev, "failed to submit null pkt\n");
		ret = -ENXIO;
		goto err;
	}

	/*
	 * Check that CP_ACE has released the context
	 * by making sure that the owner bit is 0
	 */
	/*
	 * Security context had been swizzled by 128 bits
	 * before handing to CP_ACE
	 */
	own_off = ((SA_CTX_SCCTL_OWNER_OFFSET/16) * 16)
		+ (15 - (SA_CTX_SCCTL_OWNER_OFFSET % 16));
	while (__raw_readb(&ctx->sc[own_off])) {
		if (!--cnt)
			return -EAGAIN;
		msleep_interruptible(SA_SC_TEAR_DELAY);
	}
	return 0;

err:
	atomic_inc(&pdata->stats.sc_tear_dropped);
	sg_free_table(&dma_ctx->sg_tbl);
	kfree(dma_ctx);
	return ret;
}

/************************************************************/
/*	Algorithm interface functions & templates	*/
/************************************************************/
struct sa_alg_tmpl {
	u32 type; /* CRYPTO_ALG_TYPE from <linux/crypto.h> */
	union {
		struct crypto_alg crypto;
		struct ahash_alg hash;
	} alg;
	int registered;
};

/* Free the per direction context memory */
static void sa_free_ctx_info
(
	struct sa_ctx_info *ctx,
	struct keystone_crypto_data *data
)
{
	unsigned long bn;

	if (sa_tear_sc(ctx, data)) {
		dev_err(keystone_dev,
			"Failed to tear down context id(%x)\n", ctx->sc_id);
		return;
	}

	bn = ctx->sc_id - data->sc_id_start;
	spin_lock(&data->scid_lock);
	__clear_bit(bn, data->ctx_bm);
	data->sc_id--;
	spin_unlock(&data->scid_lock);

	if (ctx->sc) {
		dma_pool_free(data->sc_pool, ctx->sc, ctx->sc_phys);
		ctx->sc = NULL;
	}
	return;
}

/* Initialize the per direction context memory */
static int sa_init_ctx_info
(
	struct sa_ctx_info *ctx,
	struct keystone_crypto_data *data
)
{
	unsigned long bn;
	int err;

	spin_lock(&data->scid_lock);
	if (data->sc_id > data->sc_id_end) {
		spin_unlock(&data->scid_lock);
		dev_err(&data->pdev->dev, "Out of SC IDs\n");
		return -1;
	}
	bn = find_first_zero_bit(data->ctx_bm, sizeof(data->ctx_bm));
	__set_bit(bn, data->ctx_bm);
	data->sc_id++;
	spin_unlock(&data->scid_lock);

	ctx->sc_id = (u16)(data->sc_id_start + bn);

	ctx->rx_chan = data->dma_data.rx_chan;

	ctx->sc = dma_pool_alloc(data->sc_pool, GFP_KERNEL, &ctx->sc_phys);
	if (!ctx->sc) {
		dev_err(&data->pdev->dev, "Failed to allocate SC memory\n");
		err = -ENOMEM;
		goto scid_rollback;
	}

	return 0;

scid_rollback:
	spin_lock(&data->scid_lock);
	__clear_bit(bn, data->ctx_bm);
	data->sc_id--;
	spin_unlock(&data->scid_lock);

	return err;
}

/* Initialize TFM context */
static int sa_init_tfm(struct crypto_tfm *tfm)
{
	struct crypto_alg *alg = tfm->__crt_alg;
	struct sa_alg_tmpl *sa_alg;
	struct sa_tfm_ctx *ctx = crypto_tfm_ctx(tfm);
	struct keystone_crypto_data *data = dev_get_drvdata(keystone_dev);
	int ret;

	if ((alg->cra_flags & CRYPTO_ALG_TYPE_MASK) == CRYPTO_ALG_TYPE_AHASH)
		sa_alg = container_of(__crypto_ahash_alg(alg),
					struct sa_alg_tmpl, alg.hash);
	else
		sa_alg = container_of(alg, struct sa_alg_tmpl, alg.crypto);

	memset(ctx, 0, sizeof(*ctx));
	ctx->dev_data = data;

	if (sa_alg->type == CRYPTO_ALG_TYPE_AHASH) {
		ret = sa_init_ctx_info(&ctx->auth, data);
		if (ret)
			return ret;
	} else if (sa_alg->type == CRYPTO_ALG_TYPE_AEAD) {
		ret = sa_init_ctx_info(&ctx->enc, data);
		if (ret)
			return ret;
		ret = sa_init_ctx_info(&ctx->dec, data);
		if (ret) {
			sa_free_ctx_info(&ctx->enc, data);
			return ret;
		}
	} else if (sa_alg->type == CRYPTO_ALG_TYPE_ABLKCIPHER) {
		ret = sa_init_ctx_info(&ctx->enc, data);
		if (ret)
			return ret;
		ret = sa_init_ctx_info(&ctx->dec, data);
		if (ret) {
			sa_free_ctx_info(&ctx->enc, data);
			return ret;
		}
	}

	dev_dbg(keystone_dev, "%s(0x%p) sc-ids(0x%x(0x%x), 0x%x(0x%x))\n",
			__func__, tfm, ctx->enc.sc_id, ctx->enc.sc_phys,
			ctx->dec.sc_id, ctx->dec.sc_phys);
	return 0;
}

/* Algorithm init */
static int sa_cra_init_aead(struct crypto_tfm *tfm)
{
	return sa_init_tfm(tfm);
}

/* Algorithm init */
static int sa_cra_init_ablkcipher(struct crypto_tfm *tfm)
{
	return sa_init_tfm(tfm);
}

/* Algorithm init */
static int sa_cra_init_ahash(struct crypto_tfm *tfm)
{
	return sa_init_tfm(tfm);
}

/* Algorithm context teardown */
static void sa_exit_tfm(struct crypto_tfm *tfm)
{
	struct crypto_alg *alg = tfm->__crt_alg;
	struct sa_tfm_ctx *ctx = crypto_tfm_ctx(tfm);
	struct keystone_crypto_data *data = dev_get_drvdata(keystone_dev);

	dev_dbg(keystone_dev, "%s(0x%p) sc-ids(0x%x(0x%x), 0x%x(0x%x))\n",
			__func__, tfm, ctx->enc.sc_id, ctx->enc.sc_phys,
			ctx->dec.sc_id, ctx->dec.sc_phys);

	if ((alg->cra_flags & CRYPTO_ALG_TYPE_MASK)
			== CRYPTO_ALG_TYPE_AEAD) {
		sa_free_ctx_info(&ctx->enc, data);
		sa_free_ctx_info(&ctx->dec, data);
	} else if ((alg->cra_flags & CRYPTO_ALG_TYPE_MASK)
			== CRYPTO_ALG_TYPE_AHASH) {
		sa_free_ctx_info(&ctx->auth, data);
	} else if ((alg->cra_flags & CRYPTO_ALG_TYPE_MASK)
			== CRYPTO_ALG_TYPE_ABLKCIPHER) {
		sa_free_ctx_info(&ctx->enc, data);
		sa_free_ctx_info(&ctx->dec, data);
	}
	return;
}

/* AEAD algorithm configuration interface function */
static int sa_aead_setkey(struct crypto_aead *authenc,
		const u8 *key, unsigned int keylen)
{
	struct sa_tfm_ctx *ctx = crypto_aead_ctx(authenc);
	unsigned int enckey_len, authkey_len, auth_size;
	struct rtattr *rta = (struct rtattr *)key;
	struct crypto_authenc_key_param *param;
	struct sa_eng_info enc_eng, auth_eng;
	int ealg_id, aalg_id, cmdl_len;
	struct sa_cmdl_cfg cfg;
	struct sa_swinfo swinfo;
	u8 const *enc_key;
	u8 const *auth_key;
	const char *cra_name;

	if (!RTA_OK(rta, keylen))
		goto badkey;
	if (rta->rta_type != CRYPTO_AUTHENC_KEYA_PARAM)
		goto badkey;
	if (RTA_PAYLOAD(rta) < sizeof(*param))
		goto badkey;

	param = RTA_DATA(rta);
	enckey_len = be32_to_cpu(param->enckeylen);

	key += RTA_ALIGN(rta->rta_len);
	keylen -= RTA_ALIGN(rta->rta_len);

	if (keylen < enckey_len)
		goto badkey;

	authkey_len = keylen - enckey_len;
	auth_size = crypto_aead_authsize(authenc);

	enc_key = key + authkey_len;
	auth_key = key;

	cra_name = crypto_tfm_alg_name(crypto_aead_tfm(authenc));

	sa_conv_calg_to_salg(cra_name, &ealg_id, &aalg_id);
	sa_get_engine_info(ealg_id, &enc_eng);
	sa_get_engine_info(aalg_id, &auth_eng);

	memset(&cfg, 0, sizeof(cfg));
	cfg.enc1st = 1;
	cfg.aalg = aalg_id;
	cfg.enc_eng_id = enc_eng.eng_id;
	cfg.auth_eng_id = auth_eng.eng_id;
	cfg.iv_size = crypto_aead_ivsize(authenc);
	cfg.akey = auth_key;
	cfg.akey_len = authkey_len;

	/* Setup Encryption Security Context & Command label template */
	if (sa_init_sc(&ctx->enc, enc_key, enckey_len, auth_key,
				authkey_len, cra_name, 1, &swinfo))
		goto badkey;

	memcpy(&ctx->enc.epib[1], &swinfo.word[0], sizeof(swinfo));
	cmdl_len = sa_format_cmdl_gen(&cfg,
				(u8 *)ctx->enc.cmdl, &ctx->enc.cmdl_upd_info);
	if (cmdl_len <= 0)
		goto badkey;

	ctx->enc.cmdl_size = cmdl_len;

	/* Setup Decryption Security Context & Command label template */
	if (sa_init_sc(&ctx->dec, enc_key, enckey_len, auth_key,
				authkey_len, cra_name, 0, &swinfo))
		goto badkey;

	memcpy(&ctx->dec.epib[1], &swinfo.word[0], sizeof(swinfo));

	cfg.enc1st = 0;
	cfg.enc_eng_id = enc_eng.eng_id;
	cfg.auth_eng_id = auth_eng.eng_id;
	cmdl_len = sa_format_cmdl_gen(&cfg,
				(u8 *)ctx->dec.cmdl, &ctx->dec.cmdl_upd_info);

	if (cmdl_len <= 0)
		goto badkey;

	ctx->dec.cmdl_size = cmdl_len;
	return 0;

badkey:
	dev_err(keystone_dev, "%s: badkey\n", __func__);
	crypto_aead_set_flags(authenc, CRYPTO_TFM_RES_BAD_KEY_LEN);
	return -EINVAL;
}

/* AEAD algorithm configuration interface function */
static int sa_aead_setauthsize(struct crypto_aead *tfm,
				unsigned int auth_size)
{
	if (auth_size > crypto_aead_alg(tfm)->maxauthsize)
		return -EINVAL;
	return 0;
}

static int sa_aead_perform(struct aead_request *req, u8 *iv, int enc)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct sa_tfm_ctx *ctx = crypto_aead_ctx(tfm);
	struct sa_ctx_info *sa_ctx = enc ? &ctx->enc : &ctx->dec;
	struct device *dev = keystone_dev;
	dma_cookie_t cookie;

	struct keystone_crypto_data *pdata = dev_get_drvdata(dev);
	unsigned ivsize = crypto_aead_ivsize(tfm);
	u8 enc_offset = req->assoclen + ivsize;
	struct sa_dma_req_ctx *req_ctx = NULL;
	struct dma_async_tx_descriptor *desc;
	int assoc_sgents, src_sgents;
	int psdata_offset, ret = 0;
	unsigned long irq_flags;
	u8 auth_offset = 0;
	u8 *auth_iv = NULL;
	int sg_nents = 2; /* First 2 entries are for EPIB & PSDATA */
	u8 *aad = NULL;
	u8 aad_len = 0;
	int sg_idx = 0;
	u16 enc_len;
	u16 auth_len;
	u32 req_type;

	gfp_t flags = req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP ?
			GFP_KERNEL : GFP_ATOMIC;

	if (unlikely(atomic_read(&pdata->pend_compl) >= pdata->tx_thresh)) {
		ret = -EBUSY;
		goto err;
	}

	req_ctx = kmalloc(sizeof(struct sa_dma_req_ctx), flags);

	if (unlikely(req_ctx == NULL)) {
		ret = -ENOMEM;
		goto err;
	}

	enc_len = req->cryptlen;

	/* req->cryptlen includes authsize when decrypting */
	if (!enc)
		enc_len -= crypto_aead_authsize(tfm);

	auth_len = req->assoclen + ivsize + enc_len;

	memcpy(req_ctx->cmdl, sa_ctx->cmdl, sa_ctx->cmdl_size);
	/* Update Command Label */
	sa_update_cmdl(dev, enc_offset, enc_len,
			iv, auth_offset, auth_len,
			auth_iv, aad_len, aad,
			&sa_ctx->cmdl_upd_info, req_ctx->cmdl);

	/* Allocate descriptor & submit packet */
	assoc_sgents = sg_count(req->assoc, req->assoclen);
	sg_nents += assoc_sgents;
	src_sgents = sg_count(req->src, enc_len);
	sg_nents += src_sgents;

	if (likely(ivsize))
		sg_nents += 1;

	if (unlikely(sg_alloc_table(&req_ctx->sg_tbl, sg_nents, flags))) {
		ret = -ENOMEM;
		goto err;
	}

	sg_set_buf(&req_ctx->sg_tbl.sgl[sg_idx++], sa_ctx->epib,
			sizeof(sa_ctx->epib));

	/* Last 2 words in PSDATA will have the crypto alg type &
	 * crypto request pointer
	 */
	req_type = CRYPTO_ALG_TYPE_AEAD;
	if (enc)
		req_type |= (SA_REQ_SUBTYPE_ENC << SA_REQ_SUBTYPE_SHIFT);
	else
		req_type |= (SA_REQ_SUBTYPE_DEC << SA_REQ_SUBTYPE_SHIFT);
	psdata_offset = sa_ctx->cmdl_size/sizeof(u32);
	/* Append the type of request */
	req_ctx->cmdl[psdata_offset++] = req_type;
	/* Append the pointer to request */
	req_ctx->cmdl[psdata_offset] = (u32)req;

#ifdef DEBUG
	dev_info(dev, "cmdl:\n");
	print_hex_dump(KERN_CONT, "", DUMP_PREFIX_OFFSET,
			16, 1, req_ctx->cmdl, sa_ctx->cmdl_size, false);
#endif
	sg_set_buf(&req_ctx->sg_tbl.sgl[sg_idx++], req_ctx->cmdl,
			(sa_ctx->cmdl_size +
			 (SA_NUM_PSDATA_CTX_WORDS * sizeof(u32))));
	req_ctx->map_idx = sg_idx;

	/* clone the assoc sg list */
	if (likely(req->assoclen)) {
		sa_clone_sg(req->assoc, &req_ctx->sg_tbl.sgl[sg_idx],
				req->assoclen);
		sg_idx += assoc_sgents;
	}

	if (likely(ivsize))
		sg_set_buf(&req_ctx->sg_tbl.sgl[sg_idx++], iv, ivsize);

	/* clone the src sg list */
	if (likely(enc_len)) {
		sa_clone_sg(req->src, &req_ctx->sg_tbl.sgl[sg_idx], enc_len);
		sg_idx += src_sgents;
	}

	/* map the packet */
	req_ctx->sg_tbl.nents = dma_map_sg(dev,
					&req_ctx->sg_tbl.sgl[req_ctx->map_idx],
					(sg_nents - req_ctx->map_idx),
					DMA_TO_DEVICE);
	if (unlikely(req_ctx->sg_tbl.nents != (sg_nents - req_ctx->map_idx))) {
		dev_warn_ratelimited(dev, "failed to map tx pkt\n");
		ret = -EIO;
		goto err;
	}

	desc = dmaengine_prep_slave_sg(pdata->dma_data.tx_chan,
					req_ctx->sg_tbl.sgl, sg_nents,
					DMA_MEM_TO_DEV,
					(DMA_HAS_EPIB | DMA_HAS_PSINFO));

	if (unlikely(IS_ERR_OR_NULL(desc))) {
		dma_unmap_sg(dev, &req_ctx->sg_tbl.sgl[2],
				(sg_nents - 2), DMA_TO_DEVICE);
		dev_warn_ratelimited(dev, "failed to prep slave dma\n");
		ret = -ENOBUFS;
		goto err;
	}

	req_ctx->tx_chan = pdata->dma_data.tx_chan;
	req_ctx->dev_data = pdata;
	req_ctx->pkt = true;
	desc->callback = sa_tx_dma_cb;
	desc->callback_param = req_ctx;

	spin_lock_irqsave(&pdata->irq_lock, irq_flags);
	cookie = dmaengine_submit(desc);
	req_ctx->cookie = cookie;
	spin_unlock_irqrestore(&pdata->irq_lock, irq_flags);

	if (unlikely(dma_submit_error(cookie))) {
		dev_warn_ratelimited(dev, "failed to submit tx pkt\n");
		ret = -EIO;
		goto err;
	}

	return -EINPROGRESS;
err:
	atomic_inc(&pdata->stats.tx_dropped);
	if (req_ctx && req_ctx->sg_tbl.sgl)
		sg_free_table(&req_ctx->sg_tbl);
	kfree(req_ctx);
	return ret;
}

/* AEAD algorithm encrypt interface function */
static int sa_aead_encrypt(struct aead_request *req)
{
	return sa_aead_perform(req, req->iv, 1);
}

/* AEAD algorithm decrypt interface function */
static int sa_aead_decrypt(struct aead_request *req)
{
	return sa_aead_perform(req, req->iv, 0);
}

/* AEAD algorithm givencrypt interface function */
static int sa_aead_givencrypt(struct aead_givcrypt_request *req)
{
	struct crypto_aead *tfm = aead_givcrypt_reqtfm(req);

	get_random_bytes(req->giv, crypto_aead_ivsize(tfm));
	return sa_aead_perform(&req->areq, req->giv, 1);
}

static int sa_ablkcipher_setkey(struct crypto_ablkcipher *cipher,
			     const u8 *key, unsigned int keylen)
{
	return 0;
}

static int sa_ablkcipher_encrypt(struct ablkcipher_request *areq)
{
	return 0;
}

static int sa_ablkcipher_decrypt(struct ablkcipher_request *areq)
{
	return 0;
}

static int sa_ahash_init(struct ahash_request *areq)
{
	return 0;
}

static int sa_ahash_update(struct ahash_request *areq)
{
	return 0;
}

static int sa_ahash_final(struct ahash_request *areq)
{
	return 0;
}

static int sa_ahash_finup(struct ahash_request *areq)
{
	return 0;
}

static int sa_ahash_digest(struct ahash_request *areq)
{
	return 0;
}

static int sa_ahash_setkey(struct crypto_ahash *tfm, const u8 *key,
			unsigned int keylen)
{
	return 0;
}

static struct sa_alg_tmpl sa_algs[] = {
	/* AEAD algorithms */
	{	.type = CRYPTO_ALG_TYPE_AEAD,
		.alg.crypto = {
			.cra_name = "authenc(hmac(sha1),cbc(aes))",
			.cra_driver_name =
				"authenc-hmac-sha1-cbc-aes-keystone-sa",
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_aead = {
				.geniv = "custom",
				.ivsize = AES_BLOCK_SIZE,
				.maxauthsize = SHA1_DIGEST_SIZE,
			}
		}
	},
	{	.type = CRYPTO_ALG_TYPE_AEAD,
		.alg.crypto = {
			.cra_name = "authenc(hmac(sha1),cbc(des3_ede))",
			.cra_driver_name =
				"authenc-hmac-sha1-cbc-3des-keystone-sa",
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_aead = {
				.geniv = "custom",
				.ivsize = DES3_EDE_BLOCK_SIZE,
				.maxauthsize = SHA1_DIGEST_SIZE,
			}
		}
	},
	{       .type = CRYPTO_ALG_TYPE_AEAD,
		.alg.crypto = {
			.cra_name = "authenc(xcbc(aes),cbc(aes))",
			.cra_driver_name =
				"authenc-aes-xcbc-mac-cbc-aes-keystone-sa",
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_aead = {
				.geniv = "custom",
				.ivsize = AES_BLOCK_SIZE,
				.maxauthsize = AES_XCBC_DIGEST_SIZE,
			}
		}
	},
	{	.type = CRYPTO_ALG_TYPE_AEAD,
		.alg.crypto = {
			.cra_name = "authenc(xcbc(aes),cbc(des3_ede))",
			.cra_driver_name =
				"authenc-aes-xcbc-mac-cbc-3des-keystone-sa",
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_aead = {
				.geniv = "custom",
				.ivsize = DES3_EDE_BLOCK_SIZE,
				.maxauthsize = AES_XCBC_DIGEST_SIZE,
			}
		}
	},
#ifdef TODO
	{	.type = CRYPTO_ALG_TYPE_AEAD,
		.alg.crypto = {
			.cra_name = "authenc(hmac(md5),cbc(aes))",
			.cra_driver_name =
				"authenc-hmac-md5-cbc-aes-keystone-sa",
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_aead = {
				.geniv = "custom",
				.ivsize = AES_BLOCK_SIZE,
				.maxauthsize = MD5_DIGEST_SIZE,
			}
		}
	},
	{	.type = CRYPTO_ALG_TYPE_AEAD,
		.alg.crypto = {
			.cra_name = "authenc(hmac(md5),cbc(des3_ede))",
			.cra_driver_name =
				"authenc-hmac-md5-cbc-3des-keystone-sa",
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_aead = {
				.geniv = "custom",
				.ivsize = DES3_EDE_BLOCK_SIZE,
				.maxauthsize = MD5_DIGEST_SIZE,
			}
		}
	},
	/* ABLKCIPHER algorithms. */
	{	.type = CRYPTO_ALG_TYPE_ABLKCIPHER,
		.alg.crypto = {
			.cra_name = "cbc(aes)",
			.cra_driver_name = "cbc-aes-keystone-sa",
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ablkcipher = {
				.geniv = "custom",
				.min_keysize = AES_MIN_KEY_SIZE,
				.max_keysize = AES_MAX_KEY_SIZE,
				.ivsize = AES_BLOCK_SIZE,
			}
		}
	},
	{	.type = CRYPTO_ALG_TYPE_ABLKCIPHER,
		.alg.crypto = {
			.cra_name = "cbc(des3_ede)",
			.cra_driver_name = "cbc-3des-keystone-sa",
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ablkcipher = {
				.geniv = "custom",
				.min_keysize = DES3_EDE_KEY_SIZE,
				.max_keysize = DES3_EDE_KEY_SIZE,
				.ivsize = DES3_EDE_BLOCK_SIZE,
			}
		}
	},
	/* AHASH algorithms. */
	{	.type = CRYPTO_ALG_TYPE_AHASH,
		.alg.hash = {
			.halg.digestsize = AES_XCBC_DIGEST_SIZE,
			.halg.base = {
				.cra_name = "xcbc(aes)",
				.cra_driver_name =
					"aes-xcbc-mac-keystone-sa",
				.cra_blocksize = SHA224_BLOCK_SIZE,
			}
		}
	},
	{	.type = CRYPTO_ALG_TYPE_AHASH,
		.alg.hash = {
			.halg.digestsize = MD5_DIGEST_SIZE,
			.halg.base = {
				.cra_name = "hmac(md5)",
				.cra_driver_name =
					"hmac-md5-keystone-sa",
				.cra_blocksize = MD5_BLOCK_SIZE,
			}
		}
	},
	{	.type = CRYPTO_ALG_TYPE_AHASH,
		.alg.hash = {
			.halg.digestsize = SHA1_DIGEST_SIZE,
			.halg.base = {
				.cra_name = "hmac(sha1)",
				.cra_driver_name =
					"hmac-sha1-keystone-sa",
				.cra_blocksize = SHA1_BLOCK_SIZE,
			}
		}
	}
#endif
};

/* Register the algorithms in crypto framework */
static void sa_register_algos(const struct device *dev)
{
	struct crypto_alg *cra;
	struct ahash_alg *hash = NULL;
	char *alg_name;
	u32 type;
	int i, err, num_algs = ARRAY_SIZE(sa_algs);

	for (i = 0; i < num_algs; i++) {
		type = sa_algs[i].type;
		if (type == CRYPTO_ALG_TYPE_AEAD) {
			cra = &sa_algs[i].alg.crypto;
			alg_name = cra->cra_name;
			if (snprintf(cra->cra_driver_name, CRYPTO_MAX_ALG_NAME,
			"%s""-keystone-sa", alg_name) >= CRYPTO_MAX_ALG_NAME) {
				continue;
			}
			cra->cra_type = &crypto_aead_type;
			cra->cra_flags = CRYPTO_ALG_TYPE_AEAD |
					CRYPTO_ALG_KERN_DRIVER_ONLY |
					CRYPTO_ALG_ASYNC;
			cra->cra_aead.setkey = sa_aead_setkey;
			cra->cra_aead.setauthsize = sa_aead_setauthsize;
			cra->cra_aead.encrypt = sa_aead_encrypt;
			cra->cra_aead.decrypt = sa_aead_decrypt;
			cra->cra_aead.givencrypt = sa_aead_givencrypt;
			cra->cra_init = sa_cra_init_aead;
		} else if (type == CRYPTO_ALG_TYPE_ABLKCIPHER) {
			cra = &sa_algs[i].alg.crypto;
			alg_name = cra->cra_name;
			if (snprintf(cra->cra_driver_name, CRYPTO_MAX_ALG_NAME,
			"%s""-keystone-sa", alg_name) >= CRYPTO_MAX_ALG_NAME) {
				continue;
			}
			cra->cra_type = &crypto_ablkcipher_type;
			cra->cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER |
					CRYPTO_ALG_KERN_DRIVER_ONLY |
					CRYPTO_ALG_ASYNC;
			cra->cra_ablkcipher.setkey = sa_ablkcipher_setkey;
			cra->cra_ablkcipher.encrypt = sa_ablkcipher_encrypt;
			cra->cra_ablkcipher.decrypt = sa_ablkcipher_decrypt;
			cra->cra_init = sa_cra_init_ablkcipher;
		} else if (type == CRYPTO_ALG_TYPE_AHASH) {
			hash = &sa_algs[i].alg.hash;
			alg_name = hash->halg.base.cra_name;
			if (snprintf(hash->halg.base.cra_driver_name,
				CRYPTO_MAX_ALG_NAME, "%s""-keystone-sa",
				alg_name) >= CRYPTO_MAX_ALG_NAME) {
				continue;
			}
			hash->init = sa_ahash_init;
			hash->update = sa_ahash_update;
			hash->final = sa_ahash_final;
			hash->finup = sa_ahash_finup;
			hash->digest = sa_ahash_digest;
			hash->setkey = sa_ahash_setkey;
			cra = &hash->halg.base;
			cra->cra_flags = CRYPTO_ALG_TYPE_AHASH |
						CRYPTO_ALG_KERN_DRIVER_ONLY |
						CRYPTO_ALG_ASYNC;
			cra->cra_type = &crypto_ahash_type;
			cra->cra_init = sa_cra_init_ahash;
		} else {
			dev_err(dev,
				"un-supported crypto algorithm (%d)", type);
			continue;
		}

		cra->cra_ctxsize = sizeof(struct sa_tfm_ctx);
		cra->cra_module = THIS_MODULE;
		cra->cra_alignmask = 0;
		cra->cra_priority = 3000;
		cra->cra_exit = sa_exit_tfm;

		if (type == CRYPTO_ALG_TYPE_AHASH)
			err = crypto_register_ahash(hash);
		else
			err = crypto_register_alg(cra);

		if (err)
			dev_err(dev, "Failed to register '%s'\n", alg_name);
		else
			sa_algs[i].registered = 1;
	}
	return;
}

/* un-register the algorithms from crypto framework */
static void sa_unregister_algos(const struct device *dev)
{
	u32 type;
	char *alg_name;
	int err = 0, i, num_algs = ARRAY_SIZE(sa_algs);

	for (i = 0; i < num_algs; i++) {
		type = sa_algs[i].type;
		if ((type == CRYPTO_ALG_TYPE_AHASH) &&
				(sa_algs[i].registered)) {
			alg_name = sa_algs[i].alg.hash.halg.base.cra_name;
			err = crypto_unregister_ahash(&sa_algs[i].alg.hash);
		} else if (sa_algs[i].registered) {
			alg_name = sa_algs[i].alg.crypto.cra_name;
			err = crypto_unregister_alg(&sa_algs[i].alg.crypto);
		}

		if (err)
			dev_err(dev, "Failed to unregister '%s'", alg_name);
	}
	return;
}

/************************************************************/
/*	SYSFS interface functions			    */
/************************************************************/
struct sa_kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, char *buf);
	ssize_t	(*store)(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, const char *, size_t);
};

#define SA_ATTR(_name, _mode, _show, _store) \
	struct sa_kobj_attribute sa_attr_##_name = \
__ATTR(_name, _mode, _show, _store)

static ssize_t sa_stats_show_tx_pkts(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			atomic_read(&crypto->stats.tx_pkts));
}

static ssize_t sa_stats_reset_tx_pkts(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, const char *buf, size_t len)
{
	atomic_set(&crypto->stats.tx_pkts, 0);
	return len;
}

static ssize_t sa_stats_show_rx_pkts(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			atomic_read(&crypto->stats.rx_pkts));
}

static ssize_t sa_stats_reset_rx_pkts(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, const char *buf, size_t len)
{
	atomic_set(&crypto->stats.rx_pkts, 0);
	return len;
}

static ssize_t sa_stats_show_tx_drop_pkts(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			atomic_read(&crypto->stats.tx_dropped));
}

static ssize_t sa_stats_reset_tx_drop_pkts(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, const char *buf, size_t len)
{
	atomic_set(&crypto->stats.tx_dropped, 0);
	return len;
}

static ssize_t
sa_stats_show_sc_tear_drop_pkts(struct keystone_crypto_data *crypto,
		struct sa_kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			atomic_read(&crypto->stats.sc_tear_dropped));
}

static SA_ATTR(tx_pkts, S_IRUGO | S_IWUSR,
		sa_stats_show_tx_pkts, sa_stats_reset_tx_pkts);
static SA_ATTR(rx_pkts, S_IRUGO | S_IWUSR,
		sa_stats_show_rx_pkts, sa_stats_reset_rx_pkts);
static SA_ATTR(tx_drop_pkts, S_IRUGO | S_IWUSR,
		sa_stats_show_tx_drop_pkts, sa_stats_reset_tx_drop_pkts);
static SA_ATTR(sc_tear_drop_pkts, S_IRUGO,
		sa_stats_show_sc_tear_drop_pkts, NULL);

static struct attribute *sa_stats_attrs[] = {
	&sa_attr_tx_pkts.attr,
	&sa_attr_rx_pkts.attr,
	&sa_attr_tx_drop_pkts.attr,
	&sa_attr_sc_tear_drop_pkts.attr,
	NULL
};

#define to_sa_kobj_attr(_attr) \
	container_of(_attr, struct sa_kobj_attribute, attr)
#define to_crypto_data_from_stats_obj(obj) \
	container_of(obj, struct keystone_crypto_data, stats_kobj)

static ssize_t sa_kobj_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct sa_kobj_attribute *sa_attr = to_sa_kobj_attr(attr);
	struct keystone_crypto_data *crypto =
		to_crypto_data_from_stats_obj(kobj);
	ssize_t ret = -EIO;

	if (sa_attr->show)
		ret = sa_attr->show(crypto, sa_attr, buf);
	return ret;
}

static ssize_t sa_kobj_attr_store(struct kobject *kobj, struct attribute *attr,
			     const char *buf, size_t len)
{
	struct sa_kobj_attribute *sa_attr = to_sa_kobj_attr(attr);
	struct keystone_crypto_data *crypto =
		to_crypto_data_from_stats_obj(kobj);
	ssize_t ret = -EIO;

	if (sa_attr->store)
		ret = sa_attr->store(crypto, sa_attr, buf, len);
	return ret;
}

static const struct sysfs_ops sa_stats_sysfs_ops = {
	.show = sa_kobj_attr_show,
	.store = sa_kobj_attr_store,
};

static struct kobj_type sa_stats_ktype = {
	.sysfs_ops = &sa_stats_sysfs_ops,
	.default_attrs = sa_stats_attrs,
};

static int sa_create_sysfs_entries(struct keystone_crypto_data *crypto)
{
	struct device *dev = &crypto->pdev->dev;
	int ret;

	ret = kobject_init_and_add(&crypto->stats_kobj, &sa_stats_ktype,
		kobject_get(&dev->kobj), "stats");

	if (ret) {
		dev_err(dev, "failed to create sysfs entry\n");
		kobject_put(&crypto->stats_kobj);
		kobject_put(&dev->kobj);
	}
	return ret;
}

static void sa_delete_sysfs_entries(struct keystone_crypto_data *crypto)
{
	kobject_del(&crypto->stats_kobj);
}

/*
 * HW RNG functions
 */

static int sa_rng_init(struct hwrng *rng)
{
	u32 value;
	struct device *dev = (struct device *)rng->priv;
	struct keystone_crypto_data *crypto = dev_get_drvdata(dev);
	u32 startup_cycles, min_refill_cycles, max_refill_cycles, clk_div;

	crypto->trng_regs = (struct sa_trng_regs *)((void *)crypto->regs +
				SA_REG_MAP_TRNG_OFFSET);

	startup_cycles = SA_TRNG_DEF_STARTUP_CYCLES;
	min_refill_cycles = SA_TRNG_DEF_MIN_REFILL_CYCLES;
	max_refill_cycles = SA_TRNG_DEF_MAX_REFILL_CYCLES;
	clk_div = SA_TRNG_DEF_CLK_DIV_CYCLES;

	/* Enable RNG module */
	value = __raw_readl(&crypto->regs->mmr.CMD_STATUS);
	value |= SA_CMD_STATUS_REG_TRNG_ENABLE;
	__raw_writel(value, &crypto->regs->mmr.CMD_STATUS);

	/* Configure RNG module */
	__raw_writel(0, &crypto->trng_regs->TRNG_CONTROL); /* Disable RNG */
	value = startup_cycles << SA_TRNG_CONTROL_REG_STARTUP_CYCLES_SHIFT;
	__raw_writel(value, &crypto->trng_regs->TRNG_CONTROL);
	value =
	(min_refill_cycles << SA_TRNG_CONFIG_REG_MIN_REFILL_CYCLES_SHIFT) |
	(max_refill_cycles << SA_TRNG_CONFIG_REG_MAX_REFILL_CYCLES_SHIFT) |
	(clk_div << SA_TRNG_CONFIG_REG_SAMPLE_DIV_SHIFT);
	__raw_writel(value, &crypto->trng_regs->TRNG_CONFIG);
	/* Disable all interrupts from TRNG */
	__raw_writel(0, &crypto->trng_regs->TRNG_INTMASK);
	/* Enable RNG */
	value = __raw_readl(&crypto->trng_regs->TRNG_CONTROL);
	value |= SA_TRNG_CONTROL_REG_TRNG_ENABLE;
	__raw_writel(value, &crypto->trng_regs->TRNG_CONTROL);

	/* Initialize the TRNG access lock */
	spin_lock_init(&crypto->trng_lock);

	return 0;
}

void sa_rng_cleanup(struct hwrng *rng)
{
	u32 value;
	struct device *dev = (struct device *)rng->priv;
	struct keystone_crypto_data *crypto = dev_get_drvdata(dev);

	/* Disable RNG */
	__raw_writel(0, &crypto->trng_regs->TRNG_CONTROL);
	value = __raw_readl(&crypto->regs->mmr.CMD_STATUS);
	value &= ~SA_CMD_STATUS_REG_TRNG_ENABLE;
	__raw_writel(value, &crypto->regs->mmr.CMD_STATUS);
}

/* Maximum size of RNG data available in one read */
#define SA_MAX_RNG_DATA	8
/* Maximum retries to get rng data */
#define SA_MAX_RNG_DATA_RETRIES	5
/* Delay between retries (in usecs) */
#define SA_RNG_DATA_RETRY_DELAY	5

static int sa_rng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	u32 value;
	u32 st_ready;
	u32 rng_lo, rng_hi;
	int retries = SA_MAX_RNG_DATA_RETRIES;
	int data_sz = min_t(u32, max, SA_MAX_RNG_DATA);
	struct device *dev = (struct device *)rng->priv;
	struct keystone_crypto_data *crypto = dev_get_drvdata(dev);

	do {
		spin_lock(&crypto->trng_lock);
		value = __raw_readl(&crypto->trng_regs->TRNG_STATUS);
		st_ready = value & SA_TRNG_STATUS_REG_READY;
		if (st_ready) {
			/* Read random data */
			rng_hi = __raw_readl(&crypto->trng_regs->TRNG_OUTPUT_H);
			rng_lo = __raw_readl(&crypto->trng_regs->TRNG_OUTPUT_L);
			/* Clear ready status */
			__raw_writel(SA_TRNG_INTACK_REG_READY,
					&crypto->trng_regs->TRNG_INTACK);
		}
		spin_unlock(&crypto->trng_lock);
		udelay(SA_RNG_DATA_RETRY_DELAY);
	} while (wait && !st_ready && retries--);

	if (!st_ready)
		return -EAGAIN;

	if (likely(data_sz > sizeof(rng_lo))) {
		memcpy(data, &rng_lo, sizeof(rng_lo));
		memcpy((data + sizeof(rng_lo)), &rng_hi,
				(data_sz - sizeof(rng_lo)));
	} else {
		memcpy(data, &rng_lo, data_sz);
	}

	return data_sz;
}

static int sa_register_rng(struct device *dev)
{
	struct keystone_crypto_data *crypto = dev_get_drvdata(dev);

	crypto->rng.name = dev_driver_string(dev);
	crypto->rng.init = sa_rng_init;
	crypto->rng.cleanup = sa_rng_cleanup;
	crypto->rng.read = sa_rng_read;
	crypto->rng.priv = (unsigned long)dev;

	return hwrng_register(&crypto->rng);
}

static void sa_unregister_rng(struct device *dev)
{
	struct keystone_crypto_data *crypto = dev_get_drvdata(dev);
	hwrng_unregister(&crypto->rng);
}

/************************************************************/
/*	Driver registration functions			*/
/************************************************************/
static int sa_read_dtb(struct device_node *node,
			struct keystone_crypto_data *data)
{
	int i, ret = 0;
	struct sa_dma_data *dma_data = &data->dma_data;
	struct device *dev = &data->pdev->dev;
	u32 sc_id_range[2];

	/* Get DMA channel specifications from device tree */
	ret = of_property_read_string(node, "tx_channel",
				      &dma_data->tx_chan_name);
	if (ret < 0) {
		dma_data->tx_chan_name = "crypto-tx";
		dev_err(dev, "missing \"tx_channel\" parameter, defaulting to %s\n",
				dma_data->tx_chan_name);
	}
	dev_dbg(dev, "tx_channel %s\n", dma_data->tx_chan_name);

	ret = of_property_read_u32(node, "tx_queue_depth",
				   &dma_data->tx_queue_depth);
	if (ret < 0) {
		dma_data->tx_queue_depth = 128;
		dev_err(dev, "missing tx_queue_depth parameter, defaulting to %d\n",
				dma_data->tx_queue_depth);
	}
	dev_dbg(dev, "tx_queue_depth %u\n", dma_data->tx_queue_depth);

	ret = of_property_read_string(node, "rx_channel",
				      &dma_data->rx_chan_name);
	if (ret < 0) {
		dma_data->rx_chan_name = "crypto-rx0";
		dev_err(dev, "missing \"rx-channel\" parameter, defaulting to %s\n",
				dma_data->rx_chan_name);
	}
	dev_dbg(dev, "rx_channel %s\n", dma_data->rx_chan_name);

	ret = of_property_read_u32_array(node, "rx_queue_depth",
			   dma_data->rx_queue_depths, KEYSTONE_QUEUES_PER_CHAN);
	if (ret < 0) {
		dma_data->rx_queue_depths[0] = 128;
		dev_err(dev, "missing rx_queue_depth parameter, defaulting to %d\n",
				dma_data->rx_queue_depths[0]);
	}
	for (i = 0; i < KEYSTONE_QUEUES_PER_CHAN; i++)
		dev_dbg(dev, "rx_queue_depth[%d]= %u\n", i,
				dma_data->rx_queue_depths[i]);

	data->tx_thresh = dma_data->rx_queue_depths[0] - SA_MIN_RX_DESCS;

	ret = of_property_read_u32_array(node, "rx_buffer_size",
			dma_data->rx_buffer_sizes, KEYSTONE_QUEUES_PER_CHAN);
	if (ret < 0) {
		dma_data->rx_buffer_sizes[0] = 1500;
		dev_err(dev, "missing rx_buffer_size parameter, defaulting to %d\n",
				dma_data->rx_buffer_sizes[0]);
	}
	for (i = 0; i < KEYSTONE_QUEUES_PER_CHAN; i++)
		dev_dbg(dev, "rx_buffer_size[%d]= %u\n", i,
				dma_data->rx_buffer_sizes[i]);

	if (of_property_read_u32_array(node, "sc-id", sc_id_range, 2)) {
		data->sc_id_start = 0x7000;
		data->sc_id_end = 0x70ff;
		dev_err(dev, "No sc-id range-map array in dt bindings, defaulting to [%x, %x]\n",
				data->sc_id_start, data->sc_id_end);
	} else {
		data->sc_id_start = sc_id_range[0];
		data->sc_id_end = sc_id_range[1];
	}
	dev_dbg(dev, "sc-id range [%x, %x]\n",
			data->sc_id_start, data->sc_id_end);
	data->sc_id = data->sc_id_start;

	data->regs = of_iomap(node, 0);
	if (!data->regs) {
		dev_err(dev, "failed to of_iomap\n");
		return -ENOMEM;
	}

	return 0;
}

static int keystone_crypto_remove(struct platform_device *pdev)
{
	struct keystone_crypto_data *crypto = platform_get_drvdata(pdev);

	/* un-register crypto algorithms */
	sa_unregister_algos(&pdev->dev);
	/* un-register HW RNG */
	sa_unregister_rng(&pdev->dev);
	/* Delete SYSFS entries */
	sa_delete_sysfs_entries(crypto);
	/* Free Security context DMA pool */
	if (crypto->sc_pool)
		dma_pool_destroy(crypto->sc_pool);
	/* Release DMA channels */
	sa_teardown_dma(crypto);
	/* Kill tasklets */
	tasklet_kill(&crypto->rx_task);

	clk_disable_unprepare(crypto->clk);
	clk_put(crypto->clk);
	kfree(crypto);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static int keystone_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	struct keystone_crypto_data *crypto;
	u32 value;
	int ret;

	keystone_dev = dev;
	crypto = devm_kzalloc(dev, sizeof(*crypto), GFP_KERNEL);
	if (!crypto)
		return -ENOMEM;

	crypto->clk = clk_get(dev, NULL);
	if (IS_ERR_OR_NULL(crypto->clk)) {
		dev_err(dev, "Couldn't get clock\n");
		ret = -ENODEV;
		goto err;
	}

	ret = clk_prepare_enable(crypto->clk);
	if (ret < 0) {
		dev_err(dev, "Couldn't enable clock\n");
		clk_put(crypto->clk);
		ret = -ENODEV;
		goto err;
	}

	crypto->pdev = pdev;
	platform_set_drvdata(pdev, crypto);

	/* Read configuration from device tree */
	ret = sa_read_dtb(node, crypto);
	if (ret) {
		dev_err(dev, "Failed to get all relevant configurations from DTB...\n");
		goto err;
	}

	/* Enable the required sub-modules in SA */
	value = __raw_readl(&crypto->regs->mmr.CMD_STATUS);

	value |= (0x00000001u  /* Enc SS */
		| 0x00000002u /* Auth SS */
		| 0x00000080u /* Context Cache */
		| 0x00000100u /* PA in port */
		| 0x00000200u /* CDMA in port */
		| 0x00000400u /* PA out port */
		| 0x00000800u /* CDMA out port */
		| 0x00001000u /* Enc SS1 */
		| 0x00002000u); /* Auth SS1 */

	__raw_writel(value, &crypto->regs->mmr.CMD_STATUS);

	tasklet_init(&crypto->rx_task, sa_chan_work_handler,
		     (unsigned long) crypto);

	/* Setup DMA channels */
	if (sa_setup_dma(crypto)) {
		dev_err(dev, "Failed to set DMA channels");
		ret = -ENODEV;
		goto err;
	}

	/* setup dma pool for security context buffers */
	crypto->sc_pool = dma_pool_create("keystone-sc", dev,
					SA_CTX_MAX_SZ , 64, 0);
	if (!crypto->sc_pool) {
		dev_err(dev, "Failed to create dma pool");
		ret = -ENOMEM;
		goto err;
	}

	/* Initialize the SC-ID allocation lock */
	spin_lock_init(&crypto->scid_lock);

	/* Initialize the IRQ schedule prevention lock */
	spin_lock_init(&crypto->irq_lock);

	/* Initialize counters */
	atomic_set(&crypto->stats.tx_dropped, 0);
	atomic_set(&crypto->stats.sc_tear_dropped, 0);
	atomic_set(&crypto->pend_compl, 0);
	atomic_set(&crypto->stats.tx_pkts, 0);
	atomic_set(&crypto->stats.rx_pkts, 0);

	/* Create sysfs entries */
	ret = sa_create_sysfs_entries(crypto);
	if (ret)
		goto err;

	/* Register HW RNG support */
	ret = sa_register_rng(dev);
	if (ret) {
		dev_err(dev, "Failed to register HW RNG");
		goto err;
	}

	/* Register crypto algorithms */
	sa_register_algos(dev);
	dev_info(dev, "crypto accelerator enabled\n");
	return 0;

err:
	keystone_crypto_remove(pdev);
	return ret;
}

static struct of_device_id of_match[] = {
	{ .compatible = "ti,keystone-crypto", },
	{},
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver keystone_crypto_driver = {
	.probe	= keystone_crypto_probe,
	.remove	= keystone_crypto_remove,
	.driver	= {
		.name		= "keystone-crypto",
		.owner		= THIS_MODULE,
		.of_match_table	= of_match,
	},
};

static int __init keystone_crypto_mod_init(void)
{
	return  platform_driver_register(&keystone_crypto_driver);
}

static void __exit keystone_crypto_mod_exit(void)
{
	platform_driver_unregister(&keystone_crypto_driver);
}

module_init(keystone_crypto_mod_init);
module_exit(keystone_crypto_mod_exit);

MODULE_DESCRIPTION("Keystone crypto acceleration support.");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sandeep Nair <sandeep_n@ti.com>");

