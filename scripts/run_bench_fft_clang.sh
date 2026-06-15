#!/bin/bash
# Compiler A/B for the small/mid codelet gap: the EXACT representative bench (real crd TLSF allocator + the
# gcc-built crd .a libs + MKL/FFTW/PocketFFT refs) but the bench TU compiled by CLANG++ instead of g++ — so
# only Cerid's fft.hpp template codegen changes. clang/gcc are ABI-compatible on Linux ⇒ the .a libs link
# cleanly. If clang's Cerid GFLOPS >> g++'s at 8K-64K, the codelet SOURCE is fine and gcc's scheduler is the
# gap (a lever the prior 8 gcc/MSVC dead-ends never tried; KFR: "requires Clang for top performance").
set -e
cd /mnt/d/Dev/cerid
# The gcc .a libs are LTO bytecode (clang's linker can't consume them), so compile the crd sources WITH clang
# directly into the bench ⇒ the WHOLE binary (incl. the real TlsfAllocator) is clang codegen, single toolchain.
SRCS="$(find engine/memory/src engine/core/src engine/log/src engine/containers/src engine/vm/src -name '*.cpp')"
clang++ -O3 -std=c++20 -mavx2 -mfma -DCRD_SIMD_TARGET=2 -DCRD_DETERMINISTIC_FP=1 \
  -I build/linux-gcc-release/engine/core/include \
  -I engine/hesap-fft/include -I engine/hesap/include -I engine/core/include \
  -I engine/containers/include -I engine/memory/include -I engine/log/include -I engine/vm/include \
  -I engine/math/include -I "$HOME/fft_refs" -I /usr/include/mkl \
  runtime/examples/bench_fft_vs_refs.cpp $SRCS \
  -lfftw3 -lmkl_rt -lpthread \
  -o /tmp/bench_fft_clang
echo "BUILD OK (clang++ $(clang++ --version | head -1))"
MKL_NUM_THREADS=1 OMP_NUM_THREADS=1 /tmp/bench_fft_clang
