#ifndef PG_INTERNAL_H
#define PG_INTERNAL_H

#include "pg.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <infiniband/verbs.h>
#include <emmintrin.h>
#include <smmintrin.h>

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
#define PG_IB_QP_TIMEOUT        14           /* ~67.1 ms (4.096us * 2^14) */
#define PG_IB_QP_RETRY_CNT      7            /* Maximum 3-bit retry count */
#define PG_IB_QP_RNR_RETRY      7            /* Infinite RNR retry */
#define PG_HUGEPAGE_ALIGN_BYTES (2 * 1024 * 1024) /* 2 MiB hugepage alignment */
#define PG_CACHELINE_ALIGN_BYTES 64          /* 64-byte L1/L2 cacheline alignment */

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

/* Eager Protocol Constants (ADR-0003) */
#define PG_EAGER_THRESHOLD      (64 * 1024)  /* Conservative receive-memory bound */
#define PG_EAGER_WINDOW         8            /* In-flight send flow control window */
#define PG_EAGER_BUF_SIZE       PG_EAGER_THRESHOLD /* Sized strictly to threshold (64 KiB) for 75% memory footprint reduction */

/* Pipelining Constants (256 KiB chunk, 32 in-flight window, 8 signal interval) */
#define PG_PIPELINE_CHUNK        (256 * 1024)  /* 256 KiB optimal sweet spot */
#define PG_SMALL_PIPELINE_CHUNK  (64 * 1024)   /* 64 KiB chunk for sub-256 MiB tensors */
#define PG_ADAPTIVE_CHUNK_THRESHOLD (64ULL * 1024ULL * 1024ULL) /* 64 MiB segment threshold */
#define PG_RDMA_WINDOW           32            /* 32 in-flight micro-chunks */
#define PG_RDMA_SIGNAL_INTERVAL  8             /* Signal every 8 WRs (2 MiB pipeline step) */

/* Multi-WR Linked-List Batching */
#ifndef PG_DEFAULT_BATCH_SIZE
#define PG_DEFAULT_BATCH_SIZE    8             /* 8 chained WRs per ibv_post_send */
#endif
#define PG_MAX_BATCH_SIZE        16            /* Max bounded WR batch depth (Strategy 1) */

/* QP Directions (index into per-direction arrays) */
#define PG_QP_DIR_TO_NEXT       0
#define PG_QP_DIR_FROM_PREV     1

/* WR_ID Type Enumerations (Bits 0-3 of wr_id per ADR-0001) */
#define PG_WR_TYPE_RECV_CTRL    1
#define PG_WR_TYPE_SEND_CTRL    2
#define PG_WR_TYPE_RDMA_WRITE   6
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

/* Strategy 1: Unified inline assembler for batched RDMA Write Work Requests */
static inline void pg_assemble_rdma_write_wr(struct ibv_send_wr *wr, struct ibv_sge *sge,
                                             uint32_t slot, uintptr_t local_addr, uint32_t len,
                                             uint32_t lkey, uint64_t remote_addr, uint32_t rkey,
                                             int is_signaled, struct ibv_send_wr *next) {
    sge->addr   = local_addr;
    sge->length = len;
    sge->lkey   = lkey;

    wr->wr_id      = pg_make_wr_slot(PG_QP_DIR_TO_NEXT, PG_WR_TYPE_RDMA_WRITE, slot);
    wr->opcode     = IBV_WR_RDMA_WRITE;
    wr->send_flags = is_signaled ? IBV_SEND_SIGNALED : 0;
    wr->sg_list    = sge;
    wr->num_sge    = 1;
    wr->next       = next;
    wr->wr.rdma.remote_addr = remote_addr;
    wr->wr.rdma.rkey        = rkey;
}

/* Control Message Types */
#define PG_CTRL_MSG_PING            1
#define PG_CTRL_MSG_RTS             3
#define PG_CTRL_MSG_CTS             4
#define PG_CTRL_MSG_DATA_DONE       5
#define PG_CTRL_MSG_EAGER_PAYLOAD   6

/* 64-byte Control Message Structure */
struct pg_ctrl_msg {
    uint32_t tag;           /* PG_CTRL_TAG (0x50474354) */
    uint16_t type;          /* PG_CTRL_MSG_* */
    uint16_t sender_rank;   /* Sender rank */
    union {
        struct {
            uint64_t remote_addr; /* Remote staging/recvbuf virtual address (Rendezvous protocol) */
            uint32_t rkey;        /* Remote memory key (Rendezvous protocol) */
            uint32_t seg_idx;     /* Ring segment index */
            uint32_t micro_idx;   /* Pipelined micro-chunk index */
            uint32_t length;      /* Payload byte length */
        } rdv;
        uint8_t raw[56];          /* Reserved / padding to 64 bytes total */
    } payload;
};

_Static_assert(sizeof(struct pg_ctrl_msg) == PG_CTRL_MSG_LEN,
               "pg_ctrl_msg must remain exactly one control header");

static inline int pg_ctrl_msg_matches(const struct pg_ctrl_msg *msg, int type,
                                      uint32_t seg_idx) {
    if (!msg || msg->type != type) return 0;
    return seg_idx == (uint32_t)-1 || msg->payload.rdv.seg_idx == seg_idx;
}

/* TCP QP Metadata exchanged during bootstrap */
struct pg_tcp_qp_info {
    uint32_t qpn;
    uint32_t psn;
    uint16_t lid;
    uint16_t reserved;
};

/* Pending control messages stored directly in a circular FIFO per QP direction. */
#define PG_PENDING_QUEUE_MAX    256

struct pg_pending_entry {
    struct pg_ctrl_msg msg;
    char *eager_buf;
    uint32_t eager_len;
};

struct pg_pending_queue {
    struct pg_pending_entry entries[PG_PENDING_QUEUE_MAX];
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
    uint32_t rdma_window;
    uint32_t rdma_signal_interval;
    uint32_t batch_size;
    size_t eager_threshold;
    uint32_t eager_window;

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

#define PG_EAGER_SLOT_SIZE      (PG_CTRL_MSG_LEN + PG_EAGER_BUF_SIZE)

    /* Pre-allocated Unified Receive Buffers and MRs (ADR-0001, ADR-0002) */
    char *recv_slot_buf[2][PG_CTRL_POOL_DEPTH];
    void *recv_slot_raw_mem[2];
    struct ibv_mr *recv_slot_mr[2];

    /* Control & Eager Send Header Buffers (Unified Pool) */
    char ctrl_send_buf[2][PG_CTRL_POOL_DEPTH][PG_CTRL_MSG_LEN];
    uint32_t ctrl_send_slot[2];
    struct ibv_mr *ctrl_send_mr[2];

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

    /* Temporary buffer for eagerly received payloads (single-threaded progress engine) */
    char eager_rx_buf[PG_EAGER_SLOT_SIZE] __attribute__((aligned(64)));

    /* TCP Bootstrap QP metadata */
    struct pg_tcp_qp_info local_to_next;           /* Local QP info for next rank */
    struct pg_tcp_qp_info local_from_prev;         /* Local QP info for prev rank */
    struct pg_tcp_qp_info remote_to_next;          /* Received QP info from next rank */
    struct pg_tcp_qp_info remote_from_prev;        /* Received QP info from prev rank */
};

static inline int pg_pending_push(struct pg_context *ctx, int qp_dir,
                                  const struct pg_ctrl_msg *msg,
                                  const void *slot_buf) {
    if (!ctx || qp_dir < 0 || qp_dir >= 2 || !msg) return PG_ERR_INVAL;
    struct pg_pending_queue *q = &ctx->pending_q[qp_dir];
    if (q->count >= PG_PENDING_QUEUE_MAX) {
        fprintf(stderr, "[pg] Error: Pending control queue overflow on qp_dir %d (count=%d)\n", qp_dir, q->count);
        return PG_ERR_RDMA;
    }

    struct pg_pending_entry *entry = &q->entries[q->tail];
    entry->eager_len = 0;
    if (msg->type == PG_CTRL_MSG_EAGER_PAYLOAD && slot_buf) {
        if (msg->payload.rdv.length > PG_EAGER_BUF_SIZE) return PG_ERR_RDMA;
        uint32_t elen = msg->payload.rdv.length + PG_CTRL_MSG_LEN;
        if (!entry->eager_buf) {
            entry->eager_buf = (char *)malloc(PG_EAGER_SLOT_SIZE);
            if (!entry->eager_buf) {
                fprintf(stderr, "[pg] Fatal: OOM allocating eager buffer in pending queue\n");
                return PG_ERR_NOMEM;
            }
        }
        memcpy(entry->eager_buf, slot_buf, elen);
        entry->eager_len = elen;
    }

    entry->msg = *msg;
    q->tail = (q->tail + 1) % PG_PENDING_QUEUE_MAX;
    q->count++;
    return PG_SUCCESS;
}

static inline int pg_pending_pop_matching(struct pg_context *ctx, int qp_dir, int type,
                                          uint32_t seg_idx,
                                          struct pg_ctrl_msg *out_msg,
                                          void *out_slot_buf,
                                          uint32_t *out_eager_len) {
    if (!ctx || qp_dir < 0 || qp_dir >= 2) return 0;
    struct pg_pending_queue *q = &ctx->pending_q[qp_dir];
    if (q->count == 0) return 0;

    struct pg_pending_entry *entry = &q->entries[q->head];
    struct pg_ctrl_msg *m = &entry->msg;
    if (!pg_ctrl_msg_matches(m, type, seg_idx)) return 0;

    if (out_msg) *out_msg = *m;
    if (out_eager_len) *out_eager_len = entry->eager_len;
    if (out_slot_buf && entry->eager_len > 0 && entry->eager_buf) {
        memcpy(out_slot_buf, entry->eager_buf, entry->eager_len);
    }
    q->head = (q->head + 1) % PG_PENDING_QUEUE_MAX;
    q->count--;
    /* Reuse the preallocated eager buffers after draining a short burst. */
    if (q->count == 0) q->head = q->tail = 0;
    return 1;
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

#define pg_recv_slot_msg(ctx, dir, slot)          ((struct pg_ctrl_msg *)ctx->recv_slot_buf[dir][slot])

/* ============================================================================
 * Ring Step Transfer Engine (Pipelined Micro-Chunk Transfer)
 * ============================================================================ */

static inline int pg_get_datatype_shift(DATATYPE datatype) {
    return (datatype == PG_DOUBLE) ? 3 : 2;
}

enum pg_chunk_action {
    PG_CHUNK_ACTION_REDUCE,
    PG_CHUNK_ACTION_MEMCPY
};

struct pg_ring_step_desc {
    uint32_t step_idx;          /* 0-indexed ring step */
    size_t total_bytes;         /* Total collective tensor size in bytes (uniform across ring) */

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

    /* Direct action chunk dispatch (Strategy 4) */
    enum pg_chunk_action chunk_action;
    void *cb_dest;              /* Destination buffer pointer for compute/copy */
    DATATYPE datatype;
    OPERATION op;
    int elem_shift;
};

/* Protocol selection helper: determines whether micro-chunk transfer uses Eager Send/Recv or Rendezvous RDMA Write */
static inline int pg_is_eager(struct pg_context *ctx, const struct pg_ring_step_desc *desc) {
#if (PG_ACTIVE_MODE == PG_MODE_TYPE_EAGER)
    (void)ctx; (void)desc;
    return 1;
#elif (PG_ACTIVE_MODE == PG_MODE_TYPE_AUTO)
    size_t max_seg_bytes = desc->total_bytes ?
        ((desc->total_bytes + ctx->size - 1) / ctx->size) :
        (desc->send_bytes > desc->recv_bytes ? desc->send_bytes : desc->recv_bytes);
    return max_seg_bytes <= ctx->eager_threshold;
#else
    (void)ctx; (void)desc;
    return 0;
#endif
}

/* Internal RDMA and TCP bootstrap helper functions */
int pg_rdma_init_resources(struct pg_context *ctx);
int pg_rdma_connect_qp(struct ibv_qp *qp, const struct pg_tcp_qp_info *remote,
                       uint32_t my_psn, enum ibv_mtu active_mtu);
int pg_tcp_bootstrap(struct pg_context *ctx);
int pg_post_ctrl_send(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *msg);
int pg_post_eager_send(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *hdr,
                       void *payload_addr, uint32_t payload_len, uint32_t lkey, int signaled, uint32_t slot);
void pg_rdma_cleanup(struct pg_context *ctx);

/* Runtime Hyperparameter Init */
void pg_init_tuning_params(struct pg_context *ctx);

/* Memory Registration Cache Helpers (ADR-0002) */
struct ibv_mr *pg_get_or_reg_mr(struct pg_context *ctx, void *addr, size_t length, int access_flags);
int pg_ensure_internal_buffers(struct pg_context *ctx, size_t count_bytes, size_t segment_bytes);

/* Vectorized Reduction Math Engine */
void pg_reduce_buffer(void *dest, const void *src, int count,
                      DATATYPE datatype, OPERATION op);

/* Ring All-Gather Generalized Core Engine (Zero-Copy RDMA Write) */
int pg_ring_all_gather_generalized(struct pg_context *ctx, void *recvbuf, int count,
                                   DATATYPE datatype);

#endif /* PG_INTERNAL_H */
