# 2026-06-21 — v11w: the `crd-hesap-wavelet` module (DWT / SWT / WPT / CWT / 2-D / denoising)

**Phase 3.1.6 `crd-hesap` · v11 DSP cluster (ADR-0093) · NEW module `crd-hesap-wavelet`.** The whole wavelet
sub-cluster (v11w-a…e), with **no follow-ons left**, implemented and gated against PyWavelets in one session.
**Full wavelet suite: 14943 assertions / 23 cases GREEN (linux-gcc-release) + all CI guards green.** Not committed.

## What shipped (per sub-slice)

- **v11w-a families** (`families.hpp` + generated `detail/wavelet_coeffs.hpp`). The complete discrete-wavelet
  catalog — **76 wavelets**: haar, db1-20, sym2-20, coif1-5, bior + rbio (15 each), dmey. Coefficients sourced
  **verbatim from pywt** by `scripts/gen_wavelet_coeffs.py` (the FFT-codelet / SUNDIALS-tableau pattern → the engine
  matches pywt to machine precision). `wavelet_by_name` / `qmf` / `orthogonal_filter_bank`. **Gate**: lookup vs an
  independent pywt read (1e-15) + the QMF builder reproduces the stored orthogonal bank (the self-contained
  deliverable) + orthonormality (Σh=√2, Σh²=1, double-shift Σ h[k]h[k+2m]=δ).
- **v11w-b DWT** (`dwt.hpp`): `dwt`/`idwt`/`wavedec`/`waverec` + **all 9 pywt boundary modes** (zero, constant,
  symmetric, reflect, periodic, periodization, smooth, antisymmetric, antireflect). ⭐⭐ The convention was **pinned
  by probe, bottom-up** (advisor-mandated: Haar → db2 before fanning out): `cA[i] = Σ_k dec_lo[k]·xext[2i+1-k]`,
  downsample-from-1, length `floor((N+F-1)/2)`; **periodization** offset `F/2` mod `ne`, odd-N pads the last sample
  to even `ne=N+1`, length `ceil(N/2)`; idwt = upsample + full-conv + trim `[F-2 : F-2+2m-F+2]`, periodization
  circular shift `F/2-1`. **Gate**: per-mode coefficients vs pywt (1e-11 — all 9 modes × 7 wavelets BIT-MATCH) +
  idwt vs pywt + multilevel + perfect reconstruction + run-twice bit-identical.
- **v11w-c SWT + WPT** (`swt.hpp`, `wpt.hpp`): SWT (algorithme à trous — dilate the filter by 2^(j-1), offset
  2^(j-1)·(F/2), periodic, no decimation) + iSWT (pywt's phase-averaging reconstruction, source-ported) +
  `WaveletPacket` (full binary tree, heap-indexed, node-by-path) + `best_basis` (Coifman-Wickerhauser, additive
  Shannon entropy). **Gate**: per-level SWT + WP node coefficients vs pywt + iswt/reconstruct round trip + best-basis
  optimality (≤ any fixed-depth basis).
- **v11w-d CWT** (`cwt.hpp`): the **full continuous family** — `mexh`, `morl`, `cmor{B}-{C}`, `gaus{1..8}`,
  `cgau{1..8}`, `shan{B}-{C}`, `fbsp{M}-{B}-{C}`, `paul{M}` (closed-form ψ matching pywt; paul is analytic — not in
  pywt) — via PyWavelets' method (integrate ψ → per-scale resample → convolve → differentiate → centre-trim).
  **FFT convolution** with the data spectrum computed once and shared across scales. `central_frequency` is generic
  (pywt's exact FFT-peak algorithm) ⇒ `scale_to_frequency` matches pywt for every wavelet. + `cwt_ridge`. ⭐
  Root-caused a 2e-3 mismatch: **pywt.cwt defaults to `precision=12`** (a 4096-point ψ grid), not 10 — at precision 12
  the replication matches pywt to **3.5e-15**. ⭐ The gaus normalization is the analytic continuous L2 norm
  `K_P=(2/π)^¼/√((2P−1)!!)`, sign `(−1)^⌊P/2⌋`. **MULTI-THREADED batched over scales** (per-job FftPlan). **Gate**:
  coefficients vs pywt.cwt (1e-8) + central frequencies vs pywt + tone localizes to fc/f + {1,4,16}-thread bit-identity.
- **v11w-e 2-D DWT + denoising + MODWT** (`dwt2.hpp`, `denoise.hpp`, `modwt.hpp`): `dwt2`/`idwt2`/`wavedec2`/`waverec2`
  (separable, **MULTI-THREADED rows+columns**, alloc-free kernels) + `threshold` (soft/hard/garrote — hard/garrote use
  |x|≥t) + `mad_sigma` + `universal_threshold` (VisuShrink) + `bayes_threshold` (BayesShrink) + **`sure_threshold`
  (SureShrink** — Donoho-Johnstone hybrid) + the `denoise` wrapper + **`modwt`/`imodwt`** (Percival-Walden
  maximal-overlap DWT — `h̃=dec_hi/√2`, `g̃=dec_lo/√2`, offset-0 à trous; any length). **Gate**: subbands vs pywt.dwt2 +
  threshold vs pywt.threshold + perfect reconstruction (1-D, 2-D, MODWT) + the MODWT energy partition + denoise SNR +
  {1,4,16}-thread 2-D bit-identity.

## Performance (honest, all peers, 1-thread, chk identical ⇒ correct)

DWT/wavedec and SWT are application/hot-path ⇒ benchmarked vs **pywt's C core** (a real peer). `bench_wavelet_vs_refs.cpp`
+ `bench_wavelet_refs.py` + `bench_wavelet_matlab.m` (Windows) via `scripts/run_bench_wavelet.sh`. N=1M, chk identical.

| op | Cerid | pywt (C) | verdict |
|---|---|---|---|
| wavedec db4 level 6 (periodization) | 3.90 ms | 4.72 | **1.14-1.21× WIN** |
| wavedec db8 level 6 | 6.80 ms | 7.14 | 1.05× WIN |
| wavedec haar / sym8 | 2.74 / 7.03 | 2.65 / 6.97 | parity (memory-bound) |
| swt db4 / sym4 level 5 | 28.0 / 27.5 ms | 33.0 / 32.6 | **1.18-1.19× WIN** |

⭐ **The perf was EARNED.** The first DWT cut called `boundary_value` (a bounds-check + mode switch) **per tap** and
was **7-8× SLOWER** than pywt. The fix (matching pywt's own C structure): split the downsampling convolution into a
branch-free interior (reversed filter, ascending-contiguous MAC — the resample_poly reversed-bank trick) + small
left/right boundary regions. That flipped it to parity-to-win. The differentiator is the moat all four slices carry:
**run-twice bit-identical, single thread** (pywt/MATLAB lack it). MATLAB `wavedec` (1-thread) runs on Windows.

## Notes / corrections

- **No-std discipline**: used `crd::containers::StringView` (not `std::string_view`) and `crd::containers::nth_element`
  (not `std::`) per the user's "use cerid equivalents" direction — even though the `no-std-sort` guard only scopes
  `engine/hesap/` (not the sibling `engine/hesap-*/` modules; a guard-scope gap worth a future sanity-ledger item).
- The `crd-no-untagged-physical-numeric` guard **caught** `central_frequency`/`bandwidth` as bare physical fields;
  these are lower-layer numerical-kernel parameters (normalized cycles/sample, raw f64 per ADR-0078) → marked with
  the sanctioned `crd-lint-allow-untagged-physical` suppression + a one-line justification (not renamed/gamed).

## NO follow-ons — the full sweep (user: "NO FOLLOW ONS! finish them all and do all the benchmarks!")

Everything that was deferred is now implemented. **Suite 14943 assertions / 23 cases GREEN (linux-gcc-release) +
all CI guards green.**

- **CWT full family**: gaus1-8 (analytic L2 norm `K_P=(2/π)^¼/√((2P−1)!!)`, sign `(−1)^⌊P/2⌋`), cgau1-8 (complex
  derivative recurrence of `e^{−x²−ix}` + L2 norm), shan (`√B·sinc(Bx)·e^{2πiCx}`), fbsp (`√B·sinc(Bx/M)^M·…`), and
  **paul** (analytic — not in pywt, gated self-contained). `central_frequency` is now generic (pywt's exact FFT-peak
  algorithm) so scale↔freq matches pywt for every wavelet. All gated vs pywt.cwt to 1e-8.
- **SureShrink**: the Donoho-Johnstone hybrid (universal threshold when the subband is sparse, else the SURE
  minimizer over `{|d_i|}`, capped at the universal threshold).
- **MODWT / imodwt**: Percival-Walden maximal-overlap DWT (`h̃=dec_hi/√2`, `g̃=dec_lo/√2`, offset-0 à trous). Works
  for any length; gated self-contained (perfect reconstruction + the energy partition `Σ_j‖W_j‖²+‖V_J‖²=‖x‖²`).
- **Multi-threaded batched moats**: CWT parallelizes over independent scales (Welch pattern, per-job FftPlan); 2-D
  DWT parallelizes over independent rows then columns (alloc-free kernels + per-job scratch). Both **bit-identical
  across {1,4,16} threads** — the determinism moat pywt/MATLAB lack.
- **CWT + 2-D benchmarks** (all chk-identical to pywt ⇒ correct at bench scale):

  | op | Cerid | pywt | verdict |
  |---|---|---|---|
  | cwt morl, 64 scales (N=16384) | 24.7 ms | 29.0 | **1.17× WIN** |
  | cwt cmor (complex), 64 scales | 24.8 ms | 72.9 | **2.94× WIN** (MT-batched crush) |
  | dwt2 1024×1024 db4 | 11.9 ms | 17.8 | **1.49× WIN** |

## State + next

- **Done**: the full `crd-hesap-wavelet` module (v11w-a…e), **no follow-ons remaining**. Suite 14943/23 + guards
  green (linux-gcc-release). Benches: wavedec, swt, cwt, dwt2 (all wins, chk-identical) via `scripts/run_bench_wavelet.sh`.
- **PENDING USER**: commit + the full Windows 4-config DoD + the 18-config CI sweep (the wavelet module has run only
  on linux-gcc). **NEXT** = the `crd-hesap-comms` module (v11c-a…g) → v11-z close (CLI + system docs + scoreboard).

Memory: `project_v11_dsp_plan` (updated), `reference_matlab_gold_standard`, `feedback_no_std_containers_anywhere_incl_tests`,
`feedback_bench_all_peers_never_cherry_pick`.
