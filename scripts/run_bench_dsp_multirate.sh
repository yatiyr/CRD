#!/bin/bash
# v11-k: build + run the resample_poly throughput bench (Cerid) then scipy + liquid-dsp columns. MATLAB runs on
# Windows (tests/numerics/hesap-dsp/bench_multirate_matlab.m). WSL, single-threaded, AVX2+FMA, N=1M, up/down = 3/2 + 2/3.
set -e
cd /mnt/d/Dev/cerid
B=$HOME/cerid-build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DNDEBUG \
  -I "$B/core/include" \
  -I engine/numerics/hesap-dsp/include -I engine/numerics/hesap-fft/include -I engine/numerics/hesap-dense/include \
  -I engine/numerics/hesap/include -I engine/foundation/core/include -I engine/foundation/containers/include -I engine/foundation/memory/include \
  -I engine/foundation/log/include -I engine/foundation/vm/include -I engine/foundation/math/include -I engine/foundation/units/include \
  runtime/examples/bench_dsp_multirate_vs_refs.cpp \
  -Wl,--start-group \
    "$B/hesap-dsp/libcrd-hesap-dsp.a" "$B/hesap-fft/libcrd-hesap-fft.a" "$B/hesap-dense/libcrd-hesap-dense.a" \
    "$B/hesap/libcrd-hesap.a" "$B/hesap-sched/libcrd-hesap-sched.a" "$B/jobs/libcrd-jobs.a" \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group -lpthread -lm \
  -o /tmp/bench_dsp_multirate
echo "BUILD OK"
/tmp/bench_dsp_multirate
echo ""
echo "--- scipy reference ---"
python3 runtime/examples/bench_dsp_multirate_refs.py
echo ""
echo "--- liquid-dsp rresamp ---"
if [ -f runtime/examples/bench_dsp_multirate_liquid.c ]; then
  gcc -O3 -mavx2 -mfma runtime/examples/bench_dsp_multirate_liquid.c -lliquid -lm -o /tmp/bench_liquid_mr 2>/tmp/liquid_mr_err.log \
    && /tmp/bench_liquid_mr || { echo "(liquid build failed — see /tmp/liquid_mr_err.log)"; head -5 /tmp/liquid_mr_err.log; }
fi
