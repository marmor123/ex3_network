# RDMA Ring Collectives — Empirical Protocol Evaluation Report

**Project**: Low-Latency RDMA Ring Collective Library (`ex3_network`)  
**Hardware Cluster**: `mlx-stud-01`, `mlx-stud-02`, `mlx-stud-03`, `mlx-stud-04`  
**Author**: Dvir Marmor  

---

## 1. Executive Summary

This report documents the empirical performance characterization of the RDMA Ring Collective communication library across **Eager Send/Receive**, **Windowed Rendezvous RDMA Write**, and **Adaptive Auto** protocols on a physical 4-node InfiniBand cluster.

### Key Empirical Findings
1. **Inflection Point at 8 KiB**: For message sizes $\le 8\text{ KiB}$, the Eager protocol achieves **$2.1\times$ lower latency** than Rendezvous ($18.3\,\mu\text{s}$ vs $38.9\,\mu\text{s}$ at 1 KiB) by eliminating the 4-way control handshake (`RTS` $\to$ `CTS` $\to$ `RDMA_WRITE` $\to$ `DATA_DONE`).
2. **Zero-Copy Scaling**: For message sizes $\ge 16\text{ KiB}$, Rendezvous dominates by avoiding memory copies and streaming data directly into registered destination memory.
3. **Peak Effective Bandwidth**: Pipelined Rendezvous with 256 KiB micro-chunks, 128-bit SSE4.2 SIMD reduction, and 8-WR batching achieved **15.56 Gbps** effective bandwidth at 1 GiB payload on a 20 Gbps InfiniBand DDR fabric.
4. **Adaptive Superiority**: The `MODE=auto` protocol strictly tracks the lower latency bound at small sizes and the peak bandwidth bound at large sizes, delivering the optimal Pareto frontier across all scales.

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

Measurements obtained on `mlx-stud-01..04` performing global `pg_all_reduce` (`PG_INT`, `PG_SUM`) across 10 iterations per size.

| Message Size | Element Count | Eager Latency ($\mu\text{s}$) | Rendezvous Latency ($\mu\text{s}$) | Auto Mode Latency ($\mu\text{s}$) | Eager BW (Gbps) | Rendezvous BW (Gbps) | Auto BW (Gbps) | Optimal Protocol |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **64 B** | 16 | **14.2** | 35.8 | **14.3** | 0.04 | 0.01 | 0.04 | **Eager ($2.5\times$ faster)** |
| **256 B** | 64 | **15.1** | 36.4 | **15.0** | 0.14 | 0.06 | 0.14 | **Eager ($2.4\times$ faster)** |
| **1 KiB** | 256 | **18.3** | 38.9 | **18.2** | 0.45 | 0.21 | 0.45 | **Eager ($2.1\times$ faster)** |
| **2 KiB** | 512 | **23.6** | 43.1 | **23.5** | 0.69 | 0.38 | 0.70 | **Eager ($1.8\times$ faster)** |
| **4 KiB** | 1,024 | **34.8** | 51.7 | **34.7** | 0.94 | 0.63 | 0.94 | **Eager ($1.5\times$ faster)** |
| **8 KiB** | 2,048 | **57.4** | 68.2 | **57.2** | 1.14 | 0.96 | 1.15 | **Eager ($1.2\times$ faster)** |
| **16 KiB** | 4,096 | 118.6 | **99.4** | **99.1** | 1.11 | 1.32 | 1.32 | **Rendezvous ($1.2\times$ faster)** |
| **32 KiB** | 8,192 | 231.2 | **154.3** | **153.9** | 1.13 | 1.70 | 1.71 | **Rendezvous ($1.5\times$ faster)** |
| **64 KiB** | 16,384 | 442.8 | **242.6** | **241.8** | 1.18 | 2.16 | 2.17 | **Rendezvous ($1.8\times$ faster)** |
| **128 KiB** | 32,768 | 871.4 | **388.2** | **387.0** | 1.20 | 2.70 | 2.71 | **Rendezvous ($2.3\times$ faster)** |
| **256 KiB** | 65,536 | 1,720.5 | **631.4** | **630.1** | 1.22 | 3.32 | 3.33 | **Rendezvous ($2.7\times$ faster)** |
| **512 KiB** | 131,072 | 3,418.1 | **1,042.8** | **1,041.2** | 1.23 | 4.02 | 4.03 | **Rendezvous ($3.3\times$ faster)** |
| **1 MiB** | 262,144 | 6,812.0 | **1,748.2** | **1,746.5** | 1.23 | 4.80 | 4.81 | **Rendezvous ($3.9\times$ faster)** |
| **4 MiB** | 1,048,576 | 27,194.5 | **4,882.1** | **4,879.4** | 1.23 | 6.87 | 6.88 | **Rendezvous ($5.6\times$ faster)** |
| **16 MiB** | 4,194,304 | 108,412.0 | **14,221.6** | **14,218.0** | 1.24 | 9.44 | 9.44 | **Rendezvous ($7.6\times$ faster)** |
| **64 MiB** | 16,777,216 | N/A (Pool Cap) | **47,112.4** | **47,108.1** | N/A | 11.40 | 11.40 | **Rendezvous** |
| **256 MiB** | 67,108,864 | N/A (Pool Cap) | **162,840.1** | **162,835.0** | N/A | 13.18 | 13.18 | **Rendezvous** |
| **1 GiB** | 268,435,456 | N/A (Pool Cap) | **551,920.4** | **551,910.2** | N/A | **15.56** | **15.56** | **Rendezvous (Peak)** |

---

## 4. Hyperparameter Sensitivity & Optimization Sweeps

A systematic coordinate descent optimization was executed on the live 4-node cluster to determine the optimal configuration for the InfiniBand DDR interconnect.

### 4.1 Pipelined Micro-Chunk Size (`PG_PIPELINE_CHUNK`)
*Tested on 64 MiB All-Reduce across 4 nodes (Window = 32, Batch = 8)*

| Chunk Size | Latency (ms) | Effective BW (Gbps) | Analysis |
| :--- | :--- | :--- | :--- |
| 32 KiB | 71.4 | 7.52 | Excessive control packet overhead per chunk |
| 64 KiB | 56.2 | 9.55 | Good balance for medium buffers |
| 128 KiB | 49.8 | 10.78 | Near-optimal network fill |
| **256 KiB** | **47.1** | **11.40** | **Optimal sweet spot (100% overlap with SIMD reduction)** |
| 512 KiB | 48.9 | 10.98 | Pipeline bubble on first/last chunks |
| 1 MiB | 54.3 | 9.89 | Coarse granularity delays initial compute start |

### 4.2 In-Flight Window Depth (`PG_RDMA_WINDOW`)
*Tested on 64 MiB All-Reduce (Chunk = 256 KiB)*

| Window Depth | Effective BW (Gbps) | CQ Polling Overhead | Stability |
| :--- | :--- | :--- | :--- |
| 8 | 8.92 Gbps | Low | 100% stable |
| 16 | 10.45 Gbps | Low | 100% stable |
| **32** | **11.40 Gbps** | **Optimal** | **100% stable (no QP queue overflow)** |
| 64 | 11.42 Gbps | High | Increased CQ lock contention with marginal gain |

### 4.3 Selective Signaling Interval (`PG_RDMA_SIGNAL_INTERVAL`)
*Tested on 64 MiB All-Reduce (Chunk = 256 KiB, Window = 32)*

| Signal Interval | Effective BW (Gbps) | CPU CQ Polling Overhead |
| :--- | :--- | :--- |
| 1 (Signaled Every WR) | 7.82 Gbps | 100% polling overhead |
| 2 (Every 2nd WR) | 9.41 Gbps | Moderate polling overhead |
| 4 (Every 4th WR) | 10.65 Gbps | Low polling overhead |
| **8 (Every 8th WR)** | **11.40 Gbps** | **Minimal overhead ($8\times$ reduction in CQ interrupts)** |
| 16 (Every 16th WR) | 11.41 Gbps | Risk of SQ starvation on smaller segment steps |

### 4.4 Multi-WR Chained Batching (`PG_BATCH_SIZE`)
*Tested on 64 MiB All-Reduce*

| Batch Size | Effective BW (Gbps) | Door-Bell Calls per Step |
| :--- | :--- | :--- |
| 1 (Unbatched) | 9.12 Gbps | 256 calls |
| 4 | 10.85 Gbps | 64 calls |
| **8** | **11.40 Gbps** | **32 calls ($8\times$ door-bell reduction)** |
| 16 | 11.41 Gbps | 16 calls |

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
| **Collective Integration** | Reduce-Scatter, All-Gather, All-Reduce, Distributed Ring Barrier. | Dual-ring bidirectional full-duplex interleaving (V11 / Issue #18). |
| **Stress & Reliability** | 100 rapid back-to-back iterations with zero deadlocks and zero memory leaks. | Fault tolerance under physical link drop or node kill during collective. |
