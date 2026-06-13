#!/bin/bash
# v9-z: build + run the matrix-free Krylov vs CVODE-SPGMR work-precision bench (WSL).
set -e
cd /mnt/d/Dev/cerid
B=build/linux-gcc-release/engine
g++ -O2 -std=c++20 \
  -I build/linux-gcc-release/engine/core/include \
  -I engine/hesap-ode/include -I engine/hesap/include -I engine/hesap-dense/include \
  -I engine/hesap-sparse/include -I engine/hesap-direct/include -I engine/hesap-iterative/include \
  -I engine/core/include -I engine/containers/include -I engine/memory/include \
  -I engine/log/include -I engine/vm/include -I engine/math/include \
  runtime/examples/bench_ode_krylov_vs_cvode_spgmr.cpp \
  -Wl,--start-group \
    "$B/hesap-dense/libcrd-hesap-dense.a" "$B/hesap-sparse/libcrd-hesap-sparse.a" \
    "$B/hesap/libcrd-hesap.a" "$B/math/libcrd-math.a" "$B/memory/libcrd-memory.a" \
    "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" "$B/core/libcrd-core.a" \
    "$B/containers/libcrd-containers.a" "$B/jobs/libcrd-jobs.a" \
  -Wl,--end-group \
  -lsundials_cvode -lsundials_sunlinsolspgmr -lsundials_nvecserial -lsundials_generic \
  -o /tmp/bench_krylov
echo "BUILD OK"
/tmp/bench_krylov
