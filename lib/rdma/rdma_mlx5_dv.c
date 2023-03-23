/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 */

#include <rdma/rdma_cma.h>
#include <infiniband/mlx5dv.h>

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk_internal/sgl.h"
#include "spdk_internal/accel_module.h"
#include "spdk_internal/mlx5.h"

#include "spdk_internal/rdma.h"
#include "spdk_internal/rdma_utils.h"
#include "spdk/log.h"
#include "spdk/util.h"

struct spdk_rdma_mlx5_dv_qp {
	struct spdk_rdma_qp common;
	struct ibv_qp_ex *qpex;
	struct mlx5dv_qp_ex *mqpx;
	bool wr_started;
};

struct mlx5_dv_rdma_memory_domain {
	TAILQ_ENTRY(mlx5_dv_rdma_memory_domain) link;
	uint32_t ref;
	struct ibv_pd *pd;
	struct spdk_memory_domain *domain;
	struct spdk_memory_domain_rdma_ctx rdma_ctx;
};

struct mlx5_dv_rdma_accel_sequence_ctx {
	struct spdk_rdma_accel_sequence_ctx base;
	struct mlx5dv_mkey *mkey;
	struct spdk_rdma_utils_mem_map *map;
	struct mlx5_dv_rdma_memory_domain *domain;
	struct ibv_sge sg[16];
	int sge_count;
};

static TAILQ_HEAD(, mlx5_dv_rdma_memory_domain) g_memory_domains = TAILQ_HEAD_INITIALIZER(
			g_memory_domains);
static pthread_mutex_t g_memory_domains_lock = PTHREAD_MUTEX_INITIALIZER;

static int
rdma_mlx5_dv_init_qpair(struct spdk_rdma_mlx5_dv_qp *mlx5_qp)
{
	struct ibv_qp_attr qp_attr;
	int qp_attr_mask, rc;

	qp_attr.qp_state = IBV_QPS_INIT;
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_INIT, errno %s (%d)\n", spdk_strerror(errno), errno);
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_INIT) failed, rc %d\n", rc);
		return rc;
	}

	qp_attr.qp_state = IBV_QPS_RTR;
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTR) failed, rc %d\n", rc);
		return rc;
	}

	qp_attr.qp_state = IBV_QPS_RTS;
	rc = rdma_init_qp_attr(mlx5_qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
		return rc;
	}

	rc = ibv_modify_qp(mlx5_qp->common.qp, &qp_attr, qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTS) failed, rc %d\n", rc);
	}

	return rc;
}

struct spdk_rdma_qp *
spdk_rdma_qp_create(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr)
{
	assert(cm_id);
	assert(qp_attr);

	struct ibv_qp *qp;
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	struct ibv_qp_init_attr_ex dv_qp_attr = {
		.qp_context = qp_attr->qp_context,
		.send_cq = qp_attr->send_cq,
		.recv_cq = qp_attr->recv_cq,
		.srq = qp_attr->srq,
		.cap = qp_attr->cap,
		.qp_type = IBV_QPT_RC,
		.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
		.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_RDMA_READ | IBV_QP_EX_WITH_BIND_MW,
		.pd = qp_attr->pd ? qp_attr->pd : cm_id->pd
	};
	/* Attrs required for MKEYs registration */
	struct mlx5dv_qp_init_attr mlx5_qp_attr = {
		.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS,
		.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE
	};

	assert(dv_qp_attr.pd);

	if (spdk_rdma_accel_seq_supported()) {
		SPDK_NOTICELOG("Multiply queue_depth by 2 to support mkey registration\n");
		dv_qp_attr.cap.max_send_wr *= 2;
		dv_qp_attr.cap.max_send_sge = spdk_min(dv_qp_attr.cap.max_send_sge, 16);
		dv_qp_attr.cap.max_inline_data = sizeof(struct ibv_sge) * 16;
	}

	mlx5_qp = calloc(1, sizeof(*mlx5_qp));
	if (!mlx5_qp) {
		SPDK_ERRLOG("qp memory allocation failed\n");
		return NULL;
	}

	if (qp_attr->stats) {
		mlx5_qp->common.stats = qp_attr->stats;
		mlx5_qp->common.shared_stats = true;
	} else {
		mlx5_qp->common.stats = calloc(1, sizeof(*mlx5_qp->common.stats));
		if (!mlx5_qp->common.stats) {
			SPDK_ERRLOG("qp statistics memory allocation failed\n");
			free(mlx5_qp);
			return NULL;
		}
	}

	qp = mlx5dv_create_qp(cm_id->verbs, &dv_qp_attr, &mlx5_qp_attr);

	if (!qp) {
		SPDK_ERRLOG("Failed to create qpair, errno %s (%d)\n", spdk_strerror(errno), errno);
		free(mlx5_qp);
		return NULL;
	}

	mlx5_qp->common.qp = qp;
	mlx5_qp->common.cm_id = cm_id;
	mlx5_qp->qpex = ibv_qp_to_qp_ex(qp);

	if (!mlx5_qp->qpex) {
		spdk_rdma_qp_destroy(&mlx5_qp->common);
		return NULL;
	}

	mlx5_qp->mqpx = mlx5dv_qp_ex_from_ibv_qp_ex(mlx5_qp->qpex);
	if (!mlx5_qp->mqpx) {
		SPDK_ERRLOG("Failed to get mqpx\n");
		spdk_rdma_qp_destroy(&mlx5_qp->common);
		return NULL;
	}

	qp_attr->cap = dv_qp_attr.cap;

	return &mlx5_qp->common;
}

int
spdk_rdma_qp_accept(struct spdk_rdma_qp *spdk_rdma_qp, struct rdma_conn_param *conn_param)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;

	assert(spdk_rdma_qp != NULL);
	assert(spdk_rdma_qp->cm_id != NULL);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	/* NVMEoF target must move qpair to RTS state */
	if (rdma_mlx5_dv_init_qpair(mlx5_qp) != 0) {
		SPDK_ERRLOG("Failed to initialize qpair\n");
		/* Set errno to be compliant with rdma_accept behaviour */
		errno = ECONNABORTED;
		return -1;
	}

	return rdma_accept(spdk_rdma_qp->cm_id, conn_param);
}

int
spdk_rdma_qp_complete_connect(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;

	assert(spdk_rdma_qp);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	rc = rdma_mlx5_dv_init_qpair(mlx5_qp);
	if (rc) {
		SPDK_ERRLOG("Failed to initialize qpair\n");
		return rc;
	}

	rc = rdma_establish(mlx5_qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("rdma_establish failed, errno %s (%d)\n", spdk_strerror(errno), errno);
	}

	return rc;
}

void
spdk_rdma_qp_destroy(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;

	assert(spdk_rdma_qp != NULL);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (spdk_rdma_qp->send_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying qpair with queued Work Requests\n");
	}

	if (!mlx5_qp->common.shared_stats) {
		free(mlx5_qp->common.stats);
	}

	if (mlx5_qp->common.qp) {
		rc = ibv_destroy_qp(mlx5_qp->common.qp);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy ibv qp %p, rc %d\n", mlx5_qp->common.qp, rc);
		}
	}

	free(mlx5_qp);
}

int
spdk_rdma_qp_disconnect(struct spdk_rdma_qp *spdk_rdma_qp)
{
	int rc = 0;

	assert(spdk_rdma_qp != NULL);

	if (spdk_rdma_qp->qp) {
		struct ibv_qp_attr qp_attr = {.qp_state = IBV_QPS_ERR};

		rc = ibv_modify_qp(spdk_rdma_qp->qp, &qp_attr, IBV_QP_STATE);
		if (rc) {
			SPDK_ERRLOG("Failed to modify ibv qp %p state to ERR, rc %d\n", spdk_rdma_qp->qp, rc);
			return rc;
		}
	}

	if (spdk_rdma_qp->cm_id) {
		rc = rdma_disconnect(spdk_rdma_qp->cm_id);
		if (rc) {
			SPDK_ERRLOG("rdma_disconnect failed, errno %s (%d)\n", spdk_strerror(errno), errno);
		}
	}

	return rc;
}

bool
spdk_rdma_qp_queue_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr *first)
{
	struct ibv_send_wr *tmp;
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	bool is_first;

	assert(spdk_rdma_qp);
	assert(first);

	is_first = spdk_rdma_qp->send_wrs.first == NULL;
	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (!mlx5_qp->wr_started) {
		ibv_wr_start(mlx5_qp->qpex);
		mlx5_qp->wr_started = true;
	}

	if (is_first) {
		spdk_rdma_qp->send_wrs.first = first;
	} else {
		spdk_rdma_qp->send_wrs.last->next = first;
	}

	for (tmp = first; tmp != NULL; tmp = tmp->next) {
		mlx5_qp->qpex->wr_id = tmp->wr_id;
		mlx5_qp->qpex->wr_flags = tmp->send_flags;

		switch (tmp->opcode) {
		case IBV_WR_SEND:
			ibv_wr_send(mlx5_qp->qpex);
			break;
		case IBV_WR_SEND_WITH_INV:
			ibv_wr_send_inv(mlx5_qp->qpex, tmp->invalidate_rkey);
			break;
		case IBV_WR_RDMA_READ:
			ibv_wr_rdma_read(mlx5_qp->qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			break;
		case IBV_WR_RDMA_WRITE:
			ibv_wr_rdma_write(mlx5_qp->qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			break;
		default:
			SPDK_ERRLOG("Unexpected opcode %d\n", tmp->opcode);
			assert(0);
		}

		ibv_wr_set_sge_list(mlx5_qp->qpex, tmp->num_sge, tmp->sg_list);

		spdk_rdma_qp->send_wrs.last = tmp;
		spdk_rdma_qp->stats->send.num_submitted_wrs++;
	}

	return is_first;
}

int
spdk_rdma_qp_flush_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr **bad_wr)
{
	struct spdk_rdma_mlx5_dv_qp *mlx5_qp;
	int rc;

	assert(bad_wr);
	assert(spdk_rdma_qp);

	mlx5_qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_rdma_mlx5_dv_qp, common);

	if (spdk_unlikely(!mlx5_qp->wr_started)) {
		return 0;
	}

	rc = ibv_wr_complete(mlx5_qp->qpex);

	if (spdk_unlikely(rc)) {
		/* If ibv_wr_complete reports an error that means that no WRs are posted to NIC */
		*bad_wr = spdk_rdma_qp->send_wrs.first;
	}

	spdk_rdma_qp->send_wrs.first = NULL;
	spdk_rdma_qp->stats->send.doorbell_updates++;
	mlx5_qp->wr_started = false;

	return rc;
}

static struct mlx5_dv_rdma_memory_domain *
mlx5_dv_rdma_get_memory_domain(struct ibv_pd *pd)
{
	struct mlx5_dv_rdma_memory_domain *domain = NULL;
	struct spdk_memory_domain_ctx ctx;
	int rc;

	pthread_mutex_lock(&g_memory_domains_lock);

	TAILQ_FOREACH(domain, &g_memory_domains, link) {
		if (domain->pd == pd) {
			domain->ref++;
			pthread_mutex_unlock(&g_memory_domains_lock);
			return domain;
		}
	}

	domain = calloc(1, sizeof(*domain));
	if (!domain) {
		SPDK_ERRLOG("Memory allocation failed\n");
		pthread_mutex_unlock(&g_memory_domains_lock);
		return NULL;
	}

	domain->rdma_ctx.size = sizeof(domain->rdma_ctx);
	domain->rdma_ctx.ibv_pd = pd;
	ctx.size = sizeof(ctx);
	ctx.user_ctx = &domain->rdma_ctx;

	rc = spdk_memory_domain_create(&domain->domain, SPDK_DMA_DEVICE_TYPE_RDMA, &ctx,
				       SPDK_RDMA_DMA_DEVICE);
	if (rc) {
		SPDK_ERRLOG("Failed to create memory domain\n");
		free(domain);
		pthread_mutex_unlock(&g_memory_domains_lock);
		return NULL;
	}

	domain->pd = pd;
	domain->ref = 1;
	TAILQ_INSERT_TAIL(&g_memory_domains, domain, link);

	pthread_mutex_unlock(&g_memory_domains_lock);

	return domain;
}

static void
mlx5_dv_rdma_put_memory_domain(struct mlx5_dv_rdma_memory_domain **_domain)
{
	struct mlx5_dv_rdma_memory_domain *domain;

	if (!*_domain) {
		return;
	}

	domain = *_domain;
	*_domain = NULL;

	pthread_mutex_lock(&g_memory_domains_lock);

	assert(domain->ref > 0);

	domain->ref--;

	if (domain->ref == 0) {
		spdk_memory_domain_destroy(domain->domain);
		TAILQ_REMOVE(&g_memory_domains, domain, link);
		free(domain);
	}

	pthread_mutex_unlock(&g_memory_domains_lock);
}

int
spdk_rdma_qp_accel_seq_ctx_create(struct spdk_rdma_qp *spdk_rdma_qp,
				  struct spdk_rdma_accel_sequence_ctx **_ctx)
{
	struct mlx5_dv_rdma_accel_sequence_ctx *ctx;
	struct mlx5dv_mkey_init_attr attr;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->map = spdk_rdma_utils_create_mem_map(spdk_rdma_qp->qp->pd, NULL,
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!ctx->map) {
		free(ctx);
		return -EINVAL;
	}

	ctx->domain = mlx5_dv_rdma_get_memory_domain(spdk_rdma_qp->qp->pd);
	if (!ctx->domain) {
		spdk_rdma_utils_free_mem_map(&ctx->map);
		free(ctx);
		return -EINVAL;
	}

	attr.pd = spdk_rdma_qp->qp->pd;
	attr.max_entries = 16;
	attr.create_flags = MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT | /* This MKEY refers to another MKEYs */
			    MLX5DV_MKEY_INIT_ATTR_FLAGS_CRYPTO;

	ctx->mkey = mlx5dv_create_mkey(&attr);
	if (!ctx->mkey) {
		spdk_rdma_utils_free_mem_map(&ctx->map);
		mlx5_dv_rdma_put_memory_domain(&ctx->domain);
		free(ctx);
		return -ENOTSUP;
	}

	ctx->base.qp = spdk_rdma_qp;

	*_ctx = &ctx->base;

	return 0;
}

void
spdk_rdma_qp_accel_seq_ctx_release(struct spdk_rdma_accel_sequence_ctx **_ctx)
{
	struct mlx5_dv_rdma_accel_sequence_ctx *ctx;

	if (!*_ctx) {
		return;
	}

	ctx = SPDK_CONTAINEROF(*_ctx, struct mlx5_dv_rdma_accel_sequence_ctx, base);
	if (ctx->mkey) {
		mlx5dv_destroy_mkey(ctx->mkey);
	}
	if (ctx->map) {
		spdk_rdma_utils_free_mem_map(&ctx->map);
	}
	if (ctx->domain) {
		mlx5_dv_rdma_put_memory_domain(&ctx->domain);
	}

	free(ctx);

	*_ctx = NULL;
}

static inline int
mlx5_dv_fill_sge(struct mlx5_dv_rdma_accel_sequence_ctx *ctx, struct spdk_iov_sgl *iovs,
		 uint64_t data_size, struct spdk_memory_domain *domain, void *domain_ctx)
{
	struct spdk_rdma_utils_memory_translation translation;
	struct spdk_memory_domain_translation_ctx translation_ctx;
	struct spdk_memory_domain_translation_result dma_translation = {.iov_count = 0};
	struct spdk_memory_domain *local_domain = ctx->domain->domain;
	struct ibv_sge *sge = ctx->sg;
	size_t size;
	void *addr;
	int i = 0;
	int rc;

	ctx->sge_count = 0;

	while (data_size && ctx->sge_count < 16) {
		size = spdk_min(data_size, iovs->iov->iov_len - iovs->iov_offset);
		addr = (void *)iovs->iov->iov_base + iovs->iov_offset;
		if (domain) {
			/* SPDK_NOTICELOG("translate memory domain\n"); */
			translation_ctx.size = sizeof(struct spdk_memory_domain_translation_ctx);
			translation_ctx.rdma.ibv_qp = ctx->base.qp->qp;
			dma_translation.size = sizeof(struct spdk_memory_domain_translation_result);

			rc = spdk_memory_domain_translate_data(domain, domain_ctx,
							       local_domain, &translation_ctx, addr,
							       size, &dma_translation);
			if (spdk_unlikely(rc || dma_translation.iov_count > 1)) {
				SPDK_ERRLOG("Memory domain translation failed, addr %p, length %zu, rc %d\n", addr, size, rc);
				if (rc == 0) {
					rc = -EINVAL;
				}

				return rc;
			}
			sge[i].lkey = dma_translation.rdma.lkey;
			sge[i].addr = (uint64_t)dma_translation.iov.iov_base;
			sge[i].length = dma_translation.iov.iov_len;
		} else {
			/* SPDK_NOTICELOG("translate mem map\n"); */
			rc = spdk_rdma_utils_get_translation(ctx->map, addr, size, &translation);
			if (spdk_unlikely(rc)) {
				SPDK_ERRLOG("Memory translation failed, addr %p, length %zu\n", addr, size);
				return rc;
			}
			sge[i].lkey = spdk_rdma_utils_memory_translation_get_lkey(&translation);
			sge[i].addr = (uint64_t)addr;
			sge[i].length = size;

		}
		spdk_iov_sgl_advance(iovs, size);
		i++;
		data_size -= size;
	}

	if (data_size) {
		SPDK_ERRLOG("%"PRIu64" bytes were not consumed\n", data_size);
		/* Should never happen, there should be 1:1 relation between iovcnt and sgecnt */
		assert(0);

		return -EINVAL;
	}

	return i;
}
#if 0
static int
mlx5_dv_accel_manager_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task **_task,
				   struct spdk_accel_external_manager *manager, void *manager_arg)
{
	struct mlx5_dv_rdma_accel_sequence_ctx *ctx;
	struct spdk_rdma_mlx5_dv_qp *qp;
	struct mlx5dv_mkey_conf_attr mkey_attr = {};
	struct mlx5dv_crypto_attr crypto_attr;
	struct spdk_accel_task *task;
	struct iovec *data_iov;
	struct spdk_iov_sgl data_sgl;
	struct spdk_memory_domain *domain;
	void *domain_ctx;
	uint32_t data_iovcnt;
	uint32_t i;
	size_t nbytes = 0;
	int rc;

	if (!*_task || !manager_arg) {
		return -EINVAL;
	}

	ctx = SPDK_CONTAINEROF(manager_arg, struct mlx5_dv_rdma_accel_sequence_ctx, base);
	qp = SPDK_CONTAINEROF(ctx->base.qp, struct spdk_rdma_mlx5_dv_qp, common);

	task = *_task;
	/* In encrypt operation unencrypted data resides in `src` buffer.
	 * bdev_crypto may assign a destination buffer for crypto operation, but we don't
	 * need since encrypted data will go directly to the wire
	 * In decrypt flow we need to decrypt data from the wire and put it directly into
	 * user's buffer, which is `dst` in accel task */
	if (task->op_code == ACCEL_OPC_ENCRYPT) {
		data_iov = task->s.iovs;
		data_iovcnt = task->s.iovcnt;
		domain = task->src_domain;
		domain_ctx = task->src_domain_ctx;
		/* SPDK_NOTICELOG("Encrypt, iov %p %zu, count %d, iv %"PRIu64"\n", data_iov->iov_base, data_iov->iov_len, data_iovcnt, task->iv); */
	} else if (task->op_code == ACCEL_OPC_DECRYPT) {
		data_iov = task->d.iovs;
		data_iovcnt = task->d.iovcnt;
		domain = task->dst_domain;
		domain_ctx = task->dst_domain_ctx;
		/* SPDK_NOTICELOG("Decrypt, iov %p %zu, count %d, iv %"PRIu64"\n", data_iov->iov_base, data_iov->iov_len, data_iovcnt, task->iv); */
	} else {
		return -ENOTSUP;
	}

	if (TAILQ_NEXT(task, seq_link) != NULL) {
		SPDK_ERRLOG("Tasks after crypto are not supported\n");
		return -ENOTSUP;
	}

	if (data_iovcnt > 16) {
		SPDK_ERRLOG("Invalid iovcnt in src (%u) or dst (%u) - must not exceed 16\n", task->s.iovcnt,
			    task->d.iovcnt);
		return -ENOTSUP;
	}

	rc = spdk_mlx5_crypto_set_attr(&crypto_attr,
				       (struct spdk_mlx5_crypto_keytag *)task->crypto_key->priv, ctx->base.qp->qp->pd, task->block_size,
				       task->iv, true);
	if (rc) {
		/* Many reasons why this function could fail. Return an error to accel fw, it will try to execute the task */
		return rc;
	}

	spdk_iov_sgl_init(&data_sgl, data_iov, (int)data_iovcnt, 0);
	for (i = 0; i < data_iovcnt; ++i) {
		nbytes += data_iov[i].iov_len;
	}

	rc = mlx5_dv_fill_sge(ctx, &data_sgl, nbytes, domain, domain_ctx);
	if (rc < 0) {
		return rc;
	}
	ctx->sge_count = rc;

	if (!qp->wr_started) {
		ibv_wr_start(qp->qpex);
		qp->wr_started = true;
	}

	qp->qpex->wr_flags = IBV_SEND_INLINE;
	qp->qpex->wr_id = (uint64_t)ctx;

	mlx5dv_wr_mkey_configure(qp->mqpx, ctx->mkey, 3, &mkey_attr);
	mlx5dv_wr_set_mkey_access_flags(qp->mqpx,
					IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	/* SPDK_NOTICELOG("Config mkey, sg count %d\n", ctx->sge_count); */
	mlx5dv_wr_set_mkey_layout_list(qp->mqpx, ctx->sge_count, ctx->sg);
	mlx5dv_wr_set_mkey_crypto(qp->mqpx, &crypto_attr);

	/* Fill translation data */
	ctx->base.result.addr = NULL;
	ctx->base.result.length = nbytes;
	ctx->base.result.lkey = ctx->mkey->lkey;
	ctx->base.result.rkey = ctx->mkey->rkey;

	/* SPDK_NOTICELOG("Mkey: lkey %u, rkey %u, addr %p, len %zu\n", ctx->base.result.lkey, ctx->base.result.rkey,
			       ctx->base.result.addr, ctx->base.result.length); */

	spdk_accel_task_complete(task, 0);

	return 0;
}

static struct spdk_accel_external_manager g_mlx5dv_accel_manager = {
	.accel_manager_submit_tasks = mlx5_dv_accel_manager_submit_tasks,
};


static void
__attribute__((constructor))
mlx5_dv_register_external_accel_manager(void)
{
	int rc;

	rc = spdk_accel_register_external_manager(&g_mlx5dv_accel_manager);
	if (rc) {
		SPDK_ERRLOG("Failed to register external accel manager\n");
	}
}
#endif

int
spdk_rdma_qp_apply_accel_seq(struct spdk_accel_sequence *seq,
			     struct spdk_rdma_accel_sequence_ctx *ctx, accel_seq_done_fn cpl_cb, void *cb_arg)
{
	int rc;

	if (!seq || !ctx) {
		return -EINVAL;
	}

	/* SPDK_NOTICELOG("Executing accel sequence\n"); */
	spdk_accel_sequence_set_external_manager_arg(seq, ctx);
	rc = spdk_accel_sequence_finish(seq, cpl_cb, cb_arg);
	if (rc) {
		return rc;
	}

	return -EINPROGRESS;
}

bool
spdk_rdma_accel_seq_supported(void)
{
	return true;
}
