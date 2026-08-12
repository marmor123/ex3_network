# R2: Eager vs Rendezvous (Lecture #2) + RDMA Write/Read Zero-Copy for All-Gather

## Sources

Primary sources examined for this research:

| Source | What it contributed |
|---|---|
| `assignment.txt:1-18` | Eager vs rendezvous comparison ask; pipelining; RDMA Read/Write zero-copy for all-gather; crossed-out `RDMA_WRITE_WITH_IMM` |
| `summary_of_grilling.txt` §§4, 8-10, 18-21, 24-25 | Settled `PG_CTRL_MSG` layout, RTS/CTS/DATA_DONE/BARRIER types, eager 2-SGE design, threshold `8 KiB`, pipelining, window constants |
| `past_exercise/bw.c` (all sections, esp. `bw_post_ctrl_send:644-677`, `bw_post_control_recvs:617-638`, `bw_post_writes:827-878`, QP init `552-590`) | Control SEND path (signaled + inline), 32-deep pool, linked-list WRITEs, max_inline stepping, CQ poll |
| `past_exercise/bw_template.c` (`pp_connect_ctx:131-186`, `pp_init_ctx:360-457`) | Baseline RC QP transitions, GID handling stripped in bw.c |
| `chat-Final_Project_Planning.txt` §§3-5 | Ring reduce-scatter / all-gather decomposition |

Ticket: #13 (`wayfinder:research`). Map: #1.

---

## 1. Lecture #2 framing — eager vs rendezvous

Lecture #2 defines two message-passing protocols by **when the receiver grants resources**:

```
Eager (push, optimistic)          Rendezvous (handshake, pessimistic)
─────────────────────             ─────────────────────
Sender assumes receiver           Sender asks, receiver grants,
already has a buffer.             then sender moves data.
Data moves in one RTT.            Data moves in 1.5-2 RTTs.
No address exchange.              Requires addr/rkey/len exchange.
Copies into pre-posted buffer.    Zero-copy directly to final VA.
Good for small messages.          Good for large messages.
Bounded by recv-buffer budget.    Unbounded per-transfer size.
```

The assignment asks the implementation to **implement both** and compare them on the
ring collectives, exactly to reproduce the classic crossover curve where eager wins
below a threshold and rendezvous wins above it.

---

## 2. Eager protocol — `IBV_WR_SEND` with 2 SGEs into pre-posted receives

### Verbs shape (from `summary_of_grilling.txt` §10 + `bw.c` SEND path)

```c
// Eager SEND — one WR, two SGEs
struct ibv_sge sge[2];
sge[0].addr   = (uint64_t) &ctrl_hdr;   // 64 B pg_ctrl_msg (type=EAGER_PAYLOAD)
sge[0].length = sizeof(struct pg_ctrl_msg); // PG_CTRL_MSG_LEN = 64
sge[0].lkey   = ctrl_mr->lkey;           // or 0 if IBV_SEND_INLINE
sge[1].addr   = (uint64_t) payload;       // segment or micro-chunk bytes
sge[1].length = payload_len;
sge[1].lkey   = payload_mr->lkey;

struct ibv_send_wr wr = {
    .opcode     = IBV_WR_SEND,
    .send_flags = IBV_SEND_SIGNALED | (fits_inline ? IBV_SEND_INLINE : 0),
    .sg_list    = sge,
    .num_sge    = 2,
    .wr_id      = pack_qp_and_class(EAGER_SEND),
};
ibv_post_send(qp_to_next, &wr, &bad);
```

Receiver side (pre-posted):

```c
// One eager recv per slot: header + payload contiguous
struct ibv_sge r_sge[2];
r_sge[0].addr = eager_buf[slot];                // 64 B header area
r_sge[1].addr = eager_buf[slot] + 64;           // payload area (size = max(threshold, chunk))
ibv_post_recv(qp_from_prev, &r_wr_with_2_sge, &bad);
 // On completion: validate tag, copy payload into work/final buffer, repost slot
```

Key properties:

| Property | Value |
|---|---|
| Transfer | Single `IBV_WR_SEND`, RC reliable |
| SGEs | 2 — header + payload coalesced in one WQE |
| Receiver cost | Pre-post `PG_EAGER_POOL_DEPTH = 16` recvs (summary §10); each sized `max(PG_EAGER_THRESHOLD, PG_PIPELINE_CHUNK)` so a `MODE=eager` sweep can send pipeline-sized chunks |
| Buffer exhaustion | If sender outruns receiver, RNR NAK (`min_rnr_timer=12`, `rnr_retry=7`); flow-controlled but still requires enough recvs for the pipeline depth |
| Extra copy | Receiver must `memcpy` from eager buffer into final `recvbuf[origin]` or reduction staging — this copy is the eager penalty at large sizes |
| Inline | Header always fits inline (64 B < `max_inline_data`); payload inline only when `len <= max_inline_data` (bw.c uses `size<=64 && <=max_inline` at `bw.c:833`) |

### Relation to `bw.c` control SEND path

`bw.c:644-677` (`bw_post_ctrl_send`) is the exact template reused:

* Single SGE, `IBV_SEND_SIGNALED`, conditional `IBV_SEND_INLINE` when `msg_len <= max_inline_data`, otherwise `memcpy` into registered `ctrl_buf` and DMA from there.
* `bw.c:617-638` (`bw_post_control_recvs`) posts 32 recvs once, never refreshed. The ring **differs**: both QPs share a CQ (`bw.c:536`), but ring needs per-QP control pools (`PG_CTRL_POOL_DEPTH=32` each) that are **reposted after consumption** because pipelined rendezvous + barriers generate >32 control messages (summary §8).

Eager benchmark mode reuses the same QP/CQ/pool machinery but with the 2-SGE WR above; completion classes are bit-packed into `wr_id` (summary §7: `eager send`, `eager receive` classes).

---

## 3. Rendezvous protocol — RTS / CTS / RDMA_WRITE / DATA_DONE

### Four-step handshake

```
Rank r (sender)                          Rank r+1 (receiver)
──────────────                           ──────────────────
                                          
  RTS  ─────────────────────────►         
  (type=RTS, phase/step/segment,         
   len, tag=PG_TAG)  — IBV_WR_SEND 64 B  
   on qp_to_next                         
                                         
                                          ◄──────────────── CTS
                                            (type=CTS, addr, rkey, len,
                                             credit) — IBV_WR_SEND 64 B
                                            grants ONE region:
                                              RS → staging buffer
                                              AG → recvbuf[recv_origin]
  RDMA_WRITE ────────────────────►        
  (IBV_WR_RDMA_WRITE,                     NIC DMAs directly into granted VA
   remote_addr = CTS.addr,                
   rkey = CTS.rkey, len = segment/micro)  
                                         
  DATA_DONE ─────────────────────►        
  (type=DATA_DONE, segment/micro)         
   — IBV_WR_SEND 64 B  (completion signal)
                                          reposts CTS recv, validates tag,
                                          progresses (reduce or forward)
```

Per summary §§18-20:

* **Reduce-Scatter**: receiver CTS grants the full-segment staging buffer (one contiguous allocation, grow-only), sender writes each micro at `staging_base + micro * PG_PIPELINE_CHUNK`, then **one `DATA_DONE` per micro-chunk** so the receiver can reduce micro `k` while the NIC writes micro `k+1` (receive-while-reduce).
* **All-Gather**: receiver CTS grants `recvbuf + recv_origin * segment_bytes` (final placement), sender pipelines RDMA_WRITE micros with windowed selective signaling, drains, then **one `DATA_DONE` per segment** (no per-micro completion needed because there is no reduction).

### Verbs details

```c
// RTS / CTS / DATA_DONE — all 64 B control SENDs on the data QP they belong to
// (summary §8: control for edge r→next rides qp_to_next)
struct pg_ctrl_msg {           // 64 B — summary §8
    uint32_t tag;    // PG_TAG = 0x50475244
    uint32_t type;   // RTS / CTS / DATA_DONE / BARRIER / EAGER_PAYLOAD
    uint32_t phase, step, segment, micro, slot, flags;
    uint64_t len;    // bytes granted / to write
    uint64_t addr;   // remote VA (CTS) or local (RTS diagnostic)
    uint32_t rkey, credit;
    uint32_t pad[2];
};

// RDMA_WRITE — zero-copy data path
struct ibv_sge sge = { .addr=(uint64_t)src, .length=len, .lkey=src_mr->lkey };
struct ibv_send_wr wr = {
    .opcode = IBV_WR_RDMA_WRITE,
    .sg_list=&sge, .num_sge=1,
    .wr.rdma.remote_addr = cts.addr,
    .wr.rdma.rkey        = cts.rkey,
    .send_flags = signaled ? IBV_SEND_SIGNALED : 0,
};
```

QP access: `IBV_ACCESS_REMOTE_WRITE` only (summary §6). If `RDMA_READ` were added, flags must be reopened to `REMOTE_READ`.

### Why `RDMA_WRITE_WITH_IMM` was crossed out

`assignment.txt:18` originally listed `RDMA_WRITE_WITH_IMM`; that line is **crossed out** in the scanned assignment. Consequences (summary §4 settled):

* `IBV_WR_RDMA_WRITE_WITH_IMM` would piggyback a 4 B immediate as a receive completion on the remote side, saving one SEND.
* With it forbidden, **completion must be explicit**: sender posts a normal `IBV_WR_SEND` (`DATA_DONE`) after the WRITE, receiver polls the shared CQ for that SEND's recv completion.
* This matches `bw.c`'s separation: data WRITEs are `IBV_WR_RDMA_WRITE` (`bw.c:858`), control is always `IBV_WR_SEND` (`bw.c:654`). The ring control similarly "rides the data QP" via `IBV_WR_SEND` (summary §8).
* Benefit of separate `DATA_DONE`: carry richer metadata (segment, micro, phase, slot) beyond 4 B imm, and keep the wire protocol uniform with RTS/CTS/BARRIER (all 64 B SENDs).

---

## 4. All-gather zero-copy — CTS-granted `recvbuf` region

Assignment requirement (`assignment.txt:18`):

> Use RDMA Read or RDMA Write for large messages zero copy on the all-gather phase.

Settled design (summary §19):

```
All-gather step i (rank r):
  send_origin = (rank - i + N) % N
  recv_origin = (rank - i - 1 + N) % N

  1. r sends RTS for segment send_origin to next
  2. next replies CTS { addr = recvbuf + recv_origin*seg_bytes,
                        rkey = recvbuf_mr->rkey, len = seg_bytes }
  3. r RDMA_WRITE-s directly into that remote VA — no staging copy
     (pipelined case: K micros in window, last forced signaled, drain)
  4. r sends DATA_DONE (one per segment)
  5. next validates tag, reposts control recv, can forward that segment in step i+1
```

Without zero-copy, each hop would copy into a staging buffer then `memcpy` to `recvbuf` — two copies per hop (`N-1` times). RDMA_WRITE into the final `recvbuf` eliminates both.

**RDMA_WRITE vs RDMA_READ for all-gather**:

|  | RDMA_WRITE (chosen) | RDMA_READ (deferred, summary §31) |
|---|---|---|
| Initiator | Sender pushes | Receiver pulls (needs RTS with src addr/rkey) |
| Receiver CPU | Passive — NIC places data | Passive but sender must wait for READ to land before use |
| Sender CPU | Active — posts WRITE | Passive after advertising |
| Extra protocol | CTS grants dst | RTS advertises src |
| Default | Yes — matches `bw.c` data path (`IBV_WR_RDMA_WRITE` at `bw.c:858`) | Deferred; would require `IBV_ACCESS_REMOTE_READ` and READ-aware QP attrs |

Zero-copy holds for both; WRITE is preferred because the sender already knows the payload (it owns the segment) and the receiver knows the destination (`recvbuf`).

### Address / rkey dissemination

* TCP bootstrap exchanges only QP metadata (`qpn/psn/lid` via `pg_tcp_qp_info` — summary §5), **not** buffer addresses/rkeys. This is intentional: user `sendbuf`/`recvbuf` addresses are not known at `connect_process_group` time; they are registered on first use via an MR cache keyed by `(ptr, len)` (summary §17) and communicated per-transfer in CTS.
* CTS carries `addr + rkey` for exactly one segment's final region; stale rkeys are impossible because CTS is per-step and the MR remains registered until `pg_close`.

---

## 5. Threshold — `PG_EAGER_THRESHOLD = 8 KiB`

Settled constant (summary §§9, 25):

```c
#define PG_EAGER_THRESHOLD 8192   // 8 KiB
```

Protocol mode selection:

```c
// summary §9 + §25
PG_MODE_RENDEZVOUS_DATA  // default: payload always rendezvous
PG_MODE_EAGER_DATA       // benchmark: payload always eager SEND (header+payload)
PG_MODE_AUTO             // thresholded:
    if (payload_bytes <= PG_EAGER_THRESHOLD) eager SEND (2 SGEs)
    else rendezvous (RTS/CTS/WRITE/DATA_DONE)
```

Rationale:

* Eager cost = `RTT + memcpy(N bytes)` + pre-posted buffer occupancy. Rendezvous cost = `RTT(RTS→CTS) + RTT(WRITE) + RTT(DATA_DONE)` + zero-copy DMA. Crossover is where copy + buffer pressure exceeds extra RTT.
* Classic InfiniBand literature and MPI eager thresholds cluster at 4-16 KiB for RC SEND (UCX, MVAPICH). 8 KiB is a defensible middle for MTU≈4 KiB (summary QP uses `portinfo.active_mtu`) — at 8 KiB the payload spans ~2 MTUs and the header/payload copy already exceeds one extra RTT.
* Benchmark must demonstrate this by rebuilding with `make MODE=rendezvous / eager / auto` and sweeping sizes `size*1 … size*65536` (summary §28) plus the V8 perf sweep `64 MB … 1 GB` (summary §29).

Eager buffer sizing rule (summary §10):

```
eager_recv_buf_size = max(PG_EAGER_THRESHOLD, PG_PIPELINE_CHUNK)
```

Ensures a forced-eager perf run can still send `256 KiB` pipeline chunks as eager payloads; under `PG_EAGER_THRESHOLD` the buffer carries the whole segment, above it carries one micro.

---

## 6. Pipelining — per-micro vs per-segment `DATA_DONE`

### Segment-first chunking (summary §21)

```
message (count * sizeof(T)) → N segments → each segment → micros of PG_PIPELINE_CHUNK
                                                        (last micro may be smaller)
perf: PG_PIPELINE_CHUNK = 256 KiB
bringup: 64 KiB
```

### Reduce-scatter vs all-gather granularity

| Phase | Non-pipelined | Pipelined | Why different |
|---|---|---|---|
| Reduce-Scatter | one RTS→CTS (full segment) → one WRITE → one DATA_DONE per segment | one RTS→CTS (full segment) → per-micro WRITE → **per-micro DATA_DONE** | Receiver must reduce each micro before the next can overwrite staging slack; per-micro completion lets CPU reduce micro `k` while NIC writes micro `k+1` (summary §18) |
| All-Gather | one RTS→CTS (segment in final recvbuf) → one WRITE → one DATA_DONE per segment | one RTS→CTS (segment in final recvbuf) → windowed WRITEs for micros → **one DATA_DONE per segment after drain** | No reduction — micros are placed at `base + micro*chunk`; only segment completion matters (summary §19) |

Bringup aliases (summary §24): `WINDOW=1, SIGNAL_INTERVAL=1` so each WRITE is signaled and completion is deterministic. Perf uses `PG_RDMA_WINDOW=32, SIGNAL_INTERVAL=8` — selective signaling (every K-th WRITE + final forced signaled, same as `bw.c:847`).

### Overlap shape (summary §§18, 20, 23)

```
time ──►
RS micro k:   [ CTS ][ WRITE k ][ DATA_DONE k ][  CPU reduce k ]
RS micro k+1:              [ WRITE k+1 ]  ◄── NIC writes while CPU reduces k
AG micro k:   [ CTS ][ WRITE k ][ WRITE k+1 ] ... [ drain ][ DATA_DONE seg ]
                ▲ window of 32 WRs, signal every 8 (perf)
Progress engine polls shared CQ, advances RTS/CTS/DATA_DONE state,
posts newly granted RDMA_WRITEs, processes completed WRITEs,
reduces completed RS micros, reposts control/eager recvs (summary §23).
All in one thread — no blocking on one event.
```

The existing `bw.c` pipeline (`WINDOW=256, K=64`, refill trigger `outstanding+K >= sq_depth` at `bw.c:799`) is the direct precedent for the selective-signaling window in all-gather perf mode; ring scales it down to `32/8` because `N-1` ring steps share the QP depth.

---

## 7. `past_exercise/bw.c` control SEND path — what the ring reuses and what diverges

| Aspect | `bw.c` | Ring (summary) |
|---|---|---|
| WR opcode | `IBV_WR_SEND` (`bw.c:654`) | Same for RTS/CTS/DATA_DONE/BARRIER/EAGER header |
| Inline | `IBV_SEND_INLINE` if `msg_len <= max_inline_data`, else `memcpy(ctrl_buf)` + registered SEND (`bw.c:662-669`) | Same; attempt `max_inline_data` stepping at QP create (`bw.c:552-568`) and runtime read-back (`bw.c:584-590`) |
| Signaled | Always `IBV_SEND_SIGNALED` (`bw.c:655`) | Same |
| Recv pool | 32 recvs posted once at init (`bw.c:617-638`), never refreshed; covers 21 done/ack pairs | Two per-QP pools (`PG_CTRL_POOL_DEPTH=32` each), **reposted after consumption** — pipelined RTS/CTS/DATA_DONE + barriers exceed 21 |
| CQ | Shared (`bw.c:536`) | Shared single CQ, both QPs; `wr_id` bit-packed QP + class + slot/seq (summary §7) |
| Poll | `bw_poll_until` +10 s deadline + `bw_wc_bad` (`bw.c:682-737`) | Same pattern; timeout `PG_CTRL_POLL_TIMEOUT_SEC=10` |
| Max inline | Stepped from 1024 down by 64 until QP creation succeeds (mlx4 rejects oversized WQEs) (`bw.c:552`) | Same stepping |
| Data WRs | `IBV_WR_RDMA_WRITE` to server `buf_addr:rkey` from TCP exchange (`bw.c:858-866`), linked lists of K (`bw.c:840-869`) | Same opcode; remote addr/rkey from CTS (not TCP), windowed per summary §20 |

---

## 8. Benchmark comparison — assignment ask vs implementation

Assignment: *"compare rendezvous vs trivial eager; implement pipelining to overlap communication and computation; use RDMA Read/Write zero-copy on all-gather."*

Implementation delivers the comparison axis as **compile-time modes** (summary §9):

```bash
make MODE=rendezvous PROFILE=perf   # default — rendezvous payload, zero-copy AG
make MODE=eager     PROFILE=perf   # eager payload (2-SGE SEND), copy at receiver
make MODE=auto      PROFILE=perf   # ≤8 KiB eager, >8 KiB rendezvous
```

Plus `PROFILE=bringup` (64 KiB chunk, window 1) vs `PROFILE=perf` (256 KiB, 32/8) for deterministic bringup vs throughput-optimized.

Harness (summary §29):

```
1. alloc sendbuf/recvbuf (count derived from swept bytes / sizeof(T))
2. fill deterministic pattern: sendbuf[i] = rank*count + i (summary §28)
3. one warmup collective (primes MR cache, internal buffer cache)
4. barrier
5. PG_BENCH_ITER=5 timed iters: barrier → clock → pg_all_reduce → clock → barrier
6. report min / median / avg (μs) and effective_Gbps = 2*(N-1)/N * bytes *8 / median / 1e9
   (summary §29: all-reduce effective bytes)
```

To show the eager↔rendezvous crossover, sweep \(N\in\{2,4\}\) × counts \(\{N·1, N·4, N·1024, N·65536\}\) (summary §28 correctness matrix) and the large perf sweep `64 MB … 1 GB` (powers of two). Expect eager faster ≤8 KiB (RTT saved > copy cost), rendezvous faster ≥64 KiB (copy + buffer cost dominates).

---

## 9. Protocol flow diagrams

### Non-pipelined rendezvous — one segment `s` from `r` to `next`

```
r                          next
│  RTS(s, len=L)  ────────►│  (IBV_WR_SEND 64 B, qp_to_next → qp_from_prev)
│                          │  ──► CTS(s, addr=recvbuf[recv_origin], rkey, L)
│  ◄──────── CTS  ─────────│  (IBV_WR_SEND 64 B, qp_to_next of next's view)
│  RDMA_WRITE L bytes ────►│  (IBV_WR_RDMA_WRITE → final VA, zero-copy for AG
│                          │                     → staging for RS)
│  DATA_DONE(s)  ─────────►│  (IBV_WR_SEND 64 B)
│                          │  ──► progress: validate, repost, reduce/forward
(RS repeats N-1 segments; AG repeats N-1 segments with same 4 steps)
```

### Pipelined reduce-scatter — segment `s` split into micros `m0 … mk`

```
r                                          next
│  RTS(s, L)  ─────────────────────────►   │
│  ◄──────── CTS(s, staging=L)  ─────────  │
│  WR(m0)→staging+0          ───────────►  │
│  DATA_DONE(m0)  ─────────────────────►  │  reduce m0 ─┐
│  WR(m1)→staging+chunk      ───────────►  │  (CPU)      │ NIC writes m1 concurrently
│  DATA_DONE(m1)  ─────────────────────►  │  reduce m1 ─┘
│  ... per micro until L exhausted
(Achieves receive-while-reduce; summary §18)
```

### Pipelined all-gather — segment `s` → micros, windowed

```
r                                          next
│  RTS(s, L)  ─────────────────────────►   │
│  ◄──────── CTS(s, recvbuf[origin], L) ─  │
│  WR(m0)→recvbuf+0          ───────────►  │
│  WR(m1)→recvbuf+chunk      ───────────►  │  (no per-micro DATA_DONE)
│  ... up to WINDOW=32 in flight, signal every K=8
│  [ drain: poll completions until all WRs signaled ]
│  DATA_DONE(s)  ──────────────────────►  │  segment done → forward in next AG step
(summary §§19-20: one DATA_DONE per segment for AG)
```

### Eager — one payload (segment or micro)

```
r                          next
│  SEND{ header 64 B │ payload L } ──►  │  (IBV_WR_SEND 2 SGEs, qp_to_next)
│                                       │  recv completion → copy payload
│                                       │  into work/final at slot, repost eager recv
(no RTS/CTS, no rkey exchange, one RTT)
```

---

## 10. Summary table — eager vs rendezvous

| Dimension | Eager (2-SGE SEND) | Rendezvous (RTS/CTS/WRITE/DATA_DONE) |
|---|---|---|
| Trigger | `len ≤ threshold` in `PG_MODE_AUTO`, or forced `MODE=eager` | `len > threshold` in AUTO, or forced `MODE=rendezvous` |
| Threshold | `PG_EAGER_THRESHOLD = 8 KiB` (summary §25) | — |
| RTTs | 1 (SEND) | ~2 (RTS→CTS + WRITE + DATA_DONE; WRITE is pipelined) |
| Copies | 1 copy at receiver (eager buf → final) | 0 copies (AG: WRITE lands in final `recvbuf`; RS: WRITE lands in staging then one reduction op) |
| Buffer mgmt | 16 pre-posted eager recvs, each `max(8 KiB, 256 KiB)` | CTS grants on demand; staging is one grow-only full-segment buffer (summary §17) |
| Address exchange | None (anonymous SEND) | CTS carries `addr + rkey` (summary §8) |
| Completion | Recv CQE on remote (SEND) | Explicit `DATA_DONE` SEND (since `WRITE_WITH_IMM` crossed out) |
| Pipelining | Per-micro eager SENDs possible, still copy-bound | Per-micro WRITE + per-micro DATA_DONE (RS) / per-segment DATA_DONE (AG) enables overlap (summary §§18-19) |
| Best for | Small / latency-sensitive | Large / bandwidth-sensitive |

---

## 11. Constants at a glance

```
PG_CTRL_MSG_LEN            64 B
PG_CTRL_POOL_DEPTH         32 per QP
PG_EAGER_POOL_DEPTH        16
PG_EAGER_THRESHOLD         8 KiB
PG_PIPELINE_CHUNK          256 KiB (perf) / 64 KiB (bringup)
PG_RDMA_WINDOW             32 (perf) / 1 (bringup)
PG_RDMA_SIGNAL_INTERVAL    8 (perf) / 1 (bringup)
PG_TAG                     0x50475244
PG_TCP_BASE_PORT           19000
PG_TCP_RETRY_MS            100 / PG_TCP_TIMEOUT_SEC 30
PG_CTRL_POLL_TIMEOUT_SEC   10
PG_MODE_DEFAULT            PG_MODE_RENDEZVOUS_DATA
PG_WORKBUFFER_DEFAULT      safe
IB_PORT                    1, first device, RC, LID only (no RoCE)
min_rnr_timer 12 / timeout 14 / retry_cnt 7 / rnr_retry 7
max_rd_atomic 1 / max_dest_rd_atomic 1
```

---

## 12. Deferred / non-goals (summary §31)

RDMA_READ data path, RoCE/GID, general `count % size != 0`, pipeline remainder for arbitrary counts, double full-segment staging, `PG_MODE_AUTO` as default, multiple internal buffer caches — all explicitly deferred. Any of them would change the threshold or pipelining shape and must be re-grilled.

---

## 13. What to build next (V3-V9 implication)

V3 proves the rendezvous 4-step on one segment. V4/V5 add ring formulas (summary §§12-13). V7 adds per-micro DATA_DONE and the progress engine (summary §23). V8 adds `WINDOW/interval` windowing and the `min/median/avg` harness (summary §29). V9 adds the 2-SGE eager path and `MODE` selectors — the actual eager vs rendezvous comparison is only valid once V9 exists.
