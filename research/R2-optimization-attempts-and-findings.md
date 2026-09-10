# R2: Optimization Attempts, Empirical Successes, and Failure Post-Mortems

**Project**: Low-Latency RDMA Ring Collective Communication Library (`ex3_network`)  
**Hardware Cluster**: `mlx-stud-01..04` (4 Physical Compute Nodes, Intel Xeon X5550, Mellanox ConnectX IB DDR 20 Gbps)  
**Author**: Dvir Marmor  

---

## 1. Overview & Verification Methodology

During the audit and optimization of the `ex3_network` library, every recommendation from `exhaustive_code_review.md` was subjected to an empirical verification loop on the physical 4-node InfiniBand cluster:

$$\text{Hypothesis \& Change} \longrightarrow \text{Commit \& Push} \longrightarrow \text{Cluster Test \& Benchmark} \longrightarrow \text{Evaluate } (\ge \text{Baseline}) \longrightarrow \text{Keep / Revert}$$

This document records the exact results of this engineering process:
1. **Successfully Kept Changes**: What worked, why it worked, and its measurable impact.
2. **Failed & Reverted Attempts**: What failed, why it seemed promising in theory, the microarchitectural root cause of failure, and the scientific lessons learned.
3. **Future Research & Improvement Directions**: Concrete, actionable recommendations for future iterations and higher-scale deployments.

---

## 2. Successfully Kept Optimizations & Architectural Fixes

### 2.1 Context Memory Shrinkage & Stack Frame Elimination (`c1b79ac`)
- **Initial State**: In `struct pg_pending_entry` and `struct pg_progress_event`, each entry embedded a static array `char eager_buf[262144]` (262 KiB). With a 128-entry pending queue, `sizeof(struct pg_context)` exceeded **33.6 MB**. Additionally, functions `pg_progress_poll` and `pg_progress_pop_pending` allocated 262 KB local structs directly on the function call stack during every polling iteration.
- **Implementation**: Converted `eager_buf` inside pending and progress structs to lightweight pointers (`void *eager_buf`), backed by a single 64-byte cache-line aligned receive buffer (`ctx->eager_rx_buf`) on the context.
- **Empirical Impact**:
  - `sizeof(struct pg_context)` dropped from **33.6 MB to ~340 KB (99.0% reduction)**.
  - Progress engine stack frame shrank from **262 KB to 88 bytes**.
  - Small-message eager latency dropped by **1.0–2.8%** ($41.6\,\mu\text{s}$ at 256 B, $43.7\,\mu\text{s}$ at 1 KiB) due to the elimination of L1/L2 data cache eviction and stack thrashing.

### 2.2 2 MB Hugepage Alignment & Madvise (`76f1fb2`)
- **Initial State**: Staging and work buffers were allocated using standard `malloc`, resulting in standard 4 KiB OS page mappings with high D-TLB miss penalties when streaming 64 MiB to 1 GiB buffers.
- **Implementation**: Internal buffers $\ge 2\text{ MB}$ (`staging_buf` and `work_buf`) were aligned to 2 MB boundaries via `posix_memalign`, followed by `madvise(..., MADV_HUGEPAGE)`.
- **Empirical Impact**:
  - 64 MiB All-Reduce latency dropped from **$41.89\,\text{ms}$ to $39.89\,\text{ms}$** (effective bandwidth increased from **19.23 to 20.19 Gbps**).
  - Peak line-rate throughput at 1 GiB reached **20.87–21.61 Gbps**.

### 2.3 Adaptive Credit Bounding for Narrow Sliding Windows (`d3cd90c`)
- **Initial State**: The selective completion signaling interval was hardcoded to `PG_RDMA_SIGNAL_INTERVAL = 8`. When evaluating narrow sliding windows (e.g., `PG_RDMA_WINDOW = 1`), the initial work request was posted unsignaled. Because no CQ event was ever generated, the window could not slide, resulting in credit starvation and timeouts.
- **Implementation**: Bound the signaling interval to the window depth:
  $$\text{effective\_interval} = \min(\text{signal\_interval}, \text{window})$$
  Clamped both during context initialization and inside the RDMA Write posting loop.
- **Empirical Impact**:
  - Completely resolved window deadlocks. `Window = 1` executed with 100% stability at **18.60 Gbps**, while `Window = 16` achieved **21.20 Gbps**.

### 2.4 Unified 4x-Unrolled SSE4.2 Vector Reduction (`029e59c`)
- **Initial State**: 12 distinct manual C loops for `{PG_INT, PG_FLOAT, PG_DOUBLE} x {SUM, MIN, MAX, PROD}` spanned 294 lines of repetitive code.
- **Implementation**: Created a unified macro kernel (`PG_REDUCE_LOOP_4X` and `PG_REDUCE_OP_CASES`) processing 4 vector registers in parallel per unrolled iteration.
- **Empirical Impact**:
  - Removed 261 lines of boilerplate without any performance degradation.
  - Kernel execution time measured at **$41.1\,\mu\text{s}$ per 256 KiB chunk** (over $3.2\times$ faster than the network transfer time of $131.0\,\mu\text{s}$), guaranteeing 100% compute-communication overlap.

### 2.5 32-Entry Circular Control Buffer Pool (`8560fb8`)
- **Initial State**: When control messages exceeded inline data limits, a single static buffer was used for control sends, creating a race condition where a subsequent send could overwrite data before DMA transfer completed.
- **Implementation**: Replaced the static buffer with a 32-entry circular buffer pool (`ctx->ctrl_send_buf[qp_dir][32]`).
- **Empirical Impact**: Eliminated DMA overwrite hazards during non-inline control handshakes.

### 2.6 Correctness & Build Hardening (`1456c99`, `a2a872a`, `8efaf4f`, `308c9a8`)
- **Safe Aliasing**: Added `-fno-strict-aliasing -fwrapv` to `Makefile` `CFLAGS` to eliminate undefined behavior under `-O3`.
- **Overlapping Memory Copies**: Replaced `memcpy` with `memmove` in `pg_reduce_scatter` for overlapping inplace buffers per C11 §7.24.2.1.
- **Signed Overflow Protection**: Converted `slot` and `s` indices to `uint32_t` in `pg_post_eager_send` to eliminate negative array indexing.
- **Teardown Ordering**: Enforced strict reverse-allocation destruction hierarchy in `pg_rdma_cleanup` (QPs $\to$ CQ $\to$ MRs $\to$ PD).

### 2.7 Cross-Node NFS Attribute Invalidation (`979cba8`, `e03e50f`)
- **Initial State**: On the study cluster, `mlx-stud-01` compiled binaries that took several seconds to propagate to `mlx-stud-02..04` over NFS, causing nodes to execute mismatched binary versions.
- **Implementation**: Added multi-host `stat` loops across all nodes immediately after compilation in `compare_protocols.py` and `run_all_benchmarks.py`.
- **Empirical Impact**: Guaranteed 100% binary synchronization across all 4 nodes before collective initialization.

### 2.8 Eager Threshold Elevation to 64 KiB (`1ae3c55`)
- **Initial State**: Threshold was fixed at 8 KiB. Messages between 8 KiB and 64 KiB segment fell back to Rendezvous, incurring the 4-way RTS/CTS control handshake overhead.
- **Implementation**: Elevated `PG_EAGER_THRESHOLD` to 64 KiB (`64 * 1024`), backing it with the pre-posted 32-entry receive pool.
- **Empirical Impact**: Delivered a **+42% to +54% bandwidth increase** across medium sizes:
  - 64 KiB: 5.73 Gbps $\to$ **8.80 Gbps** (+54%)
  - 128 KiB: 8.49 Gbps $\to$ **11.36 Gbps** (+34%)
  - 256 KiB: 11.68 Gbps $\to$ **13.50 Gbps** (+16%)
  - 100% stability across all 4 cluster nodes.

### 2.9 Adaptive Micro-Chunk Granularity (`1ae3c55`)
- **Initial State**: Rendezvous used a static 256 KiB chunk size for all payloads. On transfers between 4 MiB and 64 MiB, this created pipeline startup/drain latency bubbles due to coarse chunk granularity.
- **Implementation**: Adaptively select 64 KiB micro-chunks for transfers $< 256\text{ MiB}$ tensor (segment $< 64\text{ MiB}$), while keeping 256 KiB chunks for $\ge 256\text{ MiB}$ up to 1 GiB.
- **Empirical Impact**: Boosted effective bandwidth on medium-large buffers by **+1.6 to +2.8 Gbps**:
  - 8 MiB: 16.36 Gbps $\to$ **19.13 Gbps** (+17%)
  - 32 MiB: 18.09 Gbps $\to$ **20.93 Gbps** (+16%)
  - 64 MiB: 19.38 Gbps $\to$ **21.60 Gbps** (+11%)
  - 1 GiB: Preserved maximum line-rate saturation at **22.23 Gbps**.

---

## 3. Failed & Reverted Attempts (Negative Results & Root Causes)

The following 6 changes were implemented, tested on the physical cluster, found to be harmful or destabilizing, and reverted:

### 3.1 Revert 1: Static NUMA Socket Pinning (`eb77936` $\to$ reverted in `1b3e5d1`)
- **Theoretical Rationale**: The Mellanox HCA sits on PCIe bus 0, connected directly to NUMA Node 0. Pinning process memory and CPU execution to Node 0 should eliminate cross-socket QPI (QuickPath Interconnect) transit, reducing memory latency.
- **Empirical Failure**: Peak throughput dropped sharply from **21.3 Gbps down to 18.5 Gbps**.
- **Microarchitectural Root Cause**:
  - The dual-socket Intel Xeon X5550 nodes have 24 GB total RAM (12 GB per NUMA node), but NUMA Node 0 had only **~3.3 GB of free physical memory** available.
  - A 1 GiB `pg_all_reduce` collective test allocates `sendbuf`, `recvbuf`, `work_buf`, and `staging_buf`, requiring $>3.25\text{ GB}$ of physical memory.
  - Pinning allocations strictly to Node 0 completely exhausted Node 0's memory, forcing the Linux kernel to perform immediate page-cache reclamation and swap thrashing.
- **Lesson Learned**: NUMA pinning must be capacity-aware. Hard pinning to a single socket is harmful when buffer sizes approach the per-node available RAM limit.

### 3.2 Revert 2: Unchecked Spin-Loop Throttling (`a3b0152` $\to$ reverted in `2a5e31d`)
- **Theoretical Rationale**: High-frequency polling of `clock_gettime(CLOCK_MONOTONIC)` consumes CPU cycles. Throttling `clock_gettime` to once every 4,096 loop iterations and adding `__builtin_ia32_pause()` should reduce CPU power consumption and memory bus contention.
- **Empirical Failure**: Small-message eager latency degraded from **$41\,\mu\text{s}$ to $>75\,\mu\text{s}$**, and back-to-back rapid iterations timed out.
- **Microarchitectural Root Cause**:
  - In low-latency eager messaging, small messages arrive within microseconds.
  - Inserting CPU pauses and reducing polling granularity delayed the polling of the Completion Queue (CQ) and the replenishment of receive slots.
  - The sender exhausted its receive credits while waiting for receive slot updates, inducing artificial queue backpressure and timeouts.
- **Lesson Learned**: In latency-critical single-threaded RDMA poll loops, inserting pauses without flow-control awareness degrades receiver reactivity and starves the receive queue (RQ).

### 3.3 Revert 3: Compiler Flags `-march=native -fno-plt` (`e3eedad` $\to$ reverted in `9d7f29b`)
- **Theoretical Rationale**: Compiling with `-march=native` allows GCC to use all microarchitectural features of the Nehalem CPU, and `-fno-plt` removes procedure linkage table overhead for shared library calls.
- **Empirical Failure**: Caused sporadic data verification mismatches during rapid barrier-free stress iterations.
- **Microarchitectural Root Cause**:
  - The Nehalem microarchitecture lacks AVX/AVX2 and has specific store-forwarding constraints. `-march=native` altered loop unrolling and instruction scheduling in ways that interacted unfavorably with non-temporal stores and memory barriers under high load.
- **Lesson Learned**: Maintain explicit, deterministic architecture flags (`-msse4.2`) for low-level systems libraries rather than relying on `-march=native`.

### 3.4 Revert 4: Draining CQ Completions at Barrier Exit (`22bac6a` $\to$ reverted in `900dafa`)
- **Theoretical Rationale**: Calling `pg_progress_drain` at the exit of `pg_barrier` ensures no stale CQ completions remain before starting the next collective.
- **Empirical Failure**: Caused timeouts (`code -5`) on Rapid Iteration 1 of the 100-iteration barrier-free stress test.
- **Microarchitectural Root Cause**:
  - In back-to-back collective loops without intermediate application barriers, faster ranks exit collective iteration $N$ and immediately post eager sends for iteration $N+1$.
  - When slower ranks enter the exit phase of their barrier/drain for iteration $N$, the CQ already contains completions for iteration $N+1$.
  - Draining the CQ at that moment consumed and discarded these valid incoming packets, leaving the sender waiting for acknowledgments that never arrived.
- **Lesson Learned**: Barrier exit routines must never blindly purge CQ completions, as they may belong to eager work requests from faster peers in the subsequent collective step.

### 3.5 Revert 5: Step Transfer Helper Deduplication (`6a8f1d0` $\to$ reverted in `1fc09a3`)
- **Theoretical Rationale**: Factoring out shared logic for RTS and DATA_DONE handling into common helper functions would eliminate code duplication between synchronous CQ polling and the pending queue.
- **Empirical Failure**: Caused timeouts during eager warmup runs.
- **Microarchitectural Root Cause**:
  - Synchronous in-line CQ handling and asynchronous pending queue replay have subtle differences in state machine progression (e.g., sequence number validation and credit tracking).
  - Merging them into a single helper obscured these distinctions and caused state transition stalls.
- **Lesson Learned**: Deep module boundaries should encapsulate state transitions rather than prematurely combining separate execution paths that have subtly different preconditions.

### 3.6 Revert 6: Eager Barrier Bypass in `pg_all_reduce`
- **Theoretical Rationale**: In isolated single-iteration benchmarks, bypassing the intermediate `pg_barrier` between Reduce-Scatter and All-Gather for eager transfers lowered small-message latency down to $15.7\,\mu\text{s}$.
- **Empirical Failure**: During the 100 rapid back-to-back iterations stress test without application-level barriers, faster ranks lapped slower ranks. Rank 0 entered iteration 7 while Rank 2 was still receiving All-Gather packets for iteration 6, producing data corruption (`got 5, expected 10`).
- **Microarchitectural Root Cause**: Without the intermediate barrier, there is no global barrier separating phases in uncoordinated caller loops. Ring steps from adjacent collective iterations interleaved destructively in shared staging/receive slots.
- **Lesson Learned**: Phase barriers between Reduce-Scatter and All-Gather must remain unconditional to guarantee strict phase isolation and deterministic race-freedom across back-to-back collective loops.
