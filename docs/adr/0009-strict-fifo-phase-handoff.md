---
status: accepted
---

# Strict-FIFO Barrier-Free Phase Handoff

All-Reduce hands off directly from Reduce-Scatter to All-Gather without a global barrier. The implementation relies on the ordering already guaranteed by each Reliable Connected QP instead of carrying a separate collective epoch, kind, phase, and step in every control message.

## Decision

- Each RC QP carries one blocking logical collective stream for its lifetime.
- All ranks call collectives in the same order. Concurrent, overlapping, skipped, retried, or partially restarted collectives are unsupported.
- Data-path control messages are selected by message type and segment tag. Micro-index and byte length validate progress and payload shape.
- Each per-QP pending queue is a strict FIFO: only its head may be consumed, and a live receive cannot bypass an older queued receive from the same QP.
- Phase and step transitions are local consequences of completing all expected transfers. Message arrival never advances collective state.
- An RTS receives a CTS, remote address, and rkey only after its segment becomes the receiver's active transfer.
- The 64-byte control header remains fixed, so removing identity fields reduces state and source code rather than wire traffic.
- Connection and teardown use a symmetric neighbor ping. The benchmark harness aligns timed samples with a one-element public All-Gather outside the timed region.

## Required Interface Contract

The process-group interface is intentionally single-threaded and blocking. Correct use requires the same collective call sequence on every rank and one ordered logical stream per QP. Adding nonblocking collectives, multiple logical streams, multiple QPs per direction, reconnection with undrained traffic, or partial retry requires restoring a generation or transfer identifier.

## Considered Options

- Keep the three-pass barrier: safe but adds three full ring traversals and a dedicated public/internal protocol.
- Carry epoch, collective, phase, step, segment, and micro identity: robust to more future concurrency, but duplicates the current blocking stream order in wire state and implementation code.
- Carry one monotonically increasing transfer ID: a reasonable extension point if overlapping streams are later required, but unnecessary under the accepted interface contract.

## Consequences

Compared with exact transfer identity, `pg.c` and `pg_internal.h` lose 104 net lines while `pg.h` adds no interface surface. Divergent collective order now causes head-of-line blocking or, when type/segment/length coincide, can select semantically wrong traffic; such divergence is outside the supported contract. Diagnostics also cannot name a remote epoch or phase.

On 2026-09-28, four-rank eager and rendezvous builds both passed 100 rapid back-to-back All-Reduce iterations with rank 0 delayed 20 ms after every Reduce-Scatter. The complete benchmark matrix also passed: Rendezvous peaked at 22.23 Gbps and AUTO at 21.57 Gbps for 1 GiB. Those results were respectively 0.8% and 3.4% below the previous day's non-interleaved sweep; the fixed-size header means this simplification is not claimed as a performance optimization.
