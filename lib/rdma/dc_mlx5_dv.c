/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
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

#include <rdma/rdma_cma.h>
#include <infiniband/mlx5dv.h>

#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/nvmf_transport.h"

#include "spdk_internal/rdma.h"
#include "spdk_internal/log.h"

#include "ibv_helper.h"

struct spdk_rdma_poller_context {};

struct spdk_dc_mlx5_dv_poller_context {      
	struct spdk_rdma_poller_context common;
	struct ibv_qp *qp_dci;
	struct ibv_qp_ex *qp_dci_qpex;
	struct mlx5dv_qp_ex *qp_dci_mqpex;

	struct ibv_qp *srq; /*FIXME now owning. Just pointer to SRQ*/
	struct ibv_qp *qp_dct;
	bool   activated;
	struct spdk_dc_mlx5_dv_qp *last_qp;	
};

struct spdk_dc_mlx5_dv_qp {
	struct spdk_rdma_qp common; 
	struct spdk_dc_mlx5_dv_poller_context *poller_ctx;
	uint32_t remote_dctn;
	uint32_t remote_dci;
	uint64_t remote_dc_key;
	struct ibv_ah *ah;
	/* we don't expect concurent usage of spdk_dc_mlx5_dv_qp */
	__be32 dctn;
	struct ibv_send_wr *bad_wr;
	int    last_flush_rc;
};

#define DC_KEY 0xDC00DC00DC00DC00 /*FIXME ???*/

static int 
dc_mlx5_dv_init_dci(struct spdk_dc_mlx5_dv_poller_context *poller_ctx, struct rdma_cm_id *cm_id)
{
	int attr_mask = 0;
	int rc = 0;
	struct ibv_qp *qp = poller_ctx->qp_dci;        

	/* modify QP to INIT */
	{
		attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;

		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = cm_id->port_num,
		};

/*		rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask);*/
		if (rc) {
			SPDK_ERRLOG("Failed to init attr IBV_QPS_INIT, errno %s (%d)\n", spdk_strerror(errno), errno);
			return rc;
		}

		rc = ibv_modify_qp(qp, &attr, attr_mask);
		if (rc) {
			SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_INIT) failed, rc %d\n", rc);
			return rc;
		}
	}

	/* modify QP to RTR */
	{
		struct ibv_port_attr port_attr = {0};
		uint8_t is_global = 0;
		uint8_t sgid_index = 0;
		
		attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV;
		if (ibv_query_port(cm_id->verbs, cm_id->port_num, &port_attr)) {
			SPDK_ERRLOG("ib_query_port\n");
			return -1;
		}
		if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
			is_global = 1;
		        sgid_index = ibv_find_sgid_type(cm_id->verbs, cm_id->port_num, IBV_GID_TYPE_ROCE_V2, AF_INET); /* ??? PF_INET */
		}
		struct ibv_qp_attr attr = {
			.qp_state = IBV_QPS_RTR,
			.path_mtu               = port_attr.active_mtu,
			  .min_rnr_timer          = 0x10,
			  .rq_psn                 = 0,
			  .ah_attr                = {
				.is_global      = is_global, /* ??? */
				.sl             = 0,
				.src_path_bits  = 0,
				.port_num       = cm_id->port_num,
				.grh.hop_limit  = 1,
				.grh.sgid_index = sgid_index,
				.grh.traffic_class = 0,
			  }
		};
/*		rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask);*/
		if (rc) {
			SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
			return rc;
		}

		rc = ibv_modify_qp(qp, &attr, attr_mask);
		if (rc) {
			SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTR) failed, rc %d\n", rc);
			return rc;
		}

	}

	/* modify QP to RTS */
	attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT |
		IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
		IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
	// Optional: IB_QP_MIN_RNR_TIMER

	struct ibv_qp_attr attr = {
		.qp_state               = IBV_QPS_RTS,		
		  .timeout                = 0x10,
		  .retry_cnt              = 3,
		  .rnr_retry              = 3,
		  .sq_psn                 = 0,
		  .max_rd_atomic          = 1,
	};


	/* rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask); */
	/* if (rc) { */
	/* 	SPDK_ERRLOG("Failed to init attr IBV_QPS_RTS, errno %s (%d)\n", spdk_strerror(errno), errno); */
	/* 	return rc; */
	/* } */

	rc = ibv_modify_qp(qp, &attr, attr_mask);
	if (rc) {
		SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTS) failed, rc %d\n", rc);
	}
	return rc;
}


static int 
dc_mlx5_dv_init_dct(struct spdk_dc_mlx5_dv_poller_context *poller_ctx, struct rdma_cm_id *cm_id)
{
	int attr_mask = 0;
	int rc = 0;
	struct ibv_qp *qp = poller_ctx->qp_dct;        

	SPDK_NOTICELOG("DCT qp_num before modifications :%"PRIu32"\n", qp->qp_num);
	/* modify QP to INIT */
	{
		attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = cm_id->port_num,
			.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
		};

/*		rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask); */
		if (rc) {
			SPDK_ERRLOG("Failed to init attr IBV_QPS_INIT, errno %s (%d)\n", spdk_strerror(errno), errno);
			return rc;
		}

		rc = ibv_modify_qp(qp, &attr, attr_mask);
		if (rc) {
			SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_INIT) failed, rc %d\n", rc);
			return rc;
		}
	}

	/* modify QP to RTR */
	{
		attr_mask = IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_AV | IBV_QP_MIN_RNR_TIMER;
		struct ibv_port_attr port_attr = {0};
		uint8_t is_global = 0;
		uint8_t sgid_index = 0;
		
		if (ibv_query_port(cm_id->verbs, cm_id->port_num, &port_attr)) {
			SPDK_ERRLOG("ib_query_port\n");
			return -1;
		}
//		SPDK_NOTICELOG("port_attr: %
		if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
			is_global = 1;
		        sgid_index = ibv_find_sgid_type(cm_id->verbs, cm_id->port_num, IBV_GID_TYPE_ROCE_V2, PF_INET); /* ??? PF_INET */
		}
		struct ibv_qp_attr attr = {
			.qp_state = IBV_QPS_RTR,
			.path_mtu               = port_attr.active_mtu,
			  .min_rnr_timer          = 0x10,
			  .rq_psn                 = 0,
			  .ah_attr                = {
				.is_global      = is_global, /* ??? */
				.sl             = 0,
				.src_path_bits  = 0,
				.port_num       = cm_id->port_num,
				.grh.hop_limit  = 1,
				.grh.sgid_index = sgid_index,
				.grh.traffic_class = 0,
			  }
		};

/*		rc = rdma_init_qp_attr(cm_id, &attr, &attr_mask);*/
		if (rc) {
			SPDK_ERRLOG("Failed to init attr IBV_QPS_RTR, errno %s (%d)\n", spdk_strerror(errno), errno);
			return rc;
		}

		rc = ibv_modify_qp(qp, &attr, attr_mask);
		if (rc) {
			SPDK_ERRLOG("ibv_modify_qp(IBV_QPS_RTR) failed, errno %s (%d)\n", spdk_strerror(rc), rc);
			return rc;
		}

	}
	SPDK_NOTICELOG("DCT qp_num after modifications :%"PRIu32"\n", qp->qp_num);
	return rc;
}

static int
dc_mlx5_dv_init_qpairs(struct spdk_dc_mlx5_dv_qp *qp)
{
	int rc = 0;

	rc = dc_mlx5_dv_init_dci(qp->poller_ctx, qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("Failed to init dci\n");
		goto exit;
	}
	rc = dc_mlx5_dv_init_dct(qp->poller_ctx, qp->common.cm_id);
	if (rc) {
		SPDK_ERRLOG("Failed to init dct\n");
		goto exit; /*FIXME destroy DCI first*/
	}

exit:
	return rc;
}

static struct spdk_dc_mlx5_dv_poller_context *
spdk_dc_mlx5_dv_create_poller_context(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr)
{
	assert(cm_id);
	assert(qp_attr);

	struct spdk_dc_mlx5_dv_poller_context *poller_ctx = NULL;

	/* create DCT */

	
	struct ibv_qp_init_attr_ex attr_ex = {
		.qp_type = IBV_QPT_DRIVER,
		.send_cq = qp_attr->send_cq,
		.recv_cq = qp_attr->recv_cq,
		.comp_mask = IBV_QP_INIT_ATTR_PD,
/*		.pd = qp_attr->pd,*/
		.pd = qp_attr->pd ? qp_attr->pd : cm_id->pd,
		.srq = qp_attr->srq
	};
	struct mlx5dv_qp_init_attr attr_dv = {
		.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_DC,
		.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCT,
		.dc_init_attr.dct_access_key = DC_KEY,
	};
	poller_ctx = calloc(1, sizeof(*poller_ctx));
	if (!poller_ctx) {
		SPDK_ERRLOG("poller context memory allocation failed\n");
		goto exit_ok;
	}
//	SPDK_NOTICELOG("
	poller_ctx->qp_dct = mlx5dv_create_qp(cm_id->verbs, &attr_ex, &attr_dv);
	if (!poller_ctx->qp_dct) {
		SPDK_ERRLOG("DCT creation failed. errno %s (%d)\n", spdk_strerror(errno), errno);
		goto exit_free_qps;
	}
	SPDK_NOTICELOG("DCT qp_num:%"PRIu32"\n", poller_ctx->qp_dct->qp_num);
	SPDK_NOTICELOG("DCT pd:%p\n", attr_ex.pd);
	SPDK_NOTICELOG("DCT srq:%p\n", qp_attr->srq);
	
	/* create DCI */
	attr_ex.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
	attr_ex.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE | IBV_QP_EX_WITH_SEND | IBV_QP_EX_WITH_RDMA_READ;
	attr_ex.cap.max_send_wr = 128; /* FIXME */
	attr_ex.cap.max_send_sge = 30; /*FIXME  must be configured in runtime */
	attr_ex.srq = NULL;

	attr_dv.comp_mask |= MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;
	attr_dv.dc_init_attr.dc_type = MLX5DV_DCTYPE_DCI;
	attr_dv.create_flags |= MLX5DV_QP_CREATE_DISABLE_SCATTER_TO_CQE; /*driver doesnt support scatter2cqe data-path on DCI yet*/

	poller_ctx->qp_dci = mlx5dv_create_qp(cm_id->verbs, &attr_ex, &attr_dv);

	if (!poller_ctx->qp_dci) {
		SPDK_ERRLOG("DCI creation failed. errno %s (%d)\n", spdk_strerror(errno), errno);
		goto exit_destroy_dct;
	}
	SPDK_NOTICELOG("DCI qp_num:%"PRIu32"\n", poller_ctx->qp_dci->qp_num);
	poller_ctx->qp_dci_qpex = ibv_qp_to_qp_ex(poller_ctx->qp_dci);
	if (!poller_ctx->qp_dci_qpex) {
		SPDK_ERRLOG("ibv_qp_to_qp_ex(DC). errno %s (%d)\n", spdk_strerror(errno), errno);
		goto exit_destroy_dci;
	}
	poller_ctx->qp_dci_mqpex = mlx5dv_qp_ex_from_ibv_qp_ex(poller_ctx->qp_dci_qpex);
	if (!poller_ctx->qp_dci_mqpex) {
		SPDK_ERRLOG("mlx5dv_qp_ex_from_ibv_qp_ex(DC). errno %s (%d)\n", spdk_strerror(errno), errno);
	}
	
	goto exit_ok;
exit_destroy_dci:
	/*FIXME destroy DCI*/
exit_destroy_dct:
	/*FIXME destroy DCT*/
exit_free_qps:
	free(poller_ctx);
	poller_ctx=NULL;
exit_ok:
	return poller_ctx; 
    
}

static
struct spdk_dc_mlx5_dv_qp * 
spdk_dc_mlx5_dv_create_qp(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr)
{
	struct spdk_dc_mlx5_dv_qp *qp = NULL;

	qp = calloc(1, sizeof(*qp));
	if (!qp) {
		SPDK_ERRLOG("spdk_rdma_qp memory allocation failed\n");
	}
	qp->common.cm_id = cm_id; /* FIXME check if it is needed */
	qp->remote_dc_key = DC_KEY; /*FIXME*/

	return qp;
}

static
int
spdk_dc_qp_accept(struct spdk_dc_mlx5_dv_qp *qp, struct rdma_conn_param *conn_param)
{
	struct spdk_nvmf_rdma_accept_private_data	*accept_data;
	struct ibv_qp_attr ah_qp_attr = {0};
	int ah_qp_attr_mask = 0;
	int rc = 0;

	assert(qp != NULL);
	assert(qp->common.cm_id != NULL);
	assert(qp->poller_ctx != NULL);

	ah_qp_attr.qp_state = IBV_QPS_RTR;
	rc = rdma_init_qp_attr(qp->common.cm_id, &ah_qp_attr, &ah_qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("rdma_init_qp_attr: %s\n",strerror(errno));
		return NULL;
	}
	/*FIXME it doesn't seem that ah_attr consists with such one in modify_qp part*/
	qp->ah = ibv_create_ah(qp->common.cm_id->pd, &ah_qp_attr.ah_attr);
	if (!qp->ah) {
		SPDK_ERRLOG("ibv_create_ah: %s\n", strerror(errno));
		return NULL;
	}

	SPDK_NOTICELOG("created ah (%p)\n", qp->ah);
	
	accept_data = conn_param->private_data;
	accept_data->dctn = qp->poller_ctx->qp_dct->qp_num;
	accept_data->dci_qp_num = qp->poller_ctx->qp_dci->qp_num;
	SPDK_NOTICELOG("Sending dctn: %"PRIu32"\n",accept_data->dctn);
	
	/* NVMEoF target must move qpair to RTS state */
	/* if (dc_mlx5_dv_init_qpairs(qp) != 0) { */
	/* 	SPDK_ERRLOG("Failed to initialize qpairs\n"); */
	/* 	/\* Set errno to be compliant with rdma_accept behaviour *\/ */
	/* 	errno = ECONNABORTED; */
	/* 	return -1; */
	/* } */

	return rdma_accept(qp->common.cm_id, conn_param);
}

static
int
spdk_dc_qp_complete_connect(struct spdk_dc_mlx5_dv_qp *qp)
{
	int rc;

	assert(qp);

	struct ibv_qp_attr qp_attr;
	int qp_attr_mask;

	/* rc = dc_mlx5_dv_init_qpairs(qp); */
	
	/* if (rc) { */
	/* 	SPDK_ERRLOG("Failed to initialize qpair\n"); */
	/* 	return rc; */
	/* } */

	qp_attr.qp_state = IBV_QPS_RTR;
	rc = rdma_init_qp_attr(qp->common.cm_id, &qp_attr, &qp_attr_mask);
	if (rc) {
		SPDK_ERRLOG("rdma_init_qp_attr");
		return rc;
	}
	
	qp->ah = ibv_create_ah(qp->poller_ctx->qp_dci->pd, &qp_attr.ah_attr);
	if (!qp->ah) {
		SPDK_ERRLOG("ibv_create_ah");
		rc = errno;
		return rc;
	}
	SPDK_NOTICELOG("created ah (%p)\n", qp->ah);
	SPDK_NOTICELOG("with    pd (%p)\n", qp->poller_ctx->qp_dci->pd);

	rc = rdma_establish(qp->common.cm_id);

	if (rc) {
		SPDK_ERRLOG("rdma_establish failed, errno %s (%d)\n", spdk_strerror(errno), errno);
	}
	SPDK_NOTICELOG("Complete_connect\n");
	return rc;
}

static
void
spdk_dc_poller_context_destroy(struct spdk_dc_mlx5_dv_poller_context *poller_ctx)
{
	int rc;

	assert(poller_ctx != NULL);

	if (poller_ctx->qp_dci) {
		rc = ibv_destroy_qp(poller_ctx->qp_dci);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy DCI ibv qp %p, rc %d\n", poller_ctx->qp_dci, rc);
		}
	}

	if (poller_ctx->qp_dct) {
		rc = ibv_destroy_qp(poller_ctx->qp_dct);
		if (rc) {
			SPDK_ERRLOG("Failed to destroy DCT ibv qp %p, rc %d\n", poller_ctx->qp_dct, rc);
		}
	}
	free(poller_ctx);
}

static
void
spdk_dc_qp_destroy(struct spdk_dc_mlx5_dv_qp *qp)
{
	assert(qp != NULL);

	if (qp->common.send_wrs.first != NULL) {
		SPDK_WARNLOG("Destroying qpair with queued Work Requests\n");
	}

	/*FIXME to add atomic counter and call poller_context_destroy if needed*/
	free(qp);
}

static int
spdk_dc_qp_to_err_state(struct ibv_qp *qp)
{
	int rc = 0;
	struct ibv_qp_attr qp_attr = {.qp_state = IBV_QPS_ERR};
	assert(qp != NULL);
	rc = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE);
	if (rc) {
		SPDK_ERRLOG("Failed to modify ibv qp %p state to ERR, rc %d\n", qp, rc);
	}
	return rc;
}

static
int
spdk_dc_poller_context_disconnect(struct spdk_dc_mlx5_dv_poller_context *poller_ctx)
{
	int rc = 0;
	assert(poller_ctx != NULL);
	if (poller_ctx->qp_dci) {
		rc = spdk_dc_qp_to_err_state(poller_ctx->qp_dci);
	}

	if (poller_ctx->qp_dct) {
		rc = spdk_dc_qp_to_err_state(poller_ctx->qp_dct);
	}
	return rc;
}

static
int
spdk_dc_qp_disconnect(struct spdk_dc_mlx5_dv_qp *qp)
{
	int rc = 0;

	assert(qp != NULL);
	/*FIXME call poller_context_disconnect if it really needed. IN_USE counter? */
	if (qp->common.cm_id) {
		rc = rdma_disconnect(qp->common.cm_id);
		if (rc) {
			SPDK_ERRLOG("rdma_disconnect failed, errno %s (%d)\n", spdk_strerror(errno), errno);
		}
	}
	return rc;
}

static
int
spdk_dc_qp_flush_send_wrs(struct spdk_dc_mlx5_dv_qp *qp, struct ibv_send_wr **bad_wr);

static
bool
spdk_dc_qp_queue_send_wrs(struct spdk_dc_mlx5_dv_qp *qp, struct ibv_send_wr *first)
{
	struct ibv_send_wr *tmp;
	struct spdk_dc_mlx5_dv_poller_context *poller_ctx;
	bool is_first;

	assert(qp);
	assert(first);
	assert(qp->poller_ctx);
	poller_ctx = qp->poller_ctx;

	is_first = qp->common.send_wrs.first == NULL;
	/* SPDK_NOTICELOG("ah: %p\n", qp->ah); */
	/* SPDK_NOTICELOG("Sending from qpair with dci: %"PRIu32"\n", poller_ctx->qp_dci->qp_num); */
	/* SPDK_NOTICELOG("remote dctn: %"PRIu32"\n", qp->remote_dctn); */
	/* SPDK_NOTICELOG("remote_dc_key: %"PRIu64"\n", qp->remote_dc_key); */

	struct spdk_dc_mlx5_dv_qp *last_qp = poller_ctx->last_qp;
	if (last_qp && last_qp != qp) {
		struct ibv_send_wr *bad_wr = NULL;
		last_qp->last_flush_rc = spdk_dc_qp_flush_send_wrs(last_qp, &bad_wr);
		SPDK_NOTICELOG("last_qp->last_flush_rc got value: %d\n", last_qp->last_flush_rc);
		if (last_qp->last_flush_rc) {
			last_qp->bad_wr = bad_wr;
		}
	}

	if (is_first) {
		SPDK_NOTICELOG("pending trace in is_first case - ibv_wr_start call\n");
		ibv_wr_start(poller_ctx->qp_dci_qpex);
		qp->common.send_wrs.first = first;
	} else {
		SPDK_NOTICELOG("pending trace in else case\n");
		qp->common.send_wrs.last->next = first;
	}

	for (tmp = first; tmp != NULL; tmp = tmp->next) {
		poller_ctx->qp_dci_qpex->wr_id = tmp->wr_id;
		poller_ctx->qp_dci_qpex->wr_flags = tmp->send_flags;

		switch (tmp->opcode) {
		case IBV_WR_SEND:
			ibv_wr_send(poller_ctx->qp_dci_qpex);
			break;
		case IBV_WR_SEND_WITH_INV:
			ibv_wr_send_inv(poller_ctx->qp_dci_qpex, tmp->invalidate_rkey);
			break;
		case IBV_WR_RDMA_READ:
			ibv_wr_rdma_read(poller_ctx->qp_dci_qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			break;
		case IBV_WR_RDMA_WRITE:
			ibv_wr_rdma_write(poller_ctx->qp_dci_qpex, tmp->wr.rdma.rkey, tmp->wr.rdma.remote_addr);
			break;
		default:
			SPDK_ERRLOG("Unexpected opcode %d\n", tmp->opcode);
			assert(0);
		}

		mlx5dv_wr_set_dc_addr(poller_ctx->qp_dci_mqpex,
				      qp->ah, qp->remote_dctn,
				      qp->remote_dc_key);
		ibv_wr_set_sge_list(poller_ctx->qp_dci_qpex, tmp->num_sge, tmp->sg_list);

		qp->common.send_wrs.last = tmp;
	}
	poller_ctx->last_qp = qp;
	return is_first;
}

static
int
spdk_dc_qp_flush_send_wrs(struct spdk_dc_mlx5_dv_qp *qp, struct ibv_send_wr **bad_wr)
{
	int rc;

	assert(bad_wr);
	assert(qp);
	assert(qp->poller_ctx);

	if (spdk_unlikely(qp->last_flush_rc)) {
		SPDK_NOTICELOG("Happened spdk_unlikely(qp->last_flush_rc)\n");
		/*As the previous complete operation was failed let's
		  return the whole list of wrs for reprocessing without
		  any attempts to post them to NIC */
		struct ibv_send_wr *last;
		last = qp->bad_wr;
		while (last->next != NULL) {
			last = last->next;
		}
		last->next = qp->common.send_wrs.first;
		*bad_wr = qp->bad_wr;
		rc = qp->last_flush_rc;
		ibv_wr_abort(qp->poller_ctx->qp_dci_qpex);
	} else if (spdk_unlikely(qp->common.send_wrs.first == NULL)) {
		rc = 0;
	} else {
		rc =  ibv_wr_complete(qp->poller_ctx->qp_dci_qpex);
		if (spdk_unlikely(rc)) {
			/* If ibv_wr_complete reports an error that means that no WRs are posted to NIC */
			*bad_wr = qp->common.send_wrs.first;
		}
	}
	qp->common.send_wrs.first = NULL;
	return rc;
}

struct spdk_rdma_qp *spdk_rdma_qp_create(struct rdma_cm_id *cm_id,
					 struct spdk_rdma_qp_init_attr *qp_attr)
{
	return &spdk_dc_mlx5_dv_create_qp(cm_id, qp_attr)->common;
}

int spdk_rdma_qp_accept(struct spdk_rdma_qp *spdk_rdma_qp, struct rdma_conn_param *conn_param)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_accept(qp, conn_param);
}

int spdk_rdma_qp_complete_connect(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_complete_connect(qp);
}

void spdk_rdma_qp_destroy(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_destroy(qp);
}

int spdk_rdma_qp_disconnect(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_disconnect(qp);
}

bool spdk_rdma_qp_queue_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr *first) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_queue_send_wrs(qp, first);
}

int spdk_rdma_qp_flush_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr **bad_wr) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return spdk_dc_qp_flush_send_wrs(qp, bad_wr);
}

struct spdk_rdma_poller_context *
spdk_rdma_create_poller_context(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr)
{
	return &spdk_dc_mlx5_dv_create_poller_context(cm_id, qp_attr)->common;
}

int spdk_rdma_qp_set_poller_context(struct spdk_rdma_qp *spdk_rdma_qp,
				    struct spdk_rdma_poller_context *poller_ctx)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	if (qp->poller_ctx) {
		SPDK_NOTICELOG("WARNING resetting poller_ctx. old=%p new=%p\n", qp->poller_ctx, poller_ctx);
	}
	qp->poller_ctx = SPDK_CONTAINEROF(poller_ctx, struct spdk_dc_mlx5_dv_poller_context, common);
	if (!qp->poller_ctx->activated && (dc_mlx5_dv_init_qpairs(qp) != 0)) {
		SPDK_ERRLOG("Failed to initialize qpairs\n");
		/* Set errno to be compliant with rdma_accept behaviour */
		errno = ECONNABORTED;
		return -1;
	}
	qp->poller_ctx->activated = true;
	return 0;
}

uint32_t spdk_rdma_send_qp_num(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dci->qp_num;
}

uint32_t spdk_rdma_recv_qp_num(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dct->qp_num;
}


struct ibv_pd *
spdk_rdma_qp_pd(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
        SPDK_NOTICELOG("DCI pd: %p\n",qp->poller_ctx->qp_dci->pd);
        SPDK_NOTICELOG("DCT pd: %p\n",qp->poller_ctx->qp_dct->pd);
	return qp->poller_ctx->qp_dct->pd;
}

int spdk_rdma_query_qp_dci(struct spdk_rdma_qp *spdk_rdma_qp,  struct ibv_qp_attr *attr,
			   enum ibv_qp_attr_mask attr_mask,
			   struct ibv_qp_init_attr *init_attr)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return ibv_query_qp(qp->poller_ctx->qp_dci, attr, attr_mask, init_attr);
}

int spdk_rdma_query_qp_dct(struct spdk_rdma_qp *spdk_rdma_qp,  struct ibv_qp_attr *attr,
			   enum ibv_qp_attr_mask attr_mask,
			   struct ibv_qp_init_attr *init_attr)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return ibv_query_qp(qp->poller_ctx->qp_dct, attr, attr_mask, init_attr);
}

struct ibv_qp *
spdk_rdma_receive_qp(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dct;
}

struct ibv_qp *
spdk_rdma_send_qp(struct spdk_rdma_qp *spdk_rdma_qp)
{
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dci;
}

void spdk_rdma_qp_set_remote_dctn(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t dctn) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	SPDK_NOTICELOG("qp: %p gets dctn: %"PRIu32"\n", qp, dctn);
	qp->remote_dctn = dctn;      
}

void spdk_rdma_qp_set_remote_dci(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t dci_qp_num) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	qp->remote_dci = dci_qp_num;      
}

uint32_t spdk_rdma_qp_get_local_dctn(struct spdk_rdma_qp *spdk_rdma_qp) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	return qp->poller_ctx->qp_dct->qp_num;
}

bool spdk_rdma_is_corresponded_qp(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_wc *wc) {
	struct spdk_dc_mlx5_dv_qp *qp = SPDK_CONTAINEROF(spdk_rdma_qp, struct spdk_dc_mlx5_dv_qp, common);
	SPDK_NOTICELOG("qp->remote_dci: %"PRIu32" qp->poller_ctx->qp_dct->qp_num: %"PRIu32" wc->qp_num: %"PRIu32"\n",
		       qp->remote_dci, qp->poller_ctx->qp_dct->qp_num, wc->qp_num);
	return (qp->remote_dci == wc->src_qp) && (wc->qp_num == qp->poller_ctx->qp_dct->qp_num);	
}
