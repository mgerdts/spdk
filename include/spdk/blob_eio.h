/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * Blobstore device that fails all reads with EIO. This may be used as a back_bs_dev when the bdev
 * needed for a blob_bdev is missing, as it can allow the blob to load and service IO that does not
 * depend on the back_bs_dev.
 */

#ifndef SPDK_BLOB_EIO_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a blobstore block device that fails all reads with EIO.
 *
 * \param ctx Context that may be of interest to the caller.
 * \param bs_dev Newly allocated bs_dev. Destroy with bs_dev->destroy().
 * \param name Name of this eio blobstore device. Uniqueness is not required.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_bs_dev_eio_create(void *ctx, struct spdk_bs_dev **bs_dev, const char *name);

#ifdef __cplusplus
}
#endif

#endif
