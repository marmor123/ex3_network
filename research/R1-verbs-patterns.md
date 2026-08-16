# R1: Verbs RC ring patterns from bw.c — findings

## Source
- `past_exercise/bw.c` (39053 bytes, fully adapted from `bw_template.c`)
- `past_exercise/bw_template.c` (pingpong template)
- `assignment.txt`, `summary_of_grilling.txt`

## Device / PD / CQ / QP creation (bw.c:470-612)

- **Device selection**: first device from `ibv_get_device_list`, no `-d` option (`bw.c:1052-1058`). `past_exercise/bw_template.c:360` same but there option existed in older template removed in bw.c.
- **IB port**: fixed `IB_PORT 1` (`bw.c:114`), not configurable.
- **page_size** via `sysconf(_SC_PAGESIZE)` and `roundup(BUFFER_SIZE, page_size)` for `malloc` (`bw.c:482`).
- **PD**: `ibv_alloc_pd` single per ctx (`bw.c:513`).
- **MRs**: two MRs — 1 MB data `buf` (`IBV_ACCESS_LOCAL_WRITE | REMOTE_WRITE` if server else 0) at `bw.c:521`, and `CTRL_MSG_LEN 64` control area at `bw.c:529`. Both registered once.
- **CQ**: `ibv_create_cq(ctx, max_send_wr + max_recv_wr, NULL,NULL,0)` at `bw.c:536` — shared CQ for send+recv. Depth is `max_send_wr + max_recv_wr` where `max_send_wr = W+K=320` and `max_recv_wr=32` but clamped to `dev_attr.max_qp_wr` via `ibv_query_device` at `bw.c:502-511`. This clamping is critical for mlx4 with small max_qp_wr.
- **QP**: `IBV_QPT_RC` with `max_send_sge=1 max_recv_sge=1` at `bw.c:553-560`. `max_inline_data` declaration stepping: `for (try_inline=1024;; try_inline-=64)` until `ibv_create_qp` succeeds (`bw.c:552`). mlx4 rejects when WQE overhead too large, no portable query. Runtime `max_inline_data` read back via `ibv_query_qp(..., IBV_QP_CAP)` at `bw.c:584-590`; stored in `ctx->max_inline_data` and `sq_depth = init_attr.cap.max_send_wr`. This read-back is the **only** correct inline limit for data path (`bw.c:833`). Inline used only when `size <=64 && size <= max_inline_data` (bw.c:833) — control always inline (8 B).

## QP state transitions

- **INIT**: `pkey_index 0, port_num IB_PORT, qp_access_flags REMOTE_READ|REMOTE_WRITE` (`bw.c:592-601`). Note bw.c adds REMOTE_READ even though only WRITE used — harmless.
- **RTR**: `path_mtu = portinfo.active_mtu`, `dest_qpn`, `rq_psn`, `max_dest_rd_atomic 1`, `min_rnr_timer 12`, `ah_attr {is_global 0, dlid, sl 0, src_path_bits 0, port_num}` (`bw.c:236-248`). Template `pp_connect_ctx` (`bw_template.c:134-149`) identical except adds `is_global` handling for GID/RoCE — bw.c strips it (InfiniBand-only LID, `bw.c:1075-1078` checks `link_layer == IBV_LINK_LAYER_INFINIBAND && !lid` fail).
- **RTS**: `timeout 14, retry_cnt 7, rnr_retry 7, sq_psn, max_rd_atomic 1` (`bw.c:266-274`). Same as template at `bw_template.c:169-174`.

## Handshake / TCP exchange

- **Port**: `HANDSHAKE_PORT 18515` fixed (`bw.c:115`).
- **Wire format**: `DEST_FMT "%04x:%06x:%06x"` plus server `":%"PRIx64":%x"` for `buf_addr:rkey` (`bw.c:138-140`). Parse via `DEST_FMT_PARSE "%x:%x:%x:%"SCNx64":%x"` requiring 3 fields client, 5 server (`bw.c:224-229`). Size `DEST_MSG_LEN 128`, zero-padded `memset(msg,0)` before `sprintf`+`write_full` (`bw.c:330-335`).
- **Client flow** (`bw.c:286-358`): `getaddrinfo` connect, `write_full(client DEST_FMT)`, `read_full(server DEST_FMT_SERVER)`, `write_full("ready")` (sizeof includes \0, 6 bytes), then `bw_parse_dest` with `expect_addr=1`. Silent fail if no server (return NULL no print) (`bw.c:325`).
- **Server flow** (`bw.c:361-467`): `getaddrinfo AI_PASSIVE`, `SO_REUSEADDR`, `bind` loop, `listen 1`, `accept`, `read_full(client)`, `bw_parse_dest(expect 0)`, `bw_connect_qp` **before** sending reply (unlike template where server connects after), `write_full(server)`, `read_full("ready")` (`bw.c:450-462`).
- **psn**: `lrand48() & 0xffffff` (`bw.c:1082`).

## Control pool / data-path constants

- **CTRL_POOL_DEPTH 32, CTRL_MSG_LEN 64** (`bw.c:66`): posted once at init via `bw_post_control_recvs` (`bw.c:617-638`) — 32 WRs same `wr_id=1` same addr/lkey, never refreshed. Covers 21 per-direction done/ack pairs (SWEEP_SIZES 21). Control SEND via `bw_post_ctrl_send` (`bw.c:644-677`): single SGE, `IBV_SEND_SIGNALED` (+INLINE if fits), non-inline staged via `memcpy(ctrl_buf)`.
- **Window 256, Signal interval 64** (`bw.c:112-113`): `WINDOW <=115? 256 K=64`. `K<=W` compile-time assert (`bw.c:121`). SQ depth `W+K=320` requested (`bw.c:506`). Refill trigger `outstanding+K >= sq_depth` holds pipe at W (`bw.c:799`).
- **Streaming**: `MSG_COUNTS[21]` powers-of-two table (`bw.c:77-83`), per-size timed batch of RDMA WRITEs posted as linked lists of up to K WRs (`bw.c:827-876`), only K-th WR signaled except final WR always signaled (`bw.c:847`). WR opcode `IBV_WR_RDMA_WRITE`, `remote_addr = dest->buf_addr` (same VA each WR — server absorbs into single 1 MB buffer), inline only if `size<=64 && <=max_inline_data` (`bw.c:833`).
- **Polling**: `bw_poll_until` (+10s deadline via `CLOCK_MONOTONIC`) (`bw.c:704-737`) and `bw_wc_bad` (`bw.c:682-696`) centralize status checks. Client ack wait passes `BW_SEND_DONE_WRID|BW_DATA_WRID` through (`bw.c:936`), server consumes ack SEND via same poll. Refill (`bw.c:798-815`) never empties SQ: while loop polls one CQE at a time, each accounts for K WRs, continues on 0.

## Divergence for ring (2 QPs vs 1)

- bw.c uses **one RC QP per process pair** for both data and control (ADR-0001). Ring needs **two QPs per rank**: `qp_to_next` (send) and `qp_from_prev` (recv) or equivalently two unidirectional RC QPs — each remote peer appears once as destination. Lessons:
  - Each QP needs its own control pool (e.g., 32 depth per QP) or share CQ but per-QP recv WR accounting.
  - `bw_post_control_recvs` loops 32 `ibv_post_recv` with same wr_id — ring should distinguish QP via wr_id bit-packing or separate handling, but bw.c proves 32 per-direction is sufficient for 21 exchanges.
  - QP count doubles CQ depth: `cq_depth = 2*(max_send + max_recv)` if sharing CQ.
  - Handshake must exchange **both** QP infos atomically: rank r's `qp_to_next` addr must be given to next, and `qp_from_prev` addr from prev. Ring size N>2 requires N edges; bw.c does one edge.

## Implications for perf (for R2/R3 cross-ref)
- Inline limit reality on mlx4: ~ ~ 200-400 B typical after stepdown, not 1024. bw.c's read-back is normative.
- max_qp_wr clamp: request 320 may be granted 128 on older HW — refill still works at W' = sq_depth - K.

