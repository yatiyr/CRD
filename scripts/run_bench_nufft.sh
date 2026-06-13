#!/bin/bash
# v10-g: build + run the 1D NUFFT shootout (Cerid NufftPlan vs FINUFFT). WSL, single-threaded (fair — Cerid's
# v10-g spreader is serial; FINUFFT pinned via opts.nthreads=1 + OMP_NUM_THREADS=1). AVX2+FMA for the 14900K.
# Requires FINUFFT built at ~/finufft (lib-static/libfinufft.a) and libfftw3-dev.
set -e
cd /mnt/d/Dev/cerid
B=build/linux-gcc-release/engine
FI="$HOME/finufft"
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DCRD_DETERMINISTIC_FP=1 \
  -I build/linux-gcc-release/engine/core/include \
  -I engine/hesap-fft/include -I engine/hesap/include -I engine/core/include \
  -I engine/containers/include -I engine/memory/include -I engine/log/include -I engine/vm/include \
  -I engine/math/include -I "$FI/include" \
  runtime/examples/bench_nufft_vs_finufft.cpp \
  -Wl,--start-group \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group \
  "$FI/lib-static/libfinufft.a" -lfftw3 -lfftw3_omp -lgomp -lm \
  -o /tmp/bench_nufft
echo "BUILD OK"
OMP_NUM_THREADS=1 /tmp/bench_nufft
