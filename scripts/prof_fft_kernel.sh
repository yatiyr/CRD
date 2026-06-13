#!/bin/bash
# Profile the isolated Cerid FFT kernel with perf stat (uops/IPC/port-pressure) at an L1-resident size.
set -e
cd /mnt/d/Dev/cerid
B=build/linux-gcc-release/engine
g++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DCRD_DETERMINISTIC_FP=1 -DCRD_FFT_PROFILE \
  -I build/linux-gcc-release/engine/core/include \
  -I engine/hesap-fft/include -I engine/hesap/include -I engine/core/include \
  -I engine/containers/include -I engine/memory/include -I engine/log/include -I engine/vm/include \
  -I engine/math/include \
  runtime/examples/prof_fft_kernel.cpp \
  -Wl,--start-group \
    "$B/memory/libcrd-memory.a" "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" \
    "$B/core/libcrd-core.a" "$B/containers/libcrd-containers.a" \
  -Wl,--end-group -lm -o /tmp/prof_fft
echo "BUILD OK"
LG=${1:-10}
IT=${2:-2000000}
echo "=== plain timing ==="
/tmp/prof_fft "$LG" "$IT"
echo "=== perf stat (IPC + FP + ports + cache) ==="
perf stat -d -d \
  -e cycles,instructions,fp_arith_inst_retired.256b_packed_double \
  -e uops_dispatched.port_0,uops_dispatched.port_1,uops_dispatched.port_5,uops_dispatched.port_6 \
  -e L1-dcache-loads,L1-dcache-load-misses \
  /tmp/prof_fft "$LG" "$IT" 2>&1 | tail -30 || \
perf stat /tmp/prof_fft "$LG" "$IT" 2>&1 | tail -20
