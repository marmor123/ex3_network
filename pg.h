#ifndef PG_H
#define PG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Datatypes supported by the collective library */
typedef enum {
    PG_INT,
    PG_FLOAT,
    PG_DOUBLE
} DATATYPE;

/* Operations supported by the collective library */
typedef enum {
    PG_SUM,
    PG_MIN,
    PG_MAX,
    PG_PROD
} OPERATION;

/* Return / error codes */
enum {
    PG_SUCCESS          =  0,
    PG_ERR_INVAL        = -1,
    PG_ERR_NOMEM        = -2,
    PG_ERR_RDMA         = -3,
    PG_ERR_TCP          = -4,
    PG_ERR_TIMEOUT      = -5,
    PG_ERR_UNSUPPORTED  = -6
};

/* Maximum supported ranks and hostname length */
#define PG_MAX_RANKS 64
#define PG_MAX_HOST_LEN 256

/* Parsed CLI process group arguments */
struct pg_args {
    int myindex_raw;                            /* 1-based index from command line */
    int rank;                                   /* 0-based rank (myindex_raw - 1) */
    int size;                                   /* Total number of processes in -list */
    char hosts[PG_MAX_RANKS][PG_MAX_HOST_LEN];  /* Hostnames in ring order */
};

/* Global parsed CLI arguments populated by main before connect_process_group */
extern struct pg_args g_pg_args;

/* Public API required by the assignment */
int connect_process_group(char *servername, void **pg_handle);
int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op,
                      void *pg_handle);
int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype,
                  void *pg_handle);
int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op,
                  void *pg_handle);
int pg_close(void *pg_handle);

/* Topology query helpers */
int pg_get_rank(void *pg_handle);
int pg_get_size(void *pg_handle);
int pg_get_prev_rank(void *pg_handle);
int pg_get_next_rank(void *pg_handle);
const char *pg_get_hostname(void *pg_handle, int rank);

#ifdef __cplusplus
}
#endif

#endif /* PG_H */
