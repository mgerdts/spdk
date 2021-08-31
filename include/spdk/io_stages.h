#ifndef IO_STAGES_H
#define IO_STAGES_H

#include "spdk/stdinc.h"
#include "spdk/env.h"

#ifdef __cplusplus
extern "C" {
#endif

enum io_stage {
	NO_STAGE = 0,
	PROCESS_SQE,
	SOCK_BATCH_QUEUE,
	WAIT_FOR_TARGET,
	PROCESS_RESP_PDU,
	WAIT_FOR_DMA,
	NUM_OF_STAGES
};

struct io_stage_val {
	char *name;
	int64_t val;
};

extern char *io_stage_name[NUM_OF_STAGES];

/* TODO: dynamically allocate io_stage_counts */
#define NUM_CORES 128
extern int64_t io_stage_counts[NUM_CORES][NUM_OF_STAGES];

static inline int
spdk_io_stage_update(enum io_stage prev_stage, enum io_stage new_stage, int64_t count) {
	uint32_t core = spdk_env_get_current_core();

	io_stage_counts[core][prev_stage] -= count;
	io_stage_counts[core][new_stage] += count;

	return 0;
}

static inline int
spdk_io_stage_get(uint32_t core, struct io_stage_val **isv) {
	struct io_stage_val *ret;
	uint32_t j;

	if (!isv || core >= NUM_CORES)
		return -EINVAL;

	ret = (struct io_stage_val *)calloc(NUM_OF_STAGES, sizeof(*ret));
	if (!ret)
		return -ENOMEM;

	*isv = ret;
	for (j = 0; j < NUM_OF_STAGES; j++) {
		ret->name = io_stage_name[j];
		ret->val = io_stage_counts[core][j];
		ret++;
	}

	return 0;
}

#ifdef __cplusplus
}
#endif

#endif
