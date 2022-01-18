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
#include "spdk/likely.h"
#include "spdk/log.h"

#include "spdk/bdev_module.h"
#include "bdev_ro.h"

struct vbdev_ro;

struct vbdev_ro_claim {
	struct spdk_bdev	*base_bdev;
	struct spdk_bdev_desc	*base_bdev_desc;
	struct spdk_thread	*thread;
	RB_ENTRY(vbdev_ro_claim) node;
	LIST_HEAD(, vbdev_ro)	ro_bdevs;
};

struct vbdev_ro {
	struct spdk_bdev	bdev;
	struct vbdev_ro_claim	*claim;
	LIST_ENTRY(vbdev_ro)	link;
};

struct vbdev_ro_channel {
	struct spdk_io_channel	*base_ch;
};

static void vbdev_ro_release_bdev(struct vbdev_ro *ro_bdev);
static int vbdev_ro_claim_cmp(struct vbdev_ro_claim *claim1, struct vbdev_ro_claim *claim2);

RB_HEAD(vbdev_ro_claims_tree, vbdev_ro_claim) g_bdev_ro_claims = RB_INITIALIZER(&head);
RB_GENERATE_STATIC(vbdev_ro_claims_tree, vbdev_ro_claim, node, vbdev_ro_claim_cmp);
pthread_mutex_t g_bdev_ro_claims_mutex;

/*
 * Read-only device module functions
 */
static int
vbdev_ro_init(void)
{
	return 0;
}

static struct spdk_bdev_module ro_if = {
	.name = "ro",
	.module_init = vbdev_ro_init,
};

/*
 * Read-only device vbdev functions
 */

static int
vbdev_ro_destruct(void *ctx)
{
	struct vbdev_ro *ro_vbdev = ctx;

	vbdev_ro_release_bdev(ro_vbdev);
	free(ro_vbdev->bdev.name);
	free(ro_vbdev);
	return 0;
}

static void
vbdev_ro_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	spdk_bdev_io_complete(cb_arg, success);
	spdk_bdev_free_io(bdev_io);
}

static void
vbdev_ro_submit_request(struct spdk_io_channel *ro_ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_ro		*ro_vbdev = bdev_io->bdev->ctxt;
	struct vbdev_ro_channel	*base_io_ch = spdk_io_channel_get_ctx(ro_ch);
	struct spdk_bdev	*base_bdev = ro_vbdev->claim->base_bdev;
	struct spdk_bdev_desc	*base_desc = ro_vbdev->claim->base_bdev_desc;

	int rc;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (spdk_bdev_is_md_separate(base_bdev)) {
			rc = spdk_bdev_readv_blocks_with_md(base_desc,
						    base_io_ch->base_ch,
						    bdev_io->u.bdev.iovs,
						    bdev_io->u.bdev.iovcnt,
						    bdev_io->u.bdev.md_buf,
						    bdev_io->u.bdev.offset_blocks,
						    bdev_io->u.bdev.num_blocks,
						    vbdev_ro_complete_io,
						    bdev_io);
		} else {
			rc = spdk_bdev_readv_blocks(base_desc,
						    base_io_ch->base_ch,
						    bdev_io->u.bdev.iovs,
						    bdev_io->u.bdev.iovcnt,
						    bdev_io->u.bdev.offset_blocks,
						    bdev_io->u.bdev.num_blocks,
						    vbdev_ro_complete_io,
						    bdev_io);
		}
		break;
	default:
		SPDK_ERRLOG("IO type %" PRIu32 " not supported by vbdev_ro\n",
			    bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (spdk_unlikely(rc != 0)) {
		enum spdk_bdev_io_status status;

		SPDK_ERRLOG("Failed to pass through read from %s to %s: %d\n",
			    spdk_bdev_get_name(bdev_io->bdev),
			    spdk_bdev_get_name(ro_vbdev->claim->base_bdev), rc);

		if (rc == -ENOMEM) {
			status = SPDK_BDEV_IO_STATUS_NOMEM;
		} else {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		}
		spdk_bdev_io_complete(bdev_io, status);
	}
}

static struct spdk_io_channel *
vbdev_ro_get_io_channel(void *ctx)
{
	struct vbdev_ro		*ro_vbdev = ctx;
	struct vbdev_ro_claim	*claim = ro_vbdev->claim;

	return spdk_get_io_channel(claim);
}

static int
vbdev_ro_get_memory_domains(void *ctx, struct spdk_memory_domain **domains,
			    int array_size)
{
	struct vbdev_ro		*ro_vbdev = ctx;

	return spdk_bdev_get_memory_domains(ro_vbdev->claim->base_bdev,
					    domains, array_size);
}

static bool
vbdev_ro_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
		return true;
	default:
		return false;
	}

	abort();
}

static const struct spdk_bdev_fn_table vbdev_ro_fn_table = {
	.destruct		= vbdev_ro_destruct,
	.submit_request		= vbdev_ro_submit_request,
	.io_type_supported	= vbdev_ro_io_type_supported,
	.get_io_channel		= vbdev_ro_get_io_channel,
	.get_memory_domains	= vbdev_ro_get_memory_domains,
};

static void
vbdev_ro_base_bdev_event_cb(enum spdk_bdev_event_type type,
			    struct spdk_bdev *base_bdev, void *event_ctx)
{
	struct vbdev_ro_claim	*claim = event_ctx;
	struct vbdev_ro		*ro_bdev, *tmp;

        switch (type) {
        case SPDK_BDEV_EVENT_REMOVE:
		pthread_mutex_lock(&g_bdev_ro_claims_mutex);
		LIST_FOREACH_SAFE(ro_bdev, &claim->ro_bdevs, link, tmp) {
			spdk_bdev_unregister(&ro_bdev->bdev, NULL, NULL);
		}
		pthread_mutex_unlock(&g_bdev_ro_claims_mutex);
                break;
        default:
                SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
                break;
        }
}

static int
vbdev_ro_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_ro_claim	*claim = io_device;
	struct vbdev_ro_channel	*ro_ch = ctx_buf;

	ro_ch->base_ch = spdk_bdev_get_io_channel(claim->base_bdev_desc);
	if (ro_ch->base_ch == NULL) {
		return -1;
	}

	return 0;
}

static void
vbdev_ro_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_ro_channel	*ro_ch = ctx_buf;

	spdk_put_io_channel(ro_ch->base_ch);
}

/*
 * Claim tracking
 */

static int
vbdev_ro_claim_cmp(struct vbdev_ro_claim *claim1, struct vbdev_ro_claim *claim2)
{
	return claim1->base_bdev == claim2->base_bdev ? 0 :
	       claim1->base_bdev < claim2->base_bdev ? -1 : 1;
}

static struct vbdev_ro_claim *
vbdev_ro_get_claim_by_base_bdev(struct spdk_bdev *base_bdev)
{
	struct vbdev_ro_claim find = {};

	find.base_bdev = base_bdev;
	return RB_FIND(vbdev_ro_claims_tree, &g_bdev_ro_claims, &find);
}

static int
vbdev_ro_claim_bdev(struct spdk_bdev *base_bdev, struct vbdev_ro *ro_bdev)
{
	struct vbdev_ro_claim *claim = NULL;
	struct vbdev_ro_claim *existing;
	int ret;
	char claimname[32];

	snprintf(claimname, sizeof(claimname), "ro_claim_%s",
		 spdk_bdev_get_name(base_bdev));

	pthread_mutex_lock(&g_bdev_ro_claims_mutex);

	claim = vbdev_ro_get_claim_by_base_bdev(base_bdev);
	if (claim != NULL) {
		LIST_INSERT_HEAD(&claim->ro_bdevs, ro_bdev, link);
		pthread_mutex_unlock(&g_bdev_ro_claims_mutex);
		ro_bdev->claim = claim;
		return 0;
	}

	/* Create a new claim */

	ret = spdk_bdev_module_claim_bdev(base_bdev, NULL, &ro_if);
	if (ret != 0) {
		pthread_mutex_unlock(&g_bdev_ro_claims_mutex);
		return ret;
	}
	claim = calloc(1, sizeof (*claim));
	if (claim == NULL) {
		pthread_mutex_unlock(&g_bdev_ro_claims_mutex);
		return -ENOMEM;
	}
	claim->thread = spdk_get_thread();
	ret = spdk_bdev_open_ext(base_bdev->name, false, vbdev_ro_base_bdev_event_cb,
				 claim, &claim->base_bdev_desc);
	if (ret != 0) {
		spdk_bdev_module_release_bdev(base_bdev);
		pthread_mutex_unlock(&g_bdev_ro_claims_mutex);
		free(claim);
		return ret;
	}
	claim->base_bdev = base_bdev;
	LIST_INIT(&claim->ro_bdevs);
	LIST_INSERT_HEAD(&claim->ro_bdevs, ro_bdev, link);

	existing = RB_INSERT(vbdev_ro_claims_tree, &g_bdev_ro_claims, claim);
	if (existing != NULL) {
		SPDK_ERRLOG("collision while inserting claim for bdev %s\n",
			    spdk_bdev_get_name(base_bdev));
		abort();
	}

	spdk_io_device_register(claim, vbdev_ro_channel_create_cb,
				vbdev_ro_channel_destroy_cb,
				sizeof(struct vbdev_ro_channel),
				claimname);

	pthread_mutex_unlock(&g_bdev_ro_claims_mutex);

	ro_bdev->claim = claim;
	return 0;
}

static void
vbdev_ro_device_unregister_cb(void *_claim)
{
	struct vbdev_ro_claim *claim = _claim;

	assert(LIST_EMPTY(&claim->ro_bdevs));
	free(claim);
}

int spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx);
static void
vbdev_ro_release_claim(void *ctx) {
	struct vbdev_ro_claim	*claim = ctx;

	assert(claim->thread == spdk_get_thread());

	spdk_bdev_module_release_bdev(claim->base_bdev);
	spdk_bdev_close(claim->base_bdev_desc);
	spdk_io_device_unregister(claim, vbdev_ro_device_unregister_cb);
}

static void
vbdev_ro_release_bdev(struct vbdev_ro *ro_bdev)
{
	struct vbdev_ro_claim *claim = ro_bdev->claim;

	pthread_mutex_lock(&g_bdev_ro_claims_mutex);

	if (LIST_EMPTY(&claim->ro_bdevs)) {
		SPDK_ERRLOG("bdev %s has a claim with an empty list\n",
			    spdk_bdev_get_name(claim->base_bdev));
		abort();
	}

	LIST_REMOVE(ro_bdev, link);
	if (!LIST_EMPTY(&claim->ro_bdevs)) {
		pthread_mutex_unlock(&g_bdev_ro_claims_mutex);
		return;
	}

	/* Remove the claim from the tree and release it on the right thread. */
	RB_REMOVE(vbdev_ro_claims_tree, &g_bdev_ro_claims, claim);
	pthread_mutex_unlock(&g_bdev_ro_claims_mutex);

	if (claim->thread == spdk_get_thread()) {
		vbdev_ro_release_claim(claim);
	} else {
		spdk_thread_send_msg(claim->thread, vbdev_ro_release_claim, claim);
	}
}

static char *
vbdev_ro_unique_name(const char *base_name)
{
	static pthread_mutex_t	lock;
	static uint64_t	next = 0;
	uint64_t cur;

	pthread_mutex_lock(&lock);
	cur = next++;
	pthread_mutex_unlock(&lock);

	return spdk_sprintf_alloc("ro_%s_%" PRIu64, base_name, cur);
}

/*
 * Read-only device public interface implementation
 */
int
create_ro_disk(const char *base_name, const struct spdk_uuid *base_uuid,
	       const struct vbdev_ro_opts *opts, struct spdk_bdev **bdevp)
{
	struct spdk_bdev	*base_bdev = NULL;
	struct vbdev_ro		*ro_bdev = NULL;
	int			rc;

	if ((base_name == NULL && base_uuid == NULL) ||
	    (base_name != NULL && base_uuid != NULL)) {
		SPDK_ERRLOG("Exactly one of base_name or base_uuid required\n");
		return -EINVAL;
	}

	if (bdevp == NULL &&
	    (opts == NULL || ((opts->name == NULL) && (opts->uuid == NULL)))) {
		SPDK_ERRLOG("At least one of bdevp, opts->name, or opts->uuid required\n");
		return -EINVAL;
	}

	if (base_name != NULL) {
		base_bdev = spdk_bdev_get_by_name(base_name);
	} else {
		base_bdev = spdk_bdev_get_by_uuid(base_uuid);
	}
	if (base_bdev == NULL) {
		return -ENOENT;
	}

	ro_bdev = calloc(1, sizeof(*ro_bdev));
	if (ro_bdev == NULL) {
		return -ENOMEM;
	}

	rc = vbdev_ro_claim_bdev(base_bdev, ro_bdev);
	if (rc != 0) {
		free(ro_bdev);
		return rc;
	}

	ro_bdev->bdev.ctxt = ro_bdev;
	if (opts != NULL && opts->name != NULL) {
		ro_bdev->bdev.name = strdup(opts->name);
	} else {
		ro_bdev->bdev.name = vbdev_ro_unique_name(spdk_bdev_get_name(base_bdev));
	}
	if (ro_bdev->bdev.name == NULL) {
		rc = -ENOMEM;
		goto errout;
	}
	ro_bdev->bdev.product_name = "read-only disk";

	if (opts != NULL && opts->uuid != NULL) {
		ro_bdev->bdev.uuid = *opts->uuid;
	} else {
		spdk_uuid_generate(&ro_bdev->bdev.uuid);
	}

#define COPY_FIELD(field) ro_bdev->bdev.field = base_bdev->field
	COPY_FIELD(blocklen);
	COPY_FIELD(phys_blocklen);
	COPY_FIELD(blockcnt);
	COPY_FIELD(write_unit_size);
	COPY_FIELD(acwu);
	COPY_FIELD(required_alignment);
	COPY_FIELD(split_on_optimal_io_boundary);
	COPY_FIELD(optimal_io_boundary);
	COPY_FIELD(max_segment_size);
	COPY_FIELD(max_num_segments);
	COPY_FIELD(max_unmap);
	COPY_FIELD(max_unmap_segments);
	COPY_FIELD(max_write_zeroes);
	COPY_FIELD(md_len);
	COPY_FIELD(md_interleave);
	COPY_FIELD(dif_type);
	COPY_FIELD(dif_is_head_of_md);
	COPY_FIELD(dif_check_flags);
	COPY_FIELD(zoned);
	COPY_FIELD(zone_size);
	COPY_FIELD(max_zone_append_size);
	COPY_FIELD(max_open_zones);
	COPY_FIELD(max_active_zones);
	COPY_FIELD(optimal_open_zones);
	COPY_FIELD(media_events);
#undef COPY_FIELD

	ro_bdev->bdev.module = &ro_if;
	ro_bdev->bdev.fn_table = &vbdev_ro_fn_table;

	rc = spdk_bdev_register(&ro_bdev->bdev);
	if (rc != 0) {
		goto errout;
	}

	if (bdevp != NULL) {
		*bdevp = &ro_bdev->bdev;
	}
	return 0;

errout:
	assert(rc != 0);
	vbdev_ro_release_bdev(ro_bdev);
	if (ro_bdev != NULL) {
		free(ro_bdev->bdev.name);
		free(ro_bdev);
	}
	return rc;
}

void
delete_ro_disk(struct spdk_bdev *ro_bdev, spdk_bdev_unregister_cb cb_fn,
	       void *cb_arg)
{
	if (ro_bdev == NULL || ro_bdev->module != &ro_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(ro_bdev, cb_fn, cb_arg);
}

struct spdk_bdev *
bdev_ro_get_base_bdev(struct spdk_bdev *bdev)
{
	struct vbdev_ro *ro_bdev = bdev->ctxt;

	if (bdev == NULL || bdev->module != &ro_if || ro_bdev->claim == NULL) {
		return NULL;
	}

	return ro_bdev->claim->base_bdev;
}

SPDK_BDEV_MODULE_REGISTER(bdev_ro, &ro_if)
