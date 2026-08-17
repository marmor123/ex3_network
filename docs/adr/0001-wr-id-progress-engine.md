# ADR-0001: wr_id Bit-Packing and Progress-Engine Dispatch

## Context
The collective ring consists of 2 RC QPs (`qp_to_next` and `qp_from_prev`) sharing a single Completion Queue (CQ), handling both control types (`RTS`, `CTS`, `DATA_DONE`, `BARRIER`, `EAGER_PAYLOAD`) and data path work requests (`RDMA_WRITE`, `EAGER_SEND`).

We needed a low-overhead, deterministic completion routing mechanism that distinguishes QP direction and operation type directly from the 64-bit `wr_id` without requiring dynamic memory allocation or payload parsing in the hot CQ polling loop.

## Decision

### 1. wr_id Bit-Packing Layout
- 64-bit `wr_id`:
  - bits `0-3`: `WR_TYPE` enum (0–15): `RECV_CTRL=1`, `SEND_CTRL=2`, `RTS=3`, `CTS=4`, `DATA_DONE=5`, `RDMA_WRITE=6`, `BARRIER=7`, `EAGER_RECV=8`, `EAGER_SEND=9`.
  - bit `4`: `QP_DIR` (`0 = to_next`, `1 = from_prev`).
  - bits `8-31`: Buffer slot index or micro-chunk sequence number.
- Fast bitwise inline helpers:
  - `pg_make_wr(qp_dir, type)`: Packs direction and type.
  - `pg_make_wr_slot(qp_dir, type, slot)`: Packs direction, type, and slot index.
  - `pg_wr_type(wr_id)`: Extracts `wr_id & 0x0F`.
  - `pg_wr_qp(wr_id)`: Extracts `(wr_id >> 4) & 0x01`.
  - `pg_wr_slot(wr_id)`: Extracts `(uint32_t)(wr_id >> 8)`.

### 2. Receive Slot Replenishment (Repost-After-Consume)
- A fixed receive pool of depth `PG_CTRL_POOL_DEPTH = 32` is pre-posted on each QP at initialization.
- Upon polling a `PG_WR_TYPE_RECV_CTRL` CQ completion, the progress engine copies/processes the message and immediately reposts the receive work request to maintain invariant pool depth.

### 3. Progress Engine Polling & Dispatch
- Encapsulated within `pg_progress_poll` and `pg_progress_wait`.
- Unexpected or future-step control messages are automatically diverted into an internal FIFO queue (`pending_q`) and popped when the target step begins.

## Consequences
- Single-cycle $O(1)$ decoding of completion queue entries with zero memory overhead.
- Total decoupling of higher-level collective routines from raw Verbs CQ polling.

## References
- `pg_internal.h` (`pg_make_wr`, `pg_progress_poll`, `pg_progress_wait`).
- `CONTEXT.md` (Progress Seam #1).
