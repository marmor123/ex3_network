#!/usr/bin/env python3
import subprocess
import re
import sys
import time

HOSTS = ["mlx-stud-01", "mlx-stud-02", "mlx-stud-03", "mlx-stud-04"]
CLUSTER_DIR = "/cs/usr/ateret.tabib/Downloads/ex3_network"

def sync_and_build():
    print("[Sweep] Syncing workspace to cluster and building...")
    subprocess.run(["rsync", "-avz", "--exclude", ".git", "--exclude", "*.o", "--exclude", "test", "./", f"{HOSTS[0]}:{CLUSTER_DIR}/"], check=True)
    subprocess.run(["ssh", HOSTS[0], f"cd {CLUSTER_DIR} && make clean && make PROFILE=perf"], check=True)

def run_test(env_vars):
    env_str = " ".join([f"{k}={v}" for k, v in env_vars.items()])
    procs = []
    for rank, host in enumerate(HOSTS):
        idx = f"{rank + 1:02d}"
        cmd = f"cd {CLUSTER_DIR} && {env_str} ./test -myindex {idx} -list {' '.join(HOSTS)}"
        p = subprocess.Popen(["ssh", host, cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(p)
    
    outs = []
    errs = []
    success = True
    for rank, p in enumerate(procs):
        out, err = p.communicate()
        outs.append(out)
        errs.append(err)
        if p.returncode != 0:
            success = False
            print(f"[ERROR] Rank {rank} on {HOSTS[rank]} failed (rc={p.returncode}):\n{err}")
    
    if not success:
        return None
    
    # Parse rank 0 benchmark lines
    # Format: 1073741824   | 268435456    |   1652655.69 |   1669178.29 |   1676852.92 |       7.72 Gbps
    rank0_out = outs[0]
    results = {}
    for line in rank0_out.splitlines():
        m = re.match(r"^\s*(\d+)\s+\|\s*(\d+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s*Gbps", line)
        if m:
            sz = int(m.group(1))
            bw = float(m.group(6))
            results[sz] = bw
    return results

def main():
    sync_and_build()
    
    best_config = {
        "PG_PIPELINE_CHUNK": 262144,
        "PG_RDMA_WINDOW": 32,
        "PG_RDMA_SIGNAL_INTERVAL": 16,
        "PG_BATCH_SIZE": 8,
        "PG_STREAMING_STORES": 1
    }
    
    print("\n==================================================================")
    print("  4-NODE CLUSTER HYPERPARAMETER TUNING SWEEP (Issue #17 / V10)   ")
    print("==================================================================")
    
    # 1. Pipeline Chunk Sweep
    chunks = [65536, 131072, 262144, 524288, 1048576]
    best_bw = 0.0
    best_chunk = 262144
    print("\n--- Stage 1: Pipeline Chunk Sweep ---")
    for ch in chunks:
        cfg = dict(best_config)
        cfg["PG_PIPELINE_CHUNK"] = ch
        cfg["PG_RDMA_WINDOW"] = 32
        cfg["PG_RDMA_SIGNAL_INTERVAL"] = 16
        res = run_test(cfg)
        if res:
            bw_1g = res.get(1073741824, 0.0)
            bw_256m = res.get(268435456, 0.0)
            print(f"Chunk: {ch:7d} B ({ch//1024:4d} KiB) -> 256M: {bw_256m:6.2f} Gbps | 1G: {bw_1g:6.2f} Gbps")
            if bw_1g > best_bw:
                best_bw = bw_1g
                best_chunk = ch
        else:
            print(f"Chunk: {ch:7d} B -> FAILED")
    best_config["PG_PIPELINE_CHUNK"] = best_chunk
    print(f"--> Stage 1 Winner: PG_PIPELINE_CHUNK = {best_chunk} ({best_chunk//1024} KiB)\n")
    
    # 2. RDMA Window Sweep
    windows = [8, 16, 32, 64]
    best_bw = 0.0
    best_win = 32
    print("--- Stage 2: RDMA Window Sweep ---")
    for win in windows:
        cfg = dict(best_config)
        cfg["PG_RDMA_WINDOW"] = win
        cfg["PG_RDMA_SIGNAL_INTERVAL"] = max(2, win // 2)
        res = run_test(cfg)
        if res:
            bw_1g = res.get(1073741824, 0.0)
            bw_256m = res.get(268435456, 0.0)
            print(f"Window: {win:2d} (Signal: {cfg['PG_RDMA_SIGNAL_INTERVAL']:2d}) -> 256M: {bw_256m:6.2f} Gbps | 1G: {bw_1g:6.2f} Gbps")
            if bw_1g > best_bw:
                best_bw = bw_1g
                best_win = win
        else:
            print(f"Window: {win:2d} -> FAILED")
    best_config["PG_RDMA_WINDOW"] = best_win
    best_config["PG_RDMA_SIGNAL_INTERVAL"] = max(2, best_win // 2)
    print(f"--> Stage 2 Winner: PG_RDMA_WINDOW = {best_win}\n")
    
    # 3. Batch Size Sweep
    batches = [1, 4, 8, 16]
    best_bw = 0.0
    best_batch = 8
    print("--- Stage 3: WR Batch Size Sweep ---")
    for b in batches:
        cfg = dict(best_config)
        cfg["PG_BATCH_SIZE"] = b
        res = run_test(cfg)
        if res:
            bw_1g = res.get(1073741824, 0.0)
            bw_256m = res.get(268435456, 0.0)
            print(f"Batch Size: {b:2d} -> 256M: {bw_256m:6.2f} Gbps | 1G: {bw_1g:6.2f} Gbps")
            if bw_1g > best_bw:
                best_bw = bw_1g
                best_batch = b
        else:
            print(f"Batch Size: {b:2d} -> FAILED")
    best_config["PG_BATCH_SIZE"] = best_batch
    print(f"--> Stage 3 Winner: PG_BATCH_SIZE = {best_batch}\n")
    
    # 4. Signal Interval Sweep
    signals = [2, 4, 8, 16, best_config["PG_RDMA_WINDOW"]]
    signals = sorted(list(set([s for s in signals if s <= best_config["PG_RDMA_WINDOW"]])))
    best_bw = 0.0
    best_sig = best_config["PG_RDMA_SIGNAL_INTERVAL"]
    print("--- Stage 4: Signal Interval Sweep ---")
    for sig in signals:
        cfg = dict(best_config)
        cfg["PG_RDMA_SIGNAL_INTERVAL"] = sig
        res = run_test(cfg)
        if res:
            bw_1g = res.get(1073741824, 0.0)
            bw_256m = res.get(268435456, 0.0)
            print(f"Signal Interval: {sig:2d} -> 256M: {bw_256m:6.2f} Gbps | 1G: {bw_1g:6.2f} Gbps")
            if bw_1g > best_bw:
                best_bw = bw_1g
                best_sig = sig
        else:
            print(f"Signal Interval: {sig:2d} -> FAILED")
    best_config["PG_RDMA_SIGNAL_INTERVAL"] = best_sig
    print(f"--> Stage 4 Winner: PG_RDMA_SIGNAL_INTERVAL = {best_sig}\n")

    # 5. Streaming Stores Sweep
    print("--- Stage 5: Streaming Store Sweep ---")
    for ss in [0, 1]:
        cfg = dict(best_config)
        cfg["PG_STREAMING_STORES"] = ss
        res = run_test(cfg)
        if res:
            bw_1g = res.get(1073741824, 0.0)
            bw_256m = res.get(268435456, 0.0)
            print(f"Streaming Stores ({'ON' if ss else 'OFF'}): -> 256M: {bw_256m:6.2f} Gbps | 1G: {bw_1g:6.2f} Gbps")
            if bw_1g > best_bw:
                best_bw = bw_1g
                best_config["PG_STREAMING_STORES"] = ss
    
    print("\n==================================================================")
    print("  FINAL OPTIMAL TUNING CONFIGURATION FOR 4-NODE CLUSTER          ")
    print("==================================================================")
    for k, v in best_config.items():
        print(f"  {k:26s} : {v}")
    print(f"  Peak 1 GiB Bandwidth       : {best_bw:.2f} Gbps")
    print("==================================================================")

if __name__ == "__main__":
    main()
