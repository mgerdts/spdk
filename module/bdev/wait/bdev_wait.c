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

static int bdev_wait_init(void);
static void bdev_wait_fini(void);
static void bdev_wait_examine_disk(struct spdk_bdev *bdev);

static struct spdk_bdev_module wait_if = {
	.name = "wait",
	.module_init	= bdev_wait_init,
	.module_fini	= bdev_wait_fini,
	.examine_disk	= bdev_wait_examine_disk,
};

struct bdev_wait;

/* Node in an RB tree of bdevs being waited upon. */
struct bdev_wait_target {
	struct spdk_uuid		uuid;
	RB_ENTRY(bdev_wait_target)	node;
	LIST_HEAD(, bdev_wait)		wait_bdevs;
};

/* Each tree node has a list of bdevs that are awaiting the target UUID. */
struct bdev_wait {
	/* This bdev */
	struct spdk_bdev	bdev;

	/* The bdev that is being waited upon. */
	struct bdev_wait_target	*target;

	/* Called when the bdev appears. */
	wait_disk_available_cb	available_cb;
	void			*available_ctx;

	LIST_ENTRY(bdev_wait)	link;
};

static int
bdev_wait_target_cmp(struct bdev_wait_target *w1, struct bdev_wait_target *w2)
{
	return spdk_uuid_compare(&w1->uuid, &w2->uuid);
}

RB_HEAD(bdev_wait_target_tree, bdev_wait_target) g_bdev_wait_targets = RB_INITIALIZER(&head);
RB_GENERATE_STATIC(bdev_wait_target_tree, bdev_wait_target, node, bdev_wait_target_cmp);
pthread_mutex_t g_bdev_wait_mutex;

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
	spdk_io_device_register(&wait_if, bdev_wait_create_cb,
				bdev_wait_destroy_cb, 0, "bdev_wait");
	return 0;
}

static void
bdev_wait_fini(void)
{
	spdk_io_device_unregister(&wait_if, NULL);
}

static void
bdev_wait_examine_disk(struct spdk_bdev *bdev)
{
	struct bdev_wait	*wait_bdev, *tmp;
	struct bdev_wait_target	*target;
	struct bdev_wait_target	find = { 0 };

	find.uuid = bdev->uuid;
	pthread_mutex_lock(&g_bdev_wait_mutex);
	target = RB_FIND(bdev_wait_target_tree, &g_bdev_wait_targets, &find);
	pthread_mutex_unlock(&g_bdev_wait_mutex);

	if (target != NULL) {
		LIST_FOREACH_SAFE(wait_bdev, &target->wait_bdevs, link, tmp) {
			wait_bdev->available_cb(wait_bdev->available_ctx, bdev);
		}
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
	/*
	 * This bdev does not support IO. The bdev layer won't try if it doesn't
	 * have a valid IO channel.
	 */
	return NULL;
}

static bool
bdev_wait_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	return false;
}

/*
 * Add wait_bdev to targets tree. This should be done before wait_bdev is
 * registered.
 */
static int
bdev_wait_add(struct bdev_wait *wait_bdev, const struct spdk_uuid *target_uuid)
{
	struct bdev_wait_target *target, *existing;
	struct bdev_wait_target find;

	find.uuid = *target_uuid;

	pthread_mutex_lock(&g_bdev_wait_mutex);

	target = RB_FIND(bdev_wait_target_tree, &g_bdev_wait_targets, &find);
	if (target == NULL) {
		target = calloc(1, sizeof(*target));
		if (target == NULL) {
			pthread_mutex_unlock(&g_bdev_wait_mutex);
			return -ENOMEM;
		}

		target->uuid = *target_uuid;
		LIST_INIT(&target->wait_bdevs);
		existing = RB_INSERT(bdev_wait_target_tree,
				     &g_bdev_wait_targets, target);
		if (existing != NULL) {
			SPDK_ERRLOG("Collision while inserting wait for %s\n",
				    wait_bdev->bdev.name);
			abort();
		}
	}

	wait_bdev->target = target;

	LIST_INSERT_HEAD(&target->wait_bdevs, wait_bdev, link);

	pthread_mutex_unlock(&g_bdev_wait_mutex);

	return 0;
}

/*
 * Remove wait_bdev from targets tree, removing and freeing tree node if needed.
 * Caller should free wait_bdev if it is no longer needed.
 *
 * This should happen when the wait bdev is no longer registered - likely only
 * during the bdev's destruct callback.
 */
static void
bdev_wait_remove(struct bdev_wait *wait_bdev)
{
	struct bdev_wait_target *target = wait_bdev->target;

	pthread_mutex_lock(&g_bdev_wait_mutex);

	LIST_REMOVE(wait_bdev, link);

	if (LIST_EMPTY(&target->wait_bdevs)) {
		RB_REMOVE(bdev_wait_target_tree, &g_bdev_wait_targets, target);
		free(target);
	}
	pthread_mutex_unlock(&g_bdev_wait_mutex);
}

static int
bdev_wait_destruct(void *ctx)
{
	struct bdev_wait *wait_bdev = ctx;

	bdev_wait_remove(wait_bdev);

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
bdev_wait_unique_name(const struct spdk_uuid *uuid)
{
	static pthread_mutex_t	lock;
	static uint64_t		next = 0;
	uint64_t		cur;
	char			uuid_str[SPDK_UUID_STRING_LEN];

	pthread_mutex_lock(&lock);
	cur = next++;
	pthread_mutex_unlock(&lock);

	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), uuid);
	return spdk_sprintf_alloc("wait_%s_%" PRIu64, uuid_str, cur);
}

/*
 * Wait device public interface implementation
 */

/*
 * There is a small race here. A caller can cover the race by registering the
 * wait, then performing its own lookup for the bdev that it is awaiting.
 */
int
create_wait_disk(const char *new_name, const struct spdk_uuid *new_uuid,
		 const struct spdk_uuid *base_uuid,
		 wait_disk_available_cb available_cb, void *available_ctx,
		 struct spdk_bdev **bdevp)
{
	struct bdev_wait	*wait_bdev = NULL;
	struct spdk_bdev	*bdev = NULL;
	int			rc;

	if (base_uuid == NULL) {
		SPDK_ERRLOG("Missing UUID\n");
		return -EINVAL;
	}

	if (available_cb == NULL) {
		SPDK_ERRLOG("Missing callback\n");
		return -EINVAL;
	}

	wait_bdev = calloc(1, sizeof(*wait_bdev));
	if (wait_bdev == NULL) {
		return -ENOMEM;
	}
	wait_bdev->available_cb = available_cb;
	wait_bdev->available_ctx = available_ctx;
	bdev = &wait_bdev->bdev;

	if (new_uuid == NULL) {
		spdk_uuid_generate(&bdev->uuid);
	} else {
		bdev->uuid = *new_uuid;
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

	rc = bdev_wait_add(wait_bdev, base_uuid);
	if (rc != 0) {
		free(bdev->name);
		free(wait_bdev);
		return (rc);
	}

	rc = spdk_bdev_register(bdev);
	if (rc != 0) {
		bdev_wait_remove(wait_bdev);
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

#if 0
void
bdev_wait_print(const char *msg)
{
	struct bdev_wait_target *target;
	struct bdev_wait *wait_bdev;
	char uuid_str[SPDK_UUID_STRING_LEN];

	if (msg == NULL) {
		msg = "";
	}

	printf("\nwait disks: %s\n", msg);
	RB_FOREACH(target, bdev_wait_target_tree, &g_bdev_wait_targets) {
		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &target->uuid);

		printf("  (struct bdev_wait_target *)%p %s\n",
			target, uuid_str);
		LIST_FOREACH(wait_bdev, &target->wait_bdevs, link) {
			printf("    (struct bdev_wait *)%p %s\n",
				wait_bdev, wait_bdev->bdev.name);
			assert(wait_bdev->target == target);
		}
	}
	printf("\n");
}
#endif

SPDK_BDEV_MODULE_REGISTER(bdev_wait, &wait_if)
