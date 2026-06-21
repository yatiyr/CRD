# crd-hesap-wavelet — wavelet transforms (Phase 3.1.6 v11w)

> One short overview per shipped module. Story + decisions live in the session logs
> (`docs/sessions/2026-06-21-v11w-wavelet-module.md`); the decision is ADR-0093.

## Purpose

The wavelet sub-cluster of the v11 DSP subject, as its own module (a DAW links `crd-hesap-dsp`;
a scientific / medical-imaging / geophysics consumer links the wavelets). Gold standard:
**PyWavelets 1.8.0** (+ MATLAB Wavelet Toolbox). The filter-bank coefficients are GENERATED from
pywt (`scripts/gen_wavelet_coeffs.py` → `detail/wavelet_coeffs.hpp`, the FFT-codelet / SUNDIALS-tableau
pattern), so the engine matches pywt to machine precision.

## API surface (header → what it ships)

| Header | Contents |
|---|---|
| `families.hpp` | 76 wavelets (Haar, db1-20, sym2-20, coif1-5, bior + rbio, dmey); `wavelet_by_name`, `qmf`, `orthogonal_filter_bank`. |
| `dwt.hpp` | `dwt`/`idwt`/`wavedec`/`waverec` + all 9 pywt boundary modes (zero/constant/symmetric/reflect/periodic/periodization/smooth/antisymmetric/antireflect). |
| `swt.hpp` | `swt`/`iswt` — stationary (undecimated) à trous transform (shift-invariant). |
| `wpt.hpp` | `WaveletPacket` (full tree, node-by-path) + `best_basis` (Shannon-entropy Coifman-Wickerhauser). |
| `cwt.hpp` | `cwt` (mexh/morl/cmor/gaus1-8/cgau1-8/shan/fbsp/paul) + `central_frequency` (FFT-peak) + `cwt_ridge`. Multi-threaded over scales. |
| `dwt2.hpp` | `dwt2`/`idwt2`/`wavedec2`/`waverec2` — separable 2-D, multi-threaded rows+columns. |
| `denoise.hpp` | soft/hard/garrote `threshold` + `mad_sigma` + VisuShrink / BayesShrink / SureShrink + `denoise`. |
| `modwt.hpp` | `modwt`/`imodwt` — Percival-Walden maximal-overlap DWT (any length, energy-preserving). |

## Honest gate (ADR-0093)

Per-mode DWT coefficients **bit-match pywt** (1e-11); CWT matches pywt.cwt to ~1e-9 (precision-12 grid);
SWT/WPT/2-D vs pywt + perfect reconstruction; MODWT self-contained (PR + energy partition; pywt has no
modwt). Every application kernel carries the **run-twice / {1,4,16}-thread bit-identity moat** (CWT-over-scales
and 2-D rows/columns are parallel + bit-identical — the determinism differentiator pywt/MATLAB lack).

## Performance (vs pywt's C core, 1 thread, chk-identical ⇒ correct)

wavedec db4 **1.14-1.21×** · swt **1.18×** · CWT morl **1.17×** / cmor **2.94×** (MT-batched complex crush) ·
dwt2 1024² **1.49×**. EARNED via the reversed-filter branch-free interior (the first cut, switch-per-tap, was
7-8× slower). Benches: `runtime/examples/bench_wavelet_vs_refs.{cpp,py}` + `.m`, `scripts/run_bench_wavelet.sh`.

## Module edges (acyclic)

`crd-hesap-wavelet` → `crd-hesap-fft` (CWT FFT convolution) · `crd-hesap-dense` (eig consumers) · `crd-hesap`
(Complex<T>) · `crd-jobs` (MT batched CWT / 2-D) · `crd-math` · core/containers/memory. Lower-layer raw scalars
+ raw `Complex<T>` (ADR-0078 §5). CLI: `hesap.wavelet.{dwt,denoise}.f64` (`cli_register_wavelet.cpp`).

## Tests

`tests/hesap-wavelet/` — 24 cases / ~14,951 assertions (linux-gcc + win-clang-cl + win-release). Reference
vectors generated once into committed `.inc` (plain C arrays; `gen_*.py` → `*.inc`). PR + {1..16} moats gated.
