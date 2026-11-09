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
| CSparse, CHOLMOD and UMFPACK group, `link_models` | see the companion note | LGPL, GPL | comments and tokens | cleanup running |
| `hesap-special`: `bessel.hpp`, `airy.hpp`, `elliptic.hpp`, `expint.hpp`, `incomplete.hpp`, `fresnel.hpp`, Dawson in `erf.hpp`; `hesap-opt/powell.hpp`; `hesap-diff/finite_difference.hpp` | Numerical Recipes | proprietary | comments name the routines; `bessel.hpp` Steed CF2 and `betacf` and `mnbrak` read by hand | queued for rewrite |
| `hesap-iterative/qmr.hpp` | Krylov.jl `qmr.jl` | MPL-2.0 (LICENSE read) | comment: "transcribed VERBATIM" | queued for rewrite |
| `hesap-tensor/tt.hpp`, `tt_cross` | tntorch `cross.py` | LGPL-3.0 (repository licence) | comment names the scheme; text not yet compared | compare, replace if translated |

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
| L-BFGS-B-C | BSD-3 | `lbfgsb.hpp` | notice added; its `lnsrlb` line search, which follows MINPACK-2 `dcsrch`, joins the rewrite list |
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

Validators: `python scripts/check-master-plan.py`, `python scripts/check-repository.py`.
