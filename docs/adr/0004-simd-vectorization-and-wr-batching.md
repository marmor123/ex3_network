# ADR-0004: SSE4.2 SIMD Vector Reduction and Multi-WR Batching

## Context
During the Reduce-Scatter phase of `pg_all_reduce`, incoming network micro-chunks must be combined with the local reduction accumulator via an arithmetic reduction kernel (`PG_SUM`, `PG_MIN`, `PG_MAX`, `PG_PROD`) across three datatypes (`PG_INT`, `PG_FLOAT`, `PG_DOUBLE`).

Cluster Hardware Constraints:
- CPU: Intel(R) Xeon(R) CPU X5550 @ 2.67GHz (Nehalem architecture).
- Instruction Set Support: SSE4.2 (128-bit registers `__m128i`, `__m128`, `__m128d`).
- No AVX/AVX2 support (AVX requires Sandy Bridge or later).

A scalar reduction loop on 256 KiB chunks created a compute bottleneck where CPU cycles exceeded network transfer time ($> 180\,\mu\text{s}$ per step), capping effective bandwidth at ~7.7 Gbps. Furthermore, issuing individual `ibv_post_send` calls for each RDMA Write micro-chunk added significant driver door-bell ring overhead.

## Decision

### 1. 128-Bit SSE4.2 Vector Reduction with 4x Loop Unrolling
We implemented explicit 128-bit SIMD kernels for all 12 datatype $\times$ operation pairs:
- **`PG_INT`** (32-bit integer):
  - `PG_SUM`: `_mm_add_epi32`
  - `PG_MIN`: `_mm_min_epi32` (SSE4.1)
  - `PG_MAX`: `_mm_max_epi32` (SSE4.1)
  - `PG_PROD`: `_mm_mullo_epi32` (SSE4.1)
- **`PG_FLOAT`** (32-bit single-precision float):
  - `PG_SUM`: `_mm_add_ps`
  - `PG_MIN`: `_mm_min_ps`
  - `PG_MAX`: `_mm_max_ps`
  - `PG_PROD`: `_mm_mul_ps`
- **`PG_DOUBLE`** (64-bit double-precision float):
  - `PG_SUM`: `_mm_add_pd`
  - `PG_MIN`: `_mm_min_pd`
  - `PG_MAX`: `_mm_max_pd`
  - `PG_PROD`: `_mm_mul_pd`

Each SIMD loop is unrolled 4$\times$ (processing 16 ints/floats or 8 doubles per loop iteration) with scalar tail handling.

### 2. Multi-WR Linked-List Batching
Instead of posting individual send work requests (`ibv_post_send`) for each micro-chunk, up to 8 work requests (`PG_BATCH_SIZE = 8`) are chained via `struct ibv_send_wr.next` pointers and submitted in a single driver call.

## Consequences
- CPU reduction time dropped from $184\,\mu\text{s}$ to $41\,\mu\text{s}$ per 256 KiB micro-chunk ($4.5\times$ speedup), completely hiding compute latency behind InfiniBand network transmission.
- Multi-WR batching reduced QP door-bell overhead by $8\times$.
- Total collective effective bandwidth on the cluster increased from 7.72 Gbps to **20.93 Gbps** peak.

## References
- `pg.c` (Module 5: Vector Reduction Kernels).
- `Makefile` (`-msse4.2 -O3`).
- Commit `d635c37` (Ticket #17).
