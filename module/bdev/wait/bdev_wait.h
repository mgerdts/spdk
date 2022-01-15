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

#ifndef SPDK_BDEV_WAIT_H
#define SPDK_BDEV_WAIT_H

#include "spdk/stdinc.h"

/** \file
 * Wait block device
 *
 * A wait bdev performs no IO. Its entire purpose is to allow an application to
 * await the surprise arrival of bdev with a particular UUID. When the desired
 * bdev is registered with the bdev system, the available_cb registered at bdev
 * creation time is called.
 *
 * XXX-mg determine behavior when the desired bdev already exists, including
 * races.
 */

typedef void (*wait_disk_available_cb)(void *ctx, struct spdk_bdev *bdev);

/**
 * Create a wait bdev. For the sake of finding the newly created bdev, it is
 * advisable to pass at least one of new_name, new_uuid, or bdevp as non-NULL.
 *
 * \param new_name Name of the new bdev. May be NULL.
 * \param new_uuid UUID of the new bdev. May be NULL.
 * \param base_uuid UUID of the bdev that is being waited upon.
 * \param available_cb Callback called when the desired bdev is registered.
 * \param available_ctx Context passed during the call to available_cb. May be
 * NULL.
 * \param bdevp If non-NULL, *bdevp will be updated with a pointer to the newly
 * created bdev.
 *
 * \return 0 on success.
 * \return -EINVAL if a required option is missing.
 * \return -ENOMEM if memory allocation fails.
 */
int create_wait_disk(const char *new_name, const char *new_uuid,
		 const char *base_uuid, wait_disk_available_cb available_cb,
		 void *available_ctx, struct spdk_bdev **bdevp);

/**
 * Delete a wait bdev.
 *
 * \param bdev Pointer to the wait bdev to delete.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void delete_wait_disk(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn,
	         void *cb_arg);
#endif
