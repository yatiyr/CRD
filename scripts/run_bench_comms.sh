#!/bin/bash
# v11c: build + run the comms throughput bench (Cerid f64) then the liquid-dsp peer (f32). WSL, single-threaded.
set -e
cd /mnt/d/Dev/cerid
B=$HOME/cerid-build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DNDEBUG \
  -I "$B/core/include" \
  -I engine/numerics/hesap-comms/include -I engine/numerics/hesap-dsp/include -I engine/numerics/hesap-fft/include -I engine/numerics/hesap-dense/include \
  -I engine/numerics/hesap-stats/include -I engine/numerics/hesap/include -I engine/foundation/core/include -I engine/foundation/containers/include \
  -I engine/foundation/memory/include -I engine/foundation/log/include -I engine/foundation/vm/include -I engine/foundation/math/include -I engine/foundation/units/include \
  runtime/examples/bench_comms_vs_refs.cpp \
  -Wl,--start-group \
    "$B/hesap-comms/libcrd-hesap-comms.a" "$B/hesap-dsp/libcrd-hesap-dsp.a" "$B/hesap-fft/libcrd-hesap-fft.a" \
    "$B/hesap-dense/libcrd-hesap-dense.a" "$B/hesap-stats/libcrd-hesap-stats.a" "$B/hesap/libcrd-hesap.a" \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group -lpthread -lm \
  -o /tmp/bench_comms
echo "BUILD OK (cerid)"
/tmp/bench_comms
echo ""
echo "--- liquid-dsp peer (f32) ---"
gcc -O3 -mavx2 -mfma -D_GNU_SOURCE runtime/examples/bench_comms_liquid.c -lliquid -lm -o /tmp/bench_comms_liquid 2>/tmp/liquid_comms_err.log \
  && /tmp/bench_comms_liquid || { echo "(liquid build failed — see /tmp/liquid_comms_err.log)"; head -20 /tmp/liquid_comms_err.log; }
