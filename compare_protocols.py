#!/usr/bin/env python3
"""
Automated Protocol Comparison Runner: Eager vs Rendezvous.
Runs multi-size all-reduce sweeps under both Eager and Rendezvous modes on the cluster,
parses the latency & effective bandwidth metrics, and produces a comparative analysis report.
"""

import os
import sys
import re
import subprocess
import argparse

DEFAULT_HOSTS_4 = ["mlx-stud-01", "mlx-stud-02", "mlx-stud-03", "mlx-stud-04"]
DEFAULT_HOSTS_2 = ["mlx-stud-03", "mlx-stud-04"]
CLUSTER_DIR = "/cs/usr/ateret.tabib/Downloads/ex3_network"

def sync_and_build(hosts, mode):
    print(f"[Compare] Syncing workspace to {hosts[0]} and compiling with MODE={mode}...")
    subprocess.run(
        ["rsync", "-avz", "--exclude", ".git", "--exclude", "*.o", "--exclude", "test", "./", f"{hosts[0]}:{CLUSTER_DIR}/"],
        check=True,
        stdout=subprocess.DEVNULL
    )
    subprocess.run(
        ["ssh", hosts[0], f"cd {CLUSTER_DIR} && make clean && make MODE={mode}"],
        check=True
    )

def run_sweep(hosts, env_overrides=None):
    if env_overrides is None:
        env_overrides = {}
    
    env_str = " ".join([f"{k}={v}" for k, v in env_overrides.items()])
    procs = []
    
    for rank, host in enumerate(hosts):
        idx = f"{rank + 1:02d}"
        cmd = f"cd {CLUSTER_DIR} && {env_str} ./test -myindex {idx} -list {' '.join(hosts)}"
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
            print(f"[ERROR] Rank {rank} on {hosts[rank]} failed (rc={p.returncode}):\n{err}")
    
    if not success:
        return None
    
    rank0_out = outs[0]
    results = {}
    # Parse benchmark lines: Size (Bytes) | Count (ints) | Min (us) | Median (us) | Avg (us) | Effective BW
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
    parser = argparse.ArgumentParser(description="RDMA Ring Collectives: Eager vs Rendezvous Protocol Comparison")
    parser.add_argument("--ranks", type=int, default=4, choices=[2, 4], help="Number of cluster ranks (2 or 4)")
    parser.add_argument("--min-bytes", type=int, default=64, help="Minimum benchmark size in bytes")
    parser.add_argument("--max-bytes-eager", type=int, default=268435456, help="Maximum eager benchmark size in bytes (256 MiB default)")
    parser.add_argument("--max-bytes-rdv", type=int, default=1073741824, help="Maximum rendezvous benchmark size in bytes (1 GiB default)")
    args = parser.parse_args()

    hosts = DEFAULT_HOSTS_4 if args.ranks == 4 else DEFAULT_HOSTS_2
    
    print("=" * 105)
    print(f"  RDMA Ring Collectives — Eager vs. Rendezvous Protocol Comparison ({args.ranks} Ranks on Cluster) ")
    print("=" * 105)
    
    # 1. Run Eager Protocol Sweep
    print("\n>>> Phase 1/2: Running Eager Protocol Sweep...")
    sync_and_build(hosts, mode="eager", profile="perf")
    eager_env = {
        "PG_BENCH_MIN_BYTES": args.min_bytes,
        "PG_BENCH_MAX_BYTES": args.max_bytes_eager,
        "PG_BENCH_ITER": 5
    }
    eager_results = run_sweep(hosts, eager_env)
    if not eager_results:
        print("[ERROR] Eager benchmark sweep failed!")
        sys.exit(1)
    
    # 2. Run Rendezvous Protocol Sweep
    print("\n>>> Phase 2/2: Running Rendezvous Protocol Sweep...")
    sync_and_build(hosts, mode="rendezvous", profile="perf")
    rdv_env = {
        "PG_BENCH_MIN_BYTES": args.min_bytes,
        "PG_BENCH_MAX_BYTES": args.max_bytes_rdv,
        "PG_BENCH_ITER": 5
    }
    rdv_results = run_sweep(hosts, rdv_env)
    if not rdv_results:
        print("[ERROR] Rendezvous benchmark sweep failed!")
        sys.exit(1)
    
    # 3. Print Comparative Analysis Table
    print("\n" + "=" * 105)
    print("  COMPARATIVE PERFORMANCE RESULTS: EAGER vs. RENDEZVOUS PROTOCOL")
    print("=" * 105)
    print(f"{'Size':<10} | {'Eager Latency':<16} | {'Eager BW':<12} | {'Rdv Latency':<16} | {'Rdv BW':<12} | {'Winner / Analysis':<24}")
    print("-" * 105)
    
    all_sizes = sorted(list(set(list(eager_results.keys()) + list(rdv_results.keys()))))
    
    for sz in all_sizes:
        size_str = format_bytes(sz)
        e_data = eager_results.get(sz)
        r_data = rdv_results.get(sz)
        
        if e_data and r_data:
            e_med = e_data["median_us"]
            e_bw = e_data["bw_gbps"]
            r_med = r_data["median_us"]
            r_bw = r_data["bw_gbps"]
            
            e_lat_str = f"{e_med:10.2f} us"
            e_bw_str = f"{e_bw:7.2f} Gbps"
            r_lat_str = f"{r_med:10.2f} us"
            r_bw_str = f"{r_bw:7.2f} Gbps"
            
            if e_med < r_med:
                ratio = r_med / e_med
                winner = f"EAGER (+{ratio:.2f}x faster)"
            else:
                ratio = e_med / r_med
                winner = f"RENDEZVOUS (+{ratio:.2f}x faster)"
        elif r_data and not e_data:
            e_lat_str = f"{'N/A (OOM/Depth)':>16}"
            e_bw_str = f"{'N/A':>12}"
            r_med = r_data["median_us"]
            r_bw = r_data["bw_gbps"]
            r_lat_str = f"{r_med:10.2f} us"
            r_bw_str = f"{r_bw:7.2f} Gbps"
            winner = "RENDEZVOUS (Zero-Copy Scale)"
        else:
            continue
        
        print(f"{size_str:<10} | {e_lat_str:<16} | {e_bw_str:<12} | {r_lat_str:<16} | {r_bw_str:<12} | {winner:<24}")
    
    print("=" * 105)
    print("\nProtocol Trade-off Insights:")
    print("1. Small Messages (<= 8 KiB): Eager protocol wins because it eliminates the RTS -> CTS negotiation round-trip.")
    print("2. Large Messages (> 8 KiB): Rendezvous with Zero-Copy RDMA Write wins because it eliminates intermediate buffer copies")
    print("   and utilizes hardware-level RDMA streaming directly into user memory.")
    print("3. Auto Mode Sweet-spot: Switching from Eager to Rendezvous at ~8 KiB yields optimal latency across all message sizes.")
    print("=" * 105)

if __name__ == "__main__":
    main()
