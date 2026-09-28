# RDMA Ring Collectives

This context defines the language used for collective communication over the project's deterministic RDMA ring.

## Language

**Process Group**:
The participating ranks and their deterministic ring topology. Each rank has one previous and one next neighbor.

**Ordered Transfer Stream**:
The single sequence of collective transfers that every rank performs in the same order. A Process Group supports one active Ordered Transfer Stream at a time.

**Phase Handoff**:
The transition from Reduce-Scatter to All-Gather inside All-Reduce. It occurs after local Reduce-Scatter completion without a rank-wide rendezvous.

**Collective Buffer Contract**:
The ownership and overlap rules for a collective's input and output ranges. Reduce operations require disjoint ranges; All-Gather also permits its exact local output slice as input.

**Ring Step Transfer**:
One of the $N-1$ neighbor exchanges in a collective phase. Every rank sends one Segment to its next neighbor while receiving another from its previous neighbor.

**Segment**:
A contiguous slice of a collective payload assigned to a rank. Non-divisible payloads use MPI-style remainder distribution.

**Micro-Chunk**:
A pipelined subdivision of a Segment used to overlap transfer and reduction.

**Staging Buffer**:
Internal registered memory that receives data before Reduce-Scatter combines it with the local accumulator.

**Work Buffer**:
Internal memory used by safe mode to preserve the caller's input during a reduction.

**Reduce-Scatter**:
A collective that reduces an array across all ranks and leaves each rank with one reduced Segment.

**All-Gather**:
A collective that distributes every rank's Segment so every rank receives the complete concatenated array.

**All-Reduce**:
A collective that produces the complete reduced array on every rank through Reduce-Scatter followed by All-Gather.
