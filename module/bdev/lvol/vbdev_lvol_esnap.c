/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/blob_bdev.h"
#include "spdk/rpc.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/blob_eio.h"

#include "vbdev_lvol.h"

struct vbdev_lvol_degraded_dev {
	struct spdk_bs_dev	bs_dev;
	struct spdk_lvol	*lvol;
	int32_t			refs;
	struct spdk_spinlock	lock;
};

static void
vbdev_lvol_esnap_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			       void *event_ctx)
{
	SPDK_NOTICELOG("bdev name (%s) received unsupported event type %d\n",
		       spdk_bdev_get_name(bdev), type);
}

int
vbdev_lvol_esnap_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
			    const void *esnap_id, uint32_t id_len,
			    struct spdk_bs_dev **_bs_dev)
{
	struct spdk_lvol_store	*lvs = bs_ctx;
	struct spdk_lvol	*lvol = blob_ctx;
	struct spdk_bs_dev	*bs_dev = NULL;
	struct spdk_uuid	uuid;
	int			rc;
	char			uuid_str[SPDK_UUID_STRING_LEN] = { 0 };

	if (esnap_id == NULL) {
		SPDK_ERRLOG("lvol %s: NULL esnap ID\n", lvol->unique_id);
		return -EINVAL;
	}

	/* Guard against arbitrary names and unterminated UUID strings */
	if (id_len != SPDK_UUID_STRING_LEN) {
		SPDK_ERRLOG("lvol %s: Invalid esnap ID length (%u)\n", lvol->unique_id, id_len);
		return -EINVAL;
	}

	if (spdk_uuid_parse(&uuid, esnap_id)) {
		SPDK_ERRLOG("lvol %s: Invalid esnap ID: not a UUID\n", lvol->unique_id);
		return -EINVAL;
	}

	/* Format the UUID the same as it is in the bdev names tree. */
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &uuid);
	if (strcmp(uuid_str, esnap_id) != 0) {
		SPDK_WARNLOG("lvol %s: esnap_id '%*s' does not match parsed uuid '%s'\n",
			     lvol->unique_id, SPDK_UUID_STRING_LEN, (const char *)esnap_id,
			     uuid_str);
		assert(false);
	}

	rc = spdk_bdev_create_bs_dev(uuid_str, false, NULL, 0,
				     vbdev_lvol_esnap_bdev_event_cb, NULL, &bs_dev);
	if (rc == -ENODEV) {
		SPDK_NOTICELOG("lvol %s: esnap device '%s' is missing\n", lvol->unique_id,
			       uuid_str);
	} else if (rc == 0) {
		rc = spdk_bs_bdev_claim(bs_dev, &g_lvol_if);
		if (rc != 0) {
			SPDK_ERRLOG("lvol %s: unable to claim esnap bdev '%s': %d\n",
				    lvol->unique_id, uuid_str, rc);
			bs_dev->destroy(bs_dev);
			bs_dev = NULL;
			rc = -ENODEV;
		}
	}

	if (rc == -ENODEV) {
		/*
		 * A missing external snapshot does not strictly need to prevent IO on the lvol
		 * because the LBAs backed by clusters allocated in the blobstore can still service
		 * IO. An lvol that is an esnap clone that does not have its external snapshot
		 * loaded is considered degraded. Attempts to read from a degraded external snapshot
		 * will generate EIO.
		 */
		rc = spdk_bs_dev_eio_create(lvol, &bs_dev, uuid_str);
		if (rc != 0) {
			SPDK_ERRLOG("lvol %s: unable to created degraded bs_bdev for '%s': %d\n",
				    lvol->unique_id, uuid_str, rc);
			goto out;
		}

		SPDK_NOTICELOG("lvol %s: opened in degraded mode\n", lvol->unique_id);

		/*
		 * Each lvstore has a tree of missing external snapshots. When the missing
		 * bdev arrives, the degraded device created above will be replaced with a
		 * device created by spdk_bev_create_bs_dev_ro().
		 */
		rc = spdk_lvs_esnap_missing_add(lvs, lvol, uuid_str, sizeof(uuid_str));
		if (rc != 0) {
			SPDK_NOTICELOG("lvol %s: unable to register missing esnap device %s: "
				       "it will not be hotplugged if added later\n",
				       lvol->unique_id, uuid_str);
			rc = 0;
		}
	}

out:
	if (rc != 0) {
		SPDK_ERRLOG("lvol %s: failed to create bs_dev from bdev '%s': %d\n",
			    lvol->unique_id, uuid_str, rc);
		return rc;
	}

	*_bs_dev = bs_dev;
	return 0;
}
