#ifndef PG_INTERNAL_H
#define PG_INTERNAL_H

#include "pg.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <infiniband/verbs.h>
#include <emmintrin.h>
#include <smmintrin.h>
#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif

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

/* Pipelining and Profile Constants (Summary Sec 21, 24, 25 & V10 Cluster Tuning) */
#ifdef PROFILE_PERF
#define PG_PIPELINE_CHUNK        (256 * 1024)  /* 256 KiB (optimal sweet spot) */
#define PG_RDMA_WINDOW           32            /* 32 in-flight micro-chunks */
#define PG_RDMA_SIGNAL_INTERVAL  8             /* Signal every 8 WRs (2 MiB pipeline step) */
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

/* Multi-WR Linked-List Batching & Streaming Store Thresholds (V10) */
#ifndef PG_DEFAULT_BATCH_SIZE
#define PG_DEFAULT_BATCH_SIZE    8             /* 8 chained WRs per ibv_post_send */
#endif
#ifndef PG_STREAMING_STORE_THRESHOLD
#define PG_STREAMING_STORE_THRESHOLD (64 * 1024) /* 64 KiB */
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

/* Pending control message queues (FIFO per QP direction with Pool Indirection) */
#define PG_PENDING_QUEUE_MAX    1024

struct pg_pending_entry {
    struct pg_ctrl_msg msg;
    char eager_buf[PG_EAGER_THRESHOLD + PG_CTRL_MSG_LEN];
    uint32_t eager_len;
    int in_use;
};

struct pg_pending_queue {
    struct pg_pending_entry pool[PG_PENDING_QUEUE_MAX];
    int ring[PG_PENDING_QUEUE_MAX];
    int head;
    int tail;
    int count;
};

/* MR Cache Constants (ADR-0002) */
#define PG_MR_CACHE_MAX         1024

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

    /* Runtime Hyperparameters & Tuning (V10) */
    size_t pipeline_chunk;
    int rdma_window;
    int rdma_signal_interval;
    int batch_size;
    size_t eager_threshold;
    int eager_window;
    int use_streaming_stores;

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

    /* Pending control message queues (FIFO per QP direction) */
    struct pg_pending_queue pending_q[2];

    /* TCP Bootstrap QP metadata */
    struct pg_tcp_qp_info local_to_next;           /* Local QP info for next rank */
    struct pg_tcp_qp_info local_from_prev;         /* Local QP info for prev rank */
    struct pg_tcp_qp_info remote_to_next;          /* Received QP info from next rank */
    struct pg_tcp_qp_info remote_from_prev;        /* Received QP info from prev rank */
};

static inline void pg_pending_push(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *msg, const void *slot_buf) {
    if (!ctx || qp_dir < 0 || qp_dir >= 2 || !msg) return;
    struct pg_pending_queue *q = &ctx->pending_q[qp_dir];
    if (q->count >= PG_PENDING_QUEUE_MAX) {
        fprintf(stderr, "[pg] Error: Pending control queue overflow on qp_dir %d (count=%d)\n", qp_dir, q->count);
        return;
    }

    int slot = -1;
    for (int k = 0; k < PG_PENDING_QUEUE_MAX; k++) {
        if (!q->pool[k].in_use) {
            slot = k;
            break;
        }
    }
    if (slot < 0) return;

    q->pool[slot].in_use = 1;
    q->pool[slot].msg = *msg;
    q->pool[slot].eager_len = 0;
    if (msg->type == PG_CTRL_MSG_EAGER_PAYLOAD && slot_buf) {
        uint32_t elen = msg->payload.rdv.length + PG_CTRL_MSG_LEN;
        if (elen > (PG_EAGER_THRESHOLD + PG_CTRL_MSG_LEN)) elen = PG_EAGER_THRESHOLD + PG_CTRL_MSG_LEN;
        memcpy(q->pool[slot].eager_buf, slot_buf, elen);
        q->pool[slot].eager_len = elen;
    }

    q->ring[q->tail] = slot;
    q->tail = (q->tail + 1) % PG_PENDING_QUEUE_MAX;
    q->count++;
}

static inline int pg_pending_pop_matching(struct pg_context *ctx, int qp_dir, int type, uint32_t seg_idx, struct pg_ctrl_msg *out_msg, void *out_slot_buf) {
    if (!ctx || qp_dir < 0 || qp_dir >= 2) return 0;
    struct pg_pending_queue *q = &ctx->pending_q[qp_dir];
    if (q->count == 0) return 0;

    for (int i = 0; i < q->count; i++) {
        int ring_idx = (q->head + i) % PG_PENDING_QUEUE_MAX;
        int slot = q->ring[ring_idx];
        struct pg_pending_entry *entry = &q->pool[slot];
        struct pg_ctrl_msg *m = &entry->msg;

        if (m->type == type && (seg_idx == (uint32_t)-1 || m->payload.rdv.seg_idx == seg_idx)) {
            if (out_msg) *out_msg = *m;
            if (out_slot_buf && entry->eager_len > 0) {
                memcpy(out_slot_buf, entry->eager_buf, entry->eager_len);
            }
            entry->in_use = 0;

            if (i == 0) {
                q->head = (q->head + 1) % PG_PENDING_QUEUE_MAX;
            } else {
                for (int j = i; j < q->count - 1; j++) {
                    int cur = (q->head + j) % PG_PENDING_QUEUE_MAX;
                    int next = (q->head + j + 1) % PG_PENDING_QUEUE_MAX;
                    q->ring[cur] = q->ring[next];
                }
                q->tail = (q->tail - 1 + PG_PENDING_QUEUE_MAX) % PG_PENDING_QUEUE_MAX;
            }
            q->count--;
            return 1;
        }
    }
    return 0;
}

/* Datatype Size Helper */
static inline size_t pg_get_datatype_size(DATATYPE datatype) {
    switch (datatype) {
        case PG_INT:    return sizeof(int);
        case PG_FLOAT:  return sizeof(float);
        case PG_DOUBLE: return sizeof(double);
        default:        return sizeof(int);
    }
}

/* MPI-style Remainder Distribution Math Helpers (V10) */
static inline int pg_get_seg_count(int rank, int count, int size) {
    int q = count / size;
    int r = count % size;
    return q + (rank < r ? 1 : 0);
}

static inline size_t pg_get_seg_offset_elems(int rank, int count, int size) {
    int q = count / size;
    int r = count % size;
    return (size_t)rank * (size_t)q + (size_t)(rank < r ? rank : r);
}

static inline size_t pg_get_seg_offset_bytes(int rank, int count, int size, size_t elem_size) {
    return pg_get_seg_offset_elems(rank, count, size) * elem_size;
}

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

/* Runtime Hyperparameter Init */
void pg_init_tuning_params(struct pg_context *ctx);

/* Memory Registration Cache Helpers (ADR-0002) */
struct ibv_mr *pg_get_or_reg_mr(struct pg_context *ctx, void *addr, size_t length, int access_flags);
int pg_ensure_internal_buffers(struct pg_context *ctx, size_t count_bytes, size_t segment_bytes);

/* Vectorized Reduction Math Engine (V10) */
void pg_reduce_buffer(void *dest, const void *src, int count,
                      DATATYPE datatype, OPERATION op, int use_streaming);

/* RDMA Operation Helpers */
int pg_post_rdma_write(struct pg_context *ctx, int qp_dir, void *local_addr, size_t length,
                       uint32_t lkey, uint64_t remote_addr, uint32_t rkey);

/* Distributed Ring Barrier (Summary Sec 22) */
int pg_barrier(void *pg_handle);

/* Ring All-Gather Generalized Core Engine (Zero-Copy RDMA Write) */
int pg_ring_all_gather_generalized(struct pg_context *ctx, void *recvbuf, int count, DATATYPE datatype);
int pg_ring_all_gather_core(struct pg_context *ctx, void *recvbuf, size_t segment_bytes);

/* V3 Rendezvous Segment Transfer Test */
int pg_test_v3_rendezvous(void *pg_handle, void *sendbuf, void *recvbuf, size_t size_bytes);

#endif /* PG_INTERNAL_H */


