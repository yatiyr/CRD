#!/bin/bash
# The dtype conversion board (docs/bench/2026-07-02-v14a-dtype-converts.md) on the linux-gcc-release lane tree of the
# WSL source mirror: the hesap-tensor gate, the Cerid probe at each CPU level the host runs (run-lane-bench.sh;
# CRD_CPU_LEVEL selects the conversion and tensor kernels' level, ADR-0134), then the single-threaded peers.
#   bash scripts/run_dtype_bench.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^v14-a'
bash scripts/run-lane-bench.sh bench_dtypes -DCRD_BUILD_RETAINED_BENCHES=ON
echo "--- python peers (same machine, single thread) ---"
bash scripts/wsl-lane.sh run linux-gcc-release -- env OMP_NUM_THREADS=1 taskset -c 4 \
    python3 "$PWD/scripts/bench_dtypes_peers.py"
