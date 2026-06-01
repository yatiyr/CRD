# Cerid hesap v5b-3 — Multifrontal LU (crush UMFPACK on CFD/sim-target factor)

> **Status:** v5b-3a SKELETON LANDED (2026-06-01) — symmetric-pattern front structure BUILT + validated;
> `build_symmetric_multifrontal_symbolic` (fronts from chol(B+Bᵀ) via `ordering::symbolic_factorize`) +
> `check_multifrontal_containment` in `multifrontal_lu.{hpp,cpp}`; test `[v5b-3a]` proves containment HOLDS
> on the same genuinely-unsymmetric matrices that VIOLATED it with unsymmetric-pattern fronts (full suite
> 591536/48, gcc -Werror clean). NEXT = v5b-3b dense-front BLAS-3 numeric (the crush kernel, FRESH context).
> Design blueprint from studying the SuiteSparse UMFPACK source (sparse-cloned WSL `~/ss/UMFPACK`).
> Build the numeric with FRESH context per `feedback_hesap_clean_structure_over_calendar`. Why this exists: the fair UMFPACK benchmark
> (2026-06-01) showed UMFPACK beats Cerid's supernodal left-looking LU **and** Eigen's ~2.6× on CFD
> factor (af23560 0.39×, ns3Da 0.16×) — an ARCHITECTURAL edge (dense frontal-matrix BLAS-3). See
> `project_lu_crush_two_kernel_plan`.
>
> ## ⚠ v5b-3a RESULT (2026-06-01) — THE ASSEMBLY MODEL IS DECIDED BY A GATING CHECK
> Built the multifrontal SYMBOLIC front structure (`multifrontal_lu.hpp/.cpp`: front-etree from the LU
> column-etree + per-front rows from L / cols from a U transpose pass) and the **containment check**:
> does every child front's contribution block fit inside its DIRECT parent front in BOTH dims (the
> `extend_add` ascending-subset contract)? **MEASURED (test `[v5b-3a]`, genuinely-unsymmetric patterns):**
> - SYMMETRIC pattern (positive control) → containment **HOLDS** (the Cholesky theorem; validates the check).
> - UNSYMMETRIC pattern → containment **FAILS** (dd_unsymmetric 1 row-violation; divergent 2 col-violations
>   persistent at n=150 & 500). A contribution block's rows/cols can belong to an ANCESTOR, not the direct
>   parent — exactly why UMFPACK splits assembly into LUson/Lson/Uson.
> ⇒ **The naive "child CB → direct-parent `extend_add`" (the v5a-1 Cholesky model) does NOT transfer to
> UNSYMMETRIC-PATTERN LU fronts over the LU column etree.** Discovered cheaply BEFORE building the
> contribution-stack + assembly pipeline (the gating check's whole purpose).
>
> **DECISION — pivot to SYMMETRIC-PATTERN (MUMPS-style) multifrontal:** build the assembly tree from the
> structure of **B+Bᵀ** (the PROVEN v5a symmetric multifrontal substrate: symmetric etree/supernodes +
> the working child→parent `extend_add` + the cross-thread determinism moat), and do the UNSYMMETRIC
> dense-front BLAS-3 numeric WITHIN each (symmetric-structured) front. Containment then HOLDS by the
> Cholesky theorem. For near-structurally-symmetric CFD/FEM (af23560, ns3Da — the sim targets), the
> symmetrized fill overhead is small. This is MUMPS's approach (itself a gold-standard target), reuses our
> infrastructure, and keeps the moat. The UMFPACK unsymmetric-pattern element-stack + tuple assembly
> (Sections 2.5-2.6 below) is the no-extra-fill alternative for genuinely-unsymmetric matrices — deferred
> (circuit matrices, NOT the sim target, already win via the supernodal/scalar path).
>
> ## ✅ v5b-3a SYMMETRIZED-FILL MEASUREMENT (2026-06-01) — SYMMETRIC-PATTERN IS ~FREE ON THE SIM TARGETS
> Measured (bench `CRD_BENCH_SYMFILL=1`, fair AMD-permuted corpus): UNSYM fill (lu_symbolic L+U) vs
> SYMMETRIC-PATTERN fill `2·nnz(chol(B+Bᵀ)) − n` (build_adjacency symmetrizes B internally), + B's
> structural symmetry. **SIM TARGETS: af23560 (CFD) 1.02× (90.3% sym) · ns3Da (3D NS) 1.06× (79.3%) ·
> raefsky3 (CFD) 1.03× (89.4%) · garon2 (2D NS FEM) 1.08× (71.5%) · wang3 1.00× (98.7%).** Several others
> are even SMALLER symmetric (add32 0.83×, memplus 0.88×, sherman3 0.97× — all 100% sym; our relaxed-
> supernode padding over-fills vs the tight symmetric factor). Only west2021 (a genuinely-unsymmetric
> chemical matrix, 2.4% sym — NOT a target) pays 1.67×. ⇒ **GREEN LIGHT: symmetrizing costs 0–8% extra
> fill on every CFD/FEM/NS sim target** (71–99% structurally symmetric ⇒ B+Bᵀ ≈ B). The symmetric-pattern
> (MUMPS-style) multifrontal is the path: dense-front BLAS-3 crush + the proven v5a symmetric assembly/moat
> + containment-by-theorem + ~free fill. NEXT: build the symmetric-pattern front structure (reuse
> `ordering::symbolic_factorize(B)` = chol(B+Bᵀ) supernodes; v5a substrate) then the v5b-3b dense-front
> unsymmetric BLAS-3 numeric (the crush kernel) within those fronts.

## 1. Why UMFPACK wins — the architectural edge (measured + source-confirmed)

Cerid's `SupernodalLU` is **left-looking supernodal**: for each supernode, gather contributions from all
descendants via many SKINNY per-descendant cmod GEMMs (k = descendant width, often small). Eigen's
SparseLU is the same family — and UMFPACK beats BOTH ~2.6×.

UMFPACK is **unsymmetric-pattern multifrontal** (Davis & Duff). It assembles a **dense frontal matrix**
per front and factors its pivots with a **delayed rank-`nb` BLAS-3 update** — the LAPACK right-looking
blocked-LU pattern applied to a dense front. The trailing update is ONE large dense GEMM (m×n×nb), far
more BLAS-3-efficient than many skinny left-looking cmod GEMMs. That dense-front GEMM is the crush.

## 2. UMFPACK numeric architecture (source: `umf_kernel.c`, `umf_blas3_update.c`, `umf_internal.h`)

### 2.1 Driver loop (`UMF_kernel`, `umf_kernel.c`)
```
for each chain in the column-etree (nchains, Chain_start):
    UMF_start_front(chain)                  // size the initial dense front for the chain
    for each front f in [f1..f2] of the chain:
        set up pivot-column candidate set (Front_npivcol[f] cands, MAX_CANDIDATES cap)
        while candidates remain:
            UMF_local_search()              // pick pivot col + row (threshold partial pivot + Markowitz)  <-- DYNAMIC
            if do_update:  UMF_blas3_update(); UMF_store_lu()       // flush accumulated pivots
            if do_extend:  UMF_extend_front()                        // grow current front in place
            else:          UMF_create_element(); UMF_init_front()    // push Schur as element; start new front
            UMF_assemble()                  // extend-add prior elements into current front
            UMF_scale_column()              // divide pivot column by pivot
            if fnpiv >= nb or front evaporates:  UMF_blas3_update(); UMF_store_lu()
        UMF_blas3_update(); UMF_store_lu(); UMF_create_element()     // wrap up last front in chain
UMF_kernel_wrapup()
```

### 2.2 Frontal matrix layout (`umf_internal.h` WorkType, lines ~807-869) — ONE memory block, 4 sub-blocks
Let `k = fnpiv` (pivots accumulated, ≤ nb), `r = fnrows`, `c = fncols`; `dr = fnr_curr` (leading dim,
**always ODD** to avoid cache aliasing), `dc = fnc_curr`.
- **`LU` (Flublock):** `nb×nb`. First `k×k` = the diagonal LU of the k pivots (unit-lower L + upper U packed).
- **`L` (Flblock):** `dr×nb`, COL-major. The k pivot columns below the diagonal block (leading `r×k` used).
- **`U` (Fublock):** `nb×dc`, **ROW-oriented**. The k pivot rows right of the diagonal (leading `c×k` used).
- **`C` (Fcblock):** `dr×dc`, COL-major. The contribution block = Schur complement (leading `r×c` used).
  Row indices `Frows[0..r-1]`, col indices `Fcols[0..c-1]` (global ↔ front-local maps `Frpos`/`Fcpos`).

Conceptually the front is `[ LU  U ] / [ L  C ]` — exactly LAPACK's blocked panel + trailing block.

### 2.3 The rank-`nb` BLAS-3 flush (`UMF_blas3_update`, the crush kernel)
After accumulating `k` pivots in LU/L/U:
1. **TRSM** (k>1): `U = LU⁻¹ · U` — `BLAS_TRSM_RIGHT(n, k, LU, nb, U, dc)` finishes the U block (apply
   the unit-lower L-part of the pivot block to the pivot rows).
2. **GEMM**: `C -= L · U'` — `BLAS_GEMM(m, n, k, L, U, dc, C, d)`. The big rank-k trailing update of the
   Schur block: `m=fnrows`, `n=fncols`, `k=fnpiv≤nb`. (k==1 fast path: `BLAS_GER` rank-1.)

This is the dense-front trailing update. With `nb`~32-64, k accumulates to nb between flushes ⇒ a FAT
rank-nb GEMM, not skinny rank-1s. This realizes BLAS-3 peak where our left-looking cmod realizes ~55%.

### 2.4 Symbolic structure consumed (`SymbolicType`, `umf_internal.h` ~883-933)
From `UMFPACK_symbolic` (the analysis phase): `nchains`/`Chain_start`/`Chain_maxrows`/`Chain_maxcols`;
`Front_npivcol[j]` (pivot cols of front j), `Front_parent` (super-column elimination tree),
`Front_1strow`, `Front_leftmostdesc`; `Cperm_init`/`Rperm_init`; `maxnrows`/`maxncols`; `nb`;
`Diagonal_map` (for the prefer-diagonal strategy). I.e. the symbolic builds the **assembly tree + chains
+ column pre-order + front-size bounds**; the numeric stays within those bounds.

### 2.5 Chains = in-place front reuse (the second efficiency idea)
A "chain" is a path in the front-etree where each front feeds directly into the next (parent reuses the
child's workspace by EXTENDING it in place — `UMF_extend_front`), bypassing the contribution-block stack.
Keeps data hot + amortizes allocation. Cross-chain parent/child go through elements (`UMF_create_element`
pushes the Schur as an element; `UMF_assemble` extend-adds it into the parent front later).

### 2.6 Unsymmetric assembly: LUson / Lson / Uson element types (`umf_assemble.c`)
Because the pattern is UNSYMMETRIC, a child element's contribution block overlaps the parent front in one
of three ways, each with a tailored assembly path: **LUson** (overlaps both parent pivot rows AND columns
⇒ full extend-add of the child C into the parent front), **Lson** (rows only ⇒ assemble row-slices into
the L side), **Uson** (columns only ⇒ assemble col-slices into the U side). UMFPACK tracks partial
assembly with `Rows[f]=EMPTY`/`Cols[f]=EMPTY` flags to avoid rescanning. **v5b-3a SIMPLIFICATION:** start
with a single full extend-add (scatter the child C[i,j] into the parent front at `Frpos[grow[i]]`,
`Fcpos[gcol[j]]` by GLOBAL index — handles all three cases uniformly since absent rows/cols simply don't
map). The LUson/Lson/Uson split is a later perf refinement (avoids scanning non-overlapping slices), not
needed for correctness.

## 3. Cerid moat-deterministic adaptation (the crux)

UMFPACK pivots **dynamically** (`UMF_local_search` = threshold partial pivot + Markowitz min-fill) ⇒ the
pivot sequence (and thus L,U) is data-order-dependent ⇒ **NOT bit-deterministic across workers**. Cerid's
moat forbids that. The SuperLU_DIST-style fix (already our supernodal model):

- **STATIC pivoting:** MC64 (`mc64_match_and_scale`, v4j-1a) permutes large entries onto the diagonal +
  scales toward an I-matrix ⇒ the diagonal is the column max ⇒ a STATIC diagonal pivot is stable in ~every
  column. The pivot sequence is FIXED by the symbolic + MC64 phase, NOT searched at numeric time. Tiny
  pivot ⇒ deterministic √ε·‖B‖ GESP perturbation (exactly as `dense_lu_nopivot` does today).
- ⇒ NO `local_search` at numeric time. The fronts, chains, and pivot order are all predetermined ⇒ the
  numeric is a pure deterministic function of the (fixed) structure ⇒ **bit-identical across {1,2,4,8}
  workers** (parallel = independent etree subtrees, exactly like v5a-3 Cholesky tree-parallel).
- **Stability recovered by IR** (Demmel-Li GESP) — we already have it, with the 2026-06-01 stagnation fix
  (`supernodal_lu.cpp::solve`). Saddle-point/indefinite NS still correctly flags INACCURATE (the moat's
  documented correctness limit — `project_hesap_simulation_target_and_gold_standards`).

### Front factorization without local_search (deterministic, BLAS-3)
Each front's fully-summed pivot block is factored by the LAPACK right-looking blocked pattern we ALREADY
built for the supernodal diagonal (`dense_lu_nopivot`, blocked nb=48): factor the nb-panel (static diag
pivot + GESP) → TRSM the U-row block → rank-nb GEMM trailing update of C. Reuse `dl::gemm` /
`trsm_unit_lower_left`. The contribution block C (Schur) is then extend-added into the parent front.

## 4. Reuse vs new (what exists in Cerid already)

**Reuse:** `Frontal<T>` + `extend_add` (v5a-1 multifrontal-assembly substrate, standalone-tested,
complex-ready); `dl::gemm` + `trsm_unit_lower_left` + blocked `dense_lu_nopivot` (the BLAS-3 front kernel);
MC64 front-end + AMD reorder + the column etree / relaxed supernodes from `lu_symbolic`; v5a-3
etree-level tree-parallel scheduling + per-worker scratch + the determinism-moat test harness; the IR
solve (stagnation-fixed). **New:** the front-etree → chains + front-size bounds (multifrontal symbolic,
analogous to UMFPACK_symbolic; our `lu_symbolic` supernodes are the seed); the dense-front assemble loop
(global↔front-local row/col maps `Frpos`/`Fcpos`); the contribution-block stack (+ chains as the in-place
optimization); the front-level numeric driver behind `IFactorization<T>`, selected by a dispatcher
(structured/CFD → multifrontal; circuit → supernodal/scalar — circuit already wins via MC64 low fill).

## 4a. FRONT LAYOUT DECISION — LOCKED 2026-06-01 (the gate the numeric was blocked on)

The contradiction the advisor surfaced: `extend_add` operates on the v5a-1 `Frontal<T>` (ROW-major,
single-block), but §2.2 / the v5b-3b plan describe UMFPACK's COL-major 4-block (LU/L/U/C) front. The
front-factorization kernel is the thing that FORCES this choice, so it must be made on paper first.

**DECISION: COL-MAJOR, SINGLE-BLOCK front + a col-major `extend_add` variant. (Defer UMFPACK's 4-block.)**
Rationale:
1. **Match the BLAS-3 kernels' native layout.** `dense_lu_nopivot` (blocked LU), `dl::gemm`,
   `trsm_unit_lower_left` are all COL-major. The multifrontal's whole value is dense-front BLAS-3
   throughput ⇒ store the front col-major so the hot path needs NO transpose gymnastics.
2. **The blocked partial-LU reuses the proven `dense_lu_nopivot` structure** (factor `nb` pivots →
   rank-`nb` TRSM+GEMM trailing update), generalized to stop at `npiv` and expose the trailing Schur.
   Minimal new numeric.
3. **A col-major `extend_add` is a ~10-line variant of the proven row-major one** — IDENTICAL ascending
   two-pointer row/col-merge index logic (the determinism contract = FIXED postorder contributor order,
   which is layout-INDEPENDENT) with only the inner scatter changed (`F[c*ld+r]` vs `F[r*ncols+c]`). The
   moat transfers. (Either add `ColMajorFrontal` or template `Frontal` on layout — decide at v5b-3b-1.)
4. **Single-block (not UMFPACK's 4-block LU/L/U/C) for the first numeric.** The 4-block separates stored
   L/U from the active Schur for cache + in-place storage reuse — a PERF/STORAGE refinement. A single
   dense m×n col-major front with the pivots factored in place + the Schur in the trailing
   (m−npiv)×(n−npiv) block, updated by BLOCKED rank-`nb` flushes, already achieves the BLAS-3 crush. The
   4-block is a CHARACTERIZED refinement (v5b-3c) IF throughput/memory needs it — don't pre-optimize
   storage before the numeric is correct + benched (the v5a "measure the premise" rule).
⇒ §2.2's 4-block layout is downgraded to "the perf-refinement target"; v5b-3b ships single-block col-major.

## 5. Slice plan (v5b-3, multi-session) — symmetric-pattern fronts, col-major

- **v5b-3a SKELETON ✅ DONE 2026-06-01:** `multifrontal_lu.{hpp,cpp}` — `build_symmetric_multifrontal_symbolic`
  (fronts = chol(B+Bᵀ) supernodes via `ordering::symbolic_factorize`) + `check_multifrontal_containment`.
  Containment PROVEN to hold on the unsymmetric matrices that failed with unsymmetric-pattern fronts;
  symmetrized fill measured ~free (1.00–1.08×) on the CFD/FEM/NS sim targets. Test `[v5b-3a]`, suite 591536/48.
- **v5b-3b NUMERIC (the crush kernel) — FRESH CONTEXT; against the LOCKED col-major single-block layout:**
  - **v5b-3b-1 ASSEMBLY:** the col-major front type + the col-major `extend_add` variant (the proven merge
    logic, layout-only change) + the contribution-block STACK. Validate vs the proven row-major `extend_add`
    (transpose-equivalent) + the determinism contract (fixed postorder).
  - **v5b-3b-2 FRONT FACTOR KERNEL:** `factor_front` — blocked partial-LU of `npiv` pivots over an m×n
    col-major front (static MC64 pivot + GESP; rank-`nb` TRSM + `dl::gemm`), leaving the Schur in the
    trailing block. A clean generalization of the proven blocked `dense_lu_nopivot` (rows→m, stop at npiv,
    trailing [npiv:m,npiv:n] = Schur). **PREREQUISITE (decided 2026-06-01 — found while scoping): the dense-LU
    kernel helpers `lu2_mag`/`lu2_one`/`lu2_from_real` + `trsm_unit_lower_left` are file-local to the
    anon namespace of the PROVEN `supernodal_lu.cpp` ⇒ FIRST extract them to a shared header (e.g.
    `dense_lu_kernels.hpp`) consumed by BOTH supernodal_lu.cpp and multifrontal_lu.cpp (DRY + the
    single-path quality bar). This EDITS the shipped supernodal numeric (the v5a/v5b-2 determinism moat) ⇒
    do it FRESH + re-run the moat tests (bit-identical {1,2,4,8}) after — NOT improvised at deep context.**
    Then `factor_front` against the shared kernels. Validate standalone vs a reference dense LU
    (reconstruct A = [L11·U11, L11·U12; L21·U11, L21·U12 + Schur], a tight oracle that catches any
    indexing/Schur-boundary bug loudly).
  - **v5b-3b-3 DRIVER:** `MultifrontalLU<T> : IFactorization<T>` — `build_symmetric_multifrontal_symbolic`
    → postorder front walk: alloc front, scatter A's pivot entries + assemble children's CBs (stack),
    `factor_front`, emit Schur CB to parent, store L/U into the CSC the existing solve reads. Validate vs
    the v5b-1 GP-LU oracle (residual on ORIGINAL A) + `SupernodalLU` (matched residual; fill ≈ symmetric-fill).
  - **v5b-3b-4 SOLVE:** reuse the existing `lu_lu_solve` + the stagnation-fixed IR (L/U are CSC — works as-is).
  - **v5b-3b-5 BENCH:** vs UMFPACK (1-thr fair) + Eigen-COLAMD on af23560/ns3Da/wang3/sherman3 — THE crush
    measurement. Target: beat UMFPACK's dense-front factor on the CFD sim targets.
- **v5b-3c PARALLEL + MOAT + refinements:** tree-parallel front walk (reuse v5a-3 level scheduling +
  per-worker front scratch) → verify L,U bit-identical {1,2,4,8} (the moat); within huge near-root fronts
  reuse the v5a-7 row-slab; relaxed front amalgamation; the 4-block col-major layout IF benched-needed.
- **v5b-3d** complex (c32/c64) + CLI `hesap.direct.lu_mf.*` + a per-matrix dispatcher
  (symmetric-pattern multifrontal for structured/CFD; supernodal/scalar for circuit).

## 6. Key source references (in `~/ss/UMFPACK/Source`, study at build time)
- `umf_kernel.c` — the driver (chains/fronts/the pivot-accumulate-then-flush loop). **(read 2026-06-01)**
- `umf_blas3_update.c` — the rank-nb TRSM+GEMM flush (the crush kernel). **(read 2026-06-01)**
- `umf_internal.h` — WorkType front layout + SymbolicType. **(read 2026-06-01)**
- `umf_local_search.c` — dynamic pivot + front-size/extend decision (we REPLACE with static MC64). TODO.
- `umf_assemble.c` / `umf_create_element.c` / `umf_extend_front.c` / `umf_init_front.c` — the
  assembly / element-creation / front-extension mechanics (extend-add + Frpos/Fcpos maps). TODO.
- `umf_start_front.c` / `umf_grow_front.c` — chain front sizing + growth. TODO.
- `umfpack_qsymbolic.c` / `umf_analyze.c` — how the symbolic builds chains + front-etree + COLAMD order
  (the analog of our `lu_symbolic`; informs v5b-3a). TODO.
- Davis & Duff, "An unsymmetric-pattern multifrontal method" + Davis 2004 column-preorder (UPM) papers.
