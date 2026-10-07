#define _POSIX_C_SOURCE 200809L
#include "pg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Run concurrently on every host, with the same ordered list and unique index:
 * ./test -myindex 01 -list host1 host2
 * ./test -myindex 02 -list host1 host2
 * For four hosts, use indices 01..04 and list all four hosts on every rank.
 */
static int parse_args(int argc, char **argv) {
    int have_index = 0, have_list = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-myindex") && !have_index && i + 1 < argc) {
            char *end;
            long index = strtol(argv[++i], &end, 10);
            if (!argv[i][0] || *end || index < 1 || index > PG_MAX_RANKS)
                return -1;
            g_pg_args.myindex_raw = (int)index;
            g_pg_args.rank = (int)index - 1;
            have_index = 1;
        } else if (!strcmp(argv[i], "-list") && !have_list) {
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                const char *host = argv[++i];
                if (!host[0] || strlen(host) >= PG_MAX_HOST_LEN ||
                    g_pg_args.size == PG_MAX_RANKS)
                    return -1;
                strcpy(g_pg_args.hosts[g_pg_args.size++], host);
            }
            have_list = 1;
        } else {
            return -1;
        }
    }
    return have_index && have_list && g_pg_args.size >= 2 &&
           g_pg_args.rank < g_pg_args.size ? 0 : -1;
}

/* Keep every rank on the same path after an allocation or validation failure.
 * This also aligns ranks before the next timed collective.
 */
static int all_ok(void *group, int local_ok) {
    int status[PG_MAX_RANKS] = {0};
    status[pg_get_rank(group)] = local_ok;
    if (pg_all_gather(&status[pg_get_rank(group)], status, 1, PG_INT, group)
        != PG_SUCCESS)
        return 0;
    for (int i = 0; i < pg_get_size(group); ++i)
        if (!status[i]) return 0;
    return 1;
}

static int run_tests(void *group, int *send, int *recv, int per_rank) {
    int rank = pg_get_rank(group), size = pg_get_size(group);
    int count = per_rank * size + 1; /* Exercise remainder partitioning. */
    int local_count = count / size + (rank < count % size);
    int offset = rank * (count / size) +
                 (rank < count % size ? rank : count % size);
    const char *names[] = {"Reduce-Scatter", "All-Gather", "All-Reduce"};

    for (int kind = 0; kind < 3; ++kind) {
        /* Inplace reduction modifies send, so refill before every call. */
        for (int i = 0; i < count; ++i) send[i] = rank + 1 + i % 97;
        memset(recv, 0, (size_t)count * sizeof(*recv));
        struct timespec start, finish;
        int clock_ok = clock_gettime(CLOCK_MONOTONIC, &start) == 0;
        if (!all_ok(group, clock_ok)) return 1;
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return 1;
        int rc;
        if (kind == 0)
            rc = pg_reduce_scatter(send, recv, count, PG_INT, PG_SUM, group);
        else if (kind == 1)
            rc = pg_all_gather(send, recv, per_rank, PG_INT, group);
        else
            rc = pg_all_reduce(send, recv, count, PG_INT, PG_SUM, group);
        clock_ok = clock_gettime(CLOCK_MONOTONIC, &finish) == 0;
        if (rc != PG_SUCCESS) {
            fprintf(stderr, "Rank %d: %s returned %d\n", rank, names[kind], rc);
            return 1;
        }

        int checked = kind == 0 ? local_count :
                      (kind == 1 ? per_rank * size : count);
        int ok = clock_ok;
        for (int i = 0; i < checked; ++i) {
            int expected = kind == 1 ? i / per_rank + 1 + (i % per_rank) % 97 :
                           size * (size + 1) / 2 +
                           size * ((i + (kind == 0 ? offset : 0)) % 97);
            if (recv[i] != expected) {
                fprintf(stderr, "Rank %d: %s[%d] = %d, expected %d\n",
                        rank, names[kind], i, recv[i], expected);
                ok = 0;
                break;
            }
        }
#ifndef PG_WORKBUFFER_INPLACE
        if (kind != 1) {
            for (int i = 0; i < count; ++i) {
                if (send[i] != rank + 1 + i % 97) {
                    fprintf(stderr, "Rank %d: %s modified input in safe mode\n",
                            rank, names[kind]);
                    ok = 0;
                    break;
                }
            }
        }
#endif
        if (!all_ok(group, ok)) return 1;
        double usec = (finish.tv_sec - start.tv_sec) * 1e6 +
                      (finish.tv_nsec - start.tv_nsec) / 1e3;
        printf("Rank %d: PASS %-14s count=%d %.1f us\n", rank, names[kind],
               kind == 1 ? per_rank : count, usec);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("Usage: %s -myindex <01..NN> -list <host1> <host2> [host3 ...]\n",
               argv[0]);
        return 0;
    }
    if (parse_args(argc, argv) != 0) {
        fprintf(stderr, "Usage: %s -myindex <01..NN> -list <host1> <host2> [host3 ...]\n",
                argv[0]);
        return 1;
    }
    void *group = NULL;
    int rc = connect_process_group(g_pg_args.hosts[g_pg_args.rank], &group);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "connect_process_group returned %d\n", rc);
        return 1;
    }

    const int large = 262144; /* 1 MiB per rank: multiple pipeline chunks. */
    size_t bytes = ((size_t)large * pg_get_size(group) + 1) * sizeof(int);
    int *send = malloc(bytes), *recv = malloc(bytes);
    int failed = !all_ok(group, send != NULL && recv != NULL);
    if (!failed) failed = run_tests(group, send, recv, 16);
    if (!failed) failed = run_tests(group, send, recv, large);
    rc = pg_close(group);
    /* Keep buffers alive until close: the library caches their registrations. */
    free(recv);
    free(send);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "pg_close returned %d\n", rc);
        failed = 1;
    }
    return failed ? 1 : 0;
}
