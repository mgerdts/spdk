/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/* XXX-mg: with the mocked up bs_dev in blob.c/esnap_dev.c, this file can be deleted. */

#include "spdk/bdev_module.h"
#include "bdev/malloc/bdev_malloc.c"
#include "bdev/bdev.c"

/*
 * Misc. stubs
 */
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_json_write_object_begin, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_object_end, int, (struct spdk_json_write_ctx *w), 0);
DEFINE_STUB(spdk_json_write_named_string, int,
	    (struct spdk_json_write_ctx *w, const char *name, const char *val), 0);
DEFINE_STUB(spdk_json_write_named_uint32, int,
	    (struct spdk_json_write_ctx *w, const char *name, uint32_t val), 0);
DEFINE_STUB(spdk_json_write_named_uint64, int,
	    (struct spdk_json_write_ctx *w, const char *name, uint64_t val), 0);
DEFINE_STUB(spdk_json_write_named_object_begin, int,
	    (struct spdk_json_write_ctx *w, const char *name), 0);
DEFINE_STUB(spdk_memory_domain_pull_data, int,
	    (struct spdk_memory_domain *src_domain, void *src_domain_ctx,
	     struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
	     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg), -ENOTSUP);
DEFINE_STUB(spdk_memory_domain_push_data, int,
	    (struct spdk_memory_domain *src_domain, void *src_domain_ctx,
	     struct iovec *src_iov, uint32_t src_iov_cnt, struct iovec *dst_iov, uint32_t dst_iov_cnt,
	     spdk_memory_domain_data_cpl_cb cpl_cb, void *cpl_cb_arg), -ENOTSUP);
DEFINE_STUB(spdk_memory_domain_get_dma_device_id, const char *, (struct spdk_memory_domain *domain),
	    "test_domain");
/*
 * accel replacement
 */
static void *g_accel_p = (void *)0xdeadbeef;

DEFINE_STUB(spdk_accel_submit_fill, int,
	    (struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
	     int flags, spdk_accel_completion_cb cb_fn, void *cb_arg), -ENOTSUP)
DEFINE_STUB(accel_engine_create_cb, int, (void *io_device, void *ctx_buf), 0);
DEFINE_STUB_V(accel_engine_destroy_cb, (void *io_device, void *ctx_buf));

int
spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		       int flags, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	memcpy(dst, src, (size_t)nbytes);
	cb_fn(cb_arg, 0);
	return 0;
}

struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(g_accel_p);
}

static void
init_accel(void)
{
	spdk_io_device_register(g_accel_p, accel_engine_create_cb, accel_engine_destroy_cb,
				sizeof(int), "accel_p");
}

static void
fini_accel(void)
{
	spdk_io_device_unregister(g_accel_p, NULL);
}