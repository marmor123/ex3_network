#!/usr/bin/env python3
"""
Empirical Sweep of PG_EAGER_THRESHOLD on 4-Node Cluster.
Tests thresholds [1 KiB, 2 KiB, 4 KiB, 8 KiB, 16 KiB, 32 KiB, 64 KiB] in MODE=auto,
measures actual median latencies across payload sizes 64 B to 1 MiB,
and prints the empirical winner.
"""

import sys
import re
import subprocess

HOSTS = ["mlx-stud-01", "mlx-stud-02", "mlx-stud-03", "mlx-stud-04"]
CLUSTER_DIR = "/cs/usr/ateret.tabib/Downloads/ex3_network"

def sync_and_build():
    print("[Threshold Sweep] Syncing workspace and compiling with MODE=auto PROFILE=perf...")
    subprocess.run(
        ["rsync", "-avz", "--exclude", ".git", "--exclude", "*.o", "--exclude", "test", "./", f"{HOSTS[0]}:{CLUSTER_DIR}/"],
        check=True,
        stdout=subprocess.DEVNULL
    )
    subprocess.run(
        ["ssh", HOSTS[0], f"cd {CLUSTER_DIR} && make clean && make MODE=auto PROFILE=perf"],
        check=True
    )

def run_test(threshold):
    env_vars = {
        "PG_EAGER_THRESHOLD": threshold,
        "PG_BENCH_MIN_BYTES": 64,
        "PG_BENCH_MAX_BYTES": 1048576,  # Up to 1 MiB
        "PG_BENCH_ITER": 7
    }
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
            print(f"[ERROR] Threshold {threshold} Rank {rank} failed:\n{err}")
    
    if not success:
        return None
    
    rank0_out = outs[0]
    results = {}
    for line in rank0_out.splitlines():
        m = re.match(r"^\s*(\d+)\s+\|\s*(\d+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s*Gbps", line)
        if m:
            sz = int(m.group(1))
            med_us = float(m.group(4))
            bw = float(m.group(6))
            results[sz] = {"median_us": med_us, "bw": bw}
    return results

def format_bytes(n):
    if n >= 1024 * 1024:
        return f"{n // (1024*1024)} MiB"
    elif n >= 1024:
        return f"{n // 1024} KiB"
    return f"{n} B"

def main():
    sync_and_build()
    
    thresholds = [1024, 2048, 4096, 8192, 16384, 32768, 65536]
    all_results = {}
    
    print("\n==================================================================================")
    print("  EMPIRICAL PG_EAGER_THRESHOLD SWEEP (4 Nodes, MODE=auto)                         ")
    print("==================================================================================")
    
    for t in thresholds:
        t_label = format_bytes(t)
        print(f"Testing PG_EAGER_THRESHOLD = {t_label} ({t} bytes)...", end="", flush=True)
        res = run_test(t)
        if res:
            all_results[t] = res
            print(" DONE")
        else:
            print(" FAILED")
    
    if not all_results:
        print("No valid results collected.")
        sys.exit(1)
    
    test_sizes = sorted(list(next(iter(all_results.values())).keys()))
    
    print("\n" + "=" * 110)
    print("  MEASURED MEDIAN LATENCY (microseconds) FOR EACH THRESHOLD")
    print("=" * 110)
    
    header = f"{'Size':<10} | " + " | ".join([f"{format_bytes(t):>10}" for t in thresholds]) + " | Best Threshold"
    print(header)
    print("-" * 110)
    
    for sz in test_sizes:
        size_label = format_bytes(sz)
        row = f"{size_label:<10} | "
        best_t = None
        best_lat = 1e9
        
        for t in thresholds:
            if t in all_results and sz in all_results[t]:
                lat = all_results[t][sz]["median_us"]
                row += f"{lat:10.2f} | "
                if lat < best_lat:
                    best_lat = lat
                    best_t = t
            else:
                row += f"{'N/A':>10} | "
        
        row += f" {format_bytes(best_t)} ({best_lat:.2f} us)"
        print(row)
    
    print("=" * 110)

if __name__ == "__main__":
    main()
