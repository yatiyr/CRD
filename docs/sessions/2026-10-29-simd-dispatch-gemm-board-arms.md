# 2026-10-29 — SIMD-DISPATCH: the one-thread GEMM board's arms

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 2, one-thread GEMM where the peers lead).

## CI first

- Scheduled run 40849112570 (fea7ee0a): win-asan, linux-gcc-asan and linux-gcc-arm64-asan are still running. Every
  completed failure was traced in earlier batches and is fixed at e496d16c or before. Nothing was cancelled.
- Push run 40869334591 (e496d16c): 12 jobs are green and none has failed. Green: both Repository checks,
  linux-gcc-shipping, win-debug-scalar, win-release, linux-gcc-release, linux-gcc-public-checks, win-vs, and the three
  ARM64 debug lanes. Still running: win-tidy, win-shipping, win-shipping-profile, win-clang-cl-shipping,
  win-public-checks, win-asan, win-relwithdebinfo, win-debug, win-clang-cl, linux-clang-tsan,
  linux-gcc-debug-scalar, linux-gcc-asan, linux-gcc-relwithdebinfo, linux-gcc-debug and the two ARM64 ASan lanes.
- The push runs from 27c15027 to b94e83e9 were replaced before any job started (0 jobs each).
- ARM64 first green run: linux-gcc-arm64-debug (2479 of 2479), linux-clang-arm64-debug (2479 of 2479) and
  win-arm64-debug (2480 of 2480) passed every test at e496d16c, which comes after 74a95125. That includes the forced
  portable and neon arms. The clause stays open on the two ARM64 ASan lanes.
- The ISA guard passed on every lane that finished. Its times: 32.3 s (linux-gcc-arm64-debug), 30.4 s
  (linux-clang-arm64-debug), 22.9 s (win-arm64-debug), 149 s (linux-gcc-public-checks), 155 s (linux-gcc-release),
  177 s (win-debug-scalar), 319 s (linux-gcc-shipping) and 446 s (win-release). The CTest timeout is 1800 s.
- DIAG.11c and DIAG.12 stay Needs CI: there is no green complete-tier run yet.

## What landed

- **GemmKernels** (`crd/hesap/dense/dense_kernels.hpp`) now holds a GEMM's tile shape, block and both packers.
  - It is a member of each level's `DenseTypeKernels`.
  - The complex types get their own GEMM kernels in general code (`kComplexGemmKernels`, `detail/gemm_pack.hpp`). A
    `GemmSource` carries `conj` for their ConjTranspose.
  - So `pack_a`, `pack_b` and `gemm_packed_inner` run the kernels they are given, with no branch per type.
  - `gemm_shape_of` and `run_gemm_block` are gone. gemm, gemm_parallel, the small path, syrk and the supernodal walks
    each read the table once per call.
  - The strided packers live once in `gemm_block.hpp` (`pack_a_source`, `pack_b_source`), shared by every level and
    by the complex types.
- **The 4 by 4 arm.** The portable unit alone also defines `kDenseGemmArmsPortable`: PortableTile 4 by 4 for f64 and
  f32, two roundings. It gives the same bits as the 2 by 4 tile.
- **Forced paths.** `detail::gemm_arm` (`detail/gemm_arms.hpp`) runs a serial GEMM on given kernels down
  `GemmPath::Auto`, `Resident` or `Packed`.
  - A forced Resident asserts `gemm_resident_admits`: op(A) row-major and the B budget. That rule now lives in
    `gemm_pack.hpp`.
  - blas3's single 200M threshold is split into `kSerialResidentThreshold` and `kParallelResidentThreshold`. Both are
    still 200M until the board sets the serial one.
  - `GemmPath`, `gemm_arm` and the arm table are board scaffolding, deleted at the keep-or-drop step.
- **The board** (`bench_hesap_gemm_vs_reference --one-thread`, run by the new `scripts/run_bench_gemm.sh`):
  - The one-thread shootout also runs at 600, 768 and 1000. The resident threshold falls near the 594 cube, and
    1000 and 1024 hit the 4 KiB alias of A's rows.
  - An arms table times every arm in rounds, one after another. It prints n/a where Resident does not admit the
    product, and the 4 by 4 rows only at the portable level.
  - Every arm must give the Auto arm's bits, and every peer must lie within 1e-3 of Cerid. A miss now fails the run
    (exit 1); before, it was only printed.

## Checks

- Built crd-hesap-dense-tests and crd-hesap-direct on win-debug.
- The `[kernels]` cases pass at the detected level and with forced portable and sse2. They add the 4 by 4 arm's block
  and packer cases, gemm_arm on both tiles down every path, a new empty-product case and the skinny product's
  admit check.
- The whole crd-hesap-dense-tests program passes (372 cases).
- Strict tidy is clean on the nine changed engine and test files. Format was applied to the added lines only.
- The ctest guards pass (19), as do Allman braces, the plan, repository and tooling tests.
- The bench cannot be configured here: its Eigen and OpenBLAS pins are not among the local archives, and the
  configure would fetch them. It was compiled objects-only on WSL GCC 13 at the baseline with the tree's
  `-Werror` warnings. That compile caught a GCC 13 `-Wdangling-reference` false positive on a reference-returning
  lambda; the fix uses a pointer.

## Left

- The board itself runs in the row-close pass. It needs the eigen and OpenBLAS archives from cmake/pins.json, a
  download that is asked for at that time.
