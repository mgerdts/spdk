/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk/string.h"

#include "common/lib/ut_multithread.c"
#include "blob/eio/blob_eio.c"

static void
ut_bs_dev_cpl(struct spdk_io_channel *ch, void *cb_arg, int bserrno)
{
	int *io_error = cb_arg;

	*io_error = bserrno;
}

static void
eio_refs(void)
{
	uint64_t ctx = 42;
	struct spdk_io_channel *ch1, *ch2;
	struct spdk_bs_dev *bs_dev = NULL;
	struct bs_dev_eio *eio_dev;
	int rc;

	set_thread(0);

	/* When first created the reference count is 1. */
	rc = spdk_bs_dev_eio_create(&ctx, &bs_dev, "eio1");
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	eio_dev = bs_dev_to_eio_dev(bs_dev);
	SPDK_CU_ASSERT_FATAL(eio_dev != NULL);
	CU_ASSERT(eio_dev->refs == 1);

	/* Verify reference count increases with channels on the same thread. */
	ch1 = bs_dev->create_channel(bs_dev);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);
	CU_ASSERT(eio_dev->refs == 2);
	ch2 = bs_dev->create_channel(bs_dev);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);
	CU_ASSERT(eio_dev->refs == 3);
	bs_dev->destroy_channel(bs_dev, ch1);
	CU_ASSERT(eio_dev->refs == 2);
	bs_dev->destroy_channel(bs_dev, ch2);
	CU_ASSERT(eio_dev->refs == 1);

	/* Verify reference count increases with channels on different threads. */
	ch1 = bs_dev->create_channel(bs_dev);
	SPDK_CU_ASSERT_FATAL(ch1 != NULL);
	CU_ASSERT(eio_dev->refs == 2);
	set_thread(1);
	ch2 = bs_dev->create_channel(bs_dev);
	SPDK_CU_ASSERT_FATAL(ch2 != NULL);
	CU_ASSERT(eio_dev->refs == 3);
	set_thread(0);
	bs_dev->destroy_channel(bs_dev, ch1);
	CU_ASSERT(eio_dev->refs == 2);

	/* Destroy should defer free until last channel is destroyed. */
	bs_dev->destroy(bs_dev);
	CU_ASSERT(eio_dev->refs == 1);

	/* New channels cannot be created after destroy is called */
	ch1 = bs_dev->create_channel(bs_dev);
	CU_ASSERT(ch1 == NULL);
	CU_ASSERT(eio_dev->refs == 1);

	set_thread(1);
	bs_dev->destroy_channel(bs_dev, ch2);
	/* Should be freed now. Leave it to asan to check for leaks. */

	set_thread(0);
}

static void
eio_io(void)
{
	uint64_t ctx = 42;
	struct spdk_io_channel *ch;
	struct spdk_bs_dev *bs_dev = NULL;
	struct spdk_bs_dev_cb_args cb_args = { 0 };
	struct spdk_blob_ext_io_opts io_opts = { 0 };
	int rc;
	int io_errno;

	/* Create an eio device */
	rc = spdk_bs_dev_eio_create(&ctx, &bs_dev, "eio1");
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	ch = bs_dev->create_channel(bs_dev);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	cb_args.cb_fn = ut_bs_dev_cpl;
	cb_args.channel = ch;
	cb_args.cb_arg = &io_errno;

	/* All read()s return -EIO and don't touch the buffer */
	io_errno = 0xbad;
	bs_dev->read(bs_dev, ch, (void *)0x1, 0, 1, &cb_args);
	CU_ASSERT(io_errno == -EIO);

	/* All readv()s return -EIO and don't touch the iovec */
	io_errno = 0xbad;
	bs_dev->readv(bs_dev, ch, (struct iovec *)0x1, 1, 0, 1, &cb_args);
	CU_ASSERT(io_errno == -EIO);

	/* All readv_ext()s return -EIO and don't touch the iovec */
	io_errno = 0xbad;
	bs_dev->readv_ext(bs_dev, ch, (struct iovec *)0x1, 1, 0, 1, &cb_args, &io_opts);

	/* All the write callbacks are NULL */
	CU_ASSERT(bs_dev->write == NULL);
	CU_ASSERT(bs_dev->writev == NULL);
	CU_ASSERT(bs_dev->writev_ext == NULL);
	CU_ASSERT(bs_dev->flush == NULL);
	CU_ASSERT(bs_dev->write_zeroes == NULL);
	CU_ASSERT(bs_dev->copy == NULL);

	/* is_zeroes returns false */
	CU_ASSERT(!bs_dev->is_zeroes(bs_dev, 0, 1));

	bs_dev->destroy_channel(bs_dev, ch);
	bs_dev->destroy(bs_dev);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("blob_eio", NULL, NULL);

	CU_ADD_TEST(suite, eio_refs);
	CU_ADD_TEST(suite, eio_io);

	allocate_threads(2);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return spdk_min(num_failures, 255);
}
