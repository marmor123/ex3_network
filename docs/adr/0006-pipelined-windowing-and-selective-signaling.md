# ADR-0006: Pipelined Windowing and Selective CQ Signaling

## Context
In large-scale RDMA collectives (up to 1 GiB payloads), transferring entire ring segments as single monolithic transfers prevents overlap between network communication and CPU vector reduction. Furthermore, requesting a Completion Queue Work Completion (`IBV_SEND_SIGNALED`) for every individual micro-chunk incurs severe CQ polling overhead and driver lock contention.

## Decision

### 1. Pipelined Micro-Chunk Slicing
- Segments are partitioned into fixed-size micro-chunks of 256 KiB (`PG_PIPELINE_CHUNK = 262144`).
- Transfer and computation are pipelined: as micro-chunk $m$ arrives in staging memory from `prev_rank`, the CPU begins SSE4.2 vector reduction on micro-chunk $m$ while the NIC concurrently receives micro-chunk $m+1$.

### 2. Sliding Window Flow Control
- We maintain a sliding window of at most 32 in-flight micro-chunks (`PG_RDMA_WINDOW = 32`).
- The sender transmits up to the window capacity without stalling. If the window is full, the sender yields to the progress engine to drain completed work requests before posting new ones.

### 3. Selective CQ Signaling
- Instead of signaling every RDMA Write, only 1 out of every 8 Work Requests (`PG_RDMA_SIGNAL_INTERVAL = 8`) sets `IBV_SEND_SIGNALED`.
- The final Work Request of a segment transfer always sets `IBV_SEND_SIGNALED` to guarantee completion before advancing to the next ring step.
- Polling frequency is reduced by $8\times$, drastically reducing CPU CQ inspection cycles.

## Consequences
- Full overlap of network transmission and SIMD reduction for all payload sizes $\ge 1\text{ MiB}$.
- Sustained line-rate throughput reaching 15.56 Gbps on 20 Gbps InfiniBand DDR interconnect.

## References
- `pg_internal.h` (`PG_PIPELINE_CHUNK`, `PG_RDMA_WINDOW`, `PG_RDMA_SIGNAL_INTERVAL`).
- Commit `d643a3a` (Ticket #9) & `d264ca3` (Ticket #10).
