# 2026-11-01 — REPO.3: routes for the ggml and VkFFT peers, and the CUDA programs classified

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [REPO.3](../ROADMAP.md#slice-repo.3), the 22 `unrouted` sources the
[peer routes note](2026-11-01-repo-3-peer-routes.md#remaining-on-repo3) left; [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0)'s
hosted evidence. Two peers get a checked route, three generated kernels are recorded as included, and 17 CUDA
programs remain. Built and run on `cerid-track-b` (spot c4-standard-8, Intel Xeon with AVX-512, Ubuntu 24.04, GCC 13.3)
in the `linux-gcc-debug` tree and the peers tree (`linux-gcc-release` with the retained benches and the peer options).

## CI first

The `workflow_dispatch` run 41274380605 at de5e10b0 is the first GitHub Actions run with jobs for this track's commits
since 03571fa3. Its finished jobs, and the older runs 41269105413 (b0b1b245) and 41262267004 (8a243a34), traced by
failing step, file and commit:

- **win-tidy, this track's.** `tests/foundation/platform/test_filesystem.cpp:134`: `readability-identifier-naming`
  on the local constant `kUnixNs`, which aa91b382 (OWN.1) added; function-local constants are `lower_case`
  (`.clang-tidy`). Renamed `unix_ns`; the optimization track made the same rename first (4b9bc837), so the rebase
  dropped this track's identical commit. The job built with `-k 0` to the end (2,704 of 2,705 steps), and this was its
  only finding.
- **win-clang-cl and win-clang-cl-shipping, track A's.** `crd-perf-tests.exe` does not link: `__udivti3` and
  `__divti3` undefined, referenced from `tests/foundation/perf/test_instruction_access.cpp:515` and `:522`, the 128-bit
  division of the divide-fault oracle that 1f053102 (HW-RESILIENCE.2) added. clang-cl emits compiler-rt builtins for
  `unsigned __int128` division, which MSVC's runtime does not provide. Left to track A, which owns the file and
  edited it twice today.
- **linux-gcc-release, fixed.** `tests/numerics/hesap-stats/test_multivariate.cpp:125` (`-Wmaybe-uninitialized` under
  LTO), fixed on main by 76330344 after de5e10b0.
- **Run 41269105413**: every Windows build stopped at `engine/foundation/platform/src/filesystem.cpp(308)`, `windows.h`'s
  `min` and `max` macros against `std::numeric_limits` (C4003); fixed by 9723ac75, which de5e10b0 contains (win-vs
  passes there). **Run 41262267004**: `test_deflate.cpp:755` (`-Wnull-dereference` under LTO, fixed by 95ec6965), the
  win-tidy findings of `test_arguments.cpp` (track C, a40af1bf) and `crd-ci-orchestrator-e2e` (track A, aa05dd37).
- **MCP-ADAPT.0's lanes.** The ARM64 jobs pass (`win-arm64-debug` 2,632 tests, `linux-gcc-arm64-debug` and
  `linux-clang-arm64-debug` 2,628), but they build no `ceridc`: `arm64-base` sets `CRD_MODULES_WITHOUT=jobs` until
  JOBS-PORT.2 gives crd-jobs an ARM64 fiber context, which omits `ceridc` with its closure (the configure line lists it
  among the omitted modules). So the ARM64 lanes hold no evidence for this row, and the row asks for none: its
  evidence lanes are the Windows ones (MSVC, clang-cl) and the scalar ones. win-debug and win-debug-scalar were still
  testing when this note was written; clang-cl's tests did not run (its build stopped at track A's link failure).

## ggml: the nn board's same-format peer

`scripts/v14m_ggml_q8_bench.c` moved to `runtime/examples/bench_nn_q8_vs_ggml.c`, beside the other peers.

- **The pin.** `cmake/pins.json` `ggml`: ggml 0.28.0 (ggml-org/ggml at facdc44a, the newest release on 2026-11-01; the
  board ran an unversioned llama.cpp master), MIT, read from the archive.
- **The route.** `CRD_BUILD_HESAP_VS_GGML` (Linux, with `CRD_BUILD_RETAINED_BENCHES`; the program reads POSIX's
  monotonic clock as the board did) fetches the archive and builds ggml by its own CMake as an external project, as
  NLopt's route does: static, CPU backend only, with llama.cpp's defaults (`GGML_NATIVE`, OpenMP), its tests and
  examples off. The program builds with the tree's warnings at the host's ISA (`crd_peer_isa_flags`), links ggml's
  three archives with OpenMP, and links as C++ (ggml's CPU backend has C++ objects); it links no Cerid library and is a
  peer to the ISA isolation guard. `scripts/run_bench_nn.sh` builds and runs it after the torch and onnxruntime peers.
- **Run on the track machine** (one thread, core 4): 864.1, 353.9 and 81.5 ns per row for the 64x128, 128x32 and
  32x10 linears at batch 4096, 1,299.5 ns per row in all. This proves the route; the board's i9-14900K rows (471.0 in
  all) are not comparable with a cloud Xeon's, and a board refresh runs Cerid's bench on the same host.

## VkFFT: the GPU FFT board's Vulkan peer

`bench/gpu-fft/vkfft_bench.cpp` moved to `runtime/examples/bench_gpu_fft_vkfft.cpp`.

- **The pin.** `cmake/pins.json` `vkfft`: VkFFT 1.3.4 (DTolm/VkFFT at 066a17c1, the newest release; the board built
  an unversioned clone), MIT. The archive's metal-cpp copy (Apache-2.0) is not used.
- **The route.** `CRD_BUILD_GPU_VS_VKFFT` fetches the archive; VkFFT's benchmark utilities (`utils_VkFFT.cpp`) build
  as `crd-vkfft-utils`, VkFFT's code without the tree's warnings, with the Vulkan backend (`VKFFT_BACKEND=0`, now
  defined once by the build instead of in the program) on the host's Vulkan loader and glslang (its package from
  glslang-dev or the Vulkan SDK). glslang 15's exported targets link `SPIRV-Tools-opt` by name without finding it,
  which left SPIRV-Tools' symbols undefined at the first link; finding `SPIRV-Tools-opt` first resolves the name to
  its imported target. `scripts/run_bench_gpu_fft_vkfft.sh` builds and runs it once.
- **The program, under the tree's warnings.** Its Vulkan structures were brace-initialized with only their `sType`
  (`-Wmissing-field-initializers`), now value-initialized; the timing took whole microseconds into a `long` and
  converted it (`-Wconversion`), now a `double` millisecond duration. Every Vulkan and VkFFT call of the timing loop is
  now checked, and a size whose buffer, plan or submits fail prints why and fails the run (exit 1), where a failed
  submit was timed as a result and a failed plan leaked its buffer.
- **Run on the track machine**, on lavapipe (llvmpipe, LLVM 20.1.2, 256 bits): N = 256 to 4096 over 16.7M complex
  elements, best per-FFT 39.8 to 53.5 ms, 15.7 to 19.2 GFLOP/s, 1 min 43 s in all; with no Vulkan driver
  (`VK_ICD_FILENAMES` naming none) it stops at `createInstance` and exits 1.

## The CUDA programs

Of the 20 CUDA sources under `bench/`, three are CKIR-emitted kernels that one duel program each includes
(`ckir_mlp_fwd_gen.cu`, `ckir_mlp_bwd_gen.cu`, `ckir_onesweep_gen.cu`): `included_by` entries now, as the FFT
profile program's generated kernels already were. The other 17 stay `unrouted`, listed by exact path instead of the
`bench/` directory, so a new file there is no longer covered silently. Each has a board: the cuFFT, cuFFT 2-D and
batched programs in [2026-07-13-gpu-fft-cufft-gold](../bench/2026-07-13-gpu-fft-cufft-gold.md) and
[2026-07-15-fft-ocean-batched-ifft](../bench/2026-07-15-fft-ocean-batched-ifft.md); CUB reduce, scan and radix sort
and the onesweep duel in [2026-07-13-gpu-compute-primitives](../bench/2026-07-13-gpu-compute-primitives.md) (the scan
section names `DeviceScan::InclusiveSum`, not the file); the fused-MLP programs in
[2026-07-14-fused-mlp-cublas-gold](../bench/2026-07-14-fused-mlp-cublas-gold.md); the attention prototype in
[2026-07-23-as4-flash-attention-fusion](../bench/2026-07-23-as4-flash-attention-fusion.md).

Decision: route them, not retire them. They are the vendor bars of live GPU rows, and a retired program cannot rerun
its bar. The route is one option, `CRD_BUILD_GPU_VS_CUDA`: CUDA enabled as a language, the toolkit found with
`find_package(CUDAToolkit)`, the boards' `sm_89` as the default architecture, cuFFT, cuBLASLt and CCCL's CUB from the
toolkit, and the profile program at the boards' `precise` semantics (no contraction). It is compile-checkable on the
track machine once the CUDA toolkit is installed there (pinned, by the repository's installer scripts); running needs
an NVIDIA GPU, which only the measuring PC has (RTX 4070 Ti SUPER, CUDA 13.3, where the boards ran).

## Also fixed

- The root `CMakeLists.txt` checked `CRD_BUILD_HESAP_VS_MKL`'s prerequisite under the HPTT heading; the check has its
  own heading now.

## Evidence

- Peers tree: `bench_nn_q8_vs_ggml` and `bench_gpu_fft_vkfft` build with the tree's warnings as errors and run (above).
- `crd-check-source-routes`: `linux-gcc-debug` (configured fresh) 1,976 sources, 1,805 compiled, 134 gated off, 12
  built outside, 8 included, 17 unrouted, PASS; the peers tree 1,812 compiled, 127 gated off, 8 included, 17
  unrouted, PASS. `crd-check-isa-isolation` on the peers tree passes (the new targets are peers).
- `linux-gcc-debug`: `crd-platform-tests` builds and its `[fs]` cases pass with the rename (7 cases, 50 assertions).
- Local validators: `check-pins.py`, `check-repository.py`, `check-allman-braces.py`, `check-generated.py`,
  `check-master-plan.py`, `check-docs.py`.

## Remaining on REPO.3

- The 17 CUDA programs: `CRD_BUILD_GPU_VS_CUDA` as decided above, compile-checked on the track machine, then run on
  the measuring PC's NVIDIA GPU.
- The LunarG Qt line, from a cold Windows lane.
- Then REPO.3d (scoped configuration and consumer checks, then full remote CI evidence on the published revision once
  DIAG is Done) and REPO.3's close.
