/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) Mellanox Technologies LTD. All rights reserved.
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

#ifndef SPDK_RDMA_H
#define SPDK_RDMA_H

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

struct spdk_rdma_qp_init_attr {
	void		       *qp_context;
	struct ibv_cq	       *send_cq;
	struct ibv_cq	       *recv_cq;
	struct ibv_srq	       *srq;
	struct ibv_qp_cap	cap;
	struct ibv_pd	       *pd;
};

struct spdk_rdma_send_wr_list {
	struct ibv_send_wr	*first;
	struct ibv_send_wr	*last;
};

struct spdk_rdma_qp {
	struct ibv_qp *qp;
	struct rdma_cm_id *cm_id;
	struct spdk_rdma_send_wr_list send_wrs;
};

/**
 * Create RDMA provider specific qpair
 * \param cm_id Pointer to RDMACM cm_id
 * \param qp_attr Pointer to qpair init attributes
 * \return Pointer to a newly created qpair on success or NULL on failure
 */
struct spdk_rdma_qp *spdk_rdma_qp_create(struct rdma_cm_id *cm_id,
		struct spdk_rdma_qp_init_attr *qp_attr);

/**
 * Accept a connection request. Called by the passive side (NVMEoF target)
 * \param spdk_rdma_qp Pointer to a qpair
 * \param conn_param Optional information needed to establish the connection
 * \return 0 on success, errno on failure
 */
int spdk_rdma_qp_accept(struct spdk_rdma_qp *spdk_rdma_qp, struct rdma_conn_param *conn_param);

/**
 * Complete the connection process, must be called by the active
 * side (NVMEoF initiator) upon receipt RDMA_CM_EVENT_CONNECT_RESPONSE
 * \param spdk_rdma_qp Pointer to a qpair
 * \return 0 on success, errno on failure
 */
int spdk_rdma_qp_complete_connect(struct spdk_rdma_qp *spdk_rdma_qp);

/**
 * Destroy RDMA provider specific qpair
 * \param spdk_rdma_qp Pointer to qpair to be destroyed
 */
void spdk_rdma_qp_destroy(struct spdk_rdma_qp *spdk_rdma_qp);

/**
 * Disconnect a connection and transition assoiciated qpair to error state.
 * Generates RDMA_CM_EVENT_DISCONNECTED on both connection sides
 * \param spdk_rdma_qp Pointer to qpair to be destroyed
 */
int spdk_rdma_qp_disconnect(struct spdk_rdma_qp *spdk_rdma_qp);

/**
 * Append the given send wr structure to the qpair's outstanding sends list.
 * This function accepts either a single Work Request or the first WR in a linked list.
 *
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair
 * \param first Pointer to the first Work Request
 * \return true if there were no outstanding WRs before, false otherwise
 */
bool spdk_rdma_qp_queue_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr *first);

/**
 * Submit all queued Work Request
 * \param spdk_rdma_qp Pointer to SPDK RDMA qpair
 * \param bad_wr Stores a pointer to the first failed WR if this function return nonzero value
 * \return 0 on succes, errno on failure
 */
int spdk_rdma_qp_flush_send_wrs(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_send_wr **bad_wr);

struct spdk_rdma_poller_context;
struct spdk_rdma_poller_context *
spdk_rdma_create_poller_context(struct rdma_cm_id *cm_id, struct spdk_rdma_qp_init_attr *qp_attr);

int spdk_rdma_qp_set_poller_context(struct spdk_rdma_qp *spdk_rdma_qp,
                                            struct spdk_rdma_poller_context *poller_ctx);
uint32_t spdk_rdma_send_qp_num(struct spdk_rdma_qp *spdk_rdma_qp);
uint32_t spdk_rdma_recv_qp_num(struct spdk_rdma_qp *spdk_rdma_qp);

struct ibv_pd *spdk_rdma_qp_pd(struct spdk_rdma_qp *spdk_rdma_qp);
int spdk_rdma_query_qp_dci(struct spdk_rdma_qp *spdk_rdma_qp,  struct ibv_qp_attr *attr,
			   enum ibv_qp_attr_mask attr_mask,
			   struct ibv_qp_init_attr *init_attr);
int spdk_rdma_query_qp_dct(struct spdk_rdma_qp *spdk_rdma_qp,  struct ibv_qp_attr *attr,
			   enum ibv_qp_attr_mask attr_mask,
			   struct ibv_qp_init_attr *init_attr);

void spdk_rdma_qp_set_remote_dctn(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t dctn);
uint32_t spdk_rdma_qp_get_local_dctn(struct spdk_rdma_qp *spdk_rdma_qp);
bool spdk_rdma_is_corresponded_qp(struct spdk_rdma_qp *spdk_rdma_qp, struct ibv_wc *wc);
void spdk_rdma_qp_set_remote_dci(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t dci_qp_num);
struct ibv_qp *spdk_rdma_receive_qp(struct spdk_rdma_qp *spdk_rdma_qp);
struct ibv_qp *spdk_rdma_send_qp(struct spdk_rdma_qp *spdk_rdma_qp);
uint32_t spdk_rdma_generate_qpair_id(struct spdk_rdma_qp *spdk_rdma_qp);
void spdk_rdma_qp_assign_id(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t assigned_id);
void spdk_rdma_notify_qp_on_send_completion(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t wrs_released);
int spdk_rdma_qp_get_qpn_reservation(struct spdk_rdma_qp *spdk_rdma_qp, uint32_t *qpn_reservation);

#endif /* SPDK_RDMA_H */
