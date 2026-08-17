#!/usr/bin/env python3
"""
Comprehensive Benchmark Runner: Executes all sweeps on the 4-node physical cluster
and outputs formatted Markdown tables for empirical_protocol_report.md and README.md.
"""

import os
import sys
import re
import time
import subprocess

HOSTS = ["mlx-stud-01", "mlx-stud-02", "mlx-stud-03", "mlx-stud-04"]
CLUSTER_DIR = "/cs/usr/ateret.tabib/Downloads/ex3_network"

def sync_and_build(mode="auto"):
    print(f"\n[Bench] Syncing workspace to {HOSTS[0]} and compiling with MODE={mode}...")
    subprocess.run(
        ["rsync", "-avz", "--exclude", ".git", "--exclude", "*.o", "--exclude", "test", "./", f"{HOSTS[0]}:{CLUSTER_DIR}/"],
        check=True,
        stdout=subprocess.DEVNULL
    )
    subprocess.run(
        ["ssh", HOSTS[0], f"cd {CLUSTER_DIR} && make clean && make MODE={mode}"],
        check=True
    )

def run_single_sweep(env_overrides=None):
    if env_overrides is None:
        env_overrides = {}
    env_str = " ".join([f"{k}={v}" for k, v in env_overrides.items()])
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
            print(f"[ERROR] Rank {rank} on {HOSTS[rank]} failed:\n{err}")
    
    if not success:
        return {}
    
    rank0_out = outs[0]
    results = {}
    for line in rank0_out.splitlines():
        m = re.match(r"^\s*(\d+)\s+\|\s*(\d+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s*Gbps", line)
        if m:
            sz = int(m.group(1))
            count = int(m.group(2))
            min_us = float(m.group(3))
            med_us = float(m.group(4))
            avg_us = float(m.group(5))
            bw_gbps = float(m.group(6))
            results[sz] = {
                "count": count,
                "min_us": min_us,
                "median_us": med_us,
                "avg_us": avg_us,
                "bw_gbps": bw_gbps
            }
    return results

def format_bytes(n):
    if n >= 1024 * 1024 * 1024:
        return f"{n / (1024**3):.0f} GiB"
    elif n >= 1024 * 1024:
        return f"{n / (1024**2):.0f} MiB"
    elif n >= 1024:
        return f"{n / 1024:.0f} KiB"
    return f"{n} B"

def main():
    print("=" * 80)
    print("STARTING FULL RE-BENCHMARKING ON 4-NODE CLUSTER (mlx-stud-01..04)")
    print("=" * 80)

    # 1. Sweep EAGER
    sync_and_build(mode="eager")
    eager_results = run_single_sweep({
        "PG_BENCH_MIN_BYTES": 64,
        "PG_BENCH_MAX_BYTES": 16 * 1024 * 1024,
        "PG_BENCH_ITER": 5
    })
    print(f"Eager results collected for {len(eager_results)} sizes.")

    # 2. Sweep RENDEZVOUS
    sync_and_build(mode="rendezvous")
    rdv_results = run_single_sweep({
        "PG_BENCH_MIN_BYTES": 64,
        "PG_BENCH_MAX_BYTES": 1024 * 1024 * 1024,
        "PG_BENCH_ITER": 5
    })
    print(f"Rendezvous results collected for {len(rdv_results)} sizes.")

    # 3. Sweep AUTO
    sync_and_build(mode="auto")
    auto_results = run_single_sweep({
        "PG_BENCH_MIN_BYTES": 64,
        "PG_BENCH_MAX_BYTES": 1024 * 1024 * 1024,
        "PG_BENCH_ITER": 5
    })
    print(f"Auto results collected for {len(auto_results)} sizes.")

    # 4. Micro-Chunk Sensitivity (64 MiB)
    chunk_sizes = [64 * 1024, 128 * 1024, 256 * 1024, 512 * 1024, 1024 * 1024]
    chunk_results = {}
    for csz in chunk_sizes:
        res = run_single_sweep({
            "PG_BENCH_MIN_BYTES": 64 * 1024 * 1024,
            "PG_BENCH_MAX_BYTES": 64 * 1024 * 1024,
            "PG_BENCH_ITER": 5,
            "PG_PIPELINE_CHUNK": csz
        })
        if 64 * 1024 * 1024 in res:
            chunk_results[csz] = res[64 * 1024 * 1024]

    # 5. Window Depth Sensitivity (64 MiB)
    windows = [1, 8, 16, 32, 64]
    win_results = {}
    for win in windows:
        res = run_single_sweep({
            "PG_BENCH_MIN_BYTES": 64 * 1024 * 1024,
            "PG_BENCH_MAX_BYTES": 64 * 1024 * 1024,
            "PG_BENCH_ITER": 5,
            "PG_RDMA_WINDOW": win
        })
        if 64 * 1024 * 1024 in res:
            win_results[win] = res[64 * 1024 * 1024]

    # 6. Multi-WR Batch Depth Sensitivity (64 MiB)
    batches = [1, 4, 8, 16]
    batch_results = {}
    for b in batches:
        res = run_single_sweep({
            "PG_BENCH_MIN_BYTES": 64 * 1024 * 1024,
            "PG_BENCH_MAX_BYTES": 64 * 1024 * 1024,
            "PG_BENCH_ITER": 5,
            "PG_BATCH_SIZE": b
        })
        if 64 * 1024 * 1024 in res:
            batch_results[b] = res[64 * 1024 * 1024]

    print("\n\n" + "=" * 120)
    print("### FULL PROTOCOL COMPARISON TABLE (SECTION 3)")
    print("=" * 120)
    print("| Message Size | Element Count | Eager Latency (us) | Rendezvous Latency (us) | Auto Mode Latency (us) | Eager BW (Gbps) | Rendezvous BW (Gbps) | Auto BW (Gbps) | Optimal Protocol |")
    print("| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |")
    
    all_sizes = sorted(list(set(list(eager_results.keys()) + list(rdv_results.keys()) + list(auto_results.keys()))))
    for sz in all_sizes:
        cnt = auto_results.get(sz, rdv_results.get(sz, eager_results.get(sz, {}))).get("count", sz // 4)
        e_m = eager_results.get(sz, {}).get("median_us")
        r_m = rdv_results.get(sz, {}).get("median_us")
        a_m = auto_results.get(sz, {}).get("median_us")
        e_bw = eager_results.get(sz, {}).get("bw_gbps")
        r_bw = rdv_results.get(sz, {}).get("bw_gbps")
        a_bw = auto_results.get(sz, {}).get("bw_gbps")

        e_str = f"{e_m:.1f}" if e_m is not None else "N/A (Pool Cap)"
        r_str = f"{r_m:.1f}" if r_m is not None else "N/A"
        a_str = f"{a_m:.1f}" if a_m is not None else "N/A"
        e_bw_str = f"{e_bw:.2f}" if e_bw is not None else "N/A"
        r_bw_str = f"{r_bw:.2f}" if r_bw is not None else "N/A"
        a_bw_str = f"{a_bw:.2f}" if a_bw is not None else "N/A"

        winner = ""
        if e_m and r_m:
            if e_m < r_m:
                winner = f"**Eager (${r_m/e_m:.1f}\\times$ faster)**"
            else:
                winner = f"**Rendezvous (${e_m/r_m:.1f}\\times$ faster)**"
        elif r_m:
            if sz == 1024 * 1024 * 1024:
                winner = "**Rendezvous (Peak)**"
            else:
                winner = "**Rendezvous**"
        
        print(f"| **{format_bytes(sz)}** | {cnt:,} | {e_str} | {r_str} | {a_str} | {e_bw_str} | {r_bw_str} | {a_bw_str} | {winner} |")

    print("\n\n" + "=" * 80)
    print("### MICRO-CHUNK SENSITIVITY TABLE (SECTION 4.1)")
    print("=" * 80)
    print("| Chunk Size | Latency (ms) | Effective BW (Gbps) |")
    print("| :--- | :--- | :--- |")
    for csz, d in chunk_results.items():
        print(f"| {format_bytes(csz)} | {d['median_us']/1000.0:.2f} | {d['bw_gbps']:.2f} |")

    print("\n\n" + "=" * 80)
    print("### SLIDING WINDOW DEPTH TABLE (SECTION 4.2)")
    print("=" * 80)
    print("| Window Depth | Latency (ms) | Effective BW (Gbps) |")
    print("| :--- | :--- | :--- |")
    for win, d in win_results.items():
        print(f"| {win} | {d['median_us']/1000.0:.2f} | {d['bw_gbps']:.2f} |")

    print("\n\n" + "=" * 80)
    print("### MULTI-WR BATCH DEPTH TABLE (SECTION 4.3)")
    print("=" * 80)
    print("| Batch Depth | Latency (ms) | Effective BW (Gbps) |")
    print("| :--- | :--- | :--- |")
    for b, d in batch_results.items():
        print(f"| {b} | {d['median_us']/1000.0:.2f} | {d['bw_gbps']:.2f} |")

if __name__ == "__main__":
    main()
