# crd-hesap-comms — digital communications / SDR (Phase 3.1.6 v11c)

> One short overview per shipped module. Story + decisions live in the session log
> (`docs/sessions/2026-06-21-v11c-comms-start.md`); the decision is ADR-0093.

## Purpose

The comms / software-defined-radio sub-cluster of the v11 DSP subject, as its own module (module-isolation:
a DAW links `crd-hesap-dsp`, an SDR app links comms). Gold standard: **liquid-dsp 1.6.0** (the comms C peer)
+ **theoretical AWGN BER curves**. The honest gate (ADR-0093): a modem's correctness reference is **BER-vs-theory
+ the Nyquist property**, not bit-matching one library's arbitrary constellation rotation/Gray table — so liquid
is the **performance** peer (which Cerid crushes), correctness is gated against theory + self-contained round-trips.

## API surface (header → what it ships)

| Header | Contents |
|---|---|
| `modulation.hpp` | Gray-coded PSK / square-QAM / PAM (unit energy) + O(1) slicing demod + soft (max-log) LLR + bit packing + orthogonal M-FSK. |
| `pulse_shaping.hpp` | RRC/RC (via dsp) + Gaussian (GFSK) + `pulse_shape` + `matched_filter` + `eye_segments` + `peak_distortion`. |
| `loop.hpp` | `LoopFilter2` (2nd-order PI) + `Nco` + `cubic_interp` — the shared sync substrate. |
| `timing.hpp` | Gardner + Mueller-Müller TEDs + `SymbolSync` (interpolating PI loop). |
| `carrier.hpp` | `CostasLoop` (decision-directed) + `Pll` + M-th-power `estimate_cfo_mpsk` (AFC). |
| `equalizer.hpp` | `LmsEqualizer` (train + DD) + `CmaEqualizer` (blind) + `DfeEqualizer` + `mlse_viterbi`. |
| `channel.hpp` | AWGN / Rayleigh / Rician (deterministic Philox) + `Agc`. |
| `framing.hpp` | `find_preamble` (correlation sync) + Hamming(7,4) FEC. |
| `ofdm.hpp` | `OfdmModulator` (IFFT+CP / strip-CP+FFT on the v10 engine) + pilot channel estimation + ZF equalize. |

## Honest gate (ADR-0093)

modulation = Gray property + noise-free round-trip + unit energy + **BER vs the theoretical AWGN curve** (BPSK/QPSK
the exact `Q(√(2Eb/N0))`; QAM/PAM within 1.6× of the union-bound formula, deterministic Philox noise); pulse shaping
= **RRC-TX ⊛ RRC-RX Nyquist zero-ISI** (ISI → 0 as span grows); timing/carrier = the loop LOCKS (BER 0); equalizers
= open a multipath ISI channel (BER → 0), MLSE == the brute-force ML sequence; channels = measured SNR/fading stats;
OFDM = round-trip + multipath-through-CP recovery. Streaming kernels carry the run-twice determinism moat.

## Performance — Cerid (f64) CRUSHES liquid-dsp (f32), all five ops

modem QAM64 modulate **4.2×** · demodulate **3.6×** (O(1) per-axis slicing, gated == nearest-point) · eqlms 15-tap
**2.5×** · rrc interp ×4 **1.6×** · OFDM 1024 mod+demod **3.0×** (the v10 FFT engine). Benches:
`runtime/examples/bench_comms_vs_refs.cpp` + `bench_comms_liquid.c`, `scripts/run_bench_comms.sh`.

## Module edges (acyclic)

`crd-hesap-comms` → `crd-hesap-dsp` (RRC/FIR) · `crd-hesap-fft` (OFDM) · `crd-hesap-stats` (Philox + NormalSampler
= deterministic AWGN/fading) · `crd-hesap` (Complex<T>) · `crd-math` · core/containers/memory. Lower-layer raw
`Complex<T>` (ADR-0078 §5). CLI: `hesap.comms.qam.{modulate,demodulate}` (`cli_register_comms.cpp`).

## Tests

`tests/hesap-comms/` — 27 cases / ~39,663 assertions (linux-gcc + win-clang-cl + win-release). Self-contained +
theory-BER gates; the timing-loop sign was pinned by an inline diagnostic. MLSE cross-checked vs brute-force ML.
