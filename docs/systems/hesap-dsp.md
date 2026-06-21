# crd-hesap-dsp — DSP cluster (core + adaptive)

> Phase 3.1.6 **v11**. ADR-0093. Plan: `docs/phases/phase-3.1.6-hesap.md` (the v11 block + the promoted sub-slice
> table). Status: **COMPLETE (a–t)** — the full analysis surface. Suite 100 cases / ~27,069 assertions GREEN on
> linux-gcc + win-clang-cl + win-release. CLI `hesap.dsp.{welch,resample}.f64`. Siblings `crd-hesap-wavelet` +
> `crd-hesap-comms` likewise complete. Sessions: `docs/sessions/2026-06-21-v11-dsp-core.md` + `…-g-h-k-l.md`.

## What it is

The MAXIMAL DSP subject for every Cerid domain (DAW, SDR/comms, audio, robotics/control, scientific analysis),
delivered as **three isolated modules** (module-isolation cornerstone — a DAW build links `crd-hesap-dsp`, not
comms) + a `crd-units` addition:

- **`crd-hesap-dsp`** — core DSP + adaptive (this doc; sub-slices v11-a…t)
- **`crd-hesap-wavelet`** — wavelets (→ `crd-hesap-fft`; v11w-a…e) *(planned)*
- **`crd-hesap-comms`** — comms/SDR (→ `crd-hesap-dsp`; v11c-a…g) *(planned)*
- **`crd-units`** — `DecibelRatio`/`DecibelPower` (logarithmic I/O lenses, NOT linear `Quantity`) + `NormalizedFrequency`

## The honest-gate split (ADR-0093 — load-bearing)

The single most important design decision: filter **correctness** is two different things.

- **Filter DESIGN** (`ellip`/`cheby2`/`remez`/`firls` — transcendental + iterative numerics) gates on
  **SPEC-COMPLIANCE** (passband ripple ≤ Rp · stopband ≥ Rs · equiripple alternation count) **+ coefficients to
  10 significant digits** vs scipy/MATLAB — **NOT bit-match** (`std::sin` is not bit-identical across libm; the
  ULP drift compounds through the iteration; spec-compliance is what MATLAB itself validates against, the stronger
  gate). *Closed-form designs (firwin/firwin2/savgol/RRC) DO get the full bit-match — the distinction is
  iterative-vs-closed-form, not all-design.*
- **Filter APPLICATION** (`lfilter`/`sosfilt`/biquad — pure multiply-add) gates **BIT-EXACT vs scipy** (same
  arithmetic, `-ffp-contract=off`, ADR-0078) **+ the {1..16} determinism MOAT** (block-streaming == one batch call,
  bit-identical). The moat scopes to application/streaming — design is one-time.

## The data-flow rule (locked v11-a)

Filters are **designed in zpk and converted zpk → sos directly**. Transfer-function (b/a) is an interop/output
format, **never a design intermediate**: roots-of-tf is Wilkinson-ill-conditioned and tf coefficients lose
precision badly above order ~8 (proven by an order-12 conditioning gate). `zpk_freqz` evaluates the factored form
(well-conditioned at any order); `zpk_to_sos` uses nearest-to-unit-circle pole/zero pairing (the conditioning
property that is the reason SOS exists). SOS is the default for high-order IIR.

## The two-layer streaming contract (ADR-0078)

Every filter ships **both**: a typed whole-array **batch** API (upper) and an allocation-free **stateful streaming
kernel** (lower) — Direct-Form-II-Transposed biquad cascades, the DAW/SDR real-time hot loop, with a fixed
per-element FP order so block-streaming output is bit-identical to a single batch call and across thread counts.

## Shipped surface (by header)

| header | contents |
|---|---|
| `filter.hpp` | `TransferFunction`/`Zpk`/`Biquad`/`SecondOrderSections` + zpk↔tf · zpk→sos (nearest pairing) · sos→tf |
| `polynomial.hpp` | poly roots (companion + `dense::eig`), `poly_from_roots`, Horner eval + derivative |
| `state_space.hpp`, `lattice.hpp` | tf↔ss (controllable-canonical + Faddeev-LeVerrier) · denom↔lattice (Levinson, \|k\|<1 stability) |
| `freqz.hpp` | `freqz`/`sosfreqz`/`zpk_freqz`/`group_delay` |
| `windows.hpp` | 24 windows: cosine-sum (`general_cosine`) · simple · periodic · Kaiser/gaussian/tukey/Taylor · **Dolph-Chebyshev** (FFT) · **DPSS/Slepian** (`dense::eig_sym`) · `enbw` |
| `fir.hpp`, `fir_special.hpp` | `firwin`/`firwin2` · Savitzky-Golay · raised-cosine/RRC |
| `firls.hpp`, `remez.hpp` | least-squares FIR · **Parks-McClellan equiripple** (the exchange algorithm) |
| `iir.hpp` | `buttap`/`cheb1ap`/`cheb2ap`/`besselap` + `bilinear_zpk`/`lp2lp_zpk` + digital `butter`/`cheby1`/`cheby2`/`bessel` |
| `elliptic_fn.hpp`, `ellip.hpp` | `ellipk`/`ellipj`/`ellipdeg`/`arc_jac_sc` (each gated vs scipy.special) + `ellipap`/`ellip` (Cauer) |
| `filtering.hpp` | `sosfilt_stream`/`sosfilt`/`sosfiltfilt` + `lfilter`/`lfilter_zi`/`filtfilt` |
| `convolution.hpp` | `convolve`/`fftconvolve`/**`FftConvolver`**/`correlate`/`oaconvolve`/`deconvolve`/`matched_filter` |
| `spectral.hpp` | `welch_psd`/`spectrogram`/`stft`/`istft`/`periodogram`/`csd`/`coherence` — **the multi-threaded FFT** |

## The multi-threaded FFT (v11-m)

Welch / spectrogram / STFT take **many independent segment FFTs** — the safe, high-value place for a parallel FFT
(NOT the single long×long four-step). Each `crd::jobs::parallel_for` job owns a `RealFftPlan` built serially up
front (so no thread-safe allocation is needed — the parallel region only executes plans + writes its own segment
rows), and the final average is a **serial fixed-order reduction**. The result is **bit-identical across {1, 2, 4,
8, 16} threads** — a determinism edge MKL/FFTW don't offer — and it crushes MATLAB `pwelch` 15.3× / scipy 11.5×.

## Performance (all gold standards, honest)

scipy.signal (free primary) · MATLAB R2026a (the MKL-backed authority) · liquid-dsp 1.6.0 · Intel IPP *(deferred)*.
Cerid **crushes the kernels it owns** — windows (1.9–10.4× scipy / 1.2–2.7× MATLAB), firwin (8.6× / 3.9×), firls
(41.8× / 3.4×), sosfilt application (1.11× scipy / 1.82× liquid), **Welch (11.5× scipy / 15.3× MATLAB `pwelch`)**.
FFT-bound ops are competitive once FFT plans are cached (`FftConvolver` 2.1× scipy) and crush on the many-FFT
target via the parallel batched FFT. Benches committed: `runtime/examples/bench_dsp_*_vs_refs.{cpp,py}` +
`_vs_liquid.c` + MATLAB `.m` baselines.

## Module edges (acyclic)

`crd-hesap-dsp` → `crd-hesap-fft` (spectral / fast-conv / chebwin / firwin2) · `crd-hesap-eigen` (DPSS, future
multitaper / MUSIC-ESPRIT / AR) · `crd-hesap-dense` (LU/eig for design solves) · `crd-jobs` (the parallel batched
FFT) · `crd-math` · `crd-units` · core/containers/memory. **No edge to `crd-hesap-opt`.** Lower-layer raw scalars
+ raw `Complex<T>` (ADR-0078 §5).

## Tests

`tests/hesap-dsp/` — 47 cases / 6252 assertions. Reference vectors are generated once into committed `.inc`
headers (`gen_*.py` / `.m` → `*.inc`, the SUNDIALS-tableau pattern — never per-test) as **plain C arrays** (no
owning STL anywhere, tests included). Application kernels carry the {1..16} bit-identity moat; the multi-threaded
spectral functions carry it across thread counts.

## Status — COMPLETE (a–t)

The whole analysis surface is shipped: filter design (FIR windowed/firls/remez/SavGol/RRC, IIR
Butter/Cheb/Bessel/Elliptic, RBJ biquads, IIR digital + order-est + notch/comb), filtering (lfilter/sosfilt/filtfilt
streaming), fast convolution, multirate, Hilbert, spectral (Welch/STFT/multitaper — the multi-threaded FFT), AR,
subspace MUSIC/ESPRIT, transforms (Goertzel/CZT/cepstrum/FWHT), waveforms + PN sequences, detection + measurements,
adaptive (LMS/NLMS/RLS/AP). CLI `hesap.dsp.{welch,resample}.f64`. The sibling `crd-hesap-wavelet`
(`docs/systems/hesap-wavelet.md`) and `crd-hesap-comms` (`docs/systems/hesap-comms.md`) modules are likewise
complete. v11-z close = CLI + the three system docs + the per-bench scoreboards (in the bench sources + session logs).
