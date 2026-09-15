#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define PG_PENDING_QUEUE_MAX 256
#define PG_PENDING_EAGER_POOL_SIZE 8
#define PG_EAGER_SLOT_SIZE 262144 /* 256 KiB */
#define PG_CTRL_MSG_LEN 48

#define PG_CTRL_MSG_EAGER_PAYLOAD 1
#define PG_CTRL_MSG_RTS           2
#define PG_CTRL_MSG_CTS           3
#define PG_CTRL_MSG_BARRIER       4

struct pg_ctrl_msg {
    uint32_t type;
    uint32_t tag;
    struct {
        struct {
            uint32_t length;
        } rdv;
    } payload;
};

struct pg_pending_entry {
    int in_use;
    struct pg_ctrl_msg msg;
    void *eager_buf;
    uint32_t eager_len;
    int eager_buf_idx; /* -1 if none, 0..7 if allocated from pool */
};

struct pg_pending_queue {
    struct pg_pending_entry pool[PG_PENDING_QUEUE_MAX];
    int head;
    int tail;
    int count;
};

struct pg_context {
    uint8_t eager_buf_mask[2]; /* 1 bit per pool slot (bits 0..7) */
    void *eager_pending_buf[2][PG_PENDING_EAGER_POOL_SIZE];
    struct pg_pending_queue pending_q[2];
};

/* Strategy 3 implementation from docs/refactoring_roadmap.md:704-750 */
static inline void pg_pending_push(struct pg_context *ctx, int qp_dir,
                                   const struct pg_ctrl_msg *msg, const void *slot_buf) {
    if (__builtin_expect(!ctx || qp_dir < 0 || qp_dir >= 2 || !msg, 0)) return;
    struct pg_pending_queue *q = &ctx->pending_q[qp_dir];
    if (__builtin_expect(q->count >= PG_PENDING_QUEUE_MAX, 0)) return;

    int slot = q->tail;
    q->pool[slot].in_use = 1;
    q->pool[slot].msg = *msg;
    q->pool[slot].eager_len = 0;
    q->pool[slot].eager_buf = NULL;
    q->pool[slot].eager_buf_idx = -1;

    if (msg->type == PG_CTRL_MSG_EAGER_PAYLOAD && slot_buf) {
        uint32_t elen = msg->payload.rdv.length + PG_CTRL_MSG_LEN;
        if (elen > PG_EAGER_SLOT_SIZE) elen = PG_EAGER_SLOT_SIZE;

        /* Explicit bitmask allocation: single-cycle ctz avoids buffer aliasing */
        uint8_t free_mask = (uint8_t)(~ctx->eager_buf_mask[qp_dir] & 0xFF);
        if (__builtin_expect(free_mask != 0, 1)) {
            int buf_idx = __builtin_ctz(free_mask);
            ctx->eager_buf_mask[qp_dir] |= (1U << buf_idx);
            q->pool[slot].eager_buf_idx = buf_idx;
            q->pool[slot].eager_buf = ctx->eager_pending_buf[qp_dir][buf_idx];
            memcpy(q->pool[slot].eager_buf, slot_buf, elen);
            q->pool[slot].eager_len = elen;
        } else {
            fprintf(stderr, "[pg_pending] Fatal: Eager pool overflow on qp_dir %d\n", qp_dir);
            return;
        }
    }

    q->tail = (q->tail + 1) % PG_PENDING_QUEUE_MAX;
    q->count++;
}

static inline void pg_pending_free_entry(struct pg_context *ctx, int qp_dir,
                                         struct pg_pending_entry *entry) {
    if (entry->eager_buf_idx >= 0) {
        /* Free buffer back to pool: clear bitmask in 1 CPU cycle */
        ctx->eager_buf_mask[qp_dir] &= ~(1U << entry->eager_buf_idx);
        entry->eager_buf_idx = -1;
        entry->eager_buf = NULL;
    }
    entry->in_use = 0;
}

/* Naive buggy implementation for comparison */
static inline void pg_pending_push_naive(struct pg_context *ctx, int qp_dir,
                                         const struct pg_ctrl_msg *msg, const void *slot_buf) {
    struct pg_pending_queue *q = &ctx->pending_q[qp_dir];
    int slot = q->tail;
    q->pool[slot].in_use = 1;
    q->pool[slot].msg = *msg;
    q->pool[slot].eager_len = 0;
    q->pool[slot].eager_buf = NULL;

    if (msg->type == PG_CTRL_MSG_EAGER_PAYLOAD && slot_buf) {
        uint32_t elen = msg->payload.rdv.length + PG_CTRL_MSG_LEN;
        if (elen > PG_EAGER_SLOT_SIZE) elen = PG_EAGER_SLOT_SIZE;
        int buf_idx = slot % PG_PENDING_EAGER_POOL_SIZE;
        q->pool[slot].eager_buf = ctx->eager_pending_buf[qp_dir][buf_idx];
        memcpy(q->pool[slot].eager_buf, slot_buf, elen);
        q->pool[slot].eager_len = elen;
    }
    q->tail = (q->tail + 1) % PG_PENDING_QUEUE_MAX;
    q->count++;
}

int main(void) {
    printf("=== STRATEGY 3 EMPIRICAL VERIFICATION HARNESS ===\n");

    /* Allocate dummy eager buffers for ctx */
    struct pg_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    for (int dir = 0; dir < 2; dir++) {
        ctx.eager_buf_mask[dir] = 0;
        for (int s = 0; s < PG_PENDING_EAGER_POOL_SIZE; s++) {
            ctx.eager_pending_buf[dir][s] = malloc(PG_EAGER_SLOT_SIZE);
            assert(ctx.eager_pending_buf[dir][s] != NULL);
            memset(ctx.eager_pending_buf[dir][s], 0, 1024);
        }
    }

    /* PART 1: Reproduce naive modulo bug */
    printf("\n[Test 1.1] Reproducing Naive Modulo Aliasing Defect...\n");
    {
        struct pg_context naive_ctx;
        memset(&naive_ctx, 0, sizeof(naive_ctx));
        for (int s = 0; s < PG_PENDING_EAGER_POOL_SIZE; s++) {
            naive_ctx.eager_pending_buf[0][s] = ctx.eager_pending_buf[0][s];
        }

        struct pg_ctrl_msg eager_msg1 = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 101, .payload.rdv.length = 16 };
        const char *payload1 = "STEP_1_TENSOR_DATA";
        pg_pending_push_naive(&naive_ctx, 0, &eager_msg1, payload1);

        /* Interleave 7 control messages */
        for (int i = 0; i < 7; i++) {
            struct pg_ctrl_msg ctrl_msg = { .type = PG_CTRL_MSG_RTS, .tag = 200 + i };
            pg_pending_push_naive(&naive_ctx, 0, &ctrl_msg, NULL);
        }

        /* Push Eager message 2 -> slot 8 -> 8 % 8 = 0 -> ALIASES SLOT 0! */
        struct pg_ctrl_msg eager_msg2 = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 102, .payload.rdv.length = 16 };
        const char *payload2 = "STEP_2_CORRUPTS_DATA";
        pg_pending_push_naive(&naive_ctx, 0, &eager_msg2, payload2);

        char *read_payload1 = (char *)naive_ctx.pending_q[0].pool[0].eager_buf;
        printf("  Naive Modulo: Slot 0 expected '%s', actual '%s'\n", payload1, read_payload1);
        if (strcmp(read_payload1, payload1) != 0) {
            printf("  -> CONFIRMED: Naive modulo indexing caused SILENT DATA CORRUPTION!\n");
        } else {
            printf("  -> UNEXPECTED: Data corruption not observed.\n");
            assert(0);
        }
    }

    /* PART 2: Verify Bitmask Allocator Prevents Aliasing */
    printf("\n[Test 1.2] Verifying Bitmask Allocator Eliminates Aliasing...\n");
    {
        /* Clear ctx state */
        ctx.eager_buf_mask[0] = 0;
        ctx.pending_q[0].head = ctx.pending_q[0].tail = ctx.pending_q[0].count = 0;

        struct pg_ctrl_msg eager_msg1 = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 101, .payload.rdv.length = 16 };
        const char *payload1 = "STEP_1_TENSOR_DATA";
        pg_pending_push(&ctx, 0, &eager_msg1, payload1);
        int slot1 = 0;
        int buf1 = ctx.pending_q[0].pool[slot1].eager_buf_idx;
        printf("  Pushed Eager 1: slot=%d, buf_idx=%d, mask=0x%02X\n", slot1, buf1, ctx.eager_buf_mask[0]);
        assert(buf1 == 0);
        assert(ctx.eager_buf_mask[0] == 0x01);

        /* Interleave 7 control messages */
        for (int i = 0; i < 7; i++) {
            struct pg_ctrl_msg ctrl_msg = { .type = PG_CTRL_MSG_RTS, .tag = 200 + i };
            pg_pending_push(&ctx, 0, &ctrl_msg, NULL);
        }
        assert(ctx.pending_q[0].tail == 8);

        /* Push Eager message 2 -> slot 8 -> bitmask finds next free bit (bit 1)! */
        struct pg_ctrl_msg eager_msg2 = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 102, .payload.rdv.length = 16 };
        const char *payload2 = "STEP_2_TENSOR_DATA";
        pg_pending_push(&ctx, 0, &eager_msg2, payload2);
        int slot2 = 8;
        int buf2 = ctx.pending_q[0].pool[slot2].eager_buf_idx;
        printf("  Pushed Eager 2: slot=%d, buf_idx=%d, mask=0x%02X\n", slot2, buf2, ctx.eager_buf_mask[0]);
        assert(buf2 == 1);
        assert(ctx.eager_buf_mask[0] == 0x03);

        /* Verify both payloads intact and distinct */
        assert(strcmp((char *)ctx.pending_q[0].pool[slot1].eager_buf, payload1) == 0);
        assert(strcmp((char *)ctx.pending_q[0].pool[slot2].eager_buf, payload2) == 0);
        printf("  -> PASS: Payloads are completely isolated in separate buffers! buf0 != buf1\n");

        /* Pop Eager 1 */
        pg_pending_free_entry(&ctx, 0, &ctx.pending_q[0].pool[slot1]);
        printf("  Popped Eager 1: mask=0x%02X (bit 0 cleared)\n", ctx.eager_buf_mask[0]);
        assert(ctx.eager_buf_mask[0] == 0x02);
        assert(ctx.pending_q[0].pool[slot1].eager_buf_idx == -1);

        /* Pop Eager 2 */
        pg_pending_free_entry(&ctx, 0, &ctx.pending_q[0].pool[slot2]);
        printf("  Popped Eager 2: mask=0x%02X (bit 1 cleared)\n", ctx.eager_buf_mask[0]);
        assert(ctx.eager_buf_mask[0] == 0x00);
        assert(ctx.pending_q[0].pool[slot2].eager_buf_idx == -1);
        printf("  -> PASS: Release on pop correctly returns bits to mask!\n");
    }

    /* PART 3: Stress Test Exhaustive Pool Occupancy and Out-of-order Release */
    printf("\n[Test 1.3] Exhaustive 8-Slot Occupancy & Out-of-Order Lifecycle...\n");
    {
        ctx.eager_buf_mask[0] = 0;
        ctx.pending_q[0].head = ctx.pending_q[0].tail = ctx.pending_q[0].count = 0;

        char payloads[8][32];
        int slots[8];

        /* Fill all 8 eager slots */
        for (int i = 0; i < 8; i++) {
            snprintf(payloads[i], sizeof(payloads[i]), "EAGER_PAYLOAD_%d", i);
            struct pg_ctrl_msg msg = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 1000 + i, .payload.rdv.length = 32 };
            slots[i] = ctx.pending_q[0].tail;
            pg_pending_push(&ctx, 0, &msg, payloads[i]);
            assert(ctx.pending_q[0].pool[slots[i]].eager_buf_idx == i);
        }
        assert(ctx.eager_buf_mask[0] == 0xFF);
        printf("  All 8 eager slots filled: mask=0x%02X\n", ctx.eager_buf_mask[0]);

        /* Test overflow rejection: 9th push must not corrupt anything */
        struct pg_ctrl_msg ovf_msg = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 9999, .payload.rdv.length = 32 };
        pg_pending_push(&ctx, 0, &ovf_msg, "OVERFLOW_PAYLOAD");
        assert(ctx.eager_buf_mask[0] == 0xFF);
        printf("  9th push safely rejected on overflow: mask preserved at 0x%02X\n", ctx.eager_buf_mask[0]);

        /* Free slots out of order: free 3, then 1, then 7 */
        pg_pending_free_entry(&ctx, 0, &ctx.pending_q[0].pool[slots[3]]);
        assert(ctx.eager_buf_mask[0] == (0xFF & ~(1U << 3)));
        pg_pending_free_entry(&ctx, 0, &ctx.pending_q[0].pool[slots[1]]);
        assert(ctx.eager_buf_mask[0] == (0xFF & ~((1U << 3) | (1U << 1))));
        pg_pending_free_entry(&ctx, 0, &ctx.pending_q[0].pool[slots[7]]);
        assert(ctx.eager_buf_mask[0] == (0xFF & ~((1U << 3) | (1U << 1) | (1U << 7))));
        printf("  Freed slots 3, 1, 7 out of order: mask=0x%02X\n", ctx.eager_buf_mask[0]);

        /* Now push new eager messages: ctz must assign slot 1 first, then 3, then 7 */
        struct pg_ctrl_msg new_msg1 = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 2001, .payload.rdv.length = 32 };
        int s_new1 = ctx.pending_q[0].tail;
        pg_pending_push(&ctx, 0, &new_msg1, "NEW_PAYLOAD_A");
        assert(ctx.pending_q[0].pool[s_new1].eager_buf_idx == 1);

        struct pg_ctrl_msg new_msg2 = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 2002, .payload.rdv.length = 32 };
        int s_new2 = ctx.pending_q[0].tail;
        pg_pending_push(&ctx, 0, &new_msg2, "NEW_PAYLOAD_B");
        assert(ctx.pending_q[0].pool[s_new2].eager_buf_idx == 3);

        struct pg_ctrl_msg new_msg3 = { .type = PG_CTRL_MSG_EAGER_PAYLOAD, .tag = 2003, .payload.rdv.length = 32 };
        int s_new3 = ctx.pending_q[0].tail;
        pg_pending_push(&ctx, 0, &new_msg3, "NEW_PAYLOAD_C");
        assert(ctx.pending_q[0].pool[s_new3].eager_buf_idx == 7);

        assert(ctx.eager_buf_mask[0] == 0xFF);
        printf("  Re-allocated free slots: assigned 1, 3, 7 correctly! mask=0x%02X\n", ctx.eager_buf_mask[0]);

        /* Verify remaining original slots (0, 2, 4, 5, 6) are still pristine */
        for (int i = 0; i < 8; i++) {
            if (i == 1 || i == 3 || i == 7) continue;
            assert(strcmp((char *)ctx.pending_q[0].pool[slots[i]].eager_buf, payloads[i]) == 0);
        }
        assert(strcmp((char *)ctx.pending_q[0].pool[s_new1].eager_buf, "NEW_PAYLOAD_A") == 0);
        assert(strcmp((char *)ctx.pending_q[0].pool[s_new2].eager_buf, "NEW_PAYLOAD_B") == 0);
        assert(strcmp((char *)ctx.pending_q[0].pool[s_new3].eager_buf, "NEW_PAYLOAD_C") == 0);
        printf("  -> PASS: All original and new payloads 100%% verified pristine!\n");
    }

    /* Clean up buffers */
    for (int dir = 0; dir < 2; dir++) {
        for (int s = 0; s < PG_PENDING_EAGER_POOL_SIZE; s++) {
            free(ctx.eager_pending_buf[dir][s]);
        }
    }

    printf("\n>>> STRATEGY 3 VERIFICATION COMPLETE: ALL CHECKS PASSED <<<\n");
    return 0;
}
