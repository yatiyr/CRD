#!/bin/bash
# The einsum execution board (docs/bench/2026-07-03-v14f-einsum-exec-vs-numpy-torch.md) on the linux-gcc-release lane
# tree of the WSL source mirror: the einsum gates, the Cerid bench at each CPU level the host runs (run-lane-bench.sh;
# CRD_CPU_LEVEL selects the level of the tensor and dense kernels, ADR-0134), then the Python peers, single-threaded on
# one core.
#   bash scripts/run_einsum_bench.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-einsum-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^v14-f'
bash scripts/run-lane-bench.sh bench_einsum -DCRD_BUILD_RETAINED_BENCHES=ON
echo "--- peers (1T pinned) ---"
bash scripts/wsl-lane.sh run linux-gcc-release -- env OMP_NUM_THREADS=1 taskset -c 4 \
    python3 "$PWD/scripts/bench_einsum_peers.py"
