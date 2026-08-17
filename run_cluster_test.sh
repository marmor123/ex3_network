#!/bin/bash
# ==============================================================================
# Multi-Node Cluster Test Runner for RDMA Collective Library (V1 TCP Bootstrap)
# ==============================================================================
# Usage:
#   ./run_cluster_test.sh 2              # Runs 2-node ring: mlx-stud-03 mlx-stud-04
#   ./run_cluster_test.sh 4              # Runs 4-node ring: mlx-stud-01..04
#   ./run_cluster_test.sh local 2        # Runs 2 ranks locally on localhost
#   ./run_cluster_test.sh local 4        # Runs 4 ranks locally on localhost
# ==============================================================================

set -e

MODE="${1:-local}"
NUM_NODES="${2:-2}"
CLUSTER_DIR="${CLUSTER_DIR:-/cs/usr/ateret.tabib/Downloads/ex3_network}"
SSH_OPTS="${SSH_OPTS:-}"

if [ "$MODE" = "2" ] || [ "$MODE" = "4" ]; then
    NUM_NODES="$MODE"
    MODE="cluster"
fi

echo "=================================================================="
echo "  RDMA Collective Library — Multi-Node Test Runner (V8 Benchmark)  "
echo "=================================================================="

if [ "$MODE" = "local" ]; then
    # Ensure binary is compiled locally
    make all
    echo "Running local $NUM_NODES-rank loopback test..."
    python3 test_v1_local.py
    exit 0
fi

# Cluster multi-node mode
if [ "$NUM_NODES" -eq 2 ]; then
    HOSTS=("mlx-stud-03" "mlx-stud-04")
elif [ "$NUM_NODES" -eq 4 ]; then
    HOSTS=("mlx-stud-01" "mlx-stud-02" "mlx-stud-03" "mlx-stud-04")
else
    echo "Error: Only 2 or 4 nodes supported (got $NUM_NODES)"
    exit 1
fi

echo "Launching ring across: ${HOSTS[*]}"
echo "Remote working directory: $CLUSTER_DIR"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "Syncing local workspace to ${HOSTS[0]}:$CLUSTER_DIR..."
rsync -avz --exclude '.git' --exclude '*.o' --exclude 'test' "$SCRIPT_DIR/" "${HOSTS[0]}:$CLUSTER_DIR/"

MODE_BUILD="${MODE_BUILD:-${COLLECTIVE_MODE:-auto}}"
echo "Building on ${HOSTS[0]} (MODE=$MODE_BUILD)..."
ssh $SSH_OPTS "${HOSTS[0]}" "cd $CLUSTER_DIR && make clean && make MODE=$MODE_BUILD"

PIDS=()
for i in "${!HOSTS[@]}"; do
    RANK=$i
    INDEX=$(printf "%02d" $((RANK + 1)))
    HOST="${HOSTS[$i]}"
    
    echo "Starting Rank $RANK (index $INDEX) on host $HOST..."
    ssh $SSH_OPTS "$HOST" "cd $CLUSTER_DIR && ./test -myindex $INDEX -list ${HOSTS[*]}" &
    PIDS+=($!)
done

# Wait for all remote processes
FAIL=0
for pid in "${PIDS[@]}"; do
    wait "$pid" || FAIL=1
done

if [ "$FAIL" -eq 0 ]; then
    echo "=================================================================="
    echo "  CLUSTER TEST SUCCESS: All $NUM_NODES nodes completed V8 Performance Benchmark Suite! "
    echo "=================================================================="
else
    echo "=================================================================="
    echo "  CLUSTER TEST FAILURE: One or more remote nodes failed.           "
    echo "=================================================================="
    exit 1
fi
