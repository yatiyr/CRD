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

- **2026-07-15-ckir-vs-handwritten-glsl.md** — the zero-IR-overhead head-to-head: CKIR-emitted GLSL vs hand-written GLSL, same algorithm, GPU-timed, output bit-matched. ReSTIR RIS reaches **PARITY** (0.318 vs 0.312 ms, 1.02×) after fixing a 1.56× loss — the statement-tier builders were UNROLLING the candidate loop (occupancy collapse); a tight runtime loop (`stmt_for_begin` + shared accumulators) closed it. Residual: transmittance 1.23× (compute-bound; register-carried loops are a scoped compute-emitter gap, negligible once/frame LUT). Rule: memory-bound big loops → runtime shared loop, not unroll.
- **2026-07-19-oit-tier-perf.md** — D-007 B17 OIT tier GPU PERF board (Vulkan, `last_gpu_ms` min-of-30, 1024² px × 4 layers = 4.2M fragments): the static store-based tiers are cheapest (A-buffer/MBOIT-4/MBOIT-6 all ~0.28 ms with the shared store) and **MBOIT's accuracy win is FREE** (memory-bound ⇒ the cubic reconstruction is hidden); the atomic linked-list is ~10× (3.0 ms — atomic contention, the price of unbounded depth); **stochastic is the cheapest unbounded tier per-frame** (~0.061 ms @ S=1 + TAA). Companion to the accuracy board below.
- **2026-07-19-oit-tier-scoreboard.md** — D-007 B17 OIT tier ACCURACY board (max per-channel RGBA8 error vs the exact `over`): A-buffer (static + atomic linked-list) = 0 (bit-exact both backends, atomic == static reference); MBOIT-6 = 1 LSB @ 3 layers vs WBOIT's 18 (**18× win**); MBOIT-4 = 0 @ 2 layers vs WBOIT's 30; stochastic = unbiased (converges to exact, bit-exact GPU==oracle==both backends via a deterministic hash); WBOIT the 14–30 LSB single-pass floor. No loss recorded.
- **2026-07-15-gi-atmosphere-vulkan.md** — D-007 B14 GI + B15 atmosphere GPU throughput on Vulkan (RTX 4070 Ti SUPER, `last_gpu_ms` min-of-30): DDGI probe-sample 0.171 ms/1080p (~65% peak BW), SVGF à-trous 0.236 ms, ReSTIR RIS 0.527 ms/262 k px, and the whole Hillaire sky (4 LUTs) ~0.12 ms/frame — the portable bit-exact IR runs in real time (zero portability tax). No direct vendor peer (RTXGI/NRD/RTXDI are proprietary); the hand-written-GLSL head-to-head is the next board.
- **2026-07-05-v14z-scoreboard.md** — the consolidated v14 ALL-PEERS scoreboard (a–m): 210 measured rows — 199 won / 7 tie-parity / 4 open (all four = the one named v0d GEMM-kernel gap) — across 19 peers, with the conformance audit mapping every moat claim to its ctest gate.
- **2026-07-05-v14z-tblis-xtensor.md** — the v14-z close rows: TBLIS measured at last (TTGT case won 1.14×; 3 pure-GEMM rows = the named v0d gap) + xtensor 3/3 wins.
- **2026-07-05-v14m-nn.md** — v14-m NN inference: f32 8/8 (torch 1.9–9.7×, ort 1.15–2.1×), q8 7/8 (torch-int8 6.6–10.2×, ggml-from-source 1.22–1.48×/layer); torch parity ≤6e-8; Q8_0 byte-exact; zero-alloc infer; the ort per-tensor cell CLOSED same evening (i8 tier 1.23× at better accuracy) — every cell on every v14-m board won.
- **2026-07-05-v14l-io.md** — v14-l I/O: 12/12 vs numpy/safetensors-py (writes 1.8–3.0×, st-read 11.15 GB/s); npy writer byte-identical to np.save 17/17; DLPack zero-copy; npz-DEFLATE write N/A-with-check.
- **2026-07-05-v14k-tt.md** — v14-k tensor trains: 8/8 vs tntorch (eval 13.1×, cross 12.6–15×); the 16⁶ LUT demo 1748× compression, 1.5× faster than materialized-table interp; honesty rows (raw gather; TT-rank wall).
- **2026-07-05-v14j-decomp.md** — v14-j CP/Tucker/randomized: 9/9 vs TensorLy at equal-or-better fit (CP 5.2–5.6×, HOOI 5.1–5.8×); MATLAB TTB 1.11–1.67× (tol=0 fixed budgets — its default early-stops, trap recorded); first-board full loss root-caused + flipped.
- **2026-07-05-v14i-sparse.md** — v14-i sparse: 13/13 incl. SPLATT-from-source 1.5× on MTTKRP, TACO 2.3×/2.8×, MATLAB TTB 29–39×; CSF≡COO bit-identity; three losing rows flipped (registers/staged-TTM/branchless merges).
- **2026-07-05-v14h-batched-la.md** — v14-h batched LA vs MKL/torch/MATLAB: GEMM 7 wins + 1 DRAM-wall tie, chol 2.5–8.5×, LU 1.8–3.8× (post-miscompile-fix re-measure), SVD 1.45–12×, pagemtimes 2.0–8.2×, pagemldivide 1.11–1.14×; the MSVC autovec wrong-code scar lives here.
- **2026-07-05-v14g-hyperopt-oracle.md** — v14-g contraction-path hyper-optimizer vs cotengra/cotengrust: better-or-equal quality on every corpus network, matched-protocol wall-clock 1.81–5.83×; deterministic-by-seed at any worker count.
- **2026-07-02-v14a-dtype-converts.md** — v14-a dtype/SR converts vs numpy/ml_dtypes/torch (see the float_convert-migration caveat noted in the v14-z scoreboard).

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
