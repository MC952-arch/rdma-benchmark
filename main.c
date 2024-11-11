#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <unistd.h>
#include <assert.h>

#define IB_PORT 1 // Define IB_PORT with an appropriate value
typedef enum {
    SENDRECV,
    WRITE,
} rdma_op_t;

struct config_t {
    int msg_size;
    int num_concurr_msgs;
    int num_qps;
    int num_concurr_msgs_per_qp;
    int num_blocks_per_qp;
    int sig_interval;
    int msg_inline; // 0: no inline, 1: inline
    int msg_block_size;
    rdma_op_t op;
};

struct rdma_context {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp **qps;
    struct ibv_srq *srq;
    struct ibv_port_attr port_attr;
    struct ibv_device_attr dev_attr;
    struct ibv_send_wr *send_wrs;
    struct ibv_sge *send_sges;
    char *buffer;
    size_t buffer_size;
    // for remote access
    uint32_t rkey; 
    uint64_t raddr;
};

void print_usage(const char *prog_name) {
    printf("Usage: %s -m <msg_size> -n <num_concurr_msgs> -q <num_qps> -s <sig_interval> -i <msg_inline> -b <msg_block_size> -o <op>\n", prog_name);
    printf("  -m  Message size (in bytes)\n");
    printf("  -n  Number of concurrent messages\n");
    printf("  -q  Number of queue pairs\n");
    printf("  -s  Signal interval\n");
    printf("  -i  Message inline\n");
    printf("  -b  Message block size\n");
    printf("  -o  Operation type\n");
}

void die(const char *reason) {
    fprintf(stderr, "%s\n", reason);
    exit(EXIT_FAILURE);
}

void success(const char *reason) {
    fprintf(stdout, "%s\n", reason);
}

struct rdma_context *init_rdma_context(struct config_t *config, struct ibv_device *device) {
    int ret = 0;
    struct rdma_context *ctx = malloc(sizeof(struct rdma_context));
    if(!ctx) {
        die("Failed to allocate rdma_context");
    } else {
        success("Successfully allocated rdma_context");
    }

    ctx->context = ibv_open_device(device);
    if(!ctx->context) {
        die("Failed to open device");
    } else {
        success("Successfully opened device");
        printf("  Device Name: %s\n", ibv_get_device_name(ctx->context->device));
        printf("  Device Path: %s\n", ctx->context->device->dev_path);
        printf("  IB Device Name: %s\n", ctx->context->device->name);
    }

    ctx->pd = ibv_alloc_pd(ctx->context);
    if(!ctx->pd) {
        die("Failed to allocate protection domain");
    } else {
        success("Successfully allocated protection domain");
    }

    ctx->buffer_size = config->msg_size * config->num_concurr_msgs;
    ctx->buffer = malloc(ctx->buffer_size);
    if(!ctx->buffer) {
        die("Failed to allocate buffer");
    } else {
        success("Successfully allocated buffer");
    }
    printf("  Buffer Size: %lu\n", ctx->buffer_size);

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buffer, ctx->buffer_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if(!ctx->mr) {
        die("Failed to register memory region");
    } else {
        success("Successfully registered memory region");
    }

    ret = ibv_query_port(ctx->context, IB_PORT, &ctx->port_attr);
    if(ret) {
        die("Failed to query IB port information");
    } else {
        success("Successfully queried IB port information");
    }

    ret = ibv_query_device(ctx->context, &ctx->dev_attr);
    if(ret) {
        die("Failed to query device information");
    } else {
        success("Successfully queried device information");
    }

    ctx->cq = ibv_create_cq(ctx->context, ctx->dev_attr.max_cqe, NULL, NULL, 0);
    if(!ctx->cq) {
        die("Failed to create completion queue");
    } else {
        success("Successfully created completion queue");
    }

    struct ibv_srq_init_attr srq_init_attr = {
        .attr.max_wr  = config->num_concurr_msgs,
        .attr.max_sge = 1,
    };
    ctx->srq = ibv_create_srq (ctx->pd, &srq_init_attr);
    if(!ctx->srq) {
        die("Failed to create shared receive queue");
    } else {
        success("Successfully created shared receive queue");
    }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        // .srq = ctx->srq,
        .cap = {
            .max_send_wr = config->num_concurr_msgs_per_qp,
            .max_recv_wr = config->num_concurr_msgs_per_qp,
            .max_send_sge = 1,
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };
    if (config->msg_inline) {
        qp_attr.cap.max_inline_data = config->msg_size;
    }

    ctx->qps = (struct ibv_qp **)calloc (config->num_qps, sizeof(struct ibv_qp *));
    if (!ctx->qps) {
        die("Failed to allocate qps");
    } else {
        success("Successfully allocated qps");
    }

    for (int i = 0; i < config->num_qps; i++) {
        ctx->qps[i] = ibv_create_qp(ctx->pd, &qp_attr);
        if (!ctx->qps[i]) {
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
        if (ibv_modify_qp(ctx->qps[i], &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
            die("Failed to modify QP to INIT");
        } else {
            success("Successfully modified QP to INIT");
        }
    }


    ctx->send_wrs = (struct ibv_send_wr *)calloc(config->num_concurr_msgs, sizeof(struct ibv_send_wr));
    if (!ctx->send_wrs) {
        die("Failed to allocate send_wrs");
    } else {
        success("Successfully allocated send_wrs");
    }

    ctx->send_sges = (struct ibv_sge *)calloc(config->num_concurr_msgs, sizeof(struct ibv_sge));
    if (!ctx->send_sges) {
        die("Failed to allocate send_sges");
    } else {
        success("Successfully allocated send_sges");
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

void post_receive(struct config_t *config, struct rdma_context *ctx) {
    printf("Posting receive request\n");
    int buf_offset = 0;
    char *buf_ptr  = ctx->buffer;
    for (int i = 0; i < config->num_concurr_msgs; i++) {
        struct ibv_sge sge = {
            .addr = (uintptr_t)buf_ptr,
            .length = config->msg_size,
            .lkey = ctx->mr->lkey,
        };
        struct ibv_recv_wr recv_wr = {
            .wr_id = (uint64_t)i,
            .sg_list = &sge,
            .num_sge = 1,
        };
        struct ibv_recv_wr *bad_recv_wr;
        if (ibv_post_recv(ctx->qps[i / config->num_concurr_msgs_per_qp], &recv_wr, &bad_recv_wr)) {
            die("Failed to post receive request");
        }
	    buf_offset = (buf_offset + config->msg_size) % ctx->buffer_size;
	    buf_ptr    = ctx->buffer + buf_offset;
    }
    printf("Successfully posted receive request\n");
}

void post_send(struct config_t *config, struct rdma_context *ctx, const char *msg) {
    strncpy(ctx->buffer, msg, ctx->buffer_size);
    printf("Posting send request\n");
    int buf_offset = 0;
    char *buf_ptr  = ctx->buffer;
    for (int i = 0; i < config->num_concurr_msgs; i++) {
        struct ibv_sge sge = {
            .addr = (uintptr_t)buf_ptr,
            .length = config->msg_size,
            .lkey = ctx->mr->lkey,
        };
        struct ibv_send_wr send_wr = {
            .wr_id = (uint64_t)i,
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
        };
        struct ibv_send_wr *bad_send_wr;
        if (ibv_post_send(ctx->qps[i / config->num_concurr_msgs_per_qp], &send_wr, &bad_send_wr)) {
            die("Failed to post send request");
        }
	    buf_offset = (buf_offset + config->msg_size) % ctx->buffer_size;
	    buf_ptr    = ctx->buffer + buf_offset;
    }
    printf("Successfully posted send request\n");
}

void post_write(struct config_t *config, struct rdma_context *ctx, const char *msg) {
    strncpy(ctx->buffer, msg, ctx->buffer_size);
    printf("Posting write request\n");
    int ind = 0;
    int buf_offset = 0;
    char *buf_ptr = ctx->buffer;
    uint64_t raddr = ctx->raddr;
    for (int i = 0; i < config->num_concurr_msgs; i++) {
        ctx->send_sges[i].addr = (uintptr_t)buf_ptr;
        ctx->send_sges[i].length = config->msg_size;
        ctx->send_sges[i].lkey = ctx->mr->lkey;
	    buf_offset = (buf_offset + config->msg_size) % ctx->buffer_size;
        buf_ptr = ctx->buffer + buf_offset;
    }
    for (int i = 0; i < config->num_qps; i++) {
        for (int j = 0; j < (config->num_blocks_per_qp - 1); j++) {
            for (int k = 0; k < config->msg_block_size; k++) {
	            ctx->send_wrs[ind].wr_id = ind;
	            ctx->send_wrs[ind].next	= (k < config->msg_block_size - 1) ? &ctx->send_wrs[ind + 1] : NULL;
	            ctx->send_wrs[ind].sg_list = &ctx->send_sges[ind];
	            ctx->send_wrs[ind].num_sge = 1;
	            ctx->send_wrs[ind].opcode = IBV_WR_RDMA_WRITE;
	            ctx->send_wrs[ind].wr.rdma.remote_addr = raddr;
	            ctx->send_wrs[ind].wr.rdma.rkey = ctx->rkey;
                  if (ind % config->sig_interval == 0) {
                      ctx->send_wrs[ind].send_flags = IBV_SEND_SIGNALED;
                  }
                  if (config->msg_inline) {
                      ctx->send_wrs[ind].send_flags |= IBV_SEND_INLINE;
                  }
	            ind += 1;
	            buf_offset = (buf_offset + config->msg_size) % ctx->buffer_size;
	            raddr = ctx->raddr + buf_offset;
            }
        }
        for (int k = 0; k < (config->num_concurr_msgs_per_qp % config->msg_block_size); k++) {
	        ctx->send_wrs[ind].wr_id = ind;
	        ctx->send_wrs[ind].next	= (k < (config->num_concurr_msgs_per_qp % config->msg_block_size - 1)) ? &ctx->send_wrs[ind + 1] : NULL;
	        ctx->send_wrs[ind].sg_list = &ctx->send_sges[ind];
	        ctx->send_wrs[ind].num_sge = 1;
	        ctx->send_wrs[ind].opcode = IBV_WR_RDMA_WRITE;
	        ctx->send_wrs[ind].wr.rdma.remote_addr = raddr;
	        ctx->send_wrs[ind].wr.rdma.rkey = ctx->rkey;
              if (ind % config->sig_interval == 0) {
                  ctx->send_wrs[ind].send_flags = IBV_SEND_SIGNALED;
              }
              if (config->msg_inline) {
                  ctx->send_wrs[ind].send_flags |= IBV_SEND_INLINE;
              }
	        ind += 1;
	        buf_offset = (buf_offset + config->msg_size) % ctx->buffer_size;
	        raddr = ctx->raddr + buf_offset;
        }
    }

    ind = 0;
    buf_offset = 0;
    buf_ptr = ctx->buffer;
    raddr = ctx->raddr;
    for (int i = 0; i < config->num_qps; i++) {
        for (int j = 0; j < config->num_blocks_per_qp; j++) {
            struct ibv_send_wr *bad_send_wr;
            if (ibv_post_send(ctx->qps[i], &ctx->send_wrs[ind], &bad_send_wr)) {
                die("Failed to post write request");
            }
            ind += config->msg_block_size;
	        buf_offset = buf_offset + config->msg_size * config->msg_block_size;
	        buf_ptr = ctx->buffer + buf_offset;
	        raddr = ctx->raddr + buf_offset;
        }
    }
    printf("Successfully posted write request\n");
}

void poll_completion(struct config_t *config, struct rdma_context *ctx) {
    int num_completions;
    struct ibv_wc *wcs;
    wcs = (struct ibv_wc *)calloc(config->num_blocks_per_qp * config->num_qps, sizeof(struct ibv_wc));
    do {
        num_completions = ibv_poll_cq(ctx->cq, config->num_blocks_per_qp * config->num_qps, wcs);
    } while (num_completions == 0);

    if (num_completions < 0) {
        die("Failed to poll completion queue");
    }

    for (int i = 0; i < num_completions; i++) {
        if (wcs[i].status != IBV_WC_SUCCESS) {
            die("Work completion failed");
        }
    }
}

void cleanup_rdma_context(struct config_t *config, struct rdma_context *ctx) {
    for (int i = 0; i < config->num_qps; i++) {
        ibv_destroy_qp(ctx->qps[i]);
    }
    ibv_destroy_cq(ctx->cq);
    ibv_dereg_mr(ctx->mr);
    ibv_dealloc_pd(ctx->pd);
    ibv_close_device(ctx->context);
    free(ctx->buffer);
    free(ctx);
}

int main (int argc, char *argv[]) {
    struct config_t *config = malloc(sizeof(struct config_t));
    config->msg_size = 4096;
    config->num_concurr_msgs = 1;
    config->num_qps = 1;
    config->sig_interval = 1000;
    config->msg_inline = 0;
    config->msg_block_size = config->num_concurr_msgs;
    config->op = SENDRECV;

    int opt;
    while ((opt = getopt(argc, argv, "m:n:q:s:i:b:o:")) != -1) {
        switch (opt) {
            case 'm':
                config->msg_size = atoi(optarg);
                break;
            case 'n':
                config->num_concurr_msgs = atoi(optarg);
                break;
            case 'q':
                config->num_qps = atoi(optarg);
                break;
            case 's':
                config->sig_interval = atoi(optarg);
                break;
            case 'i':
                config->msg_inline = atoi(optarg) % 2;
                break;
            case 'b':
                config->msg_block_size = atoi(optarg);
                break;
            case 'o':
                config->op = atoi(optarg);
                break;
            default:  // '?'
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    assert(config->num_concurr_msgs % config->num_qps == 0 && "num_concurr_msgs must be divisible by num_qps");
    config->num_concurr_msgs_per_qp = config->num_concurr_msgs / config->num_qps;
    config->num_blocks_per_qp = (config->op == WRITE) ? ((config->num_concurr_msgs_per_qp + config->msg_block_size - 1) / config->msg_block_size) : 1;
    printf("msg_size = %d, num_concurr_msgs = %d, num_qps = %d, num_concurr_msgs_per_qp = %d, num_blocks_per_qp = %d, sig_interval = %d, msg_inline = %s, msg_block_size = %d, op = %s\n",
           config->msg_size,
           config->num_concurr_msgs,
           config->num_qps,
           config->num_concurr_msgs_per_qp,
           config->num_blocks_per_qp,
           config->sig_interval,
           config->msg_inline ? "MSG_INLINE" : "MSG_NOINLINE",
           config->msg_block_size,
           config->op == SENDRECV ? "SENDRECV" : "WRITE");
    
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        die("Failed to get IB devices list");
    } else {
        success("Successfully got IB devices list");
        for (int i = 0; dev_list[i] != NULL; ++i) {
            printf("  Device %d: %s\n", i, ibv_get_device_name(dev_list[i]));
        }
    }

    struct rdma_context *ctx_sender   = init_rdma_context(config, dev_list[2]);
    struct rdma_context *ctx_receiver = init_rdma_context(config, dev_list[8]);

    // Store receiver's rkey and raddr in sender's context
    ctx_sender->rkey = ctx_receiver->mr->rkey;
    ctx_sender->raddr = (uintptr_t)ctx_receiver->mr->addr;

    for (int i = 0; i < config->num_qps; i++) {
        // Extract QP number and LID
        uint32_t qpn_sender = ctx_sender->qps[i]->qp_num;
        uint32_t qpn_receiver = ctx_receiver->qps[i]->qp_num;
        uint16_t lid_sender = ctx_sender->port_attr.lid;
        uint16_t lid_receiver = ctx_receiver->port_attr.lid;
        // Modify QP to RTR/RTS
        modify_qp_to_rtr(ctx_sender->qps[i], qpn_receiver, lid_receiver, NULL);
        modify_qp_to_rtr(ctx_receiver->qps[i], qpn_sender, lid_sender, NULL);
        modify_qp_to_rts(ctx_sender->qps[i]);
        modify_qp_to_rts(ctx_receiver->qps[i]);
    }

    const char *msg = "Hello, RDMA!";
    if (config->op == SENDRECV) {
        // Perform RDMA SEND/RECV operations
        // Receiver should post receive first
        post_receive(config, ctx_receiver);

        // Sender should post send next
        post_send(config, ctx_sender, msg);

        // Poll for completion
        poll_completion(config, ctx_sender);
        poll_completion(config, ctx_receiver);
        // Display received message
        printf("Received message: %s\n", ctx_receiver->buffer);
    } else {
        // Perform RDMA WRITE operation
        // Sender should post write
        post_write(config, ctx_sender, msg);

        // Poll for completion
        poll_completion(config, ctx_sender);
        // Display received message
        printf("Received message: %s\n", ctx_receiver->buffer);
    }

    cleanup_rdma_context(config, ctx_sender);
    cleanup_rdma_context(config, ctx_receiver);
    ibv_free_device_list(dev_list);
    free(config);

    return 0;
}