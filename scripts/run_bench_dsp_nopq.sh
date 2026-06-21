#!/bin/bash
# v11-n/o/p/q: build + run the compute-bound benchmarks (Cerid czt + arburg) then the scipy czt column. MATLAB runs
# on Windows (tests/hesap-dsp/bench_nopq_matlab.m). WSL, single-threaded, AVX2+FMA.
set -e
cd /mnt/d/Dev/cerid
B=$HOME/cerid-build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DNDEBUG \
  -I "$B/core/include" \
  -I engine/hesap-dsp/include -I engine/hesap-fft/include -I engine/hesap-dense/include \
  -I engine/hesap/include -I engine/core/include -I engine/containers/include -I engine/memory/include \
  -I engine/log/include -I engine/vm/include -I engine/math/include -I engine/units/include \
  runtime/examples/bench_dsp_nopq_vs_refs.cpp \
  -Wl,--start-group \
    "$B/hesap-dsp/libcrd-hesap-dsp.a" "$B/hesap-fft/libcrd-hesap-fft.a" "$B/hesap-dense/libcrd-hesap-dense.a" \
    "$B/hesap/libcrd-hesap.a" "$B/hesap-sched/libcrd-hesap-sched.a" "$B/jobs/libcrd-jobs.a" \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group -lpthread -lm \
  -o /tmp/bench_dsp_nopq
echo "BUILD OK"
/tmp/bench_dsp_nopq
echo ""
echo "--- scipy reference ---"
python3 runtime/examples/bench_dsp_nopq_refs.py
