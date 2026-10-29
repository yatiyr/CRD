#!/bin/bash
# The FFT boards: complex forward FFT throughput of Cerid against FFTW (FFTW_MEASURE), pocketfft (its peer library at
# the host's ISA) and Intel MKL on identical data (bench_fft_vs_refs, f64) and the f32 forward against MKL
# (bench_fft_f32), one thread, at each CPU level (scripts/run-lane-bench.sh: CRD_BENCH_LEVELS, CRD_BENCH_CPUS). The
# peers are installed on the WSL host (CRD_BUILD_HESAP_VS_FFT_REFS in runtime/CMakeLists.txt).
#   bash scripts/run_bench_fft.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
for bench in bench_fft_vs_refs bench_fft_f32; do
    bash scripts/run-lane-bench.sh "$bench" -DCRD_BUILD_HESAP_VS_FFT_REFS=ON
done
