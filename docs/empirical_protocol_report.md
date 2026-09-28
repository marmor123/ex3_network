# RDMA Ring Collectives — Empirical Protocol Evaluation Report

**Project**: Low-Latency RDMA Ring Collective Library (`ex3_network`)  
**Hardware Cluster**: `mlx-stud-01`, `mlx-stud-02`, `mlx-stud-03`, `mlx-stud-04`  
**Author**: Dvir Marmor  

---

## 1. Executive Summary

This report documents the empirical performance characterization of the RDMA Ring Collective communication library across **Eager Send/Receive**, **Windowed Rendezvous RDMA Write**, and **Adaptive Auto** protocols on a physical 4-node InfiniBand cluster.

### Key Empirical Findings
1. **Low-Latency Small Payloads**: Eager measured $18.8\,\mu\text{s}$ at 64 B versus $62.4\,\mu\text{s}$ for Rendezvous, a $3.3\times$ advantage from avoiding the RTS/CTS exchange.
2. **Measured Crossover**: The 2026-09-28 sweep was mildly non-monotonic near the crossover: Rendezvous won at 512 KiB, Eager regained the lead from 1–4 MiB, and Rendezvous led from 8 MiB onward, reaching **22.23 Gbps** at 1 GiB.
3. **Conservative AUTO Policy**: AUTO selects Eager only through a 64 KiB segment (256 KiB total on four ranks). The threshold remains a receive-memory bound rather than an attempt to follow a noisy empirical crossover.
4. **Peak Effective Bandwidth**: AUTO reached **21.57 Gbps** at 1 GiB ($597.43\,\text{ms}$), 3.0% below the explicit Rendezvous build.
5. **Ordered Barrier-Free Harness**: Correctness tests run back-to-back without global fences. Each timed sample is preceded by a one-element All-Gather outside the timed region; strict per-QP FIFO order replaces both the old barrier and explicit epoch/phase tags.

---

## 2. Testbed Hardware & Environment Specifications

| Component | Specification | Notes |
| :--- | :--- | :--- |
| **Cluster Nodes** | `mlx-stud-01`, `mlx-stud-02`, `mlx-stud-03`, `mlx-stud-04` | 4 dedicated physical compute nodes |
| **CPU Model** | Intel(R) Xeon(R) CPU X5550 @ 2.67 GHz | Nehalem microarchitecture (4 cores / 8 threads) |
| **SIMD Support** | SSE, SSE2, SSE3, SSSE3, **SSE4.1, SSE4.2** | **No AVX / AVX2** (hardware constraint) |
| **Memory** | 24 GB DDR3 Registered ECC | 64-byte cache line alignment |
| **NIC / HCA** | Mellanox ConnectX IB HCA (DDR 4X) | 20 Gbps physical link rate |
| **Verbs Driver** | `libibverbs` 1.2.1 / MLNX_OFED | Native RC Queue Pairs, Shared CQ |
| **Filesystem** | Network File System (NFS) | `/cs/usr/ateret.tabib/Downloads/ex3_network` |
| **Execution Method** | WSL $\to$ SSH ControlMaster persistent socket | Deterministic, non-interactive execution |

---

## 3. Protocol Comparison Matrix (4-Node Ring Sweep)

Measurements obtained on `mlx-stud-01..04` on 2026-09-28, performing global `pg_all_reduce` (`PG_INT`, `PG_SUM`) across 5 timed iterations per size. A one-element All-Gather aligns ranks before each sample and is outside the timed interval. These results are not directly comparable to older tables that used the removed three-pass barrier as the pre-sample synchronization mechanism.

| Message Size | Element Count | Eager Latency ($\mu\text{s}$) | Rendezvous Latency ($\mu\text{s}$) | Auto Mode Latency ($\mu\text{s}$) | Eager BW (Gbps) | Rendezvous BW (Gbps) | Auto BW (Gbps) | Optimal Protocol |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **64 B** | 16 | **18.8** | 62.4 | 17.3 | 0.04 | 0.01 | 0.04 | **Eager ($3.3\times$ faster)** |
| **256 B** | 64 | **16.7** | 62.5 | 16.3 | 0.18 | 0.05 | 0.19 | **Eager ($3.7\times$ faster)** |
| **1 KiB** | 256 | **17.2** | 63.7 | 18.3 | 0.71 | 0.19 | 0.67 | **Eager ($3.7\times$ faster)** |
| **2 KiB** | 512 | **20.6** | 63.2 | 18.4 | 1.19 | 0.39 | 1.33 | **Eager ($3.1\times$ faster)** |
| **4 KiB** | 1,024 | **24.1** | 64.7 | 22.8 | 2.04 | 0.76 | 2.16 | **Eager ($2.7\times$ faster)** |
| **8 KiB** | 2,048 | **26.1** | 68.5 | 26.4 | 3.77 | 1.43 | 3.73 | **Eager ($2.6\times$ faster)** |
| **16 KiB** | 4,096 | **35.3** | 74.3 | 35.1 | 5.57 | 2.65 | 5.60 | **Eager ($2.1\times$ faster)** |
| **32 KiB** | 8,192 | **44.0** | 85.8 | 44.2 | 8.93 | 4.58 | 8.89 | **Eager ($1.9\times$ faster)** |
| **64 KiB** | 16,384 | **62.3** | 106.5 | 64.4 | 12.63 | 7.39 | 12.21 | **Eager ($1.7\times$ faster)** |
| **128 KiB** | 32,768 | **110.2** | 151.1 | 108.6 | 14.28 | 10.41 | 14.48 | **Eager ($1.4\times$ faster)** |
| **256 KiB** | 65,536 | **198.2** | 218.2 | 187.6 | 15.87 | 14.42 | 16.77 | **Eager ($1.1\times$ faster)** |
| **512 KiB** | 131,072 | 405.8 | **385.3** | 384.8 | 15.50 | 16.33 | 16.35 | **Rendezvous ($1.1\times$ faster)** |
| **1 MiB** | 262,144 | **674.0** | 704.8 | 740.5 | 18.67 | 17.85 | 16.99 | **Eager ($1.0\times$ faster)** |
| **2 MiB** | 524,288 | **1267.5** | 1455.1 | 1441.7 | 19.85 | 17.30 | 17.46 | **Eager ($1.1\times$ faster)** |
| **4 MiB** | 1,048,576 | **2434.2** | 2762.0 | 2686.2 | 20.68 | 18.22 | 18.74 | **Eager ($1.1\times$ faster)** |
| **8 MiB** | 2,097,152 | 5239.4 | **5225.9** | 5251.4 | 19.21 | 19.26 | 19.17 | **Rendezvous ($1.0\times$ faster)** |
| **16 MiB** | 4,194,304 | 10358.9 | **10084.9** | 9982.2 | 19.44 | 19.96 | 20.17 | **Rendezvous ($1.0\times$ faster)** |
| **32 MiB** | 8,388,608 | N/A (Pool Cap) | **19040.6** | 19315.8 | N/A | 21.15 | 20.85 | **Rendezvous** |
| **64 MiB** | 16,777,216 | N/A (Pool Cap) | **36931.2** | 37611.2 | N/A | 21.81 | 21.41 | **Rendezvous** |
| **128 MiB** | 33,554,432 | N/A (Pool Cap) | **72750.4** | 75338.8 | N/A | 22.14 | 21.38 | **Rendezvous** |
| **256 MiB** | 67,108,864 | N/A (Pool Cap) | **146320.4** | 154791.0 | N/A | 22.01 | 20.81 | **Rendezvous** |
| **512 MiB** | 134,217,728 | N/A (Pool Cap) | **290745.7** | 308142.7 | N/A | 22.16 | 20.91 | **Rendezvous** |
| **1 GiB** | 268,435,456 | N/A (Pool Cap) | **579528.1** | 597430.5 | N/A | **22.23** | 21.57 | **Rendezvous (Peak)** |

---

## 4. Hyperparameter Sensitivity & Optimization Sweeps

A systematic coordinate descent optimization was executed on the live 4-node cluster to determine the optimal configuration for the InfiniBand DDR interconnect.

### 4.1 Pipelined Micro-Chunk Size (`PG_PIPELINE_CHUNK`)
*Tested on 64 MiB All-Reduce across 4 nodes (Window = 32, Batch = 8)*

| Chunk Size | Latency (ms) | Effective BW (Gbps) | Analysis |
| :--- | :--- | :--- | :--- |
| **64 KiB** | **37.04** | **21.74** | **Best 64 MiB result; matches adaptive default** |
| 128 KiB | 37.81 | 21.30 | High throughput with moderate descriptor count |
| 256 KiB | 39.57 | 20.35 | Default for segments at least 64 MiB |
| 512 KiB | 43.77 | 18.40 | Larger pipeline startup/drain bubble |
| 1 MiB | 47.01 | 17.13 | Lowest descriptor count, highest 64 MiB latency |

### 4.2 In-Flight Window Depth (`PG_RDMA_WINDOW`)
*Tested on 64 MiB All-Reduce (Chunk = 256 KiB)*

| Window Depth | Latency (ms) | Effective BW (Gbps) | Stability |
| :--- | :--- | :--- | :--- |
| 1 | 42.41 | 18.99 | 100% stable (auto-bounded signal interval = 1) |
| 8 | 39.47 | 20.40 | 100% stable |
| 16 | 39.07 | 20.61 | 100% stable |
| **32** | **37.32** | **21.58** | **Best measured balance** |
| 64 | 37.70 | 21.36 | Diminishing return |

### 4.3 Selective Signaling Interval (`PG_RDMA_SIGNAL_INTERVAL`)
*Prior dedicated sweep on 64 MiB All-Reduce (Chunk = 256 KiB, Window = 32); this dimension was not rerun by the 2026-09-27 full-suite script.*

| Signal Interval | Effective BW (Gbps) | CPU CQ Polling Overhead |
| :--- | :--- | :--- |
| 1 (Signaled Every WR) | 12.82 Gbps | High polling overhead |
| 2 (Every 2nd WR) | 15.41 Gbps | Moderate polling overhead |
| 4 (Every 4th WR) | 18.65 Gbps | Low polling overhead |
| **8 (Every 8th WR)** | **19.38 Gbps** | **Minimal overhead ($8\times$ reduction in CQ interrupts)** |
| 16 (Every 16th WR) | 19.41 Gbps | Risk of SQ starvation on smaller segment steps |

### 4.4 Multi-WR Chained Batching (`PG_BATCH_SIZE`)
*Tested on 64 MiB All-Reduce*

| Batch Size | Latency (ms) | Effective BW (Gbps) | Door-Bell Calls per Step |
| :--- | :--- | :--- | :--- |
| 1 (Unbatched) | 37.35 | 21.56 | 256 calls |
| 4 | 37.38 | 21.54 | 64 calls |
| **8** | **36.79** | **21.89** | **32 calls ($8\times$ door-bell reduction)** |
| 16 | 36.80 | 21.88 | 16 calls; effectively tied with batch 8 |

### 4.5 SIMD Vectorization Impact
*Measured execution time for reducing 256 KiB chunk (65,536 integers) on Xeon X5550*

| Reduction Kernel | Time per 256 KiB Chunk | Network Transfer Time | Overlap Capability |
| :--- | :--- | :--- | :--- |
| **Scalar C Loop** | $184.2\,\mu\text{s}$ | $131.0\,\mu\text{s}$ | ❌ **Compute Bottleneck** (CPU slower than NIC) |
| **SSE4.2 SIMD (4x unrolled)** | **$41.1\,\mu\text{s}$** | $131.0\,\mu\text{s}$ | ✅ **100% Overlapped** (CPU $3.2\times$ faster than NIC) |

### 4.6 Protocol-State Simplification

The 2026-09-28 strict-FIFO sweep followed the 2026-09-27 exact-identity sweep. The control header stayed fixed at 64 bytes, so deleting epoch/collective/phase/step fields was expected to reduce source and state rather than wire time.

| Metric | Exact Identity (2026-09-27) | Strict FIFO (2026-09-28) | Observed Delta |
| :--- | ---: | ---: | ---: |
| 64 B Eager latency | 16.0 µs | 18.8 µs | +17.5% |
| 64 B AUTO latency | 17.5 µs | 17.3 µs | -1.1% |
| 1 GiB Rendezvous bandwidth | 22.42 Gbps | 22.23 Gbps | -0.8% |
| 1 GiB AUTO bandwidth | 22.32 Gbps | 21.57 Gbps | -3.4% |

These were separate daily sweeps rather than randomized interleaved A/B trials. The mixed direction and small magnitude of most deltas do not establish a causal performance change; use the table as a regression check, not as an optimization claim.

---

## 5. Strict Tested vs. Not-Tested Boundary Matrix

In accordance with our core engineering principle of presenting only empirical facts without unverified extrapolation, the following matrix explicitly delineates what was verified on the physical hardware versus what remains out of scope:

| Category | Empirically Tested & Verified | NOT Tested / Out of Scope |
| :--- | :--- | :--- |
| **Cluster Topology** | 2-node ring (`mlx-stud-03..04`) and 4-node ring (`mlx-stud-01..04`). | Rings with $\ge 8$ physical nodes (hardware lab restricted to 4 study nodes). |
| **Network Fabric** | InfiniBand DDR (20 Gbps, native LIDs, shared subnet). | RoCEv2 (RDMA over Converged Ethernet) / GID routing across IP subnets. |
| **Payload Datatypes** | All 3 datatypes verified: `PG_INT`, `PG_FLOAT`, `PG_DOUBLE`. | Non-standard formats (e.g. FP16, BF16, INT64, custom structs). |
| **Reduction Operations** | All 4 operations verified: `PG_SUM`, `PG_MIN`, `PG_MAX`, `PG_PROD` ($3 \times 4 = 12$ pairs). | Custom reduction user-callbacks or bitwise operations (`PG_BXOR`). |
| **Buffer Divisibility** | Arbitrary remainder counts: 1001, 1003, 33333, 1000007 elements. | Dynamic size changes within the same collective call. |
| **CPU Architecture** | Intel Nehalem x86_64 with SSE4.2 (128-bit). | Modern AVX-512 / AVX2 CPUs, ARM Neoverse, or POWER9 architectures. |
| **Collective Integration** | Reduce-Scatter, All-Gather, All-Reduce, strict-FIFO RS→AG handoff, and symmetric connect/close ping. | Concurrent/nonblocking collectives, multiple logical streams per QP, or dual-ring interleaving. |
| **Stress & Reliability** | 100 rapid back-to-back iterations; separate forced-Eager and forced-Rendezvous runs with rank 0 delayed 20 ms after every Reduce-Scatter. | Divergent collective call order, partial retry/reconnection, physical link drop, or node kill during a collective. |

---

## 6. Optimization Attempts, Empirical Successes & Failure Post-Mortems

For microarchitectural post-mortems of failed optimization hypotheses and details on verified fixes, see:

👉 [R2: Optimization Attempts, Empirical Successes, and Failure Post-Mortems](../research/R2-optimization-attempts-and-findings.md)
