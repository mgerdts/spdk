/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
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
#include "spdk/log.h"
#include "spdk/thread.h"

#include "blobstore.h"

struct spdk_bs_zero_dev {
	struct spdk_bs_dev dev;
	struct spdk_blob_store *bs;
};

/* zero part */

static void
zero_dev_destroy(struct spdk_bs_dev *bs_dev)
{
	struct spdk_bs_zero_dev *zero_dev = (struct spdk_bs_zero_dev *)bs_dev;

	free(zero_dev);
}

static void
zeroes_dev_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		     struct iovec *iov, int iovcnt,
		     uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
		     struct spdk_blob_ext_io_opts *ext_io_opts)
{
	struct spdk_bs_zero_dev *zero_dev = (struct spdk_bs_zero_dev *)dev;
	struct spdk_blob_store *bs = zero_dev->bs;
	struct spdk_bs_channel *ch = spdk_io_channel_get_ctx(channel);
	uint64_t zeroes_lba;

	zeroes_lba = bs_page_to_lba(bs, bs->zero_cluster_page_start);
	SPDK_DEBUGLOG(blob, "readv_ext, lba %"PRIu64" -> %"PRIu64"\n", lba, zeroes_lba);
	ch->dev->readv_ext(ch->dev, ch->dev_channel, iov, iovcnt, zeroes_lba, lba_count, cb_args,
			   ext_io_opts);
}

/* zero memset part */

static void
zeroes_memset_destroy(struct spdk_bs_dev *bs_dev)
{
}

static void
zeroes_memset_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		   uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	memset(payload, 0, dev->blocklen * lba_count);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
zeroes_memset_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		    uint64_t lba, uint32_t lba_count,
		    struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
zeroes_memset_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		    struct iovec *iov, int iovcnt,
		    uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		memset(iov[i].iov_base, 0, iov[i].iov_len);
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
zeroes_memset_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			struct iovec *iov, int iovcnt,
			uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
			struct spdk_blob_ext_io_opts *ext_io_opts)
{
	int i;

	if (ext_io_opts->memory_domain) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
		return;
	}

	for (i = 0; i < iovcnt; i++) {
		memset(iov[i].iov_base, 0, iov[i].iov_len);
	}

	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void
zeroes_memset_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		     struct iovec *iov, int iovcnt,
		     uint64_t lba, uint32_t lba_count,
		     struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
zeroes_memset_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			 struct iovec *iov, int iovcnt,
			 uint64_t lba, uint32_t lba_count,
			 struct spdk_bs_dev_cb_args *cb_args,
			 struct spdk_blob_ext_io_opts *ext_io_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
zeroes_memset_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
			   uint64_t lba, uint64_t lba_count,
			   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
zeroes_memset_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		    uint64_t lba, uint64_t lba_count,
		    struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static struct spdk_bs_dev g_zeroes_bs_dev = {
	.blockcnt = UINT64_MAX,
	.blocklen = 512,
	.create_channel = NULL,
	.destroy_channel = NULL,
	.destroy = zeroes_memset_destroy,
	.read = zeroes_memset_read,
	.write = zeroes_memset_write,
	.readv = zeroes_memset_readv,
	.writev = zeroes_memset_writev,
	.readv_ext = zeroes_memset_readv_ext,
	.writev_ext = zeroes_memset_writev_ext,
	.write_zeroes = zeroes_memset_write_zeroes,
	.unmap = zeroes_memset_unmap,
};

struct spdk_bs_dev *
bs_create_zeroes_dev(struct spdk_blob *blob)
{
#if 1
	struct spdk_blob_store *bs = blob->bs;

	if (bs->zero_cluster_page_start) {
		SPDK_NOTICELOG("Creating ext zeroes dev\n");
		struct spdk_bs_zero_dev *zero_dev = calloc(1, sizeof(*zero_dev));
		if (!zero_dev) {
			SPDK_ERRLOG("Memory allocation failed");
			return NULL;
		}

		zero_dev->bs = bs;
		zero_dev->dev.blockcnt = blob->bs->pages_per_cluster * bs_io_unit_per_page(blob->bs);
		zero_dev->dev.blocklen = spdk_bs_get_io_unit_size(blob->bs);
		zero_dev->dev.create_channel = NULL;
		zero_dev->dev.destroy_channel = NULL;
		zero_dev->dev.destroy = zero_dev_destroy;
		zero_dev->dev.read = zeroes_memset_read;
		zero_dev->dev.write = zeroes_memset_write;
		zero_dev->dev.readv = zeroes_memset_readv;
		zero_dev->dev.writev = zeroes_memset_writev;
		zero_dev->dev.readv_ext = zeroes_dev_readv_ext;
		zero_dev->dev.writev_ext = zeroes_memset_writev_ext;
		zero_dev->dev.write_zeroes = zeroes_memset_write_zeroes;
		zero_dev->dev.unmap = zeroes_memset_unmap;

		return &zero_dev->dev;
	} else {
		SPDK_NOTICELOG("Creating regular memset zeroes dev\n");
		return &g_zeroes_bs_dev;
	}
#else
	return &g_zeroes_bs_dev;
#endif
}
