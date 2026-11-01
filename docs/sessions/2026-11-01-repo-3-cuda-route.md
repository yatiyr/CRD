# 2026-11-01 — REPO.3: the GPU boards' CUDA programs routed on the pinned CUDA Toolkit

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [REPO.3](../ROADMAP.md#slice-repo.3), the 17 CUDA programs the
[ggml and VkFFT note](2026-11-01-repo-3-ggml-vkfft-routes.md#the-cuda-programs) left `unrouted`;
[MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0)'s hosted evidence. Built on `cerid-track-b` (spot n2-standard-8, Intel
Xeon, Ubuntu 24.04, GCC 13.3, CMake 4.4.5; no GPU) in a `linux-gcc-release` tree of its own (`~/cerid-cuda-build`). The
machine was preempted once (08:47 UTC) and restarted from the same disk.

## CI first

- **No push run has read this track's commits since 03571fa3.** The push runs at cb0c04ab (41290152491), b64d02a5
  (41290558970) and cc5ee9e9 (41291103424), which contain 2c41d89c..2ec91dd8, were each cancelled when the next push
  superseded them; this batch's push queued 41291759660 at cacd6547, the first to cover them. The dispatch run
  41274380605 (de5e10b0) is still running: win-debug, win-debug-scalar and win-public-checks were in their Test step at
  09:01 UTC.
- **Its finished failures are not this track's**: win-tidy's `test_filesystem.cpp` name (fixed by 4b9bc837), win-clang-cl
  and win-clang-cl-shipping at track A's 128-bit division in `test_instruction_access.cpp` (`__udivti3`, `__divti3`;
  fixed by track A's 7cfed26c, which links clang's builtins library on clang-cl), linux-gcc-release and
  linux-gcc-shipping at `test_multivariate.cpp:125` (`-Wmaybe-uninitialized` under LTO, fixed by 76330344 after
  de5e10b0).
- **MCP-ADAPT.0.** Since de5e10b0 the row's paths changed only by `test_filesystem.cpp`'s rename and the byte order mark
  removed from `test_ceridc.cpp`, so de5e10b0's win-debug and win-debug-scalar results are the row's MSVC and SSE2-floor
  evidence once they finish; clang-cl's tests first run in 41291759660 (7cfed26c is in cacd6547).
- **Found and fixed here, this track's**: `docs/generated/dependency-licenses.md` did not list the ggml and VkFFT pins
  that 2c41d89c added (15 packages rendered where the registry has 17), which the Repository checks jobs'
  `test_this_checkout_passes_every_provenance_guard` would fail on the first run reading 2c41d89c (none ran: all were
  cancelled). Regenerated with this batch's CUDA pin and its `generated-sources.json` digest refreshed.

## The pinned CUDA Toolkit

- **The pin.** `cmake/pins.json` `tools/cuda-linux-x64`: CUDA Toolkit 13.3.1 (2026-06-29), the measuring PC's own
  version (its `version.json`), from NVIDIA's redistributable component archives for linux-x86_64 (`redistrib_13.3.1.json`):
  `cuda_nvcc` 13.3.73, `cuda_crt`, `libnvvm`, `cuda_cudart` 13.3.29, `cccl` 13.3.3.4.1 (CUB), `cuda_nvrtc`,
  `libnvjitlink`, `libcufft` 12.3.0.29 and `libcublas` 13.6.0.2, 1.32 GB in all, each with the manifest's SHA-256 and size
  (each matched on download). Licence: the NVIDIA CUDA Toolkit EULA (no-charge use; the components are not
  redistributed); CCCL's notice is Apache-2.0, libcu++ with LLVM exceptions. A testing SDK, pre-approved (2026-10-30).
- **One prefix from several archives.** `pins.install_tree` takes a pin whose `files` list several archives
  (`pins.tree_parts`) and unpacks each below its own root into one staging tree. A path two archives place must be the
  same directory, link or file in both (`pins.unplaced`); the first install proved the rule: CCCL's `LICENSE` differs from
  the EULA the other eight carry. The pin's `notices` move each archive's `LICENSE` to `licenses/<archive>/`
  (`pins.notices_apart`), and its `links` add `lib64 -> lib` (`pins.link_inside_tree`, refused when it leaves the tree,
  names nothing or would replace a placed path): nvcc's profile links the runtime from `lib64`, where the archives place
  `lib`, and CMake's CUDA compiler check failed on `-lcudadevrt` and `-lcudart_static` without it. The stamp records the
  digests, notices and links, so a changed layout installs again.
- **The installer.** `scripts/install-cuda.py` (Linux x64) installs into `~/.cerid/tools/cuda/13.3.1/linux-x64` and
  prints `CRD_CUDA_ROOT`; `nvcc --version` must report V13.3.73, the pin's compiler component, which stands for the
  toolkit version. `scripts/check-pins.py` lists it among the helpers that carry no digest.
- **Tests** (`test_cuda_installer_unpacks_every_component_archive_into_one_prefix`, Windows and Linux Python): one prefix
  from two archives, an identical shared path accepted, the notices kept apart, the stamp's digests and reuse, a new link
  reinstalling, the three refused links, a clashing path refusing the tree and leaving nothing, a handed-in archive refused
  for a multi-archive pin, and the version mapping.

## One discovery of the host's toolkit

The CUDA_PATH hint (the newest toolkit under the Windows installer's directory when neither `CUDAToolkit_ROOT` nor
`CUDA_PATH` is set) was written three times, in `crd-kir-cuda`, `crd-gpu-context-cuda` and `crd-shader-cook`, and the
kir-cuda tests found the toolkit without it. `cmake/CrdCuda.cmake`'s `crd_find_cuda_toolkit` is the one copy, used by all
four and by the boards' programs; it sorts the versions as numbers (v13.3 after v9.2; a string sort put v9 last).

## The route

- **`CRD_BUILD_GPU_VS_CUDA`** (local only, Linux) adds `bench/CMakeLists.txt`, the one directory that enables the CUDA
  language: the toolkit from `crd_find_cuda_toolkit(REQUIRED)`, its nvcc as the CUDA compiler, the tree's C++ compiler as
  the host compiler, `CMAKE_CUDA_ARCHITECTURES` 89 by default (the measuring PC's RTX 4070 Ti SUPER), C++17 as the boards
  built them, nvcc's default static runtime, and `BUILD_RPATH` to the toolkit's libraries (an implicit link directory of
  the CUDA compiler, which CMake leaves out of the run path, so cuFFT was "not found" at the first run). Each program links
  only what it calls (cuFFT, or cuBLASLt alone: the MLP programs use only cuBLAS's types); `ckir_fft_profilee` keeps
  `--fmad=false` (the CKIR kernels' `precise` semantics). `crd-gpu-vs-cuda` builds all 17, and the tree lists them in
  `<tree>/bench/cuda-programs.txt`.
- **Warnings.** `--Werror all-warnings` for nvcc and the tree's GCC set for the host compiler (`-Wall -Wextra
  -Wconversion -Wshadow -Wdouble-promotion -Wnull-dereference -Wformat=2 -Werror`) but `-Wpedantic`: the host compiler
  reads nvcc's translation, whose GCC-style line directives it reports on every line (about 2,900 at the first build).
  The only finding in the programs was `ckir_mlp_bench.cu`'s float/double comparison (two sites), fixed.
- **`scripts/run_bench_gpu_cuda.sh [--build-only] [program ...]`** installs the pinned toolkit, configures its own tree
  (`CRD_LANE_ROOT`, default `~/cerid-cuda-build`: the toolkit in the cache also builds the engine's CUDA modules) when the
  option, the root or the build file is missing, builds, and runs each program once through `wsl-lane.sh run`; an unknown
  program name exits 2 and the first failing program stops the board. A Windows NVIDIA host runs it under WSL2.
- **Source routes.** The 17 programs are `built_when` `CRD_BUILD_GPU_VS_CUDA`; `scripts/source-routes.json` has no
  `unrouted` entry left.

## The programs

- **One failure path and one timer** (`bench/cuda_bench.hpp`): `CRD_CUDA_CHECK(call)` prints the call, file, line and
  status of a failed CUDA runtime, cuFFT or cuBLASLt call and exits 1; `best_event_ms` (CUDA events) and `best_wall_ms`
  (the steady clock between device synchronisations; the programs used `high_resolution_clock`, which is not steady on
  libstdc++) replace the timing loops each program repeated. `bench/gpu-fft/spectrum_multiply.cuh` holds the
  convolution baselines' complex multiply, which four programs defined separately (elementwise and broadcast).
- **Converted** (every call checked, kernel launches by `cudaGetLastError`, Allman bodies, `std::unique_ptr` buffers):
  the seven cuFFT programs, `ckir_fft_profilee`, the three CUB programs and `flash_attention_bench`. Results are now
  judged, not only printed: CUB's sum and inclusive scan of ones must be exact (n ≤ 2^24), the radix sort's whole output
  must be sorted (it compared the first and last key), and the flash and unfused attention outputs must be within 1e-5
  of the double reference (the board reports about 1e-8). A failed allocation or plan fails the run where the cuFFT
  programs printed and skipped the size.
- **Not yet converted**: `mlp_fused_bench`, `mlp_backward_bench`, `ckir_mlp_bench`, `ckir_mlp_bwd_bench` and
  `ckir_onesweep_bench` check most calls through their own `check`/`checkLt` copies, return 2 or 3 on a mismatch and keep
  one-line bodies; their stale nvcc build lines now name the route.

## Evidence

- **Build.** `bash scripts/run_bench_gpu_cuda.sh --build-only` built all 17 programs warning-free (23 steps), and in the
  same tree `crd-kir-cuda`, `crd-gpu-context-cuda` and `crd-shader-cook` (NVRTC PTX cooking) compiled and archived (199
  steps): the engine's CUDA modules had not been compiled on Linux by any lane before.
- **Routes.** `check-source-routes.py --build ~/cerid-cuda-build/linux-gcc-release`: 1,977 sources, 0 unrouted, PASS.
- **No GPU.** On the GPU-less machine each converted program stops at its first call with the reason, for example
  `cufft_bench.cu:24: cudaGetDevice(&dev) failed: CUDA driver version is insufficient for CUDA runtime version (35)`, exit
  1; `run_bench_gpu_cuda.sh cufft_bench` exits 1 and `run_bench_gpu_cuda.sh nosuch` exits 2. `ldd` shows the static
  runtime and the toolkit's cuBLASLt through the run path.
- **Validators**: check-master-plan, check-repository, check-allman-braces, check-generated and check-pins pass;
  `test-repository-tools.py` passes but `test_generated_target_folders`, which needs Ninja on this Windows shell's PATH.

## Remaining on REPO.3

1. The five programs above: `CRD_CUDA_CHECK` on every call, the shared MLP data, cuBLASLt pipeline and comparison in one
   header (four programs repeat them), exit 1 on a mismatch, Allman bodies.
2. The CKIR generators that write the routed kernels (`[.emit-cuda-sort]`, `[.emit-cuda-mlp]`, `[.emit-cuda-mlp-bwd]`,
   `[.emit-fft-cuda]` in `tests/gpu/kir`) open `D:/Dev/cerid/bench/...` by literal path, and `test_vulkan_context.cpp`
   writes its images to `D:/Dev/cerid/build/`; both to go through `CRD_REPO_DIR`, which both test targets define.
3. `check-allman-braces.py` to hold `.cu` and `.cuh` sources, with the generated kernels named in
   `scripts/generated-sources.json` (their style is the CKIR emitter's).
4. The run on the measuring PC's NVIDIA GPU: `bash scripts/run_bench_gpu_cuda.sh` in WSL2 (this track builds and tests
   only on its machine, which has no GPU).
5. The LunarG Qt line from a cold Windows lane, then the close after [REPO.3d](../ROADMAP.md#slice-repo.3d).
