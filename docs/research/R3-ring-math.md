# R3: Ring Collective Math and Pipelining — Research Findings

**Ticket:** [#14 — R3: Research ring math and pipelining — segment ownership and micro-chunk formulas](https://github.com/marmor123/ex3_network/issues/14) · Part of [#1 Map](https://github.com/marmor123/ex3_network/issues/1) · `wayfinder:research`

**Branch:** `research/r3-ring-math` · **Sources:** [`summary_of_grilling.txt`](../summary_of_grilling.txt), [`assignment.txt`](../assignment.txt), [`past_exercise/bw.c`](../past_exercise/bw.c) (+ `bw_template.c` for contrast)

---

## 1. Rank identity and segment ownership

### 1.1 Rank derivation (summary §3)

Command line is the sole source of rank (no MPI, no central bootstrap server):

```
test -myindex 03 -list mlxstud01 mlxstud02 mlxstud03 mlxstud04
  -myindex is 1-based => rank = atoi(myindex) - 1 = 2
  size = len(list) = 4
  prev = (rank - 1 + size) % size = 1
  next = (rank + 1) % size = 3
  local host = list[rank] = mlxstud03
```

`connect_process_group(char *servername, void **pg_handle)` validates `servername == list[rank]` else `PG_ERR_INVAL` (summary §4). Every rank listens on `PG_TCP_BASE_PORT + rank` (default 19000, [summary §5; bw.c `HANDSHAKE_PORT 18515` is the single-edge predecessor](https://github.com/marmor123/ex3_network/issues/12)).

### 1.2 Ownership convention (summary §11 — corrected)

> **Rank r owns segment r.** — N segments total, contiguous, equal-sized in the divisible bring-up case.

```
N            = size
segment_size = count / N          (elements per segment)
segment_bytes = segment_size * sizeof(DATATYPE)
```

Ownership makes `pg_all_gather` and the AG phase of `pg_all_reduce` natural: the helper assumes "this rank contributes segment rank" (summary §13) and copies it locally before any network step.

Diagram (N=4, count=16, PG_INT):

```
sendbuf / work buffer:  [ seg0 | seg1 | seg2 | seg3 ]
                          r0     r1     r2     r3      <- owner
element indices:        0..3   4..7   8..11  12..15
```

---

## 2. Count divisibility and error contract

### 2.1 Bring-up rule (summary §§11, 31)

```c
if (count % size != 0) return PG_ERR_INVAL;   // pg_all_reduce, pg_reduce_scatter
```

Applies to `pg_all_reduce` and `pg_reduce_scatter` only. Remainder support, non-divisible handling, and pipeline-remainder for arbitrary counts are **explicitly deferred** past bring-up. Bring-up test matrix uses `count = size * k` with `k ∈ {1, 4, 1024, 65536}` (summary §28) so the error path is still exercised with e.g. `count = size+1`.

Error codes (summary §26):

```c
enum { PG_SUCCESS=0, PG_ERR_INVAL=-1, PG_ERR_NOMEM=-2, PG_ERR_RDMA=-3,
       PG_ERR_TCP=-4, PG_ERR_TIMEOUT=-5, PG_ERR_UNSUPPORTED=-6 };
```

### 2.2 What `past_exercise/bw.c` teaches about counts

`bw.c` is point-to-point, so it has **no** `count % size` check — its counts are the fixed 21-entry `MSG_COUNTS[21]` sweep table (`bw.c:77-83`) covering `1 B .. 1 MB`. The lesson for the ring is what to **add**:

| bw.c pattern | Ring adaptation |
|---|---|
| `MSG_COUNTS[seq]` per `size=1<<seq` — throughput convergence table | Ring collective `count` is caller-supplied elements, not bytes; derive bytes via `sizeof(DATATYPE)` |
| Buffer is always `1 MB` single VA, all RDMA_WRITEs target same `dest->buf_addr` | Ring targets **offset** `segment * segment_bytes + micro * PIPELINE_CHUNK` inside `staging` or `recvbuf` |
| No divisibility guard needed (single peer) | Must guard `count % N` at API entry, before any MR registration |
| Benchmark rounds `count = bytes/sizeof(int)` up to next `count%N==0` (summary §29) | Same rounding applies to sweep harness, not to correctness API |

Source citations: `bw.c:77-83` counts, `bw.c:112-115` fixed `WINDOW 256 / SIGNAL_INTERVAL 64 / HANDSHAKE_PORT`, vs. ring `PG_TCP_BASE_PORT 19000` (summary §5).

---

## 3. Reduce-Scatter ring math

### 3.1 Formula (summary §12 — settled)

```
for i = 0 .. N-2:
    send_seg = (rank - i - 1 + N) % N
    recv_seg = (rank - i - 2 + N) % N

    send working[send_seg] to next   (qp_to_next)
    recv from prev into staging
    reduce: working[recv_seg] = op(working[recv_seg], staging)
```

Invariant after step i: rank `r` has reduced `i+1` remote contributions into `recv_seg`. After `N-1` steps:

```
owned = rank        // working[rank] is fully reduced
```

All other segments have been forwarded and can be discarded. The `working` buffer is either an internal safe copy of `sendbuf` (`WORKBUFFER=safe`, default) or `sendbuf` itself (`WORKBUFFER=inplace` — caller must refill before next call, summary §16).

### 3.2 Why these offsets

The `(rank - i - 1)` / `(rank - i - 2)` pattern is a **distance-from-owner** rotation. At `i=0`, each rank sends the segment *one step behind* it and receives the segment *two steps behind* — so the reduction wave moves clockwise, one hop per round, and the owned segment arrives last. `+N) % N` keeps the modulo non-negative in C (identical to `prev/next` derivation).

### 3.3 Count semantics

```
pg_reduce_scatter(sendbuf, recvbuf, count):
    sendbuf: count elements (N segments)
    recvbuf: count / N elements  (== one segment, the owned one)
```

Caller allocates `recvbuf` for exactly `segment_size` elements. Implementation copies `working[rank]` → `recvbuf` after the last reduce, or exposes `working[rank]` aliased.

---

## 4. All-Gather ring math

### 4.1 Formula (summary §13 — settled)

```
recvbuf[rank] = owned reduced segment        // local copy, zero network

for i = 0 .. N-2:
    send_origin = (rank - i + N) % N
    recv_origin = (rank - i - 1 + N) % N

    send segment send_origin to next
    recv segment recv_origin from prev into recvbuf[recv_origin]
```

A **shared helper** implements this for both `public pg_all_gather()` and `internal pg_all_reduce()` (summary §13). It assumes the caller's contribution is `segment rank` — in `pg_all_reduce` that contribution is the RS-owned segment.

### 4.2 Zero-copy detail (summary §§19-20, assignment.txt:18)

Each AG segment uses **RDMA_WRITE directly into final `recvbuf[recv_origin]`** — no staging copy on the receiver data path:

```
sender: RTS(segment = send_origin)  ──SEND──>  next
next:   CTS(addr = recvbuf[recv_origin], rkey, len = segment_bytes) ──SEND──> sender
sender: RDMA_WRITE(s) pipelined into remote recvbuf[recv_origin]
        ──(windowed selective signaling, drain)──>
sender: DATA_DONE  ──SEND──>  next       // one per segment (see §6.2)
```

This satisfies assignment requirement "Use RDMA Read or RDMA Write for large messages zero copy on the all-gather phase" (crossed-out `RDMA_WRITE_WITH_IMM` ⇒ completion via `DATA_DONE` SEND, summary §1/§4).

### 4.3 Count semantics

```
pg_all_gather(sendbuf, recvbuf, count):
    sendbuf: count elements per rank   (== one segment)
    recvbuf: count * N elements        (== N segments)

pg_all_reduce(sendbuf, recvbuf, count):
    sendbuf: count elements
    recvbuf: count elements
```

The helper's `count` is the per-rank contribution (same `count` as `pg_all_gather`).

---

## 5. All-Reduce composition and barrier

### 5.1 Three-phase structure (summary §§22, 14)

```
pg_all_reduce =  reduce_scatter (N-1 rounds)
               +  barrier (N-1 rounds)         // distributed ring, not TCP
               +  all_gather  (N-1 rounds)
```

Total ring steps: `3*(N-1)` logical rounds, plus CTS/DATA_DONE control (see §6).

### 5.2 Barrier (summary §22)

Distributed `N-1`-round ring barrier, riding the same control QP/pool and progress engine as data:

```
for round = 0 .. N-2:
    send BARRIER(phase, round) to next   (qp_to_next, SEND, 64 B pg_ctrl_msg)
    recv BARRIER(phase, round) from prev (qp_from_prev)
```

Used between RS→AG inside `pg_all_reduce`, and inside `pg_close()` before destroying QPs/CQ/PD (prevents use-after-free of in-flight operations). Replaces the earlier rank-0-rooted barrier idea.

### 5.3 Why the barrier is required

Without it, a fast rank's AG RTS could race the slow rank's still-in-flight RS RDMA_WRITE targeting the same internal buffer. The barrier guarantees **all ranks have finished RS** (and thus no more writes to staging/work) before any rank publishes its owned segment via AG.

---

## 6. Pipelining

### 6.1 Segment-first packetization (summary §21)

```
1. divide count elements into N ring segments  (segment_bytes)
2. divide each segment into PIPELINE_CHUNK micros
3. last micro of each segment may be smaller:  last_bytes = segment_bytes % PIPELINE_CHUNK (or PIPELINE_CHUNK if divisible)
```

This is **segment-first** (not a flat byte stream) so per-segment CTS/DATA_DONE accounting stays aligned with ring formulas. Each micro is addressed by offset, not by separate buffer:

```
staging_base + micro_index * PIPELINE_CHUNK            // RS staging (summary §18)
recvbuf[recv_origin] + micro_index * PIPELINE_CHUNK     // AG final placement (summary §19)
```

> "Do not allocate a separate buffer per micro-chunk. Use one contiguous segment-sized staging buffer. Each micro-chunk is addressed by offset." (summary §18)

### 6.2 Chunk sizes and window constants (summary §§20-21, 24-25)

| Profile | `PG_PIPELINE_CHUNK` | `PG_RDMA_WINDOW` | `PG_RDMA_SIGNAL_INTERVAL` | DATA_DONE (RS) | DATA_DONE (AG) |
|---|---|---|---|---|---|
| `PROFILE=bringup` | 64 KiB | 1 | 1 | per micro-chunk | per segment |
| `PROFILE=perf` | 256 KiB | 32 | 8 | per micro-chunk | per segment |

Makefile selectors (summary §24): `make PROFILE=bringup` / `make PROFILE=perf`; defaults to `perf` (`PG_PIPELINE_CHUNK=256 KiB`, summary §25). The bring-up profile is for deterministic bringup (V2-V6); perf enables windowing (V7-V8).

`bw.c` precedent: `WINDOW 256 / SIGNAL_INTERVAL 64` with `CQ depth W+K=320` (bw.c:112-113, 506), `K<=W` compile-time assert (bw.c:121). Ring scales this to `32/8` for perf pipelined segments — same selective-signaling principle: signal every K-th WRITE and always signal the final WRITE, drain before DATA_DONE.

### 6.3 Per-micro CTS/DATA_DONE flow

**Non-pipelined** (V4-V6): one CTS per segment, one DATA_DONE per segment, sequential `RTS→CTS→WRITE→DATA_DONE`.

**Pipelined** (V7, summary §§18-20):

```
RS (pipelined, per micro):
    receiver:  one CTS granting whole staging segment (addr, rkey, len=segment_bytes)
    sender:    for each micro k:
                 RDMA_WRITE(staging_base + k*CHUNK, len_k)   // windowed, signaled
                 DATA_DONE(micro=k)  ──SEND──> receiver
    receiver:  on each DATA_DONE(k): reduce staging[k] into working[recv_seg][k]
               // NIC can be writing micro k+1 while CPU reduces micro k
               // (receive-while-reduce, summary §18)

AG (pipelined, per segment DATA_DONE, windowed writes):
    receiver:  one CTS granting final recvbuf[recv_origin]
    sender:    for each micro k:
                 RDMA_WRITE(recvbuf[recv_origin] + k*CHUNK, len_k)  // windowed, selective signal
               drain all WRITEs for segment
               DATA_DONE(segment)  ──SEND──>
```

RS uses **per-micro DATA_DONE** so the receiver can reduce micro-by-micro without waiting for the whole segment (summary §18: "one DATA_DONE per micro-chunk — NIC writes k+1 while CPU reduces k"). AG uses **per-segment DATA_DONE** (summary §19) — no reduction, so micro-level completion is unnecessary; the windowed WRITEs are drained before the single DATA_DONE.

Control message budget (per rank, per collective):

```
RS:  (N-1) segments × ceil(segment_bytes / PIPELINE_CHUNK) CTS+DATA_DONE pairs
AG:  (N-1) segments × 1 CTS + 1 DATA_DONE              (writes windowed internally)
```

### 6.4 Staging and receive-while-reduce (summary §18)

- One **contiguous full-segment staging buffer** per rank, cached inside `pg_handle`, grow-only (like `bw.c`'s `buf` and MR cache, but sized to `segment_bytes`).
- Grows if a later collective's segment is larger; never shrinks.
- **Double full-segment staging** for cross-segment overlap is **deferred** (summary §31) — bring-up uses one staging buffer; V7 overlaps within a segment (micro k vs k+1), not across segments.
- Each micro WRITE targets `staging_base + micro*CHUNK`; receiver reduces completed micros incrementally. Progress engine (summary §23) must interleave CQ polling, newly-granted WRITEs, completed-WRITE processing, per-micro reduces, and control reposts without blocking on any single event.

### 6.5 Signaling summary (summary §20)

```
RS: each micro-chunk WRITE is signaled            // sender can track per-micro completion for DATA_DONE
AG: bounded window (32), signal every 8, always signal final WRITE, drain writes before DATA_DONE
        // matches bw.c selective signaling: only K-th and final signaled (bw.c:847)
bringup: window 1 / interval 1  => every WRITE signaled, simplest drain logic
```

---

## 7. Validation examples

Python-validated against the settled formulas (`+N) % N` to match C unsigned modulo). Formula coverage: after `N-1` RS rounds, last `recv_seg == rank`; after `N-1` AG rounds, `{rank} ∪ {recv_origins} == {0..N-1}`.

### 7.1 N=2 — minimal ring (summary scope requires testing with 2 and 4)

**Reduce-Scatter (1 round, i=0):**

| rank | `send_seg=(r-1)%2` | `recv_seg=(r-2)%2` | Action | Result |
|---|---|---|---|---|
| 0 | 1 | 0 | send seg1 → rank1; recv staged seg0, reduce into working[0] | owns seg0 |
| 1 | 0 | 1 | send seg0 → rank0; recv staged seg1, reduce into working[1] | owns seg1 |

Each rank sends the **non-owned** segment and receives its **owned** segment — the only possible exchange.

**All-Gather (1 round, i=0):** `recvbuf[rank]=owned` pre-copied.

| rank | `send_origin=(r)%2` | `recv_origin=(r-1)%2` | Action | Final recvbuf |
|---|---|---|---|---|
| 0 | 0 | 1 | send seg0 → rank1; recv seg1 ← rank1 into recvbuf[1] | [seg0, seg1] |
| 1 | 1 | 0 | send seg1 → rank0; recv seg0 ← rank0 into recvbuf[0] | [seg0, seg1] |

After 1 round both ranks hold `[seg0, seg1]` — which is `expected[i] = count*size*(size-1)/2 + size*i` for `PG_INT+PG_SUM` (summary §28). For `count=8, size=2` with `sendbuf[r][i]=r*8+i`, reduced seg0 = `i + (8+i)=8+2i` → checking: `expected[i]=8*2*1/2+2*i=8+2i` ✓.

**Barrier:** 1 round `BARRIER(phase,0)` around ring.

### 7.2 N=4 — full coverage (exhaustive per-rank trace)

**Reduce-Scatter — 3 rounds:**

| rank | i=0 send→recv | i=1 send→recv | i=2 send→recv | Last recv==rank? | Owned |
|---|---|---|---|---|---|
| 0 | 3→2 | 2→1 | 1→0 | 0 ✓ | seg0 |
| 1 | 0→3 | 3→2 | 2→1 | 1 ✓ | seg1 |
| 2 | 1→0 | 0→3 | 3→2 | 2 ✓ | seg2 |
| 3 | 2→1 | 1→0 | 0→3 | 3 ✓ | seg3 |

Expanded for rank 0 (illustrative data flow):

```
Initial working[0..3] = [a0,a1,a2,a3] (segments of sendbuf)
i0: send a3 to rank1,  recv b2 from rank3,  reduce working[2] = a2 ⊕ b2
i1: send working[2] (reduced) to rank1, recv c1 from rank3, reduce working[1] = a1 ⊕ c1
i2: send working[1] (reduced) to rank1, recv d0 from rank3, reduce working[0] = a0 ⊕ d0  → owned
```

After i2, rank 0's `working[0]` has contributions from all 4 ranks summed (for PG_SUM). Symmetric for other ranks — each rank's owned segment accumulates `N` contributions.

**All-Gather — 3 rounds:** (`recvbuf[rank]=owned` pre-copied, `S`/`R` = send/recv origin)

| rank | i=0 S→R | i=1 S→R | i=2 S→R | Final recvbuf segments |
|---|---|---|---|---|
| 0 | 0→3 | 3→2 | 2→1 | {0}∪{3,2,1}=[0,1,2,3] ✓ |
| 1 | 1→0 | 0→3 | 3→2 | {1}∪{0,3,2}=[0,1,2,3] ✓ |
| 2 | 2→1 | 1→0 | 0→3 | {2}∪{1,0,3}=[0,1,2,3] ✓ |
| 3 | 3→2 | 2→1 | 1→0 | {3}∪{2,1,0}=[0,1,2,3] ✓ |

Detailed for rank 1:

```
Initial: recvbuf[1]=owned seg1
i0: send seg1 → rank2, recv seg0 ← rank0 into recvbuf[0]
i1: send seg0 (just received) → rank2, recv seg3 ← rank0 into recvbuf[3]
i2: send seg3 → rank2, recv seg2 ← rank0 into recvbuf[2]
Result: recvbuf = [seg0, seg1, seg2, seg3]  (seg0=origin0, seg1=own, seg2,seg3 from ring)
```

**All-Reduce composition (N=4):**

```
RS 3 rounds  → barrier 3 rounds  → AG 3 rounds  = 9 logical ring hops
Per rank: RS data moved 3 segments out + 3 in (reduced)
          AG data moved 3 segments out + 3 in (placed into recvbuf)
          barrier 3 SENDs out + 3 in
```

**Micro-chunk examples (segment-first, §6):**

| segment_bytes | PIPELINE_CHUNK | micros | last micro bytes |
|---|---|---|---|
| 64 KiB | 64 KiB (bringup) | 1 | 64 KiB |
| 256 KiB | 64 KiB (bringup) | 4 | 64 KiB |
| 256 KiB | 256 KiB (perf) | 1 | 256 KiB |
| 500 000 B | 65 536 B | 8 | 41 248 B (remainder) |
| 500 000 B | 262 144 B | 2 | 237 856 B (remainder) |

Last micro is `segment_bytes - (micros-1)*CHUNK` and CTS `len` reflects that actual length.

---

## 8. Edge cases, remainder handling, and failure modes

| Case | Behavior | Source |
|---|---|---|
| `count % size != 0` | `return PG_ERR_INVAL` at API entry, no network | summary §11 |
| `count == 0` | Degenerate: by formula `count%N==0` vacuously, but `0` elements ⇒ no segments; treat as `PG_ERR_INVAL` or `PG_SUCCESS` no-op (implementation must choose and document; tests avoid it) | Inferred |
| `size == 1` | Single process: `N-1=0` rounds, all loops skipped; `pg_all_reduce` is `memcpy(recvbuf, sendbuf)` + op applied locally | Inferred from formulas |
| `segment_bytes < PIPELINE_CHUNK` | 1 micro, `len = segment_bytes` | summary §21 |
| `segment_bytes % PIPELINE_CHUNK != 0` | Last micro smaller; sender's final `RDMA_WRITE` and CTS `len` use actual remainder | summary §21 |
| Internal buffer / staging too small | Grow-only reallocation inside `pg_handle` (like bw.c MR cache but for internal buffers, summary §17) | summary §17 |
| User buffer not yet registered | MR cache miss: `ibv_reg_mr` on first use, keyed by `(ptr, len)` | summary §17 |
| Unsupported `DATATYPE`/`OPERATION` | `return PG_ERR_UNSUPPORTED` except `PG_INT+PG_SUM` | summary §15 |
| Allocation failure at 1 GB sweep | Print warning, skip that size, continue (benchmark harness) | summary §29 |
| `servername != list[rank]` | `return PG_ERR_INVAL` | summary §4 |
| `WORKBUFFER=inplace` | `sendbuf` is clobbered; caller must refill outside timed region | summary §16 |

Deferred / not implemented in bring-up (summary §31): RoCE/GID, RDMA Read data path, non-divisible count remainder, double full-segment staging, `PG_MODE_AUTO`, slot-pool fallback.

---

## 9. Implementation checklist for V4-V7

Derived from this research; maps directly to vertical milestones (summary §30):

- [ ] **V4 RS bring-up:** validate `count%N`, safe work buffer copy, per-QP control pools reposted, formula `send=(r-i-1)%N recv=(r-i-2)%N`, `N-1` rounds, full-segment staging grow-only.
- [ ] **V5 AG bring-up:** `recvbuf[rank]=owned`, formula `send=(r-i)%N recv=(r-i-1)%N`, shared helper, final-placement CTS, one DATA_DONE/segment, windowed WRITEs (window 1).
- [ ] **V6 AR wiring:** `RS → barrier(N-1) → AG`, barrier riding control pool, `pg_close` barrier, count-semantics dispatch.
- [ ] **V7 pipelining:** `PROFILE` switch (64 KiB/1/1 vs 256 KiB/32/8), segment-first `ceil(seg/CHUNK)` micros with offset arithmetic, staged `staging_base+k*CHUNK`, RS per-micro DATA_DONE + receive-while-reduce overlap, progress engine non-blocking poll.

---

## 10. References

- `summary_of_grilling.txt` — §§11-14 (ownership, RS/AG formulas, count semantics), §§18-22 (staging, zero-copy, signaling, chunking, barrier), §§24-25 (profiles/defaults), §28 (correctness harness), §29 (benchmark harness). These are the **settled shared understanding**; conflicts resolve in favor of this document.
- `assignment.txt` — ring topology, collectives `reduce-scatter / all-gather / all-reduce`, pipelining overlap requirement, RDMA Write zero-copy for AG, crossed-out `RDMA_WRITE_WITH_IMM` (use `IBV_WR_SEND` for completion), test with 2 and 4.
- `past_exercise/bw.c` — `MSG_COUNTS` convergence table, `WINDOW 256 / SIGNAL_INTERVAL 64`, control pool `32 × 64 B` never refreshed, inline stepdown `1024→64`, read-back via `ibv_query_qp`, selective signaling (only K-th + final), `W+K` CQ depth, handshake `DEST_FMT` wire format. Contrasts noted above. See also [R1 findings](R1-verbs-patterns.md).
- Lecture #2 / [R2 findings](R2-eager-rendezvous.md) — eager vs rendezvous thresholding and why AG zero-copy favors rendezvous for large messages.

---

*Validated: formulas checked for N=2,4 all ranks — RS last recv==rank, AG final segments==[0..N-1]. Micro arithmetic checked for remainder cases above.*
