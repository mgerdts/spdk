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

#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "bdev/ro/bdev_ro.h"

#include "spdk_internal/event.h"

#include "blobstore.h"

struct esnap_ctx {
	/* back_bs_dev must be first: see BACK_BS_DEV_TO_ESNAP_CTX() */
	struct spdk_bs_dev	back_bs_dev;

	/*
	 * io_channels, io_channels_count, and bdev_desc are used on every read.
	 * Ensure they are in the same cache line. rc is here to help with
	 * alignment.
	 */
	int			rc;
	struct spdk_bdev_desc	*bdev_desc;
	struct spdk_io_channel	**io_channels;
	uint64_t		io_channels_count;

	/* Seldom used fields where alignment doesn't matter. */
	struct spdk_thread	*thread;
	struct spdk_uuid	esnap_uuid;
	struct spdk_bdev	*bdev;
	struct spdk_blob	*blob;
	esnap_parent_cb_t	parent_cb;
	void			*parent_ctx;
};

#define CPU_CACHE_LINE_SIZE 64
SPDK_STATIC_ASSERT(offsetof(struct esnap_ctx, bdev_desc) % CPU_CACHE_LINE_SIZE == 0,
		   "bdev_desc must be aligned on cache line");

#define BACK_BS_DEV_TO_ESNAP_CTX(dev) ((struct esnap_ctx *)dev)

static void esnap_unload_on_thread(void *_ctx)
{
	struct esnap_ctx *esnap_ctx = BACK_BS_DEV_TO_ESNAP_CTX(_ctx);
	uint64_t tid = spdk_thread_get_id(spdk_get_thread());

	if (tid <= esnap_ctx->io_channels_count && esnap_ctx->io_channels[tid]) {
		spdk_put_io_channel(esnap_ctx->io_channels[tid]);
	}
}

static void esnap_unload_on_thread_done(void *_ctx)
{
	struct esnap_ctx *esnap_ctx = BACK_BS_DEV_TO_ESNAP_CTX(_ctx);
	struct spdk_bdev_desc *ro_desc = esnap_ctx->bdev_desc;

	if (ro_desc != NULL) {
		struct spdk_bdev *ro_bdev = spdk_bdev_desc_get_bdev(ro_desc);

		spdk_bdev_close(ro_desc);
		delete_ro_disk(ro_bdev, NULL, NULL);
	}

	free(esnap_ctx->io_channels);
	free(esnap_ctx);
}

static void
esnap_destroy(struct spdk_bs_dev *dev)
{
	if (dev != NULL) {
		spdk_for_each_thread(esnap_unload_on_thread, dev, esnap_unload_on_thread_done);
	}
}

static void
esnap_complete(struct spdk_bdev_io *bdev_io, bool success, void *args)
{
	struct spdk_bs_dev_cb_args *cb_args = args;

	spdk_bdev_free_io(bdev_io);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, success ? 0 : -EIO);
}

static void
esnap_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	  uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct esnap_ctx *ctx = BACK_BS_DEV_TO_ESNAP_CTX(dev);
	uint64_t tid;
	struct spdk_io_channel *ch;

	tid = spdk_thread_get_id(spdk_get_thread());
	if (tid > ctx->io_channels_count || ctx->bdev_desc == NULL ||
	    (ch = ctx->io_channels[tid]) == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_read_blocks(ctx->bdev_desc, ch, payload, lba, lba_count, esnap_complete, cb_args);
}

static void
esnap_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	   uint64_t lba, uint32_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
esnap_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	   struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	struct esnap_ctx *ctx = BACK_BS_DEV_TO_ESNAP_CTX(dev);
	uint64_t tid;
	struct spdk_io_channel *ch;

	tid = spdk_thread_get_id(spdk_get_thread());
	if (tid > ctx->io_channels_count || ctx->bdev_desc == NULL ||
	    (ch = ctx->io_channels[tid]) == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_readv_blocks(ctx->bdev_desc, ch, iov, iovcnt, lba, lba_count, esnap_complete, cb_args);
}

static void
esnap_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	       struct iovec *iov, int iovcnt,
	       uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
	       struct spdk_blob_ext_io_opts *ext_io_opts)
{
	struct esnap_ctx *ctx = BACK_BS_DEV_TO_ESNAP_CTX(dev);
	uint64_t tid;
	struct spdk_io_channel *ch;

	tid = spdk_thread_get_id(spdk_get_thread());
	if (tid > ctx->io_channels_count || ctx->bdev_desc == NULL ||
	    (ch = ctx->io_channels[tid]) == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_readv_blocks_ext(ctx->bdev_desc, ch, iov, iovcnt, lba, lba_count,
				   esnap_complete, cb_args, (struct spdk_bdev_ext_io_opts *)ext_io_opts);
}

static void
esnap_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	    struct iovec *iov, int iovcnt,
	    uint64_t lba, uint32_t lba_count,
	    struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
esnap_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		struct iovec *iov, int iovcnt,
		uint64_t lba, uint32_t lba_count,
		struct spdk_bs_dev_cb_args *cb_args,
		struct spdk_blob_ext_io_opts *ext_io_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
esnap_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		  uint64_t lba, uint64_t lba_count,
		  struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
esnap_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	   uint64_t lba, uint64_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

/*
 * XXX-mg
 * 1. Rename the function to show that it is only called for ro devices.
 * 2. Remove should create a wait bdev and install it. The parent callback
 *    should call destroy().  Be sure this is done on the right thread.
 * 3. Create something similar for the callback that is passed to the wait bdev.
 */
static void
esnap_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	struct esnap_ctx *ctx = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		spdk_bdev_close(ctx->bdev_desc);
		ctx->bdev_desc = NULL;
		break;
	default:
		SPDK_NOTICELOG("Unsupported event %d\n", type);
		break;
	}
}

static struct esnap_ctx *
esnap_ctx_alloc(struct spdk_blob *blob, const struct spdk_uuid *esnap_uuid,
		esnap_parent_cb_t cb_fn, void *cb_arg)
{
	struct esnap_ctx	*ctx;
	struct spdk_bs_dev	*back_bs_dev;
	int			rc;

	rc = (int) offsetof(struct esnap_ctx, bdev_desc);
	/*
	 * Every IO operation reads three members of ctx.  Ensure they are in
	 * the same cache line.
	 */
	rc = posix_memalign((void **)&ctx, CPU_CACHE_LINE_SIZE, sizeof(*ctx));
	if (rc != 0) {
		return NULL;
	}
	memset(ctx, 0, sizeof (*ctx));

	ctx->thread = spdk_get_thread();
	ctx->blob = blob;
	ctx->esnap_uuid = *esnap_uuid;
	ctx->parent_cb = cb_fn;
	ctx->parent_ctx = cb_arg;

	back_bs_dev = &ctx->back_bs_dev;
	back_bs_dev->create_channel = NULL;
	back_bs_dev->destroy_channel = NULL;
	back_bs_dev->destroy = esnap_destroy;
	back_bs_dev->read = esnap_read;
	back_bs_dev->write = esnap_write;
	back_bs_dev->readv = esnap_readv;
	back_bs_dev->readv_ext = esnap_readv_ext;
	back_bs_dev->writev = esnap_writev;
	back_bs_dev->writev_ext = esnap_writev_ext;
	back_bs_dev->write_zeroes = esnap_write_zeroes;
	back_bs_dev->unmap = esnap_unmap;

	return ctx;
}

static void
esnap_ctx_free(struct esnap_ctx *ctx)
{
	free(ctx);
}

static void
esnap_open_wait(struct spdk_blob *blob, const struct spdk_uuid *esnap_uuid,
		esnap_parent_cb_t cb_fn, void *cb_arg)
{
	// XXX-mg implement
	cb_fn(cb_arg, NULL, -ENOSYS);
}

static void
esnap_maybe_open_wait(struct spdk_blob *blob, const struct spdk_uuid *esnap_uuid,
		      esnap_parent_cb_t cb_fn, void *cb_arg, int errnum)
{
	char uuid[SPDK_UUID_STRING_LEN];

	switch (errnum) {
	case 0:
		SPDK_ERRLOG("Invalid errnum 0\n");
		abort();
	case -ENODEV:
	case -EAGAIN:
		/* XXX-mg Do not create another wait if already waiting. */

		spdk_uuid_fmt_lower(uuid, sizeof(uuid), esnap_uuid);
		SPDK_NOTICELOG("Failed to open external snapshot for blob 0x%"
			       PRIx64 ": error %d. Will try again when bdev "
			       "with UUID %s is registered\n",
			       spdk_blob_get_id(blob), errnum, uuid);
		esnap_open_wait(blob, esnap_uuid, cb_fn, cb_arg);
		return;
	default:
		spdk_uuid_fmt_lower(uuid, sizeof(uuid), esnap_uuid);
		SPDK_ERRLOG("Failed to open external snapshot for blob 0x%" PRIx64
			    ": error %d.\n", spdk_blob_get_id(blob), errnum);
		cb_fn(cb_arg, NULL, errnum);
		return;
	}
}

static void
load_esnap_on_thread(void *arg1)
{
	struct esnap_ctx *esnap_ctx = arg1;
	uint64_t tid;

	if (esnap_ctx->rc != 0) {
		return;
	}

	tid = spdk_thread_get_id(spdk_get_thread());
	SPDK_INFOLOG(blob, "Creating io channel for external snapshot bdev %s on "
		     "thread %"PRIu64"\n", spdk_bdev_get_name(esnap_ctx->bdev), tid);

	if (tid > esnap_ctx->io_channels_count) {
		void *tmp;

		/* realloc needed */
		SPDK_INFOLOG(blob, "Realloc from %zu to %zu\n",
			     esnap_ctx->io_channels_count, tid + 1);
		tmp = realloc(esnap_ctx->io_channels,
			      (tid + 1) * sizeof(*esnap_ctx->io_channels));
		if (!tmp) {
			SPDK_ERRLOG("Memory allocation failed\n");
			esnap_ctx->rc = -ENOMEM;
			return;
		}
		esnap_ctx->io_channels = tmp;
		esnap_ctx->io_channels_count = tid + 1;
	}

	esnap_ctx->io_channels[tid] = spdk_bdev_get_io_channel(esnap_ctx->bdev_desc);
	if (!esnap_ctx->io_channels[tid]) {
		SPDK_ERRLOG("Failed to create external snapshot bdev io_channel\n");
		esnap_ctx->rc = -ENOMEM;
	}
}

static void
load_esnap_on_thread_done(void *arg1)
{
	struct esnap_ctx	*esnap_ctx = arg1;
	struct spdk_bs_dev	*bs_dev = &esnap_ctx->back_bs_dev;

	assert(esnap_ctx->thread == spdk_get_thread());
	assert(esnap_ctx->parent_cb != NULL);

	if (esnap_ctx->rc != 0) {
		esnap_maybe_open_wait(esnap_ctx->blob, &esnap_ctx->esnap_uuid,
				      esnap_ctx->parent_cb,
				      esnap_ctx->parent_ctx, esnap_ctx->rc);
		return;
	}
	esnap_ctx->parent_cb(esnap_ctx->parent_ctx, bs_dev, esnap_ctx->rc);
}

/*
 * Create and open a read-only bdev and open it. On succesful return, the bdev
 * and bdev_desc members of esnap_ctx are updated.
 */
static void
esnap_open_ro(struct spdk_blob *blob, const struct spdk_uuid *esnap_uuid,
	      esnap_parent_cb_t cb_fn, void *cb_arg)
{
	uint32_t		io_unit_size = blob->bs->io_unit_size;
	struct spdk_bdev	*ro_bdev, *base_bdev;
	struct spdk_bs_dev	*back_bs_dev;
	struct esnap_ctx	*esnap_ctx;
	int			rc;

	assert(blob->back_bs_dev == NULL ||
	       BACK_BS_DEV_TO_ESNAP_CTX(blob->back_bs_dev)->thread == spdk_get_thread());

	esnap_ctx = esnap_ctx_alloc(blob, esnap_uuid, cb_fn, cb_arg);
	if (esnap_ctx == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	rc = create_ro_disk(NULL, esnap_uuid, NULL, &ro_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to create external snapshot: error %d\n", rc);
		esnap_ctx_free(esnap_ctx);
		esnap_maybe_open_wait(blob, esnap_uuid, cb_fn, cb_arg, rc);
		return;
	}
	esnap_ctx->bdev = ro_bdev;

	base_bdev = bdev_ro_get_base_bdev(ro_bdev);
	if (base_bdev == NULL) {
		SPDK_ERRLOG("Unable to get base bdev from nearly created ro bdev %s\n",
			    spdk_bdev_get_name(ro_bdev));
		abort();
	}

	back_bs_dev = &esnap_ctx->back_bs_dev;
	back_bs_dev->blockcnt = spdk_bdev_get_num_blocks(ro_bdev);
	back_bs_dev->blocklen = spdk_bdev_get_block_size(ro_bdev);

	if (back_bs_dev->blocklen > io_unit_size) {
		SPDK_ERRLOG("External snapshot device %s block size %" PRIu32
			    " larger than blobstore io_unit_size %" PRIu32 "\n",
			    base_bdev->name, back_bs_dev->blocklen, io_unit_size);
		delete_ro_disk(ro_bdev, NULL, NULL);
		/*
		 * EAGAIN because if the base bdev could be replaced with a
		 * target with a more suitable block size. When such a
		 * replacement is registered, it should be automatically used
		 * without having to reload the blobstore.
		 */
		esnap_maybe_open_wait(blob, esnap_uuid, cb_fn, cb_arg, -EAGAIN);
		return;
	}

	rc = spdk_bdev_open_ext(ro_bdev->name, false, esnap_bdev_event_cb,
				esnap_ctx, &esnap_ctx->bdev_desc);
	if (rc != 0) {
		delete_ro_disk(ro_bdev, NULL, NULL);
		esnap_ctx_free(esnap_ctx);
		esnap_maybe_open_wait(blob, esnap_uuid, cb_fn, cb_arg, rc);
		return;
	}

	/* +1 since thread_id starts from 1 */
	esnap_ctx->io_channels_count = spdk_thread_get_count() + 1;
	esnap_ctx->io_channels = calloc(esnap_ctx->io_channels_count,
				        sizeof(*esnap_ctx->io_channels));
	if (esnap_ctx->io_channels == NULL) {
		delete_ro_disk(ro_bdev, NULL, NULL);
		esnap_ctx_free(esnap_ctx);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	SPDK_NOTICELOG("Created read-only device %s with base bdev %s\n",
		       ro_bdev->name, base_bdev->name);

	spdk_for_each_thread(load_esnap_on_thread, esnap_ctx,
			     load_esnap_on_thread_done);
}

void
bs_create_esnap_dev(struct spdk_blob *blob, const struct spdk_uuid *esnap_uuid,
		   esnap_parent_cb_t cb_fn, void *cb_arg)
{
	esnap_open_ro(blob, esnap_uuid, cb_fn, cb_arg);
}
