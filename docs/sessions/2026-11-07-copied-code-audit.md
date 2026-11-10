# 2026-11-07: copied-code audit, first results

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Companion to the [GPL-derived code findings](2026-11-07-no-gpl-derived-code.md). Audit of commit d2020d17, ordered by the user:
no copied code in the engine, whole engine confirmed.

## Method and limits

- Token comparison (exact and rename-blind), ScanCode, comment and identifier searches, against 94 downloaded projects, about 369
  million tokens. Run on a one-run cloud machine, about 0.21 USD, deleted. Raw results stayed outside the repository.
- 310 files whose comments name a source were classified from those comments. A grade of "faithful" comes from the comment, not from
  a comparison with the source.
- The planned read of all 106 modules was dropped. A reader without the upstream beside it cannot recognise a restructured translation.
- Limits: this compares only against what was downloaded. Translation between languages and restructured text can escape: the
  netem-derived C++ shared about 4% of its 8-grams with `sch_netem.c`, and only its identifiers and comments found it. The project
  list omits Numerical Recipes, Ruckig, Filament, MaterialX and others. This is evidence, not proof that the engine is clean.

## Findings: not permitted to stay

| Files | Source | Licence | Evidence | Outcome |
|---|---|---|---|---|
| CSparse, CHOLMOD and UMFPACK group, `link_models` | see the companion note | LGPL, GPL | comments and tokens | solver group: token overlap 3% exact and 8% rename-blind at most, code kept, comments now cite the papers (80b51f9d); `link_models`: see the companion note |
| `hesap-special`: `bessel.hpp`, `airy.hpp`, `elliptic.hpp`, `expint.hpp`, `incomplete.hpp`, `fresnel.hpp`, Dawson in `erf.hpp`; `hesap-opt/powell.hpp`; `hesap-diff/finite_difference.hpp` | Numerical Recipes | proprietary | comments name the routines; `bessel.hpp` Steed CF2 and `betacf` and `mnbrak` read by hand | `incomplete.hpp` (82c2f8a2), `expint.hpp`, `elliptic.hpp`, `fresnel.hpp` and Dawson (8effc95d), `powell.hpp` and `finite_difference.hpp` (8b0c3f86), `bessel.hpp` and `airy.hpp` (b5d26da3) rewritten and landed; defects found against base fixed and landed: Bessel, Airy and Struve (4234137c, 59f85e8a), incomplete gamma and beta (a512603c), exponential, elliptic, Fresnel and Dawson (9560bc10, 5dc62e0f), see below |
| `hesap-iterative/qmr.hpp` | Krylov.jl `qmr.jl` | MPL-2.0 (LICENSE read) | comment: "transcribed VERBATIM" | rewritten and landed (784ffee6) |
| `hesap-tensor/tt.hpp`, `tt_cross` | tntorch `cross.py` | LGPL-3.0 (repository licence) | comment names the scheme; text compared | `tt_cross` is native C++ (pools, Philox streams, LU-solved core) and stays; maxvol stays as an MIT port with its notice added (df7af99c) |

The Numerical Recipes group was missed by name because the book is not in the corpus. It was found through GSL and libstdc++,
whose Steed CF2 text is similar to it. That shared lineage is inferred, not traced.

## `incomplete.hpp` and `gamma.hpp`: defects fixed before the rewrite

Landed as a512603c (one commit on 0971ef5f), after the rewrite of the Numerical Recipes text (82c2f8a2). This commit fixes
defects that were measured against base (origin/main), scipy and exact sums. Evidence is in the branch's
`build/gpl-compare/incomplete-final/round4` and `round5` directories, outside the repository. R is the error over the module
limit (f64 rtol 1e-12 + atol 1e-14, inverses 1e-9 + 1e-11; f32 1e-4 + 1e-6).

| Defect, last review | Before (da7b33dc) | After (a512603c) |
|---|---|---|
| f64 `betainc`, subnormal smaller shape, larger shape from 1e4 | NaN or inf on 41 + 41 edge points; `betainc(4.94e-324, 1e4, 1e-6)` NaN | 1 (base 1), mirrored 0; edge grid in-domain finite-to-NaN 0 |
| `betainc_inv(1, DBL_MAX, p)`, `betainc(1, DBL_MAX, 5.5e-319)` | NaN | closed forms: 5.6e-319 to 1.6e-309; 9.887e-11 |
| f32 `gammainc_p_inv` and `gammainc_q_inv`, shape 1e5 to 1e10 | 67 newly outside of 8000; NaN at 64276868 | 0 newly outside, R at most 0.00059; double from a shape of 1e5 |
| f64 `betainc_inv(324.17, 7.31, 4.34e-27)` | reported R 4.31 | not a defect: the root is correctly rounded (mpmath, 60 digits); regression test kept |
| f32 `betainc_inv`, smaller shape near 1e-6 and the 4-argument overload | R 31.3, 860, 1.33 | R below 3e-4; ib32 newly outside 4 to 0 |

Every family of the review's harness has 0 points newly outside the limit. Earlier rounds fixed:
- far tails of both inverses (wrong by factors or NaN);
- huge shapes;
- ln B at shapes of 600 to 1000;
- single-precision inverses at small shapes;
- NaN inputs that ran 50 times base's cost.

Speed: a function counts as slower when its median is more than 2 % above base's in two interleaved runs of 7 rounds. Rows
accepted over 2 %, ratio to base, both runs, and the reason:

| Row | Ratio | Reason |
|---|---|---|
| `rv_uni_only_basegood` | 1.63 / 1.67 | 2 of 1981 expansion-served points; base right by chance where its neighbours are wrong |
| `rv_bgrat_only_basegood` | 1.15 / 1.14 | 33 of 2856 points; the same reason |
| `gpi_f32_small_basegood` | 1.19 / 1.19 | base wrong on 20 % or more of every band; no test separates its correct points |
| `rv_sub1e10_f32` | 1.51 / 1.51 | float BGRAT in double, which the overflowing float sums need |
| `rx_bil_f32_64_1024_basegood` | 1.16 / 1.16 | the Stirling prefactor the base-wrong points of the same class need; no cheap predictor |
| `rv_far_bil_small` group | 1.17 (routed 1.05, unrouted 1.35) | far points routed to the plain path; the rest lies where the plain path is over the limit |
| `rv_far_bil_bgrat`, `rv_far_bil_uni` | 1.01 to 1.04 | the cost of the distance gate |
| `rx_gq3_f64_small` | 1.02 / 1.04 | the a >= 400 guard, which fixes base's points at R 216 |
| `rx_bii_lb_f64`, `rv_far_bgrat` | 1.05 / 1.05; 1.03 / 1.05 | the cost of the gate; the review timed this code at 1.02 or below |
| `gap_bi3_basebad`, `rx_bil_f32_64_1024_basebad`, `gpi_f32_small_basebad` | 1.16 to 1.21 | inputs where base is wrong |

An interleaved run of base, da7b33dc and a512603c's code (as bf05c7e2) put round 4 at 0.989 to 1.016 of da7b33dc on every row.

## `expint.hpp`, `elliptic.hpp`, `fresnel.hpp` and Dawson: defects fixed before the rewrite

Landed as 9560bc10 (the exponential, Carlson, Fresnel and Dawson defects) and 5dc62e0f (the Carlson arguments of F, E and
Pi near π/2 and at the edge of the domain), two commits on 8605c546, after the rewrite of the Numerical Recipes text
(8effc95d). The serial workflow marked this group "discarded" after three review rounds. This landing supersedes that status.

Defects were measured against base (8effc95d), scipy and mpmath at 60 digits, at the exact double inputs. Evidence is in the
branch's `build/gpl-compare/elliptic-final` directory (round 1, `review`, `round2`), outside the repository. It also uses the
`expint_elliptic-*` files of the workflow tree. A unit is the module limit 1e-13 + 1e-12 |r|.

| Defect | Before | After |
|---|---|---|
| F, E, Π for \|φ\| > π/2: the principal-range formula on the whole line | F(3, 0.3) = 0.14 (true 3.286); 1.3e12 units in the sample | quasi-period 2jK, 2jE, 2jΠ (DLMF 19.2(ii)); 0 over |
| K(1), E(1), K(−∞), E(−∞) | 1.8e18, 0, NaN, NaN | ∞, 1, 0, ∞ |
| K and E for m near 1 (Cody form) | up to 12.8 units at 1 − m = 1.1e-16 | within the limit |
| R_J, p < 0 (Cauchy principal value) | R_J(0, 0.5, 2, −1) = 103.7 (true −2.287); R_J(1e-20, 1e-20, 1, −1e-20) = 2842170.94 (true −34.83) | the DLMF 19.20.14 relation, and the cancelled form where it loses digits |
| R_J, p > 0, over the double range | NaN on 19,949 grid points; up to 3e-4 relative | finite and within the limit |
| R_F, R_C, R_D: diverging, infinite and top-of-range arguments, subnormal means | NaN; R_F(5e306, 3e307, 5e307) = 2.06258175e-154 (true 2.06259132e-154) | +∞, 0 and correct values |
| E_n(0), E_1(0), Ei(+∞); Ei for 709.78 < x < 716.3 | NaN; +∞ | +∞; finite (8.55e305 at 711) |
| Si, Ci at ±∞ | NaN | ±π/2, 0 |
| Fresnel S, C at ±∞ and \|x\| > 1.3e154; phase πx²/2 beyond \|x\| ≈ 5e4 | NaN; rounded (3e-9 at 1e8, 4830 units) | ±1/2; x² mod 4 formed exactly |
| Dawson at ±∞ and \|x\| > 9e307 | NaN | ±0 and 1/(2x) |
| F, E, Π near φ = ±π/2 with m or n near 1: 1 − m sin²φ subtracted a rounded sin²φ from 1 | F(fl(π/2), 1) = 38.718 (true 38.025); F(1.570796, 1) 1.27e6 units; Π(1, 1.570796, 0.5) 2.67e7 units; Π(1, fl(π/2), 0.5) NaN (true 2.3095860183650104e16) | the arguments are cos²φ + (1 − m) sin²φ; at most 4e-4 units at every listed point |
| quasi-period near odd multiples of π/2: the rounded \|φ\|/π named the neighbouring half turn | F(4.71238898038469, 1 − 1e-12) 8.0 units | the sign of cos φ corrects the turn; 0 units |
| first head of the arguments above, edge of the domain for m > 1 | E(0.8000996602392672, 1.942881692827097) NaN (true 0.6095122980999521) | 0.6095122980999521 |
| first head, Π(2, ±fl(π/4), m): the argument cos²φ − sin²φ was exactly 0, the pole of R_J | NaN; true Π(2, fl(π/4), 0.7408416310133681) = 23.73003728978681 | ((2 − n) + n cos 2φ)/2 near the pole; within the limit |

The last two rows were found by the independent review of the first head (1ef4f53a). They never reached main.

At the edge of the real domain for m > 1 (sin²φ = 1/m), 1 − m sin²φ is taken as 0 when rounding makes it negative by no more
than 8 units of 2⁻⁵³ in the size of its terms. Rows outside the domain by no more than that now return the value at the edge
instead of NaN. In the review's sets, the true argument of every such row is at most 9.17 of those units below 0: 53 of
2,799 out-of-domain rows of the main sample, 1,048 of 1,478 of the edge set and 3,321 of 4,605 of the delta set. Rows further
outside stay NaN.

Review sample, on the landed code:
- Main sample: 30,000 rows, 27,201 in the domain, seeds 7340001 + stratum.
  - finite-to-NaN: 0 for F, E, Π, K and complete E (base 8, 8 and 120 for F, E and Π).
  - Newly outside the limit against base: F 9, Π 5. Newly inside: F 10,064, E 9,188, Π 12,235, K 3,384, complete E 3,397.
  - Π at n in [1.9, 2.1]: 446 of 2,575 rows over the limit (base 1,802), all within input conditioning, none non-finite.
- Edge set: 3,000 rows at φ = asin(1/√m) ± 4 ulps, seed 7340101. finite-to-NaN 0 (base 43, first head 21).
- Delta set: 18,000 rows, seed 7340201. finite-to-NaN 0 (base 36, 36 and 29; first head 66 each).
- Near-π/2 sample of round 1: 50,000 rows with φ near odd multiples of π/2 and m, n in [0.9, 1.1], and 10,000 control rows.
  0 over the limit, 0 newly outside, 0 finite-to-NaN.

Accepted as ill-conditioned (within the input's conditioning; the true derivative in φ is infinite at the edge of the
domain, and 1/(1 − n sin²φ) near the pole of Π):
- the main sample's 14 newly outside rows (F 9, Π 5);
- in the delta set, F 124 and Π 95 newly outside against base. 0 are newly outside against the first head.

Speed: a function counts as slower when its median is more than 2 % above base's in two interleaved runs of 7 rounds. Every
ordinary, near-π/2 and m > 1 row is at 0.925 to 0.993 of base in both runs. Rows accepted over 2 %, ratio to base, both runs,
and the reason:

| Row | Ratio | Reason |
|---|---|---|
| F, E, Π past π/2 (`ellint_f_far`, `ellint_e_far`, `ellint_pi_far`) | 1.235 / 1.237; 1.069 / 1.068; 2.213 / 2.217 | base wrong at every point: it has no continuation past π/2 |
| `pi_n15_pv`, Π(1.5, φ, 0.5) across the pole | 1.147 / 1.145 | base over the limit on 1,285 of 1,508 principal-value rows |
| R_J principal-value rows (`elliprj_pv`, `_pgap`), Π at the poles of the complete integral (`ellint_pi_n1`, `_m1`) | 1.14 to 15 (626 ns against base's 41 ns NaN at a spread of 1e307) | base returns NaN or a wrong value there |

## Findings: permissive sources without their notices

About 56 files say they follow permissive or public-domain code, and only `dlpack.h` keeps a notice. No NOTICE file exists.

| Source | Licence | Files | Outcome |
|---|---|---|---|
| LAPACK | BSD-3 | `hesap-dense`: 15 | notice added (15 files) |
| SciPy | BSD-3 | `hesap-dsp` 8; `hesap-ode` 3; `gcrot`, `nelder_mead` | notice added (13 files) |
| NLopt (`newuoa`, `bobyqa`, `cobyla`) | MIT (COPYRIGHT files read) | 3, with 3,492 exact tokens | notice added (3 files) |
| L-BFGS-B-C | BSD-3 | `lbfgsb.hpp` | notice added; its `lnsrlb` line search, which follows MINPACK-2 `dcsrch`, rewritten with `more_thuente_line_search.hpp` (0746716d) |
| QUADPACK | public domain through SLATEC, settled by the [code-freedom note](2026-11-07-code-freedom-rule.md) | `qags`, `qng`, `oscillatory`, `gauss_kronrod`, `adaptive` in `hesap-quadrature` | notice added (5 files) |
| Ruckig; opt_einsum; IterativeSolvers.jl; PyWavelets; maxvolpy | MIT | 2; 1; 1; tables; `tt.hpp` maxvol | notice added (`tt.hpp` maxvol at df7af99c) |
| Filament, MaterialX with OSL noise, glTF Sample Renderer, cotengra | Apache-2.0; OSL BSD-3 | 4 + 1 | notice added |
| pbrt-v3, ltc_code | BSD | 2 | notice added to `ckir_hair.hpp` (pbrt-v3) and `ckir_lighting.hpp` (ltc_code) |
| libwebrtc; tinyexr; ReproBLAS; mt19937ar; Boost.odeint | BSD-3; BSD-3; BSD-style (University of California), settled by the code-freedom note; BSD-3; BSL-1.0 | 6; 2; 2; 1; 1 | notice added |
| Cephes; Shewchuk; MINPACK-2 | BSD-3 by the author's permission (xsf licence list), settled by the code-freedom note; public domain; no statement found | 2; 1; 1 | Cephes: notice added (2 files), and the "public domain" wording in both files corrected; Shewchuk: notice added to `predicates.cpp`; MINPACK-2: moved to the rewrite list, `more_thuente_line_search.hpp` is rewritten |

`assets/resources/src/hdr_piz.cpp` says its code is its own but shares 120 exact tokens and its local names with tinyexr's PIZ decode.

## Cleared

- `lbfgsb.hpp`: its match with R's `lbfgsb.c` is shared lineage; its header and differential test name Becker's BSD-3 C. Notice still missing.
- FFT codelets: `gen_fft_codelets.py` and `gen_fft_batched.py` are Cerid Python following the Frigo-Johnson paper. No FFTW text was
  compared, since a Python generator cannot be token-matched to OCaml. `hier_codelets.hpp` names `build/gen_subfft_m3.py`, a Cerid scratch generator recorded as lost in
  [the v10 FFT bench note](../bench/2026-07-03-v10-fft-remeasure-and-midband.md), with its generated products remaining.
- Marching-cubes tables, Shewchuk's public-domain text in peers, Blender and SciPy boilerplate: no copyleft text.
- netem peer sources: absent at this commit.
- ScanCode found no GPL or LGPL header in any engine source file.

## 2026-11-10: what landed, what did not, and what the comparison found

Each rewrite was compared old against new on the same inputs. "Identical" means a compare program printed byte-identical values
for both trees. Speed is the median new/old ratio of one function per program, 7 interleaved rounds, two runs, the rule being 2 %.
The evidence files are in `build/gpl-compare` (outside the repository).

| Group | Commit | Old against new | Speed |
|---|---|---|---|
| `incomplete.hpp` (`gammaq_cf`, `betacf`, `gammainc_p_inv`, 4-argument `betainc_inv`, two helpers) | 82c2f8a2 | f64 identical in worst and mean error; f32 gamma inverses at most 5.23 of their tolerance from old, worst error lower (31.3 against 36.5); 0 edge-class mismatches in 15300 rows | worst 1.0097, none above 1.02 |
| `expint.hpp`, `elliptic.hpp`, `fresnel.hpp`, Dawson | 8effc95d | worst error equals old's on 314606 grid points per tree; better on `elliprc`, `elliprf`, `sici`, `expint_en` | 38 cases, none over 2 % in both runs; most 0.4 to 0.97 |
| `powell.hpp`, `finite_difference.hpp` | 8b0c3f86 | identical (1252 lines) | Ridders +0.6 %, bracket -0.9 %, `brent_min` -3.8 % |
| `qmr.hpp` | 784ffee6 | identical (19 solver cases, all four precisions) | 0.98 to 1.007 |
| `more_thuente_line_search.hpp`, `lbfgsb.hpp` `lnsrlb` | 0746716d | identical (25828 lines) | `dcstep` 0.9773 and 1.0168, search 1.0072 and 0.9998, `lnsrlb` 1.0004 and 0.9968 |
| `tt.hpp` notice | df7af99c | no code changed | none |
| solver comments (`amd`, `camd`, `lu_symbolic`, `sparse_lu`) | 80b51f9d | disassembly hashes identical | none |

Where old was wrong, the new text differs on purpose: `ellipj` for m > 1 (old returned garbage), `E_n(inf)` and `E1(inf)` (NaN
before, 0 now), `Ei(-inf)` (NaN before, -0.0 now), `ellipj` below 1e-150, and a non-finite subdiagonal in the complex QMR rotation.

### Not landed, and why

| Group | State | Why |
|---|---|---|
| `bessel.hpp`, `airy.hpp`, Struve | landed after the workflow, 4234137c and 59f85e8a | The workflow's last defect round was set aside; the lead finished it, and the landed fixes cover every correctness item below, and the speed rows were ruled one by one (landing record: 43000 fresh points with 0 newly over the bar, 7000 complex Y points with 0 over). The workflow's review had found three regressions against base (complex Y at negative integer order with Re z < 0 gives NaN on 28 of 2500 points; complex K at half-integer order with Re z < 0 is over the bar on 3 points; J_nu(-0.0) for nu > 0 is NaN where base is 0), more defects (I and I' infinite where the true value is finite for x above 710; K and K' zero at x of 745 and up with large order; J and J' NaN where the true value underflows; complex J and I NaN at every negative integer order; complex I and K over the bar at large imaginary part), and speed misses (Airy series +42 % to +60 % on x in [-9, -5.5], where base was wrong; J asymptotic +43 % to +64 % at nu of 5 and up; two complex J rows at 2.97 and 1.96 of base). |
| `incomplete.hpp` accuracy fixes (second group) | landed after the workflow, a512603c | See the `incomplete.hpp` section above. The workflow's review had found: the uniform expansion far from the mode at a+b of 5e5 and up runs at 3.8 times base's time on points where base is correct (99.95 %); the small-shape expansion far from the mode runs at 1.96 times; the window widths were derived from base's plain prefactor, not the fallback the head uses. The 82c2f8a2 rewrite above landed without these fixes. |
| `elliptic.hpp` defects (`ellint_f`, `ellint_e_inc`, `ellint_pi`, `elliprj`, infinities) | landed after the workflow, 9560bc10 and 5dc62e0f | See the `elliptic.hpp` section above; both items below are fixed, including `elliprc` and `elliprd` at an infinite argument. The workflow's review had found: `1 - m sin^2(phi)` cancels near phi = pi/2 with m or n near 1 (`F(phi, 1)` at the double nearest pi/2 is 38.718 for a true 38.025, 1.8e10 units); `Pi(1, phi, 0.5)` stays NaN at three phi values. `elliprc` and `elliprd` still return NaN for an infinite argument, because the check costs 2.1 % to 3.9 % on `elliprc` and 2.8 % to 3.0 % on `elliprd`. |
| `tt_cross` range defect | landed 2026-11-10 (`fix(hesap-tensor): keep tt_cross finite and its validation error honest across the whole range`) | The first attempt covered one element's range, not the sums: `{400,400}` rank 4 at 1.2e153, `{64,64,64}` rank 8 at 1e153 and `{256,256}` at 2e18 in f32 still gave a NaN TT, and the validation error underflowed 16 decades above the reference norm. The landed fix is new; see "Defects found by the comparison". |
| `fuzz_link` | landed 2026-11-10 (`fix(hesap-net): read table delays exactly past 2^53`) | See "Defects found by the comparison" and the fuzz recount below. |

### Defects found by the comparison

| Group | Found | Outcome |
|---|---|---|
| `powell.hpp`, `finite_difference.hpp` | `brent_bracket` probed from the vertex itself after the slide, so mid equalled hi and the minimum was off by 1.1 to 95 for s of 5 to 100; `derivative_ridders` divided by the nominal 2 x step at large x (1.3e-12 at x = 1e3, 4.6e-9 at 1e6); h = 0 gave NaN | fixed, ba2f790e. Regression tests fail on base (56 + 56 assertions) and pass on new. 476 of 6912 Ridders rows fixed. |
| `qmr.hpp` | a serious Lanczos breakdown (`<p,q>` = 0 with q not 0) reported convergence: on A = [[1,1,-1],[1,2,0],[1,0,3]], b = e1, true residual sqrt(2) | fixed, 983ae862, reported as Breakdown. A first draft that stopped at the previous iterate lost an answer base got right (residual 1e-14); the final form finishes the step with norm(q). |
| `more_thuente_line_search.hpp`, `lbfgsb.hpp` | a non-finite trial value, or an overflowed step, ended in alpha NaN after 64 evaluations (5158 and 12793 failures in the random probe) | fixed, 4d028d20: retreat to the midpoint, bisection of an overflowed step. 0 failures; finite inputs identical. |
| `bessel.hpp`, `airy.hpp`, `incomplete.hpp`, `elliptic.hpp` | see "Not landed" | fixed: 4234137c, 59f85e8a, a512603c, 9560bc10, 5dc62e0f |
| `tt.hpp`, `tt_cross` | values below about 1e-155 or above about 1e153 in f64 (1e-19, 1e19 in f32), or a Householder or validation sum leaving the range at any block size: an all-zero TT with relative error 1.0 and `converged` true, a NaN TT, or `val_error` 0 where the true error is 5.7e-16 | fixed 2026-11-10 (commit named in "Not landed"), `tt.hpp` only. Each of the three sums that square f's scale (the QR column norms of a fiber block, the reference norm, the error sum) is formed plainly and then tested; only a result the test cannot vouch for is redone on values scaled by an exact power of two (Blue's scaled norm, after the fact). The QR test reads R and the reflector coefficients: finite, every applied reflector's R_jj^2 >= 4 rows mu, and s^2 >= 4 (r(r+1)/2) rows mu/eps for s the sum of |R|, derived in the source; a sum is trusted when finite and >= mu/eps. Once a block needed the redo, the next is scaled first. Probe (review's cases and more, base / new): `{400,400}` r4 1.2e153 NaN / rel 2.5e-15; `{64,64,64}` r8 1e153 NaN / 2.5e-15; f32 `{256,256}` r2 2e18 NaN / 0; `{9,9,9,9}` r8 at 1.5e-153 and 1e-150 val_error 0 / 4.97e-16 and 6.02e-16 (5.72e-16 at scale 1); 1e-200 rel 1.0 / 1.9e-15; 1e200 and 1e300 NaN / 1.7e-15 and 4.3e-16; subnormal 1e-310 rel 1.0 / 2.0e-13; f32 1e-30 and 1e-36 rel 1.0 / 2.2e-7 and 3.4e-7, 1e30 NaN / 2.2e-7, subnormal 1e-41 rel 1.0 / 1.3e-5 (the data carries about 17 bits). f scaled by 2^k (k from -900 to 900, f32 -100 to 100) gives cores 1..d-1 bit for bit, the first core scaled exactly and the same val_error. Speed, one-function noinline programs, 7 interleaved rounds, two runs (new/base): smooth 16^4 r10 1.0129 / 1.0129, LUT6 16^6 r12 1.0177 / 0.9566, `{400,400}` r4 1.0002 / 1.0004, `{8,8,8}` r8 0.9939 / 0.9945, f32 `{256,256}` r2 0.9971 / 0.9958, `{16,16}` r2 0.9867 / 0.9882, `{9,9,9,9}` r8 1.0168 / 1.0172; base-wrong inputs: 1.2e153 0.1302 / 0.1314 (base ran all 10 sweeps on NaN), 1e-200 1.3841 / 1.3820 and 2^-664 1.4022 / 1.4039: base's QR skipped every reflector there (all squares zero), so it did less work for a wrong answer; new at 1e-200 costs 258.8 us against base's 254.1 us at scale 1 on the same f, and at 2^-664 1.014 of new at scale 1, so the cost is the factorization a correct answer needs. Tests: 5 new cases in `crd-hesap-tensor-tt-tests` (14 cases, 538 assertions), the four that compile against base all fail there. |
| `link_models.cpp`, table-shaped `DelayProcess` (the fuzz finding) | a table entry past 2^53 read back as its nearest double: the one-entry table 1302123111085380114 ns gave a delay up to 2^7 ns past its own entry, and the f64 floor and ceiling comparisons rounded the same way (a flat table of 2^62 - 1 returned 2^62) | fixed 2026-11-10 (commit named in "Not landed"). The cause was the i64 to f64 conversion of the entries, not the interpolation. `table_at` now rounds the interpolated value and clamps it to the segment's two entries, as doubles and then as the exact integers, and `next()` holds a table delay to the floor and ceiling in integers; every step is monotone in the rank, so the table law stays non-decreasing, and below 2^53 the result equals base's (the delay digest test is unchanged). Regression test: entries 1302123111085380114, 2^62 - 1 and -2^62 + 1 read exactly, and two entries 1000 ns apart under 2^62 stay between them; base fails 4 of its checks. The crash input is adopted as `corpus-link/80091f88ff98ddeb147a5dfa045019550510666d` (34 files). `crd-hesap-net-tests` 176 cases, 3,800,751 assertions. |
| `gamma.hpp`, `gamma_sign` | the sign inversion on negative non-integers was already fixed by 4234137c; left: the value at the poles was undefined (+1 at -0 where Gamma(-0) = -inf, -1 or +1 at the negative integers where `gamma()` is NaN) and a NaN, -inf or a magnitude of 2^63 and more was converted to `long long`, undefined behaviour | fixed 2026-11-10 (`fix(hesap-special): define gamma_sign at the poles and drop its undefined conversion`): gamma_sign is the sign of `gamma()`'s value, +1 at +0 and +inf, -1 at -0, NaN at the negative integers, -inf and NaN, so gamma_sign(x) exp(lgamma(x)) carries gamma(x)'s sign everywhere; a magnitude of 2^(digits - 1) and more is a pole before any conversion, and the parity comes from the truncation of x (no floor). Test: every interval to 4096 and then out to 2^51 (f32 2^22), C's tgamma on 160 intervals, the poles out to -DBL_MAX, the specials, both precisions; base fails 22 of 9225 `[gamma]` assertions. No caller computes its own sign wrongly: `struve.hpp`'s Gamma(nu + 1/2) sign from sin(pi |nu + 1/2|) agrees with gamma_sign off the poles, which it excludes. Speed (gamma_sign behind one noinline call, new/base, two runs): f64 mixed 0.5992 / 0.6003, f32 mixed 0.6512 / 0.6499, f64 negative 0.5328 / 0.5277; a first form that kept the floor ran f32 at 1.0270 / 1.0268 and was replaced. |
| `hesap-dense` QR (found while testing the `tt_cross` guard) | a column 2^-520 to 2^-530 below the others in a 400 x 2 block: subnormal squares give an inaccurate column norm and a reflector off orthogonal, \|Q^T Q - I\| 9.5e-13 and 3.3e-6 (2^-560: squares flush to zero, 1.1e-16), at every scale of the block | fixed 2026-11-10 (`fix(hesap-dense): rescale a QR reflector whose sum of squares cannot be trusted`). `panel_factor_qr_transposed` (both the unblocked and the blocked path) forms alpha^2 + |tail|^2 plainly as before and trusts it when finite and at least mu/eps; otherwise `reflector_rescaled` scales the column by the exact power of two that brings its peak into [1/2, 1), in the manner of dlarfg's safe-minimum rescale (our own code), and returns beta through ldexp. One rare branch carries that and the zero tail. Before / after on 400 x 2 with column 0 at 2^k: k = -520 \|Q^T Q - I\| 9.5e-13 / below 1e-14, k = -530 3.3e-6 / below 1e-14, k = -560 the column's direction lost (A != QR in that column) / column error below 1e-13; matrices at 1e300 (NaN before) and 1e-300, f32 columns at 2^-60 to 2^-100 and f32 matrices at 1e30 and 1e-30, and a 300 x 140 blocked case: all orthogonal to 1e-14 (f32 1e-5). Test `QR: a column far below its neighbours ...` (`[range]`, 34 assertions): base fails 14. Bit identity on ordinary inputs: factor_qr packed and taus, lstsq (QR) and rsvd results identical, base against new, on 56 digests (14 shapes from 1 x 1 to 500 x 200 and wide 40 x 60, four kinds including column scales 2^-30 to 2^24 and upper-triangular zero tails, f32 and f64); `crd-hesap-dense-tests` 385 cases, 374,397 assertions; `crd-hesap-tensor-tt-tests` passes (the guard's 1e153 case now vouches, since the QR handles that column; 1.5e307 still overflows). Speed, new/base, two runs: factor_qr 400 x 4 1.0028 / 1.0012, 512 x 8 1.0023 / 1.0006, 4096 x 10 1.0072 / 1.0040, 64 x 64 0.9987 / 0.9948, 500 x 200 blocked 1.0021 / 0.9999, f32 400 x 4 0.9066 / 0.9055; callers lstsq QR 2000 x 100 0.9887 / 0.9896, rsvd 1000 x 300 rank 20 0.9939 / 0.9808, tt_cross smooth 16^4 r10 1.0308 / 1.0328. The tt_cross row never reaches the new path (a breakpoint on `reflector_rescaled` is hit 0 times), and a layout control, base's qr.cpp with an unused function of the same size, measures 1.0258 / 1.0256 on it, so new against that control is 1.005 / 1.007: the row moves with the binary's layout, not with the QR. Two earlier layouts of the same check measured 400 x 4 at 1.030 and f32 400 x 4 at 1.047 and were replaced. Found with it: `detail::make_householder` and `make_householder_complex` (tridiagonalization, Hessenberg, COD) sum their tails plainly too; eig_sym on a 60 x 60 matrix whose first column tail is 2^-530 below the rest gives \|V^T V - I\| 3.4e-6 and a residual 3.7e-5 (2^-520: 7.7e-12); fixed in the next row. |
| `hesap-dense` `detail::make_householder`, `make_householder_complex` (found with the QR fix) | the tail's squares summed plainly, as in the QR: eig_sym on a 60 x 60 matrix whose first column's off-diagonal is 2^-530 below the rest left \|V^T V - I\| 3.4e-6 and a residual 3.7e-5 of the matrix's peak (2^-520: 7.7e-12 and 8.3e-11); a tail at 2^-560 lost its direction; the same reflector serves the tridiagonalization, Hessenberg, bidiagonalization (SVD), COD and column-pivoted QR | fixed 2026-11-10 (`fix(hesap-dense): rescale a Householder tail whose sum of squares cannot be trusted`): the plain sum is kept when |alpha|^2 + |tail|^2 is finite and at least mu/eps, otherwise `scaled_column_norm` (real and complex) forms the norm on values scaled by an exact power of two and returns it through ldexp; dlarfg's safe-minimum loop is unchanged. Tests (`[range]`): the reflector's orthogonality tau (1 + \|v\|^2) = 2 (unitary form for the complex one) for tails at 2^-520 to 2^-1000 with alpha 0 or of the tail's order, and at 1e300; eig_sym's eigenvectors at 2^-520, 2^-530, 2^-560 to 1e-13; base fails 32 of 68 `[range]` assertions. Bit identity on ordinary inputs: eig_sym (vectors and values, f64; values f32), eig_herm, eig, svd and COD lstsq, base against new, on 18 digests (n from 1 to 160, two kinds including scales 2^-24 to 2^24); `crd-hesap-dense-tests` 387 cases, 374,431 assertions. Speed, new/base, two runs: make_householder n 8 1.0088 / 1.0087, n 200 1.0087 / 1.0098, complex n 200 0.9984 / 1.0038, eig_sym 64 0.9802 / 0.9817, eig_sym 200 0.9961 / 0.9984, eig 120 0.9974 / 0.9993, COD lstsq 500 x 100 0.9973 / 0.9951, eig_herm 100 0.9994 / 1.0023, svd 150 0.9946 / 0.9946. A first layout, which formed the scaled norm's ldexp inline, measured n 200 at 1.0339 in both runs and was replaced. |


Measured not to be defects, with the numbers:
- 29 to 32 of 6912 Ridders rows newly over the limit, all at h of 0.01 or less where the rounding noise of f is already over it; new is better on average (mean log10 of new/base error -0.12).
- 384 line-search rows with `ok` false, all at an evaluation budget of 3 or 5.
- 412 Ridders rows over 1e-12 on both trees, with the same numbers.
- The standalone `dcstep` at 1.068 and 1.038 in an earlier variant, closed by a later one (0.9773 and 1.0168).
- 27 `elliprj` points at absurd arguments disagreeing up to E = 2.47: about 1e-12 relative error on both trees against an 80-digit reference.
- The `smlnum` start test in QMR, which reports Breakdown for a right-hand side below about 1e-146 and never a wrong answer.
- Token overlap in the solver files, 3 % exact and 8 % rename-blind at most.

Recorded as limits by the workflow, then queued to be fixed: complex J and Y at nu near |z| of 35 and up (about 1e-7), complex K
near |z| of 9, Struve L at nu of -100 and below, and `gamma_sign(x)` inverted for negative non-integer x (no caller; outside the
group's files; the inversion was already fixed by 4234137c, the rest in "Defects found by the comparison"). The Bessel landing record adds Y(-188.4, 3.148) and Y'(-106.57, 0.104) at -inf where the truth is finite near
1e308, and four complex I points at 1.07 to 1.51 of the bar. Their outcome is the table below.

### Bessel, Airy and Struve limits closed (2026-11-10)

Base: 66e552ba. References: mpmath 1.4.1 at 70 digits at the exact double inputs (Struve L and H below x = 80 by their
series at two precisions 120 digits apart, J, Y and slopes at x > 1000 by Hankel's expansion at the base order and the
recurrence at 110 digits; both agree with mpmath's own functions to 4.6e-30 where those converge). Units: the module bar
1e-13 + 1e-12|ref| for the real functions, the documented complex bars (J, I 1e-11 + 1e-10|ref|, Y, K 1e-11 + 1e-9|ref|, on
the complex difference) for the complex ones. Speed: one function per program behind a noinline wrapper, 7 interleaved
rounds pinned to one core, two runs, new/base. Evidence: `build/gpl-compare/bessel-limits/` (machine cerid-track-incomplete).

| Item | Found on base | Outcome |
|---|---|---|
| Y(-188.4, 3.148), Y'(-106.57, 0.104) at -inf | both -inf (truth -8.9e307, -7.5e307); a sample at the end of the doubles (seed 1114, 4000 points, log10\|Y_\|nu\|\| 296 to 316) has 70 non-finite results for finite truths in J, J', Y and Y' of negative order and Y' of positive order (Y'_354.8(35.38) = 1.05e308); random negative orders (seed 1104, 3000) have none | fixed (`fix(hesap-special): keep J, Y and their slopes finite where Y of the reflected order leaves the doubles`). Where the plain value is not finite, Debye's estimate of ln\|coefficient·Y\| (a lower bound without a logarithm first) settles the results beyond the doubles, and the rest comes from a scale-carrying Y recurrence (exact 2^-800 steps), the reflection coefficient applied before the scale; the real Hankel functions share one estimate and one recurrence. Gate (seed 4004, 80,000 points: J 20,029, J' 20,099, Y 19,844, Y' 20,028; half at the edge, a quarter negative orders, a quarter controls): base over the bar 105 / 208 / 91 / 240 (all infinite), new 0, worst 0.039 units; newly over 0, finite to non-finite 0. Speed (two runs): the inputs base answered wrongly cost the second recurrence, J 1.659 / 1.653, Y 1.566 / 1.567, H^(1) 2.114 / 2.115 (the plain result is gone once its recurrence overflows; testing every call in advance would cost all of them); results beyond the doubles (base right) J 1.010 / 1.011, Y 1.008 / 1.008, H^(1) 1.015 / 1.011, Y' 0.998 / 1.000; every other row within 2 % in at least one run (Jp and Yp rows at 0.86 to 0.89, layout). Test: 13 assertions, 9 fail on base; special 117 cases, 413,676 assertions, stats, dsp, quadrature pass. |
| Struve L at nu of -100 and below (1e-9 at nu = -100, x = 63) | L(-100, 63) is 0.0485 units on base (the double-double sum of D12); a survey (seed 1103, 2000 points, nu -300 to -25, x 0.5 to 400) has 66 over the bar: NaN at every half-integer below about -175 (the first term (x/2)^(n+1/2)/Gamma(n+3/2) of the series from m = n), 0 where (x/2)^(nu+1) underflowed before the division by Gamma(nu+3/2) (L(-170.18, 195.7) = 9.0e52), up to 25 units near a half-integer (gamma()'s reflection rounds pi s before the sine, and the sum stopped before the terms after the crossing m = -nu - 3/2 grow back), 3 % low near x = 600 (the 500-term cap) | fixed (`fix(hesap-special): keep Struve L right near and at negative half-integer orders and where its series leaves the doubles`). The first term at a negative s = nu + 3/2 takes 1/Gamma(s) = sin(pi s) Gamma(1 - s)/pi with the turn reduced exactly, and is carried as a logarithm where it is not a normal double; below nu = -30 the sum of the ratios to it is formed (double-double where it cancels, up to 4000 terms); the series stops at a negligible term only where a bound of the terms after the crossing allows it; a half-integer order whose series leaves the doubles is I_(n+1/2) (DLMF 11.4.4). Gate (seed 3003, 20,000 points, x 0.1 to 600: nu -300 to -30, near and at half-integers, -30 to -1.5, controls): base over the bar 3,488, new 0, worst 0.484 units; newly over 0, finite to non-finite 0; Struve H (seed 3013, 20,000 negative orders, which share the series) unchanged, 3,527 over in both (item 7). Speed (two runs): base-wrong inputs: half-integers below -175 1.360 / 1.356 (I_(n+1/2)), underflowed first terms 2.065 / 2.063 (the logarithm and the double-double sum the cancellation needs); every other row within 2 % in at least one run (nu = -180.7 at 0.84). Test: 12 assertions, all fail on base; special 118 cases, 413,688 assertions. |
| Complex J and Y with the order near \|z\| of 35 and up (about 1e-7; J_50(60 + 10i) 3.4e-7), and four complex I points at 1.07 to 1.51 of the bar | the named points are within the bars on base (J_50(60 + 10i) 0.0097 of the bar, the D11/D19 methods); a survey of the region (seed 1101, 3000 points, nu/\|z\| 0.5 to 1.5, \|z\| 35 to 300, all arguments) has 77 over the bar, 74 of them NaN where the truth is finite (J_203.5(7.65 + 138.2i)); complex I over the plane (seed 1105, 4000) 8 over, 6 NaN at \|z\| 350 to 530 and I_-92.52(-576 - 115i) 0.16 off | fixed in part (`fix(hesap-special): keep complex J, Y and I finite where the series' first term or Y's integer series leaves the doubles`). The cause of the NaN: the series was priced by the sum of its ratios alone while its first term (z/2)^nu/Gamma(nu+1) left the doubles; the far paths' last series now forms that term through logarithms where it is not a normal double, and where the result is still not finite (or Y's integer series was cut short where the product of its Gamma left the doubles: Y_63(34.2 + 101.4i) 1.2e-8 off) the stable method answers; at a negative order I's series is priced as the series of the order itself. Gate (seed 1515, 80,000 points, half nu near \|z\| from 35, a quarter \|z\| 8 to 35, a quarter controls; J 20,105, Y 19,869, I 20,091, K 19,935 as a control): base over the bar 505 / 478 / 943 / 223, new 17 / 27 / 15 / 103 (K: the next item); newly over 0, finite to non-finite 0. Not cleared: J 17 at negative orders of 1.2 to 1.5 times \|z\| with a large imaginary part (the series of J_nu priced as that of \|nu\|; up to 1e22 times the bar, as base), I 15 at 1.05 to 2.37 times the bar (the series' price undershoots by the index of its largest term: the item's class), Y 27 (13 where the truth is beyond the doubles at \|z\| below 0.8 and orders 138 to 148, NaN instead of infinities as in base; 14 finite at near-integer and negative orders, up to 2e4 times the bar). A pricing variant (branch `bessel2-pricing`, d908400f: the signed price of a negative order, the index model and a third of the bar for I's series) clears J and I to 0 and Y to 23, at 1.42 (J, nu = -20.3), 1.14 (Y, nu = 50.5), 1.24 (Y, nu = -20.3) and 1.07 (I, nu = 50.5) on rows base answers right: not landed, for the lead to rule. Speed (two runs): base-wrong rows I 1.890 / 1.904 (the stable method), J 0.540 / 0.533, Y 1.014 / 1.011; Y at integer order 63, \|z\| 15 to 120, 1.025 / 1.021 as a row, of which its 194 points that base answers right 1.012 / 1.013 and its 6 wrong ones 1.129 / 1.128; every other row within 2 % in at least one run. Test: 9 assertions, all fail on base; special 119 cases, 413,697 assertions. |
| Complex K near \|z\| of 9 on or near the positive real axis (about 2e-7) | K_5(9 + 0.5i) is 9e-9 of the bar on base (Steed's CF2 from \|z\| = 8, D11); the error lies below \|z\| = 8: near the axis at \|z\| 6 to 13 (seed 1102, 2000 points) 8 over the bar (1.04e4 times it at K_4.0001(6.18 - 1.92i), 1.9 at K_0(7.04)), and over all arguments below \|z\| = 8.5 (seed 1122, 4000 points, 40 % within 1e-11.5 to 1e-1 of an integer order) 831 over, up to 1.9e17 times the bar within 3e-12 of an integer: the connection (pi/2)(I_-nu - I_nu)/sin(nu pi) divides by a small sine, and its series and the integer series hold terms of e^(\|Re z\|) to make K, e^(-Re z) | fixed (`fix(hesap-special): keep complex K right below |z| of 8 near integer orders and where its series cancel`). Below \|z\| = 8 the connection is priced by the cancellation of its difference and the integer series by its three parts (from \|z\| = 4, below which they are within the bar), and within 0.02 of an integer the connection is not taken at all; where the price is above the bar's tenth (the integer series' hundredth) Temme's series at the base order (Temme 1975, at complex z) and the recurrence upward answer below \|z\| = 2, Steed's CF2 above it in the right half-plane, and K_nu(z) = e^(-+nu pi i) K_nu(-z) -+ pi i I_nu(-z) in the left. Gate (seed 2002, 20,000 points: 60 % below \|z\| = 8.5 as the survey, 25 % near the axis at \|z\| 6 to 13, 15 % controls at \|z\| 8 to 60): base over the bar 2,572 (worst 1.4e18 times it), new 0, worst 0.663; newly over 0, finite to non-finite 0. In the complex gate's K controls (seed 1515) 99 stay over as in base, outside this region: 19 where the truth is beyond the doubles (orders 140 to 150, \|z\| below 0.83: NaN instead of infinities) and 80 at orders of -350 to -440 with \|z\| of 250 to 300 (not cleared). Speed (two runs): within 1e-6 of an integer order (base wrong) 2.608 / 2.618, the method that does not divide by sin(nu pi); integer order 0 1.018 / 1.019 and 3 1.022 / 1.017; every other row within 1.2 %, most below 1. Test: 7 assertions, all fail on base; special 120 cases, 413,704 assertions. |
| J, Y, J', Y' for nu above about 100 at x above about 9000 (CF1 capped; Hankel's expansion at the order needs x > nu^2/2) | measured first: base answers within the bar (D7/D13: Hankel's expansion at the base order and the recurrence in order, Debye's expansion from nu = 2e5): survey (seed 1106, 1500 points) 0 over, worst 0.083 units; confirmation (seed 6006, 6000 points: nu 100 to 2e4 log, a tenth to 2e5, x from 9000 or 1.05 nu to 1e7) 0 over in base and in the head, worst 0.077 units (J 1494, J' 1460, Y 1551, Y' 1495) | closed by measurement, no code; the references are Hankel's expansion at the base order with the recurrence at 110 digits, which agree with mpmath's own functions to 3.9e-30 where those converge |
| Struve H's series crossover x of 10 to 16 (about 1e-10, 45.7 units; within its documented 1e-9) | a sample (seed 7007, 20,000 points: 60 % x 10 to 16, its edges, controls; orders -60 to 80, a fifth near negative half-integers) has 2,940 over the module bar, worst 5.1e5 units; negative orders (seed 3013, 20,000) 3,527, worst 1.4e6: next to the crossover's cancellation, 1/Gamma of a negative argument near a pole from gamma()'s reflection, which rounds pi s before the sine, in the series' first term, in the asymptotic expansion and in the quadrature's factor | near the poles fixed (`fix(hesap-special): keep Struve H right near negative half-integer orders`): within 0.25 of a pole the reflection takes sin(pi s) from the exactly reduced turn (outside the crossover, where a new rounding of the series' first term would move its cancelling result). Seed 3013: 3,527 over to 20 (all in the crossover, as base), seed 7007: 2,940 to 739 (the crossover); newly over 0, finite to non-finite 0. Speed (two runs, against base; against its parent within 2 %): every row within 2 % in at least one run except the two of the row above (nu = -40.3 1.047 / 1.028, -60.7 1.046 / 1.021). Test: 6 assertions, all fail on base; special 121 cases, 413,710 assertions. The crossover itself is not landed, by the rule for this item: the downward recurrence (DLMF 11.4.23, struve_h_down) for x 10 to 16 and orders -10 to 10 (-13 from x = 14) brings both samples to 0 over (worst 0.375 and 0.292 units), but its rows of the range run at 1.11 to 1.18 of base in both runs (nu = -8.3: 1.121 / 1.176; -0.54: 1.132 / 1.130; 0: 1.160 / 1.149; 2.5: 1.133 / 1.146; 7.7: 1.114 / 1.117): branch `bessel2-crossover`, for the lead to rule. |
| Struve H at orders of -40 and below, after the Struve L row (found while measuring item 7) | the stop test of the series that the L row added is shared with H and was asked at every negligible term before the crossing: H at nu = -40.3 ran 1.092 / 1.074 and at -60.7 1.114 / 1.110 of base (two runs; the L row's own H row, nu = -12.3, was within 2 %) | improved (`fix(hesap-special): ask Struve's stop test once before the crossing, with a tighter bound`): the test is asked once before the crossing, a test without a logarithm, a division or a root settles the usual case, and the product of (j + 1/2) is bounded by Jensen. Now 1.029 / 1.047 and 1.025 / 1.027 against base: still above 2 % (about 10 ns of a stop test once per call), not cleared. Accuracy unchanged in class: Struve L (seed 3003) 0 over, H (seeds 3013, 7007) as before, newly over 0. |

### Fuzz recount

`crd-fuzz-hesap-link` (clang 18.1.3, `linux-clang-fuzz`, commit 80b51f9d, 15-minute run, corpus of 33 files) stopped on an oracle
failure after 14,081 executions in 1.8 s, exit 77. The check at `fuzz_link.cpp:342` failed: a table-shaped `DelayProcess` returned a
delay outside its own table range, and replaying the artifact fails the same way. The hypothesis that the interpolation rounds
past the ends was close but not the cause: the input is a one-entry table, and the entry itself, 1302123111085380114, became a
different double. Fixed in the model (see "Defects found by the comparison"), the oracle unchanged, the artifact adopted with
`scripts/fuzz.py adopt`. Recount on the fix (clang 18.1.3, `linux-clang-fuzz`, 15 minutes, corpus of 34 files, scratch directory):
10,359,036 executions in 901.2 s (11,497 per second), exit 0, no artifact, corpus 34 to 723 in the scratch copy, peak RSS
434 MiB. This replaces ROADMAP row 122's count.

Validators: `python scripts/check-master-plan.py`, `python scripts/check-repository.py`.
