/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2020-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 *     * Neither the name of the copyright holder nor the names of its
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
#include "spdk/likely.h"

#include "spdk_internal/event.h"

#include "blobstore.h"

typedef void(*esnap_alloc_channels_cb_t)(void *ctx, int bserrno);

struct esnap_ctx {
	/* back_bs_dev must be first: see back_bs_dev_to_esnap_ctx() */
	struct spdk_bs_dev	back_bs_dev;

	/*
	 * io_channels, io_channels_count, and bdev_desc are used on every read.
	 * Ensure they are in the same cache line.
	 */
	struct spdk_bdev_desc	*bdev_desc __attribute__((__aligned__(SPDK_CACHE_LINE_SIZE)));
	struct spdk_io_channel	**io_channels;
	uint64_t		io_channels_count;

	/* Seldom used fields where alignment doesn't matter. */
	struct spdk_io_channel	**old_io_channels;
	struct spdk_bdev	*bdev;
	struct spdk_thread	*thread;
	char			*uuid;
	struct spdk_blob	*blob;
};

struct esnap_create_ctx {
	blob_back_bs_dev_load_done_t	load_cb;
	struct spdk_blob_load_ctx	*load_arg;
	struct esnap_ctx		*esnap_ctx;
};

static void esnap_ctx_free(struct esnap_ctx *ctx);
static void esnap_realloc_channels(void *esnap_ctx);

static struct esnap_ctx *
back_bs_dev_to_esnap_ctx(struct spdk_bs_dev *bs_dev)
{
	struct esnap_ctx	*dev = (struct esnap_ctx *)bs_dev;

	return dev;
}

static void esnap_unload_on_thread(void *_ctx)
{
	struct esnap_ctx	*esnap_ctx = _ctx;
	uint64_t		tid = spdk_thread_get_id(spdk_get_thread());

	if (tid < esnap_ctx->io_channels_count && esnap_ctx->io_channels[tid]) {
		spdk_put_io_channel(esnap_ctx->io_channels[tid]);
	}
}

static void esnap_unload_on_thread_done(void *_ctx)
{
	struct esnap_ctx	*esnap_ctx = _ctx;
	struct spdk_bdev_desc	*desc = esnap_ctx->bdev_desc;

	if (desc != NULL) {
		spdk_bdev_close(desc);
	}

	esnap_ctx_free(esnap_ctx);
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

static inline struct spdk_io_channel *
get_io_channel(struct esnap_ctx *ctx)
{
	uint64_t tid = spdk_thread_get_id(spdk_get_thread());

	if (spdk_unlikely(tid >= ctx->io_channels_count ||
			  ctx->io_channels[tid] == NULL)) {
		spdk_thread_send_msg(ctx->thread, esnap_realloc_channels, ctx);
		return NULL;
	}
	return ctx->io_channels[tid];
}

static void
esnap_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	   uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct esnap_ctx	*ctx = back_bs_dev_to_esnap_ctx(dev);
	struct spdk_io_channel	*ch;

	ch = get_io_channel(ctx);
	if (spdk_unlikely(ch == NULL)) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOMEM);
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
	struct esnap_ctx	*ctx = back_bs_dev_to_esnap_ctx(dev);
	struct spdk_io_channel	*ch;

	ch = get_io_channel(ctx);
	if (spdk_unlikely(ch == NULL)) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOMEM);
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
	struct esnap_ctx	*ctx = back_bs_dev_to_esnap_ctx(dev);
	struct spdk_io_channel	*ch;

	ch = get_io_channel(ctx);
	if (spdk_unlikely(ch == NULL)) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOMEM);
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

static struct esnap_ctx *
esnap_ctx_alloc(struct spdk_blob *blob, const char *uuid_str)
{
	struct esnap_ctx	*ctx;
	struct spdk_bs_dev	*back_bs_dev;
	int			rc;

	/*
	 * Every IO operation reads three members of ctx.  Ensure they are in
	 * the same cache line.
	 */
	rc = posix_memalign((void **)&ctx, SPDK_CACHE_LINE_SIZE, sizeof(*ctx));
	if (rc != 0) {
		return NULL;
	}
	memset(ctx, 0, sizeof(*ctx));

	ctx->thread = spdk_get_thread();
	ctx->blob = blob;
	ctx->uuid = strdup(uuid_str);
	if (ctx->uuid == NULL) {
		free(ctx);
		return NULL;
	}
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
	free(ctx->io_channels);
	free(ctx->uuid);
	free(ctx);
}

static void
esnap_open_done(void *ctx, int bserrno)
{
	struct esnap_create_ctx *create_ctx = ctx;
	struct esnap_ctx	*esnap_ctx = create_ctx->esnap_ctx;
	struct spdk_bs_dev	*dev = NULL;

	if (esnap_ctx != NULL && bserrno == 0) {
		dev = &create_ctx->esnap_ctx->back_bs_dev;
	}
	assert((dev == NULL) ^ (bserrno == 0));

	create_ctx->load_cb(create_ctx->load_arg, dev, bserrno);

	if (bserrno != 0 && esnap_ctx != NULL) {
		/* XXX-mg need to handle some channels allocated */
		esnap_ctx_free(esnap_ctx);
	}
	free(create_ctx);
}

static void
esnap_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		    void *event_ctx)
{
	struct esnap_ctx	*ctx = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		/*
		 * The bdev references (descriptor, channels) will be released
		 * when the blob is unloaded and the destroy callback is called.
		 */
		SPDK_NOTICELOG("Blob 0x%" PRIx64 " external shapshot bdev %s removed\n",
			       ctx->blob->id, bdev->name);
		break;
	default:
		SPDK_NOTICELOG("Unsupported event %d\n", type);
		break;
	}
}

struct load_on_thread_ctx {
	struct esnap_ctx		*esnap_ctx;
	esnap_alloc_channels_cb_t	cb_fn;
	void				*cb_arg;
	int				rc;
};

static void
load_esnap_on_thread(void *arg1)
{
	struct load_on_thread_ctx	*lot_ctx = arg1;
	struct esnap_ctx		*esnap_ctx = lot_ctx->esnap_ctx;
	uint64_t			tid;

	if (lot_ctx->rc != 0) {
		return;
	}

	tid = spdk_thread_get_id(spdk_get_thread());
	SPDK_INFOLOG(blob, "Creating io channel for external snapshot bdev %s on "
		     "thread %"PRIu64"\n", spdk_bdev_get_name(esnap_ctx->bdev), tid);

	if (tid >= esnap_ctx->io_channels_count) {
		/*
		 * A thread was added after io_channels was allocated and now
		 * reallocation is needed. For now, this condition will lead to
		 * a read failure on the new thread.
		 */
		return;
	}

	if (esnap_ctx->io_channels[tid] != NULL) {
		/* Must be in the midst of a realloc, this one already done. */
		return;
	}

	esnap_ctx->io_channels[tid] = spdk_bdev_get_io_channel(esnap_ctx->bdev_desc);
	if (!esnap_ctx->io_channels[tid] && !spdk_thread_is_exited(spdk_get_thread())) {
		SPDK_ERRLOG("Failed to create external snapshot bdev io_channel\n");
		/* ENOMEM is a guess. The real reason is not readily available. */
		lot_ctx->rc = -ENOMEM;
	}
}

static void
load_esnap_on_thread_done(void *arg1)
{
	struct load_on_thread_ctx	*lot_ctx = arg1;
	struct esnap_ctx		*esnap_ctx = lot_ctx->esnap_ctx;

	assert(esnap_ctx->thread == spdk_get_thread());

	free(esnap_ctx->old_io_channels);
	esnap_ctx->old_io_channels = NULL;

	lot_ctx->cb_fn(lot_ctx->cb_arg, lot_ctx->rc);
	free(lot_ctx);
}

static void
esnap_alloc_channels(struct esnap_ctx *esnap_ctx, uint64_t count,
		     esnap_alloc_channels_cb_t cb_fn, void *cb_arg)
{
	struct load_on_thread_ctx	*lot_ctx;
	uint64_t			new_count;
	struct spdk_io_channel		**new_channels;

	assert(esnap_ctx->thread == spdk_get_thread());

	if (esnap_ctx->old_io_channels != NULL) {
		/*
		 * Another instance of this function has kicked off a realloc
		 * that is still in progress.
		 */
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	lot_ctx = calloc(1, sizeof(*lot_ctx));
	if (lot_ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	lot_ctx->esnap_ctx = esnap_ctx;
	lot_ctx->cb_fn = cb_fn;
	lot_ctx->cb_arg = cb_arg;

	/* +1 since thread_id starts from 1 */
	new_count = count + 1;
	if (new_count > esnap_ctx->io_channels_count) {
		/*
		 * We need to reallocate esnap_ctx->io_channels, but can't do it
		 * with realloc(). If realloc() were used and it had to allocate
		 * copy, and free, the space it frees may be referenced by
		 * another thread running on another reactor.
		 *
		 * Instead, allocate the new space and copy existing io channel
		 * pointers to this new space. Other threads running
		 * concurrently will read from the old space during the copy.
		 * Once the copy is complete, update esnap_ctx->io_channels to
		 * reference the new space. Now any threads that are running
		 * concurrently with the rest of the processing will run on the
		 * new space.
		 *
		 * Once spdk_for_each_thread() has done its thing on each
		 * thread, we know that anyone that was running concurrently
		 * with this switcharoo has is no longer at risk of referencing
		 * old memory and the load_esnap_on_thread_done() callback can
		 * free the old memory.
		 */
		new_channels = calloc(new_count, sizeof(*esnap_ctx->io_channels));
		if (new_channels == NULL) {
			free(lot_ctx);
			cb_fn(cb_arg, -ENOMEM);
			return;
		}
		if (esnap_ctx->io_channels != NULL) {
			memcpy(new_channels, esnap_ctx->io_channels,
			       sizeof(*esnap_ctx->io_channels) * esnap_ctx->io_channels_count);
			esnap_ctx->old_io_channels = esnap_ctx->io_channels;
		}
		esnap_ctx->io_channels = new_channels;
		esnap_ctx->io_channels_count = new_count;
		/*
		 * XXX-mg: do we need a memory barrier here to make the story I
		 * told above true?  How about to ensure that the update to
		 * io_channels_count happens after io_channels?
		 */
	}

	/*
	 * Load on all threads even if we didn't realloc. It may be that there
	 * was some thread that did not initialize the first time due to
	 * implementation details of tid allocation that are abstracted away
	 * from here.
	 */
	spdk_for_each_thread(load_esnap_on_thread, lot_ctx,
			     load_esnap_on_thread_done);
}

static void
esnap_realloc_channels_done(void *_ctx, int bserrno)
{
	struct esnap_ctx *ctx = _ctx;

	if (bserrno != 0) {
		SPDK_ERRLOG("blob 0x%" PRIx64 "realloc channels failed: %d\n",
			    ctx->blob->id, bserrno);
	}
}

static void
esnap_realloc_channels(void *_ctx)
{
	uint64_t		count;
	struct esnap_ctx	*esnap_ctx = _ctx;

	/*
	 * If threads exited after a thread was added, the thread count may not
	 * indicate the highest thread id.
	 */
	count = spdk_max(spdk_thread_get_id(esnap_ctx->thread), spdk_thread_get_count());
	esnap_alloc_channels(esnap_ctx, count, esnap_realloc_channels_done, esnap_ctx);
}

/*
 * Create and open the external snapshot bdev. On successful return, the bdev
 * and bdev_desc members of esnap_ctx are updated.
 */
void
blob_create_esnap_dev(struct spdk_blob *blob, const char *uuid_str,
		      blob_back_bs_dev_load_done_t load_cb,
		      struct spdk_blob_load_ctx *load_cb_arg)
{
	uint32_t		io_unit_size = blob->bs->io_unit_size;
	struct spdk_bdev	*bdev;
	struct spdk_bs_dev	*back_bs_dev;
	struct esnap_create_ctx	*create_ctx;
	struct esnap_ctx	*esnap_ctx;
	int			rc;

	if (blob == NULL || uuid_str == NULL || load_cb == NULL) {
		SPDK_ERRLOG("Required parameter(s) missing)\n");
		abort();
	}

	create_ctx = calloc(1, sizeof(*create_ctx));
	if (create_ctx == NULL) {
		load_cb(load_cb_arg, NULL, -ENOMEM);
		return;
	}

	create_ctx->load_cb = load_cb;
	create_ctx->load_arg = load_cb_arg;

	esnap_ctx = esnap_ctx_alloc(blob, uuid_str);
	if (esnap_ctx == NULL) {
		load_cb(load_cb_arg, NULL, -ENOMEM);
		return;
	}
	create_ctx->esnap_ctx = esnap_ctx;

	rc = spdk_bdev_open_ext(esnap_ctx->uuid, false, esnap_bdev_event_cb,
				esnap_ctx, &esnap_ctx->bdev_desc);
	if (rc != 0) {
		esnap_open_done(create_ctx, rc);
		return;
	}
	bdev = spdk_bdev_desc_get_bdev(esnap_ctx->bdev_desc);
	if (bdev == NULL) {
		SPDK_ERRLOG("Unable to get bdev from bdev_desc\n");
		abort();
	}
	esnap_ctx->bdev = bdev;

	back_bs_dev = &esnap_ctx->back_bs_dev;
	back_bs_dev->blockcnt = spdk_bdev_get_num_blocks(bdev);
	back_bs_dev->blocklen = spdk_bdev_get_block_size(bdev);

	if (back_bs_dev->blocklen > io_unit_size) {
		SPDK_ERRLOG("External snapshot device %s block size %" PRIu32
			    " larger than blobstore io_unit_size %" PRIu32 "\n",
			    bdev->name, back_bs_dev->blocklen, io_unit_size);
		esnap_open_done(create_ctx, -EINVAL);
		return;
	}

	esnap_alloc_channels(esnap_ctx, spdk_thread_get_count(),
			     esnap_open_done, create_ctx);
}
