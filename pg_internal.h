#ifndef PG_INTERNAL_H
#define PG_INTERNAL_H

#include "pg.h"
#include <stdint.h>
#include <infiniband/verbs.h>

/* TCP Bootstrap Constants */
#define PG_TCP_BASE_PORT        19000
#define PG_TCP_READY_TAG        0x52454144u  /* 'READ' in ASCII */
#define PG_TCP_RETRY_MS         100
#define PG_TCP_TIMEOUT_SEC      30
#define PG_TCP_SOCK_TIMEOUT_SEC 5

/* RDMA Hardware Constants */
#define PG_IB_PORT              1
#define PG_CTRL_TAG             0x50474354u  /* 'PGCT' in ASCII */
#define PG_CTRL_POOL_DEPTH      32
#define PG_CTRL_MSG_LEN         64
#define PG_CTRL_POLL_TIMEOUT_SEC 10
#define PG_MAX_INLINE_DECLARE   1024

/* QP Directions (index into per-direction arrays) */
#define PG_QP_DIR_TO_NEXT       0
#define PG_QP_DIR_FROM_PREV     1

/* WR_ID Type Enumerations (Bits 0-3 of wr_id per ADR-0001) */
#define PG_WR_TYPE_RECV_CTRL    1
#define PG_WR_TYPE_SEND_CTRL    2
#define PG_WR_TYPE_RTS          3
#define PG_WR_TYPE_CTS          4
#define PG_WR_TYPE_DATA_DONE    5
#define PG_WR_TYPE_RDMA_WRITE   6
#define PG_WR_TYPE_BARRIER      7
#define PG_WR_TYPE_EAGER_RECV   8

/* wr_id Bit-packing helpers (ADR-0001) */
static inline uint64_t pg_make_wr(int qp_dir, int type) {
    return ((uint64_t)(type & 0x0F)) | (((uint64_t)(qp_dir & 0x01)) << 4);
}

static inline int pg_wr_type(uint64_t wr_id) {
    return (int)(wr_id & 0x0F);
}

static inline int pg_wr_qp(uint64_t wr_id) {
    return (int)((wr_id >> 4) & 0x01);
}

/* Control Message Types */
#define PG_CTRL_MSG_PING        1
#define PG_CTRL_MSG_PONG        2
#define PG_CTRL_MSG_RTS         3
#define PG_CTRL_MSG_CTS         4
#define PG_CTRL_MSG_DATA_DONE   5
#define PG_CTRL_MSG_BARRIER     6

/* 64-byte Control Message Structure */
struct pg_ctrl_msg {
    uint32_t tag;           /* PG_CTRL_TAG (0x50474354) */
    uint16_t type;          /* PG_CTRL_MSG_* */
    uint16_t sender_rank;   /* Sender rank */
    uint32_t seq;           /* Sequence number */
    union {
        struct {
            uint64_t remote_addr; /* Remote staging/recvbuf virtual address (V3 rendezvous) */
            uint32_t rkey;        /* Remote memory key (V3 rendezvous) */
            uint32_t seg_idx;     /* Ring segment index */
            uint32_t micro_idx;   /* Pipelined micro-chunk index */
            uint32_t length;      /* Payload byte length */
        } rdv;
        uint8_t raw[48];          /* Reserved / padding to 64 bytes total */
    } payload;
};

/* TCP QP Metadata exchanged during bootstrap */
struct pg_tcp_qp_info {
    uint32_t qpn;
    uint32_t psn;
    uint16_t lid;
    uint16_t reserved;
};

/* Internal context structure represented by void *pg_handle */
struct pg_context {
    int rank;                                      /* 0-based local rank */
    int size;                                      /* Total number of ranks in ring */
    int prev_rank;                                 /* (rank - 1 + size) % size */
    int next_rank;                                 /* (rank + 1) % size */
    char servername[PG_MAX_HOST_LEN];              /* Validated local servername */
    char host_list[PG_MAX_RANKS][PG_MAX_HOST_LEN]; /* Copy of ring hostnames */

    /* InfiniBand Verbs Resources */
    struct ibv_context *ib_ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp_to_next;                     /* QP sending to next rank (index 0) */
    struct ibv_qp *qp_from_prev;                   /* QP receiving from prev rank (index 1) */

    /* Port and device metadata */
    uint16_t local_lid;
    enum ibv_mtu active_mtu;
    uint32_t max_inline_data[2];                   /* [0]=to_next, [1]=from_prev */
    uint32_t sq_depth[2];                          /* [0]=to_next, [1]=from_prev */

    /* Pre-allocated Control Buffers and MRs */
    char ctrl_recv_buf[2][PG_CTRL_POOL_DEPTH][PG_CTRL_MSG_LEN];
    struct ibv_mr *ctrl_recv_mr[2];
    char ctrl_send_buf[2][PG_CTRL_MSG_LEN];
    struct ibv_mr *ctrl_send_mr[2];

    /* TCP Bootstrap QP metadata */
    struct pg_tcp_qp_info local_to_next;           /* Local QP info for next rank */
    struct pg_tcp_qp_info local_from_prev;         /* Local QP info for prev rank */
    struct pg_tcp_qp_info remote_to_next;          /* Received QP info from next rank */
    struct pg_tcp_qp_info remote_from_prev;        /* Received QP info from prev rank */
};

/* Internal RDMA and TCP bootstrap helper functions */
int pg_rdma_init_resources(struct pg_context *ctx);
int pg_rdma_connect_qp(struct ibv_qp *qp, const struct pg_tcp_qp_info *remote,
                       uint32_t my_psn, enum ibv_mtu active_mtu);
int pg_tcp_bootstrap(struct pg_context *ctx);
int pg_post_ctrl_send(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *msg);
int pg_rdma_ring_ping(struct pg_context *ctx);
void pg_rdma_cleanup(struct pg_context *ctx);

#endif /* PG_INTERNAL_H */


