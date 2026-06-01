# 2026-06-01 — hesap v5b-3: Multifrontal LU (serial + deterministic-parallel) + the honest gold-standard scoreboard

> One of the longest sessions in the project. Started from a **fabricated "crush"** (the prior session
> invented benchmark numbers); restarted to establish ground truth and build the real thing honestly.
> Full arc + every measurement: memory `project_lu_umfpack_gap_is_mc64_not_gemm`.

## What shipped

**`MultifrontalLU<T> : IFactorization<T>` (f32/f64/c32/c64)** — symmetric-pattern (MUMPS-style) multifrontal:
- `build_symmetric_multifrontal_symbolic(B)` = chol(B+Bᵀ) supernode fronts (containment holds by the
  Cholesky theorem; symmetrized fill measured ~free on the sim targets).
- Postorder front walk: scatter B → **in-place extend-add** of children's Schur (`mf_extend_add_trailing`,
  no copy) → `factor_front` (blocked partial-LU, static MC64 pivot + GESP, rank-nb TRSM + `dl::gemm`) →
  store L/U to CSC → stagnation-fixed IR solve.
- **Tree-parallel (v5b-3c):** level-scheduled `parallel_for` over the assembly tree + **within-front
  parallel GEMM** (`gemm_parallel_auto`) on big near-root fronts + **depth-1 lookahead** in `factor_front`
  (async panel-factor overlapping the trailing GEMM).

## The determinism moat — PROVEN
`[v5b-3c]` test: **L,U bit-identical across {1,2,4,8} workers AND == serial**, including a 260×260 dense
front that actually splits the parallel GEMM. No peer (UMFPACK/PARDISO/MUMPS) offers this.

## Validation
Full suite **591604/54** + ordering 7771 + iterative 112609 green; gcc -Werror clean; tree clean. Every
lever bit-identical (reconstruction oracle + residual + the moat as the gates).

## Levers landed (all moat-safe)
L1 MC64 Duff-Koster dual-init (ns3Da MC64 2333→170ms) · L2 `mf_extend_add` reusable-scratch + cache-friendly ·
L3 contribution-block buffer pool · L4 MC64 precompute-log · L5 in-place no-copy Schur · serial-fallback for
small problems · `resize_uninitialized` for the L/U arrays (af23560 serial ~256→183ms).

## The honest gold-standard scoreboard (fair, matched accuracy)
- **vs UMFPACK-1thr** (its real best — UMFPACK is *serial*, does *no MC64*): circuit gemat11 **1.45×** /
  memplus **1.33×** WIN; CFD sim targets af23560 0.71 / wang3 0.80 / ns3Da 0.72 (1w) = **at par cold**.
  Read the UMFPACK source + measured the split: our numeric **330ms beats** UMFPACK's **556ms** on ns3Da —
  static MC64 pivoting avoids UMFPACK's dynamic pivot-search tax. The bench "behind" is a best-of-N
  warm-cache artifact (UMFPACK warms; we re-allocate per call).
- **vs MUMPS @8 threads** (installed `libmumps-seq`, the *parallel* gold standard, `CRD_BUILD_HESAP_VS_MUMPS`):
  af23560 **1.16× — we beat it** (a genuine non-asterisk parallel-peer crush) · wang3 0.88× · ns3Da 0.64×
  (MUMPS wins the big 3D-NS fronts via async task-DAG + node-level 2D parallelism).

## Architecture decision (kept, user-confirmed)
**Static MC64 pivoting stays.** Reading the UMFPACK source confirmed it's the *right* tradeoff, not a tax:
MC64 spends ~107ms in analysis to *save* ~226ms in the numeric (our 330 vs UMFPACK's 556), AND it's what
makes the cross-thread determinism moat possible. Dropping it (dynamic pivoting like UMFPACK) would move the
cost into the numeric AND lose the moat — a strictly worse trade.

## Next (diagnosed, measured against MUMPS)
To win ns3Da/wang3: **async task-DAG scheduling + node-level parallelism** (the techniques MUMPS uses and the
literature confirms — we are level-synchronous/fork-join where they are async-DAG). The ~50ms analysis
micro-trims (cache-pack MC64 / AMD iw-GC / setup) are characterized as modest, not transformative.

## Owed before v5b commit
Per-slice DoD (win-debug/asan/shipping/tidy + ctest guards); CI owns the full 18-config sweep. The MUMPS bench
(`CRD_BUILD_HESAP_VS_MUMPS`, dev-only/GPL) is the permanent honest parallel yardstick now.
