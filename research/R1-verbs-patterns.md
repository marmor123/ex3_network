# R1: InfiniBand Verbs RC Ring Architectural Patterns

## Overview
This document summarizes the core Verbs API patterns for Reliable Connected (RC) Queue Pairs in ring collective communications.

## Device / PD / CQ / QP Creation

- **Device selection**: First available InfiniBand device from `ibv_get_device_list`.
- **IB port**: Standard port 1 (`PG_IB_PORT = 1`).
- **Memory Alignment**: 64-byte alignment via `posix_memalign` to match CPU cache lines and InfiniBand burst transfers.
- **Protection Domain (PD)**: Single PD per process group context via `ibv_alloc_pd`.
- **Completion Queue (CQ)**: Shared CQ for both send and receive completions (`ibv_create_cq`), sized to `(max_send_wr + max_recv_wr) * 2`.
- **Queue Pairs (QP)**: Two RC Queue Pairs per rank:
  - `qp_to_next`: Outbound connection to `next_rank = (rank + 1) % size`.
  - `qp_from_prev`: Inbound connection from `prev_rank = (rank - 1 + size) % size`.
- **Inline Stepping**: Probing `max_inline_data` dynamically downwards from 1024 to 64 bytes during `ibv_create_qp` to determine exact hardware limits, followed by `ibv_query_qp` read-back.

## QP State Transitions

- **RESET &rarr; INIT**: Sets `pkey_index = 0`, `port_num = 1`, and access permissions `IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ`.
- **INIT &rarr; RTR**: Configures `path_mtu` from port query, remote QPN, remote PSN, remote LID, and `min_rnr_timer = 12`.
- **RTR &rarr; RTS**: Sets `timeout = 14`, `retry_cnt = 7`, `rnr_retry = 7`, and local starting `sq_psn`.

## Pipelining & Signaling Constants

- **Micro-Chunk Slicing**: 256 KiB chunks pipeline network RDMA Write transmission with CPU vector reduction.
- **Sliding Window**: 32 in-flight micro-chunks prevent QP queue overflow while saturating the 20 Gbps link.
- **Selective Signaling**: Signaled completions posted once every 8 work requests (`PG_RDMA_SIGNAL_INTERVAL = 8`) to reduce CQ polling interrupt overhead.
