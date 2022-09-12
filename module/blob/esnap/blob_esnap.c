/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2020-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/blob_esnap.h"
#include "spdk/likely.h"

#include "spdk_internal/event.h"
#define __SPDK_BDEV_MODULE_ONLY
#include "spdk/bdev_module.h"

struct esnap_ctx {
	/* back_bs_dev must be first: see back_bs_dev_to_esnap_ctx() */
	struct spdk_bs_dev	back_bs_dev;

	struct spdk_bdev_desc	*bdev_desc;

	/* Seldom used fields where alignment doesn't matter. */
	struct spdk_bdev	*bdev;
	struct spdk_thread	*thread;
	char			*uuid;
	struct spdk_blob	*blob;
	struct spdk_blob_store	*bs;
};

struct esnap_create_ctx {
	blob_back_bs_dev_load_done_t	load_cb;
	struct spdk_blob_load_ctx	*load_arg;
	struct esnap_ctx		*esnap_ctx;
};

static void esnap_ctx_free(struct esnap_ctx *ctx);
static int esnap_unclaim_bdev(struct spdk_bdev *bdev);

static struct esnap_ctx *
back_bs_dev_to_esnap_ctx(struct spdk_bs_dev *bs_dev)
{
	struct esnap_ctx	*dev = (struct esnap_ctx *)bs_dev;

	return dev;
}

static int
esnap_channel_compare(struct bs_esnap_channel *c1, struct bs_esnap_channel *c2)
{
	return (c1->blob_id < c2->blob_id ? -1 : c1->blob_id > c2->blob_id);
}

RB_GENERATE(bs_esnap_channel_tree, bs_esnap_channel, node, esnap_channel_compare)

static void
esnap_destroy_channels_done(struct spdk_io_channel_iter *i, int status)
{
	struct esnap_ctx	*esnap_ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_bdev_desc	*desc = esnap_ctx->bdev_desc;

	if (desc != NULL) {
		esnap_unclaim_bdev(esnap_ctx->bdev);
		spdk_bdev_close(desc);
	}

	esnap_ctx_free(esnap_ctx);
}

static void
esnap_destroy_one_channel(struct spdk_io_channel_iter *i)
{
	struct esnap_ctx	*esnap_ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel	*channel = spdk_io_channel_iter_get_channel(i);;
	struct spdk_bs_channel	*bs_channel = spdk_io_channel_get_ctx(channel);
	struct spdk_esnap_channels *esnap_channels = spdk_esnap_channels_get(bs_channel);
	struct bs_esnap_channel	*esnap_channel;
	struct bs_esnap_channel	find = {};

	find.blob_id = spdk_blob_get_id(esnap_ctx->blob);
	esnap_channel = RB_FIND(bs_esnap_channel_tree, &esnap_channels->tree, &find);
	if (esnap_channel != NULL) {
		RB_REMOVE(bs_esnap_channel_tree, &esnap_channels->tree, esnap_channel);
		spdk_put_io_channel(esnap_channel->channel);
		free(esnap_channel);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
esnap_destroy(struct spdk_bs_dev *dev)
{
	struct esnap_ctx *ctx = back_bs_dev_to_esnap_ctx(dev);

	spdk_for_each_channel(ctx->bs, esnap_destroy_one_channel,
			      ctx, esnap_destroy_channels_done);
}

void
spdk_bs_esnap_init_channels(struct spdk_esnap_channels *esnap_channels)
{
	RB_INIT(&esnap_channels->tree);
}
void
spdk_bs_esnap_destroy_channels(struct spdk_esnap_channels *esnap_channels)
{
	struct bs_esnap_channel *esnap_channel, *esnap_channel_tmp;

	RB_FOREACH_SAFE(esnap_channel, bs_esnap_channel_tree, &esnap_channels->tree,
			esnap_channel_tmp) {
		RB_REMOVE(bs_esnap_channel_tree, &esnap_channels->tree, esnap_channel);
		spdk_put_io_channel(esnap_channel->channel);
		free(esnap_channel);
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
get_io_channel(struct spdk_io_channel *ch, struct esnap_ctx *ctx)
{
	struct spdk_bs_channel	*bs_channel = spdk_io_channel_get_ctx(ch);
	struct spdk_esnap_channels *esnap_channels = spdk_esnap_channels_get(bs_channel);
	struct bs_esnap_channel	find = {};
	struct bs_esnap_channel	*esnap_channel;

	find.blob_id = spdk_blob_get_id(ctx->blob);
	esnap_channel = RB_FIND(bs_esnap_channel_tree, &esnap_channels->tree, &find);
	if (spdk_likely(esnap_channel != NULL)) {
		return esnap_channel->channel;
	}

	esnap_channel = calloc(1, sizeof(*esnap_channel));
	if (esnap_channel == NULL) {
		SPDK_NOTICELOG("blob 0x%" PRIx64 " channel allocation failed: no memory\n",
			       find.blob_id);
		return NULL;
	}
	esnap_channel->channel = spdk_bdev_get_io_channel(ctx->bdev_desc);
	if (esnap_channel->channel == NULL) {
		SPDK_NOTICELOG("blob 0x%" PRIx64 " back channel allocation failed\n",
			       find.blob_id);
		free(esnap_channel);
		return NULL;
	}
	esnap_channel->blob_id = find.blob_id;
	RB_INSERT(bs_esnap_channel_tree, &esnap_channels->tree, esnap_channel);

	return esnap_channel->channel;
}

static void
esnap_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	   uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct esnap_ctx	*ctx = back_bs_dev_to_esnap_ctx(dev);
	struct spdk_io_channel	*ch;

	ch = get_io_channel(channel, ctx);
	if (spdk_unlikely(ch == NULL)) {
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
	struct esnap_ctx	*ctx = back_bs_dev_to_esnap_ctx(dev);
	struct spdk_io_channel	*ch;

	ch = get_io_channel(channel, ctx);
	if (spdk_unlikely(ch == NULL)) {
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
	struct esnap_ctx	*ctx = back_bs_dev_to_esnap_ctx(dev);
	struct spdk_io_channel	*ch;

	ch = get_io_channel(channel, ctx);
	if (spdk_unlikely(ch == NULL)) {
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

static bool
esnap_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	return false;
}

/*
 * Claims
 *
 * Every esnap bdev is claimed by a fake "esnap" bdev module while it is open
 * for reads.  A single read-only claim is taken on the bdev and that claim is
 * reference counted for all blobs across all blobstores that use it.
 */
struct esnap_claim {
	RB_ENTRY(esnap_claim)	entry;
	struct spdk_bdev	*bdev;
	uint32_t		refs;
};

static int
esnap_claim_compare(struct esnap_claim *c1, struct esnap_claim *c2)
{
	return (c1->bdev < c2->bdev ? -1 : c1->bdev > c2->bdev);
}

RB_HEAD(esnap_claim_tree, esnap_claim) esnap_claims = RB_INITIALIZER(&esnap_claims);
RB_GENERATE_STATIC(esnap_claim_tree, esnap_claim, entry, esnap_claim_compare)

struct spdk_bdev_module esnap_module = {
	.name = "esnap"
};

pthread_mutex_t claim_mutex = PTHREAD_MUTEX_INITIALIZER;

static int
esnap_claim_bdev(struct spdk_bdev *bdev)
{
	struct esnap_claim	find = { .bdev = bdev };
	struct esnap_claim	*claim, *existing;
	int rc;

	pthread_mutex_lock(&claim_mutex);

	/* Typical case: increment claim count by one. */
	claim = RB_FIND(esnap_claim_tree, &esnap_claims, &find);
	if (spdk_likely(claim != NULL)) {
		claim->refs++;
		pthread_mutex_unlock(&claim_mutex);
		return 0;
	}

	/* Need a new claim. Drop lock across memory allocation. */
	pthread_mutex_unlock(&claim_mutex);
	claim = calloc(1, sizeof(*claim));
	if (claim == NULL) {
		return -ENOMEM;
	}
	claim->bdev = bdev;
	claim->refs = 1;

	/* Try to add the claim with the lock held. We may have lost a race. */
	pthread_mutex_lock(&claim_mutex);
	existing = RB_INSERT(esnap_claim_tree, &esnap_claims, claim);
	if (existing == NULL) {
		rc = spdk_bdev_module_claim_bdev(bdev, NULL, &esnap_module);
		if (rc != 0) {
			RB_REMOVE(esnap_claim_tree, &esnap_claims, claim);
			pthread_mutex_unlock(&claim_mutex);
			free(claim);
			SPDK_ERRLOG("could not claim bdev %s: %d\n",
				    spdk_bdev_get_name(bdev), rc);
			return rc;
		}

		pthread_mutex_unlock(&claim_mutex);
		return 0;
	}

	/* We lost the race. Increment the claim taken by someone else. */
	existing->refs++;
	pthread_mutex_unlock(&claim_mutex);
	free(claim);

	return 0;
}

static int
esnap_unclaim_bdev(struct spdk_bdev *bdev)
{
	struct esnap_claim	find = { .bdev = bdev };
	struct esnap_claim	*claim;

	pthread_mutex_lock(&claim_mutex);
	claim = RB_FIND(esnap_claim_tree, &esnap_claims, &find);
	if (claim == NULL) {
		pthread_mutex_unlock(&claim_mutex);
		SPDK_ERRLOG("Missing claim for bdev %s", spdk_bdev_get_name(bdev));
		return -ENODEV;
	}
	if (claim->refs == 1) {
		RB_REMOVE(esnap_claim_tree, &esnap_claims, claim);
		spdk_bdev_module_release_bdev(bdev);
		pthread_mutex_unlock(&claim_mutex);
		free(claim);
		return 0;
	}
	claim->refs--;
	pthread_mutex_unlock(&claim_mutex);
	return 0;
}

static struct esnap_ctx *
esnap_ctx_alloc(struct spdk_blob_store *bs, struct spdk_blob *blob, const char *uuid_str)
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
	ctx->bs = bs;
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
	back_bs_dev->is_zeroes = esnap_is_zeroes;
	back_bs_dev->unmap = esnap_unmap;
	return ctx;
}

static void
esnap_ctx_free(struct esnap_ctx *ctx)
{
	free(ctx->uuid);
	free(ctx);
}

static void
esnap_open_done(struct esnap_create_ctx *create_ctx, struct spdk_bs_dev *dev,
		int bserrno)
{
	assert((dev == NULL) ^ (bserrno == 0));
	assert(create_ctx != NULL);

	create_ctx->load_cb(create_ctx->load_arg, dev, bserrno);
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
			       spdk_blob_get_id(ctx->blob), spdk_bdev_get_name(bdev));
		break;
	default:
		SPDK_NOTICELOG("Unsupported event %d\n", type);
		break;
	}
}

/*
 * Create and open the external snapshot bdev. On successful return, the bdev
 * and bdev_desc members of esnap_ctx are updated.
 */
void
blob_create_esnap_dev(struct spdk_blob_store *bs, struct spdk_blob *blob,
		      const char *uuid_str, blob_back_bs_dev_load_done_t load_cb,
		      struct spdk_blob_load_ctx *load_cb_arg)
{
	struct esnap_create_ctx	*create_ctx;
	uint32_t		io_unit_size = spdk_bs_get_io_unit_size(bs);
	struct spdk_bdev	*bdev;
	struct spdk_bs_dev	*back_bs_dev;
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

	esnap_ctx = esnap_ctx_alloc(bs, blob, uuid_str);
	if (esnap_ctx == NULL) {
		esnap_open_done(create_ctx, NULL, -ENOMEM);
		return;
	}

	rc = spdk_bdev_open_ext(uuid_str, false, esnap_bdev_event_cb, esnap_ctx,
				&esnap_ctx->bdev_desc);
	if (rc != 0) {
		esnap_ctx_free(esnap_ctx);
		esnap_open_done(create_ctx, NULL, rc);
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
			    spdk_bdev_get_name(bdev), back_bs_dev->blocklen, io_unit_size);
		esnap_ctx_free(esnap_ctx);
		esnap_open_done(create_ctx, NULL, -EINVAL);
		return;
	}

	rc = esnap_claim_bdev(bdev);
	if (rc != 0) {
		esnap_ctx_free(esnap_ctx);
		esnap_open_done(create_ctx, NULL, rc);
		return;
	}

	esnap_open_done(create_ctx, back_bs_dev, 0);
}
