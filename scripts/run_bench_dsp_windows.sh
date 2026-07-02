#!/bin/bash
# v11-b: build + run the window-generation throughput shootout (Cerid vs scipy vs MATLAB). WSL, single-threaded,
# AVX2+FMA. N=2^20 (compute-bound — the fair compiled-vs-compiled regime). Run the .py + MATLAB .m separately fo
# the scipy/MATLAB columns. Honest: window gen is one-time setup, not a streaming hot path (see the .cpp header).
set -e
cd /mnt/d/Dev/cerid
B=build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DNDEBUG \
  -I build/linux-gcc-release/engine/core/include \
  -I engine/hesap-dsp/include -I engine/hesap-fft/include -I engine/hesap-dense/include \
  -I engine/hesap/include -I engine/core/include -I engine/containers/include -I engine/memory/include \
  -I engine/log/include -I engine/vm/include -I engine/math/include -I engine/units/include \
  runtime/examples/bench_dsp_windows_vs_refs.cpp \
  -Wl,--start-group \
    "$B/hesap-dsp/libcrd-hesap-dsp.a" "$B/hesap-fft/libcrd-hesap-fft.a" "$B/hesap-dense/libcrd-hesap-dense.a" \
    "$B/hesap/libcrd-hesap.a" "$B/hesap-sched/libcrd-hesap-sched.a" "$B/jobs/libcrd-jobs.a" \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group -lpthread -lm \
  -o /tmp/bench_dsp_windows
echo "BUILD OK"
/tmp/bench_dsp_windows
echo ""
echo "--- scipy reference: python3 runtime/examples/bench_dsp_windows_refs.py ---"
python3 runtime/examples/bench_dsp_windows_refs.py
