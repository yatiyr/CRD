# 2026-05-24 — hesap v3d-2c-2b: complex Aggressive Early Deflation (zlaqr0-class)

The production complex Schur. Single-shift `complex_schur` (v3d-2c-2) converges
one eigenvalue per O(n) sweep — sweep count grows super-linearly with n. Complex
AED converges a whole trailing window per inner Schur, collapsing the sweep count
at scale. Mirrors the real v3d-1c HARD-GATE structurally; the big simplification
is **no 2×2 blocks** (complex eigenvalues sit on the triangular diagonal ⇒ no
`dlanv2`/`dlasy2`/`dlaexc`, every reorder/deflation step is one complex Givens).

**Gate framing (differs from the real path):** both external refs (Eigen
`ComplexSchur` + LAPACK `zhseqr`) access-violate at n≥256, so the crush target is
**our own single-shift `complex_schur` baseline at n≥256**; external refs capped
at n≤128. (See `reference_eigen_complex_hessenberg_av_at_large_n`.)

Subdivision (each advisor-vetted): **2b-1 reorder (`ztrexc`)** → 2b-2 deflation
window (`zlaqr2`) → 2b-3 driver (`zlaqr0`) + multishift sweep (`zlaqr5`).

---

## v3d-2c-2b-1 — complex Schur reorder (`ztrexc`) ✅ CLOSED

The cold-entry slice. `reorder_complex_schur(T, Z, ifst, ilst)` moves the diagonal
eigenvalue at `ifst` to `ilst` by a sequence of adjacent 1×1 swaps.

### What shipped

- **`reorder_complex_schur<T>`** (`eig_nonsym.{hpp,cpp}`) — `static_assert(is_complex_v<T>)`
  (the real twin is `reorder_schur`). Each adjacent swap of positions `(p, p+1)`:
  - `g = detail::complex_givens<R>(t(p,p+1), t(p+1,p+1) − t(p,p))` — the faithful
    `zlartg` form (`f = t12`, `g = t22−t11`), overflow-safe, reused from
    `detail/householder.hpp`.
  - **Left** `G·T` over rows `(p, p+1)`, columns `[p, n-1]`; **right** `T·Gᴴ` over
    columns `(p, p+1)`, rows `[0, p+1]` (reads the just-left-updated block — the
    correct `G·B·Gᴴ` sequencing); then `t(p+1,p) := 0` to kill rotated-subdiagonal
    roundoff. **Z** updated by `Z·Gᴴ` over all rows. This is the same Givens
    application form as `complex_schur`'s bulge chase.
  - Applying the rotation over the **full block region** (rather than LAPACK's
    skip-the-2×2-block trick) is provably a unitary similarity with the diagonal
    exchanged — no ambiguity about whether `T(K,K1)` gets updated, and the
    recon<1e-9 gate has ample margin (advisor-endorsed over the LAPACK shortcut).
- Returns `bool` (always `true`) — API symmetry with the real `reorder_schur` for
  the 2b-2 deflation caller; complex has only 1×1 swaps so there is no rejection
  path (the real `bool` exists because `dlaexc` can reject an ill-conditioned 2×2).

### The one trap (advisor-flagged, written correctly first time)

**Backward move with `crd::usize`.** Moving an eigenvalue *up* (`ifst > ilst`)
naïvely as `for (usize p = ifst-1; p >= ilst; --p)` underflows to `UINT64_MAX`
and spins forever when `ilst == 0`. Used a `here` cursor mirroring the real
`dtrexc`'s `while (here > ilst)` shape instead. The c64 test exercises `ilst==0`
explicitly (the `{0,11}` and `{7,0}` pairs).

### Gate — MET

- **Valid Schur of the SAME matrix:** reorder a complex Schur `(T,Z)` of a random
  complex Hessenberg over 5 `(ifst,ilst)` pairs (forward, backward, `ilst==0`):
  `Z'·T'·Z'ᴴ` = the same H `<1e-9`, Z' unitary `<1e-9`, T' upper-triangular `<1e-9`
  (c64).
- **Eigenvalue moved:** the captured `t(ifst,ifst)` lands at `t(ilst,ilst)` to
  `<1e-9` (c64, both move-down and move-up) / `<1e-3` (c32), plus the full
  invariant recheck on the c32 path.
- **~95 LOC, 2 cases.**

### Verification

- 4-config DoD green (touched module, per `feedback_local_test_only_ci_owns_sweep`):
  **win-debug** + **win-shipping (LTCG)** each full `[nonsym]` **247 706 assertions
  / 34 cases**; **win-asan** clean on `[reorder]` (279 assertions); **win-tidy**
  exit 0. Both new cases register as distinct CTest entries (`ctest -R
  reorder_complex_schur` → 2/2 Passed — no catch_discover bracket-comma fusion).
- **win-tidy fixed 5 pre-existing violations** in `test_eig_nonsym.cpp` (the file's
  full-file win-tidy build had never run since v3d-2c-1/2c-2 added these tests +
  the body was only just committed, so CI hadn't seen them either): 3× function-
  local `constexpr crd::usize n`→`const` (the repo `.clang-tidy` sets
  `LocalConstexprVariableCase: CamelCase` prefix `k` for LLVM 19+, so a lowercase
  local `constexpr` is flagged; `n` is runtime-only ⇒ `const` = `LocalConstant` =
  `lower_case` is the clean fix), and 2× `usize→double` narrowings in a `std::sin`
  argument (`static_cast<double>`). Local clang-tidy is LLVM 22.1.1.

## v3d-2c-2b-2 — complex AED deflation window (`zlaqr2`/`zlaqr3`) ✅ CLOSED

The deflation engine. `complex_aed_deflate(...)` takes a trailing window
`[kwtop, kbot]` of size `nw`, Schur-reduces it (`complex_schur`), tests each
eigenvalue's spike tip, deflates converged ones from the bottom, reorders the
rest up out of the way, then reflects the spike + re-Hessenbergizes + updates H/Z
globally so H stays unitarily similar. Structurally mirrors the real `aed_deflate`.

### What shipped

- **`complex_aed_deflate<T>`** (`eig_nonsym.{hpp,cpp}`, ~330 LOC). Window Schur via
  `complex_schur`; deflation test `|s|·|V(1,j)| ≤ max(smlnum, ulp·cabs1(T(j,j)))`;
  `reorder_complex_schur` (2b-1) moves undeflatable eigenvalues up; eigenvalue
  restore = the diagonal of the window Schur form (no `dlanv2`); spike reflected by
  `make_householder_complex` + leading block re-Hessenbergized
  (`hessenberg<Complex>` + `form_hessenberg_q<Complex>`); window written back +
  global similarity `H := Vᴴ·H·V` (+ `Z·V` if `wantz`) via complex `gemm` slabs.
  Returns `{ns undeflated shifts, nd deflated}`.
- **New helpers** (anon namespace): `apply_hc_left`/`apply_hc_right` (complex
  reflector applies, `H = I − tau·v·vᴴ`) + `slab_left_h` (`Vᴴ·C`, complex sibling
  of `slab_left_t`).

### The three complex divergences from the real path (all zlaqr2-faithful)

The real `aed_deflate` could not show these because its V is real:

1. **Spike-apply scalar.** LEFT uses `conj(tau)` (`Hᴴ·C`), RIGHT uses `tau`
   (`C·H`) — per `zlarf` in `zlaqr2`. The spike vector is a plain copy of the
   first row of V (no conj on the copy); the conjugation enters via `conj(tau)`
   on the LEFT only. Double-conjugating (conj on the gather AND conj(tau)) would
   break the unitary similarity → recon ~1, not ~1e-13. The real `apply_h_*`
   don't conjugate at all, so new conjugating helpers were required.
2. **Left slab.** `slab_left_t` uses `Trans::Transpose` (`Vᵀ·H`); complex needs
   `Trans::ConjTranspose` (`Vᴴ·H`) → new `slab_left_h`. `slab_right` (`C·V`,
   `Trans::None`) is correct for complex as-is and was reused.
3. **Coupling restore.** `H(kwtop, kwtop−1) = s·conj(V(1,1))` — the real path
   writes `s·V(1,1)` (real, so the conj is invisible). This single line is what
   keeps the global similarity correct along the `kwtop−1` column.

### Gate — MET (the recon-vs-spectrum split, per advisor)

- **General window** (`n=20`, `nw=8` ⇒ `kwtop=12 > ktop=0`, so the coupling
  column is exercised, not just `kwtop==ktop`): `z·H·zᴴ == H0` over the **whole**
  n×n `<1e-8` (catches a coupling-conj error) AND `eig(H)==eig(H0) <1e-7` (catches
  a similarity sign error the in-window recon would miss — the advisor's reason
  for requiring BOTH). Both green ⇒ the conj details (1)+(3) landed.
- **Decoupled window** (coupling subdiagonal zeroed ⇒ `s=0`): deflates fully,
  `nd==nw`, `ns==0`; still a valid similarity.
- `nd+ns==nw` always; c32 general window `<1e-3`. **3 cases.**

### Verification

- 4-config DoD green (touched module): **win-debug** + **win-shipping (LTCG)** each
  full `[nonsym]` **247 715 assertions / 37 cases**; **win-asan** `[aed]` 43 806
  assertions clean; **win-tidy** exit 0. `ctest -R complex_aed_deflate` → 3/3
  distinct entries Passed.

## v3d-2c-2b-3 — complex AED driver (`zlaqr0`) + multishift sweep (`zlaqr5`) ✅ CLOSED

The production complex Schur. `complex_schur_aed` wires 2b-1 + 2b-2 into the
`zlaqr0`-class driver, and `complex_dlaqr5_sweep` is the BLAS-3 small-bulge
multishift sweep that delivers the scale crush.

### What shipped

- **`complex_dlaqr5_sweep<T>`** — faithful `zlaqr5` port: NBMPS=ns/2 bulges seeded
  by `complex_dlaqr1` (the complex shift-poly first column, no `si1·si2` term) +
  `make_householder_complex` 3-vectors; multi-bulge chain with delayed
  transforms + the special bottom 2×2 bulge + vigilant deflation (`cabs1`);
  KACC22=1 accumulate-into-`U` then BLAS-3 `gemm` far-updates (`slab_left_h` = Uᴴ
  horizontal, `slab_right` = U vertical + Z). **No conjugate-pair shift shuffle**
  (complex shifts used in pairs as-is). Exposed as `detail::complex_multishift_sweep`
  for the implicit-Q gate.
- **`complex_schur_small_block<T>`** — the NMIN crossover (single-shift
  `complex_schur` on a small block + writeback + complex slab updates).
- **`complex_schur_aed<T>`** — driver: split → `complex_aed_deflate` → nibble →
  `complex_dlaqr5_sweep` → NMIN=150 crossover. `h_in = Z·T·Zᴴ`.
- **`complex_dlaqr1`** (shift seed) + the `bench_hesap_eig_nonsym_vs_reference`
  "scale crush" section (AED vs single-shift `complex_schur`, internal — refs AV
  at n≥256).

### The `zlaqr5` conjugation rule (fetched VERBATIM, not reconstructed)

The reflector applies were taken character-for-character from `zlaqr5.f` (the
risk of guessing in a 400-line kernel was too high): **RIGHT** (and the delayed-
transform + U-accumulate) use `T1=tau, T2=tau·conj(v2), T3=tau·conj(v3)` with a
plain-`v` gather; **LEFT** uses `T1=conj(tau), T2=conj(tau)·v2, T3=conj(tau)·v3`
with a `conj(v)` gather. The similarity is `Hᴴ·A·H`. `zlaqr1.f` confirmed the
seed formula exactly.

### THE BUG — a model isolation trail (kept for the lesson)

First cut passed every unit test (smooth sin/cos matrices, recon ~1e-13) but the
bench's **random** matrices gave recon ~1. single-shift `complex_schur` was clean
on the same random matrices (recon 1e-13) → the bug was in `complex_schur_aed`,
not the bench. Isolation, in order:
1. **Implicit-Q gate** (`detail::complex_multishift_sweep`, `Z·H'·Zᴴ == H_orig`
   with Z=I for arbitrary shifts): the multishift sweep is an EXACT similarity for
   all block positions (full/partial, ktop>0, kbot<n-1) and all shift counts
   (ns=2…40), recon ~1e-15. **Sweep is correct.**
2. **Crossover-only** (NMIN=∞): random n=128 recon 8.6e-14. **Crossover + driver
   wiring correct.**
3. **Per-step running recon** in the driver: preserved by every sweep + every
   `nd=0` deflate, **jumps to 0.074 at the first `nd>0` deflate** → the bug is the
   `complex_aed_deflate` **spike-reflection path**, which only runs on *partial*
   deflation (`ns>1 && s≠0`) — a case the small-window 2b-2 tests never hit.

Root cause: `zlaqr2.f` line 50 **conjugates the spike row** (`WORK(I) =
DCONJG(V(1,I))`) before `zlarfg`; my port copied it plain. **The advisor flagged
this exact conjugation in the 2b-2 review and I wrongly concluded "plain copy".**
One-line fix: `work[k] = conj(v.at(0,k))`. Then 2b-2 + 2b-3 both pass on random
matrices. **Lesson: test eigensolvers on generic/random matrices — smooth
analytic spectra deflate without exercising the spike path.** (Permanent
regression guards added: implicit-Q sweep test + random-matrix `complex_schur_aed`
+ large-window partial-deflation `complex_aed_deflate`.)

### Gate — MET (the scale crush)

- **Spectrum + recon** (vs single-shift `complex_schur`, our own baseline): match
  ~1e-13, recon `z·T·zᴴ==h_in` ~1e-13 at n=40 (crossover) / 160 / 260 (AED).
- **Scale crush** (bench, c64, AED vs single-shift): n=256 **1.10×**, n=400
  **1.12×**, n=512 **2.01–2.14×** — the win **widens with N** (the AED lever);
  sweep counts collapse to 3/4/4. n=128 (crossover regime, below the n≥256 gate)
  0.90× (driver Z-accumulation overhead vs bare `complex_schur` — accepted,
  `feedback_crush_mandate_bounded_by_importance`). recon ~1e-13 throughout.
- **NMIN measured = 150** (crossover ~200; NOT the real path's 200): n=128 loses,
  n≥256 wins. **D(non-sym)-7** (window-size formula) + **D(non-sym)-8** (AED shift
  order) pinned.

### Verification

- 4-config DoD green (touched module): win-debug + win-shipping (LTCG) each full
  `[nonsym]` **247 757 assertions / 44 cases**; win-asan `[aed]` clean; win-tidy
  exit 0. The vs-reference bench builds + runs clean with the flag (reset OFF).

## State / next

2b-1 + 2b-2 + 2b-3 are 4-config-DoD green; **v3d-2c-2b (complex AED) CLOSED** — no
open debt. **NEXT = v3d-2c-3** — `ztrevc` (complex right eigenvectors) +
back-transform + public complex `eig(Matrix<Complex<T>>)` + CLI
`eig.nonsym.{c32,c64}`. Wire the public complex `eig` to `complex_schur_aed` (the
production Schur), not bare `complex_schur`. Then v3d close + v3e (§17 lock incl.
D(non-sym)-1..8; verify zlahqr/zlaqr5 exceptional constants vs the .f sources).
