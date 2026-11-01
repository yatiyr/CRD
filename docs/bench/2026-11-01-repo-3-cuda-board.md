# REPO.3: the CUDA board on the measuring PC's GPU (2026-11-01)

<!-- doc-role: historical -->
> Captured board. Live owner: [REPO.3](../ROADMAP.md#slice-repo.3). Session:
> [2026-11-01-repo-3-cuda-board](../sessions/2026-11-01-repo-3-cuda-board.md).

**Verdict:** the `CRD_BUILD_GPU_VS_CUDA` route builds and runs all 17 CUDA programs on an NVIDIA GPU, every judge
passing, exit 0. The ratios repeat the July boards: the fused MLP 2.42x cuBLASLt forward and 1.94x backward, flash
attention 4.05x the unfused peer at S = 4096, the CKIR onesweep sort 0.735x CUB (a loss, unchanged since the
[sort campaign's close](2026-07-13-gpu-compute-primitives.md#final-inch-grind-2026-07-14--the-measured-close-of-the-sort-campaign)).

## Machine and build

- NVIDIA GeForce RTX 4070 Ti SUPER (`sm_89`, L2 48 MB, 256-bit bus) on the development PC, under WSL2 (Ubuntu
  24.04), whose driver exposes the GPU to Linux programs.
- The pinned CUDA Toolkit 13.3.1 (`cmake/pins.json` `cuda-linux-x64`, `scripts/install-cuda.py`; nvcc reports
  13.3.73), host compiler GCC 13.3.0, `linux-gcc-release` with the tree's warnings (nvcc's as errors), 8 jobs, the
  pinned sccache.
- Source: a native WSL copy of the source mirror at e834f58d, clean in `bench/`, with
  `bench/gpu-compute/ckir_onesweep_bench.cu` as committed in 525c8417 (the copy also held track A's three untracked
  state-hash files of `engine/foundation/perf` and `tests/foundation/perf`, which no CUDA program compiles). Nothing
  between e834f58d and 525c8417's successors changes a CUDA compile: the later `crd-warnings` additions are C-only
  or MSVC-only.
- Harness: [run_bench_gpu_cuda.sh](../../scripts/run_bench_gpu_cuda.sh) (`bash scripts/run_bench_gpu_cuda.sh`, every
  program once on the first CUDA device, the first failure stopping the board), driven by the operator's one-off
  wrappers (`CRD_LANE_ROOT=~/cerid-gpubench-build`, `CRD_LANE_JOBS=8`). The second run rebuilt only the onesweep
  program.
- Not recorded: the host's load (`python scripts/build-bench.py host`) before and after; the board is not rerun for it
  (track B builds nothing on that PC). The CPU level line does not apply: the kernels are the GPU's, and no program
  dispatches on the CPU level.

## The first run (e834f58d)

The first 2 programs passed; `ckir_onesweep_bench` printed its three sorts (each sorted and a permutation) and the
`clear_b`, `ghist_b` and `gbase` kernel-profile rows, then stopped at the profile's `clr+sb0` row with
`cuda_bench.hpp:106: cudaDeviceSynchronize() failed: an illegal memory access was encountered (700)`, exit 1. The
profile timed `k_ghist_b` over 503 calls into histograms nothing cleared, so `k_gbase` (timed the same way) wrote
bases past the keys and the timed `k_clear` + `k_sb0` scatter wrote out of bounds. The sequence is the July
program's; it surfaced once 3fd30fe7 checked every launch and synchronisation. No `clr+sb0` figure was ever quoted
from a board. 525c8417 builds the histograms and the bases once from a cleared state before the timed scatter;
`k_sb0` reads its bases from `gb` and only the tickets and look-back cells from the auxiliary buffer that the timed
`k_clear` resets (`ckir_onesweep_gen.cu`), so each timed scatter runs on valid bases.

## The board (second run, exit 0)

### GPU compute

| Program | Peer | Cerid side | Ratio | Judge |
|---|--:|--:|--:|---|
| `ckir_mlp_bench` (64 x 6 layers, batch 1,048,576, fp16) | cuBLASLt 2.845 ms, 18.1 TF | CKIR fused 1.174 ms, 43.9 TF | **2.42x** | max abs diff 0; 0 of 67,108,864 halves differ run to run |
| `mlp_fused_bench` (same) | cuBLASLt 2.850 ms, 18.1 TF | fused 1.178 ms, 43.8 TF | **2.42x** | max abs diff 0 |
| `ckir_mlp_bwd_bench` (backward) | cuBLASLt 9.812 ms, 10.5 TF | CKIR fused 5.055 ms, 20.4 TF | **1.94x** | dW rel 5e-5 (cuBLAS 2e-4) |
| `mlp_backward_bench` (backward) | cuBLASLt 10.238 ms, 10.1 TF | fused 5.289 ms, 19.5 TF | **1.94x** | dW rel 5e-5 (cuBLAS 2e-4) |
| `ckir_onesweep_bench` A (2,048 per block), 16M u32 keys | CUB 1.0996 ms, 15,258 Mk/s | 1.733 ms, 9,680 Mk/s | 0.63x | sorted, a permutation |
| `ckir_onesweep_bench` B (4,096) | CUB 1.0996 ms | 1.495 ms, 11,222 Mk/s | **0.735x** | sorted, a permutation |
| `ckir_onesweep_bench` C (8,192) | CUB 1.0996 ms | 1.609 ms, 10,425 Mk/s | 0.68x | sorted, a permutation |

The backward breakdown (cuBLAS mask, dW and dA alone): 4.40 + 2.78 + 2.86 ms (`ckir_mlp_bwd_bench`) and 4.43 + 2.78 +
2.88 ms (`mlp_backward_bench`). The onesweep B kernel profile: `clear_b` 0.0115 ms, `ghist_b` 0.1191 ms, `gbase`
0.0045 ms, `clr+sb0` 0.3492 ms.

`flash_attention_bench` (D = 64, f32), unfused / flash:

| S | flash ms | unfused ms | Ratio | flash err | unfused err |
|--:|--:|--:|--:|--:|--:|
| 512 | 0.1382 | 0.0785 | 0.57x | 5.4e-9 | 5.1e-9 |
| 1024 | 0.2643 | 0.2775 | 1.05x | 5.8e-9 | 6.1e-9 |
| 2048 | 0.5255 | 1.0588 | 2.02x | 7.2e-9 | 7.5e-9 |
| 4096 | 1.1151 | 4.5166 | **4.05x** | 9.3e-9 | 1.0e-8 |

The vendor primitives alone (each judged exactly):

| Program | N | ms | Rate |
|---|--:|--:|--:|
| `cub_radixsort_bench` (u32 keys) | 1,048,576 / 4,194,304 / 16,777,216 | 0.0789 / 0.1828 / 1.0788 | 13,290 / 22,948 / 15,551 Mk/s |
| `cub_reduce_bench` (f32 sum, exact) | 4,194,304 / 16,777,216 | 0.0145 / 0.1165 | 1,158.9 / 575.9 GB/s |
| `cub_scan_bench` (f32 inclusive, exact last) | 4,194,304 / 16,777,216 | 0.0205 / 0.2386 | 1,635.5 / 562.5 GB/s |

### GPU FFT (cuFFT, the peer)

`cufft_bench` (C2C forward f32, about 16.8M complex elements per row):

| N | Batch | ms | GFLOP/s |
|--:|--:|--:|--:|
| 256 | 65,536 | 0.4230 | 1,586.3 |
| 512 | 32,768 | 0.4413 | 1,710.6 |
| 1,024 | 16,384 | 0.4413 | 1,900.7 |
| 2,048 | 8,192 | 0.4389 | 2,102.4 |
| 4,096 | 4,096 | 0.4321 | 2,329.5 |
| 8,192 | 2,048 | 0.4270 | 2,553.9 |
| 16,384 | 1,024 | 0.4453 | 2,637.5 |
| 65,536 | 256 | 0.8765 | 1,531.2 |
| 262,144 | 64 | 0.8827 | 1,710.6 |

`cufft_conv_bench` (forward, multiply, inverse; the same rows): 1.5247, 1.5299, 1.5336, 1.5208 and 1.5247 ms for N =
256 to 4,096. `cufft_2d_conv_bench` (per image): 0.0152, 0.0233, 0.0553 and 0.2962 ms for 256² to 2,048².

Batched, whole-batch ms (per image in parentheses):

| B | 2-D C2C inverse 256² | 2-D C2C inverse 512² | 2-D conv 1,024² | 2-D real conv 1,024² |
|--:|--:|--:|--:|--:|
| 1 | | | 0.0553 (0.0553) | 0.0417 (0.0417) |
| 4 | 0.0092 (0.0023) | 0.0205 (0.0051) | 0.1750 (0.0438) | 0.1188 (0.0297) |
| 8 | 0.0132 (0.0017) | 0.0344 (0.0043) | 1.1526 (0.1441) | 0.2908 (0.0364) |
| 16 | 0.0193 (0.0012) | 0.0635 (0.0040) | 2.2774 (0.1423) | 1.1162 (0.0698) |
| 32 | 0.0328 (0.0010) | 0.4332 (0.0135) | 4.5793 (0.1431) | 2.2282 (0.0696) |
| 64 | 0.0614 (0.0010) | 0.8748 (0.0137) | | |

`ckir_fft_profilee` and `cufft_1024_profilee` are Nsight Compute targets: each ran its kernel and printed `done`.

## Against the recorded boards

| Row | July board | This board |
|---|--:|--:|
| Fused MLP forward vs cuBLASLt | 2.37x to 2.41x ([MLP gold](2026-07-14-fused-mlp-cublas-gold.md)) | 2.42x |
| Fused MLP backward vs cuBLASLt | 1.90x to 1.94x (same) | 1.94x |
| Flash attention, S = 2,048 / 4,096 | 2.02x / 3.93x ([AS-4](2026-07-23-as4-flash-attention-fusion.md)) | 2.02x / 4.05x |
| CKIR onesweep B vs CUB | 1.415 ms, 0.735x ([sort close](2026-07-13-gpu-compute-primitives.md)) | 1.495 ms, 0.735x |
| cuFFT C2C, N = 2,048 / 4,096 / 8,192 | 2,115 / 2,297 / 2,506 GFLOP/s ([cuFFT gold](2026-07-13-gpu-fft-cufft-gold.md)) | 2,102 / 2,330 / 2,554 |

The onesweep sort is a loss to CUB at every configuration, as the sort campaign closed it (its remaining gap
itemized there); this board measures it and does not change it. Flash attention at S = 512 loses to the unfused
peer, as on the AS-4 board, whose autotuner note names the per-S tile choice.
