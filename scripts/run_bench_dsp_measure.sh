#!/bin/bash
# v11-s: build + run the detection/measurement bench (Cerid) then the scipy column. MATLAB (thd/snr) runs on Windows.
set -e
cd /mnt/d/Dev/cerid
B=$HOME/cerid-build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DNDEBUG \
  -I "$B/core/include" \
  -I engine/numerics/hesap-dsp/include -I engine/numerics/hesap-fft/include -I engine/numerics/hesap-dense/include \
  -I engine/numerics/hesap/include -I engine/foundation/core/include -I engine/foundation/containers/include -I engine/foundation/memory/include \
  -I engine/foundation/log/include -I engine/foundation/vm/include -I engine/foundation/math/include -I engine/foundation/units/include \
  -I engine/foundation/jobs/include -I engine/numerics/hesap-sched/include \
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
