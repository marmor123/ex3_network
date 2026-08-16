#include "pg.h"
#include "pg_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -myindex <01..NN> -list <host1> <host2> [host3 ...]\n", prog_name);
    fprintf(stderr, "Example: %s -myindex 03 -list mlxstud01 mlxstud02 mlxstud03 mlxstud04\n", prog_name);
}

static int parse_args(int argc, char **argv) {
    int index_found = 0;
    int list_found = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-myindex") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -myindex requires an index argument\n");
                return -1;
            }
            g_pg_args.myindex_raw = atoi(argv[++i]);
            if (g_pg_args.myindex_raw <= 0) {
                fprintf(stderr, "Error: -myindex must be positive (1-based index, got %d)\n",
                        g_pg_args.myindex_raw);
                return -1;
            }
            g_pg_args.rank = g_pg_args.myindex_raw - 1;
            index_found = 1;
        } else if (strcmp(argv[i], "-list") == 0) {
            list_found = 1;
            g_pg_args.size = 0;
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                if (g_pg_args.size >= PG_MAX_RANKS) {
                    fprintf(stderr, "Error: Exceeded max host count (%d)\n", PG_MAX_RANKS);
                    return -1;
                }
                snprintf(g_pg_args.hosts[g_pg_args.size], sizeof(g_pg_args.hosts[g_pg_args.size]),
                         "%s", argv[i]);
                g_pg_args.size++;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Error: Unrecognized option '%s'\n", argv[i]);
            return -1;
        }
    }

    if (!index_found) {
        fprintf(stderr, "Error: Missing required argument -myindex\n");
        return -1;
    }

    if (!list_found || g_pg_args.size < 2) {
        fprintf(stderr, "Error: -list must contain at least 2 hosts (got %d)\n", g_pg_args.size);
        return -1;
    }

    if (g_pg_args.rank >= g_pg_args.size) {
        fprintf(stderr, "Error: Index %d (rank %d) exceeds total hosts (%d)\n",
                g_pg_args.myindex_raw, g_pg_args.rank, g_pg_args.size);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (parse_args(argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    char *local_server = g_pg_args.hosts[g_pg_args.rank];
    void *pg_handle = NULL;

    int rc = connect_process_group(local_server, &pg_handle);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "connect_process_group failed with error code %d\n", rc);
        return 1;
    }

    int rank = pg_get_rank(pg_handle);
    int size = pg_get_size(pg_handle);
    int prev_rank = pg_get_prev_rank(pg_handle);
    int next_rank = pg_get_next_rank(pg_handle);
    struct pg_context *ctx = (struct pg_context *)pg_handle;

    printf("=================================================================\n");
    printf("[PG V2 RDMA Control Ring] Ring Edge & Ping Verified\n");
    printf("=================================================================\n");
    printf("  Total Ranks : %d\n", size);
    printf("  Local Rank  : %d (1-based index: %02d, servername: %s)\n",
           rank, g_pg_args.myindex_raw, local_server);
    printf("  RDMA Device : Port=%d, LID=0x%04x, MTU=%d\n",
           PG_IB_PORT, ctx->local_lid, ctx->active_mtu);
    printf("  Ring Edge IN (prev %d -> me %d):\n", prev_rank, rank);
    printf("    Peer sender QP   : QPN=0x%x PSN=0x%x LID=0x%04x\n",
           ctx->remote_from_prev.qpn, ctx->remote_from_prev.psn, ctx->remote_from_prev.lid);
    printf("    Local receiver QP: QPN=0x%x PSN=0x%x LID=0x%04x (inline=%u, sq_depth=%u)\n",
           ctx->local_from_prev.qpn, ctx->local_from_prev.psn, ctx->local_from_prev.lid,
           ctx->max_inline_data[PG_QP_DIR_FROM_PREV], ctx->sq_depth[PG_QP_DIR_FROM_PREV]);
    printf("  Ring Edge OUT (me %d -> next %d):\n", rank, next_rank);
    printf("    Local sender QP  : QPN=0x%x PSN=0x%x LID=0x%04x (inline=%u, sq_depth=%u)\n",
           ctx->local_to_next.qpn, ctx->local_to_next.psn, ctx->local_to_next.lid,
           ctx->max_inline_data[PG_QP_DIR_TO_NEXT], ctx->sq_depth[PG_QP_DIR_TO_NEXT]);
    printf("    Peer receiver QP : QPN=0x%x PSN=0x%x LID=0x%04x\n",
           ctx->remote_to_next.qpn, ctx->remote_to_next.psn, ctx->remote_to_next.lid);
    printf("=================================================================\n");
    printf("[PG V2 RDMA Control Ring] SUCCESS: Hardware ring established and ping verified.\n");

    /* Run V3 Rendezvous Segment Transfer Tests (4 KiB, 64 KiB, 1 MiB) */
    size_t test_sizes[] = {
        4 * 1024,       /* 4 KiB */
        64 * 1024,      /* 64 KiB */
        1024 * 1024     /* 1 MiB */
    };
    int num_tests = (int)(sizeof(test_sizes) / sizeof(test_sizes[0]));
    size_t max_size = 1024 * 1024;
    size_t max_total_size = max_size * (size_t)size;

    void *sendbuf = malloc(max_total_size);
    void *recvbuf = calloc(1, max_total_size);
    if (!sendbuf || !recvbuf) {
        fprintf(stderr, "Error allocating test buffers\n");
        if (sendbuf) free(sendbuf);
        if (recvbuf) free(recvbuf);
        pg_close(pg_handle);
        return 1;
    }

    printf("=================================================================\n");
    printf("[PG V3 Rendezvous] Testing RTS/CTS/RDMA_WRITE/DATA_DONE Transfer\n");
    printf("=================================================================\n");

    int all_passed = 1;
    for (int t = 0; t < num_tests; t++) {
        size_t sz = test_sizes[t];

        /* Synchronize all ranks before starting the next payload transfer */
        int brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Pre-test barrier failed with code %d\n", brc);
            all_passed = 0;
            break;
        }

        int trc = pg_test_v3_rendezvous(pg_handle, sendbuf, recvbuf, sz);
        if (trc == PG_SUCCESS) {
            printf("  [PASS] Size %7zu B (%6zu ints) -> Verified transferred data integrity from rank %d\n",
                   sz, sz / sizeof(int), prev_rank);
        } else {
            printf("  [FAIL] Size %7zu B (%6zu ints) -> Failed with code %d\n",
                   sz, sz / sizeof(int), trc);
            all_passed = 0;
            break;
        }

        /* Synchronize all ranks after finishing the transfer */
        brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Post-test barrier failed with code %d\n", brc);
            all_passed = 0;
            break;
        }
    }

    printf("=================================================================\n");
    if (all_passed) {
        printf("[PG V3 Rendezvous] SUCCESS: All rendezvous segment transfers verified.\n");
    } else {
        printf("[PG V3 Rendezvous] FAILURE: One or more rendezvous transfers failed.\n");
    }

    /* Run V4 Reduce-Scatter Tests (PG_INT + PG_SUM for 4 KiB, 64 KiB, 1 MiB) */
    printf("=================================================================\n");
    printf("[PG V4 Reduce-Scatter] Testing Ring Reduce-Scatter (PG_INT + PG_SUM)\n");
    printf("=================================================================\n");

    int rs_passed = 1;
    for (int t = 0; t < num_tests; t++) {
        size_t sz = test_sizes[t];
        int count = (int)(sz / sizeof(int));
        int segment_count = count / size;

        /* Synchronize all ranks before starting Reduce-Scatter */
        int brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Pre-RS barrier failed with code %d\n", brc);
            rs_passed = 0;
            break;
        }

        /* Fill sendbuf with deterministic arithmetic pattern: sendbuf[k] = (rank + 1) * (k + 1) */
        int *sints = (int *)sendbuf;
        int *rints = (int *)recvbuf;
        for (int k = 0; k < count; k++) {
            sints[k] = (rank + 1) * (k + 1);
        }
        memset(recvbuf, 0, (size_t)segment_count * sizeof(int));

        int rrc = pg_reduce_scatter(sendbuf, recvbuf, count, PG_INT, PG_SUM, pg_handle);
        if (rrc != PG_SUCCESS) {
            fprintf(stderr, "  [FAIL] Size %7zu B (%6d ints) -> pg_reduce_scatter returned %d\n",
                    sz, count, rrc);
            rs_passed = 0;
            break;
        }

        /* Verify received segment data against expected sum: ((size * (size + 1)) / 2) * (k + 1) */
        int expected_multiplier = (size * (size + 1)) / 2;
        int errors = 0;
        for (int j = 0; j < segment_count; j++) {
            int k = rank * segment_count + j;
            int expected = expected_multiplier * (k + 1);
            if (rints[j] != expected) {
                if (errors < 5) {
                    fprintf(stderr, "  [ERROR] Rank %d at segment idx %d (global %d): got %d, expected %d\n",
                            rank, j, k, rints[j], expected);
                }
                errors++;
            }
        }

        if (errors > 0) {
            fprintf(stderr, "  [FAIL] Size %7zu B (%6d ints) -> %d data mismatches on rank %d\n",
                    sz, count, errors, rank);
            rs_passed = 0;
            break;
        } else {
            printf("  [PASS] Size %7zu B (%6d ints, seg=%d ints) -> Rank %d verified sum arithmetic\n",
                   sz, count, segment_count, rank);
        }

        /* Synchronize all ranks after finishing Reduce-Scatter */
        brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Post-RS barrier failed with code %d\n", brc);
            rs_passed = 0;
            break;
        }
    }

    printf("=================================================================\n");
    if (rs_passed) {
        printf("[PG V4 Reduce-Scatter] SUCCESS: All reduce-scatter collective tests passed.\n");
    } else {
        printf("[PG V4 Reduce-Scatter] FAILURE: One or more reduce-scatter tests failed.\n");
    }

    /* Run V5 All-Gather Tests (PG_INT for 4 KiB, 64 KiB, 1 MiB per rank) */
    printf("=================================================================\n");
    printf("[PG V5 All-Gather] Testing Ring All-Gather (Zero-Copy RDMA Write)\n");
    printf("=================================================================\n");

    int ag_passed = 1;
    for (int t = 0; t < num_tests; t++) {
        size_t sz = test_sizes[t];
        int count = (int)(sz / sizeof(int));

        /* Synchronize all ranks before starting All-Gather */
        int brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Pre-AG barrier failed with code %d\n", brc);
            ag_passed = 0;
            break;
        }

        /* Fill sendbuf with deterministic pattern per rank: (rank + 1) * 100000 + k */
        int *sints = (int *)sendbuf;
        int *rints = (int *)recvbuf;
        for (int k = 0; k < count; k++) {
            sints[k] = (rank + 1) * 100000 + k;
        }
        memset(recvbuf, 0, (size_t)count * (size_t)size * sizeof(int));

        int agrc = pg_all_gather(sendbuf, recvbuf, count, PG_INT, pg_handle);
        if (agrc != PG_SUCCESS) {
            fprintf(stderr, "  [FAIL] Size %7zu B (%6d ints/rank) -> pg_all_gather returned %d\n",
                    sz, count, agrc);
            ag_passed = 0;
            break;
        }

        /* Verify every rank's gathered segment in recvbuf */
        int errors = 0;
        for (int s = 0; s < size; s++) {
            for (int k = 0; k < count; k++) {
                int expected = (s + 1) * 100000 + k;
                int actual = rints[s * count + k];
                if (actual != expected) {
                    if (errors < 5) {
                        fprintf(stderr, "  [ERROR] Rank %d at origin %d idx %d (global %d): got %d, expected %d\n",
                                rank, s, k, s * count + k, actual, expected);
                    }
                    errors++;
                }
            }
        }

        if (errors > 0) {
            fprintf(stderr, "  [FAIL] Size %7zu B (%6d ints/rank) -> %d data mismatches on rank %d\n",
                    sz, count, errors, rank);
            ag_passed = 0;
            break;
        } else {
            printf("  [PASS] Size %7zu B (%6d ints/rank, total=%6d ints) -> Rank %d verified all-gather slices\n",
                   sz, count, count * size, rank);
        }

        /* Synchronize all ranks after finishing All-Gather */
        brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Post-AG barrier failed with code %d\n", brc);
            ag_passed = 0;
            break;
        }
    }

    printf("=================================================================\n");
    if (ag_passed) {
        printf("[PG V5 All-Gather] SUCCESS: All all-gather collective tests passed.\n");
    } else {
        printf("[PG V5 All-Gather] FAILURE: One or more all-gather tests failed.\n");
    }

    /* Run V6 All-Reduce Tests (PG_INT + PG_SUM for 4 KiB, 64 KiB, 1 MiB) */
    printf("=================================================================\n");
    printf("[PG V6 All-Reduce] Testing Ring All-Reduce (RS + Barrier + AG)\n");
    printf("=================================================================\n");

    int ar_passed = 1;
    for (int t = 0; t < num_tests; t++) {
        size_t sz = test_sizes[t];
        int count = (int)(sz / sizeof(int));

        /* Synchronize all ranks before starting All-Reduce */
        int brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Pre-AR barrier failed with code %d\n", brc);
            ar_passed = 0;
            break;
        }

        /* Fill sendbuf with deterministic arithmetic pattern: sendbuf[k] = (rank + 1) * (k + 1) */
        int *sints = (int *)sendbuf;
        int *rints = (int *)recvbuf;
        for (int k = 0; k < count; k++) {
            sints[k] = (rank + 1) * (k + 1);
        }
        memset(recvbuf, 0, (size_t)count * sizeof(int));

        int arrc = pg_all_reduce(sendbuf, recvbuf, count, PG_INT, PG_SUM, pg_handle);
        if (arrc != PG_SUCCESS) {
            fprintf(stderr, "  [FAIL] Size %7zu B (%6d ints) -> pg_all_reduce returned %d\n",
                    sz, count, arrc);
            ar_passed = 0;
            break;
        }

        /* Verify all-reduced values against expected sum: ((size * (size + 1)) / 2) * (k + 1) */
        int expected_multiplier = (size * (size + 1)) / 2;
        int errors = 0;
        for (int k = 0; k < count; k++) {
            int expected = expected_multiplier * (k + 1);
            int actual = rints[k];
            if (actual != expected) {
                if (errors < 5) {
                    fprintf(stderr, "  [ERROR] Rank %d at idx %d: got %d, expected %d\n",
                            rank, k, actual, expected);
                }
                errors++;
            }
        }

        if (errors > 0) {
            fprintf(stderr, "  [FAIL] Size %7zu B (%6d ints) -> %d data mismatches on rank %d\n",
                    sz, count, errors, rank);
            ar_passed = 0;
            break;
        } else {
            printf("  [PASS] Size %7zu B (%6d ints) -> Rank %d verified all-reduce sum\n",
                   sz, count, rank);
        }

        /* Synchronize all ranks after finishing All-Reduce */
        brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Post-AR barrier failed with code %d\n", brc);
            ar_passed = 0;
            break;
        }
    }

    printf("=================================================================\n");
    if (ar_passed) {
        printf("[PG V6 All-Reduce] SUCCESS: All all-reduce collective tests passed.\n");
    } else {
        printf("[PG V6 All-Reduce] FAILURE: One or more all-reduce tests failed.\n");
    }

    rc = pg_close(pg_handle);
    free(sendbuf);
    free(recvbuf);

    if (rc != PG_SUCCESS || !all_passed || !rs_passed || !ag_passed || !ar_passed) {
        fprintf(stderr, "pg_close or tests failed (rc=%d, all_passed=%d, rs_passed=%d, ag_passed=%d, ar_passed=%d)\n",
                rc, all_passed, rs_passed, ag_passed, ar_passed);
        return 1;
    }

    return 0;
}
