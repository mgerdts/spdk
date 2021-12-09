/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Helper library to use spdk_bdev as the backing device for a blobstore
 */

#ifndef SPDK_BLOB_ESNAP_H
#define SPDK_BLOB_ESNAP_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/blob.h"
#include "spdk/tree.h"

#ifdef __cplusplus
extern "C" {
#endif

#if 0
struct spdk_bdev;
struct spdk_bdev_module;
#endif

struct spdk_bs_dev;
struct spdk_blob_load_ctx;
struct bs_esnap_channel_tree;

typedef void(*blob_back_bs_dev_load_done_t)(struct spdk_blob_load_ctx *load_ctx,
		struct spdk_bs_dev *dev, int bserrno);

/*
 * External snapshots require a channel per thread per external bdev.  The tree
 * is populated lazily as blob IOs are handled by the back_bs_dev. When this
 * channel is destroyed, all the channels in the tree are destroyed.
 */

struct bs_esnap_channel {
	RB_ENTRY(bs_esnap_channel)	node;
	spdk_blob_id			blob_id;
	struct spdk_io_channel		*channel;
};

struct spdk_esnap_channels {
	RB_HEAD(bs_esnap_channel_tree, bs_esnap_channel) tree;
};

RB_PROTOTYPE(bs_esnap_channel_tree, bs_esnap_channel, node, esnap_channel_compare)

/**
 * Create a blobstore external snapshot block device from a bdev.
 *
 * XXX-mg fix params once we know this is right.
 *
 * \param bdev_name Name of the bdev to use.
 * \param event_cb Called when the bdev triggers asynchronous event.
 * \param event_ctx Argument passed to function event_cb.
 * \param bs_dev Output parameter for a pointer to the blobstore block device.
 *
 * \return 0 if operation is successful, or suitable errno value otherwise.
 */
void blob_create_esnap_dev(struct spdk_blob_store *bs, struct spdk_blob *blob,
			   const char *uuid_str, blob_back_bs_dev_load_done_t load_cb,
			   struct spdk_blob_load_ctx *load_cb_arg);
/**
 * XXX-mg document, add init
 */
void spdk_bs_esnap_init_channels(struct spdk_esnap_channels *esnap_channels);
void spdk_bs_esnap_destroy_channels(struct spdk_esnap_channels *esnap_channels);
#ifdef __cplusplus
}
#endif

#endif
