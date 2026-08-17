# Domain Model & Ubiquitous Language

This document establishes the ubiquitous language and domain glossary for the RDMA Ring Collective library.

---

## Core Domain Concepts

### Process Group
The logical set of participating processes (ranks $0 \dots N-1$) connected in a deterministic ring topology. Each rank communicates directly with its two immediate ring neighbors: `next_rank = (rank + 1) % size` and `prev_rank = (rank - 1 + size) % size`.

### Progress Engine
The deep module responsible for driving Verbs Completion Queue (`ibv_cq`) polling, `wr_id` dispatch, unexpected control message queueing (`pending_q`), and automatic receive buffer pool replenishment (`pg_repost_recv_slot`). It presents a zero-cost event interface to higher-level collective routines.

### Ring Step Transfer
A single phase of pipelined data movement between adjacent ring neighbors during a collective algorithm. In an $N$-rank ring, collectives execute $N-1$ step transfers where each rank concurrently transmits an outbound segment to `next_rank` while receiving an inbound segment from `prev_rank`.

### Segment
A major slice of the collective payload assigned to or owned by a specific rank. For non-divisible buffer sizes, segment lengths and byte offsets are calculated using MPI-style remainder distribution (`count / size` with remainder distributed to lower ranks).

### Micro-Chunk
A pipelined subdivision of a segment (e.g. 256 KiB or 64 KiB) used to overlap RDMA network transfer with CPU vector computation. Micro-chunks flow through a sliding window (`rdma_window` or `eager_window`) with selective CQ completion signaling.

### Staging Buffer
An internal, page-aligned (64-byte aligned) memory buffer registered with `IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE` used as the target for incoming RDMA Write operations during the Reduce-Scatter phase.

### Work Buffer
An internal staging area used in safe mode to perform out-of-place vector reduction without mutating the caller's input `sendbuf` until the collective completes.

### Collective Operations
- **Reduce-Scatter**: Reduces an array of data across all processes and distributes the reduced slices across the ranks.
- **All-Gather**: Gathers distributed slices from all ranks so that every rank ends up with the complete contiguous array.
- **All-Reduce**: Performs a full global reduction and distributes the complete reduced result to all ranks (implemented as Reduce-Scatter &rarr; Barrier &rarr; All-Gather).

---

## Architecture Seams & Invariants

1. **Progress Seam**: All CQ interactions and receive pool refills are encapsulated inside the Progress Engine module. Collective logic never interacts directly with raw CQ polling or manual pending queue buffering.
2. **Transfer Seam**: Protocol selection (Eager Send/Recv vs Rendezvous RDMA Write) is encapsulated behind the step transfer module, keeping collective routines focused purely on segment permutation and compute kernels.
3. **Memory Registration Invariant**: Application memory is lazily registered in the MR cache and persists until `pg_close`, avoiding registration churn in the hot timed path.
