#include "pg.h"
#include "pg_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global process group arguments initialized from CLI */
struct pg_args g_pg_args = {0};

int connect_process_group(char *servername, void **pg_handle) {
    if (!servername || !pg_handle) {
        fprintf(stderr, "[pg] Error: Invalid NULL argument to connect_process_group\n");
        return PG_ERR_INVAL;
    }

    if (g_pg_args.size < 2) {
        fprintf(stderr, "[pg] Error: Process group size (%d) must be at least 2\n", g_pg_args.size);
        return PG_ERR_INVAL;
    }

    if (g_pg_args.rank < 0 || g_pg_args.rank >= g_pg_args.size) {
        fprintf(stderr, "[pg] Error: Local rank %d out of bounds [0, %d)\n",
                g_pg_args.rank, g_pg_args.size);
        return PG_ERR_INVAL;
    }

    /* Validate servername matches our host list at rank */
    if (strncmp(servername, g_pg_args.hosts[g_pg_args.rank], PG_MAX_HOST_LEN) != 0) {
        fprintf(stderr, "[pg] Error: servername '%s' does not match list[%d] ('%s')\n",
                servername, g_pg_args.rank, g_pg_args.hosts[g_pg_args.rank]);
        return PG_ERR_INVAL;
    }

    struct pg_context *ctx = (struct pg_context *)calloc(1, sizeof(struct pg_context));
    if (!ctx) {
        perror("[pg] Error allocating pg_context");
        return PG_ERR_NOMEM;
    }

    ctx->rank = g_pg_args.rank;
    ctx->size = g_pg_args.size;
    ctx->prev_rank = (ctx->rank - 1 + ctx->size) % ctx->size;
    ctx->next_rank = (ctx->rank + 1) % ctx->size;

    snprintf(ctx->servername, sizeof(ctx->servername), "%s", servername);

    for (int i = 0; i < ctx->size; i++) {
        snprintf(ctx->host_list[i], sizeof(ctx->host_list[i]), "%s", g_pg_args.hosts[i]);
    }

    *pg_handle = (void *)ctx;
    return PG_SUCCESS;
}

int pg_close(void *pg_handle) {
    if (!pg_handle) {
        return PG_SUCCESS;
    }

    struct pg_context *ctx = (struct pg_context *)pg_handle;
    free(ctx);
    return PG_SUCCESS;
}

int pg_get_rank(void *pg_handle) {
    if (!pg_handle) return -1;
    return ((struct pg_context *)pg_handle)->rank;
}

int pg_get_size(void *pg_handle) {
    if (!pg_handle) return -1;
    return ((struct pg_context *)pg_handle)->size;
}

int pg_get_prev_rank(void *pg_handle) {
    if (!pg_handle) return -1;
    return ((struct pg_context *)pg_handle)->prev_rank;
}

int pg_get_next_rank(void *pg_handle) {
    if (!pg_handle) return -1;
    return ((struct pg_context *)pg_handle)->next_rank;
}

const char *pg_get_hostname(void *pg_handle, int rank) {
    if (!pg_handle) return NULL;
    struct pg_context *ctx = (struct pg_context *)pg_handle;
    if (rank < 0 || rank >= ctx->size) return NULL;
    return ctx->host_list[rank];
}

int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op,
                      void *pg_handle) {
    (void)sendbuf;
    (void)recvbuf;
    (void)count;
    (void)datatype;
    (void)op;
    (void)pg_handle;
    return PG_ERR_UNSUPPORTED;
}

int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype,
                  void *pg_handle) {
    (void)sendbuf;
    (void)recvbuf;
    (void)count;
    (void)datatype;
    (void)pg_handle;
    return PG_ERR_UNSUPPORTED;
}

int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op,
                  void *pg_handle) {
    (void)sendbuf;
    (void)recvbuf;
    (void)count;
    (void)datatype;
    (void)op;
    (void)pg_handle;
    return PG_ERR_UNSUPPORTED;
}
