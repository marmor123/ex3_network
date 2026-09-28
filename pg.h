#ifndef PG_H
#define PG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Datatypes supported by the RDMA collective library.
 */
typedef enum {
    PG_INT,     /**< 32-bit signed integer (int32_t) */
    PG_FLOAT,   /**< 32-bit IEEE 754 single-precision floating point (float) */
    PG_DOUBLE   /**< 64-bit IEEE 754 double-precision floating point (double) */
} DATATYPE;

/**
 * @brief Reduction operations supported by the collective library.
 *
 * Vectorized with SSE4.2 128-bit SIMD intrinsics and 4x loop unrolling.
 */
typedef enum {
    PG_SUM,     /**< Element-wise arithmetic summation (+) */
    PG_MIN,     /**< Element-wise minimum */
    PG_MAX,     /**< Element-wise maximum */
    PG_PROD     /**< Element-wise arithmetic product (*) */
} OPERATION;

/**
 * @brief Library return and error codes.
 */
enum {
    PG_SUCCESS          =  0,   /**< Operation completed successfully */
    PG_ERR_INVAL        = -1,   /**< Invalid argument or null pointer */
    PG_ERR_NOMEM        = -2,   /**< Dynamic memory allocation failed */
    PG_ERR_RDMA         = -3,   /**< InfiniBand Verbs operation error */
    PG_ERR_TCP          = -4,   /**< TCP bootstrap connection/handshake error */
    PG_ERR_TIMEOUT      = -5,   /**< Progress engine watchdog timeout */
    PG_ERR_UNSUPPORTED  = -6    /**< Unsupported datatype or operation combination */
};

/** @brief Maximum number of ranks supported in a process group ring */
#define PG_MAX_RANKS 64

/** @brief Maximum length in bytes of a hostname string (including null terminator) */
#define PG_MAX_HOST_LEN 256

/**
 * @brief Parsed command-line arguments representing process group topology.
 */
struct pg_args {
    int myindex_raw;                            /**< 1-based index from command line (-myindex) */
    int rank;                                   /**< 0-based rank: (myindex_raw - 1) */
    int size;                                   /**< Total number of processes in the ring (-list) */
    char hosts[PG_MAX_RANKS][PG_MAX_HOST_LEN];  /**< Hostnames ordered by rank [0 .. size-1] */
};

/**
 * @brief Global CLI arguments populated by the application before connect_process_group.
 */
extern struct pg_args g_pg_args;

/* ========================================================================= */
/* === PUBLIC API SPECIFICATION (assignment.txt)                         === */
/* ========================================================================= */

/**
 * @brief Initializes the RDMA process group and establishes the communication ring.
 *
 * Establishes and verifies the RC ring. The process group is single-threaded and
 * blocking: every rank must call collectives in the same order, without overlap,
 * skipped calls, or retry on an existing connection.
 *
 * @param servername Hostname or server identifier of the process group coordinator.
 * @param pg_handle  Output pointer to receive the opaque process group handle.
 * @return PG_SUCCESS on success, or negative PG_ERR_* code on failure.
 */
int connect_process_group(char *servername, void **pg_handle);

/**
 * @brief Performs a ring-based pipelined Reduce-Scatter collective across all ranks.
 *
 * Reduces the input @p sendbuf array of @p count elements across all ranks using
 * operation @p op, distributing the globally reduced segments among the ranks.
 * Supports arbitrary (non-divisible) buffer counts via MPI-style (Q+1)/Q remainder
 * distribution. Employs pipelined micro-chunks (adaptive 64 KiB / 256 KiB) to overlap
 * network data transfer with SSE4.2 SIMD compute kernels.
 * @p sendbuf and @p recvbuf must refer to disjoint ranges. In the default
 * WORKBUFFER=inplace build, @p sendbuf is mutable scratch and is modified;
 * WORKBUFFER=safe preserves it.
 *
 * @param sendbuf   Pointer to local input array (count elements).
 * @param recvbuf   Pointer to output buffer to receive this rank's reduced segment slice.
 * @param count     Total number of elements in the tensor across the ring.
 * @param datatype  Data element type (PG_INT, PG_FLOAT, or PG_DOUBLE).
 * @param op        Reduction arithmetic operator (PG_SUM, PG_MIN, PG_MAX, or PG_PROD).
 * @param pg_handle Opaque process group handle returned by connect_process_group.
 * @return PG_SUCCESS on success, or negative PG_ERR_* code on failure.
 */
int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op,
                      void *pg_handle);

/**
 * @brief Performs a ring-based All-Gather collective across all ranks.
 *
 * Gathers @p count elements contributed by each rank in @p sendbuf so that every
 * rank finishes with the full concatenated result (count * size elements) in @p recvbuf.
 * Utilizes zero-copy direct RDMA Writes into remote target buffers for Rendezvous mode,
 * or 2-SGE scatter-gather sends for sub-threshold Eager mode.
 * The two ranges must be disjoint unless @p sendbuf points exactly at this rank's
 * owned slice inside @p recvbuf. Other full or partial overlap is invalid.
 *
 * @param sendbuf   Pointer to local contribution (count elements).
 * @param recvbuf   Pointer to output buffer (count * size elements).
 * @param count     Number of elements contributed per rank.
 * @param datatype  Data element type (PG_INT, PG_FLOAT, or PG_DOUBLE).
 * @param pg_handle Opaque process group handle returned by connect_process_group.
 * @return PG_SUCCESS on success, or negative PG_ERR_* code on failure.
 */
int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype,
                  void *pg_handle);

/**
 * @brief Performs a full global All-Reduce collective across all ranks.
 *
 * Reduces @p count elements from @p sendbuf using operation @p op, and places the
 * full reduced result into @p recvbuf on all ranks. Reduce-Scatter and All-Gather
 * hand off without a global barrier. Correctness relies on blocking collectives
 * being called in the same order by every rank; per-QP FIFO ordering and segment
 * tags keep early traffic pending until the local phase advances.
 * @p sendbuf and @p recvbuf must refer to disjoint ranges. In the default
 * WORKBUFFER=inplace build, @p sendbuf is mutable scratch and is modified;
 * WORKBUFFER=safe preserves it.
 *
 * @param sendbuf   Pointer to local input array (count elements).
 * @param recvbuf   Pointer to output buffer (count elements).
 * @param count     Total number of elements in the array.
 * @param datatype  Data element type (PG_INT, PG_FLOAT, or PG_DOUBLE).
 * @param op        Reduction arithmetic operator (PG_SUM, PG_MIN, PG_MAX, or PG_PROD).
 * @param pg_handle Opaque process group handle returned by connect_process_group.
 * @return PG_SUCCESS on success, or negative PG_ERR_* code on failure.
 */
int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op,
                  void *pg_handle);

/**
 * @brief Closes the process group and releases all allocated Verbs and memory resources.
 *
 * Every rank must call this after its final collective. A symmetric neighbor ping
 * quiesces each ring edge before local Verbs resources are destroyed.
 *
 * @param pg_handle Opaque process group handle to destroy.
 * @return PG_SUCCESS on success, or negative PG_ERR_* code on failure.
 */
int pg_close(void *pg_handle);

/* ========================================================================= */
/* === TOPOLOGY & METADATA QUERY HELPERS                                 === */
/* ========================================================================= */

/** @brief Returns the 0-based rank of the local process (0 .. size-1). */
int pg_get_rank(void *pg_handle);

/** @brief Returns the total number of ranks in the process group ring. */
int pg_get_size(void *pg_handle);

/** @brief Returns the rank of the predecessor node: (rank - 1 + size) % size. */
int pg_get_prev_rank(void *pg_handle);

/** @brief Returns the rank of the successor node: (rank + 1) % size. */
int pg_get_next_rank(void *pg_handle);

/** @brief Returns the configured hostname for the specified rank. */
const char *pg_get_hostname(void *pg_handle, int rank);

#ifdef __cplusplus
}
#endif

#endif /* PG_H */
