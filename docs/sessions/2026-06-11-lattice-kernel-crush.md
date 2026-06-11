# 2026-06-11 — the lattice kernel dig: syrk pack root cause + merge fix + in-place TRSM (part 19)

**Phase:** 3.1.6 `crd-hesap` — the named hesap-direct frontier ("I want the lattice crush too"): the
3D-lattice JᵀJ serial-kernel gap vs CHOLMOD (v7-e-2 STEP 7 verdict: W=1 0.73×, attributed to the ADR-0082
wall). This session **refuted the "kernel wall" attribution for a second time** — profile-driven, three real
levers found, the biggest one a genuine kernel-layer bug-class fix. Plus the IPOPT scoreboard row (installed
+ measured).

---

## The dig (measure → refute → fix)

Baseline reproduced exactly: lat32 serial FACTOR **4283 vs CHOLMOD 3164 ms (0.74×)**. Per-phase profile
(CRD_HESAP_CHOL_PROFILE, instrumentation re-wired post-SYRK-restructure): cmod 2535 ms (126 GFLOP ≈ 50
GF/s) + cdiv 1616 ms (84.7 GFLOP ≈ 52) vs CHOLMOD's 66.9 GF/s everything-rate.

### Lever 1 ⭐ — `syrk_lower_minus` packed its operand ELEMENTWISE (the big one)

The v0e syrk packed the FULL m×k operand twice through `MatrixView::at()` — for a ColMajor source with a
large leading dim (the supernodal panels, ld up to ~6700) that transpose-pack is **one cache/TLB miss per
element** and dwarfs the flops. REBUILT as a triangular-output mirror of the Goto gemm driver: the same
`pack_a`/`pack_b` (layout-aware, stride-friendly), the same kGemmMc/Kc/Nc blocking, the same microkernel —
only the tile VISITS differ (strict-upper skipped, diagonal-crossing masked to i ≥ j, padded-tail bounded).
**Value contract:** the K-grouping now matches `dense::gemm` exactly (kKc chunks, ascending pc, zero-init
micro per chunk) and IEEE negation symmetry makes the lower-triangle bits **identical to gemm-then-subtract**
— which is what the cmod's node-parallel path computes. This HEALS a latent serial-syrk-vs-parallel-gemm
value divergence for descendants with knc > 256 (the old full-k-per-tile reduction differed; worker count
selected the path). In-factor: **cmod 2535 → 2013 ms; lat32 serial 4283 → 3705 (0.74× → 0.85×)**, residual
unchanged 9.1e-15.

### Lever 2 — the ColMajor merge loop-order fix (bit-identical, universal)

`gemm_packed_inner`'s C-merge looped j-inner for BOTH layouts — for ColMajor C that strides EVERY write by
ld. Swapped to i-inner (contiguous) for ColMajor. Pure reordering of independent element writes ⇒
bit-identical; lifts every ColMajor-C gemm in the engine (the cmod K≥128 bin measured 71 → 76 GF/s on the
good reps).

### Lever 3 — the (B) below-outer TRSM, landed as the bit-identical IN-PLACE form

Tried and MEASURED three forms (the probe artifact lesson below applies):
- staged jb=64 walk (the v5a-4/v5a-7 form): ~54 GF/s — B1's Tᵀ-scratch + subtract pass + narrow shapes;
- whole-obw fused inverse + ONE K=256 gemm: the gemm itself hits 75 GF/s but the gemm-blocked 256-inverse
  build (~0.4 ms × ~330 outer blocks) + the tail blocks (below_o small) that can't amortize it ate the win
  — REVERTED;
- **LANDED: the staged walk in wide-N RowMajor IN-PLACE form** — every operand is a transpose-VIEW of the
  panel (RowMajor view of a ColMajor block IS its transpose in place): B1 runs as one beta=1 in-place gemm
  (reads solved cols, writes the jb block — disjoint, alias-free; kills the Tᵀ-scratch + subtract passes),
  B2 keeps only the jb-block snapshot. K-groupings unchanged ⇒ **BIT-IDENTICAL to the validated original**.
  Serial-equal speed, less scratch traffic (helps multi-lane memory pressure).

**The remaining gap is now precisely named and measured:** B1's M=64-skinny shape re-streams the packed B
panel per 6-row a-panel (bandwidth-bound ~50 GF/s in ANY orientation; M=256 fixes it but needs the inverse);
the cmod scatter (~265 ms, a cold re-read of every U element); the cdiv-A K=64 chain (~385 ms). The proper
fix is a **dedicated packed-TRSM driver** (OpenBLAS-style: pack X once, walk the triangle with gemm-class
inner loops and a fused solve microkernel) — a focused future slice, NOT asm.

## ⚠ The probe artifact (sanity lesson — tool blind spot)

The first standalone probe measured syrk at 3.4 GF/s "20× under OpenBLAS" — but the ad-hoc g++ compile
lacked `-DCRD_SIMD_TARGET=2`, so the header-only template instantiated the SCALAR microkernel in the probe
TU while the library (and the factor) ran AVX2. The library's `dense::gemm` looked fast in the same probe
only because it's compiled INSIDE the .a. **Ad-hoc probe TUs must replicate the library's SIMD/FP defines**
(`-DCRD_SIMD_TARGET=2 -DCRD_DETERMINISTIC_FP=1`), and a second artifact: a stale .a can satisfy the linker's
COMDAT for header templates — rebuild the lib before probing headers. The pack-cost diagnosis survived the
artifact (the in-factor delta proved it), but the magnitude claim did not.

## Scoreboard (clean builds, taskset-pinned; FACTOR ratio = CHOLMOD/Cerid, >1 = win)

| matrix | serial before | serial after | 8T after | note |
|---|---|---|---|---|
| nls_lat20 | ~0.8× | **0.91×** | 0.86× (noise) | |
| nls_lat24 | ~0.8× | **0.91×** | **0.99× parity** | held |
| nls_lat28 | — | **0.85×** | 0.88× | |
| nls_lat32 | **0.73×** (4300/3147) | **0.83–0.87×** (3705–3859) | 0.85× (1210/1028) | CHOLMOD's own 8T also faster today (1028 vs the recorded 1139) |
| hood (FEA) | 1.53× WIN | — | **1.57× WIN** | no regression |
| bcsstk25 (FEA) | 1.66× WIN | — | **1.85× WIN** | improved |

Residuals 9.1e-15..9.8e-15 on every lattice (unchanged). Host noise ±5-8% (14900K P/E scheduling under WSL
— pin serial runs with `taskset`).

## Verification

- WSL gcc-release: hesap-dense **359,508 / 349** ✓ · hesap-direct **598,861 / 190** ✓ (the {1..16} moat
  tests green — corroborates the bit-identical in-place B and the gemm-equal syrk).
- Windows: win-debug + win-shipping run both suites green (same counts); win-asan green; win-tidy builds
  clean. Files: `supernodal_cholesky.cpp` (in-place B, instrumentation, profile-define hygiene; the unused
  blocked-inverse helper removed), `syrk_microkernel.hpp` (rebuilt), `gemm_pack.hpp` (merge fix).

## The IPOPT row (user-side install executed this session)

`coinor-libipopt-dev` 3.11.9 + cyipopt 1.7.0 installed; `scripts/setup-ipopt-ref.sh` green. Both scoreboard
sides grew the pinned NLP row (Rosenbrock-in-the-unit-disk from (0,0), exact derivatives):

| solver | f | x | iters |
|---|---|---|---|
| IPOPT 3.11 (cyipopt) | 0.045674808 | (0.7864152, 0.6176983) | 16 |
| **Cerid filter IPM** | 0.045674809 | (0.7864152, 0.6176983) | 22 |

Same point to 7 decimals, objective to 8; iteration count same class (IPOPT carries a restoration phase +
adaptive-μ heuristics we deliberately scope out). The last pending v7-z reference row is closed.

## Honest verdict

The "serial dense-kernel wall" of STEP 7 was **one-third real wall, two-thirds fixable**: the syrk pack
pathology (fixed, −13% whole-factor), the ColMajor merge stride (fixed, free), and the B1 skinny-M shape
(named, needs the packed-TRSM driver). lat24 = 8T parity; lat32 serial 0.73 → 0.85-class with the moat and
~22% less fill. The crush-to-win on lat28/32 has ONE named lever left, and it is C-level kernel work.
