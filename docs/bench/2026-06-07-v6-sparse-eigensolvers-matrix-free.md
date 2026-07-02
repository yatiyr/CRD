# 2026-06-07 — v6 sparse eigenvalue: Lanczos / thick-restart vs ARPACK/PRIMME/scipy

**Retro-ported 2026-07-02 from the phase table (recorded numbers, not re-measured).**

- **Machine/config:** WSL2 Ubuntu 24.04, i9-14900K, well-separated spectrum test matrices (moat guard: clustered eigenvalues ≠ non-unique vectors). Cerid: GCC. Peers: scipy/ARPACK (primary), PRIMME (state-of-art symmetric), Spectra, FEAST.
- **Harness:** `crd-hesap-eigen` Rayleigh-Ritz via `eig_sym` (dense inner eigensolver), fixed-order reorthogonalization, deterministic counter-RNG start, sign convention (largest-|component| positive). Moat: `{1,2,4,8}` bit-identical eigenvalues AND eigenvectors through restarts.
- **Scope:** Matrix-free symmetric eigensolvers (Lanczos, thick-restart; generalized `A x = λ B x` native from day 1). Lanczos full-reorthog; thick-restart (Wu-Simon ≡ IRLM, bounded-memory restart). Non-symmetric + shift-invert + preconditioners deferred.

## Architecture verdict (not per-benchmark comparison table; crush axis is algorithmic)

| Axis | Result | Note |
|---|---|---|
| **Plain Lanczos parity** | ≈ ARPACK / Spectra | matvec-count matched; BLAS/LAPACK ceiling parity. |
| **Clustered eigenvalues** | Thick-restart converges | v6-a no-restart fails; v6-b fixes it (keep k=nev+buffer Ritz vectors). |
| **Moat (bit-determinism)** | Proven {1,2,4,8} | SplitMix64 deterministic start + MGS-twice reorthog + sign convention; ARPACK/PRIMME lack moat across worker counts. |
| **Algorithmic crush vector** | Preconditioned methods | LOBPCG/JD + shift-invert (our v5 factors) or AMG beats ARPACK on hard problems (fewer matvecs). NOT plain Lanczos. |

## Levers (algorithmic, core)

1. **Lanczos substrate (v6-a)**: Full reorthogonalization (MGS-twice) + fixed-order deflation/locking + deterministic counter-RNG (SplitMix64) + sign convention. Rayleigh-Ritz via inner `eig_sym` (dense v3a MRRR/D&C). parity+moat.

2. **Thick-restart (v6-b, Wu-Simon ≡ IRLM)**: Bounded-memory restart (keep k=nev+buffer Ritz vectors + residual); restarted matrix is arrowhead (diag θ + coupling s_i) but full reorthog absorbs them; cheap convergence via Ritz residual. Deterministic re-seed on lucky breakdown. Chosen over implicit shifted-QR (IRAM) = deterministic, moat-safe.

3. **Clustered eigenvalues fix**: v6-a's no-restart FAILS on bunched spectra (e.g. Laplacian largest, all near 4); v6-b restart + ncv=nev+buffer FIXES it by keeping buffer vectors capturing the clustered space.

## Test gates

- **v6-a (full Lanczos at n=16)**: 1D-Laplacian largest/smallest vs ANALYTIC λ_k + residuals <1e-9; run-twice bit-identical; {1,2,4,8} MOAT bit-identical eigenvalues AND eigenvectors (forced-parallel spmv, well-separated spectrum).
- **v6-b (thick-restart)**: Laplacian CLUSTERED largest eigenvalues at bounded ncv=20≪n=64 (no-restart FAILURE case); restart fixes it. Multi-restart n=240, well-separated end; {1,2,4,8} MOAT bit-identical through restart cycles.

## Verdict

Plain Lanczos/thick-restart = parity + matvec-count + moat (no raw-speed crush vs ARPACK — that's the algorithmic nature of eigensolvers). Crush vector = preconditioned (shift-invert + v5 factors, or AMG) on hard problems. Clustered eigenvalues handled (thick-restart fixes v6-a no-restart gap). Determinism moat proven (ARPACK/PRIMME lack it). 4-config DoD verified (win-debug/win-tidy + gcc-release RUN).
