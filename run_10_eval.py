#!/usr/bin/env python3
"""
Automated 10-run benchmark collector and statistical comparator for ex3_network.
Runs ./run_cluster_test.sh 4 repeatedly, parses the benchmark results,
and computes summary statistics (mean, median, stddev) to evaluate refactorings.
"""

import sys
import os
import re
import json
import time
import subprocess
import statistics

SIZES = [67108864, 134217728, 268435456, 536870912, 1073741824]
SIZE_LABELS = {
    67108864: "64 MiB",
    134217728: "128 MiB",
    268435456: "256 MiB",
    536870912: "512 MiB",
    1073741824: "1 GiB"
}

def parse_output(stdout_text):
    results = {}
    pattern = re.compile(r"^\s*(\d+)\s+\|\s*(\d+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s+\|\s*([\d\.]+)\s*Gbps", re.MULTILINE)
    for match in pattern.finditer(stdout_text):
        sz = int(match.group(1))
        count = int(match.group(2))
        min_us = float(match.group(3))
        med_us = float(match.group(4))
        avg_us = float(match.group(5))
        bw_gbps = float(match.group(6))
        results[sz] = {
            "count": count,
            "min_us": min_us,
            "median_us": med_us,
            "avg_us": avg_us,
            "bw_gbps": bw_gbps
        }
    return results

def run_suite(iterations=10, tag="baseline", out_json=None):
    print(f"\n=======================================================")
    print(f"  Starting {iterations}-run Benchmark Suite [{tag}]")
    print(f"=======================================================")

    all_runs = []
    i = 1
    while i <= iterations:
        success = False
        for attempt in range(1, 4):
            print(f"[{tag}] Run {i}/{iterations} (attempt {attempt})...", end=" ", flush=True)
            start_t = time.time()
            res = subprocess.run(
                ["./run_cluster_test.sh", "4"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True
            )
            elapsed = time.time() - start_t

            if res.returncode == 0:
                run_data = parse_output(res.stdout)
                if len(run_data) >= len(SIZES):
                    print(f"PASSED in {elapsed:.1f}s (1 GiB: {run_data[1073741824]['bw_gbps']:.2f} Gbps, {run_data[1073741824]['median_us']:.1f} us)")
                    all_runs.append(run_data)
                    success = True
                    i += 1
                    time.sleep(1)
                    break
                else:
                    print(f"Incomplete output ({len(run_data)}/{len(SIZES)} sizes), retrying...")
            else:
                print(f"FAILED (rc={res.returncode}) in {elapsed:.1f}s, cleaning up & retrying...")
            
            # Cleanup remote ranks if failed
            for h in ["mlx-stud-01", "mlx-stud-02", "mlx-stud-03", "mlx-stud-04"]:
                subprocess.run(["ssh", h, "pkill -u ateret.tabib -9 -f './test -myindex' 2>/dev/null || true"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(2)
        
        if not success:
            print(f"Run {i} failed after 3 attempts.")
            sys.exit(1)

    # Compute statistics across runs
    summary = {}
    for sz in SIZES:
        med_latencies = [r[sz]["median_us"] for r in all_runs if sz in r]
        bandwidths = [r[sz]["bw_gbps"] for r in all_runs if sz in r]
        
        if med_latencies:
            summary[sz] = {
                "label": SIZE_LABELS.get(sz, f"{sz} B"),
                "runs_count": len(med_latencies),
                "lat_mean_us": statistics.mean(med_latencies),
                "lat_median_us": statistics.median(med_latencies),
                "lat_stdev_us": statistics.stdev(med_latencies) if len(med_latencies) > 1 else 0.0,
                "bw_mean_gbps": statistics.mean(bandwidths),
                "bw_median_gbps": statistics.median(bandwidths),
                "bw_stdev_gbps": statistics.stdev(bandwidths) if len(bandwidths) > 1 else 0.0,
                "all_median_us": med_latencies,
                "all_bw_gbps": bandwidths
            }

    print("\n-------------------------------------------------------------------------------------------------")
    print(f"  Summary Statistics: {tag} ({iterations} runs)")
    print("-------------------------------------------------------------------------------------------------")
    print(f"{'Size':<10} | {'Median Lat (mean ± std)':<28} | {'Bandwidth (mean ± std)':<26}")
    print("-------------------------------------------------------------------------------------------------")
    for sz in SIZES:
        if sz in summary:
            s = summary[sz]
            lat_str = f"{s['lat_mean_us']:10.1f} ± {s['lat_stdev_us']:<6.1f} us"
            bw_str = f"{s['bw_mean_gbps']:6.2f} ± {s['bw_stdev_gbps']:<4.2f} Gbps"
            print(f"{s['label']:<10} | {lat_str:<28} | {bw_str:<26}")
    print("-------------------------------------------------------------------------------------------------\n")

    if out_json:
        with open(out_json, "w") as f:
            json.dump({"tag": tag, "iterations": iterations, "summary": summary, "runs": all_runs}, f, indent=2)
        print(f"Saved run data to {out_json}")

    return summary

def compare_results(baseline_json, target_json):
    with open(baseline_json) as f:
        b_data = json.load(f)
    with open(target_json) as f:
        t_data = json.load(f)

    b_sum = b_data["summary"]
    t_sum = t_data["summary"]

    print("==================================================================================================================")
    print(f"  COMPARISON: [{b_data.get('tag', 'Baseline')}] vs [{t_data.get('tag', 'New')}] (10 runs each)")
    print("==================================================================================================================")
    print(f"{'Size':<10} | {'Base Lat (us)':<14} | {'New Lat (us)':<14} | {'Lat Delta (%)':<14} | {'Base BW (Gbps)':<15} | {'New BW (Gbps)':<15} | {'BW Delta (%)':<14}")
    print("------------------------------------------------------------------------------------------------------------------")

    for sz_str, b in b_sum.items():
        sz = int(sz_str) if sz_str.isdigit() else sz_str
        if sz_str in t_sum:
            t = t_sum[sz_str]
            b_lat = b["lat_mean_us"]
            t_lat = t["lat_mean_us"]
            lat_delta_pct = ((t_lat - b_lat) / b_lat) * 100.0

            b_bw = b["bw_mean_gbps"]
            t_bw = t["bw_mean_gbps"]
            bw_delta_pct = ((t_bw - b_bw) / b_bw) * 100.0

            lat_diff_sign = "+" if lat_delta_pct > 0 else ""
            bw_diff_sign = "+" if bw_delta_pct > 0 else ""

            print(f"{b['label']:<10} | {b_lat:10.1f} us  | {t_lat:10.1f} us  | {lat_diff_sign}{lat_delta_pct:6.2f}%       | {b_bw:7.2f} Gbps    | {t_bw:7.2f} Gbps    | {bw_diff_sign}{bw_delta_pct:6.2f}%")
    print("------------------------------------------------------------------------------------------------------------------\n")

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "compare":
        if len(sys.argv) < 4:
            print("Usage: python3 run_10_eval.py compare <baseline.json> <target.json>")
            sys.exit(1)
        compare_results(sys.argv[2], sys.argv[3])
    else:
        tag = sys.argv[1] if len(sys.argv) > 1 else "run"
        out_file = sys.argv[2] if len(sys.argv) > 2 else f"{tag}.json"
        iters = int(sys.argv[3]) if len(sys.argv) > 3 else 10
        run_suite(iterations=iters, tag=tag, out_json=out_file)
