#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

#define MSG_SIZE 4096
#define IB_PORT 1 // Define IB_PORT with an appropriate value

struct rdma_context {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_port_attr port_attr;
    struct ibv_device_attr dev_attr;
    char *buffer;
    size_t buffer_size;
};

void die(const char *reason) {
    fprintf(stderr, "%s\n", reason);
    exit(EXIT_FAILURE);
}

void success(const char *reason) {
    fprintf(stdout, "%s\n", reason);
}

struct rdma_context *init_rdma_context(struct ibv_device *device) {
    int ret = 0;
    struct rdma_context *ctx = malloc(sizeof(struct rdma_context));
    if (!ctx) {
        die("Failed to allocate rdma_context");
    } else {
        success("Successfully allocated rdma_context");
    }

    ctx->context = ibv_open_device(device);
    if (!ctx->context) {
        die("Failed to open device");
    } else {
        success("Successfully opened device");
        printf("  Device Name: %s\n", ibv_get_device_name(ctx->context->device));
        printf("  Device Path: %s\n", ctx->context->device->dev_path);
        printf("  IB Device Name: %s\n", ctx->context->device->name);
    }

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        die("Failed to allocate protection domain");
    } else {
        success("Successfully allocated protection domain");
    }

    ctx->buffer = malloc(MSG_SIZE);
    ctx->buffer_size = MSG_SIZE;
    if (!ctx->buffer) {
        die("Failed to allocate buffer");
    } else {
        success("Successfully allocated buffer");
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buffer, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->mr) {
        die("Failed to register memory region");
    } else {
        success("Successfully registered memory region");
    }

    ret = ibv_query_port(ctx->context, IB_PORT, &ctx->port_attr);
    if (ret) {
        die("Failed to query IB port information");
    } else {
        success("Successfully queried IB port information");
    }

    ret = ibv_query_device(ctx->context, &ctx->dev_attr);
    if (ret) {
        die("Failed to query device information");
    } else {
        success("Successfully queried device information");
    }

    ctx->cq = ibv_create_cq(ctx->context, ctx->dev_attr.max_cqe, NULL, NULL, 0);
    if (!ctx->cq) {
        die("Failed to create completion queue");
    } else {
        success("Successfully created completion queue");
    }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .cap = {
            .max_send_wr = 1,
            .max_recv_wr = 1,
            .max_send_sge = 1,
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };
    ctx->qp = ibv_create_qp(ctx->pd, &qp_attr);
    if (!ctx->qp) {
        die("Failed to create queue pair");
    } else {
        success("Successfully created queue pair");
    }

    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    };
    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        die("Failed to modify QP to INIT");
    } else {
        success("Successfully modified QP to INIT");
    }

    return ctx;
}

void modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,          // MTU size
        .dest_qp_num = remote_qpn,         // Remote QPN
        .rq_psn = 0,                       // Receive Packet Sequence Number
        .max_dest_rd_atomic = 1,           // Max outstanding RDMA reads
        .min_rnr_timer = 12,               // Minimum RNR NAK timer
        .ah_attr = {
            .is_global = 0,
            .dlid = dlid,                  // Destination LID
            .sl = 0,                       // Service Level
            .src_path_bits = 0,
            .port_num = 1,                 // Local port number
        },
    };

    if (dgid) {
        attr.ah_attr.is_global = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.sgid_index = 0;
        attr.ah_attr.grh.hop_limit = 1;
    }

    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE |
                      IBV_QP_AV |
                      IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN |
                      IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC |
                      IBV_QP_MIN_RNR_TIMER)) {
        die("Failed to modify QP to RTR");
    }
}

void modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,                // Local ACK timeout
        .retry_cnt = 7,               // Retry count
        .rnr_retry = 7,               // RNR retry count
        .sq_psn = 0,                  // Send Packet Sequence Number
        .max_rd_atomic = 1,           // Max outstanding RDMA reads
    };

    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE |
                      IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY |
                      IBV_QP_SQ_PSN |
                      IBV_QP_MAX_QP_RD_ATOMIC)) {
        die("Failed to modify QP to RTS");
    }
}

void post_receive(struct rdma_context *ctx) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)ctx->buffer,
        .length = ctx->buffer_size,
        .lkey = ctx->mr->lkey,
    };

    struct ibv_recv_wr recv_wr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_recv_wr;

    printf("Posting receive request\n");
    if (ibv_post_recv(ctx->qp, &recv_wr, &bad_recv_wr)) {
        die("Failed to post receive request");
    }
    printf("Successfully posted receive request\n");
}

void post_send(struct rdma_context *ctx, const char *msg) {
    strncpy(ctx->buffer, msg, ctx->buffer_size);

    struct ibv_sge sge = {
        .addr = (uintptr_t)ctx->buffer,
        .length = ctx->buffer_size,
        .lkey = ctx->mr->lkey,
    };

    struct ibv_send_wr send_wr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_send_wr;

    printf("Posting send request\n");
    if (ibv_post_send(ctx->qp, &send_wr, &bad_send_wr)) {
        die("Failed to post send request");
    }
    printf("Successfully posted send request\n");
}

void poll_completion(struct rdma_context *ctx) {
    struct ibv_wc wc;
    int num_completions;
    do {
        num_completions = ibv_poll_cq(ctx->cq, 1, &wc);
    } while (num_completions == 0);

    if (num_completions < 0 || wc.status != IBV_WC_SUCCESS) {
        die("Failed to complete operation");
    }
}

void cleanup_rdma_context(struct rdma_context *ctx) {
    ibv_destroy_qp(ctx->qp);
    ibv_destroy_cq(ctx->cq);
    ibv_dereg_mr(ctx->mr);
    ibv_dealloc_pd(ctx->pd);
    ibv_close_device(ctx->context);
    free(ctx->buffer);
    free(ctx);
}

int main() {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        die("Failed to get IB devices list");
    } else {
        success("Successfully got IB devices list");
        for (int i = 0; dev_list[i] != NULL; ++i) {
            printf("  Device %d: %s\n", i, ibv_get_device_name(dev_list[i]));
        }
    }

    struct rdma_context *ctx_sender = init_rdma_context(dev_list[2]);
    struct rdma_context *ctx_receiver = init_rdma_context(dev_list[8]);

    // Extract QP number and LID
    uint32_t qpn_sender = ctx_sender->qp->qp_num;
    uint32_t qpn_receiver = ctx_receiver->qp->qp_num;
    uint16_t lid_sender = ctx_sender->port_attr.lid;
    uint16_t lid_receiver = ctx_receiver->port_attr.lid;

    // Modify QP to RTR/RTS
    modify_qp_to_rtr(ctx_sender->qp, qpn_receiver, lid_receiver, NULL);
    modify_qp_to_rtr(ctx_receiver->qp, qpn_sender, lid_sender, NULL);
    modify_qp_to_rts(ctx_sender->qp);
    modify_qp_to_rts(ctx_receiver->qp);


    // Perform RDMA operations here
    // Receiver should post receive first
    post_receive(ctx_receiver);

    // Sender should post send next
    const char *msg = "Hello, RDMA!";
    post_send(ctx_sender, msg);

    // Poll for completion
    poll_completion(ctx_sender);
    poll_completion(ctx_receiver);
    // Display received message
    printf("Received message: %s\n", ctx_receiver->buffer);

    cleanup_rdma_context(ctx_sender);
    cleanup_rdma_context(ctx_receiver);
    ibv_free_device_list(dev_list);

    return 0;
}