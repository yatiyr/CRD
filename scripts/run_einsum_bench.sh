#!/bin/bash
set -e
export CMAKE_BUILD_PARALLEL_LEVEL=16
cd /mnt/d/Dev/cerid
cmake --build "$HOME/cerid-build/linux-gcc-release" --target crd-hesap-tensor-einsum-tests > /tmp/b.log 2>&1 || { grep -m3 error /tmp/b.log; exit 1; }
"$HOME/cerid-build/linux-gcc-release/tests/hesap-tensor/crd-hesap-tensor-einsum-tests" > /tmp/t.log 2>&1
RC=$?; grep -m1 -o "All tests passed.*" /tmp/t.log; echo "gcc_exec_rc=$RC"; [ $RC -ne 0 ] && exit 1
B=$HOME/cerid-build/linux-gcc-release
g++ -O3 -march=native -std=c++20 -DNDEBUG -DCRD_SIMD_TARGET=2 \
    -I engine/hesap-tensor/include -I engine/hesap-dense/include -I engine/hesap/include -I engine/hesap-stats/include \
    -I engine/core/include -I engine/containers/include -I engine/memory/include -I engine/log/include \
    -I engine/math/include -I engine/units/include -I engine/jobs/include \
    -I "$B/engine/core/include" \
    scripts/bench_einsum.cpp \
    "$B/engine/hesap-dense/libcrd-hesap-dense.a" \
    "$B/engine/hesap/libcrd-hesap.a" \
    "$B/engine/hesap-tensor/libcrd-hesap-tensor.a" \
    "$B/engine/jobs/libcrd-jobs.a" \
    "$B/engine/math/libcrd-math.a" \
    "$B/engine/containers/libcrd-containers.a" \
    "$B/engine/memory/libcrd-memory.a" \
    "$B/engine/vm/libcrd-vm.a" \
    "$B/engine/log/libcrd-log.a" \
    "$B/engine/core/libcrd-core.a" \
    -o build/bench_einsum_bin
echo "--- cerid (1T pinned) ---"
taskset -c 4 ./build/bench_einsum_bin
echo "--- peers (1T pinned) ---"
OMP_NUM_THREADS=1 taskset -c 4 python3 scripts/bench_einsum_peers.py
