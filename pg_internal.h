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

/* Protocol Modes */
#define PG_MODE_TYPE_RENDEZVOUS 1
#define PG_MODE_TYPE_EAGER      2
#define PG_MODE_TYPE_AUTO       3

#if defined(PG_MODE_EAGER)
#define PG_ACTIVE_MODE          PG_MODE_TYPE_EAGER
#elif defined(PG_MODE_RENDEZVOUS)
#define PG_ACTIVE_MODE          PG_MODE_TYPE_RENDEZVOUS
#else
#define PG_ACTIVE_MODE          PG_MODE_TYPE_AUTO
#endif

/* Eager Protocol Constants (ADR-0002) */
#define PG_EAGER_THRESHOLD      (8 * 1024)   /* 8 KiB */
#define PG_EAGER_POOL_DEPTH     32           /* 32 pre-posted buffers per QP */
#define PG_EAGER_WINDOW         8            /* In-flight send flow control window */
#define PG_EAGER_BUF_SIZE       (PG_PIPELINE_CHUNK > PG_EAGER_THRESHOLD ? PG_PIPELINE_CHUNK : PG_EAGER_THRESHOLD)

/* Pipelining Constants (256 KiB chunk, 32 in-flight window, 8 signal interval) */
#define PG_PIPELINE_CHUNK        (256 * 1024)  /* 256 KiB optimal sweet spot */
#define PG_RDMA_WINDOW           32            /* 32 in-flight micro-chunks */
#define PG_RDMA_SIGNAL_INTERVAL  8             /* Signal every 8 WRs (2 MiB pipeline step) */

/* Multi-WR Linked-List Batching & Streaming Store Thresholds */
#ifndef PG_DEFAULT_BATCH_SIZE
#define PG_DEFAULT_BATCH_SIZE    8             /* 8 chained WRs per ibv_post_send */
#endif
#ifndef PG_STREAMING_STORE_THRESHOLD
#define PG_STREAMING_STORE_THRESHOLD (64 * 1024) /* 64 KiB */
#endif

/* Benchmark Harness Constants */
#ifndef PG_BENCH_MIN_BYTES
#define PG_BENCH_MIN_BYTES       (64ULL * 1024ULL * 1024ULL)   /* 64 MiB */
#endif
#ifndef PG_BENCH_MAX_BYTES
#if (PG_ACTIVE_MODE == PG_MODE_TYPE_EAGER)
#define PG_BENCH_MAX_BYTES       (16ULL * 1024ULL * 1024ULL)   /* 16 MiB max for Eager */
#else
#define PG_BENCH_MAX_BYTES       (1024ULL * 1024ULL * 1024ULL) /* 1 GiB max for Rendezvous */
#endif
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
            uint64_t remote_addr; /* Remote staging/recvbuf virtual address (Rendezvous protocol) */
            uint32_t rkey;        /* Remote memory key (Rendezvous protocol) */
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
#define PG_PENDING_QUEUE_MAX    64

struct pg_pending_entry {
    struct pg_ctrl_msg msg;
    char eager_buf[PG_EAGER_BUF_SIZE + PG_CTRL_MSG_LEN];
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

    /* Pre-allocated Unified Receive Buffers and MRs (ADR-0001, ADR-0002) */
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

    /* Internal Staging and Working Buffers (ADR-0002) */
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
        if (elen > (PG_EAGER_BUF_SIZE + PG_CTRL_MSG_LEN)) elen = PG_EAGER_BUF_SIZE + PG_CTRL_MSG_LEN;
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

/* MPI-style Remainder Distribution Math Helpers */
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

/* Unified 1-SGE receive buffer slot repost (ADR-0001, ADR-0002) */
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

/* ========================================================================= */
/* Deep Progress Engine Module (ADR-0001, CONTEXT.md)                        */
/* Encapsulates CQ polling, wr_id decoding, automatic receive buffer        */
/* replenishment, and pending FIFO message queue matching.                   */
/* ========================================================================= */

struct pg_progress_event {
    int type;                               /* PG_WR_TYPE_RECV_CTRL, PG_WR_TYPE_RDMA_WRITE, PG_WR_TYPE_SEND_CTRL, PG_WR_TYPE_EAGER_SEND */
    int qp_dir;                             /* PG_QP_DIR_TO_NEXT (0) or PG_QP_DIR_FROM_PREV (1) */
    uint32_t slot;                          /* Slot index or micro-chunk sequence index */
    struct pg_ctrl_msg msg;                 /* Message content (for PG_WR_TYPE_RECV_CTRL) */
    char eager_buf[PG_EAGER_BUF_SIZE + PG_CTRL_MSG_LEN]; /* Copy of eager payload if type is EAGER_PAYLOAD */
    uint32_t eager_len;                     /* Length of eager payload */
};

/* Pop matching message from internal FIFO pending queue */
static inline int pg_progress_pop_pending(struct pg_context *ctx, int qp_dir, int msg_type, uint32_t seg_idx, struct pg_progress_event *out_event) {
    if (!ctx || qp_dir < 0 || qp_dir >= 2 || !out_event) return 0;
    struct pg_ctrl_msg msg;
    char slot_buf[PG_EAGER_SLOT_SIZE];
    if (pg_pending_pop_matching(ctx, qp_dir, msg_type, seg_idx, &msg, slot_buf)) {
        out_event->type = PG_WR_TYPE_RECV_CTRL;
        out_event->qp_dir = qp_dir;
        out_event->slot = 0;
        out_event->msg = msg;
        out_event->eager_len = 0;
        if (msg.type == PG_CTRL_MSG_EAGER_PAYLOAD) {
            uint32_t elen = msg.payload.rdv.length + PG_CTRL_MSG_LEN;
            if (elen > (PG_EAGER_BUF_SIZE + PG_CTRL_MSG_LEN)) elen = PG_EAGER_BUF_SIZE + PG_CTRL_MSG_LEN;
            memcpy(out_event->eager_buf, slot_buf, elen);
            out_event->eager_len = elen;
        }
        return 1;
    }
    return 0;
}

/* Push unhandled or future-step message into pending queue */
static inline void pg_progress_push_pending(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *msg, const void *slot_buf) {
    pg_pending_push(ctx, qp_dir, msg, slot_buf);
}

/* Non-blocking single CQ poll and event decoder with auto receive slot replenishment */
static inline int pg_progress_poll(struct pg_context *ctx, struct pg_progress_event *out_event) {
    if (!ctx || !out_event) return PG_ERR_INVAL;

    struct ibv_wc wc;
    int ne = ibv_poll_cq(ctx->cq, 1, &wc);
    if (ne < 0) {
        fprintf(stderr, "[pg_progress] Error: ibv_poll_cq failed with code %d\n", ne);
        return PG_ERR_RDMA;
    }
    if (ne == 0) {
        return 0; /* No completion ready */
    }

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[pg_progress] Error: CQ completion error %s (%d) on wr_id 0x%lx\n",
                ibv_wc_status_str(wc.status), wc.status, (unsigned long)wc.wr_id);
        return PG_ERR_RDMA;
    }

    out_event->type = pg_wr_type(wc.wr_id);
    out_event->qp_dir = pg_wr_qp(wc.wr_id);
    out_event->slot = pg_wr_slot(wc.wr_id);
    out_event->eager_len = 0;

    if (out_event->type == PG_WR_TYPE_RECV_CTRL) {
        int dir = out_event->qp_dir;
        int slot = (int)out_event->slot;
        struct pg_ctrl_msg *rhdr = pg_recv_slot_msg(ctx, dir, slot);

        if (rhdr->tag != PG_CTRL_TAG) {
            fprintf(stderr, "[pg_progress] Error: Invalid tag 0x%08x on qp_dir %d slot %d\n",
                    rhdr->tag, dir, slot);
            return PG_ERR_RDMA;
        }

        out_event->msg = *rhdr;
        if (rhdr->type == PG_CTRL_MSG_EAGER_PAYLOAD) {
            uint32_t elen = rhdr->payload.rdv.length + PG_CTRL_MSG_LEN;
            if (elen > (PG_EAGER_BUF_SIZE + PG_CTRL_MSG_LEN)) elen = PG_EAGER_BUF_SIZE + PG_CTRL_MSG_LEN;
            memcpy(out_event->eager_buf, ctx->recv_slot_buf[dir][slot], elen);
            out_event->eager_len = elen;
        }

        /* Automatically repost the consumed receive buffer slot */
        if (pg_repost_recv_slot(ctx, dir, slot)) {
            perror("[pg_progress] Error: Failed to repost receive buffer slot");
            return PG_ERR_RDMA;
        }
    }

    return 1;
}

/* Watchdog-timed progress event waiter (polls until completion or timeout) */
static inline int pg_progress_wait(struct pg_context *ctx, double timeout_sec, struct pg_progress_event *out_event) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        int rc = pg_progress_poll(ctx, out_event);
        if (rc < 0) return rc;
        if (rc == 1) return 1;

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= timeout_sec) {
            fprintf(stderr, "[pg_progress] Error: Timed out after %.2f s waiting for CQ event\n", elapsed);
            return PG_ERR_TIMEOUT;
        }
    }
}

/* ============================================================================
 * Ring Step Transfer Engine (Pipelined Micro-Chunk Transfer)
 * ============================================================================ */

typedef void (*pg_chunk_handler_fn)(void *dest, const void *src, size_t len, void *user_ctx);

struct pg_ring_step_desc {
    uint32_t step_idx;          /* 0-indexed ring step */

    /* Outbound */
    uint32_t send_tag;          /* Segment index or rank origin tag */
    const void *send_buf;       /* Base memory address of outbound slice */
    size_t send_bytes;          /* Length in bytes of outbound slice */
    uint32_t send_lkey;         /* Local MR lkey */

    /* Inbound */
    uint32_t recv_tag;          /* Expected segment index or rank origin tag */
    void *recv_target_addr;     /* Advertised inbound destination (e.g. staging_buf or recvbuf) */
    uint32_t recv_rkey;         /* Advertised inbound destination rkey */
    size_t recv_bytes;          /* Length in bytes of inbound slice */

    /* Inbound micro-chunk processing */
    pg_chunk_handler_fn on_recv_chunk; /* Callback per received micro-chunk (e.g. reduction or memcpy) */
    void *cb_dest;                     /* Destination buffer pointer for callback */
    void *cb_user_ctx;                 /* User context passed into callback */
};

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

/* Vectorized Reduction Math Engine */
void pg_reduce_buffer(void *dest, const void *src, int count,
                      DATATYPE datatype, OPERATION op, int use_streaming);

/* RDMA Operation Helpers */
int pg_post_rdma_write(struct pg_context *ctx, int qp_dir, void *local_addr, size_t length,
                       uint32_t lkey, uint64_t remote_addr, uint32_t rkey);

/* Distributed Ring Barrier */
int pg_barrier(void *pg_handle);

/* Ring All-Gather Generalized Core Engine (Zero-Copy RDMA Write) */
int pg_ring_all_gather_generalized(struct pg_context *ctx, void *recvbuf, int count, DATATYPE datatype);

#endif /* PG_INTERNAL_H */


