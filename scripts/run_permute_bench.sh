#!/bin/bash
set -e
export CMAKE_BUILD_PARALLEL_LEVEL=16
cd /mnt/d/Dev/cerid
g++ -O3 -march=native -std=c++20 -DNDEBUG -DCRD_SIMD_TARGET=2 \
    -I engine/numerics/hesap-tensor/include -I engine/numerics/hesap-stats/include -I engine/foundation/core/include \
    -I engine/foundation/containers/include -I engine/foundation/memory/include -I engine/foundation/log/include \
    -I engine/foundation/math/include -I engine/foundation/units/include -I engine/foundation/jobs/include \
    -I "$HOME/cerid-build/linux-gcc-release/engine/core/include" \
    scripts/bench_permute.cpp \
    "$HOME/cerid-build/linux-gcc-release/engine/jobs/libcrd-jobs.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/containers/libcrd-containers.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/memory/libcrd-memory.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/vm/libcrd-vm.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/log/libcrd-log.a" \
    "$HOME/cerid-build/linux-gcc-release/engine/core/libcrd-core.a" \
    -o build/bench_permute_bin
taskset -c 4 ./build/bench_permute_bin
taskset -c 0,2,4,6,8,10,12,14 ./build/bench_permute_bin mt
taskset -c 0-15 ./build/bench_permute_bin mt16
