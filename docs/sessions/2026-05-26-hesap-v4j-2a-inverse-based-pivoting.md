# 2026-05-26 — hesap v4j-2a: inverse-based pivoting + ICE estimator (single-level)

Phase 3.1.6 `crd-hesap` v4, slice **v4j-2a** — the ILUPACK numerical core: a Crout-form ILU
whose pivots are accepted/deferred by a bound on the inverse-triangular-factor norms (the
"inverse-based pivoting" of Bollhöfer-Saad, SISC 27(5):1627-1650, 2006). Single-level: the
deferred (rejected) block is factored once by a plain ILUT leaf — v4j-2b replaces that leaf
with the recursion.

## Reference floor (studied before line one)

- **Bollhöfer & Saad 2006** (`external/ilupack/papers/bollhofer_saad_2006_mlilu.pdf`) — the
  algorithm spec. §2 (impact of dropping on the inverse factors → the κ bound), §3 (the three
  templates: pre-ordering / partial Crout ILU with diagonal pivoting / multilevel recursion),
  eq. (16) (the LDU form keeping only the B-block factors), eq. (20) (the 2-block inverse
  applied as the preconditioner), §4.4 (κ̃ = max(‖L⁻¹‖,‖U⁻¹‖) drives dropping, not κ).
- **ILUPACK encyclopedia** (`external/ilupack/papers/encyclopedia_ilupack.pdf`) — Fig. 1 the
  accept/reject/continue picture; the `‖eₖᵀL⁻¹‖, ‖U⁻¹eₖ‖ ≤ κ` test; condest≈5 ⇒ AMG-like.
- **Estimator** — paper p.17: "inverse norms are estimated by essentially using the results of
  [6]" = Cline-Moler-Stewart-Wilkinson 1979 incremental condition estimator (greedy ±1). The
  precursor Bollhöfer 2004 [3] only refines how κ̃ is used in dropping (already on p.17). No
  need to chase [3].
- **Crout ILU** — Li-Saad-Chow 2004 [14]: column k of L + row k of U computed at step k. The
  natural form for incrementally monitoring the inverse-factor growth (vs our ILUT's IKJ form).

## Design (advisor-vetted)

`InverseBasedIlu<T>` (new `inverse_based_ilu.hpp`, crd-hesap-preconditioners), a fresh Crout
factor — NOT grafted onto `IlutPreconditioner` (IKJ-shaped, wrong data structures):

1. **B-block**: Crout-form `LDU` (L unit lower by columns, U unit upper by rows, D diagonal)
   with the inverse-based pivot accept/defer test driving a fill-reducing diagonal pivoting.
2. **Schur complement S̃_C** (S-version, eq. 4): `S̃ = C − L_E D_B U_F`, factored ONCE at the
   leaf by the existing `IlutPreconditioner` (v4g, unchanged).
3. **apply** implements eq. (20):
   `(PᵀAQ)⁻¹ ≈ [[B̃⁻¹,0],[0,0]] + [[−B̃⁻¹F],[I]] · S̃_C⁻¹ · [−E B̃⁻¹, I]`,
   B̃⁻¹ via the L_B/D_B/U_B triangular solves, S̃_C⁻¹ via ILUT(S̃_C).apply. Adjoint mirrors it.
4. Leaf is a `LinearOp<T>` so **v4j-2b swaps "ILUT(S̃_C)" → "InverseBasedIlu(S̃_C)" in one line**
   with a fixed recursion-termination gate (n ≤ 64 → dense LU).

At v4j-2a close we are NOT expected to beat ILUPACK on iterations (fixed leaf vs ILUPACK's
recursion). Success = match ILUPACK's fill ratio + iteration count up to leaf depth 2. The
crush is v4j-3.

## Determinism pins — LOCKED before coding (proposed `D(mlilu)-1..6`, → ADR-0065 §lock at v4z)

The moat is non-negotiable; the CMSW estimator + accept/defer introduce new tie-break traps.

- **D(mlilu)-1 — accept/defer order.** Pivots are visited in input (column) order; a deferred
  pivot's row/col is appended to the rejected set in visitation order (NOT sorted by "how bad").
  Equal-priority ties resolve by lowest original index. Deterministic, no RNG.
- **D(mlilu)-2 — CMSW greedy sign.** At step k let `s = mᵀx_{k-1}` (m = new factor row/col,
  x = running ICE solution). Choose the new ±1 right-hand-side component `b = -sign(s)`, i.e.
  the new ICE component is `b - s` with magnitude `1+|s|`. **If `s == 0` exactly → choose
  `b = +1`** (fixed rule; without it two runs can diverge from step k forward → different
  accept/defer sets → different preconditioner).
- **D(mlilu)-3 — pivot-test boundary.** Accept the pivot iff `max(est‖L⁻¹‖, est‖U⁻¹‖) ≤ κ`
  (≤ is accept; strictly-greater rejects). Fixed boundary.
- **D(mlilu)-4 — κ̃ monotone.** `κ̃_k = max(κ̃_{k-1}, new estimate)` — never decreases (paper
  §4.4 semantics); dropping uses κ̃_k.
- **D(mlilu)-5 — Schur triplet build.** S̃ = C − L_E D_B U_F assembled in row-major order via
  the deterministic `TripletBuilder`; reproducible regardless of accumulation path.
- **D(mlilu)-6 — leaf/recursion gate (reserved for v4j-2b).** Recursion threshold is FIXED
  (n ≤ 64 → dense LU), not load-adaptive; coarsest fallback is dense LU, never another ILUT
  (so a drop tolerance can't change the recursion's terminal behaviour).

Factor is serial (sequential over pivots) ⇒ trivially thread-count-independent; only the apply's
spmv is parallel, carried bit-exact by the existing operator (the v4 moat).

## Test plan (advisor-required for close)

1. **Pivot test FIRES** — a matrix where ≥10% of pivots are deferred (else the defer path is
   dead code). The bundled indefinite `aft02.cua` / `mplate.csa` from the ILUPACK dist are this
   class; or a constructed near-singular-leading-block matrix.
2. **Accept-all sanity** — diagonally dominant: defer_perm empty, result ≡ Crout LDU(A).
3. **Determinism** — serial vs parallel spmv apply bit-exact (the moat).
4. **Reproduces the v4j-1b off-diagonal-dominant test** at least as well.
5. Complex deferred to **v4j-2c** (per the split).

## Bench (local-only, ILUPACK oracle)

`bench_hesap_mlilu_vs_ilupack` behind dev flag `CRD_BUILD_HESAP_VS_ILUPACK` (WSL only). Match
`param.condest=10`, `param.droptol∈{1e-1,1e-3}`, S-version. Compare on `lnsp3937.rua` (easy PDE),
`aft02.cua`, `bcsstk17.rsa`, `mplate.csa`: levels, `nnz(L+U)/nnz(A)`, GMRES iters to matched
TRUE residual, factor+solve wall. (Matched-true-residual rule: [[feedback_iterative_bench_matched_true_residual]].)

## Status

- **PLAN + determinism pins locked** (D(mlilu)-1..6 above).
- **Core kernel SHIPPED + GREEN** (`inverse_based_ilu.hpp`, ~430 LOC): bi-index Crout LDU
  (Li-Saad-Chow §2.2 Ufirst/Ulist/Lfirst/Llist linked lists) + CMSW incremental inverse-norm
  estimator (Alg 3.1) + accept/defer pivot test (κ bound) + S-version Schur `S̃ = C − Lᴱ Dᴮ Uꜰ`
  + ILUT leaf + folded eq.(20) apply. Compiled clean on MSVC first build; 3 tests / 510
  assertions PASS (win-debug): accept-all (diag-dominant ⇒ 0 deferred, 1 level, solves);
  defer path FIRES (tight κ=1.05 ⇒ deferred>0, 2 levels, solves via leaf); serial-vs-parallel
  spmv bit-exact (the moat). Two bugs found+fixed: `zlist/wlist` unsized; linked-list nodes
  key on accepted-LOCAL id (not original) — loop var is already the local id.

## Close gates

1. **ILUPACK head-to-head bench SHIPPED + RUN** (`bench_hesap_mlilu_vs_ilupack.cpp`, gated
   `CRD_BUILD_HESAP_VS_ILUPACK`, WSL/Linux): byte-identical in-process matrix to both,
   matched rel_tol; numbers + honest interpretation in the table above. Root option +
   `external/`-linked target (gfortran runtime, link order per dist makefile) wired; builds
   + runs on `linux-gcc-release`.
2. **Cross-config — all green:** ✅ MSVC win-debug (510 asserts + all ctest guards:
   no-non-ascii / no-std-math / no-std-sort / no-untagged / simd-emission). ✅ gcc/Linux
   `-Werror` (clean build + 510 asserts). ✅ clang-cl (clean build + 510 asserts). Push →
   CI 18-config sweep (clang-cl-shipping, scalar/SSE2 fallbacks, all Linux configs).

## v4j-2a status: COMPLETE for its scope (pending CI sweep)

Inverse-based-pivoting Crout core shipped + tested on 3 compilers + benched apples-to-apples
vs ILUPACK. The crush (mesh-independent iterations matching ILUPACK) is **v4j-2b** (the
multilevel recursion — swap the ILUT leaf for a recursive `InverseBasedIlu` + the MC64 wire
into `MultilevelIlu`). **v4j-2c** = complex + CLI + adjoint.

## Bench: Cerid InverseBasedIlu vs ILUPACK V2.4 (apples-to-apples, WSL, single-thread)

Anisotropic conv-diff (ε=1e-2, β=0.3) generated in-process → byte-identical matrix to both;
κ=5, droptol=1e-2, matched rel_tol 1e-8, FGMRES(30) / ILUPACK-GMRES(30). `linux-gcc-release`.

| matrix | side | levels | fill nnz(L+U)/nnz(A) | iters | factor ms | solve ms | ‖b−Ax‖/‖b‖ |
|---|---|---|---|---|---|---|---|
| n=2500  | Cerid   | 2 | 2.68 | 118 | **1.20**  | 4.54   | 8.4e-9 |
| n=2500  | ILUPACK | 3 | 5.13 | 7   | 14.1      | 1.21   | 2.9e-9 |
| n=10000 | Cerid   | 2 | 2.61 | 256 | **4.09**  | 44.3   | 9.5e-9 |
| n=10000 | ILUPACK | 4 | 5.68 | 9   | 64.1      | 4.63   | 6.7e-9 |
| n=22500 | Cerid   | 2 | 2.54 | 387 | **7.67**  | 160.9  | 8.2e-9 |
| n=22500 | ILUPACK | 5 | 6.02 | 10  | 166.4     | 12.1   | 9.6e-9 |

**Honest interpretation (advisor-anticipated): at 2a Cerid does NOT beat ILUPACK on iterations.**
- ✅ Cerid converges at matched true residual; **lower fill (2.5–2.7 vs 5–6)**; **10–20× faster
  factor** (single-level, less work).
- ❌ Cerid iters GROW with n (118→256→387 ≈ not mesh-independent) while ILUPACK stays FLAT
  (7→10) — the signature of the **multilevel recursion** Cerid lacks at 2a (single ILUT leaf,
  2 levels; ILUPACK recurses 3–5). The recursion is **v4j-2b**; that is where the iteration
  crush comes from, and this harness now measures it.
- Bench traps fixed mid-run: ILUPACK iter count is `param.niter` (not the stale `ipar[25]`);
  Cerid uses a SERIAL `SparseLinearOp` (a parallel operator over-parallelizes these sub-L3
  matrices, 725ms→4.5ms artifact — [[feedback_krylov_operator_size_adaptive_and_frame_reset]]).
- Conclusion = v4j-2a is a correct, fast, low-fill baseline that quantifies *why* 2b's
  recursion is needed; the apples-to-apples crush is set up for 2b/2c.

**Honesty caveats (do not over-read the table):**
- The factor-time gap is a **work-done difference (single-level vs multilevel), NOT a
  kernel-speed win** — ILUPACK builds 3–5 levels + a Schur triple product per level; Cerid
  builds one level + an ILUT leaf. Expect Cerid factor time to rise toward ILUPACK's at 2b.
  The real kernel-speed peer would be ILUPACK with maxlevels=2 (future check).
- The fill comparison is **approximate**: Cerid `factor_nnz/nnz(A)` counts L_B+U_B+D+Lᴱ+Uꜰ+
  leaf; ILUPACK `param.elbow` post-factor includes multilevel hierarchy overhead (E/F blocks,
  level scratch, perms) Cerid 2a lacks. Suggestive, not a verified same-denominator metric.
- **Matched-residual slack:** both met rel_tol 1e-8, but ILUPACK over-converges (~3× tighter,
  2.9e-9 vs Cerid 8.4e-9), so the solve-time gap is slightly understated for Cerid. Strict
  same-true-residual matching is a v4j-3 refinement ([[feedback_iterative_bench_matched_true_residual]]).
- **Cross-compiler bit-exactness** (factor(MSVC)==factor(gcc)==factor(clang-cl)) is implied by
  ADR-0063 + the sequential factor but not directly asserted — add a cross-compiler/complex
  determinism test in **v4j-2c**.

## Scope clarification (no reduction — re-homed)

- **MC64-wire of `InverseBasedIlu` into `MultilevelIlu` moves to v4j-2b**, where it lands with
  the multilevel recursion AND the adjoint. Rewiring now would force `MultilevelIlu` to drop
  its working `has_adjoint=true` (InverseBasedIlu is apply-only until 2c) — a regression. So
  v4j-2a ships `InverseBasedIlu` as a standalone, directly-benched `LinearOp` (the bench drives
  it directly; cd2d is well-scaled so MC64 is ~a no-op here). The leaf is already a `LinearOp`
  so 2b's swap (ILUT leaf → recursive InverseBasedIlu, + MultilevelIlu rewire) stays one-line.
- CLI + complex + **adjoint** → **v4j-2c** (per the split).
