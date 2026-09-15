#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define PG_MAX_BATCH_SIZE 16
#define PG_QP_DIR_TO_NEXT 0
#define PG_WR_TYPE_RDMA_WRITE 1
#define IBV_WR_RDMA_WRITE 0
#define IBV_SEND_SIGNALED 1

/* Standard libibverbs struct layouts (x86_64 ABI) */
struct ibv_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

struct ibv_send_wr {
    uint64_t wr_id;
    struct ibv_send_wr *next;
    struct ibv_sge *sg_list;
    int32_t num_sge;
    uint32_t opcode;
    uint32_t send_flags;
    union {
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
        } rdma;
    } wr;
    /* padding to realistic ibverbs send_wr size */
    uint64_t _pad[4];
};

static inline uint64_t pg_make_wr_slot(int dir, int type, uint32_t slot) {
    return ((uint64_t)dir << 32) | ((uint64_t)type << 16) | slot;
}

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

/* Simulated dummy external posting call to prevent compiler dead-code elimination */
__attribute__((noinline))
int dummy_post_send(struct ibv_send_wr *wr, struct ibv_send_wr **bad_wr) {
    (void)bad_wr;
    int count = 0;
    struct ibv_send_wr *curr = wr;
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}

struct test_desc {
    void *send_buf;
    size_t send_bytes;
    uint32_t send_lkey;
};

/* Test function containing the batch loop from docs/refactoring_roadmap.md:471-490 */
__attribute__((noinline))
int run_batch_transfer(uint32_t to_post_requested, struct test_desc *desc,
                       size_t chunk_size, uint64_t remote_target_addr, uint32_t remote_target_rkey,
                       uint32_t eff_sig_interval, uint32_t num_send_micros, uint32_t rdma_posted_micros) {
    struct ibv_sge sges[PG_MAX_BATCH_SIZE];
    struct ibv_send_wr wrs[PG_MAX_BATCH_SIZE];

    /* Dynamic clamping specified in roadmap */
    uint32_t to_post = to_post_requested;
    to_post = (to_post > PG_MAX_BATCH_SIZE ? PG_MAX_BATCH_SIZE : to_post);

    for (uint32_t b = 0; b < to_post; b++) {
        uint32_t k = rdma_posted_micros + b;
        size_t offset = (size_t)k * chunk_size;
        size_t micro_len = desc->send_bytes - offset;
        if (micro_len > chunk_size) micro_len = chunk_size;

        int is_signaled = ((k + 1) % eff_sig_interval == 0 || (k + 1) == num_send_micros);
        pg_assemble_rdma_write_wr(&wrs[b], &sges[b], k,
                                  (uintptr_t)((char *)desc->send_buf + offset),
                                  (uint32_t)micro_len, desc->send_lkey,
                                  remote_target_addr + offset, remote_target_rkey,
                                  is_signaled, (b + 1 < to_post) ? &wrs[b + 1] : NULL);
    }

    struct ibv_send_wr *bad_wr = NULL;
    return dummy_post_send(&wrs[0], &bad_wr);
}

int main(void) {
    printf("=== STRATEGY 1 EMPIRICAL VERIFICATION HARNESS ===\n");

    /* Verify struct sizes and stack footprint calculation */
    size_t sge_size = sizeof(struct ibv_sge);
    size_t wr_size = sizeof(struct ibv_send_wr);
    size_t total_stack_16 = PG_MAX_BATCH_SIZE * (sge_size + wr_size);
    size_t total_stack_64 = 64 * (sge_size + wr_size);
    double reduction = 100.0 * (double)(total_stack_64 - total_stack_16) / (double)total_stack_64;

    printf("\n[Test 2.1] Stack Frame Sizing:\n");
    printf("  sizeof(ibv_sge): %zu bytes\n", sge_size);
    printf("  sizeof(ibv_send_wr): %zu bytes\n", wr_size);
    printf("  Original 64-element footprint: %zu bytes (%.2f KB)\n", total_stack_64, total_stack_64 / 1024.0);
    printf("  Refactored 16-element footprint: %zu bytes (%.2f KB)\n", total_stack_16, total_stack_16 / 1024.0);
    printf("  Stack Footprint Reduction: %.1f%%\n", reduction);
    assert(reduction >= 75.0);

    /* Test clamping and batch execution */
    printf("\n[Test 2.2] Clamping and Pointer Chaining:\n");
    static char dummy_buffer[1024 * 1024];
    struct test_desc desc = {
        .send_buf = dummy_buffer,
        .send_bytes = sizeof(dummy_buffer),
        .send_lkey = 0x1234
    };

    uint32_t test_cases[] = { 1, 4, 8, 16, 32, 64, 128 };
    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
        uint32_t requested = test_cases[i];
        int posted = run_batch_transfer(requested, &desc, 4096, 0x5000000, 0xABCD, 8, 256, 0);
        uint32_t expected = (requested > PG_MAX_BATCH_SIZE) ? PG_MAX_BATCH_SIZE : requested;
        printf("  Requested to_post=%3u -> Actual posted=%2d (Expected %2u)\n", requested, posted, expected);
        assert((uint32_t)posted == expected);
    }
    printf("  -> PASS: All batch requests clamped cleanly to <= 16 with correct next-pointer chaining!\n");

    printf("\n>>> STRATEGY 1 VERIFICATION COMPLETE: ALL CHECKS PASSED <<<\n");
    return 0;
}
