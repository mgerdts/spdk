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
#include "bdev/wait/bdev_wait.c"

DEFINE_STUB(spdk_notify_send, uint64_t, (const char *type, const char *ctx), 0);
DEFINE_STUB(spdk_notify_type_register, struct spdk_notify_type *, (const char *type), NULL);
DEFINE_STUB_V(spdk_scsi_nvme_translate, (const struct spdk_bdev_io *bdev_io,
					 int *sc, int *sk, int *asc, int *ascq));


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


struct ut_event_cb {
	uint32_t count;
	enum spdk_bdev_event_type type;
	struct spdk_bdev *bdev;
};

static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct ut_event_cb *ret = event_ctx;

	if (event_ctx == NULL) {
		/* Not expected to be called. */
		CU_ASSERT(false);
		return;
	}

	ret->count++;
	ret->type = type;
	ret->bdev = bdev;
}

static void
save_errno_cb(void *cb_arg, int rc)
{
	int *errp = cb_arg;

	*errp = rc;
}

struct wait_available_cb_ctx {
	struct spdk_bdev	*bdev;
};

static void
wait_available_cb(void *_ctx, struct spdk_bdev *bdev)
{
	struct wait_available_cb_ctx *ctx = _ctx;

	ctx->bdev = bdev;
}

/*
 * Tests
 */

static void
wait_create_open_delete(void)
{
	struct spdk_bdev	*wait_bdev = NULL;
	struct spdk_bdev_desc	*wait_desc = NULL;
	const char		*new_name = "wait0";
	const char		*new_uuid = "bfad2ec9-8367-4f6d-89e6-4980b6f51875";
	const char		*wait_uuid = "3ba002f7-da8e-49f9-b356-de1eabb99925";
	struct ut_event_cb	event_ctx = { 0 };
	const struct ut_event_cb	event_ctx_init = { .count = 0, .type = -1 };
	struct wait_available_cb_ctx	wait_cb_ctx = { 0 };
	int			rc, cberrno;

	/* Create a wait bdev with all parameters specified and test lookups. */
	rc = create_wait_disk(new_name, new_uuid, wait_uuid,
			      wait_available_cb, &wait_cb_ctx, &wait_bdev);
	CU_ASSERT(rc == 0);
	CU_ASSERT(wait_bdev != NULL);
	poll_threads();
	CU_ASSERT(spdk_bdev_get_by_name(new_name) == wait_bdev);
	CU_ASSERT(spdk_bdev_get_by_uuid(new_uuid) == wait_bdev);
	CU_ASSERT(wait_cb_ctx.bdev == NULL);

	/* Verify open, close, and delete. */
	wait_desc = NULL;
	event_ctx = event_ctx_init;
	rc = spdk_bdev_open_ext(new_name, false, bdev_event_cb, &event_ctx,
				&wait_desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(wait_desc != NULL);
	poll_threads();
	CU_ASSERT(spdk_bdev_get_by_name(new_name) == wait_bdev);
	CU_ASSERT(spdk_bdev_get_by_uuid(new_uuid) == wait_bdev);
	CU_ASSERT(wait_cb_ctx.bdev == NULL);

	spdk_bdev_close(wait_desc);
	poll_threads();
	CU_ASSERT(spdk_bdev_get_by_name(new_name) == wait_bdev);
	CU_ASSERT(spdk_bdev_get_by_uuid(new_uuid) == wait_bdev);
	CU_ASSERT(memcmp(&event_ctx, &event_ctx_init, sizeof (event_ctx)) == 0);
	CU_ASSERT(wait_cb_ctx.bdev == NULL);

	cberrno = 1;
	delete_wait_disk(wait_bdev, save_errno_cb, &cberrno);
	poll_threads();
	CU_ASSERT(cberrno == 0);
	CU_ASSERT(memcmp(&event_ctx, &event_ctx_init, sizeof (event_ctx)) == 0);
	CU_ASSERT(spdk_bdev_get_by_name(new_name) == NULL);
	CU_ASSERT(spdk_bdev_get_by_uuid(new_uuid) == NULL);
	CU_ASSERT(wait_cb_ctx.bdev == NULL);

	/*
	 * Verify that having the device open delays deletion but a notification
	 * of its deletion is sent.
	 */
	rc = create_wait_disk(new_name, new_uuid, wait_uuid,
			      wait_available_cb, &wait_cb_ctx, &wait_bdev);
	CU_ASSERT(rc == 0);
	CU_ASSERT(wait_bdev != NULL);
	poll_threads();
	CU_ASSERT(spdk_bdev_get_by_name(new_name) == wait_bdev);
	CU_ASSERT(spdk_bdev_get_by_uuid(new_uuid) == wait_bdev);
	CU_ASSERT(wait_cb_ctx.bdev == NULL);

	wait_desc = NULL;
	event_ctx = event_ctx_init;
	rc = spdk_bdev_open_ext(new_name, false, bdev_event_cb, &event_ctx,
				&wait_desc);
	CU_ASSERT(rc == 0);
	CU_ASSERT(wait_desc != NULL);
	poll_threads();
	CU_ASSERT(spdk_bdev_get_by_name(new_name) == wait_bdev);
	CU_ASSERT(spdk_bdev_get_by_uuid(new_uuid) == wait_bdev);
	CU_ASSERT(wait_cb_ctx.bdev == NULL);

	cberrno = 1;
	delete_wait_disk(wait_bdev, save_errno_cb, &cberrno);
	poll_threads();
	CU_ASSERT(cberrno == 1);
	CU_ASSERT(event_ctx.count == 1);
	CU_ASSERT(event_ctx.type == SPDK_BDEV_EVENT_REMOVE);
	CU_ASSERT(spdk_bdev_get_by_name(new_name) == wait_bdev);
	CU_ASSERT(spdk_bdev_get_by_uuid(new_uuid) == wait_bdev);
	CU_ASSERT(wait_cb_ctx.bdev == NULL);

	event_ctx = event_ctx_init;
	spdk_bdev_close(wait_desc);
	poll_threads();
	CU_ASSERT(cberrno == 0);
	CU_ASSERT(memcmp(&event_ctx, &event_ctx_init, sizeof (event_ctx)) == 0);
	CU_ASSERT(spdk_bdev_get_by_name(new_name) == NULL);
	CU_ASSERT(spdk_bdev_get_by_uuid(new_uuid) == NULL);
	CU_ASSERT(wait_cb_ctx.bdev == NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("vbdev_ro", NULL, NULL);

	CU_ADD_TEST(suite, wait_create_open_delete);

	allocate_cores(1);
	allocate_threads(2);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();
	free_cores();

	return num_failures;
}
