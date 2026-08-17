# Domain Model & Ubiquitous Language

This document establishes the ubiquitous language, architectural seams, and domain glossary for the RDMA Ring Collective library.

---

## Core Domain Concepts

### Process Group
The logical set of participating processes (ranks $0 \dots N-1$) connected in a deterministic ring topology. Each rank communicates directly with its two immediate ring neighbors: `next_rank = (rank + 1) % size` and `prev_rank = (rank - 1 + size) % size`.

### Progress Engine (Seam #1)
The deep module responsible for driving Verbs Completion Queue (`ibv_cq`) polling, `wr_id` dispatch, unexpected control message queueing (`pending_q`), and automatic receive buffer pool replenishment (`pg_repost_recv_slot`). It presents a zero-cost event interface to higher-level collective routines.

### Ring Step Transfer (Seam #2)
A single phase of pipelined data movement between adjacent ring neighbors during a collective algorithm. In an $N$-rank ring, collectives execute $N-1$ step transfers where each rank concurrently transmits an outbound segment to `next_rank` while receiving an inbound segment from `prev_rank`.

### Segment
A major slice of the collective payload assigned to or owned by a specific rank. For non-divisible buffer sizes, segment lengths and byte offsets are calculated using MPI-style remainder distribution ($(Q+1)/Q$ distribution where rank $i < R$ receives $Q+1$ elements and rank $i \ge R$ receives $Q$ elements).

### Micro-Chunk
A pipelined subdivision of a segment (256 KiB optimal on cluster) used to overlap RDMA network transfer with CPU vector computation. Micro-chunks flow through a sliding window (`rdma_window = 32`) with selective CQ completion signaling (`PG_RDMA_SIGNAL_INTERVAL = 8`).

### Staging Buffer
An internal, 64-byte cache-aligned memory buffer registered with `IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE` used as the target for incoming RDMA Write operations during the Reduce-Scatter phase.

### Work Buffer
An internal staging area used in safe mode (`WORKBUFFER=safe`) to perform out-of-place vector reduction without mutating the caller's input `sendbuf` until the collective completes.

### Collective Operations
- **Reduce-Scatter**: Reduces an array of data across all processes and distributes the reduced slices across the ranks.
- **All-Gather**: Gathers distributed slices from all ranks so that every rank ends up with the complete contiguous array (zero-copy RDMA Write).
- **All-Reduce**: Performs a full global reduction and distributes the complete reduced result to all ranks (implemented as Reduce-Scatter &rarr; 3-Phase Distributed Barrier &rarr; All-Gather).

---

## The Six Architectural Modules

1. **Module 1: TCP Bootstrap & CLI Topology**: Command-line argument parsing, edge-ordered non-blocking TCP handshake, and peer QP parameter exchange.
2. **Module 2: Verbs Hardware & QP Lifecycle**: InfiniBand device context opening, Protection Domain (PD), shared Completion Queue (CQ), RC Queue Pair initialization with inline stepdown probing, and transition to RTS.
3. **Module 3: Memory Registration & Staging Cache**: Lazy MR registration cache (`pg_mr_cache`), grow-only staging buffer allocation, and safe work buffer lifecycle.
4. **Module 4: Progress Engine & CQ Dispatch**: Unified CQ polling, `wr_id` bit-packing/decoding, unexpected control message queueing, and receive pool replenishment.
5. **Module 5: SSE4.2 Vector Reduction Compute Kernels**: 128-bit SIMD reduction kernels with 4x loop unrolling across 12 datatype $\times$ operation combinations on Intel Nehalem CPUs.
6. **Module 6: Ring Step Transfer & Collectives Orchestration**: Pipelined Rendezvous / Eager step transmission, 3-phase distributed barrier synchronization, and `pg_reduce_scatter`, `pg_all_gather`, `pg_all_reduce` API implementations.

---

## Architectural Seams & Invariants

1. **Progress Seam**: All CQ interactions, `wr_id` decoding, and receive pool refills are strictly encapsulated inside the Progress Engine module. Collective routines never interact directly with raw CQ polling.
2. **Transfer Seam**: Protocol selection (Eager Send/Recv vs Rendezvous RDMA Write) is encapsulated behind the step transfer engine (`pg_step_transfer_*`), keeping collective routines focused purely on segment permutation and compute kernels.
3. **Memory Registration Invariant**: Application and staging memory are lazily registered in the MR cache and persist until `pg_close`, avoiding registration churn in the hot timed path.
4. **Barrier Isolation Invariant**: Collective phases (Reduce-Scatter and All-Gather) are decoupled by a 3-phase distributed ring barrier (`COLLECT` $\to$ `RELEASE` $\to$ `ACK`) ensuring in-flight CQ events are completely drained.

---

## Architectural Decision Records (ADRs)

- [ADR-0001: wr_id bit-packing and progress-engine dispatch](docs/adr/0001-wr-id-progress-engine.md)
- [ADR-0002: MR cache and buffer lifecycle](docs/adr/0002-mr-buffer-lifecycle.md)
- [ADR-0003: Empirical eager threshold and adaptive protocol selection](docs/adr/0003-eager-threshold-and-adaptive-selection.md)
- [ADR-0004: SSE4.2 SIMD vector reduction and multi-WR batching](docs/adr/0004-simd-vectorization-and-wr-batching.md)
- [ADR-0005: MPI remainder partitioning and ring step permutations](docs/adr/0005-mpi-remainder-and-ring-step-permutation.md)
- [ADR-0006: Pipelined windowing and selective CQ signaling](docs/adr/0006-pipelined-windowing-and-selective-signaling.md)
- [ADR-0007: Three-phase distributed ring barrier](docs/adr/0007-three-phase-distributed-barrier.md)
