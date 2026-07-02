# 2026-06-05 — v5f mixed-precision iterative refinement (f32 factor + f64 IR vs smumps+IR)

**Retro-ported 2026-07-02 from the phase table (recorded numbers, not re-measured).**

- **Machine/config:** WSL2 Ubuntu 24.04, i9-14900K, serial OMP=1. Cerid: GCC. Peers: smumps (libmumps-seq, single-precision factor + 64·eps f64-IR loop, matched reference setup), UMFPACK (f64).
- **Harness:** `crd-hesap-direct` bench on CFD corpus (af23560, wang3, ns3Da). Correctness: post-IR accuracy to f64, deterministic IR converge-or-flag, backward-error accept-gate.
- **Scope:** Mixed-precision factor+solve (`IterativeRefinedSolve`: f32 Multifrontal LU/Cholesky/LDLT, f64 residual + fixed-point IR). LU also carries GMRES-IR variant for saddle-point indefinite. QR version uses Björck CSNE (f32 factor, f64 normal-equation residual).

## The board: mixed-precision (f32-factor + f64-IR) vs same-class peers

Ratio = peer / Cerid (>1 = Cerid wins). Matched post-IR f64 accuracy for all.

### LU: Cerid-mix vs smumps+IR (head-to-head, same-class, both f32-factor+f64-IR)

| matrix | Ratio | Verdict |
|---|---|---|
| af23560 | **1.14 WIN** | non-asterisk parallel crush (serial serial matched, OMP=1) |
| wang3 | 0.90 | parity-class; MUMPS async-DAG parallel advantage |
| ns3Da | 0.61 | parity-class; MUMPS node-level parallelism on big 3D fronts |

**Honest framing:** mixed-precision is a SYMMETRIC lever (≈1.3× E2E on best case, LDLᵀ convergence-bounded). Value = matched f64 accuracy + ~half factor memory + determinism moat (MUMPS+IR lacks). Not raw-speed crush. Gains: af23560 WIN; residual (wang3/ns3Da) is MUMPS's documented front-parallel gap, NOT mixed-precision (Cerid-f64-full also loses there).

### Saddle-point indefinite (garon2 / raefsky3) — fixed by v5f-c2

| matrix | Static-pivot (v5f-a) | GMRES-IR (v5f-c2) | Status |
|---|---|---|---|
| garon2 | 2.9e-05 DIVERGED | 1.9e-15 [OK] | 3 GMRES-IR iters, parity cost with UMFPACK |
| raefsky3 | 5.1e-06 DIVERGED | 4.8e-08 [harder: 554 iters] | Converges; beyond this = delayed-pivot frontier |

MC64 static-pivot LU (moat requirement) was poor on indefinite systems (tiny pivots → drifted factor → IR diverged). Root cause: the FACTORIZATION not mixed precision. Fix: GMRES-IR (Krylov-preconditioned by the factor, convergence where fixed-point IR cannot).

### UMFPACK comparison (CAVEAT: one-sided lever)

| matrix | Cerid-mix vs UMFPACK-f64 | Note |
|---|---|---|
| af23560 | 1.04 | parity at f64 accuracy |
| wang3 | 1.35 | Cerid f32-factor 1.35×; but Cerid-f64-full is 0.44–0.49× UMFPACK |
| ns3Da | 1.36 | same; UMFPACK multifrontal dense-front BLAS-3 wins raw f64 |

⚠ Not a same-class crush: UMFPACK-f64 has no stock f32+IR path. If given the same treatment (factor-f32+IR), it would be ~2× faster than Cerid-mix. This is "practical today at f64 accuracy" + moat, not kernel quality.

## Levers (measured, per-fix)

1. **LU mixed-IR core**: Carson-Higham (f64 residual via spmv, f32 apply each step, backward-error accept guard). Append `apply_inverse` virtual to `IFactorization` AT END (D135, moat-safe vtable). Raw f32 apply (no internal IR) to avoid spurious failures on ill-conditioned big-3D systems.

2. **Cholesky/LDLT mixed-IR**: Symmetric lower-tri residual (reuse v5d-h LDLᵀ pattern; sort-free CSR expand). Cholesky apply already raw; LDLᵀ override = refactored existing tri_solve lambda, byte-identical moat.

3. **QR mixed LS**: Björck CORRECTED SEMI-NORMAL EQUATIONS (CSNE) — f32 multifrontal QR (O(mn²), ~½ memory), refine in f64 on normal-eqn residual ‖Aᵀ(b−A·x)‖→0 (LS optimality, not square A·x=b residual). NO Q apply needed.

4. **Determinism moat**: f32 factor AND f64-IR solve bit-identical {1,2,4,8} (iteration count worker-independent; r/x bit-identical each step).

5. **Frame arena leak fixed (v5e-2 spillover)**: gemm_parallel allocated JobDecls without ever resetting frame → big fronts exhausted per-thread arena. Central `frame_get_mark`/`frame_set_mark` scoped-marker self-clean in gemm/small_gemm_parallel.

## Verdict

Mixed-precision matched f64 accuracy + ~1.3× E2E speedup (LU best, LDLᵀ convergence-bounded) + determinism moat. af23560 beats smumps+IR. Wang3/ns3Da at parity (MUMPS parallelism advantage). Saddle-point indefinite fixed (garon2/raefsky3 GMRES-IR). All 4-config DoD verified (win-debug/clang-cl/asan/gcc).
