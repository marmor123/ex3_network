# ADR-0001: wr_id bit-packing and progress-engine dispatch

## Context
Ring has 2 RC QPs (`qp_to_next` / `qp_from_prev`) sharing one CQ, plus rendezvous/eager control types (RTS/CTS/DATA_DONE/BARRIER) and pipelined micro-chunks. `bw.c` uses simple `1ULL<<wr_id` mask with 5 constants (`bw.c:146-155`, `bw.c:682`), single QP, 32 control recvs never refreshed. Ring needs per-QP discrimination but wants to keep `bw.c`-style masking and avoid payload parsing in hot poll.

## Decision

### wr_id layout — Simple enum per-QP
- 64-bit `wr_id`:
  - bits `0-3`: `WR_TYPE` enum (0-15): `RECV_CTRL=1`, `SEND_CTRL=2`, `RTS=3`, `CTS=4`, `DATA_DONE=5`, `RDMA_WRITE=6`, `BARRIER=7`, `EAGER_RECV=8` (extends `bw.c:146`)
  - bit `4`: `QP_DIR` (`0 = to_next`, `1 = from_prev`)
  - bits `5-63`: unused reserved (seg/micro carried in `pg_ctrl_msg` 64B payload, not in wr_id)
- Helper: `static inline uint64_t pg_make_wr(int qp_dir, int type) { return (uint64_t)(type & 0xF) | ((uint64_t)(qp_dir & 1) << 4); }`
- Masking still works because all wr_ids `< 32` — `1ULL<<wr_id` fits 64b mask like `bw.c:682` `allowed & (1ULL<<wr_id)`.
- Parsing: `int type = wc.wr_id & 0xF; int qp = (wc.wr_id >> 4) & 1;`

Rejected: packed seg/micro in wr_id (adds encode/decode, payload already has seg), union struct helper (cost, not needed).

### Repost policy — Repost-after-consume per-QP
- Ring cannot use `bw.c` never-refresh (21 msgs total, sweep ends). Must repost after each consume to keep `CTRL_POOL_DEPTH` (32 or 16 in perf) always posted per QP.
- After `wc.wr_id == pg_make_wr(qp, RECV_CTRL)` : copy 64B msg from `ctrl_buf[qp]`, then `ibv_post_recv(qp_from_prev / qp_to_next recv QP, &wr, &bad)` with same `wr_id` / same `addr=lkey`. On failure -> protocol error.
- No batch threshold; keeps refill-simple and avoids `routs` hysteresis like `bw_template.c:572`.

### Dispatch — Switch dispatch
- Poll loop: `int ne = ibv_poll_cq(cq, 1, &wc); if(ne==1) { if(bw_wc_bad) err; switch(wr_id & 0xF) {case RECV_CTRL: handle_recv(qp); break; case RDMA_WRITE: outstanding[qp]--; break; ...}}`
- Matches `bw.c:704-737` `bw_poll_until` + `bw.c:682` `bw_wc_bad`. Table dispatch rejected (overkill for <8 types).

### Window — Per-QP refill-never-empty
- Per-QP `sq_depth[qp]` from `ibv_query_qp` read-back (like `bw.c:590`), per-QP `outstanding[qp]`.
- Before each `ibv_post_send` batch: `while(outstanding[qp] + K >= sq_depth[qp]) { poll_one; if(wc.type==RDMA_WRITE) outstanding[qp]-=K; }` mirrors `bw.c:798-815`.
- Bring-up uses `K=1` window=1 (still same code path), perf uses `W=32 K=8` (or `W=256 K=64` bw.c scale) — unified path.

## Consequences
- `V2` implements helpers `pg_make_wr` and per-QP `ctrl_buf[2][CTRL_POOL_DEPTH]` pools.
- `V3` uses same helpers for RTS/CTS/DATA_DONE; payload validates seg/micro, wr_id only routes.
- Research R1 findings (`research/r1-verbs-patterns`) still valid — inline/stepdown, QP constants unchanged.

## References
- `past_exercise/bw.c:146-155`, `682-696`, `704-738`, `798-815`
- `past_exercise/bw_template.c:572`
- Research branches `research/r1-verbs-patterns`, `r2`, `r3` (R1 9f943ea etc.)
