#!/usr/bin/env python3
"""
Automated Test Runner for Vertical Slice V1: TCP Bootstrap Dry-Run.
Spawns 2-rank and 4-rank ring processes locally on localhost / loopback,
verifies synchronization, edge ordering, and QP metadata exchange.
Works on Linux, macOS, and Windows (via WSL Ubuntu if ELF binary).
"""

import os
import sys
import time
import subprocess

TEST_BIN = "./test"

def get_exec_cmd(args):
    if sys.platform == "win32":
        return ["wsl", "-d", "Ubuntu", "sh", "-c", " ".join(args)]
    return args

def run_ring(ranks, hosts=None, timeout=15, verbose=False):
    if hosts is None:
        hosts = ["127.0.0.1"] * ranks
    
    print(f"\n--- Testing {ranks}-Rank Ring ({' -> '.join([f'r{i}' for i in range(ranks)])} -> r0) ---")
    
    procs = []
    
    # Launch all ranks in parallel
    for rank in range(ranks):
        myindex = f"{rank + 1:02d}"
        cmd_args = [TEST_BIN, "-myindex", myindex, "-list"] + hosts
        exec_cmd = get_exec_cmd(cmd_args)
        p = subprocess.Popen(
            exec_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        procs.append((rank, p))
    
    # Collect outputs and wait for completion
    results = {}
    for rank, p in procs:
        try:
            stdout, stderr = p.communicate(timeout=timeout)
            results[rank] = {
                "returncode": p.returncode,
                "stdout": stdout,
                "stderr": stderr
            }
        except subprocess.TimeoutExpired:
            p.kill()
            stdout, stderr = p.communicate()
            results[rank] = {
                "returncode": -1,
                "stdout": stdout,
                "stderr": stderr + "\n[Test Runner Error] Process timed out"
            }
    
    # Validate results
    all_success = True
    for rank, res in results.items():
        if res["returncode"] != 0:
            print(f"[FAIL] Rank {rank} exited with code {res['returncode']}")
            print(f"Stderr:\n{res['stderr']}")
            all_success = False
        else:
            if "SUCCESS: All ring edges established" not in res["stdout"]:
                print(f"[FAIL] Rank {rank} missing success marker in stdout")
                print(f"Stdout:\n{res['stdout']}")
                all_success = False
            else:
                print(f"[OK] Rank {rank} successfully established and verified ring edges.")
                if verbose:
                    print(f"\n--- Output from Rank {rank} ---")
                    print(res["stdout"])
    
    if all_success:
        print(f"[SUCCESS] All {ranks} ranks completed V1 TCP bootstrap dry-run.")
    else:
        print(f"[FAILURE] {ranks}-rank ring test failed.")
    
    return all_success

def test_v1():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    print("==================================================================")
    print("  RDMA Collective Library — V1 TCP Bootstrap Verification Suite   ")
    print("==================================================================")
    
    # Test 1: 2-rank ring
    ok2 = run_ring(ranks=2, hosts=["127.0.0.1", "127.0.0.1"], verbose=verbose)
    if not ok2:
        sys.exit(1)
        
    # Small pause to allow port reuse
    time.sleep(0.5)
    
    # Test 2: 4-rank ring
    ok4 = run_ring(ranks=4, hosts=["127.0.0.1", "127.0.0.1", "127.0.0.1", "127.0.0.1"], verbose=verbose)
    if not ok4:
        sys.exit(1)
        
    print("\n==================================================================")
    print("  ALL V1 TESTS PASSED: Edge-ordered TCP Bootstrap Verified!       ")
    print("==================================================================")

if __name__ == "__main__":
    test_v1()
