#!/bin/bash
# The nn inference board (docs/bench/2026-07-05-v14m-nn.md) on the linux-gcc-release lane tree of the WSL source
# mirror: the nn and tensor-kernel gates, the Cerid bench at each CPU level the host runs (run-lane-bench.sh;
# CRD_CPU_LEVEL selects the hesap-tensor kernels' level, ADR-0134), then the single-threaded torch and onnxruntime
# peers, all on one pinned core. The bench reads the committed corpus through CRD_NN_CORPUS (lane runs start in a
# scratch directory) and exits 1 when a tier's outputs leave their bound.
#   bash scripts/run_bench_nn.sh     CRD_BENCH_LEVELS as in run-lane-bench.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
bash scripts/wsl-lane.sh build linux-gcc-release crd-hesap-tensor-tests
bash scripts/wsl-lane.sh test linux-gcc-release -R '^nn: |^tensor kernels'
export CRD_NN_CORPUS="$PWD/tests/numerics/hesap-tensor/nn_corpus"
bash scripts/run-lane-bench.sh bench_nn -DCRD_BUILD_RETAINED_BENCHES=ON
echo "--- torch and onnxruntime peers (same machine, single thread) ---"
bash scripts/wsl-lane.sh run linux-gcc-release -- env OMP_NUM_THREADS=1 taskset -c 4 \
    python3 "$PWD/scripts/v14m_nn_peers.py"
