/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/blob.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/blob_eio.h"

struct bs_dev_eio {
	struct spdk_bs_dev	bs_dev;
	void			*ctx;
	int32_t			refs;
	struct spdk_spinlock	lock;
};

static struct bs_dev_eio *
bs_dev_to_eio_dev(struct spdk_bs_dev *bs_dev)
{
	return SPDK_CONTAINEROF(bs_dev, struct bs_dev_eio, bs_dev);
}

static void
bs_dev_eio_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
bs_dev_eio_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		 struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
		 struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
bs_dev_eio_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		     struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
		     struct spdk_bs_dev_cb_args *cb_args,
		     struct spdk_blob_ext_io_opts *io_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static bool
bs_dev_eio_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	return false;
}

static struct spdk_io_channel *
bs_dev_eio_create_channel(struct spdk_bs_dev *bs_dev)
{
	struct bs_dev_eio *eio_dev = bs_dev_to_eio_dev(bs_dev);
	struct spdk_io_channel *ch;

	ch = spdk_get_io_channel(eio_dev);
	if (ch != NULL) {
		spdk_spin_lock(&eio_dev->lock);
		eio_dev->refs++;
		spdk_spin_unlock(&eio_dev->lock);
	}

	return ch;
}

static void
bs_dev_eio_free(struct bs_dev_eio *eio_dev)
{
	assert(eio_dev->refs == 0);

	spdk_spin_destroy(&eio_dev->lock);
	free(eio_dev);
}

static void
bs_dev_eio_destroy_channel(struct spdk_bs_dev *bs_dev, struct spdk_io_channel *channel)
{
	struct bs_dev_eio *eio_dev = bs_dev_to_eio_dev(bs_dev);
	int32_t refs;

	spdk_spin_lock(&eio_dev->lock);

	assert(eio_dev->refs > 0);
	eio_dev->refs--;
	refs = eio_dev->refs;

	spdk_spin_unlock(&eio_dev->lock);

	spdk_put_io_channel(channel);

	if (refs == 0) {
		bs_dev_eio_free(eio_dev);
	}
}

static void
bs_dev_eio_destroy(struct spdk_bs_dev *bs_dev)
{
	struct bs_dev_eio *eio_dev = bs_dev_to_eio_dev(bs_dev);
	int32_t refs;

	spdk_spin_lock(&eio_dev->lock);

	eio_dev->refs--;
	refs = eio_dev->refs;

	spdk_spin_unlock(&eio_dev->lock);

	spdk_io_device_unregister(eio_dev, NULL);

	if (refs == 0) {
		free(eio_dev);
	}
}

static int
bs_dev_eio_channel_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bs_dev_eio_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	return;
}

int
spdk_bs_dev_eio_create(void *ctx, struct spdk_bs_dev **bs_dev, const char *name)
{
	struct bs_dev_eio	*eio_dev;

	eio_dev = calloc(1, sizeof(*eio_dev));
	if (eio_dev == NULL) {
		SPDK_ERRLOG("%s: cannot create degraded dev: out of memory\n", name);
		return -ENOMEM;
	}

	eio_dev->ctx = ctx;
	eio_dev->bs_dev.create_channel = bs_dev_eio_create_channel;
	eio_dev->bs_dev.destroy_channel = bs_dev_eio_destroy_channel;
	eio_dev->bs_dev.destroy = bs_dev_eio_destroy;
	eio_dev->bs_dev.read = bs_dev_eio_read;
	eio_dev->bs_dev.readv = bs_dev_eio_readv;
	eio_dev->bs_dev.readv_ext = bs_dev_eio_readv_ext;
	eio_dev->bs_dev.is_zeroes = bs_dev_eio_is_zeroes;

	/* Make the device as large as possible without risk of uint64 overflow. */
	eio_dev->bs_dev.blockcnt = UINT64_MAX / 512;
	/* Prevent divide by zero errors calculating LBAs that will never be read. */
	eio_dev->bs_dev.blocklen = 512;

	eio_dev->refs = 1;
	spdk_spin_init(&eio_dev->lock);

	spdk_io_device_register(eio_dev, bs_dev_eio_channel_create_cb,
				bs_dev_eio_channel_destroy_cb, 0, name);

	*bs_dev = &eio_dev->bs_dev;
	return 0;
}
