/* Exercise the production transfer/progress engines with deterministic verbs. */
#include <infiniband/verbs.h>
#include <limits.h>

static int test_post_send(struct ibv_qp *, struct ibv_send_wr *, struct ibv_send_wr **);
static int test_post_recv(struct ibv_qp *, struct ibv_recv_wr *, struct ibv_recv_wr **);
static int test_poll_cq(struct ibv_cq *, int, struct ibv_wc *);
static struct ibv_mr *test_reg_mr(struct ibv_pd *, void *, size_t, int);

#define ibv_post_send test_post_send
#define ibv_post_recv test_post_recv
#define ibv_poll_cq test_poll_cq
#undef ibv_reg_mr
#define ibv_reg_mr test_reg_mr
#include "../pg.c"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

struct completion {
    int type, dir, msg_type;
    uint32_t micro, length, bytes;
};

static struct pg_context test_ctx;
static struct ibv_qp test_qp[2];
static struct ibv_mr test_mr;
static unsigned char test_rx[PG_EAGER_SLOT_SIZE] __attribute__((aligned(64)));
static const struct completion *script;
static size_t script_count, script_pos;
static int queue_future_rts, cts_posts;
static int queue_controls, done_posts;
static uint32_t done_micros[32];
static int eager_posts;
static int repost_error;
static int reposts;
static int registrations;
static int posted_wrs;
static const struct pg_ctrl_msg *first_eager_header;

static void clear_test_pending(void) {
    for (int dir = 0; dir < 2; dir++) {
        for (int i = 0; i < PG_PENDING_QUEUE_MAX; i++) {
            free(test_ctx.pending_q[dir].entries[i].eager_buf);
            test_ctx.pending_q[dir].entries[i].eager_buf = NULL;
        }
    }
}

static int test_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                          struct ibv_send_wr **bad_wr) {
    (void)bad_wr;
    for (; wr; wr = wr->next) {
        posted_wrs++;
        if (wr->opcode == IBV_WR_SEND) {
            const struct pg_ctrl_msg *msg = (const void *)(uintptr_t)wr->sg_list[0].addr;
            if (pg_wr_type(wr->wr_id) == PG_WR_TYPE_EAGER_SEND) {
                if (eager_posts == 0) first_eager_header = msg;
                eager_posts++;
            }
            if (qp == &test_qp[PG_QP_DIR_FROM_PREV] && msg->type == PG_CTRL_MSG_CTS) {
                cts_posts++;
            }
            if (msg->type == PG_CTRL_MSG_DATA_DONE) {
                CHECK(done_posts < (int)(sizeof(done_micros) / sizeof(done_micros[0])));
                done_micros[done_posts++] = msg->payload.rdv.micro_idx;
            }
        }
    }
    return 0;
}

static int test_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
                          struct ibv_recv_wr **bad_wr) {
    (void)qp; (void)wr; (void)bad_wr;
    reposts++;
    return repost_error;
}

static struct ibv_mr *test_reg_mr(struct ibv_pd *pd, void *addr, size_t length, int flags) {
    (void)pd; (void)addr; (void)length; (void)flags;
    registrations++;
    errno = ENOMEM;
    return NULL;
}

static int test_poll_cq(struct ibv_cq *cq, int count, struct ibv_wc *wc) {
    (void)cq; (void)count;
    if (script_pos >= script_count) return -1;
    const struct completion *event = &script[script_pos++];
    struct pg_ctrl_msg *msg = (void *)test_rx;
    memset(msg, 0, sizeof(*msg));
    msg->tag = PG_CTRL_TAG;
    msg->type = (uint16_t)event->msg_type;
    msg->payload.rdv.seg_idx = 0;
    msg->payload.rdv.micro_idx = event->micro;
    msg->payload.rdv.length = event->length;
    msg->payload.rdv.remote_addr = 1;
    if (event->msg_type == PG_CTRL_MSG_EAGER_PAYLOAD &&
        event->bytes >= PG_CTRL_MSG_LEN + sizeof(int)) {
        int payload = 2;
        memcpy(test_rx + PG_CTRL_MSG_LEN, &payload, sizeof(payload));
    }
    if ((queue_controls && event->type == PG_WR_TYPE_RECV_CTRL) ||
        (queue_future_rts && event->msg_type == PG_CTRL_MSG_RTS && cts_posts > 0)) {
        CHECK(pg_pending_push(&test_ctx, event->dir, msg, NULL) == PG_SUCCESS);
        return 0;
    }
    memset(wc, 0, sizeof(*wc));
    wc->wr_id = pg_make_wr_slot(event->dir, event->type,
                               event->type == PG_WR_TYPE_RDMA_WRITE ? event->micro : 0);
    wc->status = IBV_WC_SUCCESS;
    wc->byte_len = event->bytes;
    return 1;
}

static void reset_test(const struct completion *events, size_t count) {
    clear_test_pending();
    memset(&test_ctx, 0, sizeof(test_ctx));
    memset(test_rx, 0, sizeof(test_rx));
    test_ctx.size = 2;
    pg_init_tuning_params(&test_ctx);
    test_ctx.qp_to_next = &test_qp[0];
    test_ctx.qp_from_prev = &test_qp[1];
    for (int dir = 0; dir < 2; dir++) {
        test_ctx.max_inline_data[dir] = PG_CTRL_MSG_LEN;
        test_ctx.ctrl_send_mr[dir] = &test_mr;
        test_ctx.recv_slot_mr[dir] = &test_mr;
        test_ctx.recv_slot_buf[dir][0] = (char *)test_rx;
    }
    script = events;
    script_count = count;
    script_pos = 0;
    queue_future_rts = cts_posts = 0;
    queue_controls = done_posts = 0;
    eager_posts = 0;
    first_eager_header = NULL;
    repost_error = 0;
    reposts = 0;
    registrations = 0;
    posted_wrs = 0;
}

static void test_future_rts(void) {
    const struct completion after_cts[] = {
        {PG_WR_TYPE_SEND_CTRL, 0, 0, 0, 0, 0},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_RTS, 0, 4, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_RECV_CTRL, 0, PG_CTRL_MSG_CTS, 0, 4, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_RDMA_WRITE, 0, 0, 0, 0, 0},
        {PG_WR_TYPE_SEND_CTRL, 1, 0, 0, 0, 0},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_DATA_DONE, 1, 4, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_RTS, 0, 4, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_SEND_CTRL, 0, 0, 0, 0, 0}
    };
    struct completion before_cts[sizeof(after_cts) / sizeof(after_cts[0])];
    memcpy(before_cts, after_cts, sizeof(before_cts));
    before_cts[4] = after_cts[5];
    before_cts[5] = after_cts[6];
    before_cts[6] = after_cts[4];

    for (int pending = 0; pending < 2; pending++) {
        for (int late_cts = 0; late_cts < 2; late_cts++) {
            reset_test(late_cts ? before_cts : after_cts,
                       sizeof(after_cts) / sizeof(after_cts[0]));
            queue_future_rts = pending;
            int outbound = 11, inbound = 22;
            struct pg_ring_step_desc desc = {
                .send_buf = &outbound, .send_bytes = 4,
                .recv_target_addr = &inbound, .recv_bytes = 4,
                .chunk_action = PG_CHUNK_ACTION_MEMCPY, .cb_dest = &inbound
            };
            CHECK(pg_ring_step_transfer_rdv(&test_ctx, &desc) == PG_SUCCESS);
            CHECK(cts_posts == 1);
            CHECK(test_ctx.pending_q[PG_QP_DIR_FROM_PREV].count == 1);
            struct pg_progress_event future;
            CHECK(pg_progress_pop_pending(&test_ctx, PG_QP_DIR_FROM_PREV,
                                          PG_CTRL_MSG_RTS, 0, &future));
            CHECK(future.msg.payload.rdv.length == 4);
        }
    }
    puts("Transfer: future RTS stays pending, including delayed CTS completion.");
}

static void test_chunk_tuning(void) {
    const char *invalid[] = {"1", "5", "12", "0", "-8", "16x", "4294967296"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        CHECK(setenv("PG_PIPELINE_CHUNK", invalid[i], 1) == 0);
        reset_test(NULL, 0);
        CHECK(test_ctx.pipeline_chunk == PG_PIPELINE_CHUNK);
    }
    CHECK(setenv("PG_PIPELINE_CHUNK", "8", 1) == 0);
    reset_test(NULL, 0);
    CHECK(test_ctx.pipeline_chunk == 8);
    int dest[] = {1, 3}, src[] = {2, 4};
    struct pg_ring_step_desc desc = {
        .chunk_action = PG_CHUNK_ACTION_REDUCE, .datatype = PG_INT,
        .op = PG_SUM, .elem_shift = 2
    };
    pg_step_process_chunk(&desc, dest, src, test_ctx.pipeline_chunk);
    CHECK(dest[0] == 3 && dest[1] == 7);
    CHECK(unsetenv("PG_PIPELINE_CHUNK") == 0);
    puts("Transfer: chunk tuning preserves whole elements and rejects invalid sizes.");
}

static void test_future_eager(void) {
    const struct completion events[] = {
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_EAGER_PAYLOAD, 0, 4, PG_CTRL_MSG_LEN + 4},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_EAGER_PAYLOAD, 0, 4, PG_CTRL_MSG_LEN + 4},
        {PG_WR_TYPE_EAGER_SEND, 0, 0, 0, 0, 0},
        {PG_WR_TYPE_EAGER_SEND, 0, 0, 0, 0, 0}
    };
    reset_test(events, sizeof(events) / sizeof(events[0]));
    int outbound = 11, inbound = 0;
    struct pg_ring_step_desc desc = {
        .send_buf = &outbound, .send_bytes = sizeof(outbound),
        .recv_bytes = sizeof(inbound), .cb_dest = &inbound,
        .chunk_action = PG_CHUNK_ACTION_MEMCPY
    };
    CHECK(pg_ring_step_transfer_eager(&test_ctx, &desc) == PG_SUCCESS);
    CHECK(inbound == 2);
    CHECK(test_ctx.pending_q[PG_QP_DIR_FROM_PREV].count == 1);
    /* The next transfer must consume its early payload before polling its send completion. */
    inbound = 0;
    CHECK(pg_ring_step_transfer_eager(&test_ctx, &desc) == PG_SUCCESS);
    CHECK(inbound == 2);
    CHECK(test_ctx.pending_q[PG_QP_DIR_FROM_PREV].count == 0);
    CHECK(script_pos == script_count);
    CHECK(reposts == 2);
    puts("Transfer: next-call eager payload stays pending and is consumed on entry.");
}

static void test_eager_window(void) {
    const char *oversized[] = {"33", "4294967296"};
    for (size_t i = 0; i < sizeof(oversized) / sizeof(oversized[0]); i++) {
        CHECK(setenv("PG_EAGER_WINDOW", oversized[i], 1) == 0);
        reset_test(NULL, 0);
        CHECK(test_ctx.eager_window == PG_CTRL_POOL_DEPTH);
        test_ctx.pipeline_chunk = 8;
        int payload[66] = {0};
        struct pg_ring_step_desc desc = {
            .send_buf = payload, .send_bytes = sizeof(payload)
        };
        /* Stop at the first poll; inspect all headers still owned by the NIC. */
        CHECK(pg_ring_step_transfer_eager(&test_ctx, &desc) == PG_ERR_RDMA);
        CHECK(eager_posts == PG_CTRL_POOL_DEPTH);
        CHECK(first_eager_header->payload.rdv.micro_idx == 0);
    }
    CHECK(setenv("PG_EAGER_WINDOW", "1", 1) == 0);
    reset_test(NULL, 0);
    CHECK(test_ctx.eager_window == 1);
    const char *invalid[] = {"0", "-1", "8x", "18446744073709551616"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        CHECK(setenv("PG_EAGER_WINDOW", invalid[i], 1) == 0);
        reset_test(NULL, 0);
        CHECK(test_ctx.eager_window == PG_EAGER_WINDOW);
    }
    CHECK(unsetenv("PG_EAGER_WINDOW") == 0);
    puts("Transfer: eager window cannot overwrite an outstanding send header.");
}

static void test_repost_error(void) {
    const struct completion event = {
        PG_WR_TYPE_RECV_CTRL, PG_QP_DIR_FROM_PREV,
        PG_CTRL_MSG_EAGER_PAYLOAD, 0, sizeof(int), PG_CTRL_MSG_LEN + sizeof(int)
    };
    reset_test(&event, 1);
    repost_error = EIO;
    int dest = 1;
    struct pg_ring_step_desc desc = {
        .recv_bytes = sizeof(dest), .cb_dest = &dest,
        .chunk_action = PG_CHUNK_ACTION_REDUCE, .datatype = PG_INT,
        .op = PG_SUM, .elem_shift = 2
    };
    CHECK(pg_ring_step_transfer_eager(&test_ctx, &desc) == PG_ERR_RDMA);
    CHECK(dest == 3);
    puts("Transfer: consumed eager payload reports receive repost failure.");
}

static void test_receive_lengths(void) {
    const struct completion malformed[] = {
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_EAGER_PAYLOAD, 0, 4, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_EAGER_PAYLOAD, 0, 4, PG_CTRL_MSG_LEN + 2},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_EAGER_PAYLOAD, 0, 4, PG_CTRL_MSG_LEN + 8},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_DATA_DONE, 0, 4, PG_CTRL_MSG_LEN - 1}
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        reset_test(&malformed[i], 1);
        struct pg_progress_event event;
        CHECK(pg_progress_poll(&test_ctx, &event) == PG_ERR_RDMA);
        CHECK(reposts == 0);
    }
    const struct completion valid = {
        PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_EAGER_PAYLOAD, 0, 4, PG_CTRL_MSG_LEN + 4
    };
    reset_test(&valid, 1);
    int dest = 0;
    struct pg_ring_step_desc desc = {
        .recv_bytes = sizeof(dest), .cb_dest = &dest,
        .chunk_action = PG_CHUNK_ACTION_MEMCPY
    };
    CHECK(pg_ring_step_transfer_eager(&test_ctx, &desc) == PG_SUCCESS);
    CHECK(dest == 2 && reposts == 1);
    puts("Transfer: actual completion length protects receive-slot payload ownership.");
}

static void test_gather_count_limit(void) {
    reset_test(NULL, 0);
    test_ctx.size = 3;
    int buffer = 17;
    /* Exact-local-slice input avoids a giant copy even on the broken code. */
    CHECK(pg_all_gather(&buffer, &buffer, INT_MAX, PG_INT, &test_ctx) == PG_ERR_INVAL);
    CHECK(registrations == 0 && buffer == 17);
    test_ctx.size = 2;
    CHECK(pg_all_gather(&buffer, &buffer, INT_MAX / 2 + 1, PG_INT, &test_ctx) == PG_ERR_INVAL);
    CHECK(registrations == 0);
    puts("Transfer: All-Gather rejects aggregate element-count overflow before side effects.");
}

static void test_segment_limit(void) {
    const size_t oversized = (size_t)UINT32_MAX + 1;
    for (int receive = 0; receive < 2; receive++) {
        reset_test(NULL, 0);
        struct pg_ring_step_desc desc = {0};
        if (receive) desc.recv_bytes = oversized;
        else desc.send_bytes = oversized;
        CHECK(pg_ring_step_transfer_rdv(&test_ctx, &desc) == PG_ERR_INVAL);
        CHECK(posted_wrs == 0 && script_pos == 0);
    }
    reset_test(NULL, 0);
    double buffer = 17;
    CHECK(pg_all_gather(&buffer, &buffer, (int)(oversized / sizeof(double)),
                        PG_DOUBLE, &test_ctx) == PG_ERR_INVAL);
    CHECK(registrations == 0 && buffer == 17);
    int tensor_count = (int)(oversized / sizeof(double) * test_ctx.size);
    CHECK(pg_reduce_scatter_impl(&buffer, &buffer, tensor_count, PG_DOUBLE,
                                 PG_SUM, &test_ctx) == PG_ERR_INVAL);
    CHECK(pg_ring_all_gather_generalized(&test_ctx, &buffer, tensor_count,
                                         PG_DOUBLE) == PG_ERR_INVAL);
    CHECK(registrations == 0 && test_ctx.staging_capacity == 0);
    puts("Transfer: oversized segments are rejected before handshake or registration.");
}

static void test_rdv_micro_progress(void) {
    const struct completion events[] = {
        {PG_WR_TYPE_SEND_CTRL, 0, 0, 0, 0, 0},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_RTS, 0, 12, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_RECV_CTRL, 0, PG_CTRL_MSG_CTS, 0, 12, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_RDMA_WRITE, 0, 0, 0, 0, 0},
        {PG_WR_TYPE_SEND_CTRL, 1, 0, 0, 0, 0},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_DATA_DONE, 1, 12, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_RDMA_WRITE, 0, 0, 0, 0, 0},
        {PG_WR_TYPE_RDMA_WRITE, 0, 0, 1, 0, 0},
        {PG_WR_TYPE_SEND_CTRL, 0, 0, 0, 0, 0},
        {PG_WR_TYPE_RECV_CTRL, 1, PG_CTRL_MSG_DATA_DONE, 2, 12, PG_CTRL_MSG_LEN},
        {PG_WR_TYPE_SEND_CTRL, 0, 0, 0, 0, 0}
    };
    for (int pending = 0; pending < 2; pending++) {
        reset_test(events, sizeof(events) / sizeof(events[0]));
        queue_controls = pending;
        test_ctx.pipeline_chunk = 8;
        test_ctx.rdma_signal_interval = 1;
        CHECK(setenv("PG_PIPELINE_CHUNK", "8", 1) == 0);
        int staging[] = {10, 20, 30}, dest[] = {1, 2, 3};
        struct pg_ring_step_desc desc = {
            .send_buf = dest, .send_bytes = sizeof(dest),
            .recv_target_addr = staging, .recv_bytes = sizeof(staging),
            .chunk_action = PG_CHUNK_ACTION_REDUCE, .cb_dest = dest,
            .datatype = PG_INT, .op = PG_SUM, .elem_shift = 2
        };
        CHECK(pg_ring_step_transfer_rdv(&test_ctx, &desc) == PG_SUCCESS);
        CHECK(dest[0] == 11 && dest[1] == 22 && dest[2] == 33);
        CHECK(cts_posts == 1 && done_posts == 2);
        CHECK(done_micros[0] == 1 && done_micros[1] == 2);
        CHECK(test_ctx.pending_q[0].count == 0 && test_ctx.pending_q[1].count == 0);
        CHECK(unsetenv("PG_PIPELINE_CHUNK") == 0);
    }
    puts("Transfer: pending/live rendezvous paths reduce partial tails and notify each completion once.");
}

int main(void) {
    test_future_rts();
    test_future_eager();
    test_chunk_tuning();
    test_eager_window();
    test_repost_error();
    test_receive_lengths();
    test_gather_count_limit();
    test_segment_limit();
    test_rdv_micro_progress();
    clear_test_pending();
    return 0;
}
