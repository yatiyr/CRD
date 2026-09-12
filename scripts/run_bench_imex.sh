#!/bin/bash
# v9-z: build + run the IMEX-vs-ARKODE work-precision crush bench (WSL). See the bench file header.
set -e
cd /mnt/d/Dev/cerid
B=build/linux-gcc-release/engine
g++ -O2 -std=c++20 \
  -I build/linux-gcc-release/engine/core/include \
  -I engine/numerics/hesap-ode/include -I engine/numerics/hesap/include -I engine/numerics/hesap-dense/include \
  -I engine/numerics/hesap-sparse/include -I engine/numerics/hesap-direct/include -I engine/numerics/hesap-iterative/include \
  -I engine/foundation/core/include -I engine/foundation/containers/include -I engine/foundation/memory/include \
  -I engine/foundation/log/include -I engine/foundation/vm/include -I engine/foundation/math/include \
  runtime/examples/bench_ode_imex_vs_arkode.cpp \
  -Wl,--start-group \
    "$B/hesap-dense/libcrd-hesap-dense.a" "$B/hesap-sparse/libcrd-hesap-sparse.a" \
    "$B/hesap/libcrd-hesap.a" "$B/math/libcrd-math.a" "$B/memory/libcrd-memory.a" \
    "$B/vm/libcrd-vm.a" "$B/log/libcrd-log.a" "$B/core/libcrd-core.a" \
    "$B/containers/libcrd-containers.a" "$B/jobs/libcrd-jobs.a" \
  -Wl,--end-group \
  -lsundials_arkode -lsundials_sunmatrixdense -lsundials_sunlinsoldense -lsundials_nvecserial -lsundials_generic \
  -o /tmp/bench_imex
echo "BUILD OK"
/tmp/bench_imex
