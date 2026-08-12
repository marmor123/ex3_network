# ADR-0002: MR cache and internal buffer-cache / staging lifecycle

## Context
Rendezvous needs remote-writeable MRs for staging / final recvbuf, eager needs pre-posted buffers, plus pipelined micros and 1 GB sweep memory pressure. `bw.c` registers once (1 MB data + 64 B ctrl) and never dregs until close (`bw.c:521,529,994-1000`). Ring has many dynamic buffers (staging per segment, workbuf, eager pool, per-rank recvbuf slices).

## Decision

### MR cache — Lazy cache, dereg at close
- Key = exact `(ptr, len)` tuple. Linear scan small-n (≤32).
- `pg_get_mr(pg, ptr, len, need_remote)`: find exact match; on miss `ibv_reg_mr(pd, ptr, len, LOCAL_WRITE | (need_remote ? REMOTE_WRITE : 0))` append to `cache[]`.
- Flags: staging/eager = `LOCAL_WRITE`; recvbuf/owned segment grant for RDMA_WRITE = `LOCAL_WRITE|REMOTE_WRITE` (like `bw.c:521` server flag `is_server ? REMOTE_WRITE` but per-buffer here).
- No LRU, no mid-life dereg; `pg_close` loops `ibv_dereg_mr` all entries (like `bw_c*` `bw_close_ctx` 989-1001). Rejected LRU (refcount complexity for in-flight staging).

### Staging buffers — Grow-only set
- Per `pg_handle` vector `staging[ring_size]` or single pool sized to `max_seg_bytes` seen so far.
- On first `pg_all_reduce` with `seg_bytes = (count/size)*sizeof(dtype)`: if `seg_bytes > cur_staging_bytes` allocate new larger (`malloc`), keep old in pool for reuse (no free until `pg_close`). Predictable for 64 MB→1 GB sweep.
- Allocation failure at 1 GB → `PG_ERR_NOMEM` or V8 harness skips iteration (summary V8 “skip on allocation failure”). No realloc churn.

### Workbuf — Safe copy + optional inplace
- Default safe: copy `sendbuf` into internal `workbuf` (size `count*sizeof`) so caller can reuse `sendbuf` immediately after call. RS operates on `workbuf` slices.
- Optional flag `PG_FLAG_INPLACE` (future): operate directly on `sendbuf` when caller guarantees stability (avoids copy). `Makefile WORKBUFFER=safe|inplace` selector (summary V9) but safe is default.
- No always-inplace (risk).

### Eager pool — 16 × max(8 KiB, chunk) pre-reg
- Per QP dir pool `[16]` each `buf = malloc(max(PG_EAGER_THRESHOLD 8192, PIPELINE_CHUNK))` registered `LOCAL_WRITE` at `pg_connect` init, posted as `RECV`s (`pg_make_wr(qp, EAGER_RECV)`).
- Send eager `< threshold`: `SEND` 2 SGEs (64 B header + payload copy into next eager buf tail). Recv: `memcpy` eager_buf → final if needed (extra copy accepted for small).
- Rejected dynamic per-send malloc+reg (overhead, not pipelined).

### Staging vs zero-copy transition
- **Bring-up V4:** full-segment staging — `staging[seg]` holds reduced data, `RDMA_WRITE staging[seg] → next staging[seg]` (copy+reduce then WRITE).
- **Perf V7 + AG V5:** zero-copy — `CTS` grants final `recvbuf[recv_origin]` `addr/rkey`, `RDMA_WRITE` directly into final (no extra copy). Same `RTS/CTS/DATA_DONE` flow, but `CTS` addr switches from staging to final. Clear upgrade, keeps bring-up simple (fewer CTS per segment early).

## Consequences
- V4 implements grow-only staging + workbuf copy; V5 perf zero-copy CTS path reuses same `pg_get_mr`.
- V7 pipelined micros reuse staging slots per seg (slot pool if memory pressure later) + eager pool for `<8KiB` micros.
- V8 warmup/benchmark allocates staging/workbuf before timed region, skips if OOM.

## References
- `past_exercise/bw.c:521,529,989-1001`, `summary_of_grilling.txt` V4/V7/V9
- Research branches `research/r1,r2,r3`
