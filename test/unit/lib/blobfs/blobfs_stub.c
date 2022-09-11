/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/blob_esnap.h"

DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "test");
DEFINE_STUB_V(spdk_bdev_close, (struct spdk_bdev_desc *desc));
DEFINE_STUB(spdk_bdev_open_ext, int, (const char *bdev_name, bool write,
				      spdk_bdev_event_cb_t event_cb, void *event_ctx,
				      struct spdk_bdev_desc **_desc), 0);
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB(spdk_bdev_read_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, void *buf,
	     uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
	     void *cb_arg), -ENOTSUP);
DEFINE_STUB(spdk_bdev_readv_blocks, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, struct iovec *iov,
	     int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg), -ENOTSUP);
DEFINE_STUB(spdk_bdev_readv_blocks_ext, int,
	    (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, struct iovec *iov,
	     int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts), -ENOTSUP);
DEFINE_STUB(spdk_bdev_get_by_name, struct spdk_bdev *, (const char *bdev_name), NULL);
DEFINE_STUB(spdk_bdev_io_type_supported, bool,
	    (struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type), false);
DEFINE_STUB(spdk_bdev_open, int,
	    (struct spdk_bdev *bdev, bool write, spdk_bdev_remove_cb_t remove_cb,
	     void *remove_ctx, struct spdk_bdev_desc **_desc), -ENOTSUP);
DEFINE_STUB(spdk_bdev_get_block_size, uint32_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *,
	    (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB_V(blob_create_esnap_dev,
	      (struct spdk_blob_store *bs, struct spdk_blob *blob,
	       const char *uuid_str, blob_back_bs_dev_load_done_t load_cb,
	       struct spdk_blob_load_ctx *load_cb_arg));
DEFINE_STUB_V(spdk_bs_esnap_destroy_channels, (struct spdk_esnap_channels *esnap_channels));
