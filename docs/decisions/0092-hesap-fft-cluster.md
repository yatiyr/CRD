# ADR-0092 — `crd-hesap-fft`: the FFT cluster — deterministic plan-from-factorization, portable-C++ MKL-adjacent, the full transform suite

- **Status:** Accepted (2026-06-20)
- **Phase:** 3.1.6 v10 (FFT cluster)
- **Tags:** `hesap` `fft` `determinism` `architecture` `module-edges` `substrate`
- **Plan:** `docs/phases/phase-3.1.6-hesap.md` (the v10 block); memory `project_v10_fft_plan`; system doc `docs/systems/hesap-fft.md`

## Context

v10 brings Fourier transforms to the hesap stack — the substrate for DSP (v11), convolution/correlation, image
and volume processing, spectroscopy and non-uniform sampling, compression/denoising, and sparse spectral
recovery. The consumers span every Cerid domain. Gold standards are FFTW3 + PocketFFT (numpy/scipy's backend) +
MKL for the dense transforms and FINUFFT for NUFFT. An FFT has no thread-dependent reduction, so it sits OFF the
usual determinism-moat axis — the differentiator has to come from elsewhere.

Three things had to be settled.

1. **Plan selection.** FFTW's MEASURE/PATIENT autotunes at runtime; its "wisdom" varies run-to-run and is not
   reproducible across builds. That is incompatible with a replay/certification engine.
2. **Asm vs portable C++.** MKL's edge is an assembly-integrated butterfly-twiddle kernel. Cerid has a standing
   no-asm decision (ADR-0082: portable C++ + the WASM-no-asm goal; hand-asm measured 0.98× for GEMM, reverted).
3. **Scope of "winning."** Chasing MKL clone-parity in portable C++ is a Spiral/genfft-class compiler project.

## Decision

**`crd-hesap-fft` is a deterministic, portable-C++, full-suite FFT module.**

1. **Deterministic plan-from-factorization.** The algorithm is chosen from the size factorization with NO runtime
   measurement; one precomputed twiddle table per plan, read-only after construction ⇒ a fixed per-element FP
   order ⇒ output **bit-identical across runs and threads** (each thread its own plan / the batched axis; per-plan
   execute scratch is not shared concurrently). Lead with this + zero planning overhead + typed zero-dependency
   integration — NOT a beat-MKL speed claim.

2. **The transform suite.** Complex FFT/IFFT (`FftPlan`, Stockham radix-2/4/8 + straight-line SIMD codelets N≤32 +
   Bailey four-step above 2¹⁹) · real FFT (`RealFftPlan`) · DCT-II/III + DST-II/III (`DctPlan`, Makhoul) · NUFFT
   type-1/2 (`NufftPlan`) · **Bluestein any-size** (`BluesteinPlan`, chirp-z over one pow-2 plan) · **N-D**
   (`NdFftPlan`, row-column; pow-2 axis → `FftPlan`, else `BluesteinPlan`) · **Sparse FFT** (`SparseFftPlan`,
   HIKP 2012, end-to-end sub-linear + noise-robust).

3. **Lower-layer raw `Complex<T>`.** An FFT is inherently complex and the kernels are SIMD; the data is raw
   `crd::hesap::Complex<T>`, never a typed `Quantity` (ADR-0078 §5 lower-layer). f32 + f64.

4. **Unnormalized both directions** (`ifft_normalized` applies 1/n). Bluestein/N-D consume this consistently via
   the forward-trick `IFFT = conj(FFT(conj·))/n`, so mixing `FftPlan` (unnormalized inverse) and `BluesteinPlan`
   (normalized inverse) on different N-D axes never clashes.

5. **The correctness oracle is the brute-force O(N²) DFT** for every transform — never the round-trip (which
   cancels a twiddle-sign/normalization error and passes while the forward transform is wrong).

6. **MKL global parity is DEFERRED — the goal is production-grade portable FFT, not an MKL clone.** On the
   AVX2-level dev box CRD is MKL-adjacent (canonical `run_split`: 256K fixed 0.25→~0.85× = 3.6×; 1M/2M ~0.77–0.83×;
   4M/8M ~0.92–0.95×, host-thermal-swing-caveated), beats PocketFFT everywhere, and real-FFT/DCT/NUFFT beat their
   peers outright. The residual ~15% at mid sizes
   is the four-step inter-stage twiddle — measured-exhausted (8 rejected attempts) as the gap MKL closes only via
   asm-integrated butterflies, **out of scope** per ADR-0082. A native small-FFT backend is an optional v11+ lane
   *if* the project ever admits asm. The generated-codelet substrate (`build/gen_subfft_m3.py` →
   `detail/hier_codelets.hpp`) carries the large-N wins.

7. **Module edges (acyclic):** `crd-core`/`crd-containers`/`crd-memory` (types, Array/Span, IAllocator) ·
   `crd-math` (`crd::math::simd`) · `crd-hesap` (`Complex<T>`, the CLI registry). No owning STL. CLI `hesap.fft.*`
   (forward + sparse) via the v7/v9 anchor pattern.

## Consequences

- v11 (DSP: FIR/IIR/resampling/spectral analysis) builds directly on this module.
- The sparse FFT is end-to-end sub-linear O(R·log n·(w+B log B)) and noise-robust (multi-scale binary location +
  voting + median) — but coefficient accuracy under noise is bounded by √(n/B)·σ/√R (information-theoretic).
- Determinism makes the FFT replay-safe and certification-friendly (DO-178C/ISO26262/FDA), unlike FFTW's tuned
  wisdom — even though {1..16} bit-identity is nearly free here.
- Extends ADR-0065; consumes ADR-0078 (two-layer, lower-layer raw) and ADR-0082 (portable-C++ microkernel, no asm).
