# 2026-10-29 — SIMD-DISPATCH: the iterative vs-reference benches on the Eigen peer

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md);
follows [the sparse direct part](2026-10-29-simd-dispatch-direct-eigen-peer.md). CI read:
[the environment note](2026-10-29-simd-dispatch-environment-readers.md).

## What landed

- `eigen_peer_iterative.cpp` adds Eigen's sparse iterative solvers to the peer. `IterativeSolve` names the eleven
  configurations the boards measure (CG with the diagonal and incomplete Cholesky preconditioners, MINRES, BiCGSTAB,
  GMRES and IDRS with the identity, diagonal or IncompleteLUT ones); `IterativeSettings` carries the iteration cap,
  tolerance, GMRES restart, IDR(s) dimension and ILUT drop tolerance and fill factor, defaulting to Eigen's. An
  `SpIterative` handle computes and solves one or many right-hand sides (each column on its own, iterations summed);
  `relative_residual` and `build_preconditioner` serve the benches' true-residual checks and timed setups.
- Converted: cg, gmres, idrs, gcrot, nested, block_cg, block_gmres, ilu and spai. Each prints the peer's ISA line, and
  each builds with the tree's warnings and passes tidy for the first time. Every vs-reference bench now calls Eigen
  through the peer, so `crd_add_hesap_vs_ref_bench` lost its `EIGEN_PEER` keyword and the in-unit fallback (Eigen
  headers, OpenMP, `/WX-` and `-Wno-error`), and runtime/'s own OpenMP lookup is gone (the root gives the peer
  OpenMP through `eigen_headers`).
- Timed regions keep their shape: where the bench built the solver per run, the run still builds, computes and
  solves; where it hoisted `compute`, it still does; each true residual stays inside the Eigen run. The right-hand
  side is now built once outside it (before: a per-run gather into a fresh `VectorXd`), which favours Eigen slightly.
- A failed preconditioner build reads as not converged (Eigen's `solve` would overwrite that `info()`).
- IDRS draws its shadow space from `std::rand` (`DenseMatrixType::Random`, IDRS.h:79), so each repetition solved a
  different problem. The peer seeds it the same before every IDRS solve, so the board gets one stable peer number per
  workload.
- In passing: the forwarding `best_ms` parameters, a capitalised preconditioner variable, the right-hand-side arrays
  `B` and `Bv` and an `int`-based enum, which tidy reported.

## Checks

On win-debug (`CRD_BUILD_HESAP_VS_REFERENCE=ON`): the peer and the nine benches build at /W4 /WX; tidy-files.py is
clean on all eleven changed files. Smoke runs against Eigen 3.4.0 with AVX2 and FMA: cg (bcsstk13: 1,451 and 1,448
iterations, both converged; MINRES 1,380 and 1,378), block_cg (tridiag and banded, residuals near 1e-8 on both sides)
and ilu (IC(0) against IncompleteCholesky on bcsstk13 and 24). No boards, following the fast-row order.

## Left

The pocketfft peer of the FFT and DCT boards, the Boost peers, the no-peer-header check and PRINCIPLES §3. win-debug
keeps `CRD_BUILD_HESAP_VS_REFERENCE=ON` until the pocketfft part lands.
