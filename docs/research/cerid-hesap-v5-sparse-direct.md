# Research — 2026-05-28 — hesap v5 sparse-direct factorization

> Deep-research design dossier for **Phase 3.1.6 `crd-hesap` v5 — sparse
> direct solvers**. Feeds the `## v5 — Sparse direct — DETAILED PLAN`
> section in `docs/phases/phase-3.1.6-hesap.md` and the eventual
> ADR-0065 §27 lock. Mandate (user-directed 2026-05-28): the **fully
> elite, world-class, COMPLETE** sparse-direct substrate — the direct
> twin of the v4 iterative spine. No deferred families.

## Question

What is the elite architecture + slice plan for `crd-hesap`'s sparse
**direct** factorization cluster (supernodal Cholesky, sparse LU,
multifrontal QR, sparse LDLᵀ, rank-structured HSS/BLR, mixed-precision
iterative refinement), such that it (a) beats Eigen on its overlapping
surface and matches the SuiteSparse reference floor (CHOLMOD / UMFPACK /
SPQR) in factor + solve time, and (b) holds the v4 **cross-thread
bit-determinism moat** no frontier direct solver offers?

## TL;DR

- **v5 = orchestrate v0's dense BLAS-3 kernels over v2c's assembly tree,
  deterministically.** The hard analysis is done: `SymbolicFactor`
  (v2c) already ships the elimination tree, postorder, column counts,
  full L-pattern (CSC), and the fundamental supernode partition. v5 is
  numeric factorization + tree-parallel orchestration, where every
  node's work is a dense panel op we already beat LAPACK with.
- **The determinism moat survives into direct solvers**, claimed
  per-family: Cholesky (no pivot, trivial), LDLᵀ (static 1×1/2×2),
  LU (MC64 + threshold partial pivot — NOT dynamic), QR (per-front
  Householder + deterministic assembly order). No CHOLMOD / UMFPACK /
  SPQR / MUMPS / PARDISO / SuperLU offers bit-exact factors across
  thread counts.
- **Complete means complete:** four exact factorizations + a real
  rank-structured sub-cluster (HSS *and* BLR — STRUMPACK + MUMPS-BLR
  patterns) + mixed-precision iterative refinement, all four type
  instantiations (f32/f64/c32/c64), CLI per op, all on the
  `crd-hesap-sched::DependencyGraph` that already exists.

## The three settled forks (user-directed 2026-05-28)

1. **Mixed-precision iterative refinement → IN**, as an opt-in
   substrate slice (v5f). It reworks the `Factorization<T>` solve API
   (low-precision factor + high-precision residual), so it is a
   substrate decision, not a follow-on.
2. **Weighted nested-dissection compression → IN** as prerequisite
   v5a-0. v5 is the named "real trigger" for the filed
   `v2e-weighted-compression` debt; large multi-DOF 3D FEM/CFD is the
   top consumer; the clean-structure mandate says solve it now.
3. **HSS → FULL, and BLR ships in v5 (both locked 2026-05-28).** The
   rank-structured sub-cluster (v5e) ships the **full STRUMPACK feature
   set** — randomized + from-dense HSS construction, ULV factorization,
   HSS-embedded multifrontal, adaptive rank / compression tolerance, with
   a dense-fallback when a front doesn't compress — AND completes the
   *family* with **BLR (MUMPS-BLR)**. BLR was ADR-0065-reserved; v5
   ships it (the reserve expansion is **locked**, pinned at the §27 lock).
   "Extend as much as possible; deliver a complete elite v5" (user).

---

## Architecture

### New module: `crd-hesap-direct`

`engine/hesap-direct/`. One-way deps, no cycles:

```
crd-hesap-direct
  ├─ crd-hesap            (LinearOp<T>, CLI CommandSchema)
  ├─ crd-hesap-sparse     (CSC/CSR storage; spmv for refinement residual)
  ├─ crd-hesap-ordering   (SymbolicFactor v2c; AMD/CAMD/ND; MC64; postorder)
  ├─ crd-hesap-dense      (dense panel kernels: Cholesky/LU/QR/LDLT/TRSM/GEMM/SYRK; rsvd/range-finder for HSS)
  ├─ crd-hesap-sched      (DependencyGraph — tree-parallel DAG, already shipped)
  └─ crd-jobs, crd-memory
```

### Two foundational data structures (specced once in v5a-1, reused everywhere)

1. **`Factorization<T>`** — the factored representation + a cheap,
   re-callable, **multi-RHS-from-day-1** `solve(F, B)`. Factor-once /
   solve-N is the FEM time-stepping + eylem articulation + opt
   inner-solve access pattern; never bolt multi-RHS on later. Carries
   the v4z factor-vs-solve break-even reporting hooks for benches.

2. **Frontal matrix + `extend_add`** — a dense `Matrix<T,RowMajor>` plus
   a relative row/col index map into the parent's pattern, and the
   scatter-add of a child's Schur-complement contribution block into its
   parent front. This is the central reusable kernel for multifrontal QR
   (v5c), multifrontal LDLᵀ (v5d), and the rank-structured fronts (v5e).
   Defined in v5a-1 even though left-looking supernodal Cholesky barely
   needs it, so v5c+ consume a settled surface.

### What v2c already hands us (the analysis is done)

`crd::hesap::ordering::SymbolicFactor` (`symbolic.hpp`):
`parent` (etree), `post` (postorder), `colcount`, `lp`/`li` (full L
pattern in CSC, diagonal-first, deduped), `super`/`nsuper` (fundamental
supernode partition). `symbolic_factorize(pattern, alloc)` builds it in
one adjacency pass. v5 numeric factorization is bit-for-bit the
structure of `chol(PAPᵀ)` under the already-applied ordering — we never
re-derive fill.

---

## The determinism moat — claimed per family

The differentiator. No frontier sparse-direct library offers cross-thread
bit-exact factors (they reduce frontal updates in completion order). The
universal discipline (v4 §26 D(iter)-1): **serial reductions within a
front; disjoint-slab parallelism over *independent subtrees* only; never a
fork-join sum across threads.** The assembly tree makes this natural —
sibling subtrees touch disjoint data until their parent assembles them,
and the parent's `extend_add` iterates children in a **fixed (ascending /
postorder) order**, so the assembled front is identical regardless of
which worker finished which child first. This exactly satisfies the
`DependencyGraph` contract ("ready tasks run in any order; algorithms
whose result depends on order must encode it as deps" — here the order
that matters is encoded in the deterministic parent-assembly step, not in
task dispatch).

| Family | Bit-exact across {1,2,4,8,16} threads | Determinism mechanism |
|---|---|---|
| **Cholesky** | L | No pivoting. Static supernodal etree schedule. |
| **LDLᵀ** | L, D, **pivot + delayed-pivot sequence** | *Static* 1×1/2×2 pivoting: per-front threshold test on deterministic assembled data + deterministic tie-break; delayed-pivot count is a function of the front, not of thread order. |
| **LU** | **pivot sequence**, L, U | **MC64 + threshold partial pivot.** Dynamic partial pivot is order-dependent → not parallel-deterministic → serial-reference only (v5b-1). |
| **QR** | R, Q (Householder vectors) | Per-front Householder is local + deterministic; assembly-tree order is the global determinism; rank-reveal column pivot uses a deterministic column-norm reduction + tie-break. |

Gate (v5z): `factor + solve` produce bit-identical {factor entries,
pivot sequence, solution} under {1,2,4,8,16} workers for all four
families, driven through `crd-jobs parallel_for` (per the
concurrent-tests-use-crd-jobs directive).

---

## Per-family algorithm decisions

### v5a — Supernodal Cholesky (SPD / HPD) — CHOLMOD-class, the flagship

- **Left-looking supernodal** (Davis 2006 ch.4; Ng-Peyton 1993). For
  supernode J: gather updates from descendant supernodes whose row
  pattern hits J → apply as a dense `GEMM`/`SYRK` (`cmod`) into J's
  panel, factor the diagonal block (dense Cholesky `cdiv`), `TRSM` the
  rectangular below-diagonal part. Left-looking matches CHOLMOD and the
  ADR's "Left-looking" Cholesky reserve.
- **Relaxed supernode amalgamation** (CHOLMOD `nrelax`/`zrelax`): merge
  fundamental supernodes that are nearly pattern-identical to grow the
  dense panels (bigger BLAS-3), trading a bounded number of explicit
  zeros. **Pinned deterministic merge predicate:** merge child into
  parent iff `extra_zeros ≤ nrelax · (parent_cols + child_cols)`
  (CHOLMOD's relaxed rule), evaluated in postorder — a pure function of
  the symbolic factor, so thread-independent.
- **Complex Hermitian** LLᴴ: real positive diagonal; `SYRK`→`HERK`,
  conj-transpose in the panel TRSM.
- **Tree-parallel** (v5a-3): supernode = a `DependencyGraph` task;
  child→parent edges. Sibling supernodes factor concurrently;
  determinism per the moat section. Level-by-level executor is
  deterministic and sufficient; the pipelined-DAG variant
  (`v0d-formal-dag-pipeline`, ~5-10%) is a perf follow-on.
- **Peer:** Eigen `SimplicialLLT` (apples-to-apples header) +
  SuiteSparse **CHOLMOD** (reference floor, WSL oracle).

### v5b — Sparse LU (unsymmetric) — Gilbert-Peierls reference + supernodal production

- **v5b-1 Gilbert-Peierls** (Davis 2006 `cs_lu`): left-looking,
  column-by-column. Column k = solve sparse `L x = A(:,k)` whose nonzero
  pattern is found by **DFS reachability** in the graph of L (symbolic +
  numeric fused — the elegant canonical), then partial-pivot + scale.
  Inherently sequential (column k needs 1..k-1) → determinism is trivial
  (serial) but it does not parallelize. Ships as the **correctness
  oracle + the dynamic-pivot reference**.
- **v5b-2 Supernodal LU** (Demmel-Eisenstat-Gilbert-Li 1999, SuperLU):
  column supernodes + 2D panel blocking + symmetric pruning of the DFS.
  **Deterministic + parallel pivoting = MC64 (v4j-1a, already shipped) +
  threshold partial pivoting**: MC64 permutes large entries onto the
  diagonal up front; threshold accepts the static diagonal pivot unless
  it is below `threshold · max-in-column`, only then swapping to the
  **lowest-index row** satisfying the threshold (the pinned deterministic
  tie-break, so the swap target never drifts on degenerate columns). This
  is the SuperLU_DIST static-pivoting strategy + a deterministic threshold
  guard → bit-exact pivot sequence across threads.
- **Peer:** Eigen `SparseLU` + SuiteSparse **UMFPACK**.

### v5c — Multifrontal QR — SuiteSparseQR / SPQR-class (Davis 2011)

- **Column elimination tree** (etree of AᵀA). Each front assembles the
  original rows with leftmost nonzero in this front's pivot columns +
  the contribution blocks from children (`extend_add`). Dense
  **Householder QR** per front → R rows for the pivot columns go to the
  global R; the residual rows (contribution block / "R-bar") pass up to
  the parent.
- **Q** stored as per-front Householder vectors for implicit Qᵀ-apply:
  least-squares `min‖Ax−b‖` = apply Qᵀ to b along the tree, back-solve R.
- **Rank-revealing:** in-front column pivoting (deterministic
  column-norm reduction + tie-break) + R-diagonal rank detection
  (SPQR heuristic). Enables minimum-norm / rank-deficient least-squares.
- **Peer:** Eigen `SparseQR` (limited) + SuiteSparse **SPQR**.

### v5d — Sparse LDLᵀ (symmetric indefinite) — multifrontal Duff-Reid (MA57-class)

- **Multifrontal LDLᵀ** (Duff-Reid 1983 — the original multifrontal
  method was symmetric-indefinite). Per front: eliminate fully-summed
  variables with **1×1 pivots** (diagonal large enough) or **2×2 pivots**
  (Bunch-Kaufman/Bunch-Parlett for indefinite); if a candidate pivot is
  too small, **delay** it — push the variable up to the parent front
  (the delayed-pivot mechanism that makes indefinite multifrontal
  robust). Static + threshold + deterministic delayed-pivot rule keeps
  the (deterministic) delayed count thread-independent.
- **Complex:** symmetric LDLᵀ (complex-symmetric, e.g. frequency-domain
  EM) AND Hermitian-indefinite LDLᴴ.
- **Peer:** Eigen `SimplicialLDLT` (fixed-pivot, no Bunch-Kaufman →
  framed as a breadth gap: our indefinite robustness is the value-add) +
  MA57/PARDISO-class if reachable.

### v5e — Rank-structured fronts (HSS + BLR) — STRUMPACK + MUMPS-BLR

The bottleneck of a 3D-elliptic factorization is the few large fronts
near the root: O(N^{2/3}) dimension, dense O(N²) factor cost. Their
off-diagonal blocks are low-rank (elliptic Green's-function decay), so
compress them.

- **v5e-1 Low-rank substrate + HSS kernel.** Interpolative
  decomposition (ID) + randomized range-finder generalized to a
  *sampling*/`LinearOp` form (extends v3b-3's `rsvd`/`rsyev`, which today
  take a dense `Matrix`). **HSS** (hierarchically semi-separable, Xia
  2010): binary partition with nested low-rank off-diagonal bases;
  **ULV factorization** + solve in O(r²N) vs O(N³).
- **v5e-2 HSS-embedded multifrontal — the FULL STRUMPACK feature set**
  (STRUMPACK, Ghysels-Li 2016). Compress fronts above a size threshold;
  the STRUMPACK key trick = build the front's HSS form **via randomized
  sampling** (`F·Ω`, `Fᵀ·Ω`) so the dense front is never formed.
  **Adaptive rank** (grow the sample count until the compression
  tolerance is met) + **dense-fallback** when a front doesn't compress +
  nested-dissection-aware clustering. Validate the 3D-Poisson asymptotic
  (O(N) vs O(N²) factor) AND robustness on indefinite/ill-conditioned
  fronts.
- **v5e-3 BLR-embedded multifrontal** (Amestoy et al. 2015, MUMPS-BLR).
  *Flat* (non-hierarchical) block-low-rank front blocking — simpler,
  more robust, cheaper to get right, the production default in MUMPS;
  complements HSS (BLR for moderate, HSS for extreme scale). **Ships in
  v5, expanding ADR-0065's BLR-reserve (locked 2026-05-28; pinned at the
  §27 lock).**
- Determinism: the randomized sampling uses a counter-based RNG
  (Philox/Threefry-class, deterministic per block index) so the sampled
  basis is thread-independent; compression tolerances are deterministic.

### v5f — Mixed-precision iterative refinement (HPL-AI / Carson-Higham 2018)

- Opt-in `refine` policy on `solve`: factor in f32, then iterate
  `r = b − A x` (computed in f64), solve `A d = r` with the f32 factor,
  `x += d` until f64 backward stability. ~2× factor speed + ~2× factor
  memory at full f64 accuracy — the CFD/FEM lever. Three-precision
  variant (Carson-Higham) reserved. Touches every `Factorization<T>`
  solve → substrate slice, not a follow-on.

---

## Recommendation for Cerid — slice order

v5a-0 (weighted-ND prereq) → v5a-1/2/3 (Cholesky: substrate+serial →
solve+complex+CLI → tree-parallel+moat) → v5b-1/2 (LU: GP reference →
supernodal MC64+threshold) → v5c-1/2 (QR: multifrontal → rank-reveal) →
v5d (LDLᵀ) → v5e-1/2/3 (HSS kernel → HSS-multifrontal → BLR) → v5f
(mixed-precision IR) → v5z (close: audits + end-to-end moat + ADR §27 +
18-config sweep + `docs/systems/hesap-direct.md`).

Honest calendar: ~14 elite sub-slices ≈ multi-month. Per
`feedback_hesap_clean_structure_over_calendar`, accepted and stated.
Revised size estimate ~7000+ LOC / ~165 tests (up from the table's
~4500/~140 — the rise is fuller HSS+BLR, mixed-precision IR, and
weighted-ND).

### Bench / reference strategy

Set up **`CRD_BUILD_HESAP_VS_SUITESPARSE`** in v5a-1 (same gating as
`CRD_BUILD_HESAP_VS_ILUPACK`: WSL, gitignored `external/`, never CI),
reused by v5b/c/d. Report `‖b−Ax‖/‖b‖` after refinement
(matched-true-residual + correct-peer discipline, `feedback_*`). Corpus:
SuiteSparse Matrix Collection — SPD `bcsstk*`/`ldoor`/`af_shell`/`nd24k`
(3D, for HSS); unsymmetric `rajat`/circuit + CFD `venkat`/`lhr`;
least-squares well/ill-posed; + the v4 corpus for continuity.

## What we read

- Davis (2006), *Direct Methods for Sparse Linear Systems*, SIAM — the
  CHOLMOD/UMFPACK/KLU foundation; Gilbert-Peierls `cs_lu`, supernodal
  Cholesky, etree/postorder (v2c already implements the symbolic side).
- Davis et al. (2011), *Algorithm 915: SuiteSparseQR* — multifrontal QR.
- Demmel, Eisenstat, Gilbert, Li, Liu (1999), *A Supernodal Approach to
  Sparse Partial Pivoting* — SuperLU; symmetric pruning, 2D blocking,
  static vs threshold pivoting.
- Duff, Reid (1983), *The Multifrontal Solution of Indefinite Sparse
  Symmetric Linear Systems* — MA27/MA57; 1×1/2×2 + delayed pivots.
- Liu (1985/1990) — supernodes, the multifrontal method survey,
  assembly tree.
- Xia, Chandrasekaran, Gu, Li (2010), *Fast algorithms for HSS matrices*
  + Ghysels, Li et al. (2016), STRUMPACK — HSS-embedded multifrontal via
  randomized sampling.
- Amestoy et al. (2015), *Improving multifrontal methods by means of
  block low-rank representations* — MUMPS-BLR.
- Carson, Higham (2018), *Accelerating the solution of linear systems by
  iterative refinement in three precisions* — HPL-AI mixed precision.
- Halko, Martinsson, Tropp (2011) — randomized range-finding / ID (the
  v3b-3 `rsvd`/`rsyev` already in `crd-hesap-dense`).

## Alternatives considered

- **Right-looking supernodal Cholesky** — viable, but left-looking
  matches CHOLMOD + the ADR reserve + the `DependencyGraph` push model;
  no reason to diverge.
- **Dynamic partial-pivoting parallel LU** — rejected for the production
  path: not parallel-deterministic (kills the moat). Kept as the v5b-1
  serial reference only.
- **WSMP/PARDISO-style left-right hybrid** — over-engineered ahead of a
  consumer; left-looking + multifrontal covers the families.
- **BLR-only (skip HSS)** — BLR is simpler and the MUMPS default, but
  HSS gives the true O(N) 3D-elliptic asymptotic the user's
  CFD/aerospace target needs. Ship both; they are complementary regimes.
- **Defer rank-structured entirely** — rejected by the elite/complete
  mandate; v5e is in.

## Pitfalls / gotchas

- **`default_allocator()` is banned in hesap** (`feedback_hesap_propagate_allocator`):
  every front/supernode scratch threads the matrix/factorization
  allocator. Never alloc inside a `parallel_for` body from a
  non-thread-safe TLSF — pre-size per-worker scratch by
  `crd::jobs::num_workers()`, `frame_reset()` between batch iterations
  (`feedback_jobs_*`).
- **Delayed pivots (LDLᵀ) grow the parent front** → front-size and L/D
  nnz are not known from the symbolic factor alone; size with headroom
  + a deterministic over-allocation rule.
- **Append new pure-virtuals at the END** of any `Factorization` /
  `LinearOp` interface (vtable-stability; win-release LTCG silently
  mis-dispatches otherwise).
- **MSVC LTCG + cross-config**: run 1 ctest + 1 clang-cl + 1 gcc build
  before close; the 5-config DoD is MSVC+AVX2 only and misses
  gcc `-Wfloat-conversion` / scalar-SSE2 fallbacks
  (`feedback_per_slice_binary_direct_misses_ctest_and_crossconfig`).
- **HSS randomized construction must use a counter-based RNG** keyed by
  block index, not a stateful stream, or the basis (hence the factor)
  becomes thread-order-dependent — breaks the moat.
- **HSS rank truncation `σ_k ≤ ε · σ_max` is only deterministic if
  `σ_max` is** — the singular-value reduction inside rank detection must
  use the same serial-reduction discipline as every other reduction, or
  the detected rank (hence the factor) drifts across threads.
- **Eigen `SparseQR` is weak** and `SimplicialLDLT` is fixed-pivot — for
  v5c/v5d the apples-to-apples Eigen comparison is a breadth/robustness
  story; the speed floor is the SuiteSparse/MA57 oracle.

## Proposed decision pins (for the ADR-0065 §27 lock at v5z)

Pre-enumerated to make the close mechanical (defer-to-close is the v4 §26
precedent; these are *proposed*, not locked):

- **D(direct)-1** — left-looking supernodal Cholesky (matches CHOLMOD + the
  ADR Cholesky reserve + the `DependencyGraph` push model).
- **D(direct)-2** — relaxed amalgamation predicate `extra_zeros ≤
  nrelax·(parent_cols+child_cols)`, evaluated in postorder.
- **D(direct)-3** — LU parallel determinism = MC64 + threshold partial
  pivot, lowest-index-row swap tie-break (Gilbert-Peierls dynamic-pivot is
  serial-reference-only).
- **D(direct)-4** — LDLᵀ = static Duff-Reid 1×1/2×2 + deterministic
  delayed-pivot rule (delay iff below threshold; deterministic tie-break).
- **D(direct)-5** — `extend_add` iterates children in fixed postorder
  (the per-front determinism anchor).
- **D(direct)-6** — HSS randomized sampling uses a counter-based RNG keyed
  by block index; rank truncation reduction is serial.
- **D(direct)-7** — BLR ships in v5, expanding ADR-0065's BLR-reserve.

## Open questions

- Whether v5 should also pull the `v2e-weighted-compression` ND quality
  fix beyond the multi-DOF case (currently scoped to v5a-0 prereq).
- Pipelined-DAG executor (`v0d-formal-dag-pipeline`, ~5-10%) — defer to
  a v5 perf follow-on unless a front-heavy bench proves level-sync is
  the bottleneck.

## Used by

- (pending) `docs/sessions/` v5a-0 onward.
- `docs/phases/phase-3.1.6-hesap.md` § v5 detailed plan.
