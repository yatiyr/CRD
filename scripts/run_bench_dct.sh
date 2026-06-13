#!/bin/bash
# v10-f: build + run the DCT-II throughput shootout (Cerid vs PocketFFT vs FFTW). WSL, 1 thread, AVX2+FMA.
set -e
cd /mnt/d/Dev/cerid
B=build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DCRD_DETERMINISTIC_FP=1 \
  -I build/linux-gcc-release/engine/core/include \
  -I engine/hesap-fft/include -I engine/hesap/include -I engine/core/include \
  -I engine/containers/include -I engine/memory/include -I engine/log/include -I engine/vm/include \
  -I engine/math/include -I "$HOME/fft_refs" \
  runtime/examples/bench_dct_vs_refs.cpp \
  -Wl,--start-group \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group \
  -lfftw3 -lm \
  -o /tmp/bench_dct
echo "BUILD OK"
/tmp/bench_dct
