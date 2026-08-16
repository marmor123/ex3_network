## Destination

Implement a single-threaded C/C++ Verbs ring collective library (connect_process_group / pg_all_reduce / pg_close + pg_reduce_scatter / pg_all_gather) that compares eager vs rendezvous, pipelines communication/computation, and uses RDMA Write zero-copy for large all-gather — tested with 2 and 4 processes via `-myindex`/`-list` ring bootstrap.

## Notes

Domain: Low-latency RDMA collectives (ring reduce-scatter, all-gather, all-reduce). Skills: consult `summary_of_grilling.txt` as the settled design, `past_exercise/bw.c` as Verbs reference, and assignment.txt for required API/topology. Preferences: thin vertical slices V0-V9, minimal codebase (pg.h/main_test.c/pg.c/Makefile) split only when too large, single-threaded progress engine, no MPI, no RDMA_WRITE_WITH_IMM, edge-ordered ring-only TCP bootstrap.

**Cluster Hardware & Remote Execution Setup (from this device)**:
- **Cluster Nodes**: `mlx-stud-01`, `mlx-stud-02`, `mlx-stud-03`, `mlx-stud-04` (bastion `bava.cs.huji.ac.il`, user `ateret.tabib`).
- **Cluster Working Directory**: `/cs/usr/ateret.tabib/Downloads/ex3_network` (NFS shared across all nodes).
- **Remote Execution**: Run commands non-interactively via WSL using the persistent ControlMaster socket:
  `wsl -d Ubuntu env HOME=/home/marmor ssh <node> "<command>"`
- **Local Verification**: `wsl -d Ubuntu make check` or `python test_v1_local.py`.
- **Multi-Node Cluster Verification**: Run `wsl -d Ubuntu env HOME=/home/marmor bash run_cluster_test.sh 2` or `4`.

## Decisions so far

- [R1: Research Verbs RC ring patterns from bw.c — device/QP/control-pool constants](https://github.com/marmor123/ex3_network/issues/12) — inline stepdown 1024→64 with `ibv_query_qp` read-back, shared CQ `W+K` clamped, `INIT→RTR→RTS` constants, 32×64B control pool — branch [`research/r1-verbs-patterns`](https://github.com/marmor123/ex3_network/tree/research/r1-verbs-patterns/docs/research/R1-verbs-patterns.md) @ `9f943ea`
- [R2: Research eager vs rendezvous (Lecture #2) + RDMA Write/Read zero-copy](https://github.com/marmor123/ex3_network/issues/13) — eager 2-SGE SEND vs RTS/CTS/RDMA_WRITE/DATA_DONE, crossed-out WRITE_WITH_IMM, threshold 8 KiB — branch [`research/r2-eager-rendezvous`](https://github.com/marmor123/ex3_network/tree/research/r2-eager-rendezvous/docs/research/R2-eager-rendezvous.md) @ `b6591fc`
- [R3: Research ring math and pipelining — segment ownership and micro-chunk formulas](https://github.com/marmor123/ex3_network/issues/14) — RS `(r-i-1)%N` / AG `(r-i)%N`, `AR=RS+barrier+AG`, per-micro CTS/DATA_DONE — branch [`research/r3-ring-math`](https://github.com/marmor123/ex3_network/tree/research/r3-ring-math/docs/research/R3-ring-math.md) @ `7b9f68c`
- [G1: Grill wr_id bit-packing and progress-engine dispatch](https://github.com/marmor123/ex3_network/issues/15) — simple enum per-QP (bits 0-3 type, bit4 qp_dir), repost-after-consume per-QP, switch dispatch, per-QP refill window — ADR [`docs/adr/0001-wr-id-progress-engine.md`](https://github.com/marmor123/ex3_network/blob/main/docs/adr/0001-wr-id-progress-engine.md) @ `8ea238f`
- [G2: Grill MR/internal buffer cache and staging lifecycle](https://github.com/marmor123/ex3_network/issues/16) — lazy MR cache dereg-at-close, grow-only staging, safe copy + optional inplace workbuf, 16×max(8 KiB,chunk) eager pool, staging→zero-copy — ADR [`docs/adr/0002-mr-buffer-lifecycle.md`](https://github.com/marmor123/ex3_network/blob/main/docs/adr/0002-mr-buffer-lifecycle.md) @ `f403213`
- [V0: CLI sanity — parse -myindex/-list and validate servername](https://github.com/marmor123/ex3_network/issues/2) — 1-based index to 0-based rank, size from host count, ring neighbor modulo arithmetic, global pg_args servername validation in connect_process_group @ `1b02ea0`
- [V1: TCP bootstrap dry-run — edge-ordered ring exchange](https://github.com/marmor123/ex3_network/issues/3) — edge-ordered TCP exchange on port 19000+rank, network-byte-order struct pg_tcp_qp_info with ready tag 0x52454144, 2-rank & 4-rank loopback and live cluster verified @ `3b3ebcc`
- [V2: RDMA control ring ping — QPs, CQ, control pools](https://github.com/marmor123/ex3_network/issues/4) — InfiniBand device/PD/CQ init, 2 RC QPs with inline stepdown (828B), 32-slot control pool, real QP metadata exchange via TCP, and symmetric control ring ping @ `3cca586`

## Not yet specified

- Full-segment staging memory pressure fallback (slot pool) — only if 1 GB sweep fails.
- Non-divisible count and pipeline remainder handling — deferred past bring-up.
- RoCE/GID support — deferred.
- Double full-segment staging buffers for cross-segment overlap — deferred.
- PG_MODE_AUTO default — deferred, rendezvous-data remains default.

## Out of scope

- MPI rank establishment — ruled out, TCP bootstrap from `-myindex`/`-list` is the design.
- RDMA Write with Immediate for completion — crossed out in assignment, use IBV_WR_SEND instead.
- GPU / NCCL integration — beyond course exercise.

## Tickets

- [x] #2 V0: CLI sanity — parse -myindex/-list and validate servername
- [x] #3 V1: TCP bootstrap dry-run — edge-ordered ring exchange
- [x] #4 V2: RDMA control ring ping — QPs, CQ, control pools
- [ ] #5 V3: Rendezvous segment transfer — RTS/CTS/RDMA_WRITE/DATA_DONE
- [ ] #6 V4: Bring-up Reduce-Scatter — PG_INT+PG_SUM ring
- [ ] #7 V5: Bring-up All-Gather — zero-copy RDMA_WRITE
- [ ] #8 V6: Bring-up All-Reduce — RS+barrier+AG integration
- [ ] #9 V7: Pipelining — full-segment staging and receive-while-reduce
- [ ] #10 V8: Performance path + benchmark harness — warmup and sweep
- [ ] #11 V9: Eager benchmark mode — SEND payload and MODE selectors
