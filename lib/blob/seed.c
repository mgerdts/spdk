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

#include "spdk_internal/event.h"

#include "blobstore.h"

/*
 * The seed vbdev module exists so that it may claim external snapshots.
 */
static int
vbdev_seed_init(void) {
	return 0;
}

static struct spdk_bdev_module seed_if = {
	.name = "seed",
	.module_init = vbdev_seed_init,
};

SPDK_BDEV_MODULE_REGISTER(seed, &seed_if);

struct seed_claim {
	const struct spdk_bdev *bdev;
	const struct spdk_uuid *uuid;
	uint32_t count;
	RB_ENTRY(seed_claim) node;
};

static int
seed_claim_cmp(struct seed_claim *claim1, struct seed_claim *claim2)
{
	return spdk_uuid_compare(claim1->uuid, claim2->uuid);
}

RB_HEAD(seed_claims_tree, seed_claim) g_seed_claims = RB_INITIALIZER(&head);
RB_GENERATE_STATIC(seed_claims_tree, seed_claim, node, seed_claim_cmp);
pthread_mutex_t g_seed_claims_mutex;

static int
vbdev_seed_claim_bdev(struct spdk_bdev *bdev)
{
	struct seed_claim *claim, *existing;
	struct seed_claim find = {};
	int ret;

	find.uuid = spdk_bdev_get_uuid(bdev);

	pthread_mutex_lock(&g_seed_claims_mutex);

	claim = RB_FIND(seed_claims_tree, &g_seed_claims, &find);
	if (claim != NULL) {
		claim->count++;
		ret = 0;
		goto done;
	}

	ret = spdk_bdev_module_claim_bdev(bdev, NULL, &seed_if);
	if (ret != 0) {
		goto done;
	}
	claim = calloc(1, sizeof (*claim));
	if (claim == NULL) {
		ret = -ENOMEM;
		goto done;
	}
	claim->bdev = bdev;
	claim->uuid = find.uuid;
	claim->count = 1;

	existing = RB_INSERT(seed_claims_tree, &g_seed_claims, claim);
	if (existing != NULL) {
		SPDK_ERRLOG("collision while inserting claim for bdev %s\n",
			    spdk_bdev_get_name(bdev));
		abort();
	}
	assert(existing == NULL);
	ret = 0;

done:
	pthread_mutex_unlock(&g_seed_claims_mutex);
	return ret;
}

static void
vbdev_seed_release_bdev(struct spdk_bdev *bdev)
{
	struct seed_claim *claim;
	struct seed_claim find = {};

	find.uuid = spdk_bdev_get_uuid(bdev);

	pthread_mutex_lock(&g_seed_claims_mutex);

	claim = RB_FIND(seed_claims_tree, &g_seed_claims, &find);
	if (claim == NULL) {
		SPDK_ERRLOG("trying to release bdev %s but not in claim tree\n",
			    spdk_bdev_get_name(bdev));
		abort();
	}
	if (claim->count == 0) {
		SPDK_ERRLOG("trying to release bdev %s with count = 0\n",
			    spdk_bdev_get_name(bdev));
		abort();
	}

	claim->count--;
	if (claim->count == 0) {
		RB_REMOVE(seed_claims_tree, &g_seed_claims, claim);
		free(claim);
		spdk_bdev_module_release_bdev(bdev);
	}

	pthread_mutex_unlock(&g_seed_claims_mutex);
}

static void seed_unload_on_thread(void *_ctx)
{
	struct seed_ctx *ctx = _ctx;
	uint64_t tid = spdk_thread_get_id(spdk_get_thread());

	if (tid <= ctx->io_channels_count && ctx->io_channels[tid]) {
		spdk_put_io_channel(ctx->io_channels[tid]);
	}
}

static void seed_unload_on_thread_done(void *_ctx)
{
	struct seed_ctx *ctx = _ctx;

	free(ctx->io_channels);
	// XXX-mg free(dev->seed_ctx)?
	free(ctx);
}

static void
seed_destroy(struct spdk_bs_dev *dev)
{
	struct seed_ctx *ctx = dev->seed_ctx;

	if (ctx != NULL) {
		vbdev_seed_release_bdev(ctx->bdev);
		ctx->bdev = NULL;
		spdk_bdev_close(ctx->bdev_desc);
		ctx->bdev_desc = NULL;

		spdk_for_each_thread(seed_unload_on_thread, dev->seed_ctx,
				     seed_unload_on_thread_done);
	}
	free(dev);
}

static void
seed_complete(struct spdk_bdev_io *bdev_io, bool success, void *args)
{
	struct spdk_bs_dev_cb_args *cb_args = args;

	spdk_bdev_free_io(bdev_io);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, success ? 0 : -EIO);
}

static void
seed_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	  uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct seed_ctx *ctx = dev->seed_ctx;
	uint64_t tid;
	struct spdk_io_channel *ch;

	tid = spdk_thread_get_id(spdk_get_thread());
	if (tid > ctx->io_channels_count || ctx->bdev_desc == NULL ||
	    (ch = ctx->io_channels[tid]) == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_read_blocks(ctx->bdev_desc, ch, payload, lba, lba_count, seed_complete, cb_args);
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
	uint64_t tid;
	struct spdk_io_channel *ch;

	tid = spdk_thread_get_id(spdk_get_thread());
	if (tid > ctx->io_channels_count || ctx->bdev_desc == NULL ||
	    (ch = ctx->io_channels[tid]) == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_readv_blocks(ctx->bdev_desc, ch, iov, iovcnt, lba, lba_count, seed_complete, cb_args);
}

static void
seed_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	       struct iovec *iov, int iovcnt,
	       uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
	       struct spdk_blob_ext_io_opts *ext_io_opts)
{
	struct seed_ctx *ctx = dev->seed_ctx;
	uint64_t tid;
	struct spdk_io_channel *ch;

	tid = spdk_thread_get_id(spdk_get_thread());
	if (tid > ctx->io_channels_count || ctx->bdev_desc == NULL ||
	    (ch = ctx->io_channels[tid]) == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_readv_blocks_ext(ctx->bdev_desc, ch, iov, iovcnt, lba, lba_count,
				   seed_complete, cb_args, (struct spdk_bdev_ext_io_opts *)ext_io_opts);
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
seed_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		struct iovec *iov, int iovcnt,
		uint64_t lba, uint32_t lba_count,
		struct spdk_bs_dev_cb_args *cb_args,
		struct spdk_blob_ext_io_opts *ext_io_opts)
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

static void
seed_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	struct seed_ctx *ctx = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		// XXX-mg what about channel?
		spdk_bdev_close(ctx->bdev_desc);
		ctx->bdev_desc = NULL;
		break;
	default:
		SPDK_NOTICELOG("Unsupported event %d\n", type);
		break;
	}
}

struct seed_bs_load_cpl_ctx {
	blob_load_seed_cpl cb_fn;
	void *cb_arg;
	struct seed_ctx *ctx;
	int rc;
};

static void load_seed_on_thread(void *arg1)
{
	struct seed_bs_load_cpl_ctx *ctx = arg1;
	uint64_t tid;

	if (ctx->rc) {
		return;
	}

	tid = spdk_thread_get_id(spdk_get_thread());
	SPDK_INFOLOG(blob, "Creating io channel for seed bdev %s on thread %"PRIu64"\n",
		       spdk_bdev_get_name(ctx->ctx->bdev), tid);

	if (tid > ctx->ctx->io_channels_count) {
		/* realloc needed */
		SPDK_INFOLOG(blob, "Realloc from %zu to %zu\n", ctx->ctx->io_channels_count, tid + 1);
		void *tmp = realloc(ctx->ctx->io_channels, (tid + 1) * sizeof(*ctx->ctx->io_channels));
		if (!tmp) {
			SPDK_ERRLOG("Memory allocation failed\n");
			ctx->rc = -ENOMEM;
			return;
		}
		ctx->ctx->io_channels = tmp;
		ctx->ctx->io_channels_count = tid + 1;
	}

	ctx->ctx->io_channels[tid] = spdk_bdev_get_io_channel(ctx->ctx->bdev_desc);
	if (!ctx->ctx->io_channels[tid]) {
		SPDK_ERRLOG("Failed to create seed bdev io_channel\n");
		ctx->rc = -ENOMEM;
	}
}

static void load_seed_on_thread_done(void *arg1)
{
	struct seed_bs_load_cpl_ctx *ctx = arg1;

	assert(ctx->cb_fn);
	ctx->cb_fn(ctx->cb_arg, ctx->rc);
	free(ctx);
}

void
bs_create_seed_dev(struct spdk_blob *front, const char *seed_uuid,
		   blob_load_seed_cpl cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;
	struct spdk_bs_dev *back;
	struct seed_ctx *ctx;
	struct seed_bs_load_cpl_ctx *seed_load_cpl;
	int rc;

	bdev = spdk_bdev_get_by_uuid(seed_uuid);
	if (bdev == NULL) {
		/*
		 * Someone removed the seed device or there is an initialization
		 * order problem.
		 * XXX-mg we now have an unremovable child because the blob will
		 * not open.
		 */
		SPDK_ERRLOG("seed device %s is not found for blob 0x%" PRIx64 "\n",
			    seed_uuid, front->id);
		cb_fn(cb_arg, -ENOENT);
		return;
	}

	if (spdk_bdev_get_block_size(bdev) > front->bs->io_unit_size) {
		SPDK_ERRLOG("seed device %s (%s) block size %" PRIu32
			    " larger than blobstore io_unit_size %" PRIu32 "\n",
			    spdk_bdev_get_name(bdev), seed_uuid,
			    spdk_bdev_get_block_size(bdev), front->bs->io_unit_size);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	seed_load_cpl = calloc(1, sizeof(*seed_load_cpl));
	if (!seed_load_cpl) {
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	/* +1 since thread_id starts from 1 */
	ctx->io_channels_count = spdk_thread_get_count() + 1;
	ctx->io_channels = calloc(ctx->io_channels_count, sizeof(*ctx->io_channels));
	if (!ctx->io_channels) {
		free(ctx);
		free(seed_load_cpl);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bdev = bdev;
	rc = spdk_bdev_open_ext(bdev->name, false, seed_bdev_event_cb, ctx, &ctx->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("seed device %s (uuid %s) could not be opened for blob 0x%"
			    PRIx64 ": error %d\n", bdev->name, seed_uuid, front->id, rc);
		free(ctx->io_channels);
		free(ctx);
		free(seed_load_cpl);
		cb_fn(cb_arg, rc);
		return;
	}

	rc = vbdev_seed_claim_bdev(bdev);
	if (rc != 0) {
		free(ctx->io_channels);
		free(ctx);
		free(seed_load_cpl);
		cb_fn(cb_arg, -EINVAL);
		return;
	}


	back = calloc(1, sizeof(*back));
	if (back == NULL) {
		vbdev_seed_release_bdev(bdev);
		spdk_bdev_close(ctx->bdev_desc);
		free(ctx->io_channels);
		free(ctx);
		free(seed_load_cpl);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	back->create_channel = NULL;
	back->destroy_channel = NULL;
	back->destroy = seed_destroy;
	back->read = seed_read;
	back->write = seed_write;
	back->readv = seed_readv;
	back->readv_ext = seed_readv_ext;
	back->writev = seed_writev;
	back->writev_ext = seed_writev_ext;
	back->write_zeroes = seed_write_zeroes;
	back->unmap = seed_unmap;
	back->blockcnt = spdk_bdev_get_num_blocks(bdev);
	back->blocklen = spdk_bdev_get_block_size(bdev);
	back->seed_ctx = ctx;

	front->back_bs_dev = back;

	seed_load_cpl->cb_fn = cb_fn;
	seed_load_cpl->cb_arg = cb_arg;
	seed_load_cpl->ctx = ctx;

	spdk_for_each_thread(load_seed_on_thread, seed_load_cpl, load_seed_on_thread_done);
}
