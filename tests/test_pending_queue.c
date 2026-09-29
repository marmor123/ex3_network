/* Exercise the production pending queue without an RDMA device. */
#include <assert.h>
#include <stdlib.h>
#include "../pg_internal.h"

struct packet {
    struct pg_ctrl_msg header;
    unsigned char payload[32];
};

static struct packet packet_for(uint32_t id) {
    struct packet p = {0};
    p.header.tag = PG_CTRL_TAG;
    p.header.type = id % 3 == 0 ? PG_CTRL_MSG_EAGER_PAYLOAD : PG_CTRL_MSG_RTS;
    p.header.payload.rdv.seg_idx = id % 4; /* Repeated tags must still obey FIFO. */
    p.header.payload.rdv.micro_idx = id;
    p.header.payload.rdv.length = sizeof(p.payload);
    memset(p.payload, (unsigned char)id, sizeof(p.payload));
    return p;
}

static void push(struct pg_context *ctx, int dir, uint32_t id) {
    struct packet p = packet_for(id);
    assert(pg_pending_push(ctx, dir, &p.header, &p) == PG_SUCCESS);
    /* The queued eager payload must survive reuse of its receive-slot buffer. */
    memset(&p, 0xff, sizeof(p));
}

static void pop(struct pg_context *ctx, int dir, uint32_t id) {
    struct packet expected = packet_for(id), received = {0};
    struct pg_ctrl_msg msg = {0};
    uint32_t len = UINT32_MAX;
    assert(pg_pending_pop_matching(ctx, dir, expected.header.type,
                                  expected.header.payload.rdv.seg_idx,
                                  &msg, &received, &len) == 1);
    assert(memcmp(&msg, &expected.header, sizeof(msg)) == 0);
    if (msg.type == PG_CTRL_MSG_EAGER_PAYLOAD) {
        assert(len == sizeof(received));
        assert(memcmp(&received, &expected, sizeof(received)) == 0);
    } else {
        assert(len == 0); /* A reused control entry must not expose old eager data. */
    }
}

int main(void) {
    /* pg_context includes aligned storage, so keep its declared alignment. */
    static struct pg_context ctx;
    struct pg_ctrl_msg out = {0};
    assert(!pg_pending_pop_matching(&ctx, 0, PG_CTRL_MSG_RTS, 0, &out, NULL, NULL));

    push(&ctx, 0, 0);
    push(&ctx, 0, 1);
    assert(!pg_pending_pop_matching(&ctx, 0, PG_CTRL_MSG_RTS, 1, &out, NULL, NULL));
    assert(!pg_pending_pop_matching(&ctx, 0, PG_CTRL_MSG_EAGER_PAYLOAD, 1,
                                   &out, NULL, NULL));
    pop(&ctx, 0, 0);
    pop(&ctx, 0, 1);

    /* Short bursts reuse the preallocated portion instead of warming 256 buffers. */
    char *first_buffer = ctx.pending_q[0].entries[0].eager_buf;
    for (uint32_t i = 0; i < PG_PENDING_QUEUE_MAX * 2; i++) {
        push(&ctx, 0, i);
        pop(&ctx, 0, i);
        assert(ctx.pending_q[0].entries[0].eager_buf == first_buffer);
    }
    for (int i = 1; i < PG_PENDING_QUEUE_MAX; i++) {
        assert(ctx.pending_q[0].entries[i].eager_buf == NULL);
    }

    /* Keep one QP queued while the other wraps repeatedly without draining. */
    push(&ctx, 1, 900);
    for (uint32_t i = 0; i < PG_PENDING_QUEUE_MAX; i++) push(&ctx, 0, i);
    struct packet extra = packet_for(999);
    assert(pg_pending_push(&ctx, 0, &extra.header, &extra) == PG_ERR_RDMA);

    uint32_t next_pop = 0, next_push = PG_PENDING_QUEUE_MAX;
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < PG_PENDING_QUEUE_MAX / 2; i++) pop(&ctx, 0, next_pop++);
        for (int i = 0; i < PG_PENDING_QUEUE_MAX / 2; i++) push(&ctx, 0, next_push++);
    }
    while (next_pop < next_push) pop(&ctx, 0, next_pop++);
    pop(&ctx, 1, 900);
    assert(!pg_pending_pop_matching(&ctx, 0, PG_CTRL_MSG_RTS, 0, &out, NULL, NULL));

    /* Rejected payloads must not consume a position or displace queued traffic. */
    push(&ctx, 0, 0);
    struct packet invalid = packet_for(3);
    invalid.header.payload.rdv.length = PG_EAGER_BUF_SIZE + 1;
    assert(pg_pending_push(&ctx, 0, &invalid.header, &invalid) == PG_ERR_RDMA);
    invalid.header.payload.rdv.length = UINT32_MAX;
    assert(pg_pending_push(&ctx, 0, &invalid.header, &invalid) == PG_ERR_RDMA);
    push(&ctx, 0, 1);
    pop(&ctx, 0, 0);
    pop(&ctx, 0, 1);
    assert(!pg_pending_pop_matching(&ctx, 0, PG_CTRL_MSG_RTS, 0, &out, NULL, NULL));

    for (int dir = 0; dir < 2; dir++) {
        for (int i = 0; i < PG_PENDING_QUEUE_MAX; i++) {
            free(ctx.pending_q[dir].entries[i].eager_buf);
        }
    }
    puts("Pending queue: FIFO, wraparound, overflow, payload ownership, and reuse passed.");
    return 0;
}
