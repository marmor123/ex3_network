# RDMA Ring Collectives — Empirical Protocol Evaluation Report

**Project**: Low-Latency RDMA Ring Collective Library (`ex3_network`)  
**Hardware Cluster**: `mlx-stud-01`, `mlx-stud-02`, `mlx-stud-03`, `mlx-stud-04`  
**Author**: Dvir Marmor  

---

## 1. Executive Summary

This report documents the empirical performance characterization of the RDMA Ring Collective communication library across **Eager Send/Receive**, **Windowed Rendezvous RDMA Write**, and **Adaptive Auto** protocols on a physical 4-node InfiniBand cluster.

### Key Empirical Findings
1. **Low-Latency Small Payloads**: Eager measured $16.0\,\mu\text{s}$ at 64 B versus $65.8\,\mu\text{s}$ for Rendezvous, a $4.1\times$ advantage from avoiding the RTS/CTS exchange.
2. **Measured Crossover**: In the 2026-09-27 sweep, pure Eager remained faster through 8 MiB; Rendezvous became faster at 16 MiB and scaled to **22.42 Gbps** at 1 GiB.
3. **Conservative AUTO Policy**: AUTO selects Eager only through a 64 KiB segment (256 KiB total on four ranks). It is therefore deliberately conservative from 512 KiB through 8 MiB rather than following the measured latency crossover.
4. **Peak Effective Bandwidth**: AUTO reached **22.32 Gbps** at 1 GiB ($577.32\,\text{ms}$), within 0.5% of the explicit Rendezvous build.
5. **Barrier-Free Harness**: Correctness tests run back-to-back without global fences. Each timed sample is preceded by a one-element All-Gather outside the timed region; the dedicated three-pass barrier protocol no longer exists.

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

Measurements obtained on `mlx-stud-01..04` on 2026-09-27, performing global `pg_all_reduce` (`PG_INT`, `PG_SUM`) across 5 timed iterations per size. A one-element All-Gather aligns ranks before each sample and is outside the timed interval. These results are not directly comparable to older tables that used the removed three-pass barrier as the pre-sample synchronization mechanism.

| Message Size | Element Count | Eager Latency ($\mu\text{s}$) | Rendezvous Latency ($\mu\text{s}$) | Auto Mode Latency ($\mu\text{s}$) | Eager BW (Gbps) | Rendezvous BW (Gbps) | Auto BW (Gbps) | Optimal Protocol |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **64 B** | 16 | **16.0** | 65.8 | 17.5 | 0.05 | 0.01 | 0.04 | **Eager ($4.1\times$ faster)** |
| **256 B** | 64 | **16.4** | 61.5 | 16.3 | 0.19 | 0.05 | 0.19 | **Eager ($3.8\times$ faster)** |
| **1 KiB** | 256 | **19.1** | 63.7 | 17.1 | 0.64 | 0.19 | 0.72 | **Eager ($3.3\times$ faster)** |
| **2 KiB** | 512 | **18.3** | 65.1 | 18.8 | 1.34 | 0.38 | 1.31 | **Eager ($3.6\times$ faster)** |
| **4 KiB** | 1,024 | **22.4** | 64.5 | 22.5 | 2.19 | 0.76 | 2.18 | **Eager ($2.9\times$ faster)** |
| **8 KiB** | 2,048 | **26.4** | 67.5 | 26.1 | 3.73 | 1.46 | 3.76 | **Eager ($2.6\times$ faster)** |
| **16 KiB** | 4,096 | **34.6** | 74.6 | 34.9 | 5.69 | 2.63 | 5.64 | **Eager ($2.2\times$ faster)** |
| **32 KiB** | 8,192 | **44.0** | 85.7 | 43.4 | 8.93 | 4.59 | 9.06 | **Eager ($1.9\times$ faster)** |
| **64 KiB** | 16,384 | **63.4** | 105.7 | 62.8 | 12.41 | 7.44 | 12.53 | **Eager ($1.7\times$ faster)** |
| **128 KiB** | 32,768 | **106.8** | 151.7 | 105.3 | 14.73 | 10.37 | 14.93 | **Eager ($1.4\times$ faster)** |
| **256 KiB** | 65,536 | **189.6** | 202.6 | 186.8 | 16.59 | 15.53 | 16.84 | **Eager ($1.1\times$ faster)** |
| **512 KiB** | 131,072 | **333.6** | 420.4 | 397.0 | 18.86 | 14.97 | 15.85 | **Eager ($1.3\times$ faster)** |
| **1 MiB** | 262,144 | **647.5** | 755.2 | 737.6 | 19.43 | 16.66 | 17.06 | **Eager ($1.2\times$ faster)** |
| **2 MiB** | 524,288 | **1232.3** | 1455.8 | 1537.1 | 20.42 | 17.29 | 16.37 | **Eager ($1.2\times$ faster)** |
| **4 MiB** | 1,048,576 | **2416.6** | 2703.8 | 2766.4 | 20.83 | 18.62 | 18.19 | **Eager ($1.1\times$ faster)** |
| **8 MiB** | 2,097,152 | **5220.3** | 5226.8 | 5275.6 | 19.28 | 19.26 | 19.08 | **Eager (marginally faster)** |
| **16 MiB** | 4,194,304 | 10439.3 | **9911.3** | 10036.5 | 19.29 | 20.31 | 20.06 | **Rendezvous ($1.1\times$ faster)** |
| **32 MiB** | 8,388,608 | N/A (Pool Cap) | **18817.6** | 19016.6 | N/A | 21.40 | 21.17 | **Rendezvous** |
| **64 MiB** | 16,777,216 | N/A (Pool Cap) | 37252.7 | **37134.5** | N/A | 21.62 | 21.69 | **Rendezvous** |
| **128 MiB** | 33,554,432 | N/A (Pool Cap) | **72108.8** | 72736.7 | N/A | 22.34 | 22.14 | **Rendezvous** |
| **256 MiB** | 67,108,864 | N/A (Pool Cap) | 146458.0 | **146288.7** | N/A | 21.99 | 22.02 | **Rendezvous** |
| **512 MiB** | 134,217,728 | N/A (Pool Cap) | 290451.8 | **289619.9** | N/A | 22.18 | 22.24 | **Rendezvous** |
| **1 GiB** | 268,435,456 | N/A (Pool Cap) | **574806.8** | 577324.9 | N/A | **22.42** | 22.32 | **Rendezvous (Peak)** |

---

## 4. Hyperparameter Sensitivity & Optimization Sweeps

A systematic coordinate descent optimization was executed on the live 4-node cluster to determine the optimal configuration for the InfiniBand DDR interconnect.

### 4.1 Pipelined Micro-Chunk Size (`PG_PIPELINE_CHUNK`)
*Tested on 64 MiB All-Reduce across 4 nodes (Window = 32, Batch = 8)*

| Chunk Size | Latency (ms) | Effective BW (Gbps) | Analysis |
| :--- | :--- | :--- | :--- |
| **64 KiB** | **36.90** | **21.82** | **Best 64 MiB result; matches adaptive default** |
| 128 KiB | 37.73 | 21.34 | High throughput with moderate descriptor count |
| 256 KiB | 39.58 | 20.35 | Default for segments at least 64 MiB |
| 512 KiB | 43.60 | 18.47 | Larger pipeline startup/drain bubble |
| 1 MiB | 46.11 | 17.46 | Lowest descriptor count, highest 64 MiB latency |

### 4.2 In-Flight Window Depth (`PG_RDMA_WINDOW`)
*Tested on 64 MiB All-Reduce (Chunk = 256 KiB)*

| Window Depth | Latency (ms) | Effective BW (Gbps) | Stability |
| :--- | :--- | :--- | :--- |
| 1 | 42.01 | 19.17 | 100% stable (auto-bounded signal interval = 1) |
| 8 | 38.84 | 20.73 | 100% stable |
| 16 | 37.13 | 21.69 | 100% stable |
| **32** | **37.01** | **21.76** | **Best measured balance** |
| 64 | 38.03 | 21.17 | Diminishing return |

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
| 1 (Unbatched) | 38.24 | 21.06 | 256 calls |
| 4 | 38.45 | 20.95 | 64 calls |
| 8 | 38.12 | 21.12 | 32 calls |
| **16** | **36.85** | **21.85** | **16 calls ($16\times$ door-bell reduction)** |

### 4.5 SIMD Vectorization Impact
*Measured execution time for reducing 256 KiB chunk (65,536 integers) on Xeon X5550*

| Reduction Kernel | Time per 256 KiB Chunk | Network Transfer Time | Overlap Capability |
| :--- | :--- | :--- | :--- |
| **Scalar C Loop** | $184.2\,\mu\text{s}$ | $131.0\,\mu\text{s}$ | ❌ **Compute Bottleneck** (CPU slower than NIC) |
| **SSE4.2 SIMD (4x unrolled)** | **$41.1\,\mu\text{s}$** | $131.0\,\mu\text{s}$ | ✅ **100% Overlapped** (CPU $3.2\times$ faster than NIC) |

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
| **Collective Integration** | Reduce-Scatter, All-Gather, All-Reduce, epoch-gated RS→AG handoff, and symmetric connect/close ping. | Dual-ring bidirectional full-duplex interleaving. |
| **Stress & Reliability** | 100 rapid back-to-back iterations in each full sweep; separate 30-iteration run with rank 0 delayed 20 ms after every Reduce-Scatter. | Fault tolerance under physical link drop or node kill during collective. |

---

## 6. Optimization Attempts, Empirical Successes & Failure Post-Mortems

For microarchitectural post-mortems of failed optimization hypotheses and details on verified fixes, see:

👉 [R2: Optimization Attempts, Empirical Successes, and Failure Post-Mortems](../research/R2-optimization-attempts-and-findings.md)
