# ADR-0093 — `crd-hesap-dsp` (+ `-wavelet` + `-comms`): the DSP cluster — the design/application honest-gate split, SOS-by-default, the two-layer streaming contract

- **Status:** Accepted (2026-06-20)
- **Phase:** 3.1.6 v11 (DSP cluster)
- **Tags:** `hesap` `dsp` `filters` `determinism` `architecture` `module-edges` `substrate`
- **Plan:** `docs/phases/phase-3.1.6-hesap.md` (the v11 DETAILED PLAN); memory `project_v11_dsp_plan`; system docs `docs/systems/hesap-dsp.md` (at close)

## Context

v11 brings digital signal processing to the hesap stack — the MAXIMAL subject (user direction 2026-06-20: "every
functionality of the DSP subject"): the full Signal-Processing-Toolbox surface (filter design + filtering +
multirate + spectral + transforms + waveforms + detection + measurements) + adaptive filters + wavelets + comms/SDR.
Consumers span every Cerid domain: DAW (real-time biquad/EQ chains, replay determinism), SDR/comms, audio, robotics
and control (estimation, system ID), scientific analysis. Gold standards: **scipy.signal** (the free primary),
**MATLAB Signal Processing Toolbox** (the industry authority — R2026a, all toolboxes, installed), **Intel IPP** +
**ARM CMSIS-DSP** (kernel-perf crush), **liquid-dsp** (comms), **PyWavelets** (wavelets).

Three things had to be settled before the first filter shipped.

1. **What "correct" means for a filter** — and it is NOT one thing.
2. **The numerical representation a high-order filter lives in** — get this wrong and every design slice inherits a
   conditioning disaster.
3. **Module boundaries** — a DAW build must not link comms.

## Decision

1. **The design/application honest-gate split (load-bearing — the v10 honest-scoreboard scar applied):**
   - **Filter DESIGN** (`ellip`/`cheby2`/`remez`/`firls`/… route through transcendental + iterative numerics —
     elliptic integrals via AGM/Landen, the Remez exchange) gates on **SPEC-COMPLIANCE** (passband ripple ≤ Rp ·
     stopband attenuation ≥ Rs · equiripple alternation count · monotonicity) **+ coefficients-agree-to-N-digits**
     vs scipy/MATLAB. It does **NOT** bit-match: `std::sin`/`exp`/`sqrt` are not bit-identical across compilers/libm,
     those ULP differences compound through the iterations, so bit-match is physically unachievable in portable C++
     AND meaningless — spec-compliance is what MATLAB's own routines are validated against, and is the stronger gate
     (bit-identical-but-wrong is possible; spec-compliant-correct is the real goal).
   - **Filter APPLICATION** (`lfilter`/`sosfilt`/biquad/conv — pure multiply-add, no transcendentals) gates
     **BIT-EXACT vs scipy + the {1..16} determinism MOAT**. Same coefficients + same input + fixed per-element FP
     order (ADR-0078) ⇒ identical bits on every machine and every thread count.
   - **The determinism moat scopes to APPLICATION + STREAMING ONLY** (DAW replay, certifiable control —
     DO-178C/ISO 26262). Design is one-time setup; "deterministic `ellip()`" is not a differentiator.
   - **N-digit tolerance LOCKED: 10 significant digits** for the design coefficient cross-check (≈ double-precision
     agreement after the transcendental drift; tighter is fragile across libm, looser misses real bugs). Application
     bit-exactness is exact (0 ULP).

2. **The data-flow rule — design in zpk, convert zpk → sos DIRECTLY; tf is output-only (locked v11-a):**
   roots-of-tf (`tf_to_zpk`) is Wilkinson-ill-conditioned and tf coefficients lose precision badly above order ~8
   (proven by the order-12 conditioning gate). So design routines produce zpk and go zpk → sos directly;
   **transfer-function (b/a) is an interop/output format, never a design intermediate**. `zpk_freqz` evaluates the
   factored form (well-conditioned at any order); `zpk_to_sos` uses **nearest-to-unit-circle pole ordering + nearest-
   zero pairing** (the conditioning property that is the reason SOS exists). **SOS is the default for high-order IIR.**

3. **The two-layer (ADR-0078) — every filter ships BOTH:** typed whole-array **batch** API upper (`Frequency<T>` Hz +
   the v11-a `DecibelRatio`/`DecibelPower` + `NormalizedFrequency`), allocation-free **stateful streaming kernels**
   lower (Direct-II-transposed biquads, FIR delay-line, polyphase resamplers — the DAW/SDR real-time hot loop, fixed
   per-element FP order). dB stays a non-linear I/O lens (ADR-0078 §nonlinear), not a linear Quantity.

4. **Three isolated modules + a units addition** (module-isolation cornerstone): **`crd-hesap-dsp`** (core + adaptive)
   · **`crd-hesap-wavelet`** (→ fft) · **`crd-hesap-comms`** (→ dsp) · **`crd-units`** gains `DecibelRatio`/
   `DecibelPower`/`NormalizedFrequency`. A DAW build links `crd-hesap-dsp`, not comms.

5. **Module edges (acyclic):** `crd-hesap-dsp` → `crd-hesap-fft` (spectral / fast-conv / Hilbert / CZT) +
   `crd-hesap-eigen` (DPSS multitaper, MUSIC/ESPRIT, AR) + `crd-math` (simd) + `crd-units` + core/containers/memory.
   `crd-hesap-wavelet` → `crd-hesap-fft`. `crd-hesap-comms` → `crd-hesap-dsp`. **NO edge to `crd-hesap-opt`** (firls/
   remez use their own exchange/LS; revisit only if constrained-FIR wants the v7 QP). Lower-layer raw scalars +
   raw `Complex<T>` (ADR-0078 §5).

## Consequences

- v11-a ships the substrate: representations (tf/zpk/**sos**/state-space/lattice) + conversions + `freqz`/`sosfreqz`/
  `zpk_freqz`/`group_delay`, gated on analytic + cross-representation + the order-12 conditioning gate.
- The design/application split means the scoreboard reports TWO honest columns — design spec-compliance and
  application bit-exactness — never a single conflated "matches scipy" number.
- Spectral confidence statistics (χ²/F CIs for multitaper/AR) defer to v12 (Statistics); estimators ship in v11
  without parametric CIs; AIC/MDL order-selection ships in v11 (log-likelihood only).
- Extends ADR-0065; consumes ADR-0078 (two-layer + dB non-linear lens) and ADR-0092 (the FFT edge).
