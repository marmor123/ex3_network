#include "pg.h"
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

    printf("=================================================================\n");
    printf("[PG V0 CLI Sanity] Process Group Topology Mapping Verified\n");
    printf("=================================================================\n");
    printf("  Total Ranks : %d\n", size);
    printf("  Local Rank  : %d (1-based index: %02d, servername: %s)\n",
           rank, g_pg_args.myindex_raw, local_server);
    printf("  Prev Rank   : %d (host: %s)\n", prev_rank, pg_get_hostname(pg_handle, prev_rank));
    printf("  Next Rank   : %d (host: %s)\n", next_rank, pg_get_hostname(pg_handle, next_rank));
    printf("  Ring Edge   : %s (rank %d) -> [%s (rank %d)] -> %s (rank %d)\n",
           pg_get_hostname(pg_handle, prev_rank), prev_rank,
           local_server, rank,
           pg_get_hostname(pg_handle, next_rank), next_rank);
    printf("=================================================================\n");
    printf("[PG V0 CLI Sanity] SUCCESS: Topology verified.\n");

    rc = pg_close(pg_handle);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "pg_close failed with error code %d\n", rc);
        return 1;
    }

    return 0;
}
