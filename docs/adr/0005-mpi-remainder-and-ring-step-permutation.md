# ADR-0005: MPI Remainder Partitioning and Ring Step Permutations

## Context
Standard ring collective algorithms assume the total element count $C$ is evenly divisible by the number of participating ranks $N$. In realistic scientific workloads, $C$ is arbitrary (e.g. $C = 1001, 1003, 33333, 1000007$), resulting in a non-zero remainder $R = C \bmod N$.

Naively rounding or truncating causes out-of-bounds memory corruption or silent arithmetic omission. We needed an exact, deterministic remainder partitioning scheme and unambiguous index formulas for all ring transfer steps.

## Decision

### 1. MPI-Standard $(Q+1)/Q$ Remainder Partitioning
For total count $C$ and ring size $N$:
- Base segment count: $Q = \lfloor C / N \rfloor$
- Remainder count: $R = C \bmod N$

For rank $i \in [0, N-1]$:
$$\text{seg\_count}(i) = \begin{cases} Q + 1 & \text{if } i < R \\ Q & \text{if } i \ge R \end{cases}$$

$$\text{seg\_offset}(i) = \begin{cases} i \times (Q + 1) & \text{if } i < R \\ R \times (Q + 1) + (i - R) \times Q & \text{if } i \ge R \end{cases}$$

Byte lengths and offsets are computed as $\text{seg\_bytes}(i) = \text{seg\_count}(i) \times \text{sizeof}(\text{datatype})$ and $\text{seg\_byte\_offset}(i) = \text{seg\_offset}(i) \times \text{sizeof}(\text{datatype})$.

### 2. Reduce-Scatter Ring Permutation
In an $N$-rank ring, Reduce-Scatter executes $N-1$ communication steps ($k \in [0, N-2]$).
At step $k$:
- Outbound segment transmitted to `next_rank`:
  $$s_{\text{out}} = (\text{rank} - k - 1 + N) \bmod N$$
- Inbound segment received from `prev_rank`:
  $$s_{\text{in}} = (\text{rank} - k - 2 + N) \bmod N$$

After $N-1$ steps, each rank $r$ holds the fully reduced slice for segment $r$.

### 3. All-Gather Ring Permutation
All-Gather executes $N-1$ communication steps ($k \in [0, N-2]$).
At step $k$:
- Outbound segment transmitted to `next_rank`:
  $$s_{\text{out}} = (\text{rank} - k + N) \bmod N$$
- Inbound segment received from `prev_rank`:
  $$s_{\text{in}} = (\text{rank} - k - 1 + N) \bmod N$$

Data is written zero-copy directly into the caller's `recvbuf` at offset $\text{seg\_byte\_offset}(s_{\text{in}})$. After $N-1$ steps, every rank holds the complete gathered buffer.

## Consequences
- 100% mathematical correctness for arbitrary counts ($C \ge N$) with zero memory corruption or padding overhead.
- Total elements transferred equals $C \times \frac{N-1}{N}$ across both phases, matching theoretical minimum ring communication volume.

## References
- `pg_internal.h` (`pg_get_seg_count`, `pg_get_seg_offset_elems`, `pg_get_seg_offset_bytes`).
- `pg.c` (`pg_reduce_scatter`, `pg_ring_all_gather_generalized`).
- `main_test.c` (Remainder test suite `run_non_divisible_counts_tests`).
