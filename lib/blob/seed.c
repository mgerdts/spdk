/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/log.h"

#include "blobstore.h"

struct seed_ctx {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
};

// XXX-mg
// This feels like it needs reference counting to handle races with the destroy callback

static struct spdk_io_channel *
seed_create_channel(struct spdk_bs_dev *dev)
{
	struct spdk_io_channel *channel;
	struct seed_ctx *ctx = dev->seed_ctx;

	if (ctx == NULL || ctx->bdev_desc == NULL) {
		SPDK_INFOLOG(blob, "bsdev %p has no seed descriptor\n", dev);
		return NULL;
	}

	channel = spdk_bdev_get_io_channel(ctx->bdev_desc);
	SPDK_INFOLOG(blob, "bsdev %p created channel %p\n", dev, channel);
	return channel;
}

static void
seed_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
	SPDK_INFOLOG(blob, "bsdev %p destroying channel %p\n", dev, channel);
	spdk_put_io_channel(channel);
}

static void
seed_destroy(struct spdk_bs_dev *dev)
{
	struct seed_ctx *ctx = dev->seed_ctx;

	if (ctx != NULL) {
		if (ctx->bdev_io_channel != NULL) {
			spdk_put_io_channel(ctx->bdev_io_channel);
		}
		if (ctx->bdev_desc != NULL) {
			spdk_bdev_close(ctx->bdev_desc);
		}
	}

	free(dev);
}

static void
seed_complete(struct spdk_bdev_io *bdev_io, bool success, void *args)
{
	struct spdk_bs_dev_cb_args *cb_args = args;

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, success ? 0 : -EIO);
}

static void
seed_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	  uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct seed_ctx *ctx = dev->seed_ctx;

	if (ctx->bdev_desc == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_read_blocks(ctx->bdev_desc, cb_args->channel, payload,
			      lba, lba_count, seed_complete, cb_args);
}

static void
seed_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	   uint64_t lba, uint32_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
seed_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	   struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	struct seed_ctx *ctx = dev->seed_ctx;

	if (ctx->bdev_desc == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_readv_blocks(ctx->bdev_desc, cb_args->channel, iov,
			       iovcnt, lba, lba_count, seed_complete, cb_args);
}

static void
seed_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	    struct iovec *iov, int iovcnt,
	    uint64_t lba, uint32_t lba_count,
	    struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
seed_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  uint64_t lba, uint64_t lba_count,
		  struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
seed_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	   uint64_t lba, uint64_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void lvol_seed_bdev_event_cb_t(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				      void *event_ctx)
{
	struct seed_ctx *ctx = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		ctx->bdev_desc = NULL;
		// XXX-mg what about channel?
		spdk_bdev_close(ctx->bdev_desc);
		break;
	default:
		SPDK_NOTICELOG("Unsupported event %d\n", type);
		break;
	}
}

int
bs_create_seed_dev(struct spdk_blob *front, const char *seedname)
{
	struct spdk_bdev *bdev;
	struct spdk_bs_dev *back;
	struct seed_ctx *ctx;
	int ret;

	bdev = spdk_bdev_get_by_name(seedname);
	if (bdev == NULL) {
		/*
		 * Someone removed the seed device or there is an initialization
		 * order problem.
		 * XXX-mg we now have an unremovable child because the lvol will
		 * not open.
		 */
		const char *name;
		size_t len;
		int rc;

		// XXX-mg hack alert!
		// Blobstore does not like to access xattrs before the blob is
		// fully loaded.  We know that the blob is loaded far enough to
		// get xattrs, so fake the state for a bit.  It would be better
		// to have a bs-private blob_get_xattr_value that works across
		// blobstore source files.
		assert(front->state == SPDK_BLOB_STATE_LOADING);
		front->state = SPDK_BLOB_STATE_CLEAN;
		rc = spdk_blob_get_xattr_value(front, "name",
					       (const void **)&name, &len);
		front->state = SPDK_BLOB_STATE_LOADING;
		SPDK_ERRLOG("seed device %s is not found for lvol %s\n",
			    seedname, rc == 0 ? name : "<unknown>");
		return (-ENOENT);
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return (-ENOMEM);
	}

	ctx->bdev = bdev;
	ret = spdk_bdev_open_ext(seedname, false, lvol_seed_bdev_event_cb_t, ctx, &ctx->bdev_desc);
	if (ret != 0) {
		free(ctx);
		return (ret);
	}

	ctx->bdev_io_channel = spdk_bdev_get_io_channel(ctx->bdev_desc);
	if (ctx->bdev_io_channel == NULL) {
		spdk_bdev_close(ctx->bdev_desc);
		free(ctx);
	}
	// XXX-mg should set bdev->claimed, but need to coordinate among all
	// instances that have this one open.

	back = calloc(1, sizeof(*back));
	if (back == NULL) {
		spdk_bdev_close(ctx->bdev_desc);
		spdk_put_io_channel(ctx->bdev_io_channel);
		free(ctx);
		return (-ENOMEM);
	}

	back->create_channel = seed_create_channel;
	back->destroy_channel = seed_destroy_channel;
	back->destroy = seed_destroy;
	back->read = seed_read;
	back->write = seed_write;
	back->readv = seed_readv;
	back->writev = seed_writev;
	back->write_zeroes = seed_write_zeroes;
	back->unmap = seed_unmap;
	back->blockcnt = spdk_bdev_get_num_blocks(bdev);
	back->blocklen = spdk_bdev_get_block_size(bdev);
	back->seed_ctx = ctx;

	front->back_bs_dev = back;

	return 0;
}
