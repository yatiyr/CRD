#!/bin/bash
# v11w-b: build + run the wavedec throughput bench (Cerid) then the pywt column. MATLAB runs on Windows
# (tests/hesap-wavelet/bench_wavelet_matlab.m, 1-thread). WSL, single-threaded, AVX2+FMA, N=1M.
set -e
cd /mnt/d/Dev/cerid
B=$HOME/cerid-build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DNDEBUG \
  -I "$B/core/include" \
  -I engine/hesap-wavelet/include -I engine/hesap-fft/include -I engine/hesap-dense/include \
  -I engine/hesap/include -I engine/core/include -I engine/containers/include -I engine/memory/include \
  -I engine/log/include -I engine/vm/include -I engine/math/include -I engine/units/include -I engine/jobs/include \
  runtime/examples/bench_wavelet_vs_refs.cpp \
  -Wl,--start-group \
    "$B/hesap-wavelet/libcrd-hesap-wavelet.a" "$B/hesap-fft/libcrd-hesap-fft.a" "$B/hesap-dense/libcrd-hesap-dense.a" \
    "$B/hesap/libcrd-hesap.a" "$B/jobs/libcrd-jobs.a" \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group -lpthread -lm \
  -o /tmp/bench_wavelet
echo "BUILD OK"
/tmp/bench_wavelet
echo ""
echo "--- pywt reference (C core, 1 thread) ---"
OMP_NUM_THREADS=1 python3 runtime/examples/bench_wavelet_refs.py
