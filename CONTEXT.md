# Domain Model & Ubiquitous Language

This document establishes the ubiquitous language, architectural seams, and domain glossary for the RDMA Ring Collective library.

---

## Core Domain Concepts

### Process Group
The logical set of participating processes (ranks $0 \dots N-1$) connected in a deterministic ring topology. Each rank communicates directly with its two immediate ring neighbors: `next_rank = (rank + 1) % size` and `prev_rank = (rank - 1 + size) % size`.

### Progress Engine (Seam #1)
The deep module responsible for driving Verbs Completion Queue (`ibv_cq`) polling, `wr_id` dispatch, unexpected control message queueing (`pending_q`), and automatic receive buffer pool replenishment (`pg_repost_recv_slot`). It presents a zero-cost event interface and declarative waiting primitives (`pg_progress_wait_type`, `pg_progress_wait_msg`, `pg_progress_wait_send_recv`) to higher-level collective routines. It includes an explicit compiler memory barrier (`asm volatile("" ::: "memory");`) to guarantee strict DMA visibility.

### Ring Step Transfer (Seam #2)
A single phase of pipelined data movement between adjacent ring neighbors during a collective algorithm. In an $N$-rank ring, collectives execute $N-1$ step transfers where each rank concurrently transmits an outbound segment to `next_rank` while receiving an inbound segment from `prev_rank`. Protocol selection (Eager Send/Recv vs Rendezvous RDMA Write) is encapsulated behind `pg_ring_step_transfer(ctx, desc)`.

### Segment
A major slice of the collective payload assigned to or owned by a specific rank. For non-divisible buffer sizes, segment lengths and byte offsets are calculated using MPI-style remainder distribution ($(Q+1)/Q$ distribution where rank $i < R$ receives $Q+1$ elements and rank $i \ge R$ receives $Q$ elements).

### Micro-Chunk
A pipelined subdivision of a segment (adaptive 64 KiB `PG_SMALL_PIPELINE_CHUNK` for transfers $< 256\text{ MiB}$ tensor / segment $< 64\text{ MiB}$ `PG_ADAPTIVE_CHUNK_THRESHOLD` to eliminate pipeline startup/drain bubbles, and 256 KiB `PG_PIPELINE_CHUNK` for large payloads $\ge 256\text{ MiB}$ to minimize descriptor overhead). Both chunk sizing and protocol selection are derived from `total_bytes`, guaranteeing symmetric behavior across remainder boundaries. Micro-chunks flow through a sliding window (`rdma_window = 32`, typed `uint32_t`) with selective CQ completion signaling (`PG_RDMA_SIGNAL_INTERVAL = 8`, dynamically bounded by $\min(\text{signal\_interval}, \text{window})$).

### Staging Buffer
An internal memory buffer aligned to `PG_CACHELINE_ALIGN_BYTES` (64 bytes) or `PG_HUGEPAGE_ALIGN_BYTES` (2 MiB for buffers $\ge 2\text{ MiB}$) registered with `IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE` used as the target for incoming RDMA Write operations during the Reduce-Scatter phase.

### Work Buffer
An internal staging area used in safe mode (`WORKBUFFER=safe`) to perform out-of-place vector reduction without mutating the caller's input `sendbuf` until the collective completes (aligned to `PG_HUGEPAGE_ALIGN_BYTES` when $\ge 2\text{ MiB}$).

### Collective Operations
- **Reduce-Scatter**: Reduces an array of data across all processes and distributes the reduced slices across the ranks.
- **All-Gather**: Gathers distributed slices from all ranks so that every rank ends up with the complete contiguous array (zero-copy RDMA Write).
- **All-Reduce**: Performs a full global reduction and distributes the complete reduced result to all ranks (implemented as Reduce-Scatter &rarr; Three-Phase Distributed Barrier &rarr; All-Gather, guaranteeing phase synchronization and race-freedom across rapid back-to-back collective iterations).

---

## The Six Architectural Modules

1. **Module 1: TCP Bootstrap & CLI Topology**: Command-line argument parsing, edge-ordered non-blocking TCP handshake, and peer QP parameter exchange.
2. **Module 2: Verbs Hardware & QP Lifecycle**: InfiniBand device context opening, Protection Domain (PD), shared Completion Queue (CQ), RC Queue Pair initialization with inline stepdown probing, transition to RTS, and resource cleanup (`pg_rdma_cleanup`).
3. **Module 3: Memory Registration & Staging Cache**: Lazy MR registration cache (`pg_mr_cache`), grow-only staging buffer allocation, 2 MB hugepage alignment (`PG_HUGEPAGE_ALIGN_BYTES`), and safe work buffer lifecycle.
4. **Module 4: Progress Engine & CQ Dispatch**: Unified CQ polling, `wr_id` bit-packing/decoding, unexpected control message queueing via dynamic pointers, DMA memory barrier, and receive pool replenishment. Implemented as zero-cost inlines in `pg_internal.h`.
5. **Module 5: SSE4.2 Vector Reduction Compute Kernels**: 128-bit SIMD reduction kernels with 4x loop unrolling across 12 datatype $\times$ operation combinations on Intel Nehalem CPUs.
6. **Module 6: Ring Step Transfer & Collectives Orchestration**: Pipelined Rendezvous / Eager step transmission (`pg_ring_step_transfer`), 3-phase distributed barrier synchronization, and `pg_reduce_scatter`, `pg_all_gather`, `pg_all_reduce` API implementations.

---

## Architectural Seams & Invariants

1. **Progress Seam**: All CQ interactions, `wr_id` decoding, DMA memory barriers, and receive pool refills are strictly encapsulated inside the Progress Engine module (`pg_internal.h`). Collective routines use declarative wait helpers (`pg_progress_wait_msg`, `pg_progress_wait_type`, `pg_progress_wait_send_recv`) and never interact directly with raw CQ polling.
2. **Transfer Seam**: Protocol selection (Eager Send/Recv vs Rendezvous RDMA Write) is encapsulated behind the step transfer engine (`pg_ring_step_transfer`), keeping collective routines focused purely on segment permutation and compute kernels.
3. **Memory Registration Invariant**: Application and staging memory are lazily registered in the MR cache and persist until `pg_close`, avoiding registration churn in the hot timed path.
4. **Barrier Isolation Invariant**: Collective phases (Reduce-Scatter and All-Gather) are decoupled by an unconditional 3-phase distributed ring barrier (`COLLECT` $\to$ `RELEASE` $\to$ `ACK`), preventing faster ranks from lapping slower ranks during rapid back-to-back collective iterations. Unexpected subsequent-iteration traffic is preserved in `pending_q` rather than purged.
5. **Symmetric Micro-Chunk Invariant**: In non-divisible remainder distributions, all ranks derive protocol mode and chunk granularity from `total_bytes` rather than local segment size, preventing deadlocks from asymmetric micro-chunk boundaries.

---

## Architectural Decision Records (ADRs)

- [ADR-0001: wr_id bit-packing and progress-engine dispatch](docs/adr/0001-wr-id-progress-engine.md)
- [ADR-0002: MR cache and buffer lifecycle](docs/adr/0002-mr-buffer-lifecycle.md)
- [ADR-0003: Empirical eager threshold and adaptive protocol selection](docs/adr/0003-eager-threshold-and-adaptive-selection.md)
- [ADR-0004: SSE4.2 SIMD vector reduction and multi-WR batching](docs/adr/0004-simd-vectorization-and-wr-batching.md)
- [ADR-0005: MPI remainder partitioning and ring step permutations](docs/adr/0005-mpi-remainder-and-ring-step-permutation.md)
- [ADR-0006: Pipelined windowing and selective CQ signaling](docs/adr/0006-pipelined-windowing-and-selective-signaling.md)
- [ADR-0007: Three-phase distributed ring barrier](docs/adr/0007-three-phase-distributed-barrier.md)
