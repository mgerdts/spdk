/*-
 *   BSD LICENSE
 *
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

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "spdk/bdev_module.h"
#include "bdev_wait.h"

struct bdev_wait {
	/* This bdev */
	struct spdk_bdev	bdev;

	/* The bdev that is being waited upon. */
	struct spdk_uuid	uuid;

	/* Called when the bdev appears. */
	wait_disk_available_cb	available_cb;
	void			*available_ctx;

	RB_ENTRY(bdev_wait)	node;
};

static int bdev_wait_init(void);
static void bdev_wait_fini(void);
static void bdev_wait_examine_disk(struct spdk_bdev *bdev);

static struct spdk_bdev_module wait_if = {
	.name = "wait",
	.module_init	= bdev_wait_init,
	.module_fini	= bdev_wait_fini,
	.examine_disk	= bdev_wait_examine_disk,
};

/*
 * Keep track of all wait bdevs in an RB tree.
 */
static int
bdev_wait_cmp(struct bdev_wait *w1, struct bdev_wait *w2)
{
	return spdk_uuid_compare(&w1->uuid, &w2->uuid);
}

RB_HEAD(bdev_wait_tree, bdev_wait) g_bdev_waits = RB_INITIALIZER(&head);
RB_GENERATE_STATIC(bdev_wait_tree, bdev_wait, node, bdev_wait_cmp);
pthread_mutex_t g_bdev_wait_mutex;

/*
 * Even though this module doesn't really support performing any IO, the read
 * and write path at the bdev layer doesn't know that. Thus, we need to mock up
 * enough support for channels that the get_io_channel callback can work.
 */
static int
bdev_wait_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_wait_destroy_cb(void *io_device, void *ctx_buf)
{
}

static int
bdev_wait_init(void)
{
	/*
	 * There is no need for space after a struct io_device for context for
	 * this module. A non-zero size is given so that
	 * spdk_io_channel_get_ctx() doesn't lead to confusion during debug
	 * sessions by returning a pointer to something else.
	 */
	spdk_io_device_register(&g_bdev_waits, bdev_wait_create_cb,
				bdev_wait_destroy_cb, 1, "bdev_wait");
	return 0;
}

static void
bdev_wait_fini(void)
{
	spdk_io_device_unregister(&g_bdev_waits, NULL);
}

static void
bdev_wait_examine_disk(struct spdk_bdev *bdev)
{
	struct bdev_wait	*wait_bdev;
	struct bdev_wait	find = { 0 };

	find.uuid = bdev->uuid;

	pthread_mutex_lock(&g_bdev_wait_mutex);
	wait_bdev = RB_FIND(bdev_wait_tree, &g_bdev_waits, &find);
	pthread_mutex_unlock(&g_bdev_wait_mutex);

	if (wait_bdev != NULL) {
		/*
		 * For this callback to safely use bdev, it needs to open it
		 * before returning.
		 */
		wait_bdev->available_cb(wait_bdev->available_ctx, bdev);
	}

	spdk_bdev_module_examine_done(&wait_if);
}

static void
bdev_wait_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static struct spdk_io_channel *
bdev_wait_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_bdev_waits);
}

static bool
bdev_wait_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	return false;
}

static int
bdev_wait_destruct(void *ctx)
{
	struct bdev_wait *wait_bdev = ctx;

	pthread_mutex_lock(&g_bdev_wait_mutex);
	RB_REMOVE(bdev_wait_tree, &g_bdev_waits, wait_bdev);
	pthread_mutex_unlock(&g_bdev_wait_mutex);

	free(wait_bdev->bdev.name);
	free(wait_bdev);
	return 0;
}

static const struct spdk_bdev_fn_table bdev_wait_fn_table = {
	.destruct		= bdev_wait_destruct,
	.submit_request		= bdev_wait_submit_request,
	.io_type_supported	= bdev_wait_io_type_supported,
	.get_io_channel		= bdev_wait_get_io_channel,
};

static char *
bdev_wait_unique_name(const char *uuid)
{
	static pthread_mutex_t	lock;
	static uint64_t	next = 0;
	uint64_t cur;

	pthread_mutex_lock(&lock);
	cur = next++;
	pthread_mutex_unlock(&lock);

	return spdk_sprintf_alloc("wait_%s_%" PRIu64, uuid, cur);
}

/*
 * Wait device public interface implementation
 */
int
create_wait_disk(const char *new_name, const char *new_uuid, const char *base_uuid,
		 wait_disk_available_cb available_cb, void *available_ctx,
		 struct spdk_bdev **bdevp)
{
	struct bdev_wait	*wait_bdev = NULL;
	struct spdk_bdev	*bdev = NULL;
	int			rc;

	wait_bdev = calloc(1, sizeof(wait_bdev));
	if (wait_bdev == NULL) {
		return -ENOMEM;
	}
	wait_bdev->available_cb = available_cb;
	wait_bdev->available_ctx = available_ctx;
	bdev = &wait_bdev->bdev;

	if (base_uuid == NULL) {
		SPDK_ERRLOG("Missing UUID\n");
		free(wait_bdev);
		return -EINVAL;
	}

	if (new_uuid == NULL) {
		spdk_uuid_generate(&bdev->uuid);
	} else {
		rc = spdk_uuid_parse(&bdev->uuid, new_uuid);
		if (rc != 0) {
			SPDK_ERRLOG("Invalid uuid '%s'\n", new_uuid);
			return rc;
		}
	}

	rc = spdk_uuid_parse(&wait_bdev->uuid, base_uuid);
	if (rc != 0) {
		SPDK_ERRLOG("Invalid uuid '%s'\n", base_uuid);
		free(wait_bdev);
		return rc;
	}

	if (new_name == NULL) {
		bdev->name = bdev_wait_unique_name(base_uuid);
	} else {
		bdev->name = strdup(new_name);
	}
	if (bdev->name == NULL) {
		SPDK_ERRLOG("Cannot allocate space for bdev name\n");
		free(wait_bdev);
		return -ENOMEM;
	}

	bdev->ctxt = wait_bdev;
	bdev->product_name = "wait";
	bdev->blockcnt = 0;
	bdev->write_unit_size = 1;
	bdev->blocklen = 512;
	bdev->phys_blocklen = 512;

	bdev->module = &wait_if;
	bdev->fn_table = &bdev_wait_fn_table;

	rc = spdk_bdev_register(bdev);
	if (rc != 0) {
		free(bdev->name);
		free(wait_bdev);
		return (rc);
	}

	if (bdevp != NULL) {
		*bdevp = bdev;
	}
	return 0;
}

void
delete_wait_disk(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
	         void *cb_arg)
{
	if (bdev == NULL || bdev->module != &wait_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_wait)
