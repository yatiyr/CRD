#!/bin/bash
set -e
export CMAKE_BUILD_PARALLEL_LEVEL=16
cd /mnt/d/Dev/cerid
# linux-gcc correctness first
cmake --build "$HOME/cerid-build/linux-gcc-release" --target crd-hesap-tensor-tests 2>&1 | tail -1
"$HOME/cerid-build/linux-gcc-release/tests/hesap-tensor/crd-hesap-tensor-tests" --reporter compact | tail -1
echo "gcc_tests_rc=$?"
# probe bench (release flags + AVX2 like the engine; TLSF from the built tree)
g++ -O3 -march=native -std=c++20 -DNDEBUG \
    -I engine/hesap-tensor/include -I engine/hesap-stats/include -I engine/core/include \
    -I engine/math/include \
    -I engine/containers/include -I engine/memory/include -I engine/log/include \
    -I "$HOME/cerid-build/linux-gcc-release/engine/core/include" \
    scripts/bench_dtypes.cpp \
    "$HOME/cerid-build/linux-gcc-release/engine/memory/libcrd-memory.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/vm/libcrd-vm.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/log/libcrd-log.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/core/libcrd-core.a" \
    -o build/bench_dtypes_bin 2>&1 | tail -3
taskset -c 4 ./build/bench_dtypes_bin
echo "--- python peers (same machine, single thread) ---"
OMP_NUM_THREADS=1 taskset -c 4 python3 scripts/bench_dtypes_peers.py
