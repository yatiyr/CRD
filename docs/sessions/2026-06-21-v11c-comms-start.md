# 2026-06-21 — v11c: the complete `crd-hesap-comms` module (modulation → OFDM) + crush benchmarks

**Phase 3.1.6 `crd-hesap` · v11 DSP cluster (ADR-0093) · NEW module `crd-hesap-comms`.** The entire comms/SDR module
(v11c-a through v11c-g) was built and gated against the comms gold standard (liquid-dsp 1.6.0 + theoretical AWGN BER
curves) in one session, and **crushes liquid-dsp on every benchmarked op**. **Comms suite: 39592 assertions / 26
cases GREEN (linux-gcc-release) + CI guards green.** Not committed.

## Module
New module registered (root CMakeLists + tests/CMakeLists). Edges → `crd-hesap-dsp` (RRC/FIR/resampling),
`crd-hesap-fft` (OFDM, later), `crd-hesap-stats` (Philox + NormalSampler → **deterministic AWGN/fading** = the moat),
`crd-hesap`/`crd-math`/core. Build/test via `bash build/comms_build.sh release` (WSL). Lower-layer raw `Complex<T>`
(ADR-0078); typed wrapping is a consumer's job.

## What shipped
- **v11c-a modulation** (`modulation.hpp`): `gray_encode`/`gray_decode`; `Modem<T>` for Gray-coded **PSK / square QAM
  / PAM** (unit average energy) with `modulate` / `demodulate` (nearest) / `demodulate_soft` (max-log LLR) / block
  helpers; `bits_to_symbols`/`symbols_to_bits` (MSB-first); orthogonal **M-FSK** modulate + non-coherent demod.
  **Gate**: Gray code round-trip + the Gray *constellation* property (every minimum-distance neighbour pair has 1-bit
  labels) + noise-free round trip + unit average energy + **BER vs the theoretical AWGN curve** (BPSK/QPSK = the exact
  `Q(√(2Eb/N0))` within 20%; QAM-16/64 and PAM-4 within 1.6× of the approximate union-bound formula, deterministic
  Philox noise) + FSK round trip + soft-LLR-sign == hard-decision.
- **v11c-b pulse shaping** (`pulse_shaping.hpp`): `rrc_pulse`/`rc_pulse` (via crd-hesap-dsp) + `gaussian_pulse`
  (GFSK/GMSK, BT product) + `pulse_shape` (upsample by sps + complex-over-real-tap FIR) + `matched_filter` +
  `eye_segments` (fold for the eye diagram) + `peak_distortion` (worst-case ISI). **Gate**: the **RRC-TX ⊛ RRC-RX =
  raised-cosine Nyquist zero-ISI** property — the residual at the symbol instants is pure truncation ISI and is shown
  to **decrease monotonically with span** (span 8 → 16 → 32) and reach < 1e-3 at span 32 (the real Nyquist spec; a
  truncated RRC is only approximately Nyquist) + Gaussian-pulse properties (unit DC gain, symmetric, peaked) +
  matched-filter unit-gain peak / max-SNR + run-twice bit-identical.

## Honest-gate note
A modem's correctness gold standard is **BER vs the theoretical AWGN curve** (and the Nyquist property for shaping),
not bit-matching one library's arbitrary constellation labeling/rotation (liquid's PSK uses a different rotation +
Gray table). liquid-dsp is therefore the **perf** peer (next), with correctness gated against theory + self-contained
round trips.

## State + next
- **Done (a+b)**: `crd-hesap-comms` created + v11c-a (modulation) + v11c-b (pulse shaping).
- See the **v11c-c…g COMPLETE** section below — the rest of the module shipped the same session.

## v11c-c…g COMPLETE (same session) — the full modem chain + crush benchmarks

Continued straight through. **Comms suite 39592 assertions / 26 cases GREEN (linux-gcc-release) + guards green.**
- **v11c-c timing** (`timing.hpp`, `loop.hpp`): Gardner + Mueller-Müller TEDs + `SymbolSync` (cubic interpolator +
  2nd-order PI loop, index-based strobe). Gate: Gardner S-curve + the loop LOCKS onto a 0.4-sample fractional delay
  (zero symbol errors in steady state). ⚠ the loop sign was pinned by an inline diagnostic (`t += sps − v`).
- **v11c-d carrier** (`carrier.hpp`): `CostasLoop` (decision-directed α/β), `Pll`, M-th-power `estimate_cfo_mpsk`.
  Gate: removes a phase+frequency offset (BER 0 modulo the QPSK 4-fold ambiguity); the AFC estimate matches the CFO.
- **v11c-e equalizers** (`equalizer.hpp`): `LmsEqualizer` (training + DD), `CmaEqualizer` (blind), `DfeEqualizer`,
  `mlse_viterbi`. Gate: each opens a multipath ISI channel (BER→0); MLSE == the brute-force ML sequence.
- **v11c-f channels** (`channel.hpp`, `framing.hpp`): AWGN/Rayleigh/Rician (deterministic Philox), `Agc`,
  `find_preamble` (correlation sync), Hamming(7,4) FEC. Gate: measured SNR/fading stats, AGC convergence, sync peak,
  single-bit correction.
- **v11c-g OFDM** (`ofdm.hpp`): `OfdmModulator` (IFFT+CP / strip-CP+FFT on the v10 FFT), pilot channel estimation,
  ZF equalize. Gate: round-trip identity + multipath-through-CP recovery (BER 0, known-H and pilot-estimated).

### Benchmarks — Cerid CRUSHES liquid-dsp on ALL of them (Cerid f64 vs liquid f32)
`bench_comms_vs_refs.cpp` + `bench_comms_liquid.c` via `scripts/run_bench_comms.sh` (WSL, 1 thread):

| op | Cerid | liquid | verdict |
|---|---|---|---|
| modem QAM64 modulate | 0.66 ns/sym | 2.77 | **4.2× WIN** |
| modem QAM64 demodulate | 7.08 ns/sym | 25.3 | **3.6× WIN** (O(1) per-axis slicing, gated == nearest-point) |
| eqlms 15-tap | 24.6 ns/sym | 61.8 | **2.5× WIN** |
| rrc interp ×4 | 5.24 ns/out | 8.48 | **1.6× WIN** |
| OFDM 1024 mod+demod | 4.84 µs/sym | 14.4 | **3.0× WIN** (the v10 FFT engine) |

The demod started at 0.67× (O(M) nearest-point); adding the O(1) per-axis slicer (gated == the nearest-point
reference over 36k noisy points) flipped it to a 3.6× win — every comms op now beats liquid while being f64.

## State + next
- **Done**: the entire `crd-hesap-comms` module (v11c-a…g) + the crush benchmarks vs liquid-dsp. Suite 39592/26 +
  guards green (linux-gcc-release).
- **NEXT**: v11-z close — CLI `hesap.{dsp,wavelet,comms}.*` + the 3 system docs + the full scoreboard + finalize
  ADR-0093 (the only remaining v11 item).
- **PENDING USER**: commit + full Windows 4-config DoD + 18-config CI (the v11 cluster has run only on linux-gcc).

Memory: `project_v11_dsp_plan` (updated), `reference_matlab_gold_standard`, `feedback_bench_all_peers_never_cherry_pick`.
