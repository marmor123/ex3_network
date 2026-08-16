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

/* Protocol Modes (Summary Sec 9) */
#define PG_MODE_TYPE_RENDEZVOUS 1
#define PG_MODE_TYPE_EAGER      2
#define PG_MODE_TYPE_AUTO       3

#if defined(PG_MODE_EAGER)
#define PG_ACTIVE_MODE          PG_MODE_TYPE_EAGER
#elif defined(PG_MODE_AUTO)
#define PG_ACTIVE_MODE          PG_MODE_TYPE_AUTO
#else
#define PG_ACTIVE_MODE          PG_MODE_TYPE_RENDEZVOUS
#endif

/* Eager Protocol Constants (ADR-0002 & Summary Sec 10, 25) */
#define PG_EAGER_THRESHOLD      (8 * 1024)   /* 8 KiB */
#define PG_EAGER_POOL_DEPTH     32           /* 32 pre-posted buffers per QP */
#define PG_EAGER_WINDOW         8            /* In-flight send flow control window */
#define PG_EAGER_BUF_SIZE       (PG_PIPELINE_CHUNK > PG_EAGER_THRESHOLD ? PG_PIPELINE_CHUNK : PG_EAGER_THRESHOLD)

/* Pipelining and Profile Constants (Summary Sec 21, 24, 25) */
#ifdef PROFILE_PERF
#define PG_PIPELINE_CHUNK        (256 * 1024)  /* 256 KiB */
#define PG_RDMA_WINDOW           32
#define PG_RDMA_SIGNAL_INTERVAL  8
#else
#ifndef PG_PIPELINE_CHUNK
#define PG_PIPELINE_CHUNK        (64 * 1024)   /* 64 KiB */
#endif
#ifndef PG_RDMA_WINDOW
#define PG_RDMA_WINDOW           1
#endif
#ifndef PG_RDMA_SIGNAL_INTERVAL
#define PG_RDMA_SIGNAL_INTERVAL  1
#endif
#endif

/* Benchmark Harness Constants (Summary Sec 29) */
#ifndef PG_BENCH_MIN_BYTES
#define PG_BENCH_MIN_BYTES       (64ULL * 1024ULL * 1024ULL)   /* 64 MiB */
#endif
#ifndef PG_BENCH_MAX_BYTES
#define PG_BENCH_MAX_BYTES       (1024ULL * 1024ULL * 1024ULL) /* 1 GiB */
#endif
#ifndef PG_BENCH_ITER
#define PG_BENCH_ITER            5
#endif

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
#define PG_WR_TYPE_EAGER_SEND   9

/* wr_id Bit-packing helpers (ADR-0001) */
static inline uint64_t pg_make_wr(int qp_dir, int type) {
    return ((uint64_t)(type & 0x0F)) | (((uint64_t)(qp_dir & 0x01)) << 4);
}

static inline uint64_t pg_make_wr_slot(int qp_dir, int type, uint32_t slot) {
    return ((uint64_t)(type & 0x0F)) | (((uint64_t)(qp_dir & 0x01)) << 4) | (((uint64_t)slot) << 8);
}

static inline int pg_wr_type(uint64_t wr_id) {
    return (int)(wr_id & 0x0F);
}

static inline int pg_wr_qp(uint64_t wr_id) {
    return (int)((wr_id >> 4) & 0x01);
}

static inline uint32_t pg_wr_slot(uint64_t wr_id) {
    return (uint32_t)(wr_id >> 8);
}

/* Control Message Types */
#define PG_CTRL_MSG_PING            1
#define PG_CTRL_MSG_PONG            2
#define PG_CTRL_MSG_RTS             3
#define PG_CTRL_MSG_CTS             4
#define PG_CTRL_MSG_DATA_DONE       5
#define PG_CTRL_MSG_BARRIER_COLLECT 6
#define PG_CTRL_MSG_BARRIER_RELEASE 7
#define PG_CTRL_MSG_BARRIER_ACK     8
#define PG_CTRL_MSG_EAGER_PAYLOAD   9

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

/* MR Cache Constants (ADR-0002) */
#define PG_MR_CACHE_MAX         32

struct pg_mr_entry {
    void *addr;
    size_t length;
    int access_flags;
    struct ibv_mr *mr;
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

#define PG_EAGER_SLOT_SIZE      (PG_CTRL_MSG_LEN + PG_EAGER_BUF_SIZE)

    /* Pre-allocated Unified Receive Buffers and MRs (ADR-0001, ADR-0002, V9) */
    char *recv_slot_buf[2][PG_CTRL_POOL_DEPTH];
    void *recv_slot_raw_mem[2];
    struct ibv_mr *recv_slot_mr[2];

    /* Control & Eager Send Header Buffers */
    char ctrl_send_buf[2][PG_CTRL_MSG_LEN];
    struct ibv_mr *ctrl_send_mr[2];
    char eager_send_hdr_buf[2][PG_CTRL_POOL_DEPTH][PG_CTRL_MSG_LEN];
    struct ibv_mr *eager_send_hdr_mr[2];

    /* Lazy MR Cache (ADR-0002) */
    struct pg_mr_entry mr_cache[PG_MR_CACHE_MAX];
    int mr_cache_count;

    /* Internal Staging and Working Buffers (ADR-0002 & V4 Reduce-Scatter) */
    void *staging_buf;
    size_t staging_capacity;
    struct ibv_mr *staging_mr;

    void *work_buf;
    size_t work_capacity;
    struct ibv_mr *work_mr;

    /* Pending control message queue (1 slot per QP direction) */
    struct pg_ctrl_msg pending_ctrl_msg[2];
    char pending_slot_buf[2][PG_EAGER_SLOT_SIZE];
    int has_pending_ctrl[2];

    /* TCP Bootstrap QP metadata */
    struct pg_tcp_qp_info local_to_next;           /* Local QP info for next rank */
    struct pg_tcp_qp_info local_from_prev;         /* Local QP info for prev rank */
    struct pg_tcp_qp_info remote_to_next;          /* Received QP info from next rank */
    struct pg_tcp_qp_info remote_from_prev;        /* Received QP info from prev rank */
};

/* Unified 1-SGE receive buffer slot repost (ADR-0001, ADR-0002, V9) */
static inline int pg_repost_recv_slot(struct pg_context *ctx, int qp_dir, int slot) {
    struct ibv_sge sge = {
        .addr   = (uintptr_t)ctx->recv_slot_buf[qp_dir][slot],
        .length = (uint32_t)PG_EAGER_SLOT_SIZE,
        .lkey   = ctx->recv_slot_mr[qp_dir]->lkey
    };
    struct ibv_recv_wr wr = {
        .wr_id   = pg_make_wr_slot(qp_dir, PG_WR_TYPE_RECV_CTRL, slot),
        .sg_list = &sge,
        .num_sge = 1,
        .next    = NULL
    };
    struct ibv_recv_wr *bad_wr = NULL;
    struct ibv_qp *target_qp = (qp_dir == PG_QP_DIR_TO_NEXT) ? ctx->qp_to_next : ctx->qp_from_prev;
    return ibv_post_recv(target_qp, &wr, &bad_wr);
}

#define pg_repost_ctrl_recv_slot(ctx, dir, slot)  pg_repost_recv_slot(ctx, dir, slot)
#define pg_repost_eager_recv_slot(ctx, dir, slot) pg_repost_recv_slot(ctx, dir, slot)
#define pg_recv_slot_msg(ctx, dir, slot)          ((struct pg_ctrl_msg *)ctx->recv_slot_buf[dir][slot])
#define pg_recv_slot_payload(ctx, dir, slot)      ((void *)((char *)ctx->recv_slot_buf[dir][slot] + PG_CTRL_MSG_LEN))

/* Internal RDMA and TCP bootstrap helper functions */
int pg_rdma_init_resources(struct pg_context *ctx);
int pg_rdma_connect_qp(struct ibv_qp *qp, const struct pg_tcp_qp_info *remote,
                       uint32_t my_psn, enum ibv_mtu active_mtu);
int pg_tcp_bootstrap(struct pg_context *ctx);
int pg_post_ctrl_send(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *msg);
int pg_post_eager_send(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *hdr,
                       void *payload_addr, uint32_t payload_len, uint32_t lkey, int signaled, int slot);
int pg_rdma_ring_ping(struct pg_context *ctx);
void pg_rdma_cleanup(struct pg_context *ctx);

/* Memory Registration Cache Helpers (ADR-0002) */
struct ibv_mr *pg_get_or_reg_mr(struct pg_context *ctx, void *addr, size_t length, int access_flags);
int pg_ensure_internal_buffers(struct pg_context *ctx, size_t count_bytes, size_t segment_bytes);

/* RDMA Operation Helpers */
int pg_post_rdma_write(struct pg_context *ctx, int qp_dir, void *local_addr, size_t length,
                       uint32_t lkey, uint64_t remote_addr, uint32_t rkey);

/* Distributed Ring Barrier (Summary Sec 22) */
int pg_barrier(void *pg_handle);

/* Ring All-Gather Core Engine (Zero-Copy RDMA Write) */
int pg_ring_all_gather_core(struct pg_context *ctx, void *recvbuf, size_t segment_bytes);

/* V3 Rendezvous Segment Transfer Test */
int pg_test_v3_rendezvous(void *pg_handle, void *sendbuf, void *recvbuf, size_t size_bytes);

#endif /* PG_INTERNAL_H */


