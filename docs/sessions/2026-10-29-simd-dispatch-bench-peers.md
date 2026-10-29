# 2026-10-29 — SIMD-DISPATCH: the vs-reference boards' Eigen peer in its own library (dense part)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md).

## CI first

Push run 40748658864 (f7304724) is still in progress (win-asan); its 15 failed jobs are the ones the last note traced
to causes fixed at HEAD. Every other run of the last 15 was cancelled by the concurrency group before a job started,
so no Needs CI row has new hosted evidence (DIAG.11c and DIAG.12 still wait for a complete-tier run).

## Why

Since the baseline flip (49e4f622) every vs-reference bench compiled Eigen inside its own unit, which takes
crd-simd-flags: the Eigen peer ran its SSE2 path while Cerid's kernels ran avx2. Building that unit at the host's ISA
instead would compile Cerid's header code above the baseline and share it with the libraries.

## What landed

- **`crd_add_bench_peer`** (runtime/CMakeLists.txt): a peer built from source is a SHARED library of its own, hidden
  symbols but for its header (`examples/peers/peer_api.hpp`), no link-time IL, no Cerid library (only the warning
  flags), `/bigobj` on MSVC, and `CRD_ISA_ROLE peer`, which the ISA isolation manifest leaves out; OpenBLAS's targets
  get the role too.
- **`crd_peer_isa_flags`** (cmake/CrdSimd.cmake): `-march=native` or `-mcpu=native` on GCC and Clang,
  `/clang:-march=native` on clang-cl, and on cl the `/arch` of the level a configure-time run of `cpu_features.cpp`
  reports (`examples/peers/peer_cpu_level.cpp`, cached as `CRD_PEER_CPU_LEVEL`, required when cross-compiling). Here:
  `/arch:AVX2`.
- **`crd-bench-eigen-peer`** (`eigen_peer.hpp`, `eigen_peer_dense.cpp`): handles that own Eigen's aligned storage,
  filled outside the timed region, and one exported call per measured Eigen expression; `isa()` prints the version,
  the vector paths Eigen compiled and the flags (`Eigen 3.4.0, AVX2, FMA (/arch:AVX2)`: Eigen's own
  `SimdInstructionSetsInUse()` says "AVX" for an AVX2 build). A compile-time check fails a build whose ISA is AVX2 or
  AVX-512 when Eigen did not take that path.
- **Converted**: gemm (row-major operands read as their column-major transposes, the kernel Eigen's row-major product
  runs), blas1, blas2, solvers, eig, svd, lstsq, eig_nonsym and hss. Their units no longer see Eigen, take the tree's
  warnings and PCH, and are tidy-clean (the LAPACK declarations in a NOLINT block, the timing loops take `const Op&`).
  The LAPACK copies read Cerid-side buffers or one hoisted pointer, never a peer call per element.
  bench_hesap_matrix_resource_vs_reference, which has no peer, is a plain bench.
- `CRD_HESAP_MATRIX_DIR` now also names the vs-reference corpus, so a tree reuses another's matrices.
- The liquid-dsp drivers build their peer at `-march=native` (was `-mavx2 -mfma`); the stale "Force TARGET=SKYLAKEX"
  OpenBLAS comment is gone.
- ADR-0134 §8 amended: one peer number per workload, never lowered by `CRD_CPU_LEVEL`; on MSVC the board takes it from
  the detected-level arm, because an applied level's UCRT FMA3 switch is process-wide.

## Checks

win-debug configured with `CRD_BUILD_HESAP_VS_REFERENCE=ON` from the pinned Eigen and OpenBLAS sources and the matrix
corpus already on disk (`CPM_eigen_SOURCE`, `CPM_OpenBLAS_SOURCE`, `CRD_HESAP_MATRIX_DIR`; nothing downloaded): the
peer and the ten benches build at /W4 /WX, tidy-files.py is clean on all thirteen files, and blas1 prints the peer line
above. No boards (the fast-row order).

## Left

The sparse, direct and iterative Eigen benches (23 programs; transitional `crd_add_hesap_vs_ref_bench` without
`EIGEN_PEER` keeps them building meanwhile), the pocketfft peer of the FFT and DCT boards, the Boost peers of the
special-function boards, and a repository check that no bench unit outside `examples/peers/` includes a peer header
library.
