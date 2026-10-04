/* Hardware regression: ordered collectives with changing buffers and counts. */
#include "../pg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 4) return 1;
    int rank = atoi(argv[1]);
    int size = argc - 2;
    if (size > PG_MAX_RANKS || rank < 0 || rank >= size) return 1;
    g_pg_args.rank = rank;
    g_pg_args.myindex_raw = rank + 1;
    g_pg_args.size = size;
    for (int i = 0; i < size; i++) {
        snprintf(g_pg_args.hosts[i], PG_MAX_HOST_LEN, "%s", argv[i + 2]);
    }
    void *handle = NULL;
    if (connect_process_group(g_pg_args.hosts[rank], &handle) != PG_SUCCESS) return 1;

    const int counts[] = {1, 17, 65537};
    const int max_count = counts[2];
    int *input = malloc((size_t)max_count * sizeof(*input));
    int *output[2] = {
        malloc((size_t)max_count * (size_t)size * sizeof(int)),
        malloc((size_t)max_count * (size_t)size * sizeof(int))
    };
    if (!input || !output[0] || !output[1]) return 1;
    for (int iteration = 0; iteration < 200; iteration++) {
        /* Repeat each count with alternating targets, then change the length. */
        int count = counts[(iteration / 4) % 3];
        int *dest = output[iteration % 2];
        for (int i = 0; i < count; i++) input[i] = iteration * 100000 + rank * 1000 + i;
        memset(dest, 0xff, (size_t)count * (size_t)size * sizeof(*dest));
        if (rank == 0 && iteration % 5 == 0) usleep(1000);
        int rc = pg_all_gather(input, dest, count, PG_INT, handle);
        if (rc != PG_SUCCESS) {
            fprintf(stderr, "Rank %d All-Gather iteration %d failed: %d\n", rank, iteration, rc);
            return 1;
        }
        for (int origin = 0; origin < size; origin++) {
            for (int i = 0; i < count; i++) {
                int expected = iteration * 100000 + origin * 1000 + i;
                if (dest[(size_t)origin * count + i] != expected) {
                    fprintf(stderr, "Rank %d iteration %d origin %d index %d: wrong output\n",
                            rank, iteration, origin, i);
                    return 1;
                }
            }
        }
    }
    for (int count = 1; count <= size; count++) {
        for (int i = 0; i < count; i++) input[i] = rank + 1;
        if (pg_all_reduce(input, output[0], count, PG_INT, PG_SUM, handle) != PG_SUCCESS) return 1;
        for (int i = 0; i < count; i++) {
            if (output[0][i] != size * (size + 1) / 2) return 1;
        }
    }
    /* Keep cached registered buffers alive until the process group closes. */
    int rc = pg_close(handle);
    free(input);
    free(output[0]);
    free(output[1]);
    if (rc != PG_SUCCESS) return 1;
    printf("Rank %d: 200 All-Gathers with alternating buffers/counts and small All-Reduces passed.\n", rank);
    return 0;
}
