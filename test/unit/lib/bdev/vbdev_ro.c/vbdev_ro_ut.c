/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION. All rights reserved.
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

#include "spdk_cunit.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk/config.h"
/* HACK: disable VTune integration so the unit test doesn't need VTune headers and libs to build */
#undef SPDK_CONFIG_VTUNE

#include "bdev/bdev.c"
#include "bdev/ro/bdev_ro.c"
#include "bdev/malloc/bdev_malloc.c"

DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB_V(spdk_scsi_nvme_translate, (const struct spdk_bdev_io *bdev_io,
					 int *sc, int *sk, int *asc, int *ascq));

/* Verify that all bytes in block `block` of `buf` are `val`. */
#define UT_ASSERT_BUF_BLOCK(buf, block, size, val)			\
	{								\
		size_t b;						\
		for (b = 0; b < (size); b++) {				\
			if (buf[(block * (size)) + b] != (val)) {	\
				break;					\
			}						\
		}							\
		CU_ASSERT(b == (size));					\
	}

/*
 * accel replacement
 */
static void *g_accel_p = (void *)0xdeadbeef;

DEFINE_STUB(spdk_accel_submit_fill, int,
	    (struct spdk_io_channel *ch, void *dst, uint8_t fill, uint64_t nbytes,
	     spdk_accel_completion_cb cb_fn, void *cb_arg), -ENOTSUP);
DEFINE_STUB(accel_engine_create_cb, int, (void *io_device, void *ctx_buf), 0);
DEFINE_STUB_V(accel_engine_destroy_cb, (void *io_device, void *ctx_buf));

int
spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src, uint64_t nbytes,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	memcpy(dst, src, (size_t)nbytes);
	cb_fn(cb_arg, 0);
	return 0;
}

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
 * A minimal bdev module for unit testing
 */
static int
bdev_ut_init(void)
{
	return 0;
}

struct spdk_bdev_module bdev_ut_if = {
	.name = "bdev_ut",
	.module_init = bdev_ut_init,
};

SPDK_BDEV_MODULE_REGISTER(bdev_ut, &bdev_ut_if)

static int
__destruct(void *ctx)
{
	return 0;
}

static struct spdk_bdev_fn_table base_fn_table = {
	.destruct		= __destruct,
};

/*
 * Callbacks used by tests
 */

static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	CU_ASSERT(false);
	return;
}

static void
save_errno_cb(void *cb_arg, int rc)
{
	int *errp = cb_arg;

	*errp = rc;
}

static void
io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	int *err = cb_arg;

	*err = success ? 0 : -EIO;
	spdk_bdev_free_io(bdev_io);
}

static void
nop_cb(void *cb_arg)
{
	return;
}

/*
 * Tests
 */

static void
ro_claims(void)
{
	struct spdk_bdev	bdev_base = {};
	struct spdk_bdev_desc	*base_desc;
	struct vbdev_ro_opts	opts = {};
	struct spdk_bdev	*bdev1, *bdev2;
	struct vbdev_ro		*ro_vbdev1, *ro_vbdev2;
	struct spdk_uuid	uuid;
	int			rc, cb_errno;

	/* Create the base bdev */
	bdev_base.name = "base";
	bdev_base.fn_table = &base_fn_table;
	bdev_base.module = &bdev_ut_if;
	rc = spdk_bdev_register(&bdev_base);
	CU_ASSERT(rc == 0);

	/* Verify the base bdev can be opened read-write and claimed */
	rc = spdk_bdev_open_ext(bdev_base.name, true, bdev_event_cb, NULL,
				&base_desc);
	CU_ASSERT(rc == 0);
	rc = spdk_bdev_module_claim_bdev(&bdev_base, base_desc, &bdev_ut_if);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev_base.internal.claim_module == &bdev_ut_if);

	/* Release the claim and close the base bdev. */
	spdk_bdev_module_release_bdev(&bdev_base);
	spdk_bdev_close(base_desc);

	/*
	 * Create a read-only bdev on the base bdev.
	 */
	opts.name = "ro_ut0";
	opts.uuid = NULL;
	bdev2 = NULL;
	rc = create_ro_disk(bdev_base.name, &opts, &bdev2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev2 != NULL);
	/*
	 * Must poll before deleting the new bdev so that the "bdev_register"
	 * event is sent before deleting the new bdev. Failure to do so leads to
	 * use after free.
	 */
	cb_errno = 0x600dd06;
	poll_threads();
	CU_ASSERT(cb_errno == 0x600dd06);

	bdev1 = spdk_bdev_get_by_name(opts.name);
	CU_ASSERT(bdev1 == bdev2);
	/* The base bdev must be claimed by read-only bdev module */
	CU_ASSERT(bdev_base.internal.claim_module == &ro_if);

	/*
	 * Ensure the claim is preventing writers and other claimants from
	 * succeeding.
	 */
	rc = spdk_bdev_open_ext(bdev_base.name, true, bdev_event_cb, NULL, &base_desc);
	CU_ASSERT(rc == -EPERM);
	rc = spdk_bdev_module_claim_bdev(&bdev_base, NULL, &bdev_ut_if);
	CU_ASSERT(rc == -EPERM);
	/* The claim must still be held by the read-only bdev module */
	CU_ASSERT(bdev_base.internal.claim_module == &ro_if);

	/*
	 * Delete the read-only bdev and verify the claim is released.
	 */
	cb_errno = 0x600dd06;
	delete_ro_disk(bdev1, save_errno_cb, &cb_errno);
	poll_threads();
	CU_ASSERT(cb_errno == 0);
	CU_ASSERT(bdev_base.internal.claim_module == NULL);

	/*
	 * The deleted bdev must really be deleted.
	 */
	bdev1 = spdk_bdev_get_by_name(opts.name);
	CU_ASSERT(bdev1 == NULL);

	/*
	 * Create two read-only bdevs with the same base.  Ensure the claim is
	 * released only after they are both deleted.
	 */
	opts.name = "ro_ut0";
	opts.uuid = NULL;
	bdev1 = NULL;
	rc = create_ro_disk(bdev_base.name, &opts, &bdev1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev1 != NULL);
	ro_vbdev1 = bdev1->ctxt;
	CU_ASSERT(ro_vbdev1->claim->count == 1);
	CU_ASSERT(bdev_base.internal.claim_module == &ro_if);
	poll_threads();
	CU_ASSERT(cb_errno == 0);

	opts.name = "ro_ut1";
	bdev2 = NULL;
	rc = create_ro_disk(bdev_base.name, &opts, &bdev2);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev2 != NULL);
	ro_vbdev2 = bdev2->ctxt;
	CU_ASSERT(ro_vbdev1->claim == ro_vbdev2->claim);
	CU_ASSERT(ro_vbdev2->claim->count == 2);
	CU_ASSERT(bdev_base.internal.claim_module == &ro_if);
	poll_threads();
	CU_ASSERT(cb_errno == 0);

	cb_errno = 0x600dd06;
	delete_ro_disk(bdev1, save_errno_cb, &cb_errno);
	poll_threads();
	CU_ASSERT(cb_errno == 0);
	CU_ASSERT(ro_vbdev2->claim->count == 1);
	CU_ASSERT(bdev_base.internal.claim_module == &ro_if);

	cb_errno = 0x600dd06;
	delete_ro_disk(bdev2, save_errno_cb, &cb_errno);
	poll_threads();
	CU_ASSERT(cb_errno == 0);
	CU_ASSERT(bdev_base.internal.claim_module == NULL);

	/*
	 * If a UUID is provided, it is used.
	 */
	spdk_uuid_generate(&uuid);
	opts.name = "ro_ut0";
	opts.uuid = &uuid;
	bdev1 = NULL;
	rc = create_ro_disk(bdev_base.name, &opts, &bdev1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bdev1 != NULL);
	CU_ASSERT(spdk_uuid_compare(spdk_bdev_get_uuid(bdev1), &uuid) == 0);
	poll_threads();
	CU_ASSERT(cb_errno == 0);
	cb_errno = 0x600dd06;
	delete_ro_disk(bdev1, save_errno_cb, &cb_errno);
	poll_threads();
	CU_ASSERT(cb_errno == 0);

	/*
	 * bdevp argument is optional.
	 */
	rc = create_ro_disk(bdev_base.name, &opts, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(cb_errno == 0);
	bdev1 = spdk_bdev_get_by_name(opts.name);
	CU_ASSERT(bdev1 != NULL);
	cb_errno = 0x600dd06;
	delete_ro_disk(bdev1, save_errno_cb, &cb_errno);
	poll_threads();
	CU_ASSERT(cb_errno == 0);

	/*
	 * Creating a bdev with no name should not work.
	 */
	opts.name = NULL;
	opts.uuid = NULL;
	rc = create_ro_disk(bdev_base.name, &opts, &bdev1);
	CU_ASSERT(rc == -EINVAL);
	opts.name = "";
	rc = create_ro_disk(bdev_base.name, &opts, &bdev1);
	CU_ASSERT(rc == -EINVAL);

	/*
	 * Deleting the wrong bdev should fail gracefully.
	 */
	cb_errno = 0x600dd06;
	delete_ro_disk(NULL, save_errno_cb, &cb_errno);
	CU_ASSERT(cb_errno == -ENODEV);
	cb_errno = 0x600dd06;
	delete_ro_disk(&bdev_base, save_errno_cb, &cb_errno);
	CU_ASSERT(cb_errno == -ENODEV);

	spdk_bdev_unregister(&bdev_base, NULL, NULL);

	poll_threads();
}

static void
ro_io(void)
{
	struct spdk_bdev	*base_bdev, *ro_bdev;
	struct spdk_bdev_desc	*base_desc, *ro_desc;
	struct spdk_io_channel	*base_ch, *ro_ch;
	const uint64_t		base_num_blocks = 16;
	const uint32_t		base_blksz = 512;
	const uint32_t		iovcnt = 3;
	uint8_t			buf[base_blksz * iovcnt];
	struct iovec		iovs[iovcnt];
	struct vbdev_ro_opts	opts = {};
	int			rc, io_errno;
	uint64_t		i;

	/*
	 * Create base device and intialize it with per-block values
	 */
	rc = create_malloc_disk(&base_bdev, NULL, NULL, base_num_blocks, base_blksz);
	CU_ASSERT(rc == 0);
	CU_ASSERT(base_bdev != NULL);
	rc = spdk_bdev_open_ext(spdk_bdev_get_name(base_bdev), true, bdev_event_cb,
				NULL, &base_desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(base_desc != NULL);
	base_ch = spdk_bdev_get_io_channel(base_desc);
	SPDK_CU_ASSERT_FATAL(base_ch != NULL);

	SPDK_CU_ASSERT_FATAL(base_num_blocks < 255);
	for (i = 0; i < base_num_blocks; i++) {
		memset(buf, i + 1, base_blksz);
		rc = spdk_bdev_write_blocks(base_desc, base_ch, buf, i, 1,
					    io_completion_cb, &io_errno);
		CU_ASSERT(rc == 0);
		poll_threads();
		CU_ASSERT(io_errno == 0)
	}
	spdk_put_io_channel(base_ch);
	spdk_bdev_close(base_desc);

	/*
	 * Create a read-only device on the base device, open it, and verify
	 * reads return the right data.
	 */
	opts.name = "ro0";
	ro_bdev = NULL;
	rc = create_ro_disk(spdk_bdev_get_name(base_bdev), &opts, &ro_bdev);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ro_bdev != NULL);
	poll_threads();
	CU_ASSERT(io_errno == 0);
	ro_desc = NULL;
	rc = spdk_bdev_open_ext(opts.name, false, bdev_event_cb, NULL, &ro_desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ro_desc != NULL);
	ro_ch = spdk_bdev_get_io_channel(ro_desc);
	CU_ASSERT(ro_ch != NULL);

	/* Test read, should succeed. */
	for (i = 0; i < base_num_blocks; i++) {
		io_errno = 0x600dd06;
		rc = spdk_bdev_read_blocks(ro_desc, ro_ch, buf, i, 1,
					   io_completion_cb, &io_errno);
		poll_threads();
		CU_ASSERT(io_errno == 0);
		UT_ASSERT_BUF_BLOCK(buf, 0, base_blksz, i + 1);
	}

	/*
	 * Writes should fail. There are at least two places where this is
	 * checked: first, bev_write_blocks_with_md() checks the write flag in
	 * the descriptor; and second, vbdev_ro_submit_request() returns an error
	 * for everything other than read requests.  We check both.
	 *
	 * The error returned by bdev_write_blocks_with_md() is done prior to
	 * bdev_io_submit() and as such the callback is never called. The error
	 * is returned via the function return value.
	 */
	io_errno = 0x600dd06;
	memset(buf, 0xff, base_blksz);
	rc = spdk_bdev_write_blocks(ro_desc, ro_ch, buf, 0, 1, io_completion_cb,
				    &io_errno);
	CU_ASSERT(rc == -EBADF);
	poll_threads();
	CU_ASSERT(io_errno == 0x600dd06);

	/*
	 * When we make it past bdev_write_blocks_with_md(),
	 * vbdev_ro_submit_request() detects the problem.  By this time,
	 * bdev_write_blocks_with_md() has alread returned success (0), so we
	 * find the error via the callback called by vbdev_ro_submit_request().
	 */
	ro_desc->write = true;
	rc = spdk_bdev_write_blocks(ro_desc, ro_ch, buf, 0, 1, io_completion_cb,
				    &io_errno);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(io_errno == -EIO);

	/* Read the block that we tried to write to ensure it did not change. */
	rc = spdk_bdev_read_blocks(ro_desc, ro_ch, buf, 0, 1, io_completion_cb,
				   &io_errno);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(io_errno == 0);
	UT_ASSERT_BUF_BLOCK(buf, 0, base_blksz, 0x01);

	/*
	 * Read several blocks using an iovec and verify that the result.
	 * Arrange the iov such that blocks are read into buf in the reverse
	 * order.
	 */
	iovs[0].iov_base = &buf[2 * base_blksz];	/* 0x02 */
	iovs[1].iov_base = &buf[1 * base_blksz];	/* 0x03 */
	iovs[2].iov_base = &buf[0 * base_blksz];	/* 0x04 */
	iovs[0].iov_len = iovs[1].iov_len = iovs[2].iov_len = base_blksz;
	memset(buf, 0xff, sizeof(buf));
	io_errno = 0x600dd06;
	assert(iovcnt == SPDK_COUNTOF(iovs));
	rc = spdk_bdev_readv_blocks(ro_desc, ro_ch, iovs, iovcnt, 1, iovcnt,
				    io_completion_cb, &io_errno);
	poll_threads();
	CU_ASSERT(io_errno == 0);
	UT_ASSERT_BUF_BLOCK(buf, 0, base_blksz, 0x04);
	UT_ASSERT_BUF_BLOCK(buf, 1, base_blksz, 0x03);
	UT_ASSERT_BUF_BLOCK(buf, 2, base_blksz, 0x02);

	/*
	 * Clean up
	 */
	io_errno = 0x600dd06;
	spdk_put_io_channel(ro_ch);
	spdk_bdev_close(ro_desc);
	delete_ro_disk(ro_bdev, save_errno_cb, &io_errno);
	poll_threads();
	CU_ASSERT(io_errno == 0);
	io_errno = 0x600dd06;
	delete_malloc_disk(base_bdev, save_errno_cb, &io_errno);
	poll_threads();
	CU_ASSERT(io_errno == 0);
}

static void
suite_setup(void)
{
	int cb_errno = 0x600dd06;

	init_accel();
	spdk_bdev_initialize(save_errno_cb, &cb_errno);
	poll_threads();
	assert(cb_errno == 0);
}

static void
suite_teardown(void)
{
	spdk_bdev_finish(nop_cb, NULL);
	fini_accel();
	poll_threads();
}

int
main(int argc, char **argv)
{
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite_with_setup_and_teardown("vbdev_ro", NULL, NULL,
						     suite_setup,
						     suite_teardown);

	CU_ADD_TEST(suite, ro_claims);
	CU_ADD_TEST(suite, ro_io);

	allocate_cores(1);
	allocate_threads(1);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();
	free_cores();

	return num_failures;
}
