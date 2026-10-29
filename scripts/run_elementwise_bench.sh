#!/bin/bash
# The elementwise board (docs/bench/2026-07-02-v14b-elementwise-broadcast.md) on the linux-gcc-release lane tree of
# the WSL source mirror: the elementwise and tensor-kernel gates, the Cerid bench at each CPU level the host runs
# (run-lane-bench.sh; CRD_CPU_LEVEL selects the tensor kernels' level, ADR-0134), then the Python peers,
# single-threaded on one core.
#   bash scripts/run_elementwise_bench.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^v14-b|^tensor kernels'
bash scripts/run-lane-bench.sh bench_elementwise -DCRD_BUILD_RETAINED_BENCHES=ON
echo "--- python peers (single thread) ---"
bash scripts/wsl-lane.sh run linux-gcc-release -- env OMP_NUM_THREADS=1 taskset -c 4 \
    python3 "$PWD/scripts/bench_elementwise_peers.py"
