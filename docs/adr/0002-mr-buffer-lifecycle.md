# ADR-0002: MR Cache and Internal Buffer Lifecycle

## Context
RDMA operations require memory regions (MRs) registered with `libibverbs` (`ibv_reg_mr`). Registering memory on the hot path introduces high latency overhead. Rendezvous RDMA Write requires remote write permissions on target buffers, while safe out-of-place collective reduction requires temporary work buffers.

## Decision

### 1. Lazy MR Registration Cache
- We implement `pg_get_or_reg_mr(ctx, addr, length, access_flags)` with a fixed-size cache (`PG_MR_CACHE_MAX = 1024`).
- On collective invocation, the cache is scanned for an existing MR covering the `[addr, addr + length)` span with matching access flags.
- On miss, `ibv_reg_mr` is invoked with required permissions (`IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE`) and added to the cache.
- All registered MRs persist for the lifetime of the process group and are cleanly deregistered in `pg_close`.

### 2. Grow-Only Staging Buffer Allocation
- For Reduce-Scatter and intermediate staging, a 64-byte cacheline-aligned staging buffer (`ctx->staging_buf`) is allocated using `posix_memalign`.
- If a collective requires a larger segment capacity than currently allocated, the old buffer is freed and a larger one is registered. No buffer reallocations occur if payload sizes stay within prior maximums.

### 3. Safe vs Inplace Work Buffer
- **Safe Mode** (`WORKBUFFER=safe`, default): `sendbuf` is copied into an internal 64-byte aligned working buffer `ctx->work_buf` before reduction, ensuring the caller's `sendbuf` remains unmodified throughout the collective.
- **Inplace Mode** (`WORKBUFFER=inplace`): Operates directly on the registered `sendbuf` memory for zero-copy in-memory reductions when the caller permits mutation.

## Consequences
- Eliminates repeated `ibv_reg_mr` / `ibv_dereg_mr` driver calls in timed benchmark sweeps.
- Predictable memory footprint with zero memory leaks.

## References
- `pg.c` (Module 3: Memory Registration & Staging Cache).
- `CONTEXT.md` (Memory Registration Invariant).
