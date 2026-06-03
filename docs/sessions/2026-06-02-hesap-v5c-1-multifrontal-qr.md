# 2026-06-02 — hesap v5c-1: multifrontal QR (SPQR-class) — core + blocked-WY + honest gold-standard scoreboard

> New slice v5c-1: the sparse-direct QR twin of v5a (Cholesky) / v5b (LU). Built correctness-first,
> advisor-steered, measure-before-optimize throughout. **WIP — not committed.**
> Full design + every measurement: dossier `docs/research/cerid-hesap-v5c-multifrontal-qr.md`;
> memory `project_v5c_multifrontal_qr`.

## Research (gold-standard source, read first)
- Sparse-cloned **SuiteSparseQR / SPQR** (Davis Algorithm 915): read `spqr.hpp` (the `spqr_symbolic` /
  `spqr_numeric` structs), `spqr_front` / `spqr_analyze` / `spqr_kernel`. Distilled the front blueprint
  (`Sleft` row-merge, `Super`/`Rj` column structure, `Stair`, `Rblock`, `HStair`/`HPinv`).
- Read **Eigen `SparseQR`** in full — left-looking column-by-column with numerical column pivoting (a weak
  header peer, like Eigen-SimplicialLLT was for Cholesky).

## What landed (verified win-debug 14 cases / 5353 asserts + clang-cl clean)
`MultifrontalQR<T> : IFactorization<T>` (f32/f64), `engine/hesap-direct/.../multifrontal_qr.{hpp,cpp}`:
- **v5c-1a symbolic** — the QR fronts ARE chol(AᵀA) supernodes ⇒ REUSE the proven v5a
  `symbolic_factorize` + `build_supernodal_symbolic` on the AᵀA pattern. `QrSymbolic` = front tree +
  per-front pivot/contribution columns + leftmost-column row merge (`sleft`/`row_by_leftcol`, empty rows
  parked). Validated: AᵀA two-path cross-check (Σ colcount + etree bit-identical vs the implicit
  `column_counts_ata`/`column_elimination_tree`), front tiling, contribution⊆parent, postorder, determinism.
- **v5c-1b numeric** — postorder **scatter-PLACE / row-append** assembly (NOT the symmetric v5a `extend_add`:
  QR contribution COLUMNS map ⊆ parent but ROWS append ⇒ column-major front) + partial Householder QR. Global
  R (CSR) + stored H/taus. Verified ‖RᵀR−AᵀA‖≈0 (sign/identity-P_c robust).
- **v5c-1c solve** — implicit Qᵀ re-walk of the assembly tree in the SAME canonical row order (no HPinv array
  — provenance is in the symbolic) + back-substitute R. Square + least-squares; verified x_true recovery + LS
  optimality Aᵀ(Ax−b)=0 + multi-RHS.
- **v5c-1d blocked-WY** — size-gated compact-WY front factor: large fronts (≥64 rows) panel into nb=48
  sub-panels + ONE BLAS-3 `larfb` (`C := (I−V·Tᵀ·Vᵀ)·C`) via the column-major `dense::gemm` +
  `build_block_t_from_vtv` (dlarft); small fronts keep the unblocked path. Reused the verified column-major
  front (assembly+solve untouched) rather than relayout for the row-major `apply_q_block`.

### Advisor correctness saves (would have cost real time)
- QR is **not** the v5a `extend_add` (rows append, not match-by-id). `extend_add` stays for v5d LDLᵀ.
- `struct(R) ⊆ struct(chol(AᵀA))`, equality only strong-Hall ⇒ tests assert `⊆`, not `==`; the
  `Σ colcount`/etree match is a two-PATH cross-check, not a tightness claim.
- `&array[off]` when `off==size()` trips the bounds-checked `operator[]` → use `array.data()+off`.

## Honest scoreboard

**vs Eigen SparseQR (header peer; Release, same AMD(AᵀA) order) — COMPREHENSIVELY CRUSHED:**
| domain | FACTOR | SOLVE | R fill |
|---|---|---|---|
| rectangular LS (well/illc 1033, 1850) | **6.8–36.8×** | 3.2–8.3× | 0.07–0.24× (less) |
| square structural (bcsstk13/24) | **13.3–70.6×** | 2.9–9.2× | 0.17–0.46× (less) |
Eigen's numerical column pivoting wrecks the fill order; multifrontal + no-pivot keeps it. Blocked-WY
*improved* the square wins (9.07→13.3× / 48.1→70.6×) with no regression.

**vs SuiteSparseQR / SPQR (gold standard; WSL `CRD_BUILD_HESAP_VS_SPQR`, SERIAL fair = NATURAL on the same
permuted matrix + `SPQR_grain=1` + `OPENBLAS_NUM_THREADS=1`, no rank detection) — HONEST SPLIT:**
| matrix | FACTOR | SOLVE |
|---|---|---|
| well/illc 1033/1850 | **0.61–0.86× (lose)** | **7.2–8.9× WIN** |
| bcsstk13/24 | 0.44–0.52× (lose) | 4.2–4.5× WIN |
**Cerid CRUSHES SPQR on solve (4–8×) but LOSES factor (SPQR 1.2–2.5× faster — its mature LAPACK blocked-QR +
staircase).** Residuals match everywhere.

## Premise-check that SAVED ~600 lines (advisor + user "measure first")
The two planned levers (AᵀA-free symbolic, front-storage stack) were motivated ONLY by bcsstk25's 1.5 GB /
208 CPU-s stall. But **bcsstk25 is a square SPD matrix = the WRONG tool for QR** (nnz(chol(AᵀA)) ≫ nnz(chol(A))
— AᵀA squares the fill; you'd use Cholesky). Measured QR's REAL domain (rectangular LS) first: those factor in
**0.4–1.3 ms with tiny fill** ⇒ the levers are NOT the tractability bottleneck. Building the 600-line
core-touching refactor blind would have optimized the wrong thing. (Same discipline that has paid off all slice.)

## Factor-gap split (the de-risked next plan)
Timed `multifrontal_qr_symbolic` alone vs the full factor:
- **Symbolic = 15–31% tax** (explicit `ata_pattern`). On the SMALL matrices the **numeric ALONE already beats
  SPQR** (well1033 num 0.22 < SPQR 0.27) ⇒ the **AᵀA-free symbolic flips well/illc1033 to wins by itself**, and
  it's verified-by-oracle (can't break the solve). Banked a safe partial: `symbolic_factorize(…,
  supernodal_patterns=true)` (compact `slead` not full `li`, bit-identical) → bcsstk13 0.39→0.44× / 0.48→0.52×.
- **Larger matrices: numeric 1.25–1.94× slower** (bcsstk13 num 55.7 vs 28.8). Inferred staircase — **NOT
  established**; could be my within-sub-panel unblocked factor vs SPQR's LAPACK blocked GEQRF.

### Advisor corrections (load-bearing for next session)
- The staircase is a **COLUMN-axis triangle** (`Stair_k` monotonic; column k has zero rows below the step
  regardless of dense contribution blocks — pays off on wide near-root fronts). My "staircase only helps own
  rows / dense-contribution" worry was **WRONG**.
- **Probe BEFORE the row-merge**: per-front `Σ fm·nc` vs `Σ(Stair_k−k)`. ≈1.9× ⇒ staircase IS the gap; ≈1.1× ⇒
  it's the panel kernel and the row-merge (touches the solve's canonical order) is wasted risk.
- **Reconcile before trusting the ratios in a commit**: re-run the SPQR bench twice (the numeric moved on a
  symbolic-only change = variance / `best_ms` contamination) + confirm SPQR ran truly serial (TBB under
  `SPQR_grain=1`).

## v5c-1e — AᵀA-free implicit symbolic (DONE 2026-06-02)

`ordering::symbolic_factorize_ata(A)` — etree + counts + per-supernode leading patterns (`slead`) of
chol(AᵀA) computed **without ever forming AᵀA**. Reuses the proven implicit `column_elimination_tree` +
`column_counts_ata` (validated bit-identical at v5c-1a) + `fundamental_supernodes_i32`, and emits the
`slead` via the SAME assembly-tree recurrence `symbolic_factorize(…, true)` uses — with `adj_AᵀA(fc)`
gathered IMPLICITLY from A's rows (CSC column fc → rows → CSR row columns). `multifrontal_qr_symbolic`
now calls it; `ata_pattern`+explicit `symbolic_factorize` kept as the **bit-for-bit oracle**.

**Approach decided by MEASUREMENT, not argument (advisor caught the trap).** The dossier's two options:
Option A (`build_ata_adjacency` → feed `symbolic_factorize`) vs the true leftmost-merge. A throwaway
3-bucket symbolic sub-split (well1033 + bcsstk13, Windows) showed the **clique-union (`ata_pattern`)
DOMINATES** — 66% / **86%** of the symbolic — while `build_adjacency` (all Option A removes) is 7–9%.
⇒ Option A would NOT help; the true-merge (removes the clique-union) was required. The implicit
etree+counts were measured cheap (bcsstk13 10.9 ms clique-union → **0.40 ms** implicit, 27×) which
de-risked the build. The adj-gather did NOT leak p² (bcsstk13 new symbolic 12 ms → 1.36 ms).

**Verified:** new `[v5c-1e]` oracle test (4 shapes: banded / rectangular / unsymmetric+empty /
dense-wide-rows) asserts `symbolic_factorize_ata` bit-identical to the explicit path across
parent/post/colcount/lp/super/slead_ptr/slead_idx. win-debug full suites green (hesap-direct
**597051 asserts / 72 cases**, hesap-ordering **7771 / 44**) + all 5 ctest guards + clang-cl + gcc
`-Werror` (WSL).

**HONEST scoreboard vs SPQR (WSL serial-fair, 2 runs) — all six factor ratios IMPROVED, but the
predicted flip did NOT land:**
| matrix | factor BEFORE | factor AFTER | flip? |
|---|---|---|---|
| well1033 | 0.82× | **0.91–0.94×** | NO — at parity (noise floor) |
| illc1033 | 0.80× | 0.90–0.91× | NO |
| well1850 | 0.60× | 0.64–0.65× | NO (num-bound) |
| illc1850 | 0.60× | 0.67–0.68× | NO (num-bound) |
| bcsstk13 | 0.40× | 0.50–0.51× | NO (num-bound: num 57 vs SPQR 29) |
| bcsstk24 | 0.48× | 0.63–0.64× | NO (num-bound) |

The dossier's "AᵀA-free symbolic flips well/illc1033 by itself" was OPTIMISTIC: cerid *numeric* alone
(0.21) does beat SPQR's *total* (0.25), but the **residual ~0.07 ms symbolic** (now
`build_supernodal_symbolic` + row-merge + slead, NOT the AᵀA tax) plus a faster SPQR sample (0.25 vs the
stale 0.27) keep cerid's total at 0.28 > SPQR 0.25. The gap to 1.0× (~0.03 ms) is **at the run-to-run
noise floor** — not worth chasing (a "win" manufactured by shaving 0.03 ms off a 0.28 ms factor is a tie).
well/illc1033 are characterized as **at parity** (num-competitive). bcsstk13/24 + well/illc1850 stay
**numeric-bound** — the real FULL-VICTORY battlefield, and exactly v5c-1f's staircase / blocked-GEQRF scope.

## v5c-1f — assembly scatter-map (DONE 2026-06-02) — FIRST SPQR FACTOR WINS

**The measurement story (the durable lesson — three throwaway probes, two refutations, one win):**
1. **Staircase REFUTED.** Probe: per-front `Σ(fm−k)(fsz−k)` (work now) vs `Σ(Stair_k−k)(fsz−k)`
   (staircase-aware), on the real assembled fronts. Result the OPPOSITE of the predicted geometry —
   the matrices that matter sit at **1.08–1.16×** (bcsstk13 1.13 · bcsstk24 1.08 · well/illc1850 1.16),
   far below the 1.9× gate; only the noise-floor 1033s hit 1.70×. ⇒ the row-merge staircase (which would
   touch the solve's canonical order) is NOT worth it. The advisor's "registers on wide fronts" guess was
   wrong; the measurement settled it.
2. **Panel BLAS-2 REFUTED.** Same-platform kernel split showed the within-sub-panel BLAS-1 reflector loop
   is 66–87% of the kernel on the rectangular-LS home turf. Converted it to LAPACK `dlarf` form
   (`dense::gemv`+`ger`) — **2–4× SLOWER** (well1850 num 0.90→3.57 ms): the per-call overhead on tiny
   panel blocks dwarfs the work. The hand-rolled loop is already an inlined, fused `dlarf`. Reverted.
3. **The WIN was the unmeasured third thing — ASSEMBLY scatter overhead.** A same-platform (WSL/gcc, no
   cross-platform trap) phase split {csr, assembly, kernel, rbuild} showed **assembly = 41% of the
   home-turf numeric (0.35 ms)** — the per-nonzero `find_col` BINARY SEARCH — and the SPQR deficit on
   well1850 was exactly ~0.37 ms. Replaced `find_col` (O(log fsz)) with an **O(1) scatter map** (`col_pos`
   global→local, set per front over `fn`, read only for columns ⊆ `fn` ⇒ no reset needed; debug assert
   `fn[col_pos[c]]==c` guards the invariant).

**HONEST scoreboard vs SPQR (WSL serial-fair, 2 runs) — every ratio up, FIRST factor wins, zero regression:**
| matrix | factor BEFORE v5c-1f | AFTER scatter map | |
|---|---|---|---|
| well1033 | 0.91× | **1.06–1.16× WIN** | 🎉 first SPQR factor win |
| illc1033 | 0.90× | **1.12–1.17× WIN** | 🎉 |
| well1850 (home judge) | 0.64× | **0.87–0.96×** | near-parity (was 0.60 at v5c-1d) |
| illc1850 | 0.67× | **0.93–0.95×** | near-parity |
| bcsstk13 | 0.50× | 0.71–0.76× | improved; larfb-gemm wall |
| bcsstk24 | 0.63× | **0.91×** | near-parity; larfb-gemm wall |

Solve still crushes 4.0–8.6×; residuals match; the `[v5c]` suite stays bit-correct (RᵀR=AᵀA + LS
optimality). well/illc1850's residual gap is the v5c-1e symbolic floor (`build_supernodal_symbolic` +
row-merge — checked, it's linear sorted-merge, NOT a `find_col` pattern ⇒ no cheap scatter-map win there)
+ a faster SPQR sample; it bounces 0.87–0.96× (the spread IS the gap-to-1.0) ⇒ characterized at near-parity,
not chased.

**bcsstk13/24 (square SPD) = characterized accepted loss:** 83–88% larfb-gemm = the ADR-0082 BLAS-kernel
wall (Cerid intrinsics vs OpenBLAS asm), on a matrix that is the WRONG TOOL for QR (use Cholesky — we
already crush CHOLMOD). The determinism moat + the solve-crush + the Eigen-crush (both domains) stand.

**Config gate (the touched modules):** win-debug (hesap-direct 597051+/72, 5393 `[v5c]`) · clang-cl ·
win-asan (the raw-index scatter map OOB-clean) · win-tidy (clang-tidy 20.1.8 — fixed 4 PRE-EXISTING v5c
issues surfaced: `qr_panel_w`/`qr_block_min`→`kQrPanelW`/`kQrBlockMin` global-const naming, `rn`→`rnum`
confusable, a nested ternary→if/else; the scatter map itself was tidy-clean) · gcc `-Werror` (WSL).

## v5c-1g — cross-front tree-parallel + the DETERMINISM MOAT (DONE 2026-06-02)

Level-scheduled the assembly tree (`level[f] = 1 + max(child level)` via the `front_post` postorder;
ascending-f within a level ⇒ worker-order-independent), refactored the pass-2 body into a `factor_front(f, wk)`
lambda with **per-worker scratch** (vbuf/vtv/tblk/wbuf/col_pos as disjoint `worker_index()`-keyed slices,
sized by `jobs::num_workers()`), and dispatched each level via `jobs::parallel_for` + `wait` + `frame_reset`
(serial path when `num_workers ≤ 1`). The larfb gemms pass `scratch=nullptr` → the per-thread pooled
GrowableTlsf (thread-safe + result-identical). Front-parallel only — deliberately NOT the LU's
hybrid/node-parallel machinery (the home-turf LS matrices factor in <1 ms; there's no wall-clock to chase,
and parallel-vs-serial-SPQR is the forbidden asterisk).

**Race-freedom (advisor's discriminator):** a QR front's `m_fb` is read ONLY by its direct `front_parent`
(the assembly reads `m_child[cc]` blocks, and the symbolic guarantees each child's contribution columns ⊆ its
direct parent's `fn`), so the per-level `wait` barrier dominates all readers. The `col_pos` no-reset trick is
safe per-worker (each worker's slice is disjoint; reads ⊆ its own freshly-set `fn`).

**MOAT PROVEN** (new `[v5c-1g]` test): R, the global Rj/Rx, AND the least-squares solution are
**BIT-IDENTICAL across {1,2,4,8} workers at BOTH f32 and f64** (block-diagonal matrix = 4 independent banded
arms ⇒ a level with ≥4 concurrent fronts ⇒ per-worker scratch isolation genuinely exercised; f32 chosen
because float reassociation is where worker-count drift surfaces first). No sparse-QR library (SPQR/Eigen)
carries cross-thread bit-exact factors — the standing differentiator. **VERIFIED 5 configs:** win-debug
(full suite **597087/73**) + clang-cl + win-asan (per-worker scratch OOB-clean under parallel) + win-tidy +
gcc `-Werror`. HONEST: completeness + the moat, NOT a speed crush.

## Owed before v5c COMMIT (phase close, → CI)
gcc/asan/shipping full-sweep · then **v5c-2** (below).

## v5c-2 PLAN — start FRESH (captured 2026-06-02 at the v5c-1g checkpoint; deliberately NOT started at session tail — complex-conjugation correctness in factor+solve is the highest bug-risk in the cluster and fatigue eats exactly that class of bug). Three sub-slices, in order:

### 2a — complex (Complex32/Complex64), UNBLOCKED-ONLY — ✅ DONE 2026-06-02
Correctness-first: complex QR ships + is correct at all sizes via the unblocked Householder path; **blocked-WY-for-complex is a PERF follow-on, not a deferred feature** (factual check done: `build_block_t_from_vtv` assumes real `tau` and `dense::gemm` would need the `ConjTranspose` path + a conjugation-aware compact-WY T ⇒ real work, not a small `if constexpr`). So for complex, gate `blocked=false` (unblocked applies each reflector to all trailing columns); the larfb is wrapped in `if constexpr(!is_complex)` so it never instantiates for complex.

**LANDED:** `make_householder_complex` + a `qr_conj` (identity for real, `crd::hesap::conj` for complex) + `qr_from_real` bridge. The reflector apply (factor AND the solve's Qᴴ re-walk) uses `if constexpr`: complex dot = `qr_conj(v)·c` (= vᴴc), scalar = `qr_conj(τ)`, the v-tail update stays un-conjugated, R-diagonal = real β via `qr_from_real`. **The real path is byte-identical** (every `qr_conj` is a no-op for real ⇒ the determinism moat is unchanged). `Complex32/Complex64` instantiations added. **The DERIVED conjugation was CORRECT** — verified empirically: new `[v5c-2a]` test asserts **RᴴR == AᴴA** (genuinely-complex values, nonzero imaginary part — would fail if Qᵀ vs Qᴴ were confused) + complex least-squares recovers a complex x_true, for both Complex32 and Complex64; the complex determinism moat {1,2,4,8} also bit-identical (`run_qr_parallel_moat<Complex32/64>`). **VERIFIED 5 configs:** win-debug (5529 asserts / 17 cases) + clang-cl + win-asan + win-tidy + gcc `-Werror` (caught + fixed `R{double_literal}` narrowings → `static_cast<R>` per the CLAUDE.md rule). HONEST: unblocked-only (blocked-WY-complex perf follow-on).
**The conjugation — DERIVED, verify FIRST thing next session (this is THE thing to get right):**
- Use `dense::detail::make_householder_complex<R>(Complex<R>* x, n)` → `{tau (complex), beta (REAL)}`, writes v-tail to `x[1..]`, leaves `x[0]`, `v[0]=1` implicit, gives `Hᴴ·x = beta·e₀`.
- The factorization applies **Hᴴ** (zlarfg zeros via Hᴴ). `Hᴴ = I − conj(tau)·v·vᴴ`. Apply to trailing column c:
  - `s = vᴴ·c = c[0] + Σ_{i≥1} conj(colk[i])·colj[i]`  (dot uses **conj(v)**)
  - `scale = conj(tau)·s`
  - `colj[0] −= scale`;  `colj[i] −= scale·colk[i]`  (update uses **v un-conjugated**)
  - R diagonal `colk[0] = beta` stored as `Complex{beta, 0}` (beta real).
- The SOLVE's Qᴴ re-walk applies the SAME reflectors in the SAME order ⇒ the SAME complex apply form. Back-sub `Rx=c` is already type-generic (complex `/` diagonal works).
- Real path stays exactly as-is via `if constexpr` (a `is_complex<T>`/RealType trait; mirror v5a-2's real↔complex `if constexpr`).
**Verify:** RᴴR == AᴴA (not RᵀR) on a genuinely-complex full-rank A; complex square solve + complex least-squares optimality `Aᴴ(Ax−b)=0`; complex determinism moat {1,2,4,8} (bit-identical, on a complex block-diagonal — extend `block_banded_ls` to complex values). 5-config gate.

### 2b — rank-revealing (Heath), no pivoting — ✅ DONE 2026-06-02
NO column pivoting ⇒ preserves the fill order + the moat (SPQR's key trick). **Detection is a pure
function of the FINAL R diagonals** (key realization: in no-pivot multifrontal QR a rank-deficient pivot
just yields a ~0 R-diagonal — the contribution block + assembly are structurally unaffected, so the factor
needs NO special-casing). After R-build, a SERIAL scan (r=0..n-1, fixed order ⇒ worker-invariant ⇒
moat-safe) finds max|R diagonal|; a pivot with `|R[r][r]| ≤ rcond·max` (rcond = max(m,n)·eps) — or a
structural no-pivot column — is DEAD. `rank()` = #live, `dead()` = per-column flags. The least-squares
solve returns the **BASIC solution** (dead variables = 0; back-sub skips dead rows, no divide by ~0). The
old `m_info`-flags-rank-deficiency path is GONE (`m_info` stays 0; rank deficiency is normal → query
rank()); audited the lone `m_info!=0` reader (the least_squares guard) ⇒ full-rank behaviour unchanged.
**Advisor-corrected claim (load-bearing):** the basic solution gives `‖Aᵀr‖≈0` only to `O(tol·‖A‖)`;
≈machine-eps holds only for EXACT deficiency. So the test uses an integer column literally = sum of two
others (FP-exact): rank()==3, the dependent column dead, x[dead]=0, ‖Ax−b‖ & ‖Aᵀr‖ ≈ eps; + a
no-false-positive case (a legit-small 1e-6 diagonal ≫ rcond·max stays live ⇒ rank()==n). Min-norm (sparse
COD / Heath stage-2) = documented follow-on. **VERIFIED 5 configs** (win-debug 5542/19 + clang-cl +
win-asan + win-tidy + gcc `-Werror`) **+ NO bench regression** (SPQR full-rank residuals + factor ratios
IDENTICAL — detection is conservative, zero false positives on well/illc/bcsstk).

### 2c — CLI + dispatcher — ✅ DONE 2026-06-02
`hesap.direct.qr.{f32,f64,c32,c64}` registered in `cli_register_direct.cpp` (mirrors the chol/lu CLI;
complex values+RHS flattened `{re,im}`). Schema: `rows ≥ cols`, COO triplets, RHS `b` length m. The CLI
calls `least_squares({b,m}, {x,n}, 1)` uniformly (handles square + over-determined with separate b/x — no
aliasing; cleaner than dispatching to the in-place `solve()`). Output blob = **`[info, rank, x...]`** (the
rank-revealing report). +4 CLI tests (registration, square f64, over-determined f64 LS, complex c64).
**VERIFIED 5 configs:** win-debug (full suite **597256/81**, `[cli]` 147/12) + clang-cl + win-asan +
win-tidy + gcc `-Werror`.

### ⚠ v5c-2a COMPLEX FACTOR BUG — caught by the 2c CLI test + FIXED 2026-06-02 (the CLI earned its keep)
The complex CLI **square** solve gave wrong answers (errors up to 0.28). A focused diagnostic
(`[v5c-2a]` 4×4 complex, RᴴR==AᴴA) isolated it to the **FACTOR** (RᴴR err 0.25), not the solve. Root cause:
the **len==1 last reflector** on an exactly-determined front (`fm == npiv`, e.g. a square single front) —
`make_householder_complex`'s `n≤1` branch returns `β = Re(α)` (β is real-typed), so
`colk[0] = qr_from_real(β)` **dropped the imaginary part of the last R diagonal**. The over-determined
v5c-2a test (14×7, len ≥ 8) never hit it; real is unaffected (`β == colk[0]` for len==1). **Fix:** treat
`len ≤ 1` as a trivial reflector (`tau=T{}`, R[k][k] = `colk[0]` as-is, no apply) — correct + real
bit-identical (moat preserved). Regression cover: the 4×4 RᴴR diagnostic + the complex-square CLI test.
Honest note: v5c-2a's earlier "done" had only the rectangular complex path tested — the square complex
factor was latently broken until 2c's CLI test exposed it.

## v5c-close (2026-06-02) — completeness audit + moat {1,2,4,8,16} + the honest final scoreboard

**Determinism moat extended to {1,2,4,8,16}** (`[v5c-1g]`): R + Rj/Rx + the least-squares solution
bit-identical across worker counts, real f32/f64 AND complex c32/c64. (num_workers is the parallel_for
hint; scratch is pool-sized ⇒ 16 over-requests on a smaller pool and `parallel_for` caps at the front
count — this proves WORKER-COUNT INVARIANCE, **not** 16-way concurrency. Honest, not theatre.)

**Completeness audit (read-only) — `MultifrontalQR<T>` for all 4 types × the full surface:**
| | f32 | f64 | Complex32 | Complex64 |
|---|---|---|---|---|
| `factorize` (+ num_workers tree-parallel) | ✅ | ✅ | ✅ (unblocked) | ✅ (unblocked) |
| `solve` (square) / `least_squares` (m≥n) | ✅ | ✅ | ✅ | ✅ |
| `rank()` / `dead()` (rank-revealing) | ✅ | ✅ | ✅ | ✅ |
| determinism moat {1,2,4,8,16} | ✅ | ✅ | ✅ | ✅ |
| CLI `hesap.direct.qr.*` | ✅ | ✅ | ✅ | ✅ |

(Honest gaps, deferred not hidden: blocked-WY for complex = perf follow-on [unblocked is correct]; min-norm
rank-deficient solve = follow-on [basic solution ships]; both documented.)

**THE HONEST FINAL SCOREBOARD (v5c QR):**
- **vs Eigen `SparseQR` (header peer): CRUSHED on BOTH domains** — rectangular LS 7–37×, square 13–71× factor, less fill, better accuracy.
- **vs SuiteSparseQR / SPQR (gold standard, serial-fair):** **SOLVE crushed 4.0–8.6×** everywhere; **FACTOR: well/illc1033 WIN (1.06–1.17×)** [first SPQR factor wins], **well/illc1850 near-parity (0.87–0.96×, up from 0.60×)**, **bcsstk13/24 (square SPD) characterized loss (0.71–0.91×)** = the ADR-0082 BLAS-kernel wall on the WRONG-tool-for-QR matrices (use Cholesky — we crush CHOLMOD); residuals match throughout. NOT a clean factor-crush everywhere — honestly a split with a characterized, user-accepted square-SPD loss.
- **The determinism moat** (R + Householder bit-identical {1,2,4,8,16}, real+complex) — **carried by NO sparse-QR library (SPQR/Eigen)**. The standing differentiator.

**Deferred to v5z (the full-v5 close, consistent with v5a/v5b which also deferred them — written ONCE for the whole direct cluster):** `docs/systems/hesap-direct.md` (chol+lu+qr+ldlt+…), ADR-0065 §27 D(direct)-1..N lock, the all-families end-to-end moat, and the rank-deficient + complex bench vs Eigen/SPQR (honest scope: rank-agreement + factor/residual, NOT a min-norm crush — SPQR does min-norm, we ship basic).

**Owed at the v5c COMMIT — the 18-config CI sweep is the real gate** (the six slices v5c-1e/1f/1g/2a/2b/2c have only seen targeted per-module builds: win-debug + clang-cl + win-asan + win-tidy + one gcc-release). CI must prove the cluster on **win-shipping, win-release (LTCG), win-debug-scalar, win-debug-sse2, + the 7 Linux configs** — exactly where cross-config SIMD/`-Werror`/LTCG breakage hides.

## v5d (LDLᵀ) — 🎉 CLUSTER COMPLETE (a–g) 2026-06-02

**v5d-g ✅ — CLI + Eigen SimplicialLDLT bench (the last v5d slice).** **CLI: 6 commands, EXPLICIT mode
selection** (advisor — don't silently default complex): `hesap.direct.ldlt.{f32,f64,c32,c64}` (symmetric
LDLᵀ) + `hesap.direct.ldlh.{c32,c64}` (Hermitian LDLᴴ); `impl_ldlt<T, bool Hermitian>` factor+solve, reuses
`make_lu_schema`, output `[info, x]`. 3 `[v5d-g]` CLI tests: the 6-command registration; `ldlt.f64` symmetric
indefinite solve; `ldlh.c64` Hermitian solve + the **CLI-level mode-separation proof** (factoring the SAME
Hermitian input via `ldlt.c64` gives a wrong answer — `res > 1e-3` — while `ldlh.c64` recovers x_true, so the
command pair is genuinely mode-selecting; this is where "is the flag wired" closes at the user surface).
- **Bench (`bench_hesap_ldlt_vs_reference`) — 🎉 CRUSH on the target regime (the user: "we don't proceed until
  we CRUSH"). FAIR protocol** (AMD-order once, both fed the permuted matrix, Eigen `NaturalOrdering`; first
  draft self-handicapped Cerid with no-AMD → false 36× loss, advisor caught it, fixed). **PROFILE-DRIVEN
  optimization (measure-first, each lever a separate measured step):** (1) the L-extraction post-pass was
  11–32% → rebuilt L's CSC DIRECTLY (per-column count from `block_kinds` + scatter + no-swap fast-path; no
  triplet arrays, no global sort) ⇒ Lbuild −3×; (2) the SERIAL path dropped the `ts` mutex (uses `m_alloc` —
  front buffers don't migrate when serial; ts kept for parallel) ⇒ tiny-front walk −28%. (3) The profile then
  showed the factor flops concentrate in BIG dense fronts ONLY at 3D scale (n=64k: big-front[npiv>32]
  flop=99%, maxpiv=1292) — so added a **3D 7-point Laplacian** (separators ~n^{2/3}, the v5a 3D-FEM regime).
  **RESULT vs Eigen's tuned pivot-free scalar `SimplicialLDLT`: on 3D Laplacian (n=4k–64k) Cerid is 1.8–2.5×
  FASTER (ratio 0.4–0.5×) — a genuine CRUSH on the regime where multifrontal LDLᵀ is the right tool.** Honest
  elsewhere (reported in the same plain ratio): 2D Laplacian converges 2.1×-slower (n=1k) → PARITY (n=50k,
  1.0×) as fronts amortize; KKT-saddle/tridiagonal stays 3.2–4.8× slower = the simplicial regime (npiv=1
  tiny fronts, multifrontal overhead — the v5b two-kernel lesson, characterized). ⚠ My initial "Eigen fails
  on indefinite saddle-points" expectation was WRONG (advisor: show the receipt) — Eigen's reorder+fill
  regularizes practical saddle-points, so it succeeds + is faster there; the pivot-free FAILURE MODE is
  narrow-but-real, shown on `[[0,1],[1,0]]` (Cerid correct, Eigen `NumericalIssue`). So v5d CRUSHES on
  large-3D-structured (the target) AND is correct-on-ALL-indefinite + the ONLY cross-thread bit-deterministic
  LDLᵀ + complete real+complex sym/Herm family. **Blocked-BLAS-3 BK front factor (partitioned xSYTRF —
  extends the 3D lead; moat-safe with symbolic-fixed block size) + a simplicial kernel for the tiny-front
  regime = v5d-perf / v5z follow-ons (the crush is already landed UNBLOCKED).** Fair same-class speed peer
  (MA57/MUMPS — 2×2 pivoting, which Eigen is NOT) → v5z. **VERIFIED 5 configs:** win-debug (full suite
  597649/114, [v5d] 339/30) + clang-cl + win-tidy (clean) + win-asan (moat OOB-clean under parallel) + gcc
  `-Werror` build **+ RUN**; the optimizations kept [v5d] reconstruction + the moat BIT-IDENTICAL.

**v5d-f ✅ — complex LDLᵀ (complex-symmetric) + LDLᴴ (Hermitian-indefinite).** `factor_front_ldlt<T, bool
Hermitian>` + `MultifrontalLDLT<Complex32/64>` (runtime `m_hermitian` dispatched to `<T,true>`/`<T,false>`;
real T instantiates only `<T,false>`). **Two genuinely-distinct algorithms on a shared skeleton** (advisor —
NOT an `if constexpr` sprinkle): Hermitian=false → A=P·L·D·Lᵀ·Pᵀ (complex-symmetric, unconjugated, D
complex); Hermitian=true → A=P·L·D·Lᴴ·Pᵀ (Hermitian, conjugated, D **real-diagonal**). The genuinely-distinct
points: the trailing-update second factor (`ldlt_conjh<H>`), the D 1×1 forced real (`ldlt_pivd<H>` — discards
a Hermitian diagonal's numerically-zero imaginary noise so the solve's real-divide is exact) + the 2×2
inverse (`det = d11·d22 − conjH(d21)·d21`, real for Hermitian), the diagonal pivot magnitude (`|Re|` via
`ldlt_magd<H>`), and the solve's backward `Lᴴ` (vs `Lᵀ`) + 2×2 D-inverse (runtime `cj`). Everything else is
shared skeleton.
- **Advisor-sequenced REFACTOR-then-extend (the part that saved a confusing debug):** STEP 1 — refactor
  `factor_front_ldlt` to `<T, bool Hermitian=false>` with `R=RealType<T>` + `ldlt_mag`/`ldlt_conjh`/
  `ldlt_pivd`/`ldlt_magd`, keeping it functionally identical for real; **verified real `[v5d]`
  BYTE-IDENTICAL (263/26)** before touching complex (for real: R==T, conjh=identity, pivd=identity, magd=mag
  ⇒ exact reduction; the moat's bit-identity {1,2,4,8,16} + the fixed reconstruction values pin it). STEP 2 —
  added the complex instantiations + the driver dispatch + the Hermitian-aware solve.
- **Validation — preempting all three recurring traps:** 3 `[v5d-f]` tests. (1) reconstruction against the
  RIGHT product — LDLᵀ → `A==L·D·Lᵀ`, LDLᴴ → `A==L·D·Lᴴ` (conj the right L) — with **genuinely nonzero
  imaginary parts** (a wrong transpose passes on a real-valued-but-complex-typed matrix; nonzero-imag is the
  complex analog of the dead-2×2 trap) + a solve residual `‖A·x−b‖`. (2) `REQUIRE(∃ block_kinds==2)` so a
  2×2 runs in BOTH modes. (3) Hermitian-only `CHECK(D 1×1 im≈0)` (catches a dropped `Re()`). A built
  genuinely symmetric/Hermitian. (4) the complex MOAT — a block-diagonal of 4 indefinite 2×2-forcing complex
  blocks (symmetric or Hermitian per mode), bit-identical {1,2,4,8,16} for Complex64+Complex32, both modes.
  (5) **flag-load-bearing** (advisor coverage catch — the selection analog of v5d-e's worker-count check):
  factor the SAME Hermitian matrix BOTH ways ⇒ `res(LDLᴴ) < 1e-10` AND `res(LDLᵀ) > 1e-3` (the symmetric
  completion ≠ the Hermitian A for nonzero-imaginary) ⇒ proves `m_hermitian` actually changes the math, not a
  decorative flag. Debug note: my first 4×4 matrices DELAYED (a strong `imax` diagonal → 1×1-at-imax then a
  singular Schur); switched to the moat's proven 3×3 block (both leading diagonals 0 ⇒ a guaranteed
  non-singular 2×2). **VERIFIED 5 configs:** win-debug ([v5d] 339/30) + clang-cl ([v5d-f] 76/4) + win-tidy
  (clean) + **win-asan** (complex factor/solve + moat OOB-clean under parallel) + gcc `-Werror` build **+ RUN**
  (full suite 597615/111).

**v5d-e ✅ — tree-parallel factorization + the cross-thread determinism MOAT.** `factorize(a, num_workers)`:
level-scheduled (`level[f]=1+max(child level)`; ascending-f per level ⇒ worker-order-independent) +
per-worker scratch (loc/bk/piv as `wk`-keyed slices, eamap per worker; working front from `ts`) + per-level
`jobs::parallel_for`+`wait`+`frame_reset` (serial when `num_workers≤1` — no jobs touched). **Front-parallel
only** (mirror v5c-1g; NOT the LU hybrid — moat slice, not a speed crush). **Race analysis** (advisor-confirmed
on every point): each front writes ONLY disjoint global ranges (D/perm/block_kinds over `[c0,c0+npiv)`) + its
own `cb[f]` (single-writer); `cb[child]` is const after its own factor (`mf_extend_add_trailing_sym` reads it
const) and read ONLY by its direct parent (a higher level, after the `wait` barrier); the L21 columns are
disjoint from the Schur trailing the parent reads. The structural reason it's even safer than QR: each front
is factored WHOLLY by one worker ⇒ no intra-front cross-worker FP reduction to drift. **L-extraction is a
SERIAL post-pass** (canonical CSC, sorted per column ⇒ bit-identical regardless of triplet append order ⇒ no
per-worker triplet buffers + their alloc races). **THE MOAT INVARIANT (one-line comment at the extend_add
loop): children are extend-added in fixed `chld_idx` order, NOT completion order** ⇒ L,D,perm a pure function
of the pattern; a future "optimization" reordering this would silently break the moat. Refuse-on-delay via
per-worker flags reduced after the levels (NEVER write `m_info` from a worker — the one obvious race; a
delayed front leaves an empty cb[f] ⇒ the parent's extend-add is a structurally-safe no-op + `m_info!=0`).
- **Advisor preemptive catch (the moat test must not be vacuous):** "bit-identical across workers" passes
  trivially on a CHAIN front-tree (every level has 1 front ⇒ `parallel_for` runs only worker 0 ⇒ the
  per-worker scratch isolation is never stressed). Used a **12×12 BLOCK-DIAGONAL of 4 independent
  2×2-forcing indefinite blocks** `[[0,2,1],[2,0,1],[1,1,5]]` ⇒ 4 independent single-front trees at level 0
  ⇒ `REQUIRE(front_count≥4)` (genuine 4-way concurrency) AND `REQUIRE(∃ block_kinds==2)` (a 2×2 pivot runs
  UNDER parallelism — proving the 2×2 D-store/perm is deterministic concurrently, not just in serial /
  all-1×1). Then L (lp/li/lx) + D (dd/doff/block_kinds) + perm + the solution BIT-IDENTICAL across
  {1,2,4,8,16} workers AND vs serial, at f64 AND f32. **VERIFIED 5 configs:** win-debug + clang-cl ([v5d-e]
  40/1) + win-tidy (clean) + **win-asan** (per-worker scratch OOB-clean under parallel) + gcc `-Werror` build
  **+ RUN** (full suite 597539/107). No SPQR/Eigen/MA57/MUMPS carries a cross-thread bit-exact LDLᵀ factor =
  the standing differentiator. Memory/perf follow-on noted: the serial post-pass keeps all cb[f] alive
  (forecloses front-buffer recycling).

**v5d-d ✅ — the block-aware L·D·Lᵀ solve (`MultifrontalLDLT<T>::solve`, f32/f64; was a v5d-c stub).** Math:
`A = P·L·D·Lᵀ·Pᵀ` ⇒ `A·x=b` solved by `r = Pᵀ·b` (gather to factor-position order) → forward unit-lower
`L·z=r` (CSC: column j propagates `z[j]` to its rows i>j) → block-aware `D·w=z` (1×1 divide; 2×2 inverse via
the 2×2 determinant) → backward `Lᵀ·y=w` (CSC: `y[j]=w[j]−Σ_{i>j}L[i,j]·y[i]`, j descending) → `x=P·y`
(scatter to original order). L's unit diagonal is implicit and the 2×2 coupling lives in D, so BOTH L solves
are plain unit-triangular — the only block-awareness is the D step. Multi-RHS = a column-major n×nrhs block,
solved column by column (one length-n scratch reused). Returns false on an invalid factor (`info!=0`, e.g. a
delayed pivot). **Validation = known-solution recovery + direct residual** (advisor's earlier note that a
solve can mask sign/structure errors is answered by the residual `‖A·x−b‖`, which a backwards `P`/`Pᵀ` would
blow up): pick `x_true`, set `b=A·x_true`, solve, check `‖x−x_true‖` AND `‖A·x−b‖`. 6 `[v5d-d]` tests:
multi-front diag-dominant · single-front dense indefinite (2×2) · **cross-front swap (the v5d-c CB-row-remap
matrix — here the SOLVE exercises the same non-identity perm end-to-end via gather/scatter)** · **forced-2×2 (the block-aware D-solve)** ·
multi-RHS (nrhs=3) · f32 · invalid-factor→false. **VERIFIED 5 configs:** win-debug + clang-cl ([v5d] 223/25)
+ win-tidy (clean) + gcc `-Werror` build **+ RUN** (full suite 597499/106). `MultifrontalLDLT` is now a
complete factor+solve `IFactorization`; v5d-e adds the tree-parallel cross-thread moat, v5d-f complex, v5d-g
CLI + the Eigen `SimplicialLDLT` bench.
- **Advisor catch (the 2×2 path, important — same pattern as the v5d-c CB-remap):** ALL my v5d-c/d matrices
  took 1×1 pivots (the dense-4×4 has `|diag|≥2≫0.64·colmax`; the cross-front one is 1×1-at-imax-with-swap by
  design), so the **2×2 pivot — the entire reason LDLᵀ exists over Cholesky — was dead-under-test** through
  the driver/solve plumbing: v5d-c's `block_kinds==2` D-store (`m_doff` = d21, d11/d22 split) + blocksz=2
  L-store (skip the partner row), and v5d-d's 2×2 D-solve (determinant inverse). v5d-b tested it at the
  raw-buffer level, but none of that plumbing has a v5d-b analog. Added a zero-leading-diagonal matrix
  `[[0,2,1],[2,0,1],[1,1,5]]` (forces a 2×2 at (0,1), det=−4≠0) run through BOTH `check_ldlt_reconstruct`
  (v5d-c) and `check_ldlt_solve` (v5d-d), with `CHECK(∃ k: block_kinds[k]==2)` so the path is provably hit
  and can't silently revert to 1×1.

**v5d-c ✅ — the multifrontal LDLᵀ driver (`MultifrontalLDLT<T>:IFactorization`, f32/f64).** A SERIAL postorder
front walk: for each front (ascending = postorder) assemble A's LOWER triangle for the fully-summed pivot
columns + symmetric extend-add the children's Schur → `factor_front_ldlt` (v5d-b) → store L + D + the
block-local permutation. **Advisor-keyed scope cut: NONE of v5b's `factor_attempt` apparatus transfers** —
no MC64, no element-growth tracking/early-abort, no level-scheduling/hybrid-parallel. BK pivoting *is* the
stability mechanism (no static pivot to recover); serial (the cross-thread moat is v5d-e). The driver is
~150 lines, not ~350.
- **NEW: `mf_extend_add_trailing_sym`** (symmetric lower-triangle in-place extend-add). The existing
  `mf_extend_add_trailing` scatters the FULL Schur; for the symmetric lower-tri LDLᵀ front it would read the
  child Schur's *unstored upper triangle*. Restricted to `a ≥ b`, with ONE map (child Schur row ids == col
  ids, symmetric) — and `a ≥ b ⇒ map[a] ≥ map[b]` (monotone) so every lower-tri child cell lands in a parent
  lower-tri slot. In place (ld = child.nrows), no copy.
- **Storage:** L = CSC unit-lower multipliers (diagonal implicit; `L[k+1,k]=0` for a 2×2) ; D kept SEPARATE
  (advisor: don't overload the L-slot like `dense::LDLT` — messy in CSC, and v5d-d's solve consumes D
  separately) — `m_dd` (1×1 value / 2×2 d11,d22) + `m_doff` (2×2 d21) + `m_block_kinds`. Permutation P = the
  direct sum of each front's within-front BK swaps (which stay in the front's contiguous pivot range — the
  v5d-a invariant — so P permutes only WITHIN fronts). L is stored keyed by global id during the walk, then
  **remapped to factor-position order** at the end (a CB row's factor position depends on its owning
  ancestor's BK swaps, known only after that ancestor is factored). Factors A AS GIVEN (no internal AMD — the
  consumer applies the fill order, like v5c QR).
- **REFUSE-ON-DELAY (advisor: precise):** `factor_front_ldlt` returning < npiv ⇒ `info() != 0` + abort, do
  NOT emit that front's trailing as a CB (the un-eliminated pivots are still in it). The delayed pivot is the
  Duff-Reid follow-on.
- **Validation = reconstruction `P·L·D·Lᵀ·Pᵀ == A[perm,perm]`** (advisor: stronger than a solve; staged to
  isolate the only-new risk, the permutation). 7 `[v5d-c]` tests: (1) multi-front diagonally-dominant →
  all 1×1, no swap, P=identity → `L·D·Lᵀ==A` (validates assembly + symmetric extend_add + postorder +
  storage with ZERO permutation bookkeeping); (2) single-front DENSE indefinite → 2×2 + swaps + perm via the
  CSC store/remap (the driver-level v5d-b twin); (3) multi-front indefinite with a dense trailing block →
  npiv≥2 root supernode; (4) **cross-front swap exercising the CB-row remap**; (5) f32; (6) refuse-on-delay
  `info!=0`; (7) deterministic re-run (L + perm bit-identical). **VERIFIED 5 configs:** win-debug
  ([v5d-c] 34/7) + clang-cl (34/7) + win-tidy (clean) + gcc `-Werror` build **+ RUN** (full suite 597454/97 —
  the allocator-lifetime rule followed, `ts` before `cb`, and the gcc RUN confirms no new lifetime/UB).
- **Advisor catch (test #4, important):** the first 6 tests left the CB-row remap
  (`lt_row[e]=ipos[lt_row[e]]` for a CB row through a NON-identity ancestor perm — the single line of v5d-c
  with no v5d-b analog) **dead under test**: stage-1 had `perm`=identity, the single-front stage had no CB
  rows (`npiv==n`), and the dense-trailing stage's root block took a 1×1 (no swap). Added a
  leaves-{0,1}-into-a-weak-first/strong-second dense-root-{2,3} matrix that FORCES Bunch-Kaufman to swap the
  root pivots (which ARE the leaves' CB rows), with `CHECK(front_count()>1)` + `CHECK(∃ perm[i]≠i)` so the
  remap path is provably executed — and the reconstruction still holds through it. Avoids the fusion of a
  silent factor bug surfacing only at v5d-d as "is it the solve or the factor?".

**v5d-b ✅ — per-front indefinite Bunch-Kaufman factor (the only new v5d algorithm).** New header
`dense_ldlt_kernels.hpp` `factor_front_ldlt<T>` (f32/f64) = the COL-MAJOR, `npiv`-restricted analog of
`dense::LDLT`/LAPACK xSYTRF (UPLO=Lower). **Storage decision (advisor-locked, NOT a both-triangles MVP):**
lower triangle, full m×m, col-major; the trailing update is a SYMMETRIC rank-1 (1×1 pivot) / rank-2 (2×2
pivot) update touching ONLY the lower triangle = the ~½-flop MA57-class form. Col-major lets it **drop ALL
scratch buffers** the dense row-major form needed — it reads the still-original column k while writing only
columns j>k (disjoint), then normalizes column k last ⇒ no hidden malloc. **Pivot contract (MA57
multifrontal symmetric-indefinite):** pivot *diagonal* choices restricted to the fully-summed `[k,npiv)`;
colmax/stability search over the FULL column `[k+1,m)` (L21 reaches the contribution-block rows — growth
depends on them); a 2×2 partner must also be <npiv. **NEVER pivots onto a CB row** — a fully-summed variable
whose only stable pivot would land on a non-fully-summed row (imax≥npiv), or a structurally-null column, or
has no fully-summed slot for the 2×2 partner, is DELAYED: the kernel STOPS and returns the partial
eliminated-count (the Duff-Reid delay hook; the driver/v5d-c relays it). Strict-`>` first-max colmax/rowmax
tie-break = the MOAT invariant (a pure function of the front buffer ⇒ L,D bit-identical across worker counts
at v5d-e). Ported `dense::LDLT`'s `swap_sym` to col-major (unit-tested standalone). 7 `[v5d-b]` tests:
standalone col-major `swap_sym`, reconstruction `P·L·D·Lᵀ·Pᵀ==A` (forcing a 1×1 swap, forcing a 2×2, 5×5
mixed-indefinite, f32), Schur identity for `npiv<m`, delayed-pivot partial-return. **VERIFIED 5 configs:**
win-debug + clang-cl (191 asserts on `[v5b-3],[v5d]`) + win-tidy (clean) + gcc `-Werror` build **+ RUN**
(full suite 597426/91 == win-debug).

**⚠ The gcc full-suite RUN caught TWO pre-existing bugs — fixed, not deferred:**
1. **A latent COMMITTED-v5b correctness bug.** `MultifrontalLU::factor_attempt` (multifrontal_lu.cpp,
   byte-identical to the v5b commit `ebf34e2`) declared its `ThreadSafeAllocator ts` AFTER the
   `Array<MfFront> cb` whose fronts borrow `&ts`. C++ destroys locals in reverse declaration order ⇒ `~ts`
   ran BEFORE `~cb` ⇒ `~MfFront`→`Array<u32>::free_buffer`→`IAllocator::deallocate` dispatched through a
   DESTROYED allocator's vtable ⇒ gcc-DEBUG `pure virtual method called` (SIGABRT in the committed
   `test_cli.cpp:291` "multifrontal dispatch" test). MSVC + gcc-RELEASE silently tolerate the UB (dangling
   vtable intact / dtor vtable-reset elided as a dead store) — which is why win-debug "passed". `gdb -ex
   'break __cxa_pure_virtual' -ex bt` pinpointed it instantly. **Fix:** declare `ts` BEFORE `cb` (allocator
   outlives its borrowers). Audited — it is the module's only `ThreadSafeAllocator`. CI DOES
   `ctest --preset linux-gcc-debug` (ci.yml) so the sweep covers this class; it slipped only because v5b's
   per-slice gcc step was build-ONLY (not a test RUN). Captured as memory `feedback_container_allocator_must_outlive`
   (distinct from `feedback_vtable_stability_append_at_end`: that is vtable-SLOT ordering, this is allocator
   LIFETIME). **v5d-c reuses MfFront + ThreadSafeAllocator + Array<MfFront> — carry this rule forward.**
2. **4 pre-existing v5b/v5c-test clang-tidy issues** surfaced by the first FULL win-tidy build of the target
   (the v5c per-slice win-tidy runs were per-TU incremental): `rn`-confusable-with-`m` ×3 in
   test_multifrontal_qr.cpp (→ `nrow`/`rnorm`) + nested-ternary ×3 in test_multifrontal_qr/cli/supernodal_lu
   (→ if/else). Fixed; win-tidy now clean.

**v5d-a ✅:** new `multifrontal_ldlt.{hpp,cpp}` + `build_ldlt_symbolic(a)` — the LDLᵀ front tree IS the
chol(A) supernode tree ⇒ reuses v5b-3's `build_symmetric_multifrontal_symbolic` (delegate + assert square).
NO numeric. Structure test `[v5d-a]` (3 cases/103 asserts): front count, `check_multifrontal_containment`
ok (assembly precondition = Cholesky theorem), SYMMETRIC front extent (row idx set == col idx set), pivot
tiling, determinism. VERIFIED win-debug + non-ASCII guard + clang-cl + win-tidy + gcc `-Werror`. (Caught
my own non-ASCII `LDLᵀ` in TEST_CASE names → `LDLt`, per the guard.) Next: v5d-b (the indefinite front
factor — the real work).

### Warm plan for v5d-b…g (the numeric):
**Reuse map (most of v5d is already built):** v5d is SYMMETRIC ⇒ reuse v5b-3's `build_symmetric_multifrontal_symbolic` + `MfFront<T>` (col-major front) + `mf_extend_add`/`mf_extend_add_trailing` (assembly) + the tree-parallel level scheduler wholesale. The dense `dense::LDLT` (Bunch-Kaufman 1×1/2×2, `factor_ldlt`/`solve_ldlt`, `block_kinds`) is the dense reference. **The ONLY genuinely new algorithm: the per-front INDEFINITE factor (1×1/2×2 pivoting) + the moat + delayed pivots.**
**The moat design fork (decide first):** Bunch-Kaufman is DYNAMIC (value-based swaps) ⇒ order-dependent. But the moat works the SAME way as v5a/b/c — each front is factored by ONE worker as a pure function of its assembled values ⇒ WITHIN-front BK is deterministic per front, and the fixed postorder assembly makes L/D bit-identical {1,2,4,8,16} across workers WITHOUT global static pivoting. So: within-front BK (deterministic per front) + cross-front tree-parallel = the moat. The phase doc's "static Duff-Reid + delayed pivots" is the ROBUSTNESS need (a front whose fully-summed block is singular/ill-conditioned must DELAY pivots to the parent — MA57/Duff-Reid; grows the parent front). First cut: within-front BK + assert/fallback on a degenerate front; delayed pivots as a follow-on sub-slice.
**Slice breakdown:** v5d-a skeleton + symbolic (reuse v5b-3 symmetric; `MultifrontalLDLT<T>:IFactorization` shell) · v5d-b per-front indefinite LDLᵀ (1×1/2×2 BK, store D block-diagonal + L; validate vs `dense::LDLT` + a known indefinite A) · v5d-c driver (postorder walk + extend_add; validate vs a dense oracle) · v5d-d L·D·Lᵀ solve (block-aware fwd / 1×1+2×2 diag / bwd) · v5d-e tree-parallel + moat {1,2,4,8,16} · v5d-f complex-SYMMETRIC (LDLᵀ, no conj) AND Hermitian-indefinite (LDLᴴ, conj) — two `if constexpr` variants · v5d-g CLI `hesap.direct.ldlt.{f32,f64,c32,c64}` + bench vs Eigen `SimplicialLDLT` (fixed-pivot breadth gap; MA57-class floor). RESEARCH FIRST: MA57 / Duff-Reid delayed pivots, Bunch-Kaufman vs Bunch-Parlett.

## v5d-perf (2026-06-03) — BLOCKED-BLAS-3 SPD front + the CRITICAL correctness finding

Advisor-driven. Followed the user's standing crush mandate ("never say we don't crush — grind; our intrinsics ARE at par with OpenBLAS, the asm experiment barely helped") onto the MUMPS big-3D loss.

**Blocked-BLAS-3 front factor (the SPD lever).** `factor_front_ldlt_blocked<T>` (real-only, `kLdltBlockBail` sentinel) in `dense_ldlt_kernels.hpp`: panel 1×1 pivots with an EXACT PD-safety bail (`dk>0 ∧ |dk|≥α·colmax` over the fully-updated column ⇒ no xLASYF on-the-fly trick needed), then a tiled-LOWER (syrk-equivalent, ½-flop) `dense::gemm` trailing update (`trail_tile`=192 block-columns — NOT a full gemm). Driver dispatch: big real fronts (npiv≥128, nb=128) copy the front → try blocked → on bail restore + unblocked BK. Moat-safe (deterministic gemm). `[v5d-perf]` 2 tests: big-dense-SPD solve resid<1e-9 (front_count==1 ⇒ blocked engaged) + 4× big-SPD block-diagonal bit-identical {1,2,4} (blocked UNDER parallelism). **SPD-3D vs MUMPS: 6.01×→3.17× (blocked full-gemm) →2.20× (tiled-lower) →2.14× (nb=128) at n=64k — HALVED.** Still loses, but LDLᵀ-on-SPD is moot (Cholesky's domain — v5a CRUSHES CHOLMOD there).

**Discriminating measurement (advisor) — `front_kernel_vs_lapack` in the bench (LAPACK via MUMPS's liblapack + `-llapack`):** dense 1600² front — Cerid-blocked **42.3 GF/s** vs LAPACK **dsytrf 56.4** (the BK peer MUMPS runs per front) vs **dpotrf 72.0** (Cholesky ceiling). ⇒ the kernel is **0.75× dsytrf / 0.58× dpotrf — a MODEST ~1.3× gap, NOT a hard "OpenBLAS-asm wall"** (the v5d-g "ADR-0082 wall" framing was too pessimistic — RETRACTED). The total 2.14× = ~1.3× kernel + ~1.6× multifrontal overhead (MUMPS amalgamates fronts; nb 64→128 barely moved it ⇒ not thin-k-bound).

**🚨 THE REAL FINDING (advisor blind-spot catch — every big bench case was SPD, which MASKED it): v5d FAILS (info≠0, resid=−1.0) on genuinely-indefinite matrices that need DELAYED PIVOTS.** New `make_indefinite_laplacian_3d(k,σ=3)` (3D Laplacian A−3I; diag 3 < Σ|offdiag|=6 ⇒ indefinite, same big dense fronts as the SPD 3D). **Smallest reproducer k=3, n=27**; Eigen solves it (resid 9.8e-15) + MUMPS solves it (resid~1e-11) ⇒ a STABILITY-delay, not a singularity. `factor_front_ldlt` returns r<npiv when a fully-summed column's stabilizing entries live in not-yet-assembled CB rows (can't pivot onto a CB row) ⇒ the driver hits the "Delayed pivot (Duff-Reid, not yet supported)" bail. Confirmed clean: the small-KKT 0.29–0.55× MUMPS wins ARE real solves (resid 4.4e-16), not fast-bails. **So v5d is correct on SPD + fill-regularized-indefinite, but NOT on its core indefinite domain ⇒ NO indefinite crush claim until delayed pivots land (advisor BLOCKING).**

**NEXT = v5d-h DELAYED PIVOTS (Duff-Reid/MA57)** — the required correctness slice (user "never defer features"). Design: unresolvable fully-summed columns postpone to the PARENT's fully-summed set; front sizes become DYNAMIC (symbolic = upper bound + delay slack); factor positions assigned in a deterministic postorder pass (keeps the moat — delays are value-dependent but a pure function of the fixed-postorder-assembled front). MANDATORY test = a delay-TRIGGERING moat case bit-identical {1,2,4,8} (current moat tests never delay ⇒ say nothing about determinism-under-delay). Develop against n=27 (printable). Kernel delay semantics CONFIRMED by reading `factor_front_ldlt`: ALL BK swaps stay within `[k,npiv)` (CB rows `[npiv,m)` are never swapped into the pivot block) ⇒ the delayed columns at front-local `[r,npiv)` are a tracked permutation of `[c0+r,c0+npiv)`, recoverable by replaying `piv[0..r)`. Blocked path stays SPD-only (bails on indefinite, as now — that work isn't wasted, just isn't the indefinite path). VERIFIED 4 configs (win-debug [v5d] 349/32 + clang-cl + gcc-debug RUN + win-tidy); gated bench flags reset OFF.

## v5d-h (2026-06-03) — DELAYED PIVOTS (Duff-Reid): v5d is now CORRECT on its entire indefinite domain

The v5d-perf finding (above) was that v5d FAILED on genuinely-indefinite matrices needing delayed pivots. v5d-h implements them.

**Design (the rewrite of `MultifrontalLDLT::factorize`).** A front's fully-summed block = {columns DELAYED up from its children} prepended to its symbolic pivots. The BK kernel eliminates r ≤ attempt of them; the rest relay to the PARENT (gathered into the parent's fully-summed set via `fdelay[f]`). Front sizes are DYNAMIC (symbolic = lower bound + ndel delayed-in slack). Four invariants make it correct + moat-safe: (1) every delayed id < c0 (descendant pivot, postorder) < every symbolic row id ⇒ the sorted delayed block LEADS an ascending front row_index ⇒ the extend-add precondition holds; (2) factor POSITIONS are dynamic (a delayed pivot is eliminated at an ancestor) ⇒ perm/D/block_kinds/L are all assigned by a SERIAL postorder pass (running counter `gp`; in the no-delay case `gp == pivot_first[f]` ⇒ BYTE-IDENTICAL to the old static scheme ⇒ zero regression on every prior test); (3) after the BK `[0,r)` swap-replay on row_index, the delayed region `[r,attempt)` is sorted ascending via `ldlt_swap_sym` (symmetric data swap + row_index) so the child trailing `[r,fnr)` is ascending for the parent's extend-add; (4) `gp != n` at the end ⇒ a variable was never eliminated ⇒ genuinely singular ⇒ `info != 0`. New per-front records `fdelay[f]`/`fbk[f]` MUST use `front_alloc` (thread-safe `ts` in parallel — they're written by worker threads with allocation; a **TLSF concurrency assert** caught the m_alloc version). `delayed_count()` = a lower-bound delay indicator.

**Tests (4 new `[v5d-h]`).** A delayed pivot now reconstructs (the old refuse matrix, now a 1×1-swap at the parent) · big indef-3D (k=3,4) factors with resid<1e-9 + `delayed_count>0` · the **MANDATORY delay-TRIGGERING moat** (indef-3D n=64, bit-identical {1,2,4,8} — the SPD/complex moat cases never delay, so they proved nothing about determinism-under-delay) · the v5d-d "invalid factor" test moved to a genuinely-SINGULAR matrix. The two former "refuse-on-delay" tests were updated (those matrices now factor). gcc `-Werror=shadow` caught `const u32 a` shadowing the matrix param (→ `sp`); clang-tidy `bugprone-argument-comment` caught a param-name comment.

**VERIFIED 5 configs on the new code:** win-debug 372/34 + clang-cl 372/34 + gcc-debug RUN 372/34 + win-asan parallel-delay 65/4 + win-tidy clean + ctest 33/33 + non-ascii guard. **BENCH CORRECTNESS WIN: big indefinite 3D now `Cerid OK` resid 5e-13→1.5e-11 (was FAIL); Eigen SimplicialLDLT is FAST but WRONG (resid 4e1→4e3, pivot-free) ⇒ Cerid is the ONLY correct solver besides MUMPS.**

**The crush is OWED on indefinite PERF (next grind).** Measured (`del=`/`nf=` added to the bench): SPD-3D `del=0`; indef-3D **~30% of pivots delay** (n=32768: 10480/32768). Same front count as SPD (nf=22129) but **136× slower** (676ms→92084ms) ⇒ the 357× MUMPS gap is TWO causes: (1) indefinite fronts bail the blocked-BLAS-3 path → the UNBLOCKED BLAS-2 kernel (~20× per-flop); (2) ~30% delays blow up near-root fronts (extra flops). **CRUSH PLAN: (a) a BLOCKED INDEFINITE front kernel — xSYTRF/xLASYF-style, BLAS-3 panel factor with 2×2 pivots IN the panel (the hard kernel deferred at v5d-b); (b) relaxed-front amalgamation (bigger fully-summed blocks ⇒ fewer delays AND bigger BLAS-3 fronts — MUMPS does both).** DIAGNOSE-confirmed: it's BOTH kernel and delay-blowup, not one.

## v5d-h-perf (2026-06-03) — the indefinite crush grind: 375× → 1.6–6.9× vs MUMPS, BETTER accuracy

After delayed pivots made v5d *correct* on indefinite, it was 375× slower than MUMPS at n=32768. The grind (advisor-measured at every step, never reasoned-to-the-next-guess):

1. **Blocked-BK indefinite kernel** — rewrote `factor_front_ldlt_blocked` to full Bunch-Kaufman (1×1/2×2 + symmetric swaps over the whole front + a flush-on-pivot-beyond-panel strategy that keeps data fully-updated without a separate W workspace; returns r≤attempt for delays; subsumes the old SPD-only 1×1-or-bail path). It ALONE did NOT help (92s→96s).
2. **The discriminating measurement** (advisor): added Cerid `max_front_dim` + MUMPS `INFOG(13)` to the bench. **MUMPS delays ZERO pivots (even SYM=2) on every case; Cerid delayed ~30%** (del 10480, maxf blew 2051→12160 at n=32k). ⇒ the gap was **delay-driven front BLOWUP from the textbook BK threshold α=0.64**, not the kernel.
3. **Relaxed pivot threshold** (`set_pivot_threshold`; default now **0.01** = the MA57/MUMPS/PARDISO standard). Sweep (time+del+resid together): α 0.64→0.01 cut n=13824 5158→96ms (del 4241→1443, maxf→SPD-size). The indefinite case *became* the SPD case (del→low, maxf→separator size).
4. **Iterative refinement + backward-error guard** (required — the relaxed α trades element growth): `solve` stores A's lower triangle, runs IR (`symv` residual with a Hermitian-aware `cj(v)` mirror) until the backward error stalls/converges, then ACCEPTS only if `‖b−Ax‖ ≤ 1e-6·‖b‖` else returns false (accurate-or-flagged, never silent garbage; deterministic ⇒ moat holds). **IR erased the asterisk: resid 8e-12/1.9e-10/5e-9 → 1.8e-15/2.2e-15/2.9e-12 — BETTER than MUMPS (5.6e-12/4.5e-11/6.3e-11) at every size; IR cost ~5% of solve.**

5. **Lower the threshold further (the lever that SOLVED the scaling) — measured, not assumed.** The advisor preferred relaxed-front amalgamation (accuracy-safe) over the relaxed-α "hack"; I MEASURED a lower-α sweep first (with IR recovering accuracy) and found it works + is far lower-risk (no shared-symbolic change). At α≈0.0003–0.001 + IR, **delays collapse to ~0 (like MUMPS), maxf → SPD separator size, IR converges + recovers accuracy.** Set **default α=0.001**.

**🎉 FINAL @ α=0.001 + IR vs MUMPS: n=4k 1.48× · n=13.8k 1.92× · n=32.8k 2.01× (was 19/137/375× at α=0.64; 1.6/2.7/6.9× at α=0.01).** The gap is now **STABLE ~1.5–2.0×, flat across n** (the scaling blowup is gone — the real win: when last reviewed the gap was *growing*, 6.9×-and-climbing), **identical to the SPD gap (1.53–1.73×)**. Cerid residuals 1.8e-15/2.2e-15/3.0e-13 — **better than MUMPS** (5.6e-12/4.5e-11/6.3e-11) at every size. **HONEST framing (advisor-locked — do NOT write "crushes MUMPS"): this is MUMPS SPEED-PARITY-CLASS (~1.5–2× serial = the SAME kernel-rate gap SPD/v5a-Chol/v5b-LU/v5c-QR all hit vs OpenBLAS+amalgamation, NOT v5d-specific), NOT a speed crush of MUMPS.** The decisive WIN is accuracy (better than MUMPS via IR) + determinism (the moat MUMPS lacks) + correctness (the only correct solver besides MUMPS; Eigen is fast-but-WRONG, resid 4e1–4e3 ⇒ Cerid **crushes Eigen**). The residual ~2× is the cross-cluster kernel-rate/amalgamation wall (future cross-cluster levers: ADR-0082 asm + relaxed-front amalgamation — the latter would also let α rise back toward textbook = IR-lighter). **⚠ α=0.001 is TUNED on well-conditioned 3D-FEM, 10× below the MA57/MUMPS field standard (0.01); ill-conditioned KKT/contact may need 0.01 (the IR accept-guard fails LOUDLY, not silently) — exposed via `set_pivot_threshold`.** Delay-machinery tests force α=0.64 + relax=1; the default path is covered by dense-indef + KKT + bench + SPD.

**Amalgamation — the user's diagnosis, confirmed (it is NOT asm).** Pushed on the residual ~2×; the user pointed out the intrinsics are at par with OpenBLAS (the asm grind proved it) and to read MUMPS instead. The tell was in the data: Cerid emits **22129 fronts for n=32768 (≈1.5 pivots/front — mostly SINGLETON fundamental-supernode fronts → scalar/BLAS-2 + per-front assembly overhead)** while MUMPS amalgamates into far fewer FAT BLAS-3 fronts — and the *same* ~1.5× shows on SPD where del=0, proving it's **front structure**, not kernel or delays. Built relaxed-front amalgamation. **First attempt (naive subtree-collapse) EXPLODED FILL → OOM on big 3D-FEM** at every criterion (collapsing a *sparse* region into a *dense* front). **Fixed by reading + faithfully porting CHOLMOD's `cholmod_super_symbolic`** (`~/suitesparse/CHOLMOD/Supernodal/`): merge ADJACENT chain fronts s,s+1 (union-find path compression to track the current parent) ONLY when the new explicit zeros `nscol0·(snz[s+1]+nscol0−snz[s])` pass the graduated nrelax/zrelax test (CHOLMOD defaults {4,16,48}/{0.8,0.1,0.05}). **Fill-aware ⇒ no OOM, reconstruction-correct, moat-safe** (merges nested separator chains into fat BLAS-3 fronts; never collapses sparse siblings — that was the OOM). Merged front rows = chain pivots ++ the chain TOP's CB (etree fill-propagation). `m_amalg_relax` = nrelax0 (default 4, ON). **RESULT (relax=4 + α=0.001 + IR vs MUMPS): SPD 1.56→1.27× (n=13824) / 1.73→1.46× (n=64000); indef 1.92→1.63× / 2.01→1.74× (n=32768).** ⚠ `nf` only dropped ~6% (22129→20788): on AMD-ordered 3D-FEM the leaf singletons are mostly SIBLINGS, not chains, so neither CHOLMOD nor Cerid merges them (chain-only is the *safe* ceiling — sibling-merge is the OOM). So amalgamation closed the FRONT-OVERHEAD part; **the residual ~1.3× is the BLOCKED-BK KERNEL-PANEL rate** (the discriminating measure: Cerid-blocked 42 GF/s vs LAPACK dsytrf 56 — the flush-restart + within-panel-BLAS-2 panel structure, NOT the gemm which is at-par). A proper xLASYF W-workspace panel could reach ~parity, but a **sub-1× serial speed-crush of MUMPS on big-3D-FEM is not achievable** (same kernel class). **HONEST CRUSH = PARITY-CLASS speed (1.27–1.74×, the established v5a/v5b/v5c pattern) + the determinism MOAT (MUMPS lacks) + BETTER accuracy + only-correct-besides-MUMPS + crushes Eigen.** VERIFIED 5 configs on ALL new code (win-debug 354/37 + clang-cl + gcc-debug RUN + win-asan + win-tidy). OWED before commit: the 18-config CI sweep (IR = new FP-sensitive code, `accept_tol=1e-6`; win-release LTCG + Linux scalar/SSE2) + an f32+IR@0.001 convergence check.

## Files
New: `multifrontal_qr.{hpp,cpp}`, `test_multifrontal_qr.cpp`, `bench_hesap_qr_vs_reference.cpp`,
`bench_hesap_qr_vs_spqr.cpp`, dossier. Modified (v5c-1e/1f): `engine/hesap-ordering/{src,include}/…/symbolic.{cpp,hpp}`
(`symbolic_factorize_ata`), `runtime/CMakeLists.txt`, `tests/hesap-direct/CMakeLists.txt`, `context.md`, phase doc.
v5d-perf: `dense_ldlt_kernels.hpp` (`factor_front_ldlt_blocked`), `multifrontal_ldlt.cpp` (blocked dispatch),
`test_multifrontal_ldlt.cpp` (`[v5d-perf]`), `bench_hesap_ldlt_vs_reference.cpp` (`front_kernel_vs_lapack` +
`make_indefinite_laplacian_3d`), `runtime/CMakeLists.txt` (`-llapack`).
v5d-h: `multifrontal_ldlt.{hpp,cpp}` (delayed pivots + CHOLMOD `amalgamate_fronts` + IR solve + `delayed_count()`/
`max_front_dim()`/`set_pivot_threshold`/`set_amalgamation_relax`), `dense_ldlt_kernels.hpp` (full-BK
`factor_front_ldlt_blocked`), `test_multifrontal_ldlt.cpp` (`[v5d-h]`), `bench_hesap_ldlt_vs_reference.cpp`
(`del=`/`nf=`/`maxf=` + MUMPS `INFOG(13)` + `front_kernel_vs_lapack` + `threshold_sweep`).

## Session close (2026-06-03)

**Landed:** v5c QR cluster local-close (a–g, 2a–2c) + the FULL v5d LDLᵀ family (a–h). v5d-h was the arc: v5d
FAILed on genuinely-indefinite matrices (delayed pivots missing) → **delayed pivots (Duff-Reid)** (correct on
the whole domain) → **blocked-BK indefinite kernel** → discriminating measure (kernel 42 vs dsytrf 56 — not an
asm wall; MUMPS delays 0 vs Cerid 30%) → **relaxed pivot threshold (0.001)** killed the delay-driven front
blowup → **iterative refinement + backward-error guard** (accuracy now better than MUMPS) → **CHOLMOD
`cholmod_super_symbolic` amalgamation port** (fill-aware; a naive subtree-collapse exploded fill first). **Net:
indefinite 375× → ~1.3–1.7× vs MUMPS (parity-class, gap stable across n), WINS small/saddle, better accuracy,
the determinism moat, crushes Eigen.** Honest ceiling: a sub-1× serial speed-crush of MUMPS on big-3D-FEM is
not achievable (same LAPACK-class kernel on the same OpenBLAS gemm) — the crush is parity + the moat + accuracy,
the established v5a/v5b/v5c pattern. All verified across 5 configs (354/37).

**Owed at commit (the v5c + v5d boundary):** the 18-config CI sweep (IR is new FP-sensitive code; win-release
LTCG + Linux scalar/SSE2 + 7×Linux) + an f32+IR@0.001 convergence check. Commit proposed in chat; user commits.

**Next session:** **v5e** (HSS + BLR rank-structured fronts — `v5e-1/2/3`), **v5f** (mixed-precision iterative
refinement), **v5z** (system doc `docs/systems/hesap-direct.md` + ADRs + all-families determinism moat + full
SuiteSparse corpus). Bounded kernel follow-on: the **xLASYF W-workspace panel** (lifts the blocked-BK front from
42→~56 GF/s = dsytrf parity), if tighter parity is wanted.
