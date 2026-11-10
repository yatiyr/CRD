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
| `hesap-special`: `bessel.hpp`, `airy.hpp`, `elliptic.hpp`, `expint.hpp`, `incomplete.hpp`, `fresnel.hpp`, Dawson in `erf.hpp`; `hesap-opt/powell.hpp`; `hesap-diff/finite_difference.hpp` | Numerical Recipes | proprietary | comments name the routines; `bessel.hpp` Steed CF2 and `betacf` and `mnbrak` read by hand | `incomplete.hpp` (82c2f8a2), `expint.hpp`, `elliptic.hpp`, `fresnel.hpp` and Dawson (8effc95d), `powell.hpp` and `finite_difference.hpp` (8b0c3f86) rewritten and landed; `bessel.hpp` and `airy.hpp` (with Struve) defect fixes not landed, see below |
| `hesap-iterative/qmr.hpp` | Krylov.jl `qmr.jl` | MPL-2.0 (LICENSE read) | comment: "transcribed VERBATIM" | rewritten and landed (784ffee6) |
| `hesap-tensor/tt.hpp`, `tt_cross` | tntorch `cross.py` | LGPL-3.0 (repository licence) | comment names the scheme; text compared | `tt_cross` is native C++ (pools, Philox streams, LU-solved core) and stays; maxvol stays as an MIT port with its notice added (df7af99c) |

The Numerical Recipes group was missed by name because the book is not in the corpus. It was found through GSL and libstdc++,
whose Steed CF2 text is similar to it. That shared lineage is inferred, not traced.

## `incomplete.hpp` and `gamma.hpp`: defects fixed before the rewrite

Landed as a512603c (one commit on 0971ef5f). The rewrite of the Numerical Recipes text above is still queued. This commit fixes
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

## Findings: permissive sources without their notices

About 56 files say they follow permissive or public-domain code, and only `dlpack.h` keeps a notice. No NOTICE file exists.

| Source | Licence | Files | Outcome |
|---|---|---|---|
| LAPACK | BSD-3 | `hesap-dense`: 15 | notice added (15 files) |
| SciPy | BSD-3 | `hesap-dsp` 8; `hesap-ode` 3; `gcrot`, `nelder_mead` | notice added (13 files) |
| NLopt (`newuoa`, `bobyqa`, `cobyla`) | MIT (COPYRIGHT files read) | 3, with 3,492 exact tokens | notice added (3 files) |
| L-BFGS-B-C | BSD-3 | `lbfgsb.hpp` | notice added; its `lnsrlb` line search, which follows MINPACK-2 `dcsrch`, rewritten with `more_thuente_line_search.hpp` (0746716d) |
| QUADPACK | public domain through SLATEC, settled by the [code-freedom note](2026-11-07-code-freedom-rule.md) | `qags`, `qng`, `oscillatory`, `gauss_kronrod`, `adaptive` in `hesap-quadrature` | notice added (5 files) |
| Ruckig; opt_einsum; IterativeSolvers.jl; PyWavelets; maxvolpy | MIT | 2; 1; 1; tables; `tt.hpp` maxvol | notice added; `tt.hpp` maxvol gets its notice when the `tt_cross` rewrite closes |
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
| `bessel.hpp`, `airy.hpp`, Struve | defect fix not landed | The review found three regressions against base (complex Y at negative integer order with Re z < 0 gives NaN on 28 of 2500 points; complex K at half-integer order with Re z < 0 is over the bar on 3 points; J_nu(-0.0) for nu > 0 is NaN where base is 0), more defects (I and I' infinite where the true value is finite for x above 710; K and K' zero at x of 745 and up with large order; J and J' NaN where the true value underflows; complex J and I NaN at every negative integer order; complex I and K over the bar at large imaginary part), and speed misses (Airy series +42 % to +60 % on x in [-9, -5.5], where base was wrong; J asymptotic +43 % to +64 % at nu of 5 and up; two complex J rows at 2.97 and 1.96 of base). |
| `incomplete.hpp` accuracy fixes (second group) | not landed | Review: the uniform expansion far from the mode at a+b of 5e5 and up runs at 3.8 times base's time on points where base is correct (99.95 %); the small-shape expansion far from the mode runs at 1.96 times; the window widths were derived from base's plain prefactor, not the fallback the head uses. The 82c2f8a2 rewrite above landed without these fixes. |
| `elliptic.hpp` defects (`ellint_f`, `ellint_e_inc`, `ellint_pi`, `elliprj`, infinities) | not landed | Review: `1 - m sin^2(phi)` cancels near phi = pi/2 with m or n near 1 (`F(phi, 1)` at the double nearest pi/2 is 38.718 for a true 38.025, 1.8e10 units); `Pi(1, phi, 0.5)` stays NaN at three phi values. `elliprc` and `elliprd` still return NaN for an infinite argument, because the check costs 2.1 % to 3.9 % on `elliprc` and 2.8 % to 3.0 % on `elliprd`. |
| `tt_cross` range defect | not landed | Values below about 1e-155 or above about 1e153 (f64; 1e-19 and 1e19 in f32) give an all-zero TT with relative error 1.0 and `converged` true, or NaN. The fix covered one element's range, not the sums: `{400,400}` rank 4 at 1.2e153, `{64,64,64}` rank 8 at 1e153 and `{256,256}` at 2e18 in f32 still give a NaN TT, and the validation error underflows 16 decades above the reference norm. |
| `fuzz_link` | open | See the fuzz recount below. |

### Defects found by the comparison

| Group | Found | Outcome |
|---|---|---|
| `powell.hpp`, `finite_difference.hpp` | `brent_bracket` probed from the vertex itself after the slide, so mid equalled hi and the minimum was off by 1.1 to 95 for s of 5 to 100; `derivative_ridders` divided by the nominal 2 x step at large x (1.3e-12 at x = 1e3, 4.6e-9 at 1e6); h = 0 gave NaN | fixed, ba2f790e. Regression tests fail on base (56 + 56 assertions) and pass on new. 476 of 6912 Ridders rows fixed. |
| `qmr.hpp` | a serious Lanczos breakdown (`<p,q>` = 0 with q not 0) reported convergence: on A = [[1,1,-1],[1,2,0],[1,0,3]], b = e1, true residual sqrt(2) | fixed, 983ae862, reported as Breakdown. A first draft that stopped at the previous iterate lost an answer base got right (residual 1e-14); the final form finishes the step with norm(q). |
| `more_thuente_line_search.hpp`, `lbfgsb.hpp` | a non-finite trial value, or an overflowed step, ended in alpha NaN after 64 evaluations (5158 and 12793 failures in the random probe) | fixed, 4d028d20: retreat to the midpoint, bisection of an overflowed step. 0 failures; finite inputs identical. |
| `bessel.hpp`, `airy.hpp`, `incomplete.hpp`, `elliptic.hpp`, `tt_cross` | see "Not landed" | open |

Measured not to be defects, with the numbers:
- 29 to 32 of 6912 Ridders rows newly over the limit, all at h of 0.01 or less where the rounding noise of f is already over it; new is better on average (mean log10 of new/base error -0.12).
- 384 line-search rows with `ok` false, all at an evaluation budget of 3 or 5.
- 412 Ridders rows over 1e-12 on both trees, with the same numbers.
- The standalone `dcstep` at 1.068 and 1.038 in an earlier variant, closed by a later one (0.9773 and 1.0168).
- 27 `elliprj` points at absurd arguments disagreeing up to E = 2.47: about 1e-12 relative error on both trees against an 80-digit reference.
- The `smlnum` start test in QMR, which reports Breakdown for a right-hand side below about 1e-146 and never a wrong answer.
- Token overlap in the solver files, 3 % exact and 8 % rename-blind at most.

Left as documented limits: complex J and Y at nu near |z| of 35 and up (about 1e-7), complex K near |z| of 9, Struve L at nu of
-100 and below, and `gamma_sign(x)` inverted for negative non-integer x (no caller; outside the group's files).

### Fuzz recount

`crd-fuzz-hesap-link` (clang 18.1.3, `linux-clang-fuzz`, commit 80b51f9d, 15-minute run, corpus of 33 files) stopped on an oracle
failure after 14,081 executions in 1.8 s, exit 77. The check at `fuzz_link.cpp:342` failed: a table-shaped `DelayProcess` returned a
delay outside its own table range, and replaying the artifact fails the same way. Untested hypothesis: `table_at` interpolates in
f64 near 2^62 (`kMaxLinkDelayNs`), above 2^53, so rounding can pass the table ends. Fixing `table_at` or the oracle, then
`scripts/fuzz.py adopt`, is the next step. ROADMAP row 122's 8.2 million executions has no replacement count.

Validators: `python scripts/check-master-plan.py`, `python scripts/check-repository.py`.
