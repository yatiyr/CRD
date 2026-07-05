# docs/bench — captured benchmark results (append-only)

Every measured benchmark board lands here as its own file, **at the time it is measured** — never quoted
from memory (SANITY #2/#5). This is the single home for numbers; session logs and system docs LINK here
instead of restating tables.

**Naming:** `YYYY-MM-DD-<slice-or-module>-<subject>.md` (e.g. `2026-07-02-v14a-dtype-converts.md`).

**Each file carries:**
1. The exact machine/config (CPU, pinning, thread count, compiler + flags, peer versions).
2. The harness used (script path — tracked under `scripts/`), so any board is re-runnable.
3. The full peer board — every peer measured, losses and ties included (SANITY #6/#9: a loss is an open
   bug, not a disclosure; N/A stated *with the check*).
4. The verdict line quoted by session logs / phase tables.

Historical results (v5–v13 + the transcendental cluster) were retro-ported 2026-07-02 from the session
logs / phase tables (recorded numbers, not re-measured — each file carries the note). From v14 on, every
board is written here AT MEASUREMENT TIME — this is part of the per-slice Definition of Done for any
perf/crush claim (see AGENTS.md § Numerical/perf work).

## Index

- **2026-07-03-v10-fft-remeasure-and-midband.md** — the FFT crush campaign (7 sessions over 2 days): generator REBUILT+tracked; standalone-hier 2-pass + deep-split A·B·C (now 8K–512K); 2026-07-04 research-led crush (VectorFFT existence proof): FMA emission + AoSoA block layout + factored twiddle + hier FMA rewrite + ds cascade ⇒ f64 mid-band 0.47→0.60–0.80× MKL, f32 0.17–0.55→0.54–0.93×, f64 4M 0.98× + BEATS FFTW ≥512K; SoA-vs-AoSoA stream-count law + the /Od dual-body scar recorded; remaining gaps mechanism-pinned.

- **2026-07-03-v0d-gemm-zeroinit-pass.md** — v0d GEMM order-preserving crush: +8-12% (65-69→70-77 GF/s, gap vs OpenBLAS halved) via ZeroInit kernels + alpha==1 merge; E1/E2/Mc-sweep refuted+recorded; the bit-locked remainder → proposed ADR-0100 opt-in fast-order tier.

- **2026-07-03-v14f-einsum-exec-vs-numpy-torch.md** — v14-f einsum execution: plan-reuse 3.5×/3.2× crush; TTGT copy-avoidance banked; 2 OPEN rows named (v0d f64 GEMM gap, thin-K direct kernels).
- **2026-07-03-v14e-einsum-path-vs-opteinsum.md** — v14-e path optimizer vs opt_einsum: paths parity-or-BETTER on all 33 oracle cases (their optimal search is internally inconsistent — we minimize their reported metric); planning 9×/41× faster.

- **2026-07-02-v14c-reduce-vs-reproblas.md** — v14-c reductions: Tier-R reproducible sum CRUSHES ReproBLAS 1.60×@1M / 1.01–1.23×@16M (12-accumulator SIMD + speculative single-pass); Tier-D fixed-tree 3× naive; {1..16} moat + repartition gates.
- **2026-07-02-v14d-permute-vs-hptt.md** — v14-d permute vs HPTT 1T: FULL CRUSH 1.11×/1.16×/1.31× (NT-store premise refuted; src-locality odometer + stride-aware tiles were the levers).

- **2026-07-02-v14b-elementwise-broadcast.md** — v14-b elementwise/broadcast engine vs numpy/torch; full-board win (broadcast 1.50×/1.68×, strided 1.57×, contiguous at the DRAM ceiling); NumPy bit-exact corpus.

- **2026-06-11-v5-sparse-direct-cholmod-crush.md** — v5 sparse direct (multifrontal LU/Cholesky/LDLᵀ) vs CHOLMOD/UMFPACK/MUMPS; factor kernel 49–53 GF/s BEATS MUMPS; small-RHS solve WIN up to 3.82×; moat proven {1,2,4,8}.
- **2026-06-04-v5e-hss-strumpack-crush.md** — v5e HSS/ULV compress/factor/solve vs STRUMPACK; compress 3.41–1.39× (global-sample QR-then-SVD), factor 2.7–4.1×, solve parity/WIN; moat proven {1,2,4,8}.
- **2026-06-05-v5f-mixed-precision-ir.md** — v5f mixed-precision (f32-factor + f64-IR) vs smumps+IR; af23560 1.14× WIN, wang3/ns3Da parity; saddle-point indefinite fixed (GMRES-IR); moat proven.
- **2026-06-07-v6-sparse-eigensolvers-matrix-free.md** — v6 sparse eigenvalue (Lanczos/thick-restart) vs ARPACK/PRIMME; parity (matvec-count matched); algorithmic crush = preconditioned methods (shift-invert + v5 factors); moat proven {1,2,4,8}.
- **2026-06-11-v7-optimization-full-crush.md** — v7 optimization (OSQP/CMA-ES/Powell/Adam) vs scipy/pycma/torch; Powell beats scipy, CMA-ES ros5 beats pycma, Adam 12-digit exact; all gates met.
- **2026-07-02-v13-numerics-motion.md** — v13 numerical computing + motion (interpolation · quadrature · differentiation · trajectory generation · Ruckig OTG) vs scipy/MATLAB/Boost/GSL/libruckig; 3-pillar moat (determinism · allocation-free · error-estimate).
- **2026-06-27-v12-statistics-special.md** — v12 statistics + special functions (samplers · distributions · log-density gradients · descriptive) vs scipy.stats/NumPy/MATLAB; counter-RNG determinism moat; betainc crush (4 CDF losses → wins).
- **2026-06-26-crd-math-deterministic-transcendental.md** — crd-math deterministic transcendental cluster (exp/log/trig/hyperbolic/power/complex) vs libm; 100% engine re-route; bit-identical gcc↔MSVC moat; sin 2.5× slower (determinism cost).
- **2026-06-13-v9-ode-dae.md** — v9 ODE/DAE cluster (ERK · BDF · Radau · IMEX · Krylov · sensitivities) vs SUNDIALS/scipy/Boost; determinism moat.
- **2026-06-20-v10-fft-transforms.md** — v10 FFT cluster (1D FFT · NUFFT · DCT/DST · sparse FFT) vs MKL/FFTW/FINUFFT/PocketFFT; codelet crush vs orchestration gap.
- **2026-06-22-v11-dsp-wavelet-comms.md** — v11 DSP/wavelet/comms (filters · spectral · wavelets · SDR) vs scipy/MATLAB/liquid/PyWavelets; multi-threaded determinism moat.
