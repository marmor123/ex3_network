# ADR-0002: MR Cache and Internal Buffer Lifecycle

## Context
RDMA operations require memory regions (MRs) registered with `libibverbs` (`ibv_reg_mr`). Registering memory on the hot path introduces high latency overhead. Rendezvous RDMA Write requires remote write permissions on target buffers, while safe out-of-place collective reduction requires temporary work buffers.

## Decision

### 1. Lazy MR Registration Cache
- We implement `pg_get_or_reg_mr(ctx, addr, length, access_flags)` with a fixed-size cache (`PG_MR_CACHE_MAX = 1024`).
- On collective invocation, the cache is scanned for an existing MR covering the `[addr, addr + length)` span with matching access flags.
- On miss, `ibv_reg_mr` is invoked with required permissions (`IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE`) and added to the cache.
- All registered MRs persist for the lifetime of the process group and are cleanly deregistered in `pg_close`.

### 2. Grow-Only Staging Buffer Allocation & 2 MB Hugepage Alignment
- For Reduce-Scatter and intermediate staging, a staging buffer (`ctx->staging_buf`) is allocated using `posix_memalign`.
- For buffers $\ge 2\text{ MB}$, allocations are aligned to 2 MB boundaries and backed by `madvise(..., MADV_HUGEPAGE)` to minimize D-TLB misses during streaming transfers. Buffers $< 2\text{ MB}$ use 64-byte cache line alignment.
- If a collective requires a larger segment capacity than currently allocated, the old buffer is freed and a larger one is registered. No buffer reallocations occur if payload sizes stay within prior maximums.

### 3. Safe vs Inplace Work Buffer
- **Safe Mode** (`WORKBUFFER=safe`): `sendbuf` is copied into an internal working buffer `ctx->work_buf` before reduction, ensuring the caller's `sendbuf` remains unmodified throughout the collective. Uses 2 MB hugepage alignment when $\ge 2\text{ MB}$.
- **Inplace Mode** (`WORKBUFFER=inplace`, default): Operates directly on the registered `sendbuf` memory for zero-copy in-memory reductions when the caller permits mutation.

## Consequences
- Eliminates repeated `ibv_reg_mr` / `ibv_dereg_mr` driver calls in timed benchmark sweeps.
- Cuts TLB miss penalties on large streaming payloads (64 MiB–1 GiB), sustaining 20.87–21.61 Gbps peak line-rate throughput.
- Predictable memory footprint with zero memory leaks.

## References
- `pg.c` (Module 3: Memory Registration & Staging Cache).
- `CONTEXT.md` (Memory Registration Invariant).
- Commit `76f1fb2` (Ticket #15).
