# ADR-0003: Empirical Eager Threshold and Adaptive Protocol Selection

## Context
In RDMA network collectives, protocol selection fundamentally trades off control handshake overhead against CPU memory copies.
- **Rendezvous Protocol** (`RTS` $\to$ `CTS` $\to$ `RDMA_WRITE` $\to$ `DATA_DONE`) delivers zero-copy direct memory placement but suffers from round-trip control latency (2 RTTs) on small payloads.
- **Eager Protocol** (`IBV_WR_SEND` with 2-SGE scatter-gather) eliminates the control handshake by immediately pushing payload into pre-posted receive buffers, but incurs buffer copies and memory consumption for receive pools.

We needed to establish the optimal crossover threshold empirically on the live 4-node InfiniBand cluster (`mlx-stud-01..04`) and provide clean compile-time / runtime mode selection.

## Decision

### 1. Empirical Threshold Determination: 64 KiB
A coordinate sweep on the 4-node cluster across thresholds from 1 KiB to 256 KiB revealed:
- Payloads $\le 64\text{ KiB}$ segment ($\le 256\text{ KiB}$ tensor): Eager protocol provides **$2.1\times\text{--}2.3\times$ lower latency** compared to Rendezvous ($41.88\,\mu\text{s}$ vs $94.3\,\mu\text{s}$ at 64 B, $89.95\,\mu\text{s}$ vs $140.9\,\mu\text{s}$ at 64 KiB, $130.89\,\mu\text{s}$ at 128 KiB, $214.56\,\mu\text{s}$ at 256 KiB) by eliminating the 4-way RTS/CTS control handshake and pushing payload directly into pre-posted receive buffers.
- Payloads $> 64\text{ KiB}$ segment: Rendezvous protocol overtakes Eager in effective bandwidth due to zero-copy direct memory transfers and pipelined micro-chunk overlap.
- We set `PG_EAGER_THRESHOLD = (64 * 1024)` (64 KiB) as the optimal crossover boundary.

### 2. Protocol Modes
We support three compilation modes via `Makefile MODE=<mode>`:
- `MODE=rendezvous` (`PG_MODE_RENDEZVOUS`): All sizes use pipelined RDMA Write rendezvous.
- `MODE=eager` (`PG_MODE_EAGER`): All sizes use Eager SEND with pipelined 64 KiB micro-chunks.
- `MODE=auto` (`PG_MODE_AUTO`): Dynamically selects Eager for segment sizes $\le 64\text{ KiB}$ and Rendezvous for $> 64\text{ KiB}$.

### 3. Unified Pre-Posted Receive Pool & Direct Zero-Copy Receive
- **Strict 64 KiB Sizing (`PG_EAGER_BUF_SIZE = PG_EAGER_THRESHOLD`)**: Receive buffers are sized strictly to the eager threshold (64 KiB + header = 65,600 bytes) rather than `PG_PIPELINE_CHUNK` (256 KiB). For 32 slots across 2 QP directions, pinned receive memory dropped from **16.78 MiB down to 4.19 MiB (75% memory footprint reduction)**.
- **Direct Zero-Copy CPU Receive**: In `pg_progress_poll`, `out_event->eager_buf` points directly to the hardware receive slot buffer (`recv_slot_buf[dir][slot]`), eliminating the intermediate 64 KiB `memcpy` into `eager_rx_buf`. SSE4.2 SIMD vector reductions and memory copies execute directly against the hardware receive slot.
- **Deferred Slot Reposting**: The receive slot is returned to the NIC Receive Queue via `pg_repost_recv_slot` only *after* `pg_step_process_chunk` finishes consuming the payload. Because the sender flow-control window is `PG_EAGER_WINDOW = 8` and the receive queue depth is `PG_EAGER_POOL_DEPTH = 32`, holding 1 active slot during processing leaves $\ge 31$ slots in the RQ, preventing receiver overrun while guaranteeing safety.
- **Unified Control & Eager Send Header Pool**: The 32-entry circular buffer pool `ctrl_send_buf` is shared between standalone control messages and 2-SGE eager send headers, eliminating duplicate MR registrations and saving verbs kernel resources.
- **Pre-Allocated Pending Bounce Buffers**: 32 bounce buffers per direction are pre-allocated during `pg_rdma_init_resources`, eliminating fast-path heap `malloc()` calls in `pg_pending_push`.

### 4. Symmetric Protocol Decisions in Remainder Distribution
When buffer counts are non-divisible by rank count ($C \bmod N \ne 0$), segment sizes across adjacent ranks can differ by 1 element ($Q$ vs $Q+1$). If segment sizes span the 64 KiB crossover boundary, checking local segment sizes independently would cause Rank $i$ to choose Eager while Rank $i-1$ chooses Rendezvous, deadlocking the transfer.
To prevent this, `pg_is_eager` evaluates the maximum possible segment across the ring:
$$\text{max\_seg\_bytes} = \frac{\text{total\_bytes} + N - 1}{N}$$
This derives from `desc->total_bytes`, ensuring all ranks make mathematically identical protocol decisions.

## Consequences
- Small-message operations (e.g. metadata sync, small tensor all-reduces) achieve near-wire latency ($41.88\,\mu\text{s}$ at 64 B).
- Medium-message eager operations achieve up to **-10.5% lower latency** ($214.56\,\mu\text{s}$ vs $239.76\,\mu\text{s}$ at 256 KiB) due to direct zero-copy CPU receive.
- Pinned receive memory footprint slashed by **75%** (from 16.78 MiB to 4.19 MiB).
- Large-message operations achieve peak link bandwidth without buffer copy bottlenecks (**22.51–22.62 Gbps**).
- `MODE=auto` provides the superior Pareto frontier across all buffer sizes from 64 B to 1 GiB.
- Guaranteed ring-wide symmetry across non-divisible remainder boundaries.

## References
- `assignment.txt`: Lecture #2 Eager vs. Rendezvous requirements.
- `docs/empirical_protocol_report.md`: Sweep data on `mlx-stud-01..04`.
- `walkthrough.md`: Optimization benchmark and memory footprint comparison.
- Commit `3b33127`, `5e33aad`, `88f62d7`, `1ae3c55`.
