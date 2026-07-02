#!/usr/bin/env bash
# setup-strumpack-ref.sh -- build STRUMPACK as a local benchmarking oracle fo
# the v5e HSS/ULV crush (bench_hesap_hss_vs_strumpack). WSL/Linux only; dev-only,
# NEVER shipped, NEVER in CI. Installs to $HOME/strumpack/install.
#
# Usage:  bash scripts/setup-strumpack-ref.sh
# Then:   configure Cerid with -DCRD_BUILD_HESAP_VS_STRUMPACK=ON
#         (optionally -DCRD_STRUMPACK_DIR=<prefix> if not the default).
set -euo pipefail

PREFIX="${HOME}/strumpack/install"

echo "== STRUMPACK deps (apt) =="
# STRUMPACK needs MPI + ScaLAPACK + BLAS/LAPACK + (Par)METIS. OpenMP comes with gcc.
sudo apt-get update -y
sudo apt-get install -y build-essential cmake git \
    libopenblas-dev liblapack-dev libopenmpi-dev openmpi-bin \
    libscalapack-openmpi-dev libscalapack-mpi-dev libmetis-dev libparmetis-dev

echo "== clone + build STRUMPACK =="
cd "${HOME}"
rm -rf strumpack
git clone --depth 1 https://github.com/pghysels/STRUMPACK strumpack
cd strumpack
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${PREFIX}" -DSTRUMPACK_USE_OPENMP=ON
cmake --build build --target install -j "$(( $(nproc) / 2 ))"

echo "== done: ${PREFIX} =="
ls "${PREFIX}/include/HSS/HSSMatrix.hpp" "${PREFIX}/lib/libstrumpack.a"
