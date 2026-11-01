#!/bin/bash
# The GPU FFT board's VkFFT peer (docs/bench/2026-07-13-gpu-fft-cufft-gold.md): VkFFT's batched C2C forward f32 FFT at
# N = 256 to 4096 over about 16.7M complex elements, 20 appends per submit, the best of 20 submits, on the first Vulkan
# device (runtime/examples/bench_gpu_fft_vkfft.cpp, CRD_BUILD_GPU_VS_VKFFT), built in the linux-gcc-release lane tree
# of the WSL source mirror and run once (a GPU board: the CPU level does not change its kernels). Cerid's rows come from
# the hidden [.fft-bench] case of crd-gpu-context-vulkan-tests on the same device.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
CRD_BENCH_LEVELS=detected bash scripts/run-lane-bench.sh bench_gpu_fft_vkfft -DCRD_BUILD_GPU_VS_VKFFT=ON
