#!/bin/bash
# v11-s: build + run the detection/measurement bench (Cerid) then the scipy column. MATLAB (thd/snr) runs on Windows.
set -e
cd /mnt/d/Dev/cerid
B=$HOME/cerid-build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DNDEBUG \
  -I "$B/core/include" \
  -I engine/hesap-dsp/include -I engine/hesap-fft/include -I engine/hesap-dense/include \
  -I engine/hesap/include -I engine/core/include -I engine/containers/include -I engine/memory/include \
  -I engine/log/include -I engine/vm/include -I engine/math/include -I engine/units/include \
  -I engine/jobs/include -I engine/hesap-sched/include \
  runtime/examples/bench_dsp_measure_vs_refs.cpp \
  -Wl,--start-group \
    "$B/hesap-dsp/libcrd-hesap-dsp.a" "$B/hesap-fft/libcrd-hesap-fft.a" "$B/hesap-dense/libcrd-hesap-dense.a" \
    "$B/hesap/libcrd-hesap.a" "$B/hesap-sched/libcrd-hesap-sched.a" "$B/jobs/libcrd-jobs.a" \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group -lpthread -lm \
  -o /tmp/bench_dsp_measure
echo "BUILD OK"
/tmp/bench_dsp_measure
echo ""
echo "--- scipy reference ---"
python3 runtime/examples/bench_dsp_measure_refs.py
