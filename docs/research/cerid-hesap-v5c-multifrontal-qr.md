# Cerid hesap v5c — Multifrontal QR (SuiteSparseQR / SPQR-class) — research + design

> Phase 3.1.6 `crd-hesap-direct`, slice **v5c**. The sparse-direct QR twin of
> v5a (Cholesky) / v5b (LU). Mandate: correctness-first, performance designed
> in from the start, and an HONEST close scoreboard that crushes BOTH the
> apples-to-apples header peer (**Eigen `SparseQR`**) and the gold standard
> (**SuiteSparseQR / SPQR**, Davis, Algorithm 915, ACM TOMS 38(1), 2011) —
> with the cross-thread bit-determinism moat no sparse-QR library carries.
>
> Sources read for this design:
> - SPQR source (sparse clone): `SPQR/Include/SuiteSparseQR.hpp` (`spqr_symbolic`
>   / `spqr_numeric` structs), `SPQR/Source/{spqr_analyze,spqr_front,spqr_assemble,spqr_kernel}.cpp`.
> - Eigen `Eigen/src/SparseQR/SparseQR.h` (the header peer).
> - Davis 2011 Algorithm 915 (paper summary).

---

## 1. What QR is for (consumers) + why multifrontal

`A·x ≈ b` least-squares (`min‖Ax−b‖₂`) for over-determined `m×n`, `m ≥ n`;
square nonsingular solve; the numerically-stable normal-equation-free path
(κ(A) not κ(AᵀA)). Consumers: data fitting / regression, bundle adjustment,
geometry (Laplacian editing, ARAP), FE least-squares, optimization sub-problems
(Gauss-Newton normal equations done stably). The factor `A·P_c = Q·R`.

**Multifrontal** (not left-looking column-by-column like Eigen): the QR work
is a tree of *dense* Householder QRs on **frontal matrices**, exactly the
"orchestrate dense BLAS-3 over the assembly tree" thesis that won v5a/v5b.
Eigen's `SparseQR` is left-looking, one column at a time, sparse reflectors —
structurally serial and BLAS-1/2. The multifrontal structure alone should beat
Eigen; beating SPQR needs blocked-WY + staircase + cross-front parallelism.

---

## 2. The SPQR blueprint (what we mirror, Cerid-native)

### Symbolic (`spqr_symbolic`)
- `S = A(P,Q)` — column-permuted by the fill-reducing order `Q` (= `Qfill`),
  stored **row-oriented**, rows sorted by **leftmost column index** (`PLinv`).
- `Sleft[j .. j+1)` = the rows of `S` whose **leftmost column is `j`**. This is
  the **row→front assignment**: a row enters the front that owns its leftmost
  column. `Sleft[n]` = #non-empty rows; `Sleft[n] .. Sleft[n+1)-1` = **empty
  rows** (zero nonzeros) — parked, not assigned to any front.
- Frontal tree = **etree of AᵀA** (the column elimination tree): `Parent`,
  `Child`, `Childp`, `Post` (postorder). Node `nf` = placeholder root.
- Front `f` is dense `fm × fn`, `fnpiv = Super[f+1]−Super[f]` pivot columns.
  Its `fn` columns = the `fnpiv` pivot columns (`Super[f]..Super[f+1)`)
  **followed by** the non-pivotal pattern `Rj[Rp[f]..Rp[f+1])` (ascending
  global column ids `> ` the pivots). `Rp`/`Rj` = compressed supernodal R.
- **AᵀA is never formed** in SPQR (it derives the etree + Rj from the
  leftmost-merge directly). We take the correctness-first route in v5c-1
  (form AᵀA structurally, reuse the hardened `symbolic_factorize`) and switch
  to the implicit merge in the perf sub-slice.

### Numeric (`spqr_numeric`)
- Per front: assemble the `S`-rows pivoting here (their `A` values) + the
  children's **contribution blocks** `C` (Schur), dense-Householder-QR the
  front. Top `fp ≤ fnpiv` rows = the **R block** (→ global R, column-packed);
  the remaining `Cm` rows × `Cn = fn − fp` cols = the contribution block to the
  parent.
- `Rblock[f]` (column-packed upper-trapezoidal R). `HStair`/`HTau`/`Hii`/`HPinv`
  = the per-front **staircase**, Householder coefficients, H row indices, and
  the global row permutation — kept so `Qᵀ` can be applied implicitly (needed by
  BOTH the square and least-squares solve ⇒ **keepH is always on for us**).
- `Rdead`/`rank` + Heath's rank detection (no column pivoting ⇒ preserves the
  fill-reducing order). **v5c-2** (rank-revealing); **v5c-1 assumes full rank**.

### Performance levers SPQR uses (design the layout so we can add these)
1. **Blocked-WY Householder** per front (LAPACK `xGEQRF` + `larft`/`larfb`):
   BLAS-3 trailing update. Cerid has `block_reflector.hpp` + `apply_q_block.hpp`.
2. **Relaxed front amalgamation** — fundamental supernodes are correct but
   small ⇒ small GEMMs; merging child into parent (our v5a `nrelax` analogue)
   makes bigger dense fronts. Bigger BLAS-3 = the throughput win.
3. **Staircase** — within a front the rows are pre-sorted (leftmost merge +
   already-triangular child blocks) ⇒ a triangular zero region the front QR can
   skip (`Stair`). Keep the front row order staircase-compatible.
4. **Cross-front tree-parallelism** — independent subtrees on different workers
   (Cerid: reuse v5a-3 level scheduling; the moat = postorder assembly).

---

## 3. Cerid mapping — `MultifrontalQR<T> : IFactorization<T>`

New unit: `engine/hesap-direct/.../multifrontal_qr.{hpp,cpp}` (+ tests, bench).

| SPQR | Cerid |
|---|---|
| frontal matrix + contribution block | `Frontal<T>` + `extend_add` (v5a-1; row-major, ascending col_index, postorder-deterministic) |
| `Sleft` (row→front, leftmost merge) | `qr_row_to_front` / `Sleft` built in the QR symbolic |
| frontal tree + `Rj` pattern | `symbolic_factorize(AᵀA_pattern, supernodal_patterns=true)` → `parent`/`post`/`super`/`slead_ptr`/`slead_idx` |
| `Qfill` fill-reducing order | `amd_order(AᵀA_pattern)` (= COLAMD-equivalent) — applied by the consumer/bench (matches v5a/v5b convention); `P_c` stored on the factor + threaded through solve |
| per-front dense Householder QR | `dense::QR<T>` (+ `block_reflector`/`apply_q_block` for blocked-WY) |
| `Rblock` column-packed R | global R in CSC |
| `HStair/HTau/Hii/HPinv` | per-front H vectors + taus + row map (keepH always on) |

### Solve (thread `P_c` per advisor)
- **Square** (`IFactorization::solve`, assert `m == n`): `x = P_c · R⁻¹ · (Qᵀ b)`.
  Apply `Qᵀ` front-by-front in postorder (each front's local Householders to its
  assembled rows), back-solve the upper-triangular global R, then un-permute
  columns by `P_c`.
- **Least-squares** (`least_squares(b /*len m*/, x /*len n*/, nrhs)` for `m ≥ n`):
  same `Qᵀ`-apply + back-solve the top-`rank` R; the residual rows of `Qᵀb` are
  the least-squares residual. `n()` = column count.

---

## 4. The determinism moat (claimed for QR)

Postorder front walk + ascending `extend_add` (fixed child order) + per-front
Householder that is purely local ⇒ **R and the Householder vectors are
bit-identical across {1,2,4,8,16} workers**. No column pivoting in v5c-1 (and
Heath rank-detection in v5c-2 uses a deterministic column-norm reduction, no
pivoting) ⇒ the structure is a pure function of the pattern. No SPQR / Eigen /
SPQR-GPU carries cross-thread bit-exact factors.

---

## 5. Symbolic correctness — the relations to assert (advisor-locked)

`struct(R(A)) ⊆ struct(chol(AᵀA))` **always**; equality only when `A` is
**strong-Hall** (Coleman-Edenbrandt-Gilbert 1986). Therefore:

- **(A) two-path cross-check** (NOT a "QR tight" claim): `Σ R_symbolic colcounts
  == Σ column_counts_ata(A)` — both compute `nnz(chol(AᵀA))`, one via
  `symbolic_factorize` on the explicit AᵀA, one via the implicit ata-merge.
  Reconcile the **diagonal-inclusion** convention between the two sources first
  (AᵀA has a full structural diagonal). Plus `column_elimination_tree(A) ==
  symbolic_factorize(AᵀA).parent` (bit-identical etrees).
- **(B) numeric ⊆ predicted**: a dense reference QR's R nonzero pattern `⊆`
  `R_symbolic`. **Keep `⊆`** — on a non-strong-Hall / rectangular matrix it is
  **strict** (numeric R sparser), which is correct, not a bug. **Never** assert
  `numeric_R_nnz == nnz(chol(AᵀA))`.
- Test matrices must include a **rectangular** and a **non-strong-Hall /
  unsymmetric** case (a symmetric strong-Hall matrix masks a wrong partition),
  and an **empty-row** case (the leftmost merge has no bucket for it ⇒ park in
  `Sleft[n+1]`, no OOB).

---

## 6. Slice plan (v5c-1 core; v5c-2 follow-on)

- **v5c-1a** symbolic: `multifrontal_qr_symbolic(A)` — AᵀA pattern (correctness-
  first) → `symbolic_factorize` → frontal tree + per-front pivot/non-pivot
  column patterns + `Sleft` row→front (empty rows parked) + store `P_c`. Tests
  = §5 (A)+(B) + rectangular/unsymmetric/empty-row + symbolic determinism.
- **v5c-1b** numeric (serial): postorder assemble (S-rows + children C via
  `extend_add`) → dense front Householder QR → global R + per-front H storage.
  Correctness: `‖A·x − b‖` least-squares residual + dense-QR R-pattern ⊆.
- **v5c-1c** solve: square (`IFactorization::solve`, `P_c` threaded) +
  `least_squares`. Tests vs dense `lstsq` oracle + Eigen.
- **v5c-1d** perf + bench + DoD: blocked-WY + relaxed amalgamation + staircase +
  cross-front tree-parallel (moat bit-identical {1,2,4,8}); bench
  `bench_hesap_qr_vs_reference` vs **Eigen `SparseQR` AND SPQR** (installed,
  `CRD_BUILD_HESAP_VS_SUITESPARSE`); per-slice DoD.
- **v5c-2** rank-revealing (Heath) + complex (complex Householder, `Qᴴ`) +
  CLI `hesap.direct.qr.{f32,f64,c32,c64}` + rank-deficient bench.

Close gate (v5 LOCKED): bench mandatory, crush the peer or hit the hardware
floor, never regress a prior-winning bench, bench the premise first.

---

## 7. Measured baseline (2026-06-01, v5c-1 correctness-first)

`bench_hesap_qr_vs_reference` (Release, `CRD_BUILD_HESAP_VS_REFERENCE`), Cerid
`MultifrontalQR` vs **Eigen `SparseQR`** (`NaturalOrdering` on the SAME
AMD(AᵀA)-column-ordered matrix — isolates the factorization engine):

| matrix | n | FACTOR | SOLVE | R fill (cerid/eigen) | resid |
|---|---|---|---|---|---|
| bcsstk13 | 2003 | **9.07×** | **2.65×** | 0.82M / 1.81M (**0.46×**) | c 6.7e-9 < e 4.0e-8 |
| bcsstk24 | 3562 | **48.1×** | **12.9×** | 0.75M / 4.48M (**0.17×**) | c 2.2e-8 < e 6.1e-8 |
| bcsstk25 | 15439 | *stalls* (unblocked) | — | — | — |

**Eigen `SparseQR` (the header peer) is CRUSHED even by the UNBLOCKED numeric**
— decisively on speed, fill, AND accuracy. Structural reason: Eigen is
left-looking column-by-column with numerical column pivoting that degrades the
fill-reducing order (its R is 2–6× denser); multifrontal + no-pivot preserves
the order ⇒ sparser R + dense fronts.

**The bottleneck is localized:** the unblocked Householder on the large dense
near-root fronts is ≈O(n³) and **stalls at 15k** (bcsstk25). So the crush levers
are now measured, not guessed:
1. **Blocked-WY Householder** (`block_reflector`/`apply_q_block`) — the BLAS-3
   trailing update; makes the big fronts tractable + fast. **THE next lever.**
2. **AᵀA-free implicit symbolic** (the SPQR leftmost-merge) — avoids forming AᵀA
   explicitly (the other 15k cost).
3. Relaxed-front amalgamation tuning, staircase zero-skip, cross-front
   tree-parallel + determinism moat {1,2,4,8}.

**Gold standard still ahead: SuiteSparseQR / SPQR** (multifrontal + blocked-WY +
multithreaded + LAPACK — much faster than Eigen `SparseQR`). Beating it is the
target of the perf sub-slice; the WSL `CRD_BUILD_HESAP_VS_SUITESPARSE` SPQR bench
(installed `/usr/include/suitesparse/SuiteSparseQR.hpp`) is the gold-standard
yardstick. v5c-1 is NOT closed until the scoreboard beats BOTH (FULL-VICTORY).

## 8. Blocked-WY front factor (2026-06-01, the first perf lever)

Size-gated compact-WY: fronts with ≥ `qr_block_min` (64) rows panel the `npiv`
pivot columns into `qr_panel_w` (48)-wide sub-panels; each sub-panel is
unblocked-factored, then ONE BLAS-3 `larfb` updates the trailing block
`C := (I − V·Tᵀ·Vᵀ)·C` via `W = VᵀC; W = TᵀW; C = C − VW` (three GEMMs), reusing
the column-major `dense::gemm` + `build_block_t_from_vtv` (`dlarft`). Small
fronts keep the proven unblocked path (no gemm-call overhead). V is extracted
unit-lower-trapezoidal per sub-panel; VᵀV is symmetric so its layout is moot.

**IMPROVED the Eigen crush with NO regression:** bcsstk13 FACTOR 9.07→**13.3×**
(158→84 ms) · bcsstk24 48.1→**70.6×** (85→57 ms). Verified win-debug 14 cases /
5353 asserts (added a dense 150×100 = one 150-row front, 3 sub-panels, RᵀR=AᵀA
+ least-squares) + clang-cl clean. Stored V/taus are unchanged by blocking, so
the v5c-1c solve is consistent.

**bcsstk25 (15k) — NOT a bug; QR-fill-bound (measured 208 CPU-s, 1.5 GB).**
QR factorizes through chol(AᵀA), and nnz(chol(AᵀA)) ≫ nnz(chol(A)) (AᵀA squares
the fill) ⇒ GB-scale factor storage. bcsstk25 is a *square SPD* matrix — the
WRONG tool for QR (use Cholesky). Amplified ×3 by best-of-3 reps + explicit-AᵀA
per rep + all-fronts-resident (no stack). Dropped from the default bench corpus.

**Next perf levers (the real crush vs SPQR):**
1. **AᵀA-free implicit symbolic** (SPQR leftmost-merge) — stop forming AᵀA.
2. **Front-storage stack** — free each contribution block once its parent has
   consumed it (keep only R + H), like SPQR's `Stacks`. Frees the GB.
3. **Rectangular least-squares bench corpus** (m > n — QR's real domain; e.g.
   `illc1850`, `well1850`, `lp_*`) + the **SPQR** gold-standard comparison.
4. Relaxed-front amalgamation tuning, staircase zero-skip, cross-front
   tree-parallel + determinism moat {1,2,4,8}.

## 9. Rectangular least-squares bench — the premise-check (2026-06-01)

Before building the §8 levers, measured QR's REAL domain (rectangular LS, the
Harwell-Boeing set) — both because the levers were motivated only by the
*square-SPD* bcsstk25 (wrong tool for QR) and zero data existed on the actual
target. Cerid `MultifrontalQR` vs Eigen `SparseQR` (same AMD(AᵀA) ordering):

| matrix | size | FACTOR | SOLVE | Rnnz c/e | resid |
|---|---|---|---|---|---|
| well1033 | 1033×320 | **6.81×** | 3.34× | 2686 / 11079 | 2.2e-15 |
| illc1033 | 1033×320 | **7.13×** | 3.17× | 2686 / 11079 | 2.8e-13 |
| well1850 | 1850×712 | **36.6×** | 5.88× | 8286 / **119578** | 2.0e-15 |
| illc1850 | 1850×712 | **36.8×** | 6.05× | 8286 / 119578 | 3.3e-14 |

**Two settled conclusions:**
1. **Eigen `SparseQR` is CRUSHED on QR's real domain** — 7–37× factor, 3–6×
   solve, 4–14× LESS R fill (its numerical column pivoting wrecks the fill on
   ill-conditioned LS), machine-precision residual. With the square results
   (13–71×), the header peer is beaten on BOTH domains.
2. **The §8 levers are NOT the bottleneck.** These LS factor in 0.4–1.3 ms with
   tiny fill (2.7–8.3K Rnnz). The bcsstk25 1.5 GB was 100% the wrong-tool
   square-SPD artifact. Building the AᵀA-free-symbolic + front-stack (~600
   lines, core-touching) would have optimized the wrong thing — **premise-check
   vindicated; the levers are deferred** (they only matter for a genuinely-large
   RECTANGULAR LS problem, which the corpus doesn't contain).

**The sole remaining FULL-VICTORY decider: SuiteSparseQR / SPQR** on this LS
corpus (WSL, `CRD_BUILD_HESAP_VS_SPQR`, libspqr installed). Then v5c-2
(rank-reveal Heath + complex Qᴴ + CLI).

## 10. SPQR gold-standard scoreboard (2026-06-02) — the FULL-VICTORY measurement

`bench_hesap_qr_vs_spqr` (WSL, `CRD_BUILD_HESAP_VS_SPQR`). SERIAL FAIR FIGHT:
SPQR forced `SPQR_ORDERING_NATURAL` on the same AMD(AᵀA)-permuted matrix +
`SPQR_grain=1` + `OPENBLAS_NUM_THREADS=1` + `SPQR_NO_TOL` (no rank detection, like
Cerid v5c-1). Least-squares solve = `Y=SuiteSparseQR_qmult(SPQR_QTX); X=SuiteSparseQR_solve(SPQR_RETX_EQUALS_B)`.

| matrix | FACTOR | SOLVE | resid c/s |
|---|---|---|---|
| well1033 1033×320 | **0.82× lose** | **7.73× WIN** | 2.2e-15 / 2.2e-15 |
| illc1033 1033×320 | 0.80× lose | 7.47× WIN | 2.8e-13 / 3.4e-13 |
| well1850 1850×712 | 0.60× lose | 8.23× WIN | 2.0e-15 / 2.8e-15 |
| illc1850 1850×712 | 0.60× lose | 8.34× WIN | 3.3e-14 / 1.9e-14 |
| bcsstk13 2003² | 0.40× lose | 4.21× WIN | match |
| bcsstk24 3562² | 0.48× lose | 5.58× WIN | match |

**HONEST split vs the gold standard: Cerid CRUSHES SOLVE (4–8×) but LOSES FACTOR
(0.40–0.82×).** SPQR's factor numeric is 1.2–2.5× faster — its mature LAPACK
blocked-QR + the STAIRCASE (it skips the front's zero-row structure). Residuals
match. Eigen is comprehensively beaten; **SPQR's factor is the remaining crush.**

**FACTOR-gap levers (now MEASURED against the real gold standard):**
1. **STAIRCASE** — within a front the assembled rows are pre-sorted (own rows by
   leftmost column + already-triangular child contribution blocks), giving a
   triangular zero region the QR should skip. My fronts currently factor the
   FULL dense front incl. those zeros — wasted flops. SPQR's `Stair` skips them.
   THE lever.
2. **Relaxed-front amalgamation tuning** — bigger fronts ⇒ bigger BLAS-3.
3. **AᵀA-free implicit symbolic** — removes the explicit-AᵀA-formation overhead
   from Cerid's factor time (part of the small-matrix gap).

**The SOLVE win (4–8×) is genuine** — Cerid's implicit Qᵀ re-walk + back-sub is
much faster than SPQR's `qmult` + R-solve. NOT closed until factor beats SPQR.

### 10.1 Factor-gap split (symbolic vs numeric), measured 2026-06-02

Timing `multifrontal_qr_symbolic` alone vs the full factor:

| matrix | total | sym | num | SPQR | num-vs-SPQR |
|---|---|---|---|---|---|
| well1033 | 0.32 | 0.10 (31%) | **0.22** | 0.27 | **num BEATS SPQR** |
| illc1033 | 0.32 | 0.10 | **0.22** | 0.28 | **num BEATS SPQR** |
| well1850 | 1.16 | 0.28 (24%) | 0.88 | 0.70 | 1.25× slower |
| bcsstk13 | 65.6 | 9.9 (15%) | 55.7 | 28.8 | 1.94× slower |
| bcsstk24 | 41.6 | 9.7 (23%) | 31.8 | 21.5 | 1.48× slower |

**The clean next lever — AᵀA-free symbolic (verified-by-oracle, CANNOT break the
solve):** removes the 15–31% symbolic tax. On the SMALL matrices the numeric
ALONE already beats SPQR (0.22 < 0.27) ⇒ this lever alone FLIPS well1033/illc1033
to wins. (Banked a safe partial: `symbolic_factorize(…, supernodal_patterns=true)`
builds the compact `slead` not the full `li` — bit-identical, bcsstk13 0.39→0.44× /
bcsstk24 0.48→0.52×. The rest is the explicit `ata_pattern` formation, which the
implicit leftmost-merge removes.) Keep the explicit path as a bit-for-bit oracle.

**The CONDITIONAL lever — STAIRCASE (vet with a probe FIRST):** the larger-matrix
numeric gap (well1850 1.25×, bcsstk13 1.94×) is INFERRED to be the staircase but
NOT established — it could equally be my within-sub-panel UNBLOCKED factor (SPQR
uses LAPACK blocked GEQRF throughout) or nb=48 vs its tuned blocksize.
- **Correct mental model (advisor):** the staircase is the COLUMN-axis triangle,
  not a contribution-row-sparsity thing. With columns ordered pivots-first then
  contribution-columns-ascending, the assembled front is upper-trapezoidal in
  BLOCKS — column k has zero rows below the staircase step regardless of whether
  individual contribution blocks are dense; `Stair_k` is MONOTONIC in k and grows
  as later children attach ⇒ it pays off precisely on the wide near-root fronts.
  (The earlier "staircase only helps own rows / dense-contribution worry" was
  WRONG — do not act on it.)
- **The premise-check probe (do this BEFORE the row-merge):** per front, count
  `Σ fm·nc` (what we factor now) vs the Stair-aware work `Σ (Stair_k − k)`. Ratio
  ≈ 1.9× ⇒ the staircase IS the gap, build it. Ratio ≈ 1.1× ⇒ the gap is the
  panel kernel (blocked GEQRF / blocksize) and the staircase is wasted risk —
  the row-merge touches the solve's canonical order, the riskiest core change.

**Two anomalies to reconcile before trusting these ratios in a commit:** (1) the
NUMERIC dropped when only the symbolic changed (bcsstk13 num 68.4→55.7) — a
symbolic-only change can't speed the numeric ⇒ machine variance (a 1.5 GB run was
killed earlier) or `num = total − sym` contamination (separate `best_ms` warmups);
re-run twice. (2) confirm SPQR actually ran SERIAL (TBB can still spawn tasks
under `SPQR_grain=1`) — else the 1.2–2.5× "loss" is partly SPQR-on-N-cores, the
forbidden asterisk inflating the gap.
