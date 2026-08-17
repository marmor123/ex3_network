#include "pg.h"
#include "pg_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>

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

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static int check_collective_alloc(void *pg_handle, void *buf1, void *buf2, size_t sz, int count) {
    int rank = pg_get_rank(pg_handle);
    int size = pg_get_size(pg_handle);
    int local_ok = (buf1 != NULL && buf2 != NULL) ? 1 : 0;
    int local_status[PG_MAX_RANKS];
    int global_status[PG_MAX_RANKS];
    for (int i = 0; i < size; i++) {
        local_status[i] = local_ok;
        global_status[i] = 0;
    }

    int rc = pg_all_reduce(local_status, global_status, size, PG_INT, PG_SUM, pg_handle);
    if (rc != PG_SUCCESS || global_status[0] != size) {
        if (rank == 0) {
            if (count > 0) {
                printf("%-12zu | %-12d | %-48s\n", sz, count, "[SKIP] Allocation failed on one or more ranks");
            } else {
                printf("  [SKIP] Size %10zu B -> Allocation failed on one or more ranks (insufficient memory)\n", sz);
            }
        }
        return 0;
    }
    return 1;
}

static int run_benchmark_harness(void *pg_handle) {
    int rank = pg_get_rank(pg_handle);
    int size = pg_get_size(pg_handle);

    const char *mode_str = "RENDEZVOUS";
#if (PG_ACTIVE_MODE == PG_MODE_TYPE_EAGER)
    mode_str = "EAGER";
#elif (PG_ACTIVE_MODE == PG_MODE_TYPE_AUTO)
    mode_str = "AUTO (Threshold <= 8 KiB Eager, > 8 KiB Rendezvous)";
#endif

    if (rank == 0) {
        printf("========================================================================================================\n");
        printf("[PG V9 Benchmark Harness] Running All-Reduce Sweep (Mode: %s, Warmup + %d Timed Iterations)\n",
               mode_str, PG_BENCH_ITER);
        printf("========================================================================================================\n");
        printf("%-12s | %-12s | %-12s | %-12s | %-12s | %-16s\n",
               "Size (Bytes)", "Count (ints)", "Min (us)", "Median (us)", "Avg (us)", "Effective BW");
        printf("--------------------------------------------------------------------------------------------------------\n");
    }

    size_t min_bytes = PG_BENCH_MIN_BYTES;
    size_t max_bytes = PG_BENCH_MAX_BYTES;
    int bench_iter = PG_BENCH_ITER;

    const char *min_env = getenv("PG_BENCH_MIN_BYTES");
    if (min_env && *min_env) {
        min_bytes = (size_t)strtoull(min_env, NULL, 10);
    }
    const char *max_env = getenv("PG_BENCH_MAX_BYTES");
    if (max_env && *max_env) {
        max_bytes = (size_t)strtoull(max_env, NULL, 10);
    }
    const char *iter_env = getenv("PG_BENCH_ITER");
    if (iter_env && *iter_env) {
        int v = atoi(iter_env);
        if (v > 0) bench_iter = v;
    }

    if (min_bytes < (size_t)size * sizeof(int)) {
        min_bytes = (size_t)size * sizeof(int);
    }

    for (size_t sz = min_bytes; sz <= max_bytes; sz = (sz < 1024 ? sz * 4 : sz * 2)) {
        int count = (int)(sz / sizeof(int));
        if (count % size != 0) {
            count += (size - (count % size));
            sz = (size_t)count * sizeof(int);
        }

        void *sendbuf = malloc(sz);
        void *recvbuf = malloc(sz);

        if (!check_collective_alloc(pg_handle, sendbuf, recvbuf, sz, count)) {
            if (sendbuf) free(sendbuf);
            if (recvbuf) free(recvbuf);
            continue;
        }

        int *sints = (int *)sendbuf;
        for (int k = 0; k < count; k++) {
            sints[k] = (rank + 1) * (int)((k % 1000) + 1);
        }
        memset(recvbuf, 0, sz);

        /* Warmup collective outside timed region */
        int rc = pg_all_reduce(sendbuf, recvbuf, count, PG_INT, PG_SUM, pg_handle);
        if (rc != PG_SUCCESS) {
            fprintf(stderr, "Rank %d warmup pg_all_reduce failed with code %d\n", rank, rc);
            free(sendbuf);
            free(recvbuf);
            return rc;
        }

        rc = pg_barrier(pg_handle);
        if (rc != PG_SUCCESS) {
            fprintf(stderr, "Rank %d post-warmup barrier failed with code %d\n", rank, rc);
            free(sendbuf);
            free(recvbuf);
            return rc;
        }

        /* Timed iterations */
        if (bench_iter > 100) bench_iter = 100;
        if (bench_iter < 1) bench_iter = 1;
        double times_us[100];
        double total_us = 0.0;

        for (int iter = 0; iter < bench_iter; iter++) {
            rc = pg_barrier(pg_handle);
            if (rc != PG_SUCCESS) {
                fprintf(stderr, "Rank %d pre-iteration barrier failed with code %d\n", rank, rc);
                free(sendbuf);
                free(recvbuf);
                return rc;
            }

            struct timespec t_start, t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_start);

            rc = pg_all_reduce(sendbuf, recvbuf, count, PG_INT, PG_SUM, pg_handle);
            if (rc != PG_SUCCESS) {
                fprintf(stderr, "Rank %d timed iteration %d failed with code %d\n", rank, iter, rc);
                free(sendbuf);
                free(recvbuf);
                return rc;
            }

            clock_gettime(CLOCK_MONOTONIC, &t_end);

            double iter_us = (double)(t_end.tv_sec - t_start.tv_sec) * 1e6 +
                             (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e3;
            times_us[iter] = iter_us;
            total_us += iter_us;

            rc = pg_barrier(pg_handle);
            if (rc != PG_SUCCESS) {
                fprintf(stderr, "Rank %d post-iteration barrier failed with code %d\n", rank, rc);
                free(sendbuf);
                free(recvbuf);
                return rc;
            }
        }

        qsort(times_us, bench_iter, sizeof(double), compare_doubles);
        double min_us = times_us[0];
        double median_us = times_us[bench_iter / 2];
        double avg_us = total_us / bench_iter;

        double effective_bytes = 2.0 * (double)(size - 1) / (double)size * (double)sz;
        double effective_gbps = (effective_bytes * 8.0) / (median_us * 1e3);

        if (rank == 0) {
            printf("%-12zu | %-12d | %12.2f | %12.2f | %12.2f | %10.2f Gbps\n",
                   sz, count, min_us, median_us, avg_us, effective_gbps);
        }

        free(sendbuf);
        free(recvbuf);
    }

    if (rank == 0) {
        printf("========================================================================================================\n");
        printf("[PG V8 Benchmark Harness] SUCCESS: Sweep completed.\n");
        printf("========================================================================================================\n");
    }

    return PG_SUCCESS;
}

static int run_v10_datatypes_and_ops_tests(void *pg_handle) {
    int rank = pg_get_rank(pg_handle);
    int size = pg_get_size(pg_handle);
    int count = 65536;

    if (rank == 0) {
        printf("=================================================================\n");
        printf("[PG V10 Datatypes & Operations] Testing SIMD Vectorized Reduction\n");
        printf("=================================================================\n");
    }

    DATATYPE datatypes[] = { PG_INT, PG_FLOAT, PG_DOUBLE };
    const char *dt_names[] = { "PG_INT", "PG_FLOAT", "PG_DOUBLE" };
    OPERATION ops[] = { PG_SUM, PG_MIN, PG_MAX, PG_PROD };
    const char *op_names[] = { "PG_SUM", "PG_MIN", "PG_MAX", "PG_PROD" };

    for (int d = 0; d < 3; d++) {
        DATATYPE dt = datatypes[d];
        size_t elem_sz = (dt == PG_INT ? sizeof(int) : (dt == PG_FLOAT ? sizeof(float) : sizeof(double)));
        size_t sz = (size_t)count * elem_sz;

        for (int o = 0; o < 4; o++) {
            OPERATION op = ops[o];

            void *sendbuf = malloc(sz);
            void *recvbuf = calloc(1, sz);
            if (!sendbuf || !recvbuf) {
                if (sendbuf) free(sendbuf);
                if (recvbuf) free(recvbuf);
                return PG_ERR_NOMEM;
            }

            if (dt == PG_INT) {
                int *s = (int *)sendbuf;
                for (int i = 0; i < count; i++) {
                    if (op == PG_PROD) s[i] = (rank % 2 == 0) ? 2 : 1;
                    else s[i] = (rank + 1) * 10 + (i % 7);
                }
            } else if (dt == PG_FLOAT) {
                float *s = (float *)sendbuf;
                for (int i = 0; i < count; i++) {
                    if (op == PG_PROD) s[i] = (rank % 2 == 0) ? 1.5f : 1.0f;
                    else s[i] = (float)((rank + 1) * 10) + (float)(i % 7) * 0.1f;
                }
            } else if (dt == PG_DOUBLE) {
                double *s = (double *)sendbuf;
                for (int i = 0; i < count; i++) {
                    if (op == PG_PROD) s[i] = (rank % 2 == 0) ? 1.5 : 1.0;
                    else s[i] = (double)((rank + 1) * 10) + (double)(i % 7) * 0.1;
                }
            }

            pg_barrier(pg_handle);
            int rc = pg_all_reduce(sendbuf, recvbuf, count, dt, op, pg_handle);
            if (rc != PG_SUCCESS) {
                fprintf(stderr, "  [FAIL] %s x %s failed with code %d\n", dt_names[d], op_names[o], rc);
                free(sendbuf); free(recvbuf);
                return rc;
            }

            int errors = 0;
            if (dt == PG_INT) {
                int *r = (int *)recvbuf;
                for (int i = 0; i < count; i++) {
                    int expected = 0;
                    if (op == PG_SUM) {
                        for (int rk = 0; rk < size; rk++) expected += (rk + 1) * 10 + (i % 7);
                    } else if (op == PG_MIN) {
                        expected = 1 * 10 + (i % 7);
                    } else if (op == PG_MAX) {
                        expected = size * 10 + (i % 7);
                    } else if (op == PG_PROD) {
                        expected = 1;
                        for (int rk = 0; rk < size; rk++) expected *= (rk % 2 == 0 ? 2 : 1);
                    }
                    if (r[i] != expected) errors++;
                }
            } else if (dt == PG_FLOAT) {
                float *r = (float *)recvbuf;
                for (int i = 0; i < count; i++) {
                    float expected = 0.0f;
                    if (op == PG_SUM) {
                        for (int rk = 0; rk < size; rk++) expected += (float)((rk + 1) * 10) + (float)(i % 7) * 0.1f;
                    } else if (op == PG_MIN) {
                        expected = 10.0f + (float)(i % 7) * 0.1f;
                    } else if (op == PG_MAX) {
                        expected = (float)(size * 10) + (float)(i % 7) * 0.1f;
                    } else if (op == PG_PROD) {
                        expected = 1.0f;
                        for (int rk = 0; rk < size; rk++) expected *= (rk % 2 == 0 ? 1.5f : 1.0f);
                    }
                    float diff = r[i] - expected;
                    if (diff < -1e-3f || diff > 1e-3f) errors++;
                }
            } else if (dt == PG_DOUBLE) {
                double *r = (double *)recvbuf;
                for (int i = 0; i < count; i++) {
                    double expected = 0.0;
                    if (op == PG_SUM) {
                        for (int rk = 0; rk < size; rk++) expected += (double)((rk + 1) * 10) + (double)(i % 7) * 0.1;
                    } else if (op == PG_MIN) {
                        expected = 10.0 + (double)(i % 7) * 0.1;
                    } else if (op == PG_MAX) {
                        expected = (double)(size * 10) + (double)(i % 7) * 0.1;
                    } else if (op == PG_PROD) {
                        expected = 1.0;
                        for (int rk = 0; rk < size; rk++) expected *= (rk % 2 == 0 ? 1.5 : 1.0);
                    }
                    double diff = r[i] - expected;
                    if (diff < -1e-5 || diff > 1e-5) errors++;
                }
            }

            if (errors > 0) {
                fprintf(stderr, "  [FAIL] %s x %s -> %d mismatches on rank %d\n", dt_names[d], op_names[o], errors, rank);
                free(sendbuf); free(recvbuf);
                return PG_ERR_RDMA;
            } else if (rank == 0) {
                printf("  [PASS] %-10s x %-10s -> Verified %d elements SIMD exactness\n",
                       dt_names[d], op_names[o], count);
            }

            free(sendbuf);
            free(recvbuf);
        }
    }
    pg_barrier(pg_handle);
    if (rank == 0) {
        printf("[PG V10 Datatypes & Operations] SUCCESS: All 12 datatype x op combinations passed.\n");
    }
    return PG_SUCCESS;
}

static int run_v10_non_divisible_counts_tests(void *pg_handle) {
    int rank = pg_get_rank(pg_handle);
    int size = pg_get_size(pg_handle);
    int counts[] = { 1001, 1003, 33333, 1000007 };
    int num_counts = (int)(sizeof(counts) / sizeof(counts[0]));

    if (rank == 0) {
        printf("=================================================================\n");
        printf("[PG V10 Non-Divisible Counts] Testing Arbitrary Remainder Slices\n");
        printf("=================================================================\n");
    }

    for (int c = 0; c < num_counts; c++) {
        int count = counts[c];
        size_t sz = (size_t)count * sizeof(int);

        void *sendbuf = malloc(sz);
        void *recvbuf = calloc(1, sz);
        if (!sendbuf || !recvbuf) {
            if (sendbuf) free(sendbuf);
            if (recvbuf) free(recvbuf);
            return PG_ERR_NOMEM;
        }

        int *s = (int *)sendbuf;
        int *r = (int *)recvbuf;
        for (int i = 0; i < count; i++) {
            s[i] = (rank + 1) * 100 + (i % 100);
        }

        pg_barrier(pg_handle);
        int rc = pg_all_reduce(sendbuf, recvbuf, count, PG_INT, PG_SUM, pg_handle);
        if (rc != PG_SUCCESS) {
            fprintf(stderr, "  [FAIL] Non-divisible count %d failed with code %d\n", count, rc);
            free(sendbuf); free(recvbuf);
            return rc;
        }

        int expected_multiplier = (size * (size + 1)) / 2;
        int errors = 0;
        for (int i = 0; i < count; i++) {
            int expected = expected_multiplier * 100 + size * (i % 100);
            if (r[i] != expected) errors++;
        }

        if (errors > 0) {
            fprintf(stderr, "  [FAIL] Count %d -> %d mismatches on rank %d\n", count, errors, rank);
            free(sendbuf); free(recvbuf);
            return PG_ERR_RDMA;
        } else if (rank == 0) {
            printf("  [PASS] Count %d ints (remainder=%d) -> Verified across all ranks\n",
                   count, count % size);
        }

        free(sendbuf);
        free(recvbuf);
    }

    pg_barrier(pg_handle);
    if (rank == 0) {
        printf("[PG V10 Non-Divisible Counts] SUCCESS: All remainder count tests passed.\n");
    }
    return PG_SUCCESS;
}

static int run_v10_barrier_free_stress_test(void *pg_handle) {
    int rank = pg_get_rank(pg_handle);
    int size = pg_get_size(pg_handle);
    int count = 16384;
    size_t sz = (size_t)count * sizeof(int);

    if (rank == 0) {
        printf("=================================================================\n");
        printf("[PG V10 Barrier-Free Stress] Testing 100 Rapid Back-to-Back Iterations\n");
        printf("=================================================================\n");
    }

    void *sendbuf = malloc(sz);
    void *recvbuf = calloc(1, sz);
    if (!sendbuf || !recvbuf) {
        if (sendbuf) free(sendbuf);
        if (recvbuf) free(recvbuf);
        return PG_ERR_NOMEM;
    }

    int *s = (int *)sendbuf;
    int *r = (int *)recvbuf;
    for (int i = 0; i < count; i++) {
        s[i] = (rank + 1);
    }

    pg_barrier(pg_handle);

    int expected_sum = (size * (size + 1)) / 2;
    int rc = PG_SUCCESS;

    for (int iter = 0; iter < 100; iter++) {
        rc = pg_all_reduce(sendbuf, recvbuf, count, PG_INT, PG_SUM, pg_handle);
        if (rc != PG_SUCCESS) {
            fprintf(stderr, "  [FAIL] Rapid iteration %d failed with code %d\n", iter, rc);
            break;
        }
        if (r[0] != expected_sum) {
            fprintf(stderr, "  [FAIL] Rapid iteration %d data mismatch: got %d, expected %d\n",
                    iter, r[0], expected_sum);
            rc = PG_ERR_RDMA;
            break;
        }
    }

    free(sendbuf);
    free(recvbuf);
    pg_barrier(pg_handle);

    if (rc == PG_SUCCESS && rank == 0) {
        printf("[PG V10 Barrier-Free Stress] SUCCESS: 100 rapid iterations completed with 0 errors.\n");
    }
    return rc;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    mallopt(M_MMAP_MAX, 0);
    mallopt(M_TRIM_THRESHOLD, -1);

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
    printf("[PG Collective Ring] Process Group Ring Connected\n");
    printf("=================================================================\n");
    printf("  Total Ranks : %d\n", size);
    printf("  Local Rank  : %d (1-based index: %02d, servername: %s)\n",
           rank, g_pg_args.myindex_raw, local_server);
    printf("  RDMA Device : Port=%d, LID=0x%04x, MTU=%d\n",
           PG_IB_PORT, ctx->local_lid, ctx->active_mtu);
    printf("  Config Mode : %s | %s | %s\n",
#if (PG_ACTIVE_MODE == PG_MODE_TYPE_EAGER)
           "MODE=EAGER",
#elif (PG_ACTIVE_MODE == PG_MODE_TYPE_AUTO)
           "MODE=AUTO (<=8KiB eager, >8KiB rdv)",
#else
           "MODE=RENDEZVOUS",
#endif
#ifdef PROFILE_PERF
           "PROFILE=PERF (256 KiB)",
#else
           "PROFILE=BRINGUP (64 KiB)",
#endif
#ifdef PG_WORKBUFFER_INPLACE
           "WORKBUFFER=INPLACE"
#else
           "WORKBUFFER=SAFE"
#endif
    );
    printf("  Ring Neighbors : prev (rank %d) <-> me (rank %d) <-> next (rank %d)\n",
           prev_rank, rank, next_rank);
    printf("=================================================================\n");

    /* Run Collectives Verification Suite (4 KiB, 64 KiB, 1 MiB, 4 MiB, and 1 GiB for RDV) */
    size_t test_sizes[] = {
        4 * 1024,                    /* 4 KiB (sub-chunk) */
        64 * 1024,                   /* 64 KiB (single micro) */
        1024 * 1024,                 /* 1 MiB (16 micro-chunks) */
        4 * 1024 * 1024,             /* 4 MiB (64 micro-chunks) */
#if (PG_ACTIVE_MODE != PG_MODE_TYPE_EAGER)
        1024ULL * 1024ULL * 1024ULL  /* 1 GiB (16,384 micro-chunks for Rendezvous) */
#endif
    };
    int num_tests = (int)(sizeof(test_sizes) / sizeof(test_sizes[0]));

    /* Run V7 Pipelined Reduce-Scatter Tests (PG_INT + PG_SUM) */
    printf("=================================================================\n");
    printf("[PG V7 Reduce-Scatter] Testing Pipelined Reduce-Scatter (PG_INT + PG_SUM)\n");
    printf("=================================================================\n");

    int rs_passed = 1;
    for (int t = 0; t < num_tests; t++) {
        size_t sz = test_sizes[t];
        int count = (int)(sz / sizeof(int));
        int segment_count = count / size;
        size_t segment_bytes = (size_t)segment_count * sizeof(int);

        void *sendbuf = malloc(sz);
        void *recvbuf = calloc(1, segment_bytes);
        if (!check_collective_alloc(pg_handle, sendbuf, recvbuf, sz, 0)) {
            if (sendbuf) free(sendbuf);
            if (recvbuf) free(recvbuf);
            continue;
        }

        /* Synchronize all ranks before starting Reduce-Scatter */
        int brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Pre-RS barrier failed with code %d\n", brc);
            rs_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        /* Fill sendbuf with deterministic arithmetic pattern */
        int *sints = (int *)sendbuf;
        int *rints = (int *)recvbuf;
        for (int k = 0; k < count; k++) {
            sints[k] = (rank + 1) * (int)((k % 1000) + 1);
        }
        memset(recvbuf, 0, segment_bytes);

        int rrc = pg_reduce_scatter(sendbuf, recvbuf, count, PG_INT, PG_SUM, pg_handle);
        if (rrc != PG_SUCCESS) {
            fprintf(stderr, "  [FAIL] Size %10zu B (%9d ints) -> pg_reduce_scatter returned %d\n",
                    sz, count, rrc);
            rs_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        /* Verify received segment data against expected sum */
        int expected_multiplier = (size * (size + 1)) / 2;
        int errors = 0;
        for (int j = 0; j < segment_count; j++) {
            int k = rank * segment_count + j;
            int expected = expected_multiplier * (int)((k % 1000) + 1);
            if (rints[j] != expected) {
                if (errors < 5) {
                    fprintf(stderr, "  [ERROR] Rank %d at segment idx %d (global %d): got %d, expected %d\n",
                            rank, j, k, rints[j], expected);
                }
                errors++;
            }
        }

        if (errors > 0) {
            fprintf(stderr, "  [FAIL] Size %10zu B (%9d ints) -> %d data mismatches on rank %d\n",
                    sz, count, errors, rank);
            rs_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        } else {
            printf("  [PASS] Size %10zu B (%9d ints, seg=%d ints) -> Rank %d verified sum arithmetic\n",
                   sz, count, segment_count, rank);
        }

        /* Synchronize all ranks after finishing Reduce-Scatter */
        brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Post-RS barrier failed with code %d\n", brc);
            rs_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        free(sendbuf);
        free(recvbuf);
    }

    printf("=================================================================\n");
    if (rs_passed) {
        printf("[PG V7 Reduce-Scatter] SUCCESS: All reduce-scatter collective tests passed.\n");
    } else {
        printf("[PG V7 Reduce-Scatter] FAILURE: One or more reduce-scatter tests failed.\n");
    }

    /* Run V7 Pipelined All-Gather Tests (PG_INT Zero-Copy) */
    printf("=================================================================\n");
    printf("[PG V7 All-Gather] Testing Pipelined Ring All-Gather (Zero-Copy RDMA Write)\n");
    printf("=================================================================\n");

    int ag_passed = 1;
    for (int t = 0; t < num_tests; t++) {
        size_t sz = test_sizes[t];
        int count = (int)(sz / sizeof(int));
        size_t total_gather_bytes = sz * (size_t)size;

        void *sendbuf = malloc(sz);
        void *recvbuf = calloc(1, total_gather_bytes);
        if (!check_collective_alloc(pg_handle, sendbuf, recvbuf, total_gather_bytes, 0)) {
            if (sendbuf) free(sendbuf);
            if (recvbuf) free(recvbuf);
            continue;
        }

        /* Synchronize all ranks before starting All-Gather */
        int brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Pre-AG barrier failed with code %d\n", brc);
            ag_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        /* Fill sendbuf with deterministic pattern per rank */
        int *sints = (int *)sendbuf;
        int *rints = (int *)recvbuf;
        for (int k = 0; k < count; k++) {
            sints[k] = (rank + 1) * 100000 + (int)(k % 100000);
        }
        memset(recvbuf, 0, total_gather_bytes);

        int agrc = pg_all_gather(sendbuf, recvbuf, count, PG_INT, pg_handle);
        if (agrc != PG_SUCCESS) {
            fprintf(stderr, "  [FAIL] Size %10zu B (%9d ints/rank) -> pg_all_gather returned %d\n",
                    sz, count, agrc);
            ag_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        /* Verify every rank's gathered segment in recvbuf */
        int errors = 0;
        for (int s = 0; s < size; s++) {
            for (int k = 0; k < count; k++) {
                int expected = (s + 1) * 100000 + (int)(k % 100000);
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
            fprintf(stderr, "  [FAIL] Size %10zu B (%9d ints/rank) -> %d data mismatches on rank %d\n",
                    sz, count, errors, rank);
            ag_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        } else {
            printf("  [PASS] Size %10zu B (%9d ints/rank, total=%9d ints) -> Rank %d verified all-gather slices\n",
                   sz, count, count * size, rank);
        }

        /* Synchronize all ranks after finishing All-Gather */
        brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Post-AG barrier failed with code %d\n", brc);
            ag_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        free(sendbuf);
        free(recvbuf);
    }

    printf("=================================================================\n");
    if (ag_passed) {
        printf("[PG V7 All-Gather] SUCCESS: All all-gather collective tests passed.\n");
    } else {
        printf("[PG V7 All-Gather] FAILURE: One or more all-gather tests failed.\n");
    }

    /* Run V7 Pipelined All-Reduce Tests (PG_INT + PG_SUM) */
    printf("=================================================================\n");
    printf("[PG V7 All-Reduce] Testing Pipelined Ring All-Reduce (RS + Barrier + AG)\n");
    printf("=================================================================\n");

    int ar_passed = 1;
    for (int t = 0; t < num_tests; t++) {
        size_t sz = test_sizes[t];
        int count = (int)(sz / sizeof(int));

        void *sendbuf = malloc(sz);
        void *recvbuf = calloc(1, sz);
        if (!check_collective_alloc(pg_handle, sendbuf, recvbuf, sz, 0)) {
            if (sendbuf) free(sendbuf);
            if (recvbuf) free(recvbuf);
            continue;
        }

        /* Synchronize all ranks before starting All-Reduce */
        int brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Pre-AR barrier failed with code %d\n", brc);
            ar_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        /* Fill sendbuf with deterministic arithmetic pattern */
        int *sints = (int *)sendbuf;
        int *rints = (int *)recvbuf;
        for (int k = 0; k < count; k++) {
            sints[k] = (rank + 1) * (int)((k % 1000) + 1);
        }
        memset(recvbuf, 0, sz);

        int arrc = pg_all_reduce(sendbuf, recvbuf, count, PG_INT, PG_SUM, pg_handle);
        if (arrc != PG_SUCCESS) {
            fprintf(stderr, "  [FAIL] Size %10zu B (%9d ints) -> pg_all_reduce returned %d\n",
                    sz, count, arrc);
            ar_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        /* Verify all-reduced values against expected sum */
        int expected_multiplier = (size * (size + 1)) / 2;
        int errors = 0;
        for (int k = 0; k < count; k++) {
            int expected = expected_multiplier * (int)((k % 1000) + 1);
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
            fprintf(stderr, "  [FAIL] Size %10zu B (%9d ints) -> %d data mismatches on rank %d\n",
                    sz, count, errors, rank);
            ar_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        } else {
            printf("  [PASS] Size %10zu B (%9d ints) -> Rank %d verified all-reduce sum\n",
                   sz, count, rank);
        }

        /* Synchronize all ranks after finishing All-Reduce */
        brc = pg_barrier(pg_handle);
        if (brc != PG_SUCCESS) {
            fprintf(stderr, "Post-AR barrier failed with code %d\n", brc);
            ar_passed = 0;
            free(sendbuf);
            free(recvbuf);
            break;
        }

        free(sendbuf);
        free(recvbuf);
    }

    printf("=================================================================\n");
    if (ar_passed) {
        printf("[PG V7 All-Reduce] SUCCESS: All all-reduce collective tests passed.\n");
    } else {
        printf("[PG V7 All-Reduce] FAILURE: One or more all-reduce tests failed.\n");
    }

    int dt_rc = PG_SUCCESS;
    int rem_rc = PG_SUCCESS;
    int stress_rc = PG_SUCCESS;
    if (rs_passed && ag_passed && ar_passed) {
        dt_rc = run_v10_datatypes_and_ops_tests(pg_handle);
        rem_rc = run_v10_non_divisible_counts_tests(pg_handle);
        stress_rc = run_v10_barrier_free_stress_test(pg_handle);
    }

    int bench_rc = PG_SUCCESS;
    if (rs_passed && ag_passed && ar_passed &&
        dt_rc == PG_SUCCESS && rem_rc == PG_SUCCESS && stress_rc == PG_SUCCESS) {
        bench_rc = run_benchmark_harness(pg_handle);
    }

    rc = pg_close(pg_handle);

    if (rc != PG_SUCCESS || !rs_passed || !ag_passed || !ar_passed ||
        dt_rc != PG_SUCCESS || rem_rc != PG_SUCCESS || stress_rc != PG_SUCCESS || bench_rc != PG_SUCCESS) {
        fprintf(stderr, "pg_close or tests failed (rc=%d, rs_passed=%d, ag_passed=%d, ar_passed=%d, dt_rc=%d, rem_rc=%d, stress_rc=%d, bench_rc=%d)\n",
                rc, rs_passed, ag_passed, ar_passed, dt_rc, rem_rc, stress_rc, bench_rc);
        return 1;
    }

    return 0;
}
