/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/lvolstore.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/blob_bdev.h"
#include "spdk/tree.h"
#include "spdk/util.h"

/* Default blob channel opts for lvol */
#define SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS 512

#define LVOL_NAME "name"

SPDK_LOG_REGISTER_COMPONENT(lvol)

static TAILQ_HEAD(, spdk_lvol_store) g_lvol_stores = TAILQ_HEAD_INITIALIZER(g_lvol_stores);
static pthread_mutex_t g_lvol_stores_mutex = PTHREAD_MUTEX_INITIALIZER;

static void lvs_esnap_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
				 spdk_blob_op_with_bs_dev cb, void *cb_arg);
static int lvs_esnap_dev_create_degraded(struct spdk_lvol_store *lvs, struct spdk_lvol *lvol,
		struct spdk_blob *blob, const char *name, struct spdk_bs_dev **bs_dev);
static struct spdk_lvol *lvs_get_lvol_by_blob_id(struct spdk_lvol_store *lvs, spdk_blob_id blob_id);
static void lvs_esnap_missing_remove(struct spdk_lvol *lvol);
static void lvs_esnap_missing_swap(struct spdk_lvol *lvol1, struct spdk_lvol *lvol2);

static int
add_lvs_to_list(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol_store *tmp;
	bool name_conflict = false;

	pthread_mutex_lock(&g_lvol_stores_mutex);
	TAILQ_FOREACH(tmp, &g_lvol_stores, link) {
		if (!strncmp(lvs->name, tmp->name, SPDK_LVS_NAME_MAX)) {
			name_conflict = true;
			break;
		}
	}
	if (!name_conflict) {
		lvs->on_list = true;
		TAILQ_INSERT_TAIL(&g_lvol_stores, lvs, link);
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	return name_conflict ? -1 : 0;
}

static struct spdk_lvol_store *
lvs_alloc(void)
{
	struct spdk_lvol_store *lvs;

	lvs = calloc(1, sizeof(*lvs));
	if (lvs == NULL) {
		return NULL;
	}

	TAILQ_INIT(&lvs->lvols);
	TAILQ_INIT(&lvs->pending_lvols);

	lvs->load_esnaps = false;
	RB_INIT(&lvs->missing_esnaps);
	pthread_mutex_init(&lvs->missing_lock, NULL);
	lvs->thread = spdk_get_thread();

	return lvs;
}

static void
lvs_free(struct spdk_lvol_store *lvs)
{
	pthread_mutex_lock(&g_lvol_stores_mutex);
	if (lvs->on_list) {
		TAILQ_REMOVE(&g_lvol_stores, lvs, link);
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	assert(RB_EMPTY(&lvs->missing_esnaps));

	free(lvs);
}

static struct spdk_lvol *
lvol_alloc(struct spdk_lvol_store *lvs, const char *name, bool thin_provision,
	   enum lvol_clear_method clear_method)
{
	struct spdk_lvol *lvol;

	lvol = calloc(1, sizeof(*lvol));
	if (lvol == NULL) {
		return NULL;
	}

	lvol->lvol_store = lvs;
	lvol->thin_provision = thin_provision;
	lvol->clear_method = (enum blob_clear_method)clear_method;
	snprintf(lvol->name, sizeof(lvol->name), "%s", name);
	spdk_uuid_generate(&lvol->uuid);
	spdk_uuid_fmt_lower(lvol->uuid_str, sizeof(lvol->uuid_str), &lvol->uuid);
	spdk_uuid_fmt_lower(lvol->unique_id, sizeof(lvol->uuid_str), &lvol->uuid);

	TAILQ_INSERT_TAIL(&lvs->pending_lvols, lvol, link);

	return lvol;
}

static void
lvol_free(struct spdk_lvol *lvol)
{
	free(lvol);
}

static void
lvol_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(lvol, "Failed to open lvol %s\n", lvol->unique_id);
		goto end;
	}

	lvol->ref_count++;
	lvol->blob = blob;
end:
	req->cb_fn(req->cb_arg, lvol, lvolerrno);
	free(req);
}

void
spdk_lvol_open(struct spdk_lvol *lvol, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_open_opts opts;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	if (lvol->action_in_progress == true) {
		SPDK_ERRLOG("Cannot open lvol - operations on lvol pending\n");
		cb_fn(cb_arg, lvol, -EBUSY);
		return;
	}

	if (lvol->ref_count > 0) {
		lvol->ref_count++;
		cb_fn(cb_arg, lvol, 0);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_blob_open_opts_init(&opts, sizeof(opts));
	opts.clear_method = lvol->clear_method;

	spdk_bs_open_blob_ext(lvol->lvol_store->blobstore, lvol->blob_id, &opts, lvol_open_cb, req);
}

static void
bs_unload_with_error_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;

	req->cb_fn(req->cb_arg, NULL, req->lvserrno);
	free(req);
}

static void
load_next_lvol(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;
	struct spdk_lvol *lvol, *tmp;
	spdk_blob_id blob_id;
	const char *attr;
	size_t value_len;
	int rc;

	if (lvolerrno == -ENOENT) {
		/* Finished iterating */
		if (req->lvserrno == 0) {
			lvs->load_esnaps = true;
			req->cb_fn(req->cb_arg, lvs, req->lvserrno);
			free(req);
		} else {
			TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
				TAILQ_REMOVE(&lvs->lvols, lvol, link);
				free(lvol);
			}
			lvs_free(lvs);
			spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		}
		return;
	} else if (lvolerrno < 0) {
		SPDK_ERRLOG("Failed to fetch blobs list\n");
		req->lvserrno = lvolerrno;
		goto invalid;
	}

	blob_id = spdk_blob_get_id(blob);

	if (blob_id == lvs->super_blob_id) {
		SPDK_INFOLOG(lvol, "found superblob %"PRIu64"\n", (uint64_t)blob_id);
		spdk_bs_iter_next(bs, blob, load_next_lvol, req);
		return;
	}

	lvol = calloc(1, sizeof(*lvol));
	if (!lvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		req->lvserrno = -ENOMEM;
		goto invalid;
	}

	/*
	 * Do not store a reference to blob now because spdk_bs_iter_next() will close it.
	 * Storing blob_id for future lookups is fine.
	 */
	lvol->blob_id = blob_id;
	lvol->lvol_store = lvs;
	lvol->thin_provision = spdk_blob_is_thin_provisioned(blob);

	rc = spdk_blob_get_xattr_value(blob, "uuid", (const void **)&attr, &value_len);
	if (rc != 0 || value_len != SPDK_UUID_STRING_LEN || attr[SPDK_UUID_STRING_LEN - 1] != '\0' ||
	    spdk_uuid_parse(&lvol->uuid, attr) != 0) {
		SPDK_INFOLOG(lvol, "Missing or corrupt lvol uuid\n");
		memset(&lvol->uuid, 0, sizeof(lvol->uuid));
	}
	spdk_uuid_fmt_lower(lvol->uuid_str, sizeof(lvol->uuid_str), &lvol->uuid);

	if (!spdk_mem_all_zero(&lvol->uuid, sizeof(lvol->uuid))) {
		snprintf(lvol->unique_id, sizeof(lvol->unique_id), "%s", lvol->uuid_str);
	} else {
		spdk_uuid_fmt_lower(lvol->unique_id, sizeof(lvol->unique_id), &lvol->lvol_store->uuid);
		value_len = strlen(lvol->unique_id);
		snprintf(lvol->unique_id + value_len, sizeof(lvol->unique_id) - value_len, "_%"PRIu64,
			 (uint64_t)blob_id);
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&attr, &value_len);
	if (rc != 0 || value_len > SPDK_LVOL_NAME_MAX) {
		SPDK_ERRLOG("Cannot assign lvol name\n");
		lvol_free(lvol);
		req->lvserrno = -EINVAL;
		goto invalid;
	}

	snprintf(lvol->name, sizeof(lvol->name), "%s", attr);

	TAILQ_INSERT_TAIL(&lvs->lvols, lvol, link);

	lvs->lvol_count++;

	SPDK_INFOLOG(lvol, "added lvol %s (%s)\n", lvol->unique_id, lvol->uuid_str);

invalid:
	spdk_bs_iter_next(bs, blob, load_next_lvol, req);
}

static void
close_super_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(lvol, "Could not close super blob\n");
		lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	/* Start loading lvols */
	spdk_bs_iter_first(lvs->blobstore, load_next_lvol, req);
}

static void
close_super_blob_with_error_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	lvs_free(lvs);

	spdk_bs_unload(bs, bs_unload_with_error_cb, req);
}

static void
lvs_read_uuid(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;
	const char *attr;
	size_t value_len;
	int rc;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(lvol, "Could not open super blob\n");
		lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "uuid", (const void **)&attr, &value_len);
	if (rc != 0 || value_len != SPDK_UUID_STRING_LEN || attr[SPDK_UUID_STRING_LEN - 1] != '\0') {
		SPDK_INFOLOG(lvol, "missing or incorrect UUID\n");
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	if (spdk_uuid_parse(&lvs->uuid, attr)) {
		SPDK_INFOLOG(lvol, "incorrect UUID '%s'\n", attr);
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&attr, &value_len);
	if (rc != 0 || value_len > SPDK_LVS_NAME_MAX) {
		SPDK_INFOLOG(lvol, "missing or invalid name\n");
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	snprintf(lvs->name, sizeof(lvs->name), "%s", attr);

	rc = add_lvs_to_list(lvs);
	if (rc) {
		SPDK_INFOLOG(lvol, "lvolstore with name %s already exists\n", lvs->name);
		req->lvserrno = -EEXIST;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	lvs->super_blob_id = spdk_blob_get_id(blob);

	spdk_blob_close(blob, close_super_cb, req);
}

static void
lvs_open_super(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(lvol, "Super blob not found\n");
		lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	spdk_bs_open_blob(bs, blobid, lvs_read_uuid, req);
}

static void
lvs_load_cb(void *cb_arg, struct spdk_blob_store *bs, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno != 0) {
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		lvs_free(lvs);
		free(req);
		return;
	}

	lvs->blobstore = bs;
	lvs->bs_dev = req->bs_dev;

	req->lvol_store = lvs;

	spdk_bs_get_super(bs, lvs_open_super, req);
}

static void
lvs_bs_opts_init(struct spdk_bs_opts *opts)
{
	spdk_bs_opts_init(opts, sizeof(*opts));
	opts->max_channel_ops = SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS;
	opts->external_bs_dev_create = lvs_esnap_dev_create;
}

void
spdk_lvs_load(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts opts = {};

	assert(cb_fn != NULL);

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->lvol_store = lvs_alloc();
	if (req->lvol_store == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->bs_dev = bs_dev;

	lvs_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "LVOLSTORE");
	opts.external_ctx = req->lvol_store;

	spdk_bs_load(bs_dev, &opts, lvs_load_cb, req);
}

static void
remove_bs_on_error_cb(void *cb_arg, int bserrno)
{
}

static void
exit_error_lvs_req(struct spdk_lvs_with_handle_req *req, struct spdk_lvol_store *lvs, int lvolerrno)
{
	req->cb_fn(req->cb_arg, NULL, lvolerrno);
	spdk_bs_destroy(lvs->blobstore, remove_bs_on_error_cb, NULL);
	lvs_free(lvs);
	free(req);
}

static void
super_create_close_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not close super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	req->cb_fn(req->cb_arg, lvs, lvolerrno);
	free(req);
}

static void
super_blob_set_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob *blob = lvs->super_blob;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not set uuid for super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	spdk_blob_close(blob, super_create_close_cb, req);
}

static void
super_blob_init_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob *blob = lvs->super_blob;
	char uuid[SPDK_UUID_STRING_LEN];

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not set super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &lvs->uuid);

	spdk_blob_set_xattr(blob, "uuid", uuid, sizeof(uuid));
	spdk_blob_set_xattr(blob, "name", lvs->name, strnlen(lvs->name, SPDK_LVS_NAME_MAX) + 1);
	spdk_blob_sync_md(blob, super_blob_set_cb, req);
}

static void
super_blob_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not open super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	lvs->super_blob = blob;
	lvs->super_blob_id = spdk_blob_get_id(blob);

	spdk_bs_set_super(lvs->blobstore, lvs->super_blob_id, super_blob_init_cb, req);
}

static void
super_blob_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not create super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	bs = req->lvol_store->blobstore;

	spdk_bs_open_blob(bs, blobid, super_blob_create_open_cb, req);
}

static void
lvs_init_cb(void *cb_arg, struct spdk_blob_store *bs, int lvserrno)
{
	struct spdk_lvs_with_handle_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->lvol_store;

	if (lvserrno != 0) {
		assert(bs == NULL);
		lvs_req->cb_fn(lvs_req->cb_arg, NULL, lvserrno);
		SPDK_ERRLOG("Lvol store init failed: could not initialize blobstore\n");
		lvs_free(lvs);
		free(lvs_req);
		return;
	}

	assert(bs != NULL);
	lvs->blobstore = bs;
	TAILQ_INIT(&lvs->lvols);
	TAILQ_INIT(&lvs->pending_lvols);
	lvs->load_esnaps = true;

	SPDK_INFOLOG(lvol, "Lvol store initialized\n");

	/* create super blob */
	spdk_bs_create_blob(lvs->blobstore, super_blob_create_cb, lvs_req);
}

void
spdk_lvs_opts_init(struct spdk_lvs_opts *o)
{
	o->cluster_sz = SPDK_LVS_OPTS_CLUSTER_SZ;
	o->clear_method = LVS_CLEAR_WITH_UNMAP;
	o->num_md_pages_per_cluster_ratio = 100;
	memset(o->name, 0, sizeof(o->name));
}

static void
setup_lvs_opts(struct spdk_bs_opts *bs_opts, struct spdk_lvs_opts *o, uint32_t total_clusters)
{
	assert(o != NULL);
	lvs_bs_opts_init(bs_opts);
	bs_opts->cluster_sz = o->cluster_sz;
	bs_opts->clear_method = (enum bs_clear_method)o->clear_method;
	bs_opts->num_md_pages = (o->num_md_pages_per_cluster_ratio * total_clusters) / 100;
	bs_opts->external_bs_dev_create = lvs_esnap_dev_create;
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvs_with_handle_req *lvs_req;
	struct spdk_bs_opts opts = {};
	uint32_t total_clusters;
	int rc;

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		return -ENODEV;
	}

	if (o == NULL) {
		SPDK_ERRLOG("spdk_lvs_opts not specified\n");
		return -EINVAL;
	}

	if (o->cluster_sz < bs_dev->blocklen) {
		SPDK_ERRLOG("Cluster size %" PRIu32 " is smaller than blocklen %" PRIu32 "\n",
			    opts.cluster_sz, bs_dev->blocklen);
		return -EINVAL;
	}
	total_clusters = bs_dev->blockcnt / (o->cluster_sz / bs_dev->blocklen);

	setup_lvs_opts(&opts, o, total_clusters);

	if (strnlen(o->name, SPDK_LVS_NAME_MAX) == SPDK_LVS_NAME_MAX) {
		SPDK_ERRLOG("Name has no null terminator.\n");
		return -EINVAL;
	}

	if (strnlen(o->name, SPDK_LVS_NAME_MAX) == 0) {
		SPDK_ERRLOG("No name specified.\n");
		return -EINVAL;
	}

	lvs = lvs_alloc();
	if (!lvs) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store base pointer\n");
		return -ENOMEM;
	}

	spdk_uuid_generate(&lvs->uuid);
	snprintf(lvs->name, sizeof(lvs->name), "%s", o->name);

	rc = add_lvs_to_list(lvs);
	if (rc) {
		SPDK_ERRLOG("lvolstore with name %s already exists\n", lvs->name);
		lvs_free(lvs);
		return -EEXIST;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		lvs_free(lvs);
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	assert(cb_fn != NULL);
	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvol_store = lvs;
	lvs->bs_dev = bs_dev;
	lvs->destruct = false;

	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "LVOLSTORE");
	opts.external_ctx = lvs;

	SPDK_INFOLOG(lvol, "Initializing lvol store\n");
	spdk_bs_init(bs_dev, &opts, lvs_init_cb, lvs_req);

	return 0;
}

static void
lvs_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;

	if (lvolerrno != 0) {
		req->lvserrno = lvolerrno;
	}
	if (req->lvserrno != 0) {
		SPDK_ERRLOG("Lvol store rename operation failed\n");
		/* Lvs renaming failed, so we should 'clear' new_name.
		 * Otherwise it could cause a failure on the next attempt to change the name to 'new_name'  */
		snprintf(req->lvol_store->new_name,
			 sizeof(req->lvol_store->new_name),
			 "%s", req->lvol_store->name);
	} else {
		/* Update lvs name with new_name */
		snprintf(req->lvol_store->name,
			 sizeof(req->lvol_store->name),
			 "%s", req->lvol_store->new_name);
	}

	req->cb_fn(req->cb_arg, req->lvserrno);
	free(req);
}

static void
lvs_rename_sync_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;
	struct spdk_blob *blob = req->lvol_store->super_blob;

	if (lvolerrno < 0) {
		req->lvserrno = lvolerrno;
	}

	spdk_blob_close(blob, lvs_rename_cb, req);
}

static void
lvs_rename_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;
	int rc;

	if (lvolerrno < 0) {
		lvs_rename_cb(cb_arg, lvolerrno);
		return;
	}

	rc = spdk_blob_set_xattr(blob, "name", req->lvol_store->new_name,
				 strlen(req->lvol_store->new_name) + 1);
	if (rc < 0) {
		req->lvserrno = rc;
		lvs_rename_sync_cb(req, rc);
		return;
	}

	req->lvol_store->super_blob = blob;

	spdk_blob_sync_md(blob, lvs_rename_sync_cb, req);
}

void
spdk_lvs_rename(struct spdk_lvol_store *lvs, const char *new_name,
		spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_req *req;
	struct spdk_lvol_store *tmp;

	/* Check if new name is current lvs name.
	 * If so, return success immediately */
	if (strncmp(lvs->name, new_name, SPDK_LVS_NAME_MAX) == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	/* Check if new or new_name is already used in other lvs */
	pthread_mutex_lock(&g_lvol_stores_mutex);
	TAILQ_FOREACH(tmp, &g_lvol_stores, link) {
		if (!strncmp(new_name, tmp->name, SPDK_LVS_NAME_MAX) ||
		    !strncmp(new_name, tmp->new_name, SPDK_LVS_NAME_MAX)) {
			pthread_mutex_unlock(&g_lvol_stores_mutex);
			cb_fn(cb_arg, -EEXIST);
			return;
		}
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	snprintf(lvs->new_name, sizeof(lvs->new_name), "%s", new_name);
	req->lvol_store = lvs;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_open_blob(lvs->blobstore, lvs->super_blob_id, lvs_rename_open_cb, req);
}

static void
_lvs_unload_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *lvs_req = cb_arg;

	SPDK_INFOLOG(lvol, "Lvol store unloaded\n");
	assert(lvs_req->cb_fn != NULL);
	lvs_req->cb_fn(lvs_req->cb_arg, lvserrno);
	free(lvs_req);
}

int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_lvs_req *lvs_req;
	struct spdk_lvol *lvol, *tmp;

	if (lvs == NULL) {
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -ENODEV;
	}

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		if (lvol->action_in_progress == true) {
			SPDK_ERRLOG("Cannot unload lvol store - operations on lvols pending\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		} else if (lvol->ref_count != 0) {
			SPDK_ERRLOG("Lvols still open on lvol store\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		}
	}

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		lvs_esnap_missing_remove(lvol);
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		lvol_free(lvol);
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;

	SPDK_INFOLOG(lvol, "Unloading lvol store\n");
	spdk_bs_unload(lvs->blobstore, _lvs_unload_cb, lvs_req);
	lvs_free(lvs);

	return 0;
}

static void
_lvs_destroy_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_destroy_req *lvs_req = cb_arg;

	SPDK_INFOLOG(lvol, "Lvol store destroyed\n");
	assert(lvs_req->cb_fn != NULL);
	lvs_req->cb_fn(lvs_req->cb_arg, lvserrno);
	free(lvs_req);
}

static void
_lvs_destroy_super_cb(void *cb_arg, int bserrno)
{
	struct spdk_lvs_destroy_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->lvs;

	assert(lvs != NULL);

	SPDK_INFOLOG(lvol, "Destroying lvol store\n");
	spdk_bs_destroy(lvs->blobstore, _lvs_destroy_cb, lvs_req);
	lvs_free(lvs);
}

int
spdk_lvs_destroy(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvs_destroy_req *lvs_req;
	struct spdk_lvol *iter_lvol, *tmp;

	if (lvs == NULL) {
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -ENODEV;
	}

	TAILQ_FOREACH_SAFE(iter_lvol, &lvs->lvols, link, tmp) {
		if (iter_lvol->action_in_progress == true) {
			SPDK_ERRLOG("Cannot destroy lvol store - operations on lvols pending\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		} else if (iter_lvol->ref_count != 0) {
			SPDK_ERRLOG("Lvols still open on lvol store\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		}
	}

	TAILQ_FOREACH_SAFE(iter_lvol, &lvs->lvols, link, tmp) {
		free(iter_lvol);
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvs = lvs;

	SPDK_INFOLOG(lvol, "Deleting super blob\n");
	spdk_bs_delete_blob(lvs->blobstore, lvs->super_blob_id, _lvs_destroy_super_cb, lvs_req);

	return 0;
}

static void
lvol_close_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not close blob on lvol\n");
		lvol_free(lvol);
		goto end;
	}

	lvol->ref_count--;
	lvol->action_in_progress = false;
	SPDK_INFOLOG(lvol, "Lvol %s closed\n", lvol->unique_id);

end:
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

bool
spdk_lvol_deletable(struct spdk_lvol *lvol)
{
	size_t count = 0;

	spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, NULL, &count);
	return (count == 0);
}

static void
lvol_delete_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not remove blob on lvol gracefully - forced removal\n");
	} else {
		SPDK_INFOLOG(lvol, "Lvol %s deleted\n", lvol->unique_id);
	}

	lvs_esnap_missing_swap(lvol, req->oldlvol);
	lvs_esnap_missing_remove(lvol);

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);
	lvol_free(lvol);
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

static void
lvol_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	TAILQ_REMOVE(&req->lvol->lvol_store->pending_lvols, req->lvol, link);

	if (lvolerrno < 0) {
		free(lvol);
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	lvol->blob = blob;
	lvol->blob_id = spdk_blob_get_id(blob);

	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);

	lvol->ref_count++;

	assert(req->cb_fn != NULL);
	req->cb_fn(req->cb_arg, req->lvol, lvolerrno);
	free(req);
}

static void
lvol_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_blob_store *bs;
	struct spdk_blob_open_opts opts;

	if (lvolerrno < 0) {
		TAILQ_REMOVE(&req->lvol->lvol_store->pending_lvols, req->lvol, link);
		free(req->lvol);
		assert(req->cb_fn != NULL);
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	spdk_blob_open_opts_init(&opts, sizeof(opts));
	opts.clear_method = req->lvol->clear_method;
	opts.external_ctx = req->lvol;
	bs = req->lvol->lvol_store->blobstore;

	lvs_esnap_missing_swap(req->lvol, req->origlvol);

	spdk_bs_open_blob_ext(bs, blobid, &opts, lvol_create_open_cb, req);
}

static void
lvol_get_xattr_value(void *xattr_ctx, const char *name,
		     const void **value, size_t *value_len)
{
	struct spdk_lvol *lvol = xattr_ctx;

	if (!strcmp(LVOL_NAME, name)) {
		*value = lvol->name;
		*value_len = SPDK_LVOL_NAME_MAX;
		return;
	}
	if (!strcmp("uuid", name)) {
		*value = lvol->uuid_str;
		*value_len = sizeof(lvol->uuid_str);
		return;
	}
	*value = NULL;
	*value_len = 0;
}

static int
lvs_verify_lvol_name(struct spdk_lvol_store *lvs, const char *name)
{
	struct spdk_lvol *tmp;

	if (name == NULL || strnlen(name, SPDK_LVOL_NAME_MAX) == 0) {
		SPDK_INFOLOG(lvol, "lvol name not provided.\n");
		return -EINVAL;
	}

	if (strnlen(name, SPDK_LVOL_NAME_MAX) == SPDK_LVOL_NAME_MAX) {
		SPDK_ERRLOG("Name has no null terminator.\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(tmp, &lvs->lvols, link) {
		if (!strncmp(name, tmp->name, SPDK_LVOL_NAME_MAX)) {
			SPDK_ERRLOG("lvol with name %s already exists\n", name);
			return -EEXIST;
		}
	}

	TAILQ_FOREACH(tmp, &lvs->pending_lvols, link) {
		if (!strncmp(name, tmp->name, SPDK_LVOL_NAME_MAX)) {
			SPDK_ERRLOG("lvol with name %s is being already created\n", name);
			return -EEXIST;
		}
	}

	return 0;
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		 bool thin_provision, enum lvol_clear_method clear_method, spdk_lvol_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol *lvol;
	struct spdk_blob_opts opts;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -EINVAL;
	}

	rc = lvs_verify_lvol_name(lvs, name);
	if (rc < 0) {
		return rc;
	}

	bs = lvs->blobstore;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	lvol = lvol_alloc(lvs, name, thin_provision, clear_method);
	if (!lvol) {
		free(req);
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return -ENOMEM;
	}

	req->lvol = lvol;
	spdk_blob_opts_init(&opts, sizeof(opts));
	opts.thin_provision = thin_provision;
	opts.num_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(bs));
	opts.clear_method = lvol->clear_method;
	opts.xattrs.count = SPDK_COUNTOF(xattr_names);
	opts.xattrs.names = xattr_names;
	opts.xattrs.ctx = lvol;
	opts.xattrs.get_value = lvol_get_xattr_value;

	spdk_bs_create_blob_ext(lvs->blobstore, &opts, lvol_create_cb, req);

	return 0;
}

int
spdk_lvol_create_bdev_clone(struct spdk_lvol_store *lvs,
			    const char *back_name, const char *clone_name,
			    spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol *lvol;
	struct spdk_blob_opts opts;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	struct spdk_bdev *bdev = spdk_bdev_get_by_name(back_name);
	uint64_t sz;
	int rc;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -EINVAL;
	}

	if (bdev == NULL) {
		SPDK_ERRLOG("bdev does not exist\n");
		return -ENODEV;
	}

	rc = lvs_verify_lvol_name(lvs, clone_name);
	if (rc < 0) {
		return rc;
	}

	sz = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);
	bs = lvs->blobstore;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	lvol = lvol_alloc(lvs, clone_name, true, LVOL_CLEAR_WITH_DEFAULT);
	if (!lvol) {
		free(req);
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return -ENOMEM;
	}
	req->lvol = lvol;

	spdk_blob_opts_init(&opts, sizeof(opts));
	opts.external_snapshot_cookie = back_name;
	opts.external_snapshot_cookie_len = strlen(back_name) + 1;
	opts.thin_provision = true;
	opts.num_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(bs));
	opts.clear_method = lvol->clear_method;
	opts.xattrs.count = SPDK_COUNTOF(xattr_names);
	opts.xattrs.names = xattr_names;
	opts.xattrs.ctx = lvol;
	opts.xattrs.get_value = lvol_get_xattr_value;

	spdk_bs_create_blob_ext(lvs->blobstore, &opts, lvol_create_cb, req);

	return 0;
}

void
spdk_lvol_create_snapshot(struct spdk_lvol *origlvol, const char *snapshot_name,
			  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *newlvol;
	struct spdk_blob *origblob;
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_xattr_opts snapshot_xattrs;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (origlvol == NULL) {
		SPDK_INFOLOG(lvol, "Lvol not provided.\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	origblob = origlvol->blob;
	lvs = origlvol->lvol_store;
	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = lvs_verify_lvol_name(lvs, snapshot_name);
	if (rc < 0) {
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	newlvol = lvol_alloc(origlvol->lvol_store, snapshot_name, true,
			     (enum lvol_clear_method)origlvol->clear_method);
	if (!newlvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	snapshot_xattrs.count = SPDK_COUNTOF(xattr_names);
	snapshot_xattrs.ctx = newlvol;
	snapshot_xattrs.names = xattr_names;
	snapshot_xattrs.get_value = lvol_get_xattr_value;
	req->lvol = newlvol;
	req->origlvol = origlvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_create_snapshot(lvs->blobstore, spdk_blob_get_id(origblob), &snapshot_xattrs,
				lvol_create_cb, req);
}

void
spdk_lvol_create_clone(struct spdk_lvol *origlvol, const char *clone_name,
		       spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *newlvol;
	struct spdk_lvol_with_handle_req *req;
	struct spdk_lvol_store *lvs;
	struct spdk_blob *origblob;
	struct spdk_blob_xattr_opts clone_xattrs;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (origlvol == NULL) {
		SPDK_INFOLOG(lvol, "Lvol not provided.\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	origblob = origlvol->blob;
	lvs = origlvol->lvol_store;
	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = lvs_verify_lvol_name(lvs, clone_name);
	if (rc < 0) {
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	newlvol = lvol_alloc(lvs, clone_name, true,
			     (enum lvol_clear_method)origlvol->clear_method);
	if (!newlvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	clone_xattrs.count = SPDK_COUNTOF(xattr_names);
	clone_xattrs.ctx = newlvol;
	clone_xattrs.names = xattr_names;
	clone_xattrs.get_value = lvol_get_xattr_value;
	req->lvol = newlvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_create_clone(lvs->blobstore, spdk_blob_get_id(origblob), &clone_xattrs,
			     lvol_create_cb, req);
}

static void
lvol_resize_done(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	req->cb_fn(req->cb_arg,  lvolerrno);
	free(req);
}

static void
lvol_blob_resize_cb(void *cb_arg, int bserrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (bserrno != 0) {
		req->cb_fn(req->cb_arg, bserrno);
		free(req);
		return;
	}

	spdk_blob_sync_md(lvol->blob, lvol_resize_done, req);
}

void
spdk_lvol_resize(struct spdk_lvol *lvol, uint64_t sz,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *blob = lvol->blob;
	struct spdk_lvol_store *lvs = lvol->lvol_store;
	struct spdk_lvol_req *req;
	uint64_t new_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(lvs->blobstore));

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_blob_resize(blob, new_clusters, lvol_blob_resize_cb, req);
}

static void
lvol_set_read_only_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
spdk_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_blob_set_read_only(lvol->blob);
	spdk_blob_sync_md(lvol->blob, lvol_set_read_only_cb, req);
}

static void
lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Lvol rename operation failed\n");
	} else {
		snprintf(req->lvol->name, sizeof(req->lvol->name), "%s", req->name);
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
spdk_lvol_rename(struct spdk_lvol *lvol, const char *new_name,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *tmp;
	struct spdk_blob *blob = lvol->blob;
	struct spdk_lvol_req *req;
	int rc;

	/* Check if new name is current lvol name.
	 * If so, return success immediately */
	if (strncmp(lvol->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	/* Check if lvol with 'new_name' already exists in lvolstore */
	TAILQ_FOREACH(tmp, &lvol->lvol_store->lvols, link) {
		if (strncmp(tmp->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
			SPDK_ERRLOG("Lvol %s already exists in lvol store %s\n", new_name, lvol->lvol_store->name);
			cb_fn(cb_arg, -EEXIST);
			return;
		}
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;
	snprintf(req->name, sizeof(req->name), "%s", new_name);

	rc = spdk_blob_set_xattr(blob, "name", new_name, strlen(new_name) + 1);
	if (rc < 0) {
		free(req);
		cb_fn(cb_arg, rc);
		return;
	}

	spdk_blob_sync_md(blob, lvol_rename_cb, req);
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	struct spdk_blob_store *bs;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	if (lvol->ref_count != 0) {
		SPDK_ERRLOG("Cannot destroy lvol %s because it is still open\n", lvol->unique_id);
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	lvol->action_in_progress = true;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;
	bs = lvol->lvol_store->blobstore;
	if (spdk_blob_is_external_clone(lvol->blob)) {
		struct spdk_lvol_store	*lvs = lvol->lvol_store;
		spdk_blob_id	clone_id;
		size_t		count = 1;
		int		rc;

		rc = spdk_blob_get_clones(lvs->blobstore, spdk_blob_get_id(lvol->blob),
					  &clone_id, &count);
		if (rc == 0 && count == 1) {
			req->oldlvol = lvs_get_lvol_by_blob_id(lvs, clone_id);
		}
	}

	spdk_bs_delete_blob(bs, lvol->blob_id, lvol_delete_blob_cb, req);
}

void
spdk_lvol_close(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	if (lvol->ref_count > 1) {
		lvol->ref_count--;
		cb_fn(cb_arg, 0);
		return;
	} else if (lvol->ref_count == 0) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	lvol->action_in_progress = true;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_blob_close(lvol->blob, lvol_close_blob_cb, req);
}

struct spdk_io_channel *
spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	return spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
}

static void
lvol_inflate_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	spdk_bs_free_io_channel(req->channel);

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not inflate lvol\n");
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
spdk_lvol_inflate(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	spdk_blob_id blob_id;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("Lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->channel = spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
	if (req->channel == NULL) {
		SPDK_ERRLOG("Cannot alloc io channel for lvol inflate request\n");
		free(req);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob_id = spdk_blob_get_id(lvol->blob);
	spdk_bs_inflate_blob(lvol->lvol_store->blobstore, req->channel, blob_id, lvol_inflate_cb,
			     req);
}

void
spdk_lvol_decouple_parent(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	spdk_blob_id blob_id;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("Lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->channel = spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
	if (req->channel == NULL) {
		SPDK_ERRLOG("Cannot alloc io channel for lvol inflate request\n");
		free(req);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob_id = spdk_blob_get_id(lvol->blob);
	spdk_bs_blob_decouple_parent(lvol->lvol_store->blobstore, req->channel, blob_id,
				     lvol_inflate_cb, req);
}

void
spdk_lvs_grow(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts opts = {};

	assert(cb_fn != NULL);

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->bs_dev = bs_dev;

	lvs_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "LVOLSTORE");

	spdk_bs_grow(bs_dev, &opts, lvs_load_cb, req);
}

/*
 * Begin external snapshot support
 *
 * An external snapshot can be any read-only bdev that takes the place of an lvol snapshot.  With a
 * clone of an external snapshot ("external clone"), the blobstore stores delta blocks just as it
 * does for every other clone. External clones can be snapshotted, cloned, renamed, inflated, etc.,
 * just like any other lvol.
 *
 * When spdk_lvs_load() is called, it iterates through all blobs in its blobstore building up a list
 * of lvols (lvs->lvols). During this initial iteration, each blob is opened, passed to
 * load_next_lvol(), then closed. There is no need to open the external snapshot during this phase,
 * so the work is avoided by setting lvs->load_esnaps to false, which causes lvs_esnap_dev_create()
 * to call the callback passed to it with a NULL spdk_bs_dev and no error. Once the blobstore is
 * loaded, lvs->load_esnaps is set to true so that future lvol opens cause the external snapshot to
 * be loaded.
 *
 * A missing external snapshot does not strictly need to prevent IO on the lvol because the LBAs
 * backed by clusters allocated in the blobstore can still service IO. An lvol that is an external
 * clone that does not have its external snapshot loaded is considered degraded. Attempts to read
 * from a degraded external snapshot will generate EIO.
 *
 * Each lvstore has a tree of missing external snapshots, lvs->missing_esnaps, indexed by external
 * snapshot name.
 *
 * When an external snapshot device becomes available, it is hot-plugged into the lvol and the lvol
 * is no longer degraded. The arrival of the bdev is noticed by vbdev_lvol's examine_disk()
 * callback. During vbdev_lvol_examine(), each lvstore's missing external snapshots tree is searched
 * for lvols matching the new bdev's name and uuid string. Note that the examine callbacks are
 * called before any bdev aliases may be registered so aliases are not considered. Each match
 * triggers the creation of a struct spdk_bs_dev which is registered with the appropriate blob. Any
 * number of blobs in any number of blobstores may concurrently use the same bdev as an external
 * snapshot. Each blob that uses a bdev will have its own bdev_desc.
 *
 * There is a potential for races between the bdev examine_config() invocation that loads the
 * lvstore and another invocation that loads external snapshot bdevs. These may be running in
 * different threads on different reactors. To ensure that an lvol doesn't get stuck in degraded
 * mode due to a race:
 *
 *   - All access of an lvs->missing_esnaps tree is done while holding lvs->_missing_esnaps_lock.
 *     This lock must not be held across async calls.
 *   - All operations that alter lvs->missing_esnaps happen on the lvs->thread thread.
 *   - When lvs_esnap_dev_create() is called with lvs->load_esnaps set to true and the external
 *     snapshot bdev cannot be opened, the lvol is added to lvs->missing_esnaps. The lvol is still
 *     opened, but in degraded mode with lvol->degraded set to true.
 *   - The lvstore's examine_disk callback will look for the new bdev in each lvstore's
 *     missing_esnaps tree. If found, a new blob_bdev is created and registered as the blob's
 *     back_bs_dev. The examine may happen on any thread, but the blob_bdev creation and
 *     registration happens on lvs->thread.
 *   - There is a small window around the time that a missing bdev is registered with
 *     spdk_bdev_register() where a race still exists:
 *       1. reactor 1, lvs->thread: lvs_esnap_dev_create() does not find the bdev
 *       2. reactor 2: someone else registers the bdev
 *       3. reactor 2: examine hooks are run. The bdev is not in any missing_esnaps tree
 *       4. reactor 1: lvs->thread: add bdev to lvs->missing_esnaps
 *     To close this race, immediately after adding a bdev to lvs->missing_esnaps, the bdev is
 *     looked up again. If found, it is opened immediately and the lvol is not opened in degraded
 *     mode.
 *
 * XXX-mg: I think with the approach above, the lvs->esnap thread requirement is not needed. We
 * still need to be sure that the bdev is closed on the same thread it is opened, so doing this all
 * on lvs->thread still may be best.
 */

struct spdk_lvs_missing {
	struct spdk_lvol_store			*lvol_store;
	const char				*name;
	TAILQ_HEAD(missing_lvols, spdk_lvol)	lvols;
	RB_ENTRY(spdk_lvs_missing)		node;
	uint32_t				holds;
};

struct lvs_degraded_dev {
	struct spdk_bs_dev	bs_dev;
	const char		*bdev_name;
	struct spdk_lvol	*lvol;
};

static int
lvs_esnap_name_cmp(struct spdk_lvs_missing *m1, struct spdk_lvs_missing *m2)
{
	return strcmp(m1->name, m2->name);
}

RB_GENERATE_STATIC(missing_esnap_tree, spdk_lvs_missing, node, lvs_esnap_name_cmp)

static void
lvs_esnap_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	SPDK_NOTICELOG("bdev name (%s) recieved unsupported event type %d\n",
		       spdk_bdev_get_name(bdev), type);
}

/*
 * Record in lvs->missing_esnaps that a bdev of the specified name is needed by the specified lvol.
 */
static int
lvs_esnap_missing_add(struct spdk_lvol_store *lvs, struct spdk_lvol *lvol, const char *name)
{
	struct spdk_lvs_missing find, *missing;

	assert(lvs->thread == spdk_get_thread());

	find.name = name;
	pthread_mutex_lock(&lvs->missing_lock);
	missing = RB_FIND(missing_esnap_tree, &lvs->missing_esnaps, &find);
	if (missing == NULL) {
		missing = calloc(1, sizeof(*missing));
		if (missing == NULL) {
			pthread_mutex_unlock(&lvs->missing_lock);
			SPDK_ERRLOG("lvol %s: cannot create missing node for bdev '%s': "
				    "out of memory\n", lvol->unique_id, name);
			return -ENOMEM;
		}
		missing->lvol_store = lvs;
		missing->name = strdup(name);
		if (missing->name == NULL) {
			pthread_mutex_unlock(&lvs->missing_lock);
			free(missing);
			SPDK_ERRLOG("lvol %s: cannot create missing node for bdev '%s': "
				    "out of memory\n", lvol->unique_id, name);
			return -ENOMEM;
		}
		TAILQ_INIT(&missing->lvols);
		RB_INSERT(missing_esnap_tree, &lvs->missing_esnaps, missing);
	}
	lvol->missing = missing;
	TAILQ_INSERT_TAIL(&missing->lvols, lvol, missing_link);
	pthread_mutex_unlock(&lvs->missing_lock);

	return 0;
}

/*
 * Remove the record of the specified lvol needing a missing bdev.
 */
static void
lvs_esnap_missing_remove(struct spdk_lvol *lvol)
{
	struct spdk_lvol_store	*lvs = lvol->lvol_store;
	struct spdk_lvs_missing	*missing = lvol->missing;

	assert(lvs->thread == spdk_get_thread());

	if (missing == NULL) {
		return;
	}

	lvol->missing = NULL;

	pthread_mutex_lock(&lvs->missing_lock);

	TAILQ_REMOVE(&missing->lvols, lvol, missing_link);
	if (!TAILQ_EMPTY(&missing->lvols) || missing->holds != 0) {
		pthread_mutex_unlock(&lvs->missing_lock);
		return;
	}

	RB_REMOVE(missing_esnap_tree, &lvs->missing_esnaps, missing);
	pthread_mutex_unlock(&lvs->missing_lock);

	free((char *)missing->name);
	free(missing);
}

/*
 * When an external clone is snapshotted or an external clone snapshot with regular clones is
 * removed, the lvol that will need a missing device changes.
 */
static void
lvs_esnap_missing_swap(struct spdk_lvol *lvol1, struct spdk_lvol *lvol2)
{
	struct spdk_lvol_store	*lvs = lvol1->lvol_store;
	struct spdk_lvs_missing *tmp;

	assert(lvs->thread == spdk_get_thread());

	if (lvol2 == NULL || (lvol1->missing == NULL && lvol2->missing == NULL)) {
		return;
	}
	assert(lvol1->lvol_store == lvol2->lvol_store);

	pthread_mutex_lock(&lvs->missing_lock);

	tmp = lvol1->missing;
	lvol1->missing = lvol2->missing;
	lvol2->missing = tmp;

	pthread_mutex_unlock(&lvs->missing_lock);
}

static void
lvs_esnap_hotplug_done(void *cb_arg, int bserrno)
{
	struct spdk_lvol	*lvol = cb_arg;
	struct spdk_lvol_store	*lvs = lvol->lvol_store;

	if (bserrno != 0) {
		SPDK_ERRLOG("lvol %s/%s: failed to hotplug blob_bdev due to error %d\n",
			    lvs->name, lvol->name, bserrno);
	}
}

static void
lvs_esnap_dev_create_on_thread_done(void *ctx, struct spdk_bs_dev *bs_dev, int bserrno)
{
	struct spdk_lvol	*lvol = ctx;

	lvol->missing = NULL;

	if (bserrno != 0) {
		SPDK_ERRLOG("lvol %s/%s: failed to create blob_bdev due to error %d\n",
			    lvol->lvol_store->name, lvol->name, bserrno);
		return;
	}

	spdk_blob_set_esnap_bs_dev(lvol->blob, bs_dev, lvs_esnap_hotplug_done, lvol);
}

static void
lvs_esnap_dev_create_on_thread(void *ctx)
{
	struct spdk_lvs_missing	*missing = ctx;
	struct spdk_lvol_store	*lvs = missing->lvol_store;
	struct spdk_lvol	*lvol, *tmp;

	assert(lvs->thread == spdk_get_thread());

	pthread_mutex_lock(&lvs->missing_lock);
	assert(missing->holds > 0);
	RB_REMOVE(missing_esnap_tree, &lvs->missing_esnaps, missing);
	pthread_mutex_unlock(&lvs->missing_lock);

	TAILQ_FOREACH_SAFE(lvol, &missing->lvols, missing_link, tmp) {
		TAILQ_REMOVE(&missing->lvols, lvol, missing_link);
		missing->holds++;
		lvol->missing = NULL;
		lvs_esnap_dev_create(lvs, lvol, lvol->blob, lvs_esnap_dev_create_on_thread_done,
				     lvol);
	}

	missing->holds--;
	if (missing->holds == 0) {
		free((char *)missing->name);
		free(missing);
	}
}

/*
 * Notify each lvstore that is missing a bdev by the specified name or uuid that the bdev now
 * exists. If spdk_thread_send_msg() fails, the lvstore will not be notified. There's not a good
 * way to recover from this as it is likely that any effort to queue a retry would fail for the same
 * reason (e.g. ENOMEM).
 *
 * Returns true if any lvstore is missing the bdev, else false.
 */
bool
spdk_lvs_esnap_notify_bdev_add(const char **names, size_t namecnt)
{
	struct spdk_lvs_missing	*found[namecnt];
	struct spdk_lvol_store	*lvs;
	struct spdk_lvs_missing find;
	size_t			i;
	bool			ret = false;
	int			rc;

	pthread_mutex_lock(&g_lvol_stores_mutex);
	TAILQ_FOREACH(lvs, &g_lvol_stores, link) {

		pthread_mutex_lock(&lvs->missing_lock);

		for (i = 0; i < namecnt; i++) {
			find.name = names[i];
			found[i] = RB_FIND(missing_esnap_tree, &lvs->missing_esnaps, &find);
			if (found[i] != NULL) {
				found[i]->holds++;
			}
		}

		pthread_mutex_unlock(&lvs->missing_lock);

		for (i = 0; i < namecnt; i++) {
			if (found[i] == NULL) {
				continue;
			}
			/*
			 * Return true even if sending the message fails, as we would prefer to
			 * prevent any other bdev module from starting to use this bdev.
			 */
			ret = true;
			rc = spdk_thread_send_msg(lvs->thread, lvs_esnap_dev_create_on_thread,
						  found[i]);
			if (rc != 0) {
				SPDK_ERRLOG("lvstore %s: missing bdev %s: failed to send message "
					    " to thread with error %d", lvs->name, names[i], rc);
			}
		}
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	return ret;
}

/*
 * Tries to create a spdk_bs_dev using the name obtained from the blob's external snapshot cookie.
 * If that fails, it creates a degraded device that generates EIO errors on read. Can fail due to
 * memory allocation errors.
 */
static void
lvs_esnap_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
		     spdk_blob_op_with_bs_dev cb, void *cb_arg)
{
	struct spdk_lvol_store	*lvs = bs_ctx;
	struct spdk_lvol	*lvol = blob_ctx;
	struct spdk_bs_dev	*bs_dev = NULL;
	const void		*cookie = NULL;
	const char		*name;
	size_t			cookie_len = 0;
	int			rc;

	if (lvs == NULL) {
		if (lvol == NULL) {
			SPDK_ERRLOG("Blob 0x%" PRIx64 ": no lvs context nor lvol context\n",
				    spdk_blob_get_id(blob));
			cb(cb_arg, NULL, -EINVAL);
			return;
		}
		lvs = lvol->lvol_store;
	}

	if (!lvs->load_esnaps) {
		cb(cb_arg, NULL, 0);
		return;
	}

	rc = spdk_blob_get_external_cookie(blob, &cookie, &cookie_len);
	if (rc != 0) {
		SPDK_ERRLOG("Blob 0x%" PRIx64 ": failed to get external snapshot cookie: %d\n",
			    spdk_blob_get_id(blob), rc);
		goto out;
	}
	name = cookie;
	if (strnlen(name, cookie_len) + 1 != cookie_len) {
		SPDK_ERRLOG("Blob 0x%" PRIx64 ": external snapshot cookie not a terminated "
			    "string of the expected length\n", spdk_blob_get_id(blob));
		rc = -EINVAL;
		goto out;
	}

	rc = spdk_bdev_create_bs_dev_ro(name, lvs_esnap_bdev_event_cb, NULL, &bs_dev);
	if (rc == -ENODEV) {
		if (lvol == NULL) {
			spdk_blob_id blob_id = spdk_blob_get_id(blob);
			/*
			 * If spdk_bs_blob_open() is used instead of spdk_bs_blob_open_ext() the
			 * lvol will not have been passed in. The same is true if the open happens
			 * spontaneously due to blobstore activity.
			 */
			lvol = lvs_get_lvol_by_blob_id(lvs, blob_id);
			if (lvol == NULL) {
				SPDK_ERRLOG("lvstore %s: no lvol for blob 0x%" PRIx64 "\n",
					    lvs->name, blob_id);
				rc = -ENODEV;
				goto out;
			}
		}

		lvs_esnap_missing_add(lvs, lvol, name);
		rc = lvs_esnap_dev_create_degraded(lvs, lvol, blob, name, &bs_dev);
	}
	if (rc != 0) {
		SPDK_ERRLOG("Blob 0x%" PRIx64 ": failed to create bs_dev from bdev '%s': %d\n",
			    spdk_blob_get_id(blob), name, rc);
	}

out:
	cb(cb_arg, bs_dev, rc);
}

static void
lvs_degraded_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		  uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
lvs_degraded_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		   struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
		   struct spdk_bs_dev_cb_args *cb_args)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static void
lvs_degraded_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		       struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
		       struct spdk_bs_dev_cb_args *cb_args, struct spdk_blob_ext_io_opts *io_opts)
{
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
}

static bool
lvs_degraded_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	return false;
}

static struct spdk_io_channel *
lvs_degraded_create_channel(struct spdk_bs_dev *bs_dev)
{
	struct lvs_degraded_dev *ddev = (struct lvs_degraded_dev *)bs_dev;

	return spdk_get_io_channel(ddev);
}

static void
lvs_degraded_destroy_channel(struct spdk_bs_dev *bs_dev, struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

static void
lvs_degraded_destroy(struct spdk_bs_dev *bs_dev)
{
	struct lvs_degraded_dev *ddev = (struct lvs_degraded_dev *)bs_dev;

	spdk_io_device_unregister(ddev, NULL);

	free(ddev);
}

static int
lvs_degraded_channel_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
lvs_degraded_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	return;
}

static struct spdk_lvol *
lvs_get_lvol_by_blob_id(struct spdk_lvol_store *lvs, spdk_blob_id blob_id)
{
	struct spdk_lvol *lvol, *tmp;

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		if (lvol->blob_id == blob_id) {
			return lvol;
		}
	}
	return NULL;
}

static int
lvs_esnap_dev_create_degraded(struct spdk_lvol_store *lvs, struct spdk_lvol *lvol,
			      struct spdk_blob *blob, const char *name,
			      struct spdk_bs_dev **bs_dev)
{
	struct lvs_degraded_dev	*ddev;

	ddev = calloc(1, sizeof(*ddev));
	if (ddev == NULL) {
		SPDK_ERRLOG("lvol %s: cannot create degraded dev: out of memory\n",
			    lvol->unique_id);
		return -ENOMEM;
	}

	ddev->bdev_name = strdup(name);
	if (ddev->bdev_name == NULL) {
		free(ddev);
		SPDK_ERRLOG("lvol %s: cannot create degraded dev: out of memory\n",
			    lvol->unique_id);
		return -ENOMEM;
	}

	ddev->lvol = lvol;
	ddev->bs_dev.create_channel = lvs_degraded_create_channel;
	ddev->bs_dev.destroy_channel = lvs_degraded_destroy_channel;
	ddev->bs_dev.destroy = lvs_degraded_destroy;
	ddev->bs_dev.read = lvs_degraded_read;
	ddev->bs_dev.readv = lvs_degraded_readv;
	ddev->bs_dev.readv_ext = lvs_degraded_readv_ext;
	ddev->bs_dev.is_zeroes = lvs_degraded_is_zeroes;

	/* Make the device as large as possible without risk of uint64 overflow. */
	ddev->bs_dev.blockcnt = UINT64_MAX / 512;
	/* Prevent divide by zero errors calculating LBAs that will never be read. */
	ddev->bs_dev.blocklen = 512;

	spdk_io_device_register(ddev, lvs_degraded_channel_create_cb,
				lvs_degraded_channel_destroy_cb, 0, lvol->name);

	*bs_dev = &ddev->bs_dev;
	return 0;
}
