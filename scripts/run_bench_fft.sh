#!/bin/bash
# v10: build + run the FFT throughput shootout (Cerid vs FFTW / PocketFFT / MKL). WSL, single-threaded
# (fair — Cerid v10-a/b are single-threaded; refs pinned to 1 thread). AVX2+FMA to match the 14900K.
set -e
cd /mnt/d/Dev/cerid
B=build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DCRD_DETERMINISTIC_FP=1 \
  -I build/linux-gcc-release/engine/core/include \
  -I engine/hesap-fft/include -I engine/hesap/include -I engine/core/include \
  -I engine/containers/include -I engine/memory/include -I engine/log/include -I engine/vm/include \
  -I engine/math/include -I "$HOME/fft_refs" -I /usr/include/mkl \
  runtime/examples/bench_fft_vs_refs.cpp \
  -Wl,--start-group \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group \
  -lfftw3 -lmkl_rt \
  -o /tmp/bench_fft
echo "BUILD OK"
MKL_NUM_THREADS=1 OMP_NUM_THREADS=1 /tmp/bench_fft
