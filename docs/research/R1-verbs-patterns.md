# R1: Verbs RC Ring Patterns from `past_exercise/bw.c` — Findings

> Primary sources: `past_exercise/bw.c` (1122 lines) and `past_exercise/bw_template.c` (833 lines).
> All line cites are against `bw.c` unless prefixed `bw_template.c:`.
> Verification: `grep -n` + full reads of both files; cites checked 2026-08-12.

## 1. Device selection — first device, fixed port

| Fact | Cite |
|---|---|
| Device: first entry from `ibv_get_device_list(NULL)` — no `-d` option (removed vs template) | `bw.c:1046` `dev_list = ibv_get_device_list(NULL)`; `bw.c:1052-1053` `ib_dev = *dev_list` + `bw.c:1052` comment "The first device found — the option-era -d selection is gone." |
| Template had `-d`/`--ib-dev` option | `bw_template.c:667-668` + search `bw_template.c:726-748` device lookup loop |
| IB port fixed `1` | `bw.c:114` `#define IB_PORT 1`; used at `bw.c:1059` `bw_init_ctx(ib_dev, IB_PORT, ...)` and `bw.c:1070` `ibv_query_port(ctx->context, IB_PORT, ...)` |
| `page_size` via `sysconf(_SC_PAGESIZE)`; buffer `malloc(roundup(BUFFER_SIZE, page_size))` | `bw.c:1044` + `bw.c:481` |
| LID is `portinfo.lid`; InfiniBand-only check fails if `link_layer == IBV_LINK_LAYER_INFINIBAND && !lid` | `bw.c:1070-1078` |
| PSN random 24-bit | `bw.c:1082` `lrand48() & 0xffffff` (also `bw_template.c:787`) |

## 2. PD / MR / CQ / QP creation (`bw_init_ctx`, `bw.c:469-612`)

### PD
- Single `ibv_alloc_pd` per context: `bw.c:513` `ctx->pd = ibv_alloc_pd(ctx->context)`.
- Mirrors template `bw_template.c:398` (same call).

### MRs — two, registered once, never re-registered
- Data MR: `bw.c:521-523` `ibv_reg_mr(ctx->pd, ctx->buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | (is_server ? IBV_ACCESS_REMOTE_WRITE : 0))` — 1 MB (`BUFFER_SIZE 1<<20`, `bw.c:61`), server needs `REMOTE_WRITE` so client `RDMA_WRITE` lands, client keeps `LOCAL_WRITE` only.
- Control MR: `bw.c:529-530` `ibv_reg_mr(ctx->pd, ctx->ctrl_buf, CTRL_MSG_LEN, IBV_ACCESS_LOCAL_WRITE)` — 64 B.
- Template registers only one MR (`bw_template.c:404` `ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE)`).

### CQ — shared, sized `max_send_wr + max_recv_wr`, clamped to `max_qp_wr`
- `bw.c:536` `ibv_create_cq(ctx->context, max_send_wr + max_recv_wr, NULL, NULL, 0)` — no completion channel (polling only).
- Sizing: `bw.c:506` `max_send_wr = WINDOW + SIGNAL_INTERVAL` (= 320); `bw.c:507` `max_recv_wr = CTRL_POOL_DEPTH` (= 32); clamped at `bw.c:508-511` against `ibv_query_device` `dev_attr.max_qp_wr` (`bw.c:502`). Critical for mlx4 where `max_qp_wr` can be 128.
- Template: `bw_template.c:410` `ibv_create_cq(ctx, rx_depth + tx_depth, NULL, ctx->channel, 0)` with `rx_depth=100 tx_depth=100` defaults.

### QP — RC, SGE 1/1, INLINE stepdown 1024→64, `ibv_query_qp` read-back

**Declaration:**
- `bw.c:130` `#define MAX_INLINE_DATA_DECLARE 1024`
- `bw.c:552` `for (try_inline = MAX_INLINE_DATA_DECLARE;; try_inline -= 64)` — decrement 64 until `ibv_create_qp` succeeds (`bw.c:566-568` `if (ctx->qp || try_inline <= 0) break`). Comment at `bw.c:546-551` explains: mlx4 rejects when declared `max_inline_data` + WQE overhead exceeds HW limit; no portable query, so stepdown is the portable probe.
- QP attrs at `bw.c:553-564`: `send_cq/recv_cq = ctx->cq`, `cap.max_send_wr = max_send_wr`, `cap.max_recv_wr = max_recv_wr`, `cap.max_send_sge = 1`, `cap.max_recv_sge = 1`, `cap.max_inline_data = try_inline`, `qp_type = IBV_QPT_RC`.

**Read-back (the only valid inline limit for the data path):**
- `bw.c:584` `ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr)` — at `bw.c:584-590` stores `ctx->max_inline_data = init_attr.cap.max_inline_data` and `ctx->sq_depth = init_attr.cap.max_send_wr`. Comment at `bw.c:579-580` says driver may clamp.
- Data-path use at `bw.c:832-834`: `inline_flag = (size <= 64 && size <= ctx->max_inline_data) ? IBV_SEND_INLINE : 0` — even if request was 1024, runtime value (often 200-400 B on mlx4) is the gate. Control SEND at `bw.c:662` always inline (8 B `sizeof(struct bw_ctrl_msg)`, `bw.c:94-95` static assert).
- Template has no inline handling at all (`bw_template.c:418-430` — no `max_inline_data` field).

**Failure message includes the tried inline value:**
- `bw.c:571-573` `fprintf(..., "Couldn't create QP (send_wr %u, recv_wr %u, inline %d)", max_send_wr, max_recv_wr, try_inline)`.

## 3. QP state transitions — INIT → RTR → RTS (`bw.c:592-284`)

### INIT (`bw.c:592-608`)
```c
// bw.c:593-599
.qp_state        = IBV_QPS_INIT,
.pkey_index      = 0,
.port_num        = port,          // IB_PORT 1
.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
```
- Mask at `bw.c:601-605`: `IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS`.
- Template identical at `bw_template.c:438-453` (same four fields/flags).

### RTR (`bw_connect_qp`, `bw.c:235-264`)
```c
// bw.c:238-251
.qp_state            = IBV_QPS_RTR,
.path_mtu            = ctx->portinfo.active_mtu,   // not fixed — from port query
.dest_qp_num         = dest->qpn,
.rq_psn              = dest->psn,
.max_dest_rd_atomic  = 1,
.min_rnr_timer       = 12,
.ah_attr = { .is_global=0, .dlid=dest->lid, .sl=0, .src_path_bits=0, .port_num=port }
```
- Mask `bw.c:254-261`: `IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER`.
- Template `bw_template.c:131-166` identical constants (`min_rnr_timer 12`, `max_dest_rd_atomic 1`, same `ah_attr` defaults) plus GID/RoCE branch at `bw_template.c:151-156` which `bw.c` strips — `bw.c` is InfiniBand-only LID (see `bw.c:1075-1078`).

### RTS (`bw.c:266-283`)
```c
// bw.c:266-271
.qp_state       = IBV_QPS_RTS,
.timeout        = 14,
.retry_cnt      = 7,
.rnr_retry      = 7,
.sq_psn         = my_psn,
.max_rd_atomic  = 1,
```
- Mask `bw.c:272-278`: `IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC`.
- Template `bw_template.c:169-183` same five constants verbatim.

## 4. Handshake — wire format + TCP exchange

### Wire format (`bw.c:134-140`, `bw.c:224-232`)
- `bw.c:138` `#define DEST_FMT         "%04x:%06x:%06x"` — `lid:qpn:psn` (zero-padded 4/6/6 hex, matches `printf` width).
- `bw.c:139` `#define DEST_FMT_SERVER  DEST_FMT ":%" PRIx64 ":%x"` — adds `:buf_addr:rkey`.
- `bw.c:140` `#define DEST_FMT_PARSE   "%x:%x:%x:%" SCNx64 ":%x"` — `sscanf` side (no width, hex).
- `bw.c:134` `#define DEST_MSG_LEN 128` — fixed-size message, zero-padded `memset(msg,0,sizeof msg)` before `sprintf` at `bw.c:330` and `bw.c:439`; on wire via `bw_write_full`/`bw_read_full` (`bw.c:186-210`) which loop until `len` bytes move.
- `bw.c:224-229` `bw_parse_dest`: `n = sscanf(msg, DEST_FMT_PARSE, &lid,&qpn,&psn,&buf_addr,&rkey)` requires `n>=3`, and `n>=5` when `expect_addr==1` (client parsing server). Client calls `expect_addr=1` (`bw.c:350`), server `expect_addr=0` (`bw.c:424`).
- Template uses `"%04x:%06x:%06x:%s"` with GID string (`bw_template.c:234`, `bw_template.c:252`) and `sizeof "0000:000000:000000:00000000000000000000000000000000"` sizing (`bw_template.c:198`).

### TCP exchange — port 18515, `write`/`read`/`"ready"`

| Role | Steps | Cite |
|---|---|---|
| **Port** | `HANDSHAKE_PORT 18515` fixed | `bw.c:115` |
| **Client** `bw_exch_dest_client` | `getaddrinfo` loop connect → `write_full(DEST_FMT)` → `read_full(DEST_FMT_SERVER)` → `write_full("ready", sizeof "ready")` (6 B inc. NUL) → `bw_parse_dest(expect 1)` → close | `bw.c:286-359`, esp. `bw.c:331-345` |
| **Server** `bw_exch_dest_server` | `getaddrinfo AI_PASSIVE` → loop `socket` + `setsockopt(SO_REUSEADDR)` + `bind` → `listen(1)` → `accept` → `read_full(client)` → `parse(expect 0)` → **`bw_connect_qp` before reply** → `write_full(DEST_FMT_SERVER)` with `my_dest.buf_addr/rkey` → `read_full("ready")` → close `connfd` (then `close(sockfd)` earlier) | `bw.c:361-467`, esp. `bw.c:388-413`, `bw.c:418-462` |
| **Server buf advertisement** | `my_dest.buf_addr = (uint64_t)ctx->buf; my_dest.rkey = ctx->mr->rkey` set before server exchange | `bw.c:1091-1092` |
| **Ready size** | `sizeof "ready"` = 6 | `bw.c:345`, `bw.c:454` |
| **Client silent fail** | `sockfd<0` → `return NULL` with no print (T1 criterion) | `bw.c:324-328` |
| **Server ordering note** | Server does `bw_connect_qp` before sending reply — unlike template where `pp_server_exch_dest` connects before reply but client connects after exchange; `bw.c` client connects after exchange at `bw.c:1099-1101` | `bw.c:430-435` vs `bw_template.c:334-339` + `bw_template.c:805-807` |

### Control messages over RC (not TCP)
- After TCP+QP bringup, per-size `done`/`ack` are RC `IBV_WR_SEND` on the same QP (`bw.c:22-24`), 8 B (`struct bw_ctrl_msg { tag seq }`, `bw.c:89-92`, `BW_CTRL_TAG 0x4354524c`, `bw.c:88`), `IBV_SEND_SIGNALED` + `INLINE` when `max_inline_data >= 8` (`bw.c:662-663`).

## 5. Control receive pool — 32 × 64 B, never refreshed

- `bw.c:66` `#define CTRL_POOL_DEPTH 32`, `bw.c:67` `#define CTRL_MSG_LEN 64`
- Posted once at init: `bw_post_control_recvs` (`bw.c:617-638`) loops `i=0..31` `ibv_post_recv(ctx->qp, &wr, &bad_wr)` with single `ibv_sge { addr=ctrl_buf, length=64, lkey=ctrl_mr }` and `ibv_recv_wr { wr_id=BW_RECV_WRID=1, sg_list=&list, num_sge=1 }` (`bw.c:619-635`). All 32 share same `wr_id` and buffer — which recv completed is irrelevant (comment `bw.c:143-146`). Never reposted: `bw.c:614-616` comment + `SWEEP_SIZES 21` (`bw.c:72`) proves 32 covers 21 per-direction messages (21 `done` + 21 `ack` across sweep, 21 per side).
- Posting before handshake: `bw.c:1063-1068` `bw_post_control_recvs` before `ibv_query_port`/`bw_exch_dest` — so no control SEND can find RQ empty (ADR-0001).
- SEND side: `bw_post_ctrl_send` (`bw.c:644-677`) — single SGE `length=sizeof(struct bw_ctrl_msg)=8`, `IBV_SEND_SIGNALED`, `IBV_SEND_INLINE` if `max_inline_data >= 8` else `memcpy(ctrl_buf)` + `lkey` staged.

## 6. W = 256, K = 64 — window, signal interval, streaming

- `bw.c:112` `#define WINDOW 256`, `bw.c:113` `#define SIGNAL_INTERVAL 64` (aka `K`).
- `bw.c:121` `typedef char bw_params_sane[SIGNAL_INTERVAL <= WINDOW ? 1 : -1]` — compile-time `K≤W`.
- SQ depth request `bw.c:506` `max_send_wr = WINDOW + SIGNAL_INTERVAL = 320`; actual `sq_depth = init_attr.cap.max_send_wr` after `ibv_query_qp` (`bw.c:589`).
- `SWEEP_SIZES 21` (`bw.c:72`) and `MSG_COUNTS[21]` (`bw.c:77-83`) — powers-of-two sizes `1 B..1 MB` with converged counts (e.g. 1 B:1310720, 1 MB:80).
- Streaming `bw_post_writes` (`bw.c:827-878`): posts `n` writes as linked lists of `chunk = min(n, K)` (`bw.c:838`); each list is one `ibv_post_send` (`bw.c:869`); `wrs[i].wr_id = BW_DATA_WRID`, `opcode = IBV_WR_RDMA_WRITE`, `remote_addr = dest->buf_addr` (same VA every WR — server absorbs into 1 MB buffer, `bw.c:865`), `rkey = dest->rkey`. Inline flag `bw.c:832-835` `size<=64 && size<=max_inline_data`.

### Signal schedule
- `bw.c:847-849`: `signal = (t % K == 0) || (final && n==chunk && i==chunk-1)` — K-th WR of stream plus final WR always signaled. Mid-stream lists yield one CQE per K WRs; final remainder covered by signaled final WR. RC in-order guarantees one CQE accounts for exactly K WRs.

## 7. Refill-never-empty + poll with deadline

### `bw_refill` (`bw.c:797-815`)
- Trigger: `bw.c:799` `while (st->outstanding + SIGNAL_INTERVAL >= (uint64_t)ctx->sq_depth)` — holds pipe at `W` when grant == request (320 → `outstanding+64>=320` → `outstanding>=256`), and at `sq_depth - K` if clamped (e.g. 128 → holds at 64). Comment at `bw.c:783-791` explains.
- Body: `bw.c:801` `ibv_poll_cq(cq,1,wc)` — `ne<0` error, `ne==0` continue (busy-poll, never blocks), `ne==1` check `bw_wc_bad(wc, 1<<BW_DATA_WRID)` (`bw.c:810`) then `st->outstanding -= SIGNAL_INTERVAL` (`bw.c:812`). One CQE reclaims exactly K WRs. Returns immediately so caller reposts — SQ never empties, NIC never idles.
- Not used for final CQE: comment `bw.c:791-793` — final list's CQE stays for ack wait; refill cannot run after final post because no list follows.
- Call site: `bw.c:842` inside `bw_post_writes` before each `chunk` post.

### `bw_poll_until` (`bw.c:704-738`) + `bw_wc_bad` (`bw.c:682-696`)
- `bw.c:708-710` deadline `CLOCK_MONOTONIC + 10 s`; loop `ibv_poll_cq(cq,1,wc)`; on `ne==1` validate `bw_wc_bad(wc, pass | 1<<want)` then `if (wr_id==want) return 0` else `continue` (pass-through). `pass` carries completions to ignore (client ack wait `bw.c:935-936` `pass = (1<<BW_SEND_DONE_WRID)|(1<<BW_DATA_WRID)`). On timeout `bw.c:728-735` prints `Timed out after 10 s waiting for wr_id`.
- `bw_wc_bad` checks `wc->status != IBV_WC_SUCCESS` and `allowed & (1<<wr_id)` (`bw.c:684-694`).
- `bw_recv_ctrl` (`bw.c:746-767`) wraps `bw_poll_until(BW_RECV_WRID)` + tag/seq check (`BW_CTRL_TAG`, `seq`), stamps `CLOCK_MONOTONIC` for client's `t1` (`bw.c:756-757`).
- Client bench `bw.c:904-950`: `clock_gettime t0` before `bw_post_writes`, `bw_post_ctrl_send(DONE)`, `bw_recv_ctrl(ACK, &t1)` with pass-through, then `elapsed = t1-t0` and `bw_print_result` (`bw.c:882-894` auto-scales bps→Gbps).

## 8. Ring divergences (what to change vs `bw.c` 1-QP pair)

`bw.c` has one RC QP per process pair carrying both data + control (ADR-0001). A ring of N ranks needs per-rank two QPs: `qp_to_next` (send) and `qp_from_prev` (recv) — or equivalently two unidirectional RC QPs per edge direction. Adaptations:

| Topic | `bw.c` pattern | Ring adaptation |
|---|---|---|
| QP count | 1 QP | 2 QPs/rank (or 1 QP/edge × N edges). Each needs its own `INIT→RTR→RTS` with same constants (`pkey 0`, `active_mtu`, `min_rnr 12`, `timeout 14/retry 7`). |
| Control pool | 32×64 B on the one QP | 32×64 B **per QP** (or shared CQ with per-QP `wr_id` namespace). `bw_post_control_recvs` reuses same `wr_id=1` (`bw.c:625`) — ring must disambiguate QP in `wr_id` (bit-packing) or separate poll. 32 still covers 21 exchanges/direction; for ring collectives the bound is `N-1` steps, still <32. |
| CQ | `max_send+max_recv` = 352 | Double if sharing: `2*(max_send+max_recv)`; or one CQ per QP (simpler `wr_id` but more polling). |
| Handshake | 1 TCP edge, client `write→read→write ready`, server `read→connect→write→read ready` | N edges, edge-ordered bootstrap (e.g. rank `r` connects to `r+1 mod N`; avoid N simultaneous `listen` deadlocks by rank-ordered `connect`/`accept` or a coordinator). Must exchange **both** QP infos per edge: `lid/qpn/psn` plus `buf_addr/rkey` for the RDMA_WRITE target of that edge. `bw.c` extends template's exchange with `buf_addr/rkey` at `bw.c:440-442`; ring does same per edge. |
| Inline | `size<=64 && <=max_inline_data` | Keep. Ring control messages stay 8 B inline; data path keeps same threshold. `max_inline_data` per QP (read back per `ibv_query_qp`). |
| Window | `W=256 K=64` | Keep or retune; `K≤W` invariant and `outstanding+K>=sq_depth` refill hold at `W` remain. For pipelined collectives the effective window is per-QP. |
| PSN | `lrand48 & 0xffffff` | Same per QP (distinct `sq_psn`/`rq_psn` per edge). |

## 9. Implications for performance / correctness

- Inline reality on mlx4: after stepdown the runtime `max_inline_data` is ~200-400 B, not 1024 — `bw.c`'s read-back is normative; hard-coding 1024 would fail QP creation or inline check.
- `max_qp_wr` clamp: request 320 may grant 128 — refill still correct at `W' = sq_depth - K`; throughput drops but correctness holds.
- Shared CQ busy-poll: `bw_refill` spins on `ne==0` (`bw.c:807-808`); ring with two QPs sharing a CQ must handle interleaved completions (pass-through `allowed` mask already supports this).

## 10. Minimal copy-paste checklist for ring

1. `ibv_get_device_list` first device, `IB_PORT 1`, `sysconf(_SC_PAGESIZE)` + `roundup`.
2. `ibv_query_device` → clamp `max_send_wr/max_recv_wr`; `ibv_alloc_pd`; two MRs (data + 64 B ctrl); `ibv_create_cq(max_send+max_recv)`.
3. `for (try_inline=1024; ; try_inline-=64) ibv_create_qp(... max_inline_data=try_inline)` until success; then `ibv_query_qp(..., IBV_QP_CAP, &init_attr)` → `max_inline_data` + `sq_depth`.
4. `INIT` (pkey 0, port 1, `REMOTE_READ|REMOTE_WRITE`); post 32 control recvs; `ibv_query_port` → `lid/active_mtu`.
5. TCP exchange `18515`, `DEST_FMT`/`DEST_FMT_SERVER`/`DEST_FMT_PARSE`, `128 B` zero-padded, `write→read→write ready` / `read→connect→write→read ready`, `SO_REUSEADDR`, `listen 1`.
6. `RTR` (`active_mtu`, `dest_qpn`, `rq_psn`, `min_rnr 12`, `is_global 0`); `RTS` (`timeout 14`, `retry 7`, `rnr 7`, `sq_psn`).
7. Streaming linked lists of ≤K `RDMA_WRITE` to `dest->buf_addr`, signal K-th + final, inline `size<=64 && <=max_inline_data`, `W=256 K=64`, `bw_refill` hold `outstanding+K>=sq_depth`, `bw_poll_until` 10 s.

---
*Sources re-checked: `bw.c:552,584-590` INLINE; `bw.c:138-140` DEST_FMT; `bw.c:592-601,236-274` INIT/RTR/RTS; `bw.c:115` port; `bw.c:617-638` control pool; `bw.c:112-113,799,832-833` W/K/refill/inline. Template refs `bw_template.c:134-174,398-453,726-748`.*
