/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk/string.h"

#include "common/lib/ut_multithread.c"
#include "bdev/lvol/vbdev_lvol_esnap.c"
#include "blob/eio/blob_eio.c"

#include "unit/lib/json_mock.c"

DEFINE_STUB(spdk_bdev_get_by_name, struct spdk_bdev *, (const char *name), NULL);
DEFINE_STUB(spdk_bs_bdev_claim, int,
	    (struct spdk_bs_dev *bs_dev, struct spdk_bdev_module *module), 0);
DEFINE_STUB(spdk_lvs_esnap_missing_add, int,
	    (struct spdk_lvol_store *lvs, struct spdk_lvol *lvol, const void *esnap_id,
	     uint32_t id_len), -ENOTSUP);

struct spdk_blob {
	uint64_t	id;
	char		name[32];
};

struct ut_bs_dev {
	struct spdk_bs_dev bs_dev;
	struct spdk_bdev *bdev;
};

struct spdk_bdev *g_bdev;
struct spdk_bdev_module g_lvol_if = {
	.name = "lvol_esnap_ut",
};

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return bdev->name;
}

static void
ut_bs_dev_destroy(struct spdk_bs_dev *bs_dev)
{
	struct ut_bs_dev *ut_bs_dev = SPDK_CONTAINEROF(bs_dev, struct ut_bs_dev, bs_dev);

	free(ut_bs_dev);
}

int
spdk_bdev_create_bs_dev(const char *bdev_name, bool write,
			struct spdk_bdev_bs_dev_opts *opts, size_t opts_size,
			spdk_bdev_event_cb_t event_cb, void *event_ctx,
			struct spdk_bs_dev **bs_dev)
{
	struct spdk_bdev *bdev;
	struct ut_bs_dev *ut_bs_dev;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (bdev == NULL) {
		return -ENODEV;
	}

	ut_bs_dev = calloc(1, sizeof(*ut_bs_dev));
	SPDK_CU_ASSERT_FATAL(ut_bs_dev != NULL);
	ut_bs_dev->bs_dev.destroy = ut_bs_dev_destroy;
	ut_bs_dev->bdev = bdev;
	*bs_dev = &ut_bs_dev->bs_dev;

	return 0;
}

static void
dev_create(void)
{
	struct spdk_lvol_store lvs = { 0 };
	struct spdk_lvol lvol = { 0 };
	struct spdk_blob blob = { 0 };
	struct spdk_bdev bdev = { 0 };
	const char uuid_str[SPDK_UUID_STRING_LEN] = "a27fd8fe-d4b9-431e-a044-271016228ce4";
	char bad_uuid_str[SPDK_UUID_STRING_LEN] = "a27fd8fe-d4b9-431e-a044-271016228ce4";
	char *unterminated;
	size_t len;
	struct spdk_bs_dev *bs_dev = NULL;
	int rc;

	bdev.name = "bdev0";
	spdk_uuid_parse(&bdev.uuid, uuid_str);

	/* NULL esnap_id */
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, NULL, 0, &bs_dev);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bs_dev == NULL);

	/* Unterminated UUID: asan should catch reads past end of allocated buffer. */
	len = strlen(uuid_str);
	unterminated = calloc(1, len);
	SPDK_CU_ASSERT_FATAL(unterminated != NULL);
	memcpy(unterminated, uuid_str, len);
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, unterminated, len, &bs_dev);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bs_dev == NULL);

	/* Invalid UUID but the right length is invalid */
	bad_uuid_str[2] = 'z';
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, bad_uuid_str, sizeof(uuid_str),
					 &bs_dev);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(bs_dev == NULL);

	/* Bdev not found: get a degraded bs_dev */
	MOCK_SET(spdk_bdev_get_by_name, NULL);
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, uuid_str, sizeof(uuid_str), &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	CU_ASSERT(bs_dev->destroy == bs_dev_eio_destroy);
	bs_dev->destroy(bs_dev);

	/* Cannot get a claim: get a degraded bs_dev */
	MOCK_SET(spdk_bdev_get_by_name, &bdev);
	MOCK_SET(spdk_bs_bdev_claim, -EPERM);
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, uuid_str, sizeof(uuid_str), &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	CU_ASSERT(bs_dev->destroy == bs_dev_eio_destroy);
	bs_dev->destroy(bs_dev);

	/* Happy path */
	MOCK_SET(spdk_bs_bdev_claim, 0);
	rc = vbdev_lvol_esnap_dev_create(&lvs, &lvol, &blob, uuid_str, sizeof(uuid_str), &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	CU_ASSERT(bs_dev->destroy != bs_dev_eio_destroy);
	bs_dev->destroy(bs_dev);

	free(unterminated);
	MOCK_CLEAR(spdk_bdev_get_by_name);
	MOCK_CLEAR(spdk_bs_bdev_claim);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("lvol_esnap", NULL, NULL);

	CU_ADD_TEST(suite, dev_create);

	allocate_threads(2);
	set_thread(0);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
