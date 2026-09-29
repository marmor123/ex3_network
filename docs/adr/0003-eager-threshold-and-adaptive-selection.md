# ADR-0003: Empirical Eager Threshold and Adaptive Protocol Selection

## Context
In RDMA network collectives, protocol selection fundamentally trades off control handshake overhead against CPU memory copies.
- **Rendezvous Protocol** (`RTS` $\to$ `CTS` $\to$ `RDMA_WRITE` $\to$ `DATA_DONE`) delivers zero-copy direct memory placement but suffers from round-trip control latency (2 RTTs) on small payloads.
- **Eager Protocol** (`IBV_WR_SEND` with 2-SGE scatter-gather) eliminates the control handshake by immediately pushing payload into pre-posted receive buffers, but incurs buffer copies and memory consumption for receive pools.

We needed to establish the optimal crossover threshold empirically on the live 4-node InfiniBand cluster (`mlx-stud-01..04`) and provide clean compile-time / runtime mode selection.

## Decision

### 1. Bounded Eager Selection: 64 KiB per Segment
`PG_EAGER_THRESHOLD = 64 KiB` bounds every pre-posted Eager receive slot and keeps protocol choice symmetric. On four ranks this selects Eager through a 256 KiB total tensor and Rendezvous above it.

The full sweep on 2026-09-28 measured Eager at $18.8\,\mu\text{s}$ versus Rendezvous at $62.4\,\mu\text{s}$ for 64 B, and $198.2\,\mu\text{s}$ versus $218.2\,\mu\text{s}$ for 256 KiB. Results near the crossover were non-monotonic: Rendezvous won at 512 KiB, Eager at 1–4 MiB, and Rendezvous from 8 MiB onward. The configured threshold is therefore a conservative receive-memory and flow-control bound, not the current empirical latency crossover.

### 2. Protocol Modes
We support three compilation modes via `Makefile MODE=<mode>`:
- `MODE=rendezvous` (`PG_MODE_RENDEZVOUS`): All sizes use pipelined RDMA Write rendezvous.
- `MODE=eager` (`PG_MODE_EAGER`): All sizes use Eager SEND with pipelined 64 KiB micro-chunks.
- `MODE=auto` (`PG_MODE_AUTO`): Dynamically selects Eager for segment sizes $\le 64\text{ KiB}$ and Rendezvous for $> 64\text{ KiB}$.

### 3. Unified Pre-Posted Receive Pool & Direct Zero-Copy Receive
- **Strict 64 KiB Sizing (`PG_EAGER_BUF_SIZE = PG_EAGER_THRESHOLD`)**: Receive buffers are sized strictly to the eager threshold (64 KiB + header = 65,600 bytes) rather than `PG_PIPELINE_CHUNK` (256 KiB). For 32 slots across 2 QP directions, pinned receive memory dropped from **16.78 MiB down to 4.19 MiB (75% memory footprint reduction)**.
- **Direct Zero-Copy CPU Receive**: In `pg_progress_poll`, `out_event->eager_buf` points directly to the hardware receive slot buffer (`recv_slot_buf[dir][slot]`), eliminating the intermediate 64 KiB `memcpy` into `eager_rx_buf`. SSE4.2 SIMD vector reductions and memory copies execute directly against the hardware receive slot.
- **Deferred Slot Reposting**: The receive slot is returned to the NIC Receive Queue via `pg_repost_recv_slot` only after the payload is consumed or copied into the pending queue. Eager and control receives share `PG_CTRL_POOL_DEPTH = 32` slots per QP; there is no separate eager receive pool. The default eager send window is 8, but completed/unpolled receives can also occupy slots, so holding one active slot does not imply that the other 31 remain available to the NIC.
- **Unified Control & Eager Send Header Pool**: The 32-entry circular buffer pool `ctrl_send_buf` is shared between standalone control messages and 2-SGE eager send headers, eliminating duplicate MR registrations and saving verbs kernel resources.
- **Pre-Allocated Pending Bounce Buffers**: The first 32 entries per direction receive bounce buffers during `pg_rdma_init_resources`. Empty queues reset their indices to reuse these buffers for short bursts; entries reached by deeper or continuously occupied queues allocate lazily and retain their buffers until cleanup.

### 4. Symmetric Protocol Decisions in Remainder Distribution
When buffer counts are non-divisible by rank count ($C \bmod N \ne 0$), segment sizes across adjacent ranks can differ by 1 element ($Q$ vs $Q+1$). If segment sizes span the 64 KiB crossover boundary, checking local segment sizes independently would cause Rank $i$ to choose Eager while Rank $i-1$ chooses Rendezvous, deadlocking the transfer.
To prevent this, `pg_is_eager` evaluates the maximum possible segment across the ring:
$$\text{max\_seg\_bytes} = \frac{\text{total\_bytes} + N - 1}{N}$$
This derives from `desc->total_bytes`, ensuring all ranks make mathematically identical protocol decisions.

## Consequences
- Small-message operations achieve $18.8\,\mu\text{s}$ Eager latency at 64 B in the 2026-09-28 four-node sweep.
- At the configured 256 KiB four-rank tensor boundary, Eager measured $198.2\,\mu\text{s}$ versus $218.2\,\mu\text{s}$ for Rendezvous.
- Pinned receive memory footprint slashed by **75%** (from 16.78 MiB to 4.19 MiB).
- Large-message operations achieve peak link bandwidth without buffer copy bottlenecks (**22.23 Gbps** Rendezvous and **21.57 Gbps** AUTO at 1 GiB).
- `MODE=auto` is conservative above 256 KiB; the threshold should be retuned separately if benchmark-optimal selection is required.
- Guaranteed ring-wide symmetry across non-divisible remainder boundaries.

## References
- `assignment.txt`: Lecture #2 Eager vs. Rendezvous requirements.
- `docs/empirical_protocol_report.md`: Sweep data on `mlx-stud-01..04`.
- Commit `3b33127`, `5e33aad`, `88f62d7`, `1ae3c55`.
