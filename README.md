# RDMA Ring Collectives — Low-Latency Communication Library

High-performance, single-threaded RDMA collective communication library implementing **Reduce-Scatter**, **All-Gather**, and **All-Reduce** over InfiniBand Reliable Connected (RC) Queue Pairs using the `libibverbs` API.

Designed and tuned on a 4-node InfiniBand cluster (`mlx-stud-01..04`), achieving **22.23 Gbps** peak Rendezvous and **21.57 Gbps** AUTO effective bandwidth with direct zero-copy eager CPU receive, 4.19 MiB compact receive pools, and pipelined zero-copy RDMA execution.

---

## 1. Architecture Overview

```
+-------------------------------------------------------------------------------+
|                             PUBLIC API LAYER                                  |
|   connect_process_group()   pg_reduce_scatter()   pg_all_gather()             |
|   pg_all_reduce()           pg_close()                                        |
+---------------------------------------+---------------------------------------+
                                        |
+---------------------------------------v---------------------------------------+
|                    COLLECTIVE ORCHESTRATION ENGINE                            |
|   - MPI Remainder Slicing ((Q+1)/Q)       - Epoch-Gated Phase Handoff         |
|   - Ring Permutation Step Math            - Safe Workbuf / Zero-Copy Staging  |
+-------------------+---------------------------------------+-------------------+
                    |                                       |
+-------------------v-------------------+   +---------------v-------------------+
|     PROGRESS ENGINE (SEAM #1)         |   |    STEP TRANSFER (SEAM #2)        |
| - wr_id Bit-Packing & CQ Polling      |   | - Eager 2-SGE Scatter-Gather SEND |
| - Unexpected Message Pending Queue    |   | - 4-Way Rendezvous RDMA Write     |
| - Auto Receive Slot Replenishment     |   | - Pipelined Window Flow Control   |
+-------------------+-------------------+   +---------------+-------------------+
                    |                                       |
+-------------------v---------------------------------------v-------------------+
|               SSE4.2 128-BIT VECTOR REDUCTION (4x UNROLLED)                   |
|   12 combinations: {PG_INT, PG_FLOAT, PG_DOUBLE} x {SUM, MIN, MAX, PROD}       |
+-------------------------------------------------------------------------------+
|                       HARDWARE & VERBS TRANSPORT                              |
|   - 2 RC QPs (qp_to_next, qp_from_prev)   - Shared Completion Queue (CQ)     |
|   - Lazy MR Registration Cache            - Inline Payload Probing            |
+-------------------------------------------------------------------------------+
```

### Core Design Principles
1. **Zero-Copy Memory Placement**: All-Gather writes directly into the caller's registered `recvbuf` via RDMA Write.
2. **Compute-Communication Overlap**: Adaptive 64 KiB / 256 KiB micro-chunks pipeline network transmission with 128-bit SSE4.2 SIMD vector reduction.
3. **Adaptive Protocol Switching**: Automatically switches between Eager Send/Recv ($\le 64\text{ KiB}$ segment / $\le 256\text{ KiB}$ tensor) for low latency and Pipelined Rendezvous ($> 64\text{ KiB}$) for line-rate throughput.
4. **Deadlock-Free Bootstrap**: Edge-ordered TCP connection initialization ensures deterministic ring setup across arbitrary rank counts.
5. **Ordered Barrier-Free Handoff**: Each RC QP carries one blocking collective stream. Strict head-only pending queues and segment tags preserve transport order while local completion advances the phase.

### Buffer Contract

- `pg_reduce_scatter` and `pg_all_reduce` require disjoint input and output ranges. With the default `WORKBUFFER=inplace`, `sendbuf` is mutable scratch.
- `pg_all_gather` accepts disjoint ranges or `sendbuf` pointing exactly at the calling rank's owned slice inside `recvbuf`; other overlap is invalid.

---

## 2. Interactive Protocol & Workflow Diagrams

### 2.1 Ring Topology & Edge-Ordered TCP Bootstrap
Ranks establish TCP sockets sequentially to exchange InfiniBand QP parameters (`QPN`, `PSN`, `LID`) without circular deadlocks:

```mermaid
sequenceDiagram
    autonumber
    participant R0 as Rank 0 (mlx-stud-01)
    participant R1 as Rank 1 (mlx-stud-02)
    participant R2 as Rank 2 (mlx-stud-03)
    participant R3 as Rank 3 (mlx-stud-04)

    Note over R0,R3: Edge-Ordered TCP Ring Exchange (Port = 19000 + Rank)
    R0->>R1: Connect to Next & Send QP Info
    R1->>R2: Connect to Next & Send QP Info
    R2->>R3: Connect to Next & Send QP Info
    R3->>R0: Connect to Next & Send QP Info (Ring Close)

    Note over R0,R3: InfiniBand QP State Transition (RESET -> INIT -> RTR -> RTS)
    R0-->>R1: RDMA Control Ring Ping-Pong
    R1-->>R2: RDMA Control Ring Ping-Pong
    R2-->>R3: RDMA Control Ring Ping-Pong
    R3-->>R0: RDMA Control Ring Ping-Pong
```

---

### 2.2 Four-Way Rendezvous Control Handshake
Used for large transfers ($> 64\text{ KiB}$ segment) to grant remote write permissions directly into registered destination memory:

```mermaid
sequenceDiagram
    autonumber
    participant S as Sender (Rank r)
    participant R as Receiver (Rank (r+1)%N)

    Note over S,R: 1. Request to Send (RTS)
    S->>R: IBV_WR_SEND: RTS (seg_idx, micro_idx, byte_len)
    
    Note over S,R: 2. Clear to Send (CTS) with Remote Memory Key
    R->>S: IBV_WR_SEND: CTS (remote_addr, rkey, micro_idx)

    Note over S,R: 3. Zero-Copy RDMA Write (Payload Transfer)
    S->>R: IBV_WR_RDMA_WRITE (addr, rkey, len=256 KiB)

    Note over S,R: 4. Data Done Confirmation
    S->>R: IBV_WR_SEND: DATA_DONE (seg_idx, micro_idx)
    Note over R: Trigger SSE4.2 SIMD Reduction on Received Chunk
```

---

### 2.3 Eager 2-SGE Scatter-Gather & Direct Zero-Copy CPU Receive
Used for small/medium transfers ($\le 64\text{ KiB}$ segment / $\le 256\text{ KiB}$ tensor) to eliminate the 2-RTT RTS/CTS handshake and push payload directly into pre-posted receive buffers with direct zero-copy CPU consumption:

```mermaid
sequenceDiagram
    autonumber
    participant S as Sender (Rank r)
    participant R as Receiver (Rank (r+1)%N)

    Note over R: Compact 4.19 MiB Receive Pool (32 slots of 65.6 KiB)
    Note over S: 2-SGE Send: SGE[0]=Unified Ctrl Header + SGE[1]=User Payload
    S->>R: IBV_WR_SEND (Atomic single-transmission push)
    Note over R: NIC DMAs header + payload into recv_slot_buf
    Note over R: Direct Zero-Copy: CPU reduces/copies straight from slot buffer!
    Note over R: Deferred Repost: Recv slot returned to RQ after processing
```

---

### 2.4 Pipelined Micro-Chunk Execution Timeline
Demonstrating concurrent communication and computation on a 4-node cluster:

```mermaid
gantt
    title Pipelined Reduce-Scatter Step (256 KiB Micro-Chunks)
    dateFormat X
    axisFormat %s

    section Rank r (Sender)
    RDMA Write Chunk 0   :active, s0, 0, 13
    RDMA Write Chunk 1   :active, s1, 13, 26
    RDMA Write Chunk 2   :active, s2, 26, 39
    RDMA Write Chunk 3   :active, s3, 39, 52

    section Rank r+1 (Receiver)
    Recv Chunk 0         :done, r0, 0, 13
    SIMD Reduce Chunk 0  :crit, c0, 13, 17
    Recv Chunk 1         :done, r1, 13, 26
    SIMD Reduce Chunk 1  :crit, c1, 26, 30
    Recv Chunk 2         :done, r2, 26, 39
    SIMD Reduce Chunk 2  :crit, c2, 39, 43
    Recv Chunk 3         :done, r3, 39, 52
    SIMD Reduce Chunk 3  :crit, c3, 52, 56
```

---

## 3. Mathematical Derivations

### 3.1 Remainder Partitioning for Arbitrary Buffer Sizes
When total element count $C$ is not divisible by rank count $N$:
- Base segment count: $Q = \lfloor C / N \rfloor$
- Remainder count: $R = C \bmod N$

Each rank $i \in [0, N-1]$ owns a deterministic segment slice:

$$\text{count}(i) = \begin{cases} Q + 1 & \text{if } i < R \\ Q & \text{if } i \ge R \end{cases}$$

$$\text{offset}(i) = \begin{cases} i \times (Q + 1) & \text{if } i < R \\ R \times (Q + 1) + (i - R) \times Q & \text{if } i \ge R \end{cases}$$

$$\sum_{i=0}^{N-1} \text{count}(i) = R(Q + 1) + (N - R)Q = NQ + R = C$$

This guarantees 100% data coverage with zero padding or memory out-of-bounds errors.

### 3.2 Reduce-Scatter Segment Permutation
In an $N$-rank ring, Reduce-Scatter executes $N-1$ steps ($k \in [0, N-2]$).
At step $k$:
- Outbound segment transmitted to `next_rank`:
  $$s_{\text{out}}(k) = (\text{rank} - k - 1 + N) \bmod N$$
- Inbound segment received from `prev_rank`:
  $$s_{\text{in}}(k) = (\text{rank} - k - 2 + N) \bmod N$$

**Proof of Invariant**: After step $k$, rank $r$ holds the partial reduction of segment $s_{\text{in}}(k)$ accumulated across $k+2$ ranks. After $N-1$ steps, each rank $r$ holds the globally reduced slice for segment $r$.

### 3.3 All-Gather Segment Permutation
All-Gather executes $N-1$ steps ($k \in [0, N-2]$).
At step $k$:
- Outbound segment transmitted to `next_rank`:
  $$s_{\text{out}}(k) = (\text{rank} - k + N) \bmod N$$
- Inbound segment received from `prev_rank`:
  $$s_{\text{in}}(k) = (\text{rank} - k - 1 + N) \bmod N$$

Data is written directly into `recvbuf + offset(s_in)` with zero memory copies. After $N-1$ steps, all ranks hold the entire contiguous result.

---

## 4. Empirical Performance Highlights

*Fresh 5-iteration medians measured on the 4-node physical InfiniBand cluster (`mlx-stud-01..04`) on 2026-09-28. A one-element All-Gather aligns ranks immediately before each timed sample and is excluded from the measurement.*

| Size | Eager Latency | Rendezvous Latency | AUTO Latency | AUTO Bandwidth | Fastest Explicit Mode |
| :--- | ---: | ---: | ---: | ---: | :--- |
| **64 B** | **18.8 µs** | 62.4 µs | 17.3 µs | 0.04 Gbps | Eager |
| **64 KiB** | **62.3 µs** | 106.5 µs | 64.4 µs | 12.21 Gbps | Eager |
| **256 KiB** | **198.2 µs** | 218.2 µs | 187.6 µs | 16.77 Gbps | Eager |
| **1 MiB** | **674.0 µs** | 704.8 µs | 740.5 µs | 16.99 Gbps | Eager |
| **8 MiB** | 5.24 ms | **5.23 ms** | 5.25 ms | 19.17 Gbps | Rendezvous |
| **16 MiB** | 10.36 ms | 10.08 ms | **9.98 ms** | 20.17 Gbps | Rendezvous |
| **64 MiB** | — | **36.93 ms** | 37.61 ms | 21.41 Gbps | Rendezvous |
| **1 GiB** | — | **579.53 ms** | 597.43 ms | **21.57 Gbps** | Rendezvous |

> [!TIP]
> For the complete dataset, hyperparameter sensitivity sweeps, and the **Tested vs. Not-Tested Boundary Matrix**, refer to the full [Empirical Protocol Evaluation Report](docs/empirical_protocol_report.md).

---

## 5. Repository Structure & Architectural Modules

```
.
├── CONTEXT.md                    # Ubiquitous language, domain concepts & architectural invariants
├── Makefile                      # Build system with mode flags (auto & inplace defaults)
├── README.md                     # Top-level architectural documentation (this document)
├── assignment.txt                # Exercise specification and constraints
├── compare_protocols.py          # Automated Eager vs Rendezvous comparative runner
├── run_all_benchmarks.py         # Full automated benchmark & sensitivity sweep runner
├── run_cluster_test.sh           # Multi-node cluster execution script
│
├── pg.h                          # Public C API and CLI structures
├── pg_internal.h                 # Internal wire protocols, SIMD dispatch & constants
├── pg.c                          # Core implementation organized in modular sections:
│   ├── MODULE 1: TCP Bootstrap & CLI Topology
│   ├── MODULE 2: Verbs Hardware & QP Lifecycle
│   ├── MODULE 3: Memory Registration & Staging Cache
│   ├── MODULE 5: SSE4.2 Vector Reduction Compute Kernels
│   ├── MODULE 4: Progress Engine & CQ Dispatch (Private Progress Seam)
│   ├── Group Lifecycle & FIFO-Gated Phase Handoff (ADR-0009)
│   └── MODULE 6: Ring Step Transfer & Collectives Orchestration

│
├── main_test.c                   # Validation & benchmark test harness
│
├── research/
│   ├── R1-verbs-patterns.md          # Verbs API patterns for RC Queue Pairs in ring collectives
│   └── R2-optimization-attempts-and-findings.md # Detailed post-mortems of successes & failures
│
└── docs/
    ├── empirical_protocol_report.md  # Detailed benchmark analysis and empirical boundary matrix
    └── adr/                          # Current architectural decision records
        ├── 0001-wr-id-progress-engine.md
        ├── 0002-mr-buffer-lifecycle.md
        ├── 0003-eager-threshold-and-adaptive-selection.md
        ├── 0004-simd-vectorization-and-wr-batching.md
        ├── 0005-mpi-remainder-and-ring-step-permutation.md
        ├── 0006-pipelined-windowing-and-selective-signaling.md
        └── 0009-strict-fifo-phase-handoff.md
```

---

## 6. Build & Execution Guide

### 6.1 Build Configurations
The library compiles cleanly with `gcc` under `-Wall -Wextra -Werror -O3 -std=gnu11 -msse4.2` and defaults to the optimal performance configuration (`MODE=auto`, `WORKBUFFER=inplace`, `PG_PIPELINE_CHUNK=256 KiB`, `PG_RDMA_WINDOW=32`):

```bash
# Default Build (Auto Mode + Inplace Workbuffer + 256 KiB Pipelining)
make

# Compile with Specific Protocol Mode
make MODE=auto       # Dynamic Eager (<=64 KiB segment / <=256 KiB tensor) and Rendezvous (>64 KiB) [Default]
make MODE=rendezvous # Pure Rendezvous RDMA Write
make MODE=eager      # Pure Eager Send/Recv

# Compile with Safe Work Buffer (Preserves sendbuf without mutation)
make WORKBUFFER=safe

# Check pending FIFO ordering, wraparound, and buffer reuse without RDMA hardware
make check
```

All-Reduce uses a strict-FIFO, barrier-free phase handoff. The library contains no dedicated barrier interface or control messages. This deliberately supports one blocking collective stream per RC QP: all ranks must call collectives in the same order, without overlap or retry. Connection and teardown use a symmetric neighbor ping; the benchmark harness uses a one-element public All-Gather before timed samples.

### 6.2 Multi-Node Cluster Execution
Run on the physical InfiniBand cluster (`mlx-stud-01..04`):

```bash
# Run 4-node collective test
bash run_cluster_test.sh 4

# Run 2-node collective test
bash run_cluster_test.sh 2

# Run automated Eager vs Rendezvous comparative sweep
python3 compare_protocols.py --ranks 4

# Run full 23-size benchmark & sensitivity suite
python3 run_all_benchmarks.py
```
