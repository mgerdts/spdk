/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

struct accel_mlx5_attr {
	/* The number of entries in qp submission/receive queue */
	uint16_t qp_size;
	/* The number of requests in the global pool */
	uint32_t num_requests;
	/* The number of data blocks to be processed in 1 UMR if \b use_crypto_mb is true
	 * 0 means no limit */
	uint32_t split_mb_blocks;
	/* Enable crypto operations on memory keys */
	bool enable_crypto;
	/* Use crypto operations on multiple data blocks if HW supports it */
	bool use_crypto_mb;
};

void accel_mlx5_get_default_attr(struct accel_mlx5_attr *attr);
int accel_mlx5_enable(struct accel_mlx5_attr *attr);
