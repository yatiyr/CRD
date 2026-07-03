#!/bin/bash
set -e
export CMAKE_BUILD_PARALLEL_LEVEL=16
cd /mnt/d/Dev/cerid
cmake --build "$HOME/cerid-build/linux-gcc-release" --target crd-hesap-tensor-tests > /tmp/b.log 2>&1 || { grep -m3 error /tmp/b.log; exit 1; }
"$HOME/cerid-build/linux-gcc-release/tests/hesap-tensor/crd-hesap-tensor-tests" "[reduce]" > /tmp/t.log 2>&1
RC=$?; grep -m1 -o "All tests passed.*" /tmp/t.log; echo "gcc_reduce_rc=$RC"; [ $RC -ne 0 ] && exit 1
g++ -O3 -march=native -std=c++20 -DNDEBUG -DCRD_SIMD_TARGET=2 \
    -I engine/hesap-tensor/include -I engine/hesap-stats/include -I engine/core/include \
    -I engine/containers/include -I engine/memory/include -I engine/log/include \
    -I engine/math/include -I engine/units/include -I engine/jobs/include \
    -I "$HOME/cerid-build/linux-gcc-release/engine/core/include" \
    scripts/bench_reduce.cpp \
    "$HOME/cerid-build/linux-gcc-release/engine/hesap-tensor/libcrd-hesap-tensor.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/jobs/libcrd-jobs.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/containers/libcrd-containers.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/math/libcrd-math.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/memory/libcrd-memory.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/vm/libcrd-vm.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/log/libcrd-log.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/core/libcrd-core.a" \
    -o build/bench_reduce_bin
taskset -c 4 ./build/bench_reduce_bin
