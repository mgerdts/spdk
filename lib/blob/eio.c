/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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

struct spdk_bs_eio_dev {
	struct spdk_bs_dev dev;
	struct spdk_blob_store *bs;
};

static void
eio_destroy(struct spdk_bs_dev *bs_dev)
{
}

static void
eio_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	 uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
eio_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	  uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
eio_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	  struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
	  struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
eio_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	      struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
	      struct spdk_bs_dev_cb_args *cb_args,
	      struct spdk_blob_ext_io_opts *ext_io_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
eio_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	   struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
	   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
eio_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	       struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
	       struct spdk_bs_dev_cb_args *cb_args,
	       struct spdk_blob_ext_io_opts *ext_io_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
eio_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		 uint64_t lba, uint64_t lba_count,
		 struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static void
eio_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	  uint64_t lba, uint64_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EPERM);
	assert(false);
}

static struct spdk_bs_dev g_eio_bs_dev = {
	.blockcnt = UINT64_MAX,
	.blocklen = 512,
	.create_channel = NULL,
	.destroy_channel = NULL,
	.destroy = eio_destroy,
	.read = eio_read,
	.write = eio_write,
	.readv = eio_readv,
	.writev = eio_writev,
	.readv_ext = eio_readv_ext,
	.writev_ext = eio_writev_ext,
	.write_zeroes = eio_write_zeroes,
	.unmap = eio_unmap,
};

struct spdk_bs_dev *
bs_create_eio_dev(struct spdk_blob *blob)
{
	return &g_eio_bs_dev;
}
