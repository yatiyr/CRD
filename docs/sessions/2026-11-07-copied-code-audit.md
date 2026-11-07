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
