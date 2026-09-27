---
status: accepted
---

# Epoch-Gated Barrier-Free Phase Handoff

All-Reduce now hands off directly from Reduce-Scatter to All-Gather without a global barrier. Every data-path message carries an exact transfer identity—collective epoch, collective kind, phase, ring step, segment, and micro-chunk—so a receiver cannot interpret early or subsequent-iteration traffic using the active transfer's memory destination.

## Decision

- A valid public collective invocation receives a monotonically increasing process-group-local epoch. Both All-Reduce phases share one epoch.
- Eager payloads are consumed only by their exact transfer. Future payloads remain pending.
- Rendezvous RTS messages remain pending until their exact receive phase is active; only then may the receiver return a CTS containing a destination address.
- Reduce-Scatter and All-Reduce require disjoint input/output ranges. All-Gather additionally permits its input to be exactly the calling rank's owned output slice.
- All-Reduce always uses barrier-free handoff and has no internal phase-fence mode or barrier control protocol.
- Connection and teardown exchange one symmetric neighbor ping so every RC QP edge is verified or quiesced without a global token circulation.
- Correctness tests run collectives back-to-back. The benchmark harness aligns timed samples with a one-element All-Gather through the public collective interface; synchronization is outside the timed region.
- Pending-queue exhaustion and malformed transfer lengths are errors rather than silent drops or clamping.

## Considered Options

- Keep the three-pass barrier: safe but pays three full ring traversals and does not identify cross-iteration traffic.
- Use a two-pass collect/release fence: sufficient for global phase entry, but still unnecessary after receiver gating.
- Match only message type and segment: rejected because phases and iterations reuse both values.

## Consequences

The progress and transfer modules carry more protocol state, but correctness no longer depends on rank-wide timing. Removing the dedicated barrier also removed its three control-message types, token-passing implementation, internal declaration, and three progress wait helpers that no longer had callers. Four-rank eager and rendezvous runs passed with rank 0 delayed after every Reduce-Scatter and every output element checked across changing back-to-back iterations.
