# ADR-0003: Empirical Eager Threshold and Adaptive Protocol Selection

## Context
In RDMA network collectives, protocol selection fundamentally trades off control handshake overhead against CPU memory copies.
- **Rendezvous Protocol** (`RTS` $\to$ `CTS` $\to$ `RDMA_WRITE` $\to$ `DATA_DONE`) delivers zero-copy direct memory placement but suffers from round-trip control latency (2 RTTs) on small payloads.
- **Eager Protocol** (`IBV_WR_SEND` with 2-SGE scatter-gather) eliminates the control handshake by immediately pushing payload into pre-posted receive buffers, but incurs buffer copies and memory consumption for receive pools.

We needed to establish the optimal crossover threshold empirically on the live 4-node InfiniBand cluster (`mlx-stud-01..04`) and provide clean compile-time / runtime mode selection.

## Decision

### 1. Empirical Threshold Determination: 8 KiB
A coordinate sweep on the 4-node cluster across thresholds from 1 KiB to 64 KiB revealed:
- Payloads $\le 8\text{ KiB}$: Eager protocol provides **2.1$\times$ lower latency** compared to Rendezvous ($42.8\,\mu\text{s}$ vs $88.2\,\mu\text{s}$ at 64 B, $44.9\,\mu\text{s}$ vs $89.7\,\mu\text{s}$ at 1 KiB).
- Payloads $\ge 16\text{ KiB}$: Rendezvous protocol overtakes Eager in effective bandwidth due to zero-copy memory transfers and pipelined micro-chunk overlap.
- We set `PG_EAGER_THRESHOLD = (8 * 1024)` (8 KiB) as the default crossover boundary.

### 2. Protocol Modes
We support three compilation modes via `Makefile MODE=<mode>`:
- `MODE=rendezvous` (`PG_MODE_RENDEZVOUS`): All sizes use pipelined RDMA Write rendezvous.
- `MODE=eager` (`PG_MODE_EAGER`): All sizes (up to 16 MiB pool capacity) use Eager SEND.
- `MODE=auto` (`PG_MODE_AUTO`): Dynamically selects Eager for transfer sizes $\le 8\text{ KiB}$ and Rendezvous for $> 8\text{ KiB}$.

### 3. Unified Pre-Posted Receive Pool
- A pre-allocated pool of 32 receive buffers per QP direction (`PG_EAGER_POOL_DEPTH = 32`), sized to $\max(\text{threshold}, \text{pipeline\_chunk})$.
- Buffers are registered with `IBV_ACCESS_LOCAL_WRITE` at `connect_process_group` and continuously reposted upon consumption.
- Control messages and small eager messages are unified through `pg_ctrl_msg` headers with 2-SGE scatter-gather sends.

## Consequences
- Small-message operations (e.g. metadata sync, small tensor all-reduces) achieve near-wire latency.
- Large-message operations achieve peak link bandwidth without buffer copy bottlenecks.
- `MODE=auto` provides the superior Pareto frontier across all buffer sizes from 64 B to 1 GiB.

## References
- `assignment.txt`: Lecture #2 Eager vs. Rendezvous requirements.
- `docs/empirical_protocol_report.md`: Sweep data on `mlx-stud-01..04`.
- Commit `3b33127` & `5e33aad`.
