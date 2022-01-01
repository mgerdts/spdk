/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
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

/*
 * accel replacement
 */
static void *g_accel_p = (void *)0xdeadbeef;

DEFINE_STUB(spdk_accel_submit_fill, int,
	    (struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
	     spdk_accel_completion_cb cb_fn, void *cb_arg), -ENOTSUP);
DEFINE_STUB(spdk_accel_submit_copy, int,
	    (struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
	     spdk_accel_completion_cb cb_fn, void *cb_arg), -ENOTSUP);
DEFINE_STUB(accel_engine_create_cb, int, (void *io_device, void *ctx_buf), 0);
DEFINE_STUB_V(accel_engine_destroy_cb, (void *io_device, void *ctx_buf));

struct spdk_io_channel *
spdk_accel_engine_get_io_channel(void)
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


/*
 * Helpers for malloc devices used in unit tests
 */
struct ut_disk_info {
	const char		*name;
	const char		*uuid_str;
	const uint64_t		num_blocks;
	const uint32_t		block_size;
	/* Remaining initialized to 0 */
	struct spdk_uuid	uuid;
	struct spdk_bdev	*bdev;
} mdisks[] = {
	{ "malloc0", "8a9ceb91-e50f-46cb-b589-2bcd03ed0a65", 64, 4096 },
	{ "malloc1", "feb1f488-9df8-48c6-8f14-bff88b9dbfdd", 256, 512 },
};

static struct spdk_bdev *
ut_open_malloc_dev(size_t devidx)
{
	struct ut_disk_info	*disk;
	int rc;

	SPDK_CU_ASSERT_FATAL(devidx <= SPDK_COUNTOF(mdisks));

	disk = &mdisks[devidx];
	SPDK_CU_ASSERT_FATAL(disk->bdev == NULL);
	SPDK_CU_ASSERT_FATAL(spdk_uuid_parse(&disk->uuid, disk->uuid_str) == 0);

	rc = create_malloc_disk(&disk->bdev, disk->name, &disk->uuid,
				disk->num_blocks, disk->block_size);
	CU_ASSERT(rc == 0);

	return disk->bdev;
}

static void
ut_close_malloc_dev_done(void *cb_arg, int bdeverrno)
{
	uint64_t devidx = (uint64_t)cb_arg;

	CU_ASSERT(bdeverrno == 0);
	if (bdeverrno == 0) {
		SPDK_CU_ASSERT_FATAL(devidx <= SPDK_COUNTOF(mdisks));
		mdisks[devidx].bdev = NULL;
	}
}

static void
ut_close_malloc_dev(size_t devidx)
{
	struct ut_disk_info	*disk;

	SPDK_CU_ASSERT_FATAL(devidx <= SPDK_COUNTOF(mdisks));

	disk = &mdisks[devidx];
	if (disk->bdev == NULL) {
		return;
	}

	delete_malloc_disk(disk->bdev, ut_close_malloc_dev_done, (void*)devidx);
	poll_threads();
	CU_ASSERT(disk->bdev == NULL);
}
