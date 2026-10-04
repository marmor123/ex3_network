# ADR-0001: wr_id Bit-Packing and Progress-Engine Dispatch

## Context
The collective ring consists of 2 RC QPs (`qp_to_next` and `qp_from_prev`) sharing a single Completion Queue (CQ), handling control types (`PING`, `RTS`, `CTS`, `DATA_DONE`, `EAGER_PAYLOAD`) and data-path work requests (`RDMA_WRITE`, `EAGER_SEND`).

We needed a low-overhead, deterministic completion routing mechanism that distinguishes QP direction and operation type directly from the 64-bit `wr_id` without requiring dynamic memory allocation or payload parsing in the hot CQ polling loop.

## Decision

### 1. wr_id Bit-Packing Layout
- 64-bit `wr_id`:
  - bits `0-3`: completion kind: `RECV_CTRL=1`, `SEND_CTRL=2`, `RDMA_WRITE=6`, or `EAGER_SEND=9`.
  - bit `4`: `QP_DIR` (`0 = to_next`, `1 = from_prev`).
  - bits `8-39`: 32-bit buffer slot index or micro-chunk sequence number.
- Fast bitwise inline helpers:
  - `pg_make_wr(qp_dir, type)`: Packs direction and type.
  - `pg_make_wr_slot(qp_dir, type, slot)`: Packs direction, type, and slot index.
  - `pg_wr_type(wr_id)`: Extracts `wr_id & 0x0F`.
  - `pg_wr_qp(wr_id)`: Extracts `(wr_id >> 4) & 0x01`.
  - `pg_wr_slot(wr_id)`: Extracts `(uint32_t)(wr_id >> 8)`.

### 2. Receive Slot Replenishment (Repost-After-Consume)
- A fixed receive pool of depth `PG_CTRL_POOL_DEPTH = 32` is pre-posted on each QP at initialization.
- For a control-only receive, the progress engine copies the header into the event and immediately reposts the receive slot.
- The completion's actual byte length is checked before reading the header or exposing an eager payload. Control-only receives must contain one complete header; eager receives must also contain exactly the declared payload bytes.
- For an eager receive, the event borrows the registered slot until its payload is consumed or copied into the pending queue. Only then is the slot reposted; reposting earlier would allow the NIC to overwrite data still in use.

### 3. Progress Engine Polling, Dispatch & Automatic Buffering
- Encapsulated within `pg_progress_poll`, the ring-step transfer engines, and the private connection/teardown helper `pg_rdma_ring_ping`. The ping helper waits for its next-neighbor send completion and previous-neighbor ping while buffering early collective traffic; it needs no general-purpose send/receive waiter.
- Unexpected or future-step control messages are automatically diverted into an internal FIFO queue (`pending_q`) via `pg_progress_buffer_unexpected` rather than leaking queue maintenance to callers.
- Pending receives are consumed only from the head of each per-QP FIFO. A newly polled receive cannot bypass an older pending message from the same RC QP.
- The queue stores entries directly at its circular head/tail positions. It needs no index array, occupancy flags, or free-slot scan; enqueue and dequeue bookkeeping are O(1). Draining resets both indices to reuse the initially allocated eager buffers for short bursts.
- **Eager Payload Ownership**: A live `pg_progress_event` points into a registered receive slot. Each queued `pg_pending_entry` owns a separate bounce buffer so the receive slot can be reused. Popping an eager entry copies its payload into the context's aligned `eager_rx_buf`, which remains valid until the next eager pop. Neither event nor pending entry embeds a large payload array.
- **Compiler Ordering**: After `ibv_poll_cq` returns a completion, `pg_progress_poll` issues `asm volatile("" ::: "memory");` before inspecting the completion and payload. This prevents compiler motion across that point; it is not a CPU fence or a replacement for the supported verbs/platform completion semantics.

## Consequences
- $O(1)$ bitwise decoding of completion queue entries without a separate dispatch allocation.
- Total decoupling of higher-level collective routines from raw Verbs CQ polling.
- Historical measurements from the original pointer conversion were **~33.6 MB to ~340 KB** for the context and **262 KB to 88 bytes** for the polling stack frame. These are not current ABI/stack-size guarantees and exclude separately allocated payload buffers.
- Receive-slot lifetime is explicit: eager payloads are consumed or preserved before the NIC can reuse their storage.

## References
- `pg_internal.h` (`pg_make_wr`, `pg_pending_queue`) and `pg.c` (`pg_progress_poll`).
- `CONTEXT.md` (Ordered Transfer Stream and Micro-Chunk).
- `docs/adr/0009-strict-fifo-phase-handoff.md` (current matching contract).
- Commit `c1b79ac` (Ticket #18) & `add1f2f`.
