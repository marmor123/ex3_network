#ifndef PG_INTERNAL_H
#define PG_INTERNAL_H

#include "pg.h"

/* Internal context structure represented by void *pg_handle */
struct pg_context {
    int rank;                                   /* 0-based local rank */
    int size;                                   /* Total number of ranks in ring */
    int prev_rank;                              /* (rank - 1 + size) % size */
    int next_rank;                              /* (rank + 1) % size */
    char servername[PG_MAX_HOST_LEN];           /* Validated local servername */
    char host_list[PG_MAX_RANKS][PG_MAX_HOST_LEN]; /* Copy of ring hostnames */
};

#endif /* PG_INTERNAL_H */
