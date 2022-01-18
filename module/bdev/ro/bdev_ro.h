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

#ifndef SPDK_BDEV_RO_H
#define SPDK_BDEV_RO_H

#include "spdk/stdinc.h"

/** \file
 * Read-only virtual block device
 *
 * A read-only vbdev claims an underlying bdev, preventing others from opening
 * the vbdev for writes. There may be many read-only vbdevs that reference the
 * same underlying bdev. In such a case, they will all share the claim for the
 * underlying bdev. The claim will be released only when the last of the
 * read-only vbdevs that reference it is destroyed.
 *
 * Read-only devices have most of the same characteristics, such as size, block
 * size, and alignment requirements, as the bdevs that back them.  Read-only
 * devices do not support write, unmap, or other operations that may alter the
 * device.
 *
 * XXX-mg fix this?
 * Read-only devices do not get updated when the backing device is resized.
 */

/**
 * Options used during the creation of a read-only vbdev.
 */
struct vbdev_ro_opts {
	/** Name of the vbdev being created. If NULL, a name will be generated. */
	const char		*name;

	/** The UUID of the new vbdev. If NULL, a UUID will be generated. */
	const struct spdk_uuid	*uuid;
};

/**
 * Create read-only vbdev
 *
 * \param base_name The name of the bdev that will back the ro vbdev. Must be
 * NULL if base_uuid is non-NULL.
 * \param base_uuid The UUID of the bdev that will back the ro vbdev. Must be
 * NULL if base_name is non-NULL.
 * \param opts Options that describe how the ro vbdev should be created. May be
 * NULL.
 * \param bdevp If not NULL, *bdevp will be updated pointer to the new bdev.
 *
 * \return 0 on success.
 * \return -EINVAL if a required option is missing.
 * \return -ENOENT if a bdev with bdev_name is not found.
 * \return -ENOMEM if memory allocation fails.
 */
int create_ro_disk(const char *base_name, const struct spdk_uuid *base_uuid,
		   const struct vbdev_ro_opts *opts, struct spdk_bdev **bdevp);

/**
 * Delete read-only vbdev
 *
 * \param bdev Pointer to ro vbdev to delete.
 * \param cb_fn Function to call after deletion. May be NULL.
 * \param cb_arg Argument to pass to cb_fn. May be NULL.
 */
void delete_ro_disk(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
		    void *cb_arg);

#endif
