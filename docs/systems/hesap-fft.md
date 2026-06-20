# crd-hesap-fft — FFT cluster

> Phase 3.1.6 **v10**. ADR-0092. Plan: `docs/phases/phase-3.1.6-hesap.md` (the v10 block).
> Status: **CLUSTER COMPLETE (a–z)**. Complex Stockham + scheduled codelets + Bailey four-step (`fft.hpp`) ·
> real FFT (`real_fft.hpp`) · DCT-II/III + DST-II/III (`dct.hpp`) · NUFFT type-1/2 (`nufft.hpp`) ·
> **Bluestein any-size** (`bluestein.hpp`, v10-c) · **N-D 2D/3D/N-D** (`nd_fft.hpp`, v10-e) ·
> **Sparse FFT, HIKP, end-to-end sub-linear + noise-robust** (`sparse_fft.hpp`, v10-h) · CLI `hesap.fft.*`
> (`cli_register_fft.cpp`, v10-z). Sessions: `docs/sessions/2026-06-1*-fft-*.md`.

## What it is

Fourier transforms for every Cerid domain: DSP and spectral analysis (v11 builds on it), convolution and
correlation, image/volume processing (N-D), spectroscopy and non-uniform sampling (NUFFT), compression and
denoising (DCT), and sparse spectral recovery. f32 + f64, complex-inherent (the lower layer carries raw
`crd::hesap::Complex<T>`, not a typed `Quantity` — ADR-0078 §5).

Gold standards: **FFTW3** + **PocketFFT** (numpy/scipy's backend) + **MKL** (proprietary) for the dense
transforms; **FINUFFT** for NUFFT; the **brute-force O(N²) DFT** is THE correctness oracle for every transform
(never the round-trip — a round-trip cancels a twiddle-sign/normalization error and passes while the forward
transform is wrong; the FFT edition of the odeint-d4 trap).

## The transform suite

| Transform | Header / type | Sizes | Notes |
|---|---|---|---|
| Complex FFT/IFFT | `fft.hpp` `FftPlan<T>` | power of two | Stockham autosort radix-2/4/8 + straight-line SIMD codelets (N≤32) + Bailey four-step cache-blocking above 2¹⁹ |
| Real FFT | `real_fft.hpp` `RealFftPlan<T>` | pow-2 ≥ 4 | packs n reals as n/2 complex + Hermitian recombine (~½ the work) — crushes numpy/scipy/PocketFFT rfft |
| DCT/DST | `dct.hpp` `DctPlan<T>` | pow-2 | DCT-II/III + DST-II/III (Makhoul over the real FFT) — **beats scipy/PocketFFT at every size** |
| NUFFT | `nufft.hpp` `NufftPlan<T>` | — | type-1/2 (Greengard-Lee, ES kernel) — **WINS FINUFFT small/mid at 3× the accuracy** |
| **Bluestein** | `bluestein.hpp` `BluesteinPlan<T>` | **ANY n** | chirp-z over one pow-2 engine plan (M=2^⌈log2(2n−1)⌉); prime/composite/non-pow-2 in O(n log n) |
| **N-D FFT** | `nd_fft.hpp` `NdFftPlan<T>` | any shape | row-column over the 1D engine (pow-2 axis → `FftPlan`, else `BluesteinPlan`) |
| **Sparse FFT** | `sparse_fft.hpp` `SparseFftPlan<T>` | pow-2 | HIKP 2012, **end-to-end sub-linear + noise-robust** (see below) |

## The determinism contract (the headline that is NOT speed)

The plan is chosen **deterministically from the size factorization** — no runtime measurement (unlike FFTW
MEASURE/PATIENT, whose wisdom varies run-to-run and is not reproducible across builds). One precomputed twiddle
table per plan, all read-only after construction, so the algorithm has a **fixed per-element FP order** ⇒ the
output is **bit-identical across runs and across threads** (each thread uses its own plan or the batched axis;
the per-plan execute scratch is not shared concurrently). Verified by the `run-twice bit identity` gate. This
is *nearly free* for an FFT (no thread-dependent reduction) — so determinism + zero planning overhead + typed
zero-dependency integration are the lead, not a beat-MKL speed claim.

## Bluestein (v10-c), N-D (v10-e) — the design that makes mixing engines sound

Both reduce to the pow-2 `FftPlan`. The engine FFT is **unnormalized both directions** (`ifft_normalized`
applies 1/n), so Bluestein folds an explicit 1/M into its convolution, and uses the forward-trick
`IFFT(x)=conj(FFT(conj x))/n` so one (forward) chirp serves both directions. N-D applies the **forward DFT
only** per axis (both engines agree on the unnormalized forward) and does the inverse with the N-D forward-trick
`conj(FFT(conj·))/∏dᵢ` — so the `FftPlan`-unnormalized vs `BluesteinPlan`-normalized inverse conventions never
clash on a mixed-axis transform, and the single 1/∏dᵢ lives at the N-D level.

## Sparse FFT (v10-h) — end-to-end sub-linear + noise-robust

For a spectrum with k≪n significant frequencies, recover them in **O(R·log n·(w + B log B))** — no O(n) step:

1. **Hash** — a random odd permutation σ spreads the frequencies; a Gaussian-windowed-sinc **flat filter** + fold
   to B≈8k bins gives each frequency a clean home bin (the filter touches w = 1.6–50% of n samples, never all n).
2. **Multi-scale binary location** — bucketize at log₂(n)+1 dyadic spatial offsets β_j = 2^{m−j−1}; the phase at
   β_j is π·f/2^j, whose **sign** (after correcting for the already-found low bits) IS bit j of f. Each bit is
   a coarse, **±π/2-noise-robust** decision — replacing the noise-fragile single-offset ratio.
3. **Voting + median** — a bin's tone is voted across rounds (collisions/noise vote inconsistently and lose), and
   the coefficient is the component-wise **median** of the per-round bucket estimates n·ẑ₀/Ĝ(σf−t·n/B).

Gates: **exact k-sparse** (frequencies exact, coeffs ~1e-7, n≤65536 k≤10) AND **noisy k-sparse** (k dominant tones
+ complex Gaussian noise σ≤0.008/bin → all frequencies recovered, coeffs to the √(n/B)·σ/√R floor). The sub-linear
regime is robust where bucket-SNR = |c|/(√(n/B)·σ) ≳ 10 — the standard sparse-FFT trade (lower SNR ⇒ B grows toward
dense; machine-eps coeffs under noise need ~n samples, information-theoretic, not a bug).

## Performance — portable-C++, MKL-adjacent (the banked verdict)

The MKL-parity grind is **banked + closed** (ADR-0092, decision 2026-06-20): the goal is *production-grade portable
FFT*, not an MKL clone. On the AVX2-level i9-14900K (Raptor Lake has no AVX-512 ⇒ MKL runs AVX2 here — a level
fight; AVX-512 server/CI is the honest caveat):

| size | ×MKL (f32) | note |
|---|---|---|
| 256K | **~0.85×** (default) | the Stockham trough (was 0.25×) fixed via the four-step 1024×256 + 16×16 hier P2 + gather/scatter fusion — **3.6× CRD**, default-on |
| 512K | ~0.66× | |
| 1M / 2M | ~0.77–0.83× | (canonical `run_split` 2026-06-20: 0.83 / 0.77) |
| 4M / 8M | ~0.92–0.95× | parity border (the 14900K's ±18% thermal swing dominates run-to-run) |

(Canonical `bench_fft_vs_refs`/`run_split` against MKL-on-AVX2; the `m3_full` harness flatters ~20% — not used.)

PocketFFT is beaten everywhere; real FFT / DCT / NUFFT beat their peers outright. The residual ~15% to MKL at
mid sizes is the four-step inter-stage twiddle — measured-exhausted (8 rejected attempts) as the gap MKL closes
only via asm-integrated butterflies, **out of scope** per ADR-0082 (portable-C++ + WASM-no-asm). A native
small-FFT backend is an optional v11+ lane *if* the project ever admits asm. The generated-codelet substrate
(`build/gen_subfft_m3.py` → `detail/hier_codelets.hpp`) is what carries the large-N wins.

## CLI (`hesap.fft.*`)

Signals are DATA (interleaved re/im f64 vectors), so an agent reaches the transforms directly:

| command | params | output blob |
|---|---|---|
| `hesap.fft.forward.f64` | `data` (interleaved re/im, len 2n; ANY n) · `inverse` (0/1) | the transformed signal, interleaved re/im |
| `hesap.fft.sparse.f64` | `data` (len 2n, n pow-2) · `k` | `[count, (freq, re, im) × count]` |

Anchor `register_fft_cli_anchor()` (pull it from a TU to survive the static-lib link). `test_cli.cpp` drives both
commands through the registry directly. The `data` param is declared `ParamKind::VectorId` (the closest kind —
there is no inline-array `ParamKind`) and carries the f64 samples inline via `set_f64_array` / `get_f64_array`;
the commands are **registered + direct-invoke-tested** (the v9-z gate). Wiring the MCP front-end's vector-store
resolution for a `VectorId` data param is a front-end integration follow-on (this is the first inline-vector-data
CLI — no precedent existed).

## Module edges (acyclic)

`crd-core` / `crd-containers` / `crd-memory` (types, Array/Span, IAllocator) · `crd-math` (`crd::math::simd`) ·
`crd-hesap` (`Complex<T>`, the CLI registry). No owning STL; SIMD via the engine's portable `simd` wrappers.

## Tests

`tests/hesap-fft/` — `test_fft.cpp` (complex + four-step oracle + Bluestein + N-D + Sparse), `test_dct.cpp`,
`test_nufft.cpp`, `test_cli.cpp`. Suite **GREEN — 259 assertions / 28 cases** (linux-gcc-release). Every transform
is gated against the brute-force DFT (or a direct N-D DFT / planted sparse tones), plus round-trip, Parseval,
and run-twice bit-identity.
