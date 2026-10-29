# 2026-10-29 — SIMD-DISPATCH: the sparse vs-reference benches on the Eigen peer

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md);
follows [the dense part](2026-10-29-simd-dispatch-bench-peers.md).

## CI first

- Push run 40748658864 (f7304724) completed: win-asan passed, every other failed job is one the earlier notes traced
  to causes fixed since. Every Linux job there stopped in the build, so DIAG.11c and DIAG.12 gain no hosted evidence.
- Push run 40784988994 (92db578d, still running): the four Linux ARM64 jobs, their first builds past the fft kernels,
  stopped on `-Werror=unused-function` in `tests/foundation/core/test_diag_fp_environment.cpp`. Its MXCSR helpers were
  defined on every architecture and called only by the x64 cases. Fixed in ccabba8b: they now sit inside the same
  `CRD_ARCH_X64` guard. A scan of every file with an x64 guard for helpers that are defined outside one and used only
  inside one found no other case (the build stops at its first failure, so the ARM64 lanes are still the oracle).

## What landed

- `eigen_peer_objects.hpp` holds the peer's object types and the ISA checks for the peer library's units only;
  `eigen_peer_sparse.cpp` adds the sparse API to `eigen_peer.hpp`. It has a coordinate `Entry`, row-major
  `csr_f32`/`csr_f64` and column-major `csc_f64` built through `setFromTriplets`, row-major dense matrices,
  `spmv`, `spgemm` (not pruned), `spmm`, and the two ways an Eigen user computes an SDDMM: `sampled_dots` and
  `masked_dense_sum`.
- Converted: sparse_assembly (its timed region still builds the triplet list, now inside `csc_f64`), spmv (f32 and
  f64), spgemm, spmm, sddmm, bsr, ell_dia and spgemm_stress. The Matrix-Market readers' `Trip` is the peer's `Entry`,
  so the Eigen side reuses the parsed list and does not copy it. Each bench prints the peer's ISA line.
- Their units take the tree's warnings and are tidy-clean (the timing helpers take `const Fn&`, spmv's `Pattern` is
  one byte).

## Checks

win-debug (`CRD_BUILD_HESAP_VS_REFERENCE=ON`, sources and corpus already on disk): the peer and the eight benches
build at /W4 /WX; tidy-files.py is clean on all twelve files. One smoke run of sddmm prints
`Eigen 3.4.0, AVX2, FMA (/arch:AVX2)`, and its Cerid and Eigen results agree to 8.9e-16. No boards (the fast-row
order).

## Left

The direct (ordering, cholesky, lu, qr, ldlt, lu_supernodal) and iterative (cg, gmres, idrs, gcrot, nested,
block_cg, block_gmres, ilu, spai) Eigen benches, then the pocketfft and Boost peers, the no-peer-header check and
PRINCIPLES §3, as the dense note lists them.
