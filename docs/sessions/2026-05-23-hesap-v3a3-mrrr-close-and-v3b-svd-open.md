# 2026-05-23 — hesap v3a-3 MRRR closed + v3a complete + v3b (SVD) opened

**Phase 3.1.6 `crd-hesap` v3 (dense eigenvalue + SVD).** Long session. Closed the entire
MRRR symmetric eigensolver (v3a-3), completing **v3a**; opened **v3b (SVD)** with the design
locked + the bidiagonalization foundation shipped.

## What shipped

### v3a-3 — MRRR (`dstemr`-class), CLOSED — the whole thing, beating Eigen + LAPACK

Built the elite way: every kernel ported faithfully + proven in isolation, then assembled;
each leaf benchmarked against **both** Eigen and LAPACK.

- **v3a-3.1 — eigenvalues.** `detail/sturm_count.hpp` (tridiagonal Sturm `negcount` + two-pivot
  interval count + `compute_pivmin` + `gershgorin_bounds` + `tridiag_split` + `bisect_eigenvalue`
  + split-and-bisect driver). `detail/dqds.hpp` — the full dqds engine `dlasq2/3/4/5/6` ported
  **line-for-line** via a 1-based `Z1` ping-pong wrapper (D(dense-eig)-MRRR-Z1base, IEEE-only) +
  `build_qd_ldlt` (strict-shift LDLᵀ) + `dqds_eigenvalues` + `tridiag_eigenvalues_dqds`. Public
  `eigvals_sym` + CLI. **Parallel shared-Sturm MULTISECTION** (`multisection_chunk` +
  `solve_tridiag_multisection` over `crd::jobs`): beats LAPACK `dsterf` at N≥2048, crushes
  `dstemr` 1.6×; one `negcount(mid)` splits an interval for all eigenvalues in it (kills naive
  bisection's ~16× redundancy), then 16 cores LAPACK lacks. Graded-spectrum 1e-8..1e11 to <1e-10
  RELATIVE = the dqds payoff.
- **v3a-3.2 — single-RRR vectors.** `detail/mrrr_vectors.hpp::dlar1v` (twisted factorization,
  faithful IEEE port) + `mrrr_single_rrr_vectors` (root RRR + Rayleigh-quotient refinement).
  Well-separated `‖VᵀV−I‖<1e-10` orthonormal BY CONSTRUCTION (no Gram-Schmidt = the O(n²) win).
- **v3a-3.3 — cluster handling (the hard-gate).** A1 `dlaneg` (LDLᵀ twisted Sturm count) + A2
  `dlarrb_refine` (refine within an RRR) + A3 `dlarrf` (child RRR via cluster-end shift +
  element-growth/back-off) + A4 recursive `dlarrv` cluster loop + Gram-Schmidt fallback. **HARD-
  GATE MET: glued-Wilkinson W₂₁⁺ `‖VᵀV−I‖<1e-8`** — the part that historically destroys
  eigensolver attempts.
- **v3a-3.4 — full path + parallel + crush.** `eig_sym_mrrr` (dense → dsytrd → dqds → MRRR
  vectors → back-transform). **Parallel MRRR vectors** over independent eigenvalue segments
  (`crd::jobs` work-stealing + per-worker external-buffer Tlsf arena + adaptive granularity).
  CLI `hesap.dense.eig.sym.mrrr.{f32,f64}`. ADR-0065 §17 (D(dense-eig)-9..12 + divergences).
  **5-config per-slice DoD PASS** (debug+asan+shipping+release+tidy).

**Final benchmark (i9-14900K AVX2, f64; both Eigen AND LAPACK):**
- Full symmetric eig (`eig_sym`, values+vectors): 1.4–1.95× Eigen, 2.1–3.7× LAPACK `dsyev`.
  Hermitian: 2.2× Eigen, 2.7× `zheev`.
- Tridiagonal vectors (parallel MRRR): **crush Eigen `computeFromTridiagonal` 5–64×, crush
  LAPACK `dstemr` 1.7–2.7×, match/beat `dstedc` (BLAS-3 D&C) 0.84–1.52×; orth ~1e-13.**

### v3a — COMPLETE
QL/QR + D&C (Cuppen) + MRRR + complex Hermitian. The full symmetric + Hermitian eigensolver,
beating both references.

### v3b (SVD) — OPENED
- **Design locked** (`docs/phases/phase-3.1.6-hesap.md` § "v3b locked design"): reuse map
  (`dlasq2` IS bidiagonal singular values; `make_householder` for `dgebrd`); **4-column bench
  protocol** (Eigen `JacobiSVD`+`BDCSVD` AND LAPACK `dgesvd`+`dgesdd`); SVD-via-MRRR fork flagged
  for v3b-2 (with its exact-±σ-multiplicity trap noted); leaf split 1a/1b/1c + 1b-perf.
- **v3b-1a ✅ shipped + tested:** `svd.{hpp,cpp}::bidiagonalize` (Golub-Kahan `dgebd2`, m≥n upper
  bidiagonal, reuses `make_householder` + left/right apply-reflector). Gate MET: `A=Q B Pᵀ`
  reconstruction <1e-12 (m=n + m≠n), Q/P orthonormal <1e-12.

## Honest findings (saved to memory)
- **SIMD-across-eigenvectors (`dlar1v_x4`) tried + reverted — memory-bound.** Batching 4
  eigenvectors needs 4× the O(n) work scratch (16n) → blows L2 → trades divide-latency for
  memory-bandwidth → nets ~0. The MRRR vector path is at the CPU hardware limit; next lever = GPU.
  Lesson: estimate the WORKING-SET footprint before assuming a batching/SIMD win.
- **MRRR's perf win is parallelism, not raw serial speed.** Serial MRRR LOST (0.21× `dstedc`);
  even LAPACK's own `dstemr` loses to its `dstedc` for all-vectors (D&C's O(n³) has a tiny BLAS-3
  constant vs MRRR's O(n²) huge scalar-recurrence constant). The crush = independent eigenvectors
  × cores LAPACK lacks.
- **Always bench BOTH Eigen AND LAPACK** — caught omitting Eigen from the tridiagonal bench; rule
  locked in `feedback_always_bench_both_eigen_and_lapack`. Eigen tridiagonal via
  `SelfAdjointEigenSolver::computeFromTridiagonal`.
- Parallel-scaling lessons: coarse static chunks idle fast P-cores on a P/E machine → fine tasks
  + Chase-Lev stealing; per-task Tlsf malloc kills it → per-worker external-buffer arenas;
  granularity must ramp with n.

## Tests
hesap-dense suite **236 cases / 99,557 assertions** green. 5-config per-slice DoD PASS on the
MRRR close.

## Decisions
ADR-0065 **§17** (MRRR): D(dense-eig)-9..12 (exact pivmin Sturm guard, deterministic RRR shift,
fixed cluster tie-break + GS-fallback trigger, fixed-iteration dqds, parallel bit-identical) +
divergences (Z1-base, dqds-IEEE-only, dlarrb-per-eigenvalue).

## v3b-1b — bidiagonal SVD (dbdsqr) serial baseline + 4-column bench, SHIPPED

Ported `dbdsqr` faithfully and built the full SVD driver on top of the v3b-1a bidiagonalization.

- **`detail/bdsqr.hpp`** — faithful LAPACK ports: `dlartg` (f90 / Anderson-2017 convention,
  `c≥0`, chained `r` — load-bearing, D(svd)-1, NOT eig_sym's `lartg`), `dlas2`, `dlasv2`,
  `dlasr` (RowMajor, PIVOT='V', all 4 side×dir, D(svd)-4), `drot`, and **`dbdsqr`** itself
  (Demmel-Kahan implicit-zero-shift QR: block-split + shift-direction + fwd/bwd convergence +
  zero/standard-shift sweeps with Givens accumulation into U/VT via dlasr + `dlasv2` 2×2 +
  the 2017 `MAXITDIVN` overflow fix + final positive/descending sort). Always called with
  vectors; values-only routes to `dlasq2` at the driver (D(svd)-5).
- **`svd` / `svdvals` drivers** (`svd.cpp`): clone → `bidiagonalize` → `form_q_bidiag`
  (U_init m×n) + `form_pt_bidiag` (VT_init = Pᵀ) from the stored reflectors → `dbdsqr` →
  V = (Vᵀ)ᵀ → sign pin (D(svd)-2). `m<n` via transpose (D(svd)-3). `svdvals` builds B's
  squared+scaled qd array → `dlasq2` → sqrt (dlasq1-style smax scaling).
- **CLI** `hesap.dense.svd.{f32,f64}` + `hesap.dense.svdvals.{f32,f64}` (singular values blob;
  vectors via engine API, mirrors eig). Anchor `register_svd_cli_anchor`.
- **Tests** (`test_svd.cpp`, +9 cases): dlartg 4-branch · dlas2/dlasv2 reconstruction ·
  dlasr dense-product cross-check (all 4 side/dir — the highest-risk port, gated before
  dbdsqr touched it) · dbdsqr on raw bidiagonal (reconstruct + orthogonality + values vs
  **`sqrt(eig(BᵀB))` using the shipped `eig_sym` as a local oracle** — no LAPACK dep) · full
  `svd` reconstruction ‖A−UΣVᵀ‖ + ‖UᵀU−I‖/‖VᵀV−I‖ for m=n/m>n/m<n · `svdvals` vs `svd` ·
  CLI round-trip. **All green.**

**4-column bench (`bench_hesap_svd_vs_reference`, i9 f64; Eigen JacobiSVD + BDCSVD, LAPACK
dgesvd + dgesdd):**
- Full SVD (values+vectors): beats **JacobiSVD 3.0–3.4×** everywhere; ties/beats `dgesvd`
  at small N (1.30× @128); **loses to D&C at scale** — C/BDC 0.14, C/dgesdd 0.13 @ N=512.
  Accuracy val err ~1e-13, recon ~1e-14.
- Values-only: crushes JacobiSVD **9–15×**; loses to D&C/`dgesdd` at N≥256.

**The honest finding that re-orders the next leaves:** at N≥256 the dominant cost is the
**unblocked `dgebd2` bidiagonalization (BLAS-2)**, NOT dbdsqr. Smoking gun — `svdvals`
@ N=512 = **156 ms vs `dgesvd`-N 46 ms**, while `dlasq2` is O(n²) (proven ~free in v3a-3.1
where dqds values-only tied `dstemr`). So the visible gap to LAPACK at scale is the
*reduction*, not the QR sweep. This makes **v3b-1a-perf (blocked `dlabrd`, BLAS-3)** likely
the LARGER lever than v3b-1b-perf for full SVD at scale — same shape as blocked `dsytrd`
carrying v3a-1. (Saved: [[project_serial_iterative_qr_loses_to_dc_reduction_is_bottleneck]].)

This is exactly what the locked design asked for: serial baseline FIRST, measure the gap,
don't architect v3b-2 on a guess. ADR-0065 **§18** (D(svd)-1..5). 5-config DoD at close.

## v3b-1a-perf — blocked bidiagonalization (the reduction crush), SHIPPED

Closed the reduction gap v3b-1b measured. `bidiagonalize` is now blocked `dgebrd`
(`dlabrd_upper` panels + ONE BLAS-3 rank-2k trailing update per panel via two
`gemm_parallel_auto` GEMMs into a temp + subtract — the proven blocked-`dsytrd` pattern;
unblocked `dgebd2` for small n ≤ 2·nb and the tail). nb=32. ADR-0065 **§19** (D(svd)-6).

**The journey (measure-driven, three increments):**
1. **Faithful `dlabrd` port (scalar).** Reconstruction <1e-11 at N=65/96/100/128/150
   immediately. But the bench exposed a **super-linear blow-up at N=1024 (1331 ms,
   0.19× dgesvd)**: the textbook Y matvec (`Σ_r A(r,jj)·A(r,i)`) walks two **strided**
   A-columns → cache/TLB thrash that explodes past the L2 working set.
2. **Row-outer + contiguous accumulator.** Reorder the Y matvec to a row-contiguous axpy
   into a contiguous `yacc` scratch, then scatter to Y's column once. **1331 → 285 ms**
   @1024; @512 156 → 36.9 ms. Now beats LAPACK at N≤512, ties @1024 (0.94×).
3. **SIMD the panel.** Route the X dot + Y axpy through `simd_dot`/`simd_axpy` (promoted to
   `detail/dot_simd.hpp` as the canonical home; single-rounded FMA, `eig_sym` precedent).
   **@1024 285 → 196 ms, flipping 0.94× → 1.31× (winning).**

**Final `svdvals` (i9 f64, vs LAPACK + Eigen) — BEATS BOTH at every N≥128:**
- vs `dgesvd`: **3.6× @128 · 2.1× @256 · 1.8× @512 · 1.3–1.5× @1024** (three runs:
  196/198 ms @1024). vs `dgesdd` comparable; vs Eigen `BDCSVD` **2.0–6.5×**. Headline
  **@512 156 → ~26 ms (≈6×)**. Accuracy val err ~1e-13.
- Reduction is the O(n³) cost (97% @1024; `dlasq2` ~3%, confirmed by the 7.74×≈8× 512→1024
  scaling). The win = SIMD panel + parallel trailing GEMM beating LAPACK's serial-SIMD
  panel.

**Honest finding — the full-SVD gap is a DIFFERENT algorithm.** Full SVD (values+vectors)
beats `dgesvd` at N=128/256 but still **loses to the D&C references** (`BDCSVD`/`dgesdd`:
C/`dgesdd` 0.41 @256, 0.15 @512). `svdvals` proves this is NOT the reduction — it is the
**serial `dbdsqr` O(n³) vector sweep**, the exact D&C-class gap MRRR hit vs `dstedc`.
Closing it is **v3b-1b-perf** (parallel split-block dbdsqr) + **v3b-2** (Gu-Eisenstat D&C
`dbdsdc` vs SVD-via-MRRR), the next leaves.

Filed: `v3b-1a-perf-followon-parallel-panel` (parallelize the two big panel matvecs across
cores — the LAPACK-serial Amdahl ~40% of the N=1024 reduction — for an even bigger values
crush; deferred per advisor: gilding a winning metric vs the bigger full-SVD D&C fish) +
`v3b-1a-perf-followon-dot_simd-consolidate-eig_sym` (eig_sym's local simd_dot/simd_axpy now
duplicate the canonical header — mechanical dedup, deferred to keep this slice's blast
radius small). Tests: +2 svd cases (blocked-path reconstruction at 65/96/100/128/150 +
blocked svd/svdvals incl. m<n transpose); suite **245 cases / 99,843 assertions** green.

## NEXT
**Sequencing decision for the user** (data-driven): the SVD **reduction now crushes** LAPACK
+ Eigen at all N (v3b-1a-perf done). The remaining gap is the full-SVD **vector** path vs
D&C. Options, in likely-impact order:
0. **v3b-2** — Gu-Eisenstat D&C bidiagonal SVD (`dbdsdc`) **OR** SVD-via-MRRR on the
   Golub-Kahan tridiagonal — the real crush for full SVD at scale (matches what `dgesdd`
   does; closes C/dgesdd 0.15 @512). The bigger fish now that the reduction is won.
~~The pre-v3b-1a-perf options (kept for history):~~ the bigger SVD
lever at scale WAS the **unblocked→blocked bidiagonalization** (v3b-1a-perf, `dlabrd` BLAS-3
panel) — now DONE. Remaining:
1. **v3b-1a-perf** — blocked `dlabrd` bidiagonalization (closes the 156-vs-46 reduction gap;
   helps BOTH `svd` and `svdvals`, and the eig path shares the pattern).
2. **v3b-2** — Gu-Eisenstat D&C bidiagonal SVD (`dbdsdc`) **OR** SVD-via-MRRR on the
   Golub-Kahan tridiagonal `J=[[0 Bᵀ][B 0]]` (parallel MRRR = the eigenvector crush, but with
   the exact-±σ-multiplicity trap noted) — build both, ship the faster, measured vs the
   dbdsqr baseline.
3. **v3b-1b-perf** — parallel `dbdsqr` across bidiagonal split-blocks.
Then v3b-1c (complex) → v3b-3 (randomized Halko). Crush BOTH Eigen AND LAPACK across these.

---

### (historical) v3b-1b original plan
**v3b-1b — port `dbdsqr`** (Demmel-Kahan implicit-zero-shift QR: block-split + `dlasv2` 2×2 +
shift-direction + forward/backward convergence + zero-shift/standard-shift QR sweeps with Givens
accumulation into U/Vᵀ via `dlasr`; helpers `dlasv2`/`dlas2`/`dlasr`/`dlartg`) + `svd` driver
(dgebrd → dbdsqr → back-transform U=Q·Uᵦ, V=P·Vᵦ → descending sort + sign; values-only →
`dlasq2`) + CLI + **4-column bench**. Then **v3b-1b-perf** (parallel `dbdsqr` across bidiagonal
split-blocks = the crush lever). Then v3b-1c (complex) → v3b-2 (D&C vs SVD-via-MRRR, pick by
measured perf vs the dbdsqr baseline) → v3b-3 (randomized Halko). The full reference read of
`dbdsqr` is done; it is a fragile ~400-line kernel — port with the reconstruction + orthogonality
+ vs-LAPACK-values test as the safety gate.
