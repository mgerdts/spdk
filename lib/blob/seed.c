/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2020 NVIDIA
 *   All rights reserved.
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

// XXX-mg
// This feels like it needs reference counting to handle races with the destroy callback

static struct spdk_io_channel *
seed_create_channel(struct spdk_bs_dev *dev)
{
	if (dev->bdev_desc == NULL) {
		return NULL;
	}

	return spdk_bdev_get_io_channel(dev->bdev_desc);
}

static void
seed_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

static void
seed_destroy(struct spdk_bs_dev *dev)
{
	if (dev->bdev_desc != NULL) {
		spdk_bdev_close(dev->bdev_desc);
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
	if (dev->bdev_desc == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_read_blocks(dev->bdev_desc, channel, payload, lba, lba_count,
			        seed_complete, cb_args);
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
	if (dev->bdev_desc == NULL) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_readv_blocks(dev->bdev_desc, channel, iov, iovcnt, lba,
			         lba_count, seed_complete, cb_args);
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
		    uint64_t lba, uint32_t lba_count,
		    struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
seed_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	     uint64_t lba, uint32_t lba_count,
	     struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
seed_back_remove_cb(void *data)
{
	struct spdk_bs_dev *bsdev = data;
	struct spdk_bdev_desc *bdev_desc = bsdev->bdev_desc;

	if (bdev_desc != NULL) {
		bsdev->bdev_desc = NULL;
		spdk_bdev_close(bdev_desc);
	}
}

int
bs_create_seed_dev(struct spdk_blob *front, const char *seedname)
{
	struct spdk_bdev *bdev;
	struct spdk_bs_dev *back;
	int ret;

	bdev = spdk_bdev_get_by_name(seedname);
	if (bdev == NULL) {
		SPDK_ERRLOG("seed device %s is not found\n", seedname);
		return (-ENOENT);
	}

	/* The seed device must be read-only. */
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE)) {
		SPDK_ERRLOG("seed device %s is not read-only\n", seedname);
		return (-EINVAL);
	}

	back = calloc(1, sizeof (*back));
	if (back == NULL) {
		return (-ENOMEM);
	}

	ret = spdk_bdev_open(bdev, false, seed_back_remove_cb, back, &back->bdev_desc);
	if (ret != 0) {
		free(back);
		return (ret);
	}
	// XXX-mg should set bdev->claimed, but need to coordinate among all
	// instances that have this one open.

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

	front->back_bs_dev = back;

	return 0;
}
