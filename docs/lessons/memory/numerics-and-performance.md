# Memory reference: numerics and performance

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../../ROADMAP.md); current rules: [AGENTS](../../../AGENTS.md).

> Reference corpus, consolidated 2026-09-12; not a live tracker. Read [AGENTS](../../../AGENTS.md),
> [MEMORY](../../../MEMORY.md) and [ROADMAP](../../ROADMAP.md) for current rules/status.
> Dated state, loop grants, tool paths and schedules below are historical. Reusable engineering lessons remain
> applicable unless superseded by current instructions. Retrieve one named record; do not load this whole file on entry.

<a id="memory-feedback_always_bench_both_eigen_and_lapack"></a>
## feedback_always_bench_both_eigen_and_lapack

---
name: feedback_always_bench_both_eigen_and_lapack
description: "EVERY performance kernel must be benchmarked against BOTH Eigen AND LAPACK — never omit either, no excuses"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 55f742ff-2462-46c9-9771-0e5dd735901a
---

Strong user directive (2026-05-23, repeated): every performance-critical kernel Cerid
ships must be benchmarked head-to-head against **BOTH Eigen AND LAPACK** — and we must
PROVE we beat both. Omitting Eigen (or LAPACK) from any comparison is not acceptable.

**Why:** the user's mandate is "perform better than Eigen and lapack and I mean it, we
need proof." A benchmark missing a reference is not proof. Caught 2026-05-23: the MRRR
tridiagonal vector bench compared only vs LAPACK `dstedc`/`dstemr`, omitting Eigen — even
though Eigen exposes `SelfAdjointEigenSolver::computeFromTridiagonal(diag, subdiag,
options)` which takes `(d,e)` directly. No methodological excuse; just add it.

**How to apply:**
- Eigen dense eig: `SelfAdjointEigenSolver<MatrixXd>(A[, EigenvaluesOnly])`.
- Eigen TRIDIAGONAL eig (values and/or vectors): `es.computeFromTridiagonal(diag,
  subdiag, ComputeEigenvectors | EigenvaluesOnly)` — works on `(d,e)` directly, so it IS
  comparable to LAPACK `dstedc`/`dstemr`/`dsterf` and to our tridiagonal kernels.
- Every `bench_hesap_*_vs_reference` section gets an Eigen column AND a LAPACK column.
  Extends [[PRINCIPLES_reference_class_benchmarking]] — both references, always, with the
  ratio printed so the win (or loss) is falsifiable.


<!-- end-memory:feedback_always_bench_both_eigen_and_lapack -->

<a id="memory-feedback_bench_against_the_correct_peer"></a>
## feedback_bench_against_the_correct_peer

---
name: feedback_bench_against_the_correct_peer
description: "Benchmark a preconditioner against the SAME algorithm class, not whatever the reference happens to ship"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 50f493b0-787d-41e4-9bb7-95fea7d05959
---

When a Cerid preconditioner has no same-tool peer in the reference (Eigen), do NOT benchmark it
against a different algorithm class and report the gap as a head-to-head. **Eigen ships only
Diagonal / IncompleteCholesky / IncompleteLUT — NO Schwarz / domain decomposition, NO SPAI, NO
polynomial/Chebyshev, NO AMG.** Benchmarking Cerid's Schwarz (a domain-decomposition method)
against Eigen IncompleteCholesky (a factorization preconditioner) produced a meaningless "25×
slower" number — an apples-to-oranges category error. The user caught it ("are we comparing
completely different stuff? does Eigen even have Schwarz?").

**Why:** the honest-crush discipline ([feedback_iterative_crush_claim_same_algorithm](numerics-and-performance.md#memory-feedback_iterative_crush_claim_same_algorithm),
[feedback_iterative_bench_matched_true_residual](numerics-and-performance.md#memory-feedback_iterative_bench_matched_true_residual)) extends to peer SELECTION, not just stopping
criteria. A number is only a head-to-head if the two things do the same job.

**How to apply:** bench against the CORRECT peer. Schwarz's peer is **block-Jacobi** (Schwarz =
block-Jacobi + overlap + exact local solves → the overlap iteration-reduction is the honest
value-add; RAS-Schwarz ov=1 beat block-Jacobi 1.11× on cd2d-200). When the reference truly lacks
the tool, frame it as **breadth** + a right-peer comparison + correctness/determinism, never as a
fabricated cross-class loss OR win. FSPAI-vs-IncompleteCholesky is defensible only because both
fill the same SPD-preconditioner ROLE; Schwarz-vs-IChol is not. Case: v4i-3, 2026-05-26.


<!-- end-memory:feedback_bench_against_the_correct_peer -->

<a id="memory-feedback_bench_all_peers_never_cherry_pick"></a>
## feedback_bench_all_peers_never_cherry_pick

---
name: feedback_bench_all_peers_never_cherry_pick
description: "Benchmark against ALL available gold standards, never just the one you beat — report losses honestly"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: deb11ae2-46e2-4805-9918-89f237d06d9b
---

**When benchmarking, run EVERY available peer and report ALL results — never cherry-pick the gold standard you
beat.** The user caught me (2026-06-21) benchmarking v11-j `fftconvolve` against ONLY scipy (Cerid 67.5ms vs scipy
77.9ms = "1.15× crush") while skipping MATLAB — and MATLAB's MKL-backed `fft` does it in ~40ms single-thread /
30.7ms multi-thread, so **Cerid actually LOSES to MKL 0.6× single-thread.** Reporting only the peer you beat is the
dishonest-scoreboard trap ([reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine) honest-scoreboards rule, [feedback_full_scoreboard_no_partial_victory](workflow-and-correctness.md#memory-feedback_full_scoreboard_no_partial_victory)).

**Why:** "we crush in the perfs" is only credible if it survives the STRONGEST peer. scipy.signal is single-threaded
PocketFFT/C (a soft peer on FFT-bound ops); MATLAB R2026a is MKL+TBB-backed (the real ceiling); liquid-dsp is
SIMD-C. Beating the weakest and hiding the loss to the strongest is worse than an honest loss.

**How to apply:**
- For EVERY perf claim, run scipy AND MATLAB AND liquid-dsp (whichever has a comparable op) — and Intel IPP when
  installed. If a peer lacks the operation, SAY SO explicitly (e.g. "liquid has no full-FFT-convolution").
- Match threading: MATLAB `fft` is multi-threaded by default — compare single-thread (`maxNumCompThreads(1)` /
  `-singleCompThread`) for apples-to-apples vs Cerid's single-threaded kernels, AND report the multi-thread number.
- ⚠⚠ CORRECTION (2026-06-21, same day): my first read — "fftconvolve loses to MKL = a fundamental v10 FFT-vs-MKL
  gap" — was WRONG (the user insisted it wasn't MKL; they were right). MEASUREMENT found the real cause: Cerid's
  `fftconvolve` REBUILT the FFT plan (28ms twiddle precompute at 2^21) EVERY call; scipy/MATLAB amortize plans via
  caches. Cerid's RAW FFT is FAST (12.7ms complex 2^21, 1-thread). FIX = `FftConvolver` (plan built once) ⇒ 67.5→
  41.7ms ⇒ **beats scipy 2.1×** (87.6ms); vs MATLAB = same ballpark (MATLAB 30-84ms host-throttle-noise; Cerid
  1-thread vs MATLAB multi-core). ⇒ LESSON: before blaming a "backend/kernel gap", PROFILE — a 5×-too-slow op vs a
  fast raw kernel screams overhead (alloc/plan-rebuild), not kernel. [reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine) root-cause rule.
- THE HONEST PATTERN: Cerid CRUSHES owned kernels (windows/FIR/filtering); FFT-heavy ops are competitive once plans
  are cached; the definitive MULTI-CORE crush vs multi-threaded MATLAB needs a multi-threaded FFT (independent
  sub-FFTs — lands cleanly for the MANY-independent-FFT cases: Welch/STFT/spectrogram/N-D/batched, v11-m/n).


<!-- end-memory:feedback_bench_all_peers_never_cherry_pick -->

<a id="memory-feedback_bench_arms_must_match_wait_and_bracket_structure"></a>
## feedback_bench_arms_must_match_wait_and_bracket_structure

---
name: feedback_bench_arms_must_match_wait_and_bracket_structure
description: "A/B GPU benches whose two arms have different submit/wait or event-bracket structure measure the sync structure, not the work — match one submit + one wait + one event bracket per arm, or the numbers are an artifact."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-05T19:03:43.472Z
---

CEIR-29z-1 (the CUDA-Graphs launch vs N-dispatch fallback bench): the first draft's graph arm ran the two-class pipeline as
`run_range(prefix)`[submit+wait] + `launch(graph)`[submit+wait] + `run_range(suffix)`[submit+wait] = THREE CPU→GPU→CPU
round-trips, while the fallback ran the whole plan in ONE `submit_and_wait`. That timed wait-latency ×3 vs ×1 and faked a
0.49× "loss" (worse on WSL, whose passthrough driver has higher per-wait latency — the tell). The GPU column had the same
defect: it SUMMED three event brackets for the graph arm vs one for the fallback, so the graph read ~4× higher — the fixed
event-record+sync cost of the extra brackets, not `cuGraphLaunch` cost.

**Why:** a submit+wait is a full CPU→GPU→CPU round-trip; an event bracket has a fixed record+sync floor. Two arms with
DIFFERENT counts of either are measuring the sync structure, not the work. The CUDA-Graphs win is specifically the CPU
submit-path (one `cuGraphLaunch` vs N `cuLaunchKernel`), which only shows when both arms use the SAME single submit + single
wait + single bracket. After a `CudaComputeContext::enqueue` split (a non-waiting `cuGraphLaunch` on the recording stream, so
prefix + graph + suffix are ONE `begin()`/`submit_and_wait()`), the sandwich FLIPPED 0.49× → 1.57–2.24× — the "loss" was the
artifact. (Same family as [feedback_a_perf_flag_that_can_measure_an_empty_frame_must_exit](rendering.md#memory-feedback_a_perf_flag_that_can_measure_an_empty_frame_must_exit): the harness structure, not the
subject, was measured.)

**How to apply:** every arm of an A/B GPU bench = ONE `begin()` → all its work (eager dispatches AND any graph launch, via a
non-waiting enqueue) → ONE `submit_and_wait()`; read `last_gpu_ms()` once per arm (one bracket). Never sum brackets for one
arm and single-bracket the other. State the bracket structure in every column header. Separately: a repeated-execute bench
needs a CACHING resolver — a per-call `resolve_stage` that creates a new pipeline each call and caps at N (16 here) exhausts on
the ~hundreds of executes a median-of-9 × K-loop bench does (it died at the 4th call); cache the ResolvedStage by op identity
(one op → one stage under fuse=false; re-key for fused GemmRelu). And a GPU-time ratio at toy dims (kernel time << the launch
fixed cost) is undiscriminated (`cuGraphLaunch` overhead vs ev0→first-node latency) — report raw µs with the floor stated, not
a ratio, until a per-bracket-floor control runs.

**The correctness GATE that guards a bench must loop every compared arm the SAME N** (CEIR-30z-1, advisor-caught): a
transfer-cost bench compared arm A (all-Host) and arm B (sharded) to an oracle after a back-to-back replay loop meant to catch
aliased-storage clobber ([feedback_aliased_storage_graph_replay_must_reestablish_inputs](workflow-and-correctness.md#memory-feedback_aliased_storage_graph_replay_must_reestablish_inputs)) — but the first draft ran arm B ×10
and arm A ×1 before the compare. The board CLAIMED "asserted after 10 back-to-back executes" for both; it was true for one. An
asymmetric gate leaves the lightly-looped arm's replay-clobber path untested while the prose says otherwise. **Rule:** loop
EVERY arm the compared claim names the same number of times before the assert (three characters: wrap arm A in the same `for`),
and make the board sentence match what the code actually does. Cheaper than the caveat that explains the asymmetry.


<!-- end-memory:feedback_bench_arms_must_match_wait_and_bracket_structure -->

<a id="memory-feedback_bench_gate_all_peers_agree"></a>
## feedback_bench_gate_all_peers_agree

---
name: feedback_bench_gate_all_peers_agree
description: "Every peer benchmark must ABORT unless all peers compute the identical result; don't verify fairness by luck"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d095b72d-cb68-40b1-aaa2-1147516ea4fd
---

⭐ **A peer benchmark must have a self-verifying fairness GATE: abort the run unless EVERY peer computes the same
quantity to tolerance.** A speedup vs a peer that is (mis-)computing a *different* number is meaningless — and it's
easy to mis-drive a peer's API. In v15-c the autodiff high-level helper `derivative(g, wrt(t,t), at(t))` mis-seeded a
directional 2nd derivative (gave 2.37 vs the true −1.64); it was caught ONLY because Cerid happened to match the
independent FD oracle and I printed all three. Lucky, not gold-standard.

**How to apply (every bench, every remaining v15 slice):**
- Compute each peer's result into arrays; before timing, assert `max_rel_diff(cerid, peer) < tol` for EVERY peer and
  `std::abort()` (print which peer, the error) on disagreement. Tight tol vs another exact method (~1e-9; only
  crd::math-vs-libm ulps differ), loose vs FD (~1e-4). Print the agreement in the result line, not just the speedup.
- Drive each peer via its LEANEST correct path (raw seed, not the overhead-heavy high-level API) so the peer gets its
  BEST case and the crush is conservative/fair. For nested-dual 2nd order, seed directly `{val.val,val.grad,grad.val,
  grad.grad} = {x, v, v, 0}` (the hyper-dual seed) and read `.grad.grad`.
- Keep an INDEPENDENT oracle (FD / FD-of-FD) in the gate so a shared-bug between two AD peers can't pass.

Complements [feedback_bench_all_peers_never_cherry_pick](numerics-and-performance.md#memory-feedback_bench_all_peers_never_cherry_pick) and [feedback_full_honest_evaluations_crush_every_metric](numerics-and-performance.md#memory-feedback_full_honest_evaluations_crush_every_metric).


<!-- end-memory:feedback_bench_gate_all_peers_agree -->

<a id="memory-feedback_benchmarks_mandatory_at_slice_close"></a>
## feedback_benchmarks_mandatory_at_slice_close

---
name: benchmarks-mandatory-at-slice-close
description: A slice is NOT closed without running its benchmark(s) vs the frontier reference when applicable — unit tests passing is not enough
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a9e2b042-249d-4290-abf9-3c707236fa70
---

A slice is **not closed** until its benchmark(s) have been run, whenever a
benchmark is applicable. Compiling clean + unit tests passing is necessary but
NOT sufficient — the close gate includes the performance/quality measurement
against the frontier reference. Two coupled gates:
1. **Crush / push to the limits** — the result must beat (or match the true
   hardware floor of) the frontier peer; "passes + is slower" is not done.
2. **NEVER REGRESS EXISTING PERFORMANCE** (strong user directive 2026-05-28) —
   a change must NOT make any previously-measured benchmark slower or worse
   (higher fill, more iterations, lower throughput). Compare against the PRIOR
   baseline, not only the reference. A change that regresses a previously-
   winning benchmark **does not ship — revert or fix it.** Case study (v5a-0):
   supervariable ND compression passed all unit tests but regressed ND fill on
   bcsstk13/24/25 (0.984/1.001/1.157 → 1.058/1.121/1.187); reverted in full.
   The corollary: **benchmark the PREMISE before building an optimization** —
   if the existing path already wins (our AMD already beat Eigen-AMD on those
   matrices), there may be nothing to fix; a quick bench of the current state
   beats writing speculative machinery.

**Why:** Cerid's hesap mandate is to **beat / crush frontier libraries**
(Eigen / LAPACK / ILUPACK / SuiteSparse). A slice that compiles and unit-tests
green but is never benchmarked has not *proven* the crush — and "crush all
frontier libraries" is the whole point. Strong user directive 2026-05-28:
"we do not close slices without BENCHMARKS! At the end of each slice, we do
benchmarks if applicable." Reinforces [reference_implementations_are_the_floor](workflow-and-correctness.md#memory-feedback_reference_implementations_are_the_floor)
and the matched-true-residual / correct-peer discipline
([iterative_bench_matched_true_residual](numerics-and-performance.md#memory-feedback_iterative_bench_matched_true_residual), [bench_against_the_correct_peer](numerics-and-performance.md#memory-feedback_bench_against_the_correct_peer)).

**How to apply:** At each slice close, if there is (or should be) an applicable
`bench_hesap_*_vs_reference` (gated by `CRD_BUILD_HESAP_VS_REFERENCE` /
`CRD_BUILD_HESAP_VS_SUITESPARSE` / `CRD_BUILD_HESAP_VS_ILUPACK`), BUILD it with
the flag on and RUN it; report the comparison numbers (and reset the flag OFF
after, per [gated_vs_reference_benches_need_flag_to_validate](numerics-and-performance.md#memory-feedback_gated_vs_reference_benches_need_flag_to_validate)). Examples:
v5a-0 weighted-ND → fill vs Eigen-AMD on bcsstk13/24/25 (the debt's named gate);
v5a supernodal Cholesky → factor+solve time vs Eigen SimplicialLLT + CHOLMOD.
If no bench exists for an op that has a frontier peer, writing one IS part of
the slice. The unit-test gate stays; the bench gate is *additional*, not a
substitute.


<!-- end-memory:feedback_benchmarks_mandatory_at_slice_close -->

<a id="memory-feedback_bit_exact_fft_crushes_only_when_dram_bound"></a>
## feedback_bit_exact_fft_crushes_only_when_dram_bound

---
name: feedback_bit_exact_fft_crushes_only_when_dram_bound
description: A bit-exact portable FFT beats a vendor FFT ONLY in the DRAM-bound regime; L2/compute-bound the no-FMA handicap loses
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 1487a581-3392-44fb-bc9e-ebeaffd19da5
---

**A bit-exact portable GPU kernel (CKIR, NoContraction/no-FMA for cross-backend bit-exactness) can only CRUSH a vendor
kernel (cuFFT) in the DRAM-BOUND regime. In the L2-resident / compute-bound regime it structurally LOSES**, because
bit-exactness FORBIDS FMA fusion (SPIR-V NoContraction / HLSL precise / CUDA --fmad=false) — a ~2× arithmetic handicap the
vendor doesn't pay. So do not chase a head-on crush where the working set fits L2; find the regime where arithmetic is hidden
behind memory traffic and OUR advantage (fewer global round-trips via fusion) decides it.

**Measured, D-007 GPU FFT campaign (2026-07-13, RTX 4070 Ti SUPER, 672 GB/s, L2 ~32–48 MB):**
- **1D FFT-conv:** DRAM-bound at any batch → **1.99× crush** (one global read+write vs the vendor's ~3 passes).
- **2D FFT-conv, SINGLE image 1024²:** the 8 MB image is L2-RESIDENT → cuFFT's FMA + L2 efficiency win; our best (tiled
  transpose-on-write, coalesced, +1 bank pad) = **0.082 ms = 0.58×**. Even PERFECT transpose elimination only reaches ~0.77×.
  This is the CEILING for a bit-exact FFT here — not a bug, physics.
- **2D FFT-conv, BATCHED (B images, ONE shared PSF):** once B·8 MB spills L2 (B≥8), cuFFT's per-image time TRIPLES
  (0.037 ms/img B=4 → 0.114 B=8); ours barely moves (0.088 → 0.098, already near-DRAM-bound) ⇒ **B=8/16 = 1.16–1.20× CRUSH**,
  bit-exact, at 84% of peak DRAM bandwidth. The crossover B is literally the L2 capacity — measured on BOTH sides.
- **2D REAL FFT-conv (R2C/C2R, bloom is real-valued):** the Hermitian half-spectrum HALVES our absolute per-image time
  (0.099 → 0.049 ms/img, 2.0×) AND still beats cuFFT's OWN R2C by 1.13–1.14× (B≥16 DRAM-bound: ours 0.049 flat vs cuFFT 0.056).
  The half-width column conv reuses the batched tiled conv verbatim (col_stride = Wp = pad(cols/2+1, tile_c)). Verified bit-exact
  on oracle + Vulkan + DX12. Two wins: real-FFT doubles absolute throughput while the fewer-round-trips crush margin holds.

**How to apply:** before declaring "we can't crush the vendor," identify whether the benchmark point is L2-resident or
DRAM-bound. If L2-resident, the honest crush target is a DIFFERENT regime (larger working set OR batched), which is usually
the more important real workload anyway (ML feature-map conv, multi-target/multi-channel). Batching is CKIR-pure: it's just
`image = WorkgroupIndex / tiles_per_image` index arithmetic (the shared filter is indexed WITHOUT the image offset). This is
the concrete meaning of the campaign's thesis "raw/L2-bound = parity ceiling; the crush is FUSION in the DRAM-bound regime."
See [project_gpu_fft_crush_campaign](project-history.md#memory-project_gpu_fft_crush_campaign), [feedback_bit_exact_blind_to_symmetric_bugs_energy_comp](workflow-and-correctness.md#memory-feedback_bit_exact_blind_to_symmetric_bugs_energy_comp) (the other bit-exact
gotcha), [feedback_memory_wall_diagnosis_two_signals](workflow-and-correctness.md#memory-feedback_memory_wall_diagnosis_two_signals) (DRAM-bound diagnosis). Board:
`docs/bench/2026-07-13-gpu-fft-cufft-gold.md`.


<!-- end-memory:feedback_bit_exact_fft_crushes_only_when_dram_bound -->

<a id="memory-feedback_bit_exact_scan_cannot_crush_decoupled_lookback"></a>
## feedback_bit_exact_scan_cannot_crush_decoupled_lookback

---
name: feedback_bit_exact_scan_cannot_crush_decoupled_lookback
description: A bit-exact portable scan CANNOT crush CUB — decoupled look-back (the only fast single-pass) is non-bit-exact
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 1487a581-3392-44fb-bc9e-ebeaffd19da5
---

**A BIT-EXACT portable prefix-sum (scan) CANNOT beat CUB `DeviceScan` — investigated exhaustively (B-cmp, 2026-07-13).** CUB's
speed is its **single-pass decoupled look-back** (2N traffic: read once, write once). But decoupled look-back computes each
block's exclusive prefix by summing *whatever predecessor states are ready* (some blocks have published only an AGGREGATE,
others a full PREFIX) — a **TIMING-DEPENDENT summation order** ⇒ non-deterministic f32 rounding ⇒ **NOT bit-exact, not even
reproducible run-to-run.** That violates the ⭐⭐ all-backends-bit-exact mission.

**The two BIT-EXACT single-pass forms both lose** (both built + measured on Vulkan, RTX 4070 Ti SUPER):
- **CHAINED** (block bid waits for predecessor bid-1's full PREFIX): serializes the prefix propagation across all blocks →
  0.03–0.08× CUB. Correct + bit-exact, just serial.
- **ALL-AGGREGATE** (sum ALL predecessor aggregates in a FIXED ascending order — deterministic, matches the 3-pass): O(nblocks²)
  work + wave-serialized on the aggregate publish → even slower (0.015–0.019×).

**So scan is the ONE compute primitive where bit-exactness + portability STRUCTURALLY forbid a crush** — it forbids the whole
fast-algorithm class (not just an arithmetic mode, unlike FFT's no-FMA handicap where DRAM-bound we still won). Our per-kernel
bandwidth is fine (94% peak); the loss is pure traffic/serialization inherent to a deterministic multi-pass.

**How to apply:** the portable bit-exact scan is the 3-pass (`build_scan`, 0.53× DRAM-bound) or a 2-pass reduce-then-scan
(~0.67×, 3N traffic — the best portable, the next lever). Do NOT chase a single-pass scan crush — it requires non-bit-exact
decoupled look-back. The atomics substrate built for the attempt (`buffer_decl_coherent` = `coherent volatile`/`globallycoherent`
+ `KStmtKind::SpinUntilNonzero`, all 5 emitters + oracle; forward-progress + coherence PROVEN on Vulkan) IS kept and reusable for
SORT / histogram / compaction, where the atomic scatter need not be bit-exact. See
[feedback_bit_exact_fft_crushes_only_when_dram_bound](numerics-and-performance.md#memory-feedback_bit_exact_fft_crushes_only_when_dram_bound) (FFT's milder handicap), [feedback_dispatch_1wg_missing_upload_barrier_race](workflow-and-correctness.md#memory-feedback_dispatch_1wg_missing_upload_barrier_race).


<!-- end-memory:feedback_bit_exact_scan_cannot_crush_decoupled_lookback -->

<a id="memory-feedback_complex_split_simd_must_be_wide_unrolled"></a>
## feedback_complex_split_simd_must_be_wide_unrolled

---
name: feedback_complex_split_simd_must_be_wide_unrolled
description: "Replacing separate-pass SIMD with a fused-pass kernel must match/exceed the pass-version's unroll (8+ FMA accumulators) or it loses to FMA-port latency — the wall is ILP, not memory passes (applies to any fused-vs-separate refactor, not just complex)"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 14cd3cee-9b6f-4f3b-b641-1da39ae5b096
---

**General rule (any fused-vs-separate SIMD refactor):** when you fuse N separate
single-product SIMD passes into one multi-product pass to cut memory traffic, the
fused kernel MUST keep at least the unroll factor (independent accumulators) of
the passes it replaces. AVX2 FMA is ~4–5 cyc latency / 2 per cyc throughput →
needs ~8 accumulators in flight; a fused-but-narrow kernel goes latency-bound and
REGRESSES despite fewer memory passes. Will apply to the `zlahqr` inner kernels
in v3d-2c-2 (different shape, same trap).

When porting a complex (two-real-array `ar`/`ai`) reduction to fused SIMD, the
instinct "4 separate `sdot`/`saxpy` passes read each row twice → fuse to one pass
to halve memory traffic" is a TRAP if the fused kernel is narrowly unrolled. The
real bottleneck on AVX2 is **FMA-port latency** (~4–5 cyc, 2/cyc throughput →
needs ~8 independent accumulators in flight), not memory bandwidth, for these
cache-resident panel rows.

Case study (v3d-2c-1 complex Hessenberg `zgehd2`, 2026-05-24):
- Separate `sdot`/`saxpy` (each 8-wide / 2 accumulators): **0.74×/0.82× Eigen**.
- First fused cut, **4-wide** (1 Vec4d per product): **REGRESSED to 0.56×/0.54×**
  — fewer memory passes but half the unroll → latency-bound.
- Fused **8-wide** (2× Vec4d, 8 independent FMA accumulators): **flipped to a WIN
  1.05×/1.21× Eigen**. Bonus: the 8-wide form is bit-identical to the
  `sdot`-combo (same 2-accumulator/8-wide reduction order).

**Why:** related to [feedback_register_tiling_needs_packing](workflow-and-correctness.md#memory-feedback_register_tiling_needs_packing) and
[feedback_memory_wall_diagnosis_two_signals](workflow-and-correctness.md#memory-feedback_memory_wall_diagnosis_two_signals) — diagnose the wall before
optimizing. A swap that "should" help by the memory argument can regress if it
costs ILP.

**How to apply:** write fused complex SIMD kernels (`simd_cdot_nc`,
`simd_caxpy`, `simd_caxpy_conjx` in `detail/dot_simd_complex.hpp`) **8-wide for
f64 (2× Vec4d) / 16-wide for f32 (2× Vec8f)** from the start — match the unroll
of the scalar-real `sdot`/`saxpy` they replace. If a fused kernel loses to the
separate-pass baseline, suspect unroll/ILP before memory. Keep the 4-accumulator
products summed-then-combined so the result stays bit-identical to
`sdot(ar,vr)−sdot(ai,vi)`. Measure (bench), don't reason from the code shape.


<!-- end-memory:feedback_complex_split_simd_must_be_wide_unrolled -->

<a id="memory-feedback_crush_mandate_bounded_by_importance"></a>
## feedback_crush_mandate_bounded_by_importance

---
name: feedback-crush-mandate-bounded-by-importance
description: "The 'crush Eigen + LAPACK everywhere' mandate is bounded by does-this-actually-matter. For a narrow micro-regime with NO consumer + a layout-fit wall (not a kernel defect) where default/other paths already win, the user wants an honest 'is this important?' judgment and is fine NOT pursuing it + NOT filing debt — after seeing the data."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 3f36099d-ab5d-457b-ab8e-a9c6b5a704a4
---

**Rule:** "Beat/crush Eigen + LAPACK" is about the SUBSTRATE being elite where it
counts — not about forcing a win in every micro-regime regardless of value. When
a measured loss is **all** of: (a) a narrow micro-regime (specific shape × size ×
opt-in method), (b) pulled by NO consumer, (c) a layout-fit / architectural wall
(e.g. ADR-0083 row-major vs a column-major-native competitor) rather than a
kernel-quality defect, and (d) the default path + other methods + the
parallel/large-N regime already win — then the elite move is to **surface it
honestly with data, ask the user "is this actually important / does it make hesap
weaker?", and (with agreement) characterize-and-move-on with NO debt.**

**Why:** 2026-05-23, v3c-1c QR-tall. lstsq `method=QR` lost to Eigen at m≈2n,
n=128–512 (0.76–0.93×). I'd planned a fix; measurement showed it's the ADR-0083
single-core column-major-fit wall (Eigen's HouseholderQR is column-major-native;
m=2n is only 2× tall so no parallel headroom; a cheap serial-W tuning lever
regressed large-n and was reverted). The unblocked fast-path banked the n=64 case
(loss→1.19× win) but mid-range stayed lost. The user asked, verbatim: **"Is this
really important? Do we really need to crush here? ... If not, we don't even need
to make a debt for it"** and then **"I approve let's close this I think this does
not make hesap weaker."** QR is opt-in (default `Auto=COD` ties Eigen; SVD crushes;
large-N/many-RHS crush via parallel gemm), the loss is 7–24% (a layout artifact,
same class v0e accepted [project_cholesky_smalln_rowmajor_limit](project-history.md#memory-project_cholesky_smalln_rowmajor_limit)), no consumer.

**How to apply:**
- This is NOT a license to defer real work — it does NOT weaken
  [feedback_never_defer_solve](workflow-and-correctness.md#memory-feedback_never_defer_solve) or [feedback_reference_implementations_are_the_floor](workflow-and-correctness.md#memory-feedback_reference_implementations_are_the_floor).
  Those still bind for: the default path, any consumer-pulled path, correctness,
  and any regime where a true crush is physically available (parallelism pays,
  large N, the actual workload). A loss you can fix and that matters = SOLVE it.
- The gate to invoke this is strict: do the measurement first, prove the wall is
  layout/architectural (try at least one real lever and show it fails), confirm
  no consumer, confirm default/other paths win — THEN ask the user with the data.
  Never assume "unimportant" to dodge effort; the user decides, informed.
- "No debt" here means a **resolved decision note** in the phase doc ("not
  pursued, here's why"), NOT a TODO/"later" entry. Distinguish: debt = "we will do
  this"; resolved-not-pursued = "we decided not to, with reasons + revisit
  trigger." The latter is honest documentation, not deferral.
- Banked partial wins (e.g. the n=64 unblocked QR win) still ship + get the full
  DoD — closing a micro-regime chase doesn't mean dropping the real improvement.


<!-- end-memory:feedback_crush_mandate_bounded_by_importance -->

<a id="memory-feedback_crush_persist_research_dont_retreat"></a>
## feedback_crush_persist_research_dont_retreat

---
name: feedback_crush_persist_research_dont_retreat
description: "On a crush goal, NEVER retreat/recommend-banking-as-defeat; step back, deep-research papers + reference code, then attack — no cycles"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: de24be60-d214-48f5-94bf-4e8ca5e4673f
---

When the user sets a CRUSH goal (beat the gold-standard peers — Eigen, UMFPACK, SuperLU, CHOLMOD,
LAPACK, …), it IS doable: working examples exist (the peer itself proves the performance is reachable
on the same hardware/regime). So:

- **NEVER say "I pushed hard and it didn't work" / never recommend banking as a defeat or a "this is
  multi-session, stop here" retreat.** That framing is banned. The peer's existence is the existence
  proof; the gap is a known technique I haven't applied yet, not a wall.
- **When stuck, do NOT cycle** — do not re-try the same approach or re-present the same "bank vs push"
  question over and over. That wastes the user's plan budget and is unprofessional.
- **Instead: take a step back → do EXTENSIVE deep research** (papers, books, AND the reference
  libraries' actual source code — read Eigen/SuperLU/UMFPACK/KLU/CHOLMOD internals) → understand the
  cutting-edge algorithm the peer uses → THEN attack again with that understanding. Repeat
  research→attack until crushed.
- Banking a clean checkpoint is fine as a *durability* step, but never frame it as "the crush isn't
  achievable this session." Keep going.

**Why:** the user is building an elite engine; the peers are existence proofs that the perf is real.
The failure mode is retreating + cycling instead of learning the missing algorithm from the references.
**How to apply:** measure to locate the gap, then go READ the reference implementation + the founding
papers for THAT phase, extract the specific technique (e.g. SuperLU panel/2D-blocking, Eisenstat-Liu
pruning, supernode-panel bmod), implement it, re-measure. Persist professionally until the crush lands.
Related: [feedback_reference_implementations_are_the_floor](workflow-and-correctness.md#memory-feedback_reference_implementations_are_the_floor), [feedback_full_honest_evaluations_crush_every_metric](numerics-and-performance.md#memory-feedback_full_honest_evaluations_crush_every_metric),
[feedback_never_defer_solve](workflow-and-correctness.md#memory-feedback_never_defer_solve), [project_eylem_crush_physx_jolt_with_determinism](project-history.md#memory-project_eylem_crush_physx_jolt_with_determinism).


<!-- end-memory:feedback_crush_persist_research_dont_retreat -->

<a id="memory-feedback_differential_oracle_must_run_the_full_legacy_pipeline_not_one_phase"></a>
## feedback_differential_oracle_must_run_the_full_legacy_pipeline_not_one_phase

---
name: feedback_differential_oracle_must_run_the_full_legacy_pipeline_not_one_phase
description: "When a NEW representation's verifier is checked against a LEGACY multi-phase validator as a differential oracle, the oracle must run ALL phases (parse THEN validate), because the legacy checks are split across phases — testing against one phase silently misses the checks the other owns"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-11T07:33:00.123Z
---

Building `validate_ceir_frame` (CEIR-15c-1c-1) — the CEIR-native graph-semantic verifier — I locked it against the
legacy `FrameGraphDesc` validator with a differential oracle: for a malformed frame, `validate_ceir_frame(m).kind == X`
must AGREE with the desc-side verdict. The trap: the legacy checks are DISTRIBUTED ACROSS PHASES —

- `DuplicateName` is caught at **parse** time (`parse_frame_toml`, per-entry, per-category), NOT in `validate_frame_graph`.
- `NoOutputPass` is caught at **validate** time (`validate_frame_graph`), after parse succeeds.

So a naive oracle `CHECK(validate_frame_graph(from_ceir_frame(m)) == X)` would **silently miss** `DuplicateName`
(validate never checks it → returns Ok → the oracle falsely disagrees, or worse, a bug slips because the oracle proved
nothing). The fix is an oracle that runs the WHOLE desc-side cook pipeline:

```cpp
FrameCookError cook_verdict(const FrameGraphDesc& d, IAllocator* a) {
    const String toml = emit_frame_toml(d, a);
    FrameGraphDesc d2(a);
    const FrameCookError pe = parse_frame_toml(sv(toml), d2);  // parse-time: DuplicateName, closed vocabs, ...
    if (pe != FrameCookError::Ok) { return pe; }
    return validate_frame_graph(d2);                            // semantic: NoOutputPass, cycles, ...
}
```

**Why it generalizes:** any migration that validates a NEW representation against a LEGACY validator split into
phases (lex/parse/semantic/link) must run every phase to be a faithful oracle — a check you forgot the legacy owns in
an earlier phase is exactly the check your new verifier is most likely to get wrong. Order also matters: the phase
order fixes the priority when two violations coexist (parse's DuplicateName beats validate's NoOutputPass), so the
new verifier must return the SAME first-violation, and each oracle fixture must exercise ONE violation (tune it until
both sides agree — disagreement means the fixture trips an earlier check, per [feedback_run_only_the_tests_you_added](build-and-verification.md#memory-feedback_run_only_the_tests_you_added)).

**Placement corollary (same slice, advisor discriminator):** a layered validator splits checks by what each layer can
DECIDE — the CORE structural verifier (`find_frame_misuse`, crd-ceir) owns only checks decidable from attrs it can
already interpret (a `lifetime` string, geometric ints, closed vocabs) with NO foreign enum and NO gpu constant;
everything needing a downstream enum (`FrameResourceKind`), a cross-op sweep, an endpoint convention (`@output`), or a
gpu constant (`kFgMaxImageLayers`) belongs to the frame-cook layer (`validate_ceir_frame`). Core reading a foreign
enum's magic int is the desync class [feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero](execution-ir.md#memory-feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero)'s sibling.


<!-- end-memory:feedback_differential_oracle_must_run_the_full_legacy_pipeline_not_one_phase -->

<a id="memory-feedback_fast_tier_must_enable_fma_bitexact_flags_cripple_gemm"></a>
## feedback_fast_tier_must_enable_fma_bitexact_flags_cripple_gemm

---
name: feedback_fast_tier_must_enable_fma_bitexact_flags_cripple_gemm
description: "The CUDA backend's bit-exact NVRTC flags (--fmad=false) HALVE GEMM throughput by disabling FMA fusion — the FAST/ULP tier (T1) must compile with --fmad=true; only the bit-exact tier (T3) needs --fmad=false"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Benchmarking the AS-4 CKIR GEMM vs cuBLAS Sgemm (ADR-0098 §4), CKIR first measured **0.28–0.49× cuBLAS** — a 2–3× loss that
looked like a kernel-quality gap but WASN'T. Root cause: `compile_cubin` in `engine/kir-cuda/src/backend_cuda.cpp` compiled
**every** kernel with the deterministic flags `--fmad=false --prec-div=true --prec-sqrt=true` (for cross-backend bit-exactness).
`--fmad=false` **disables FMA fusion**, forcing every `a*b+c` into a separate multiply + add — literally **halving** the FLOP
throughput of a GEMM inner loop (which is all `acc += a*b`).

But `--fmad=false` is only needed for the **bit-exact tier (T3, `TileSchedule::fma == false`)**. The **fast/perf tier
(T1, `fma == true`)** is EXPLICITLY ULP-tolerant, not bit-exact — so crippling it with the determinism flag is pure loss. Fix:
`compile_cubin(..., bool allow_fma)`; the fast-tier WarpTiled GEMM (`tiled && sched.fma`) compiles with `--fmad=true` (2 opts:
`--fmad=true` + arch), everything else keeps the 4 deterministic opts. Result: **0.49× → 0.75–1.04× cuBLAS** (beats it on 1024³
on warm-clock runs, 13 107 GFLOP/s). Applies to BOTH `run()` (thread a `fast_fma` bool from the tiled/fused Contract branches)
and `time_contract_schedule` (the autotuner's inner loop).

**Consequence to watch:** with FMA on, the fast tier is no longer bit-identical ACROSS different tile configs (NVRTC contracts
differently per code shape) — only SAME-config replay is bit-identical. That's correct (T1 is the ULP tier), but a determinism
test that compared two DIFFERENT configs' outputs (last-measured vs winner) then failed — compare the SAME schedule twice.

**Why:** the determinism moat and the perf moat are SEPARATE tiers; applying the bit-exact compile flags globally silently taxes
the perf tier 2×. **How to apply:** any deterministic GPU compiler with a fast/exact tier split must gate `--fmad`/`-ffp-contract`
(and `--use_fast_math`, `--prec-*`) on the TIER, never globally. The bit-exact tier keeps them; the ULP tier drops them. Related:
[feedback_bit_exact_fft_crushes_only_when_dram_bound](numerics-and-performance.md#memory-feedback_bit_exact_fft_crushes_only_when_dram_bound), [feedback_gpu_cost_model_must_include_register_occupancy](device-programs.md#memory-feedback_gpu_cost_model_must_include_register_occupancy),
[project_v17g_gemm_cublas_parity_89pct](project-history.md#memory-project_v17g_gemm_cublas_parity_89pct), [project_nrc_moat_fused_mlp_crushes_cublas](project-history.md#memory-project_nrc_moat_fused_mlp_crushes_cublas).


<!-- end-memory:feedback_fast_tier_must_enable_fma_bitexact_flags_cripple_gemm -->

<a id="memory-feedback_full_honest_evaluations_crush_every_metric"></a>
## feedback_full_honest_evaluations_crush_every_metric

---
name: full-honest-evaluations-crush-every-metric
description: "Report losses head-on, never as downplayed \"follow-ons\"; the crush mandate applies to EVERY measured metric (factor AND solve), not just the headline"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a9e2b042-249d-4290-abf9-3c707236fa70
---

**Give FULL, HONEST evaluations. A losing metric is confronted head-on and CRUSHED —
never buried as a "follow-on," footnote, or "remaining gap."**

**Why:** Strong user directive 2026-05-28 (emphatic): "why do you not try to fix the
solve loss and never mention it properly. I want FULL HONEST EVALUATIONS THIS IS VERY
IMPORTANT." When v5a-3 tree-parallel Cholesky crushed FACTOR (4.32× @148k) but LOST
SOLVE (bmwcra_1 235ms vs Eigen 134ms = ~1.75× SLOWER), I led with the factor crush and
relegated the solve loss to a one-line "follow-on." That reads as hiding the loss. The
user wants every losing number stated plainly (with the real ratio, e.g. "we are 1.75×
slower," not "0.57×") AND a plan to crush it — Cerid crushes on ALL metrics or the slice
isn't done. Reinforces [never_defer_solve](workflow-and-correctness.md#memory-feedback_never_defer_solve), [benchmarks_mandatory_at_slice_close](numerics-and-performance.md#memory-feedback_benchmarks_mandatory_at_slice_close),
[reference_implementations_are_the_floor](workflow-and-correctness.md#memory-feedback_reference_implementations_are_the_floor), [crush_mandate_bounded_by_importance](numerics-and-performance.md#memory-feedback_crush_mandate_bounded_by_importance).

**How to apply:**
- In any bench/eval report, state losing metrics FIRST or with equal weight to wins, in
  plain terms (the slower-by ratio, the absolute ms). No softening ("competitive", "~0.9×",
  "follow-on") when we actually lose.
- A slice that wins one metric and loses another is NOT closed — the losing metric is the
  next thing to crush, immediately, not deferred.
- "Crush all frontier libraries" means every consumer-relevant metric (factor + solve +
  fill + memory), not just the headline number.
- The only acceptable "characterize and move on" is a measured WALL on a narrow regime with
  no consumer (per [crush_mandate_bounded_by_importance](numerics-and-performance.md#memory-feedback_crush_mandate_bounded_by_importance)) — and even then, state it
  bluntly as a loss, not a footnote.


<!-- end-memory:feedback_full_honest_evaluations_crush_every_metric -->

<a id="memory-feedback_gated_vs_reference_benches_need_flag_to_validate"></a>
## feedback_gated_vs_reference_benches_need_flag_to_validate

---
name: gated-vs-reference-benches-need-flag-to-validate
description: "vs-reference benches are gated behind CRD_BUILD_HESAP_VS_REFERENCE=OFF by default; per-slice-check + full-sweep never build them, so changes to their CMake or .cpp must be validated by configuring with the flag ON explicitly"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 8232c613-08b5-412f-9d51-73f8c05a65d9
---

The hesap vs-reference benches (`bench_hesap_{gemm,blas1,blas2,solvers}_vs_reference`)
are gated behind `if(CRD_BUILD_HESAP_VS_REFERENCE)` in `runtime/CMakeLists.txt`,
which defaults OFF. The standard presets do NOT set it, and neither
`scripts/per-slice-check.ps1` nor `scripts/full-sweep.ps1` enables it.

**Why:** building them fetches Eigen + builds OpenBLAS from source (slow), so
they're opt-in for the shootout, not part of the routine DoD.

**How to apply:** any change to the bench `.cpp` files OR the
`crd_add_hesap_vs_ref_bench()` CMake helper is INVISIBLE to the sweep — a
green 5-config DoD says nothing about whether they still build. Validate
explicitly:
```powershell
cmake --preset win-debug -DCRD_BUILD_HESAP_VS_REFERENCE=ON
cmake --build --preset win-debug --target bench_hesap_gemm_vs_reference `
  bench_hesap_blas1_vs_reference bench_hesap_blas2_vs_reference `
  bench_hesap_solvers_vs_reference
cmake --preset win-debug -DCRD_BUILD_HESAP_VS_REFERENCE=OFF   # restore clean cache
```
The flag is a CACHED variable — it persists in the build dir until explicitly
reset, so reconfigure OFF afterward or the next sweep's win-debug will also try
to build the benches (and OpenBLAS). Case study: v0f CMake-helper refactor
2026-05-20 — the helper parsed at configure time but the calls live inside the
gated block, so only `-DCRD_BUILD_HESAP_VS_REFERENCE=ON` actually expanded and
validated them. Same gate applies to any future module that adds
fetch-heavy reference-comparison targets behind an opt-in flag.


<!-- end-memory:feedback_gated_vs_reference_benches_need_flag_to_validate -->

<a id="memory-feedback_gcc_linux_double_to_float_narrowing"></a>
## feedback_gcc_linux_double_to_float_narrowing

---
name: feedback-gcc-linux-double-to-float-narrowing
description: "In `<MathScalar T>` template code with `T = f32` instantiation, avoid `T{double_literal}` for non-exact-representable defaults + constexpr — gcc-linux fires `-Wfloat-conversion -Werror` (silent on MSVC per-slice DoD). Use `static_cast<T>(literal)` instead."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

**Pattern that bites in cluster-close full sweeps**: inside template
code `template <MathScalar T> ...`, default initializers like

```cpp
T x = T{1e-10};            // ← BAD when T = f32
T y = T{1.41421356};       // ← BAD
T mu = T{-0.53};           // ← BAD
constexpr T kPi = T{3.14159265358979323846};  // ← BAD when T = f32
```

cause gcc to fire `-Wfloat-conversion -Werror` on the f32
instantiation because the double literal isn't exactly representable in
float — `T{1e-10}` evaluates `1e-10` as `double`, then narrows to f32.

**Why MSVC per-slice DoD doesn't catch this**: MSVC's `/W4 /WX` does
NOT include this specific narrowing warning at default. The per-slice
DoD (win-debug + win-asan + win-shipping + win-tidy) ships them silently.
Only the wider full sweep — specifically `linux-gcc-*` configs with
`-Wall -Wextra -Wpedantic -Werror -Wconversion -Wfloat-conversion` —
catches them. **A cluster's per-slice DoDs can be all-green and the
cluster-close 18-config sweep still fail.**

**Fix (always use this pattern in `<MathScalar T>` template code):**

```cpp
T x = static_cast<T>(1e-10);            // ← explicit cast, no warning
T y = static_cast<T>(1.41421356);
T mu = static_cast<T>(-0.53);
constexpr T kPi = static_cast<T>(3.14159265358979323846);
```

`static_cast<T>(literal)` is the documented "I know this is a
narrowing conversion and I want it" form. Compilers don't warn on
explicit casts.

**Exact-representable doubles are FINE**: `T{0.5}`, `T{0.25}`,
`T{0.125}`, `T{0.375}`, `T{0.625}`, `T{0.75}`, `T{1.0}`, etc. — all
exactly representable in both f32 and f64. `T{3.14}` is NOT (3.14 has
no exact binary representation), and gcc DOES warn on it.

**Don't apply to non-template code**: `f32 x = 1e-10;` is also a
narrowing conversion but doesn't warn under `-Wfloat-conversion`
because the literal type is the LHS type — no template ambiguity.
The issue is specifically `T{double_literal}` where T is a template
parameter that may be f32.

**How to apply going forward in geometry/math modules:**
1. When writing `template <MathScalar T>` code, any default
   initializer or local constexpr with a numeric literal that ISN'T
   exactly representable in f32: use `static_cast<T>(literal)`, never
   `T{literal}`.
2. Run `linux-gcc-shipping` config (the strictest gcc-Wconversion
   config) during slice development if you suspect new narrowing
   patterns. Don't wait for the cluster-close 18-config sweep to
   catch them — it costs an extra sweep round per missed instance.
3. First instance: 2026-05-17 v7-close on `crd-geometry-mesh-
   processing` cluster — 12 sites (`T{1e-10}` × 3, `T{1e-20}` × 5,
   `T{-0.53}` × 1, `T{1.41421356}` × 1, `T{kPi/kTwoPi}` × 2). All
   fixed via `static_cast<T>(literal)`. Two-round retry sweep.


<!-- end-memory:feedback_gcc_linux_double_to_float_narrowing -->

<a id="memory-feedback_gemm_beta0_must_store_zero_and_umr_validation"></a>
## feedback_gemm_beta0_must_store_zero_and_umr_validation

---
name: feedback_gemm_beta0_must_store_zero_and_umr_validation
description: "GEMM beta=0 must STORE zero (not 0*C); the {1..16} moat cannot catch a UMR — validate uninit scratch with NaN-poison/ASan, not the moat"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 3571170c-1886-4c83-b787-c787213d0125
---

Two coupled lessons from v7-e-2 (uninit-scratch lever on supernodal Cholesky).

**1. GEMM `beta == 0` MUST store zero, never `beta * C`.** `blas3.cpp` scaled C as `c = beta * c` with no
`beta==0` branch (the comment claimed one; code lacked it). `0 * NaN = NaN`, so gemm beta=0 *read* C and propagated
non-finite garbage from uninitialized memory — violating the BLAS contract (*beta=0 ⇒ C not referenced on input*).
Harmless with zeroed C (`0*0=0`); corrupts on uninitialized scratch, AND latently on any `refactorize` whose pages
hold a prior factor's Inf/NaN. Fix: `if (beta==0) c=0; else c=beta*c;` in ALL gemm sites (gemm, small_gemm,
gemm_parallel, gemm_mixed). **Bit-identical for finite C** (`0*finite=±0.0`, `±0.0+alpha·AB = alpha·AB`) ⇒ moat-safe.

**Why:** enables leaving GEMM/copy scratch UNINITIALIZED (`Array::resize_uninitialized`) — the value-init memset of a
big per-worker scratch (~2.2GB / ~180ms on lat32, re-paid every refactorize) was 13% of the factor wall.

**How to apply:** before switching any scratch to uninit, audit every consumer is write-before-read; remember gemm
beta=0 only sanitizes the read AFTER this fix. Any BLAS-spec `beta=0 ⇒ C untouched` reliance needs the store-zero path.

**2. The {1..16} determinism moat CANNOT catch an uninitialized read.** Reused resident pages read
coincidentally-identical bytes across runs ⇒ the result stays bit-identical while being a UMR. ASan does NOT catch
UMR either (it's allocated memory; MSan would, but no MSan on Windows/MSVC). **Validate uninit scratch with a
NaN-poison:** `0xFF`-fill the uninit buffer (⇒ NaN for f32/f64/complex); any read-before-write ⇒ NaN into the result
⇒ NOT-SPD / wrong residual. This CAUGHT a real read-before-write (lat32 root nc=5385 → NOT-SPD info=529) that the
win-debug moat PASSED (small grids use the thin scalar cdiv path = no scratch). Run the poison on a BIG problem that
exercises the blocked path. Keep ASan too (it still catches OOB from new scratch indexing). See [project_v7_optimization_plan](project-history.md#memory-project_v7_optimization_plan).


<!-- end-memory:feedback_gemm_beta0_must_store_zero_and_umr_validation -->

<a id="memory-feedback_hesap_clean_structure_over_calendar"></a>
## feedback_hesap_clean_structure_over_calendar

---
name: feedback_hesap_clean_structure_over_calendar
description: "hesap's ONLY success metric is a clean structure usable for years without problems — calendar/timing is explicitly NOT a constraint"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 021fa904-7b98-4a07-9c4e-4f1cb715ea28
---

User directive 2026-05-22 (after closing hesap v3a-2.5): **"calendar and timing is not important. The only important thing is to create clean hesap structure to use for the upcoming years without problems."**

The success metric for `crd-hesap` is NOT how far down the v0–v18 roadmap we get, nor how fast slices close. It is: **is hesap a foundation we can build on for years without it biting back.** Calendar is free; structural soundness is the only currency.

**Why:** hesap is multi-year load-bearing substrate (dense + sparse + ordering + eig/SVD + future sparse-direct/Krylov/FFT). Every later module and `crd-eylem` physics will sit on it. A latent correctness bug, an API that churns, an untested path, or a leaky module boundary becomes a recurring tax for years. Speed of slice-closure has zero value against that.

**How to apply:**
- Take all the time a routine needs (esp. v3a-3 MRRR, v3d AED — the hard gates). Faithful + clean + exhaustively tested beats fast. Never cut a corner to close a slice "tonight."
- Reinforces [feedback_never_defer_solve](workflow-and-correctness.md#memory-feedback_never_defer_solve), [feedback_elite_only_no_shortcuts](workflow-and-correctness.md#memory-feedback_elite_only_no_shortcuts), [feedback_quality_bar](workflow-and-correctness.md#memory-feedback_quality_bar) — but goes further: even the *appearance* of calendar pressure is not a reason to compromise structure.
- Distinguish two kinds of "deferred" items: (a) **perf-characterization walls** the user explicitly accepted (memory-bound spmv, N=64 Hermitian tie, small-N Cholesky layout-fit) — these are physics, NOT structural problems, leave them; (b) **structural risks** (untested paths, latent UAF/overflow class bugs, API instability, module-boundary leaks, filed follow-ons that are correctness/architecture not perf) — these DO matter and should be solved, not carried.
- Guard the module structure: clean one-way deps (`hesap` substrate → `hesap-dense`/`hesap-sparse`/`hesap-ordering`/`hesap-sched`), stable public APIs, determinism contracts (D-pins in ADR-0065 §14/§15/§16/§17), allocator propagation ([feedback_hesap_propagate_allocator](numerics-and-performance.md#memory-feedback_hesap_propagate_allocator)), CLI-per-op, two-layer typed boundary. These are the "no problems for years" levers.


<!-- end-memory:feedback_hesap_clean_structure_over_calendar -->

<a id="memory-feedback_hesap_cli_command_for_every_op"></a>
## feedback_hesap_cli_command_for_every_op

---
name: hesap-cli-command-for-every-op
description: "Every hesap op MUST have a CLI command, registered in the SAME slice that ships the op (never batched at cluster-close). A hesap op without a CLI command is an incomplete slice. ADR-0065 §13 D16."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 8232c613-08b5-412f-9d51-73f8c05a65d9
---

In `crd-hesap`, **every public op × type variant gets a registered CLI command**
— no exceptions. This is ADR-0065 §13 D16 (agent-native plumbing) and the user
reinforces it strongly.

**Why:** Cerid is agent-native ([project_agent_native_engine_strategic_direction](project-history.md#memory-project_agent_native_engine_strategic_direction)) —
the CLI/RPC surface is a first-class consumer, not an afterthought. AI agents and
the future `crd-cli`/notebook surface drive hesap through these typed commands.
A numerical op that can't be invoked from the CLI is, for Cerid's purposes,
unfinished.

**How to apply:**
- Register the CLI command in the **same sub-slice that ships the op**, not in a
  batched "CLI sweep" at cluster-close. Precedent: v0b registered 28 BLAS-L1
  commands, v0c 17 (L2), v0d 14 (L3), v0e-g 8 (solvers) — each as that slice
  shipped. A cluster-close "CLI audit" only *verifies* completeness; it does not
  do the bulk registration.
- Each command: typed `CommandSchema` + structured `CommandResult` output
  (ResultError / ResultScalar / ResultVoid / …) + auto-emitted MCP descriptor,
  via the `cli::register_module_commands` static-init hook + an anchor symbol
  ([feedback_static_lib_anchor_symbol](workflow-and-correctness.md#memory-feedback_static_lib_anchor_symbol) — MSVC drops static-only `.obj` from a
  `.lib` without it).
- Cover all 4 type variants (f32/f64/c32/c64) where the op has them.
- When planning a hesap slice, the op list and the CLI-command list must be the
  same length. If an op has no command line in the plan, the plan is wrong.

Case: v1 sparse plan (2026-05-20) initially parked "full CLI sweep" in the last
sub-slice — user caught it ("we must have commands for everything"). Fixed to
per-slice CLI registration in v1a–f with v1g doing only the completeness audit.


<!-- end-memory:feedback_hesap_cli_command_for_every_op -->

<a id="memory-feedback_hesap_propagate_allocator"></a>
## feedback_hesap_propagate_allocator

---
name: hesap-propagate-allocator
description: "In crd-hesap (and other library code), NEVER allocate scratch from `crd::memory::default_allocator()` — it is plain MallocAllocator. Propagate the allocator from the input value type (Matrix::allocator(), Symmetric::allocator(), Vector::allocator(), or a scratch IAllocator* parameter)."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 8232c613-08b5-412f-9d51-73f8c05a65d9
---

**Rule:** in crd-hesap, crd-eylem, and any other library module, **never**
allocate scratch buffers from `crd::memory::default_allocator()`. It is
the `MallocAllocator` Meyers singleton — plain libc malloc. The whole
point of `crd-memory` is custom allocators: TlsfAllocator (O(1) bins),
LinearAllocator (bump), StackAllocator, PoolAllocator,
GrowablePoolAllocator, FrameArena, etc.

**Why:** the user owns the allocation strategy. Bench / test code
constructs a TlsfAllocator at the top of `main`/the test fixture and
passes it through. Library code that silently falls back to malloc
defeats the whole architecture — it leaks fragmentation across calls,
loses the bench's deterministic scratch lifetime, and conflicts with
ADR-0078's two-layer typed model (typed at API surface, raw in inner
kernels) which doesn't sanction implicit globals.

**How to apply:**
- Every `Matrix<T>`, `Vector<T>`, `Symmetric<T>`, `Triangular<...>`,
  `Hermitian<...>`, `Banded<T>` carries an `allocator()` getter. Use
  that for any scratch you allocate inside the function:
  ```cpp
  auto* scratch_alloc = a.allocator();
  T* buf = static_cast<T*>(scratch_alloc->allocate(n * sizeof(T), alignof(T)));
  ```
- For functions that don't have an obvious input-value-type allocator
  (free functions, helpers), take an `IAllocator*` parameter:
  ```cpp
  void some_blas_op(IAllocator* scratch, ...);
  ```
- `crd::jobs::frame_alloc` is the per-thread bump arena for jobs-level
  state. Don't use it for scratch that outlives a parallel_for.

**Case study 2026-05-20** (vs-ref-blas2 symv): I added row_scratch using
`crd::memory::default_allocator()`. User caught it with full force.
The same pattern was buried in `gemm_parallel`'s pack buffers and
`small_gemm_parallel`'s B-pack — those need the same fix.

**Audit checklist before claiming a hesap slice complete:** grep for
`default_allocator()` inside `engine/hesap*` and `engine/eylem*`. Any
hit not in a CLI argv-handling path is wrong.

Related: [quality_bar](workflow-and-correctness.md#memory-feedback_quality_bar) — elite-only, no shortcuts, single-path.


<!-- end-memory:feedback_hesap_propagate_allocator -->

<a id="memory-feedback_hesap_substrate_never_defer_features"></a>
## feedback_hesap_substrate_never_defer_features

---
name: feedback_hesap_substrate_never_defer_features
description: "For the hesap substrate, NEVER defer a feature/method/variant as \"revisit if/when a consumer needs it\" — build the COMPLETE surface; deferral of scope is banned"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 178961b8-0463-4615-82ce-96bde42457f0
---

**STRONG user directive (2026-05-25): for the hesap numeric substrate, NEVER defer a
feature, method, algorithm variant, or capability as "deferred / revisit if a consumer
needs it / out of scope for now."** The substrate's job is to be the universal, complete
numeric core that every domain (games, cinematics, engineering, maths, physics, chemistry)
builds on — so completeness IS the requirement. If a family has variants (every Krylov
method, every preconditioner, block/multi-RHS forms, nested/inner-Krylov, AMG variants,
complex variants, GPU mirror), build ALL of them in the slice/cluster that owns the family.
Do NOT split out "the exotic ones" as a follow-on, and do NOT write "deferred until a
consumer pulls."

**Why:** the universal-substrate ambition only holds if the substrate is actually complete;
a half-built family ("we'll add GMRES-recycling/block-Krylov/AMG later") leaves consumers
reaching for an external library, which defeats the entire thesis. The user explicitly
wants determinism + correctness + performance across the FULL surface, not a curated subset.

**How to apply:**
- When planning a cluster, enumerate the COMPLETE family (all solvers, all preconditioners,
  all variants, block + complex + nested forms) and put every item in the plan. No "NOT in
  this version / deferred / revisit" section for substrate features.
- The ONLY thing that may be "characterized and accepted" is a true hardware/layout perf
  WALL where the work is SHIPPED (not deferred) and a deeper kernel can't beat it — that's
  [feedback_crush_mandate_bounded_by_importance](numerics-and-performance.md#memory-feedback_crush_mandate_bounded_by_importance), and it is NOT a feature deferral.
- Calendar is NOT a reason to defer ([feedback_hesap_clean_structure_over_calendar](numerics-and-performance.md#memory-feedback_hesap_clean_structure_over_calendar)):
  a complete-but-multi-month cluster beats a fast-but-partial one.

**Reconciles with [feedback_ship_at_consumer_template_from_day_one](workflow-and-correctness.md#memory-feedback_ship_at_consumer_template_from_day_one):** that memory's
"defer speculative consumer-SPECIFIC paths" clause does NOT apply to substrate FEATURES — a
general numeric method (a Krylov solver, a preconditioner, AMG) is core substrate surface,
not a speculative consumer-specific API. Build it. Refines/overrides the defer clause for
hesap. Case: v4 planning — user mandated block-Krylov + AMG + inner-Krylov-as-preconditioner
+ everything I'd proposed deferring all be IN v4 (only sparse-DIRECT factorizations stay v5).


<!-- end-memory:feedback_hesap_substrate_never_defer_features -->

<a id="memory-feedback_iterative_bench_matched_true_residual"></a>
## feedback_iterative_bench_matched_true_residual

---
name: feedback_iterative_bench_matched_true_residual
description: "Iterative-solver vs-reference benches must compare at MATCHED TRUE residual, not the Krylov recurrence residual"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 50f493b0-787d-41e4-9bb7-95fea7d05959
---

When benching a preconditioned Krylov solve vs a frontier (Eigen etc.), compare at **matched
TRUE residual** `‖b − A·x‖₂/‖b‖₂`, computed via the operator on the final `x` — NOT the
solver's reported `final_residual_norm`. The Krylov **recurrence** residual drifts BELOW the
true residual on ill-conditioned A, so a solver that stops on the recurrence residual at
`rel_tol` actually stops EARLY (true residual much looser) and the wall-time win is partly an
artifact of unequal stopping. Eigen's iterative wrappers report the true residual, so a naive
comparison is apples-to-oranges even for the SAME algorithm.

**Why:** this is the same fairness failure `[[feedback_iterative_crush_claim_same_algorithm]]`
guards, in a subtler form — same algorithm, different EFFECTIVE tolerance. A headline like
"crushes Eigen 6.65×" can collapse to 3.70× once accuracy is matched.

**How to apply:** in the bench, (1) compute the true residual for the Cerid side via
`op.apply(x) → ‖b−Ax‖/‖b‖`; (2) drive Cerid to a tight `rel_tol` (e.g. 1e-12) so its true
residual lands in the reference's accuracy regime; (3) report **time-per-iteration** as the
durable structural headline (the unambiguous win, e.g. no-tri-solve parallel apply), with
total wall at matched accuracy alongside. Case: v4i-1 FSPAI-PCG — recurrence-residual headline
3.2–10.7× wall corrected to 2.1–9.8× wall + 1.9–3.1× per-iteration at matched true residual
(still a real same-role crush; advisor caught the dismissal at close).


<!-- end-memory:feedback_iterative_bench_matched_true_residual -->

<a id="memory-feedback_iterative_crush_claim_same_algorithm"></a>
## feedback_iterative_crush_claim_same_algorithm

---
name: feedback_iterative_crush_claim_same_algorithm
description: "Iterative-solver crush claims must be same-algorithm; cross-algorithm convergence gaps aren't kernel quality; verify+label the reference's conv/fail state"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b7a3a237-3bf1-46e1-9658-3a8d33e2d696
---

When benching an iterative solver vs a frontier library for the "crush" mandate, three disciplines (learned in hesap v4b FGMRES vs Eigen):

1. **The crush claim is SAME-ALGORITHM.** FGMRES-vs-Eigen-GMRES is apples-to-apples (kernel + per-iteration throughput). FGMRES-vs-Eigen-BiCGSTAB is algorithm-vs-algorithm: when restarted GMRES(m) STAGNATES on a hard nonsymmetric matrix (sherman3) while BiCGSTAB converges, that is a textbook GMRES(m) limitation, NOT a kernel defect. Don't report it as a loss to crush — frame it as algorithm-appropriateness (→ the reason v4c BiCGSTAB exists), and headline the same-algorithm win + the throughput win where convergence is symmetric (gemat11: both GMRES-class cap out, we're 1.84×/2.47× faster). The determinism moat is the universal differentiator (no frontier ships it).

2. **Verify + label the reference's convergence state from its actual info flag, never assumed.** Eigen's UNSUPPORTED GMRES reported "1 it, conv" on sherman3 — spurious (the unreliability that makes it `unsupported`); printing "1 it 0.30 ms" unlabeled reads as "Eigen wins". Tag every line `(conv)`/`(cap)`/`(fail)` from the solver's reported state (`info()==Success`). If still suspicious, compute the TRUE residual ‖Ax−b‖/‖b‖ on the returned x rather than trusting the flag. Same discipline as the v4-corpus "build_csr on real data" gap — tag without verifying hides bugs in EITHER direction.

3. **Preconditioners degrade gracefully, never assert.** A zero/missing diagonal in JacobiPreconditioner → inv=1 (identity for that row), not a crash. General nonsymmetric matrices (circuit/optimization, e.g. gemat11) have structural-zero diagonals; an assert there makes the solver unusable. SPD/HPD inputs never hit it. (Cerid is more robust than Eigen's DiagonalPreconditioner, which stores 1/diag and lets the inf propagate — state what CERID does, don't claim to "match Eigen" without checking.)

**Why:** the crush mandate is real, but an overclaimed or mislabeled crush is worse than an honest one — a reviewer who checks finds the hole. Honest framing (same-algorithm crush + throughput win + determinism moat + named algorithm limitations) is unassailable.

**How to apply:** every v4c–v4m solver bench: compare same-algorithm first; label conv/fail from the info flag; if a cross-algorithm comparison shows a "loss", check it's not just algorithm-appropriateness before treating it as a kernel problem. Preconditioners + solvers degrade gracefully on degenerate input. Also: complex `operator/` trips a C4723 Release-only false positive — wrap with a function-local `#pragma warning(disable:4723)`, not a global suppression.


<!-- end-memory:feedback_iterative_crush_claim_same_algorithm -->

<a id="memory-feedback_lss_round_cone_four_defects_and_tmax_roundtrip"></a>
## feedback_lss_round_cone_four_defects_and_tmax_roundtrip

---
name: feedback_lss_round_cone_four_defects_and_tmax_roundtrip
description: "The Quilez round-cone intersector had 4 defects ALL invisible for a capsule (rr==0), plus a tmax round-trip through rlen that clobbered the accumulated u - found only by a ray-march ground truth"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Two separate scars in the same function (`lss::lss_intersect`, B18-f, 2026-07-20). Both are shape-general lessons.

## 1. Four defects, every one invisible for a CAPSULE

The Quilez round-cone form had: `d2 = m0 + rr²` (should be `m0 − rr²`); the k-coefficients scaled by `m0`; the axial
span tested against `m0` instead of `d2`; and **the ray direction not normalised** (the reference form assumes a unit
direction). Result: hits reported in empty space — **118 of 132 reported hits were off-surface**.

**Why they survived four gates:** all four vanish when `rr == ra − rb == 0`, i.e. for a constant-radius capsule, which
is what the first gates tested. A tapered segment is the only configuration that exercises them.

**How to apply:** when a primitive has a degenerate special case that most tests happen to use, gate the GENERAL case
explicitly. And re-reading the code failed twice here — what found it was building a **dense ray-march ground truth**
(Python, independent of the formula) and comparing hit-by-hit. Corrected code then matched the GPU's 57 hits exactly,
zero off-surface. Same lesson as [feedback_measurement_lever_needs_second_matrix_check](workflow-and-correctness.md#memory-feedback_measurement_lever_needs_second_matrix_check).

## 2. `tmax · rlen · (1/rlen)` is NOT the identity in f32

The intersector worked in unit-direction units internally (`tmaxu = tmax · rlen`) and converted back with `· rinv`.
On a MISS that returned `tmax` a hair BELOW the value the caller passed in. Callers sweep segments with
`bu = select(t < best, u, bu); best = t;` — so **a miss tested as "nearer" and clobbered the accumulated `u` with the
miss value 0**. A shaded strand would take the ROOT's tangent and radius at an arbitrary point along its length. It
also drifted `t` ~100 ulp over an 8-segment sweep.

**Fix:** track a `hit_any` flag and return the caller's `tmax` VERBATIM on a miss —
`t_out = select(hit_any, best · rinv, tmax)`.

**How it surfaced:** the GPU gate reported `maxabs 9.03e-01` with **zero hit/miss disagreements** — t matching to ~1e-5
while `u` was 0 on the GPU and a real interior value on the oracle. Both backends failed IDENTICALLY, which ruled out
an emitter divergence and pointed at the IR. Comparing t and u TOGETHER is what caught it: a `u` that disagrees while
`t` matches means the two sides picked different surfaces at the same distance — invisible to a depth-only check.

Related: [feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage](device-programs.md#memory-feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage) (same slice),
[feedback_ckir_rt_inline_rayquery_scars](device-programs.md#memory-feedback_ckir_rt_inline_rayquery_scars) (`rayQueryGenerateIntersectionEXT` needs tHit inside the ray's CURRENT
range, which traversal narrows on every commit — seeding from the original tmax clobbers nearer hits).


<!-- end-memory:feedback_lss_round_cone_four_defects_and_tmax_roundtrip -->

<a id="memory-feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip"></a>
## feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip

---
name: feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip
description: "To prove a new execution path (e.g. routing the frame through CEIR), run the FULL suite — composition + reload + app-custom — through it on a REAL device; a device-free cook round-trip passes while the on-device render fails."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-11T18:38:26.082Z
---

CEIR-15e: to prove the shipping renderer could render through the `ceir.frame` path (Fork E:
`desc → to_ceir_frame → from_ceir_frame → desc' → same runtime`), the device-free 15e-a gate lowered every
shipped frame asset to a BYTE-EQUAL `FrameGraphTemplate` — a strong, true result. It still MISSED a real defect:
the shipping renderer could not render an app-**COMPOSED** graph (RAF-10 `app_custom`: `[[include]]` + `[[anchor]]`
+ `[[inject]]` + a custom C++ pass) through the CEIR path. `flatten_frame_graph` left a residual `[[anchor]]` in its
output (it carried anchors forward through expansion so a parent inject could find a child's extension point, but never
cleared the spent scaffolding), and `to_ceir_frame` correctly rejects any residual include/anchor/inject as
un-flattened composition — so `set_frame_graph("app://frame/app_custom")` failed only with the flag ON.

**Why the device-free gate was blind:** all 15 shipped assets are STANDALONE (no composition) with builtin executors.
Composition, injected custom executors, and hot-reload live ONLY in the scene-render suite's on-device tests
(RAF-10 Vulkan+DX12, RAF-11 reload). The cook round-trip never exercised them.

**How to apply:** when you insert a new execution path behind a flag, gate it by running the ENTIRE existing suite
through it (flag-ON) and requiring an IDENTICAL pass/fail set to a fresh flag-OFF baseline — on a real device, not
just the cook/round-trip harness. The surfaces the round-trip harness cannot reach (composition, app-registered
executors, reload, app-custom renderers) are exactly where the new path breaks. On this Windows host a real Vulkan
AND DX12 device is available once `CRD_ASSETS_DIR` is set — so the on-device A/B was doable today, no llvmpipe needed.
(The 22 "GPU test failures" when running the bare exe were purely the missing `CRD_ASSETS_DIR` env — `init_programs`'
first act is `asset_text("vertex/scene.crdv")`; see [feedback_sandbox_smoke_overlay_only_false_green_needs_crd_assets_dir](workflow-and-correctness.md#memory-feedback_sandbox_smoke_overlay_only_false_green_needs_crd_assets_dir).)

Kin: [feedback_cook_only_gates_ship_device_impossible_programs](workflow-and-correctness.md#memory-feedback_cook_only_gates_ship_device_impossible_programs) (cook-clean ≠ device-runnable),
[feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders](rendering.md#memory-feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders) (DELETION IS THE PROOF needs a real render),
[feedback_verifier_that_materializes_its_input_must_surface_a_failed_materialization](rendering.md#memory-feedback_verifier_that_materializes_its_input_must_surface_a_failed_materialization).


<!-- end-memory:feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip -->

<a id="memory-feedback_no_malloc_no_stdvector_in_benches"></a>
## feedback_no_malloc_no_stdvector_in_benches

---
name: feedback_no_malloc_no_stdvector_in_benches
description: "Bench/example files are NOT exempt from the no-malloc / no-STL-container rules — never MallocAllocator, never std::vector, even in runtime/examples benches"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b2a08d20-83df-413d-a84a-41fd3c2f00c5
---

**STRONG user directive (2026-05-27, emphatic).** `runtime/examples/` bench + smoke
files are NOT a gray area: the engine-wide rules apply to them in FULL.

- **NEVER `crd::memory::MallocAllocator` / `default_allocator()`.** Use a named
  `crd::memory::TlsfAllocator alloc{budget};` (the whole point of crd-memory is custom
  allocators). Propagate it to every `IAllocator*` ctor. See [feedback_hesap_propagate_allocator](numerics-and-performance.md#memory-feedback_hesap_propagate_allocator),
  [feedback_named_allocators_in_tests](build-and-verification.md#memory-feedback_named_allocators_in_tests).
- **NEVER `std::vector` (or any owning STL container).** Use `crd::containers::Array<T>`
  (`.data()` is contiguous → still works for ILUPACK C-API `double*` and Eigen
  `setFromTriplets(begin,end)` interop). See [feedback_no_std_array_use_crd_or_c_array](workflow-and-correctness.md#memory-feedback_no_std_array_use_crd_or_c_array).
- **USE crd TYPES, not raw `double`/`int`/`std::size_t`.** `crd::f64`/`crd::f32`,
  `crd::i32`/`crd::u32`, `crd::usize` — the fixed-width aliases from `crd/core/types.hpp`,
  everywhere on the Cerid side. Raw types stay ONLY at the literal third-party API boundary
  (ILUPACK `Dmat`'s `double*`/`integer*`, Eigen's `double`) where the foreign signature
  demands it. `crd::f64` IS `double` so it passes to those APIs unchanged.

**Why:** the user caught MallocAllocator + std::vector in the v4z bench files and was
furious — these are a load-bearing principle, not a "benches are throwaway" exception. I
had even PROPAGATED the bad pattern (added `std::vector`/`MallocAllocator`-using code into
`bench_hesap_ilu_vs_reference`/`mlilu_vs_ilupack` by mirroring the existing file instead of
fixing it). When editing a file that already violates a core rule, FIX the violation, don't
copy it.

**How to apply:** any new or edited bench/example/smoke uses TlsfAllocator + crd Array from
the first line. When touching a bench that still has the old pattern, convert the whole file
(global `MallocAllocator g_alloc` → `TlsfAllocator g_alloc{N<<20}`; every `std::vector<T> v`
→ `crd::containers::Array<T> v(&g_alloc)` + `.push_back`/`.resize`). Third-party interop
(Eigen/ILUPACK) takes `.data()`/iterators off the crd Array — no std container needed.
There is no CI guard for this in gated benches, so it must be done by discipline.


<!-- end-memory:feedback_no_malloc_no_stdvector_in_benches -->

<a id="memory-feedback_oracle_must_round_every_elementary_op"></a>
## feedback_oracle_must_round_every_elementary_op

---
name: feedback_oracle_must_round_every_elementary_op
description: "A f64 reference that rounds only on STORE is more accurate than the f32 kernel it certifies — bit-exactness needs rounding at EVERY elementary IEEE op, in the same order."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 9b330af5-88bb-499e-a92a-1752e160e0ba
---

⭐⭐ **An oracle that is MORE accurate than the thing it certifies cannot certify it.** `ckir_eval`'s A3 vec/mat corpus
(`Dot`, `VecLen`, `Normalize`, `Cross`, `MatVecMul`, `MatMatMul`, `Determinant`, `MatInverse`, `OuterProduct`,
reflect/refract/faceforward, quats, `Slerp`) accumulated in **f64 and rounded only on store**, while the elementwise ops
and `Contract` rounded **every step** (`acc = round_dtype(acc + prod, dtype)`). So for an F32 graph the oracle computed
an f64 dot product rounded once, and any f32 GPU kernel rounds every multiply-add — a ~1 ULP delta where **the reference
was wrong-by-being-better**. This made ADR-0098's T1 "certified bit-exact core" unreachable for vec/mat, i.e. for most
of a shader.

**Why it hid for so long:** every Vulkan/DX12 vec test compared against **analytic** references with tolerances, never
against the oracle. Nobody *could* have gated bit-exactness against it, so nobody noticed the oracle couldn't be
matched. The blind spot was structural, not careless — it only surfaced when the CUDA fan-out (scalarized ⇒ explicit
elementary ops ⇒ genuinely bit-exact-able) first pointed the oracle at a vector graph and asserted `==`.

**How to apply:**
- A precision oracle must reproduce the target's **elementary IEEE operations, in the target's order** — not merely
  compute the same mathematical value in wider precision. Round after *every* mul, add, sub, div, sqrt.
- **Two conditions for bit-exactness, both required:** (1) the oracle rounds per elementary op; (2) the backend emits
  those same ops in the same order with **no FMA contraction** (`--fmad=false` on nvcc, `precise` in GLSL, `NoContraction`
  in SPIR-V). CUDA's scalarized emitter satisfies both ⇒ gated `==`. GLSL/HLSL/WGSL call `dot()`/`normalize()`/
  `inverse()` **builtins whose internal order is implementation-defined** ⇒ tolerance only, until a `float_controls`
  audit pins them. Name that gap; do not assume the builtin matches.
- Rounding by the node's dtype is **safe to retrofit**: `round_dtype` is the identity for F64, so f64 graphs are
  bit-unchanged (the 200-assert CPU suite passed first try).
- **Smell to look for:** a reference implementation whose tests all use tolerances against *analytic* values rather than
  against the reference itself. That is evidence nobody has ever checked the reference can be matched — see
  [feedback_tidy_gate_clean_on_unparsed_files](build-and-verification.md#memory-feedback_tidy_gate_clean_on_unparsed_files) for the same shape (an instrument nobody verified).

Related: [feedback_mission_portable_gpu_compute_all_backends](device-programs.md#memory-feedback_mission_portable_gpu_compute_all_backends) · [reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine) (rule #4: name what your
diagnostic can't see) · [feedback_glsl_writeonly_buffer_readback_portability](device-programs.md#memory-feedback_glsl_writeonly_buffer_readback_portability).


<!-- end-memory:feedback_oracle_must_round_every_elementary_op -->

<a id="memory-feedback_roundtrip_fixture_must_be_locked_valid_or_future_verifiers_break_it"></a>
## feedback_roundtrip_fixture_must_be_locked_valid_or_future_verifiers_break_it

---
name: feedback_roundtrip_fixture_must_be_locked_valid_or_future_verifiers_break_it
description: A converter/round-trip test fixture must ASSERT the source-format validator passes (lock it VALID) — else it is silently cook-INVALID today (nothing calls validate) and the semantic verifier you are ABOUT to add turns your own passing round-trip test RED
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-11T07:42:27.201Z
---

Building the CEIR-15c-0 `frame.history` round-trip fixture (`build_taa` in `test_frame_ceir.cpp`), I made a TAA-history
graph to exercise converter round-trip identity (`emit_frame_toml(desc) == emit_frame_toml(to→from(desc))`) +
`find_frame_misuse`-clean. It passed win-debug/asan. But the advisor caught it was silently **cook-INVALID**:

- the ping-pong image was `scale`-only-sized → `PersistentNeedsSize` (a persistent/ping-pong key must be stable across
  frames — scale-only is rejected; only `add_image`'s ABSOLUTE width/height is legal, NOT `add_scaled_image`).
- the resolve pass was `raster.geometry` with no draw list → `MissingDrawList`.

Both passed ONLY because no assertion called `validate_frame_graph`. The trap: my OWN next slice (15c-1) routes exactly
those `FrameCookError`s into new CEIR verifiers (`find_frame_misuse` resource-shape rows + the differential oracle
`validate_frame_graph(from_ceir_frame(m))`). The moment that verifier lands, the `CHECK(... == None)` in THIS fixture
goes red — a self-inflicted regression from a test that "passed."

**Why:** ⭐ `ceir.frame` is a STRICT SUPERSET of `FrameGraphDesc` — a fixture can hold a shape the round-trip preserves
faithfully yet the source format forbids. Round-trip identity proves the CONVERTER lossless; it says NOTHING about the
fixture being a legal program. "A round-trip lock on a cook-invalid graph is a weak lock." (advisor)

**How to apply:** any test whose fixture exists to prove a transform/round-trip is lossless — LOCK it valid at the
source: `REQUIRE(fb.validate() == FrameCookError::Ok)` (or the domain's validator) BEFORE the round-trip asserts. It
future-proofs the fixture against every validity check you are about to add, and a round-trip over a KNOWN-valid input
is the only meaningful one. Corollary for converter slices: a fixture must be clean under BOTH the source validator AND
the target `find_*_misuse` — and text-round-trip any NEW op through the printer/parser (a `find_*_misuse`-only or
emit-only test never exercises the text path — [feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero](execution-ir.md#memory-feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero)).
See also [feedback_run_only_the_tests_you_added](build-and-verification.md#memory-feedback_run_only_the_tests_you_added) (the sibling "your test silently didn't run / didn't check" class).

**Second facet — COVERAGE, not just validity (CEIR-15c-1c-2 audit).** A round-trip-IDENTITY gate proves losslessness
ONLY for the fields the FIXTURE exercises. `to_ceir_frame`/`from_ceir_frame` silently DROPPED five serialized fields —
`kind_2d`(dimension), `depth`, `FrameResourceRef.indexed`, the entire RAF-12.3 params bag (clear_color/blend/…), and
the top-level `requires_caps`/`fallback`/`memory_budget_bytes` — yet `emit_frame_toml(desc)==emit_frame_toml(to→from(desc))`
was GREEN for four slices, because `build_scene`/`build_taa` set none of them (every default-valued field emits nothing,
so a dropped default matches a dropped default). ⭐ The audit that actually finds this: enumerate what the SERIALIZER
emits (`emit_frame_toml`, field by field) and diff against what the CONVERTER carries — a green identity gate is NOT
proof of completeness, only proof about the covered subset. **How to apply:** when a round-trip gate claims a converter
is lossless, build ONE MAXIMAL fixture that sets EVERY non-default serialized field (the full-surface principle,
[feedback_generator_needs_full_surface_reference_input](workflow-and-correctness.md#memory-feedback_generator_needs_full_surface_reference_input)); until then, treat "the gate is green" as "the gate is green
for what the fixtures touch," and audit new representations field-by-field against the legacy serializer.


<!-- end-memory:feedback_roundtrip_fixture_must_be_locked_valid_or_future_verifiers_break_it -->

<a id="memory-feedback_shape_checker_mirrors_oracle_semantics"></a>
## feedback_shape_checker_mirrors_oracle_semantics

---
name: shape-checker-mirrors-oracle-semantics
description: "A deferred-to validator that does not exist keeps the bug class open; when built, its rules must mirror the ORACLE's read semantics exactly — first run found two latent defects in shipped light-cook paths"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 31e31376-4d57-4a00-b30c-77365444ac88
  modified: 2026-07-27T01:04:48.447Z
---

`nodes::detail::bin`'s mismatched-vector arm said "leave to the shape checker" for months — and no shape
checker existed, so every caller error cooked to a valid node id and failed in the SHADER COMPILER with
nothing pointing at the cause (the 38-E7 pcf-uv scar). REN-38 audit built `ckir_shape.hpp`.

**Why:** a comment deferring to a checker is a claim the codebase must be able to cash. And the checker's
rules must come from the ORACLE's actual read loops (strict elementwise width equality — a narrower operand
is an oracle OOB read while GLSL silently broadcasts; sampler-pairing keyed on the SAMPLER node because that
is what the emitter keys the combined type on), never from "what GLSL allows". On its FIRST run it caught the
PCSS blocker search reading depth through the comparison sampler (an overload that does not exist → new
required `shadow_plain_sampler` binding) and contact shadows feeding SCALARS to the 4-tap `contact_shadow`
helper (lanes 1..3 of a 1-wide value) — both shipped, both invisible to node-count close gates.

**How to apply:** when a builder defers an invariant "to the checker", grep that the checker EXISTS and runs
at the cook boundary with a pointing error (`ShapeIssue`-style node + reason). Derive each rule from the
oracle/emitter source of truth, then run it over the whole existing corpus — its first catches are usually
real. Related: [every-render-pass-through-our-own-frame-graph-machinery](rendering.md#memory-feedback_every_render_pass_through_our_own_frame_graph_machinery).


<!-- end-memory:feedback_shape_checker_mirrors_oracle_semantics -->

<a id="memory-feedback_simd_rowwise_unblocked_beats_blocked_smallk"></a>
## feedback_simd_rowwise_unblocked_beats_blocked_smallk

---
name: feedback_simd_rowwise_unblocked_beats_blocked_smallk
description: "For a column-oriented two-sided reduction (Hessenberg/tridiag/bidiag) in a ROW-MAJOR engine, a SIMD row-wise UNBLOCKED reduction beats a blocked dlahr2+gemm at moderate N — small-K gemm overhead + jobs-frame-arena exhaustion sink the blocked path"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 3f36099d-ab5d-457b-ab8e-a9c6b5a704a4
---

When implementing a **column-oriented two-sided reduction** (Hessenberg `dgehrd`,
symmetric tridiag `dsytrd`, bidiag `dgebrd`) in Cerid's **row-major** dense layout,
the fastest kernel at moderate N (≤ ~512) is a **SIMD row-wise UNBLOCKED**
reduction, NOT the textbook blocked LAPACK panel (`dlahr2`/`dlatrd` + gemm).

**Why (v3d-1a Hessenberg, 2026-05-23):** I ported the blocked `dgehrd`
faithfully (`dlahr2` panel producing V/T/Y=A·V·T + `gemm`/`dlarfb` BLAS-3
trailing). It was **recon-correct to n=512** but ran **0.2× Eigen** (4–5× SLOWER).
Three reasons: (1) the panel reduces nb=32 columns ⇒ the trailing `gemm` has
contraction dim K=32 — **small-K gemm is overhead-bound** (~5 GFLOPS, not the
50 GFLOPS the kernel hits at large K); (2) the panel's column ops are
**scalar + strided** in row-major (reflector columns stride by `ld`); (3) the
parallel trailing `gemm_parallel` **exhausts the jobs per-thread frame arena**
across the ~15 panels of one reduction (no `frame_reset` between panels) — it
CRASHED at n=512 and I had to fall back to serial gemm (slower still).

**The fix that beat Eigen 1.16–1.28× + LAPACK 2–14×:** drop blocking; do the
unblocked `dgehd2` but with the two-sided updates **row-wise** so every inner
loop is a CONTIGUOUS SIMD sweep in row-major:
- Left `(I−τvvᵀ)A`: `w := vᵀ·A_trail` by accumulating `w += v[r]·A_row[r]`
  (SIMD axpy over the contiguous row), then `A_row[r] -= τ·v[r]·w` (SIMD axpy).
- Right `A(I−τvvᵀ)`: per row, `wr := A_row·v` (SIMD dot), `A_row -= τ·wr·v`.
- Gather the reflector `v` into a CONTIGUOUS buffer once (v[0]=1 explicit) so
  the dot/axpy operands are both contiguous.

n=256 went 13 ms → 2.5 ms.

**Why:** Cerid is row-major; the competition (Eigen) is column-major + unblocked.
Blocking only pays when K (=nb) is large enough to amortize gemm packing — false
for reduction panels (nb≈32). The row-wise reformulation makes the BLAS-2 work
contiguous, matching Eigen's column-contiguous advantage without blocking.

**How to apply:** for v3d-1b/c and any future reduction, START with the SIMD
row-wise unblocked kernel. Only reach for blocked+gemm if profiling at LARGE N
(≫512) shows the O(n³) BLAS-2 traffic dominating — and then size nb so K
amortizes, and route the parallel trailing through a NON-frame-arena path (or
`frame_reset` between panels) to avoid [feedback_jobs_parallel_for_frame_arena_exhaustion](rendering.md#memory-feedback_jobs_parallel_for_frame_arena_exhaustion).
Related: [project_cholesky_smalln_rowmajor_limit](project-history.md#memory-project_cholesky_smalln_rowmajor_limit) (the row-major-vs-column
layout-fit, ADR-0083); [feedback_register_tiling_needs_packing](workflow-and-correctness.md#memory-feedback_register_tiling_needs_packing).


<!-- end-memory:feedback_simd_rowwise_unblocked_beats_blocked_smallk -->

<a id="memory-reference_cerid_math_mandate"></a>
## reference_cerid_math_mandate

---
name: reference_cerid_math_mandate
description: "The Cerid Math Mandate — engine math routes through crd::math (never std::); + the tx-a AUDIT that reframes the transcendental cluster (deterministic:: already exists but is NOT yet faster-than-libm)"
metadata: 
  node_type: memory
  type: reference
  originSessionId: d5b177a9-0034-4430-b88d-14a6ad594f07
---

## THE RULE (Cerid Math Mandate — the north star)
All engine + tool code uses `crd::math::*` for elementary math + transcendentals (exp/log/sin/cos/tan/pow/…), NOT
`std::`. If a function you need isn't in `crd::math` yet, you IMPLEMENT it there first (deterministic, ≤1 ulp gated
vs mpmath, benched vs libm) BEFORE using it — never fall back to `std::`, never proceed without it. Reason:
cross-platform bit-determinism (the certification moat — libm differs glibc≠MSVC≠Apple≠WASM) + speed + WASM. Plan:
`docs/phases/crd-math-transcendental.md`. ⚠ The HARD guard/enforcement flips ONLY after the library is upgraded to
actually-faster-than-libm (see the audit) — until then this is the DIRECTION, not a build-breaking guard.

## tx-a AUDIT (2026-06-25) — the cluster premise needed correction (search+measure-before-build)
**`crd::math::deterministic` ALREADY ships** sin/cos/tan/asin/acos/atan/exp/exp2/log/log2/pow for f32, f64, AND SIMD
(declared in `engine/math/include/crd/math/deterministic.hpp`, defined in `src/deterministic.cpp`). So the cluster is
NOT a green-field build. AND a newer f64 core exists: `crd::math::crd_exp1/crd_log1` (+AVX2 twins) in
`simd/transcendental.hpp`. Three partial homes ⇒ the cluster's real job is **UNIFY + UPGRADE-accuracy + COMPLETE-gaps
+ BENCHMARK + ROUTE + ENFORCE**, not build.

**Measured (gcc -O3 -march=native -ffp-contract=off, ns/call + ulp vs mpmath):**
- `crd_log1` (log core): **~1–2 ulp + 1.6× faster than std::log** (2.08 vs 3.37 ns) — GOOD, ship-grade. log2/log10/log1p built on it = 1–2 ulp.
- `crd_exp1` (exp core): **1.4× faster than std::exp (2.18 vs 3.02) BUT only ~1e-13 (degree-11 Taylor = HUNDREDS of ulp, NOT ≤1 ulp), and BROKEN for denormal results** (x≲−745: the `(ki+1023)<<52` exponent injection underflows → garbage). exp2/exp10/expm1 inherit this. ⇒ **the exp core needs a minimax rewrite + denormal fix** to hit the ≤1 ulp contract.
- `crd::math::deterministic::sin`: **2.5× SLOWER than std::sin** (3.68 vs 1.48 ns). So the EXISTING determinism lib does NOT meet the "faster than libm" goal for trig — it traded speed for bit-exactness. The "we're faster" premise is PROVEN for the new exp/log cores, UNPROVEN/false for the legacy deterministic:: trig.

**⇒ Reframed cluster:** (1) upgrade the exp core to minimax ≤1 ulp + fix denormals; (2) make trig faster-than-libm AND ≤1 ulp (the legacy deterministic:: trig is slower — this is the real work); (3) complete gaps (expm1/exp10/log10/log1p [done, ride cores], atan2/sincos/sinh/cosh/tanh/asinh/acosh/atanh/cbrt/hypot, select tier); (4) UNIFY the 3 homes into one `crd::math::*` surface; (5) route consumers + broaden the existing `check_no_std_math` guard (currently only eylem/hesap/geometry; the hesap-* siblings use std:: freely) — flip hard only once faster-than-libm holds.

**Select/exact tier gap-fills (2026-07-22)** — added NATIVE to `engine/math/include/crd/math/select.hpp` (own logic, not std
passthrough), **bit-exact vs std** (gate `tests/math/test_select.cpp`, 2700 assertions across ties/±0/subnormals/NaN):
`signbit` · `fmax`/`fmin` (IEEE maxNum, NaN-dropping) · `fdim` · `rint`/`nearbyint` (round-ties-to-even from trunc/abs/copysign) ·
`modf` (sign via copysign) · `frexp` (IEEE bit surgery, subnormals ×2^54) · `remainder` (fdlibm: `r=fmod(|x|,2|y|)` then reduce to
[−|y|/2,|y|/2], ties-to-even from the mod-2|y| step, each subtract exact by Sterbenz). ⛔ remainder scars: sign is `signbit(x)?-r:r`
NOT `copysign(r,x)` (remainder is ODD in x — `remainder(5.5,2)=-0.5`); tiny/zero |y| needs the `r+r` compare (`0.5*|y|` underflows);
y==0/±inf/NaN resolve free via fmod semantics. The IEEE single-instruction ops
(floor/ceil/round/sqrt/fma → ROUNDSD/SQRTSD/VFMADD) stay `std`-backed BY DESIGN — hardware-exact + deterministic; reimplementing is
slower for identical bits ("correctness is exactness"). So `crd::math` now covers the common `<cmath>` surface natively.

**Delivered in tx-a:** `engine/math/include/crd/math/transcendental.hpp` (unified facade: exp/log family incl. the gap
fns exp10/expm1/log10/log1p — log family ≤2 ulp; exp family rides the Taylor core, accuracy-upgrade pending). The
**reusable mpmath ulp-gate harness**: `tests/math/gen_transcendental_refs.py` → `transcendental_refs.inc` (the family
pattern: add a function = one FUNCS entry; gate ≤1 ulp vs mpmath). [reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine) [feedback_search_engine_before_building](build-and-verification.md#memory-feedback_search_engine_before_building)


<!-- end-memory:reference_cerid_math_mandate -->

<a id="memory-reference_eigen_complex_hessenberg_av_at_large_n"></a>
## reference_eigen_complex_hessenberg_av_at_large_n

---
name: reference_eigen_complex_hessenberg_av_at_large_n
description: Eigen complex HessenbergDecomposition<MatrixXcd> access-violates at n>=256 on the MSVC/AVX vs-ref build — a reference fault; cap complex Eigen refs at n<=128 in benches
metadata: 
  node_type: memory
  type: reference
  originSessionId: 14cd3cee-9b6f-4f3b-b641-1da39ae5b096
---

In the `CRD_BUILD_HESAP_VS_REFERENCE` benches on Windows (MSVC + `/arch:AVX2`,
Eigen fetched into `build/_deps`), **Eigen's complex
`HessenbergDecomposition<Eigen::MatrixXcd>::compute` ACCESS-VIOLATES (0xC0000005)
at n≥256** — the COMPLEX path only; the real-double
`HessenbergDecomposition<MatrixXd>` at the same n is fine. The reuse `.compute()`
pattern crashes too → a genuine Eigen-reference fault, not a usage bug and NOT
Cerid (proven 2026-05-24 v3d-2c-1 by stderr marker isolation: Cerid's
`cerid-done` printed, Eigen's `eigen-done` never did; Cerid was recon-clean to
n=512 + ASan-clean to n=256). **Root cause NOT pinpointed** — "size-dependent +
complex-only" suggests the AVX `Packet2cd` vectorized path, but that's an
inference, not verified. If a future session has time: rebuild with
`-DEIGEN_DONT_VECTORIZE` and rerun — stops crashing ⇒ vectorization-related.
Treat as reference fragility regardless.

LAPACK `zgehrd`/`zhseqr`/`zgeev` (OpenBLAS-generic on MSVC) are likewise fragile
at large n (n>128).

**How to apply:** in any hesap complex-eig bench (v3d-2c-2/2c-3 complex Schur,
complex `eig` vs Eigen `EigenSolver` + LAPACK `zgeev`), **cap the complex
references at n≤128** and time Cerid alone at n≥256 (scaling check) — exactly as
the existing real bench caps `dgehrd`/`dhseqr`. Print a `ref-AV`/`n/a` marker, do
NOT let the harness crash. The Cerid win is established in the stable regime + the
real-path trend confirms it holds. This is reference fragility, never a Cerid
defect — verify Cerid with recon + ASan to large n in the unit tests instead.
**CONFIRMED for `ComplexSchur` (2026-05-24 v3d-2c-2):** Eigen
`ComplexSchur<MatrixXcd>::computeFromHessenberg` ALSO access-violates at n≥256
(same MSVC/AVX build). Cap it at n≤128 too. Expect `ComplexEigenSolver` (2c-3)
to do the same — cap its complex `eig` bench at n≤128 from the start.


<!-- end-memory:reference_eigen_complex_hessenberg_av_at_large_n -->

<a id="memory-reference_eigen_incomplete_factorization_amd_reorders"></a>
## reference_eigen_incomplete_factorization_amd_reorders

---
name: reference_eigen_incomplete_factorization_amd_reorders
description: "Eigen's IncompleteLUT/IncompleteCholesky AMD-reorder internally; ILU reordering is regime-dependent"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 50f493b0-787d-41e4-9bb7-95fea7d05959
---

**Eigen `IncompleteLUT` and `IncompleteCholesky` apply an AMD fill-reducing reordering of the
symmetrized pattern (Aᵀ+A) internally** (`IncompleteLUT::analyzePattern` runs `AMDOrdering` and
`factorize` factors `amat.twistedBy(m_Pinv)`). So a bare Cerid ILUT/IC-vs-Eigen comparison is
NOT apples-to-apples — Eigen reorders, Cerid (default) doesn't.

**ILU/IC reordering is REGIME-DEPENDENT** (extends [feedback_nd_fill_regime_dependent_not_correctness](workflow-and-correctness.md#memory-feedback_nd_fill_regime_dependent_not_correctness)):
AMD shrinks the incomplete-factor fill and raises quality on **small/irregular** matrices
(sherman3: Cerid 2× behind → ~1.4× with AMD), but **SCRAMBLES the banded structure** Cerid's
parallel level-scheduled triangular solve exploits on **large structured** matrices — Cerid's
structure-preserving (unordered) default ILUT CRUSHES Eigen 2.28× on cd2d-200 (n=40000)
precisely because Eigen's *mandatory* AMD throws that structure away. So Cerid ships AMD as an
**opt-in `ReorderedPreconditioner<T,Inner>` adapter** (reordered.hpp; `M_inner≈(PAPᵀ)⁻¹` ⇒
apply=Pᵀ·M_inner⁻¹·P, reuses v2 `amd_order`), NOT a default — being more complete than Eigen
(default + opt-in) instead of forcing one regime.

**How to apply:** when benching Cerid ILU/IC vs Eigen, know Eigen reorders; report both Cerid
default (structure-preserving, wins large/structured) and Cerid+AMD (wins small/irregular).
Never wire AMD-reorder as the default — it regresses the large-structured regime Cerid wins.
Source read at v4i-1 (2026-05-26). See also [feedback_iterative_bench_matched_true_residual](numerics-and-performance.md#memory-feedback_iterative_bench_matched_true_residual).


<!-- end-memory:reference_eigen_incomplete_factorization_amd_reorders -->

<a id="memory-reference_ilupack_oracle_local_only"></a>
## reference_ilupack_oracle_local_only

---
name: reference_ilupack_oracle_local_only
description: "ILUPACK V2.4 reference oracle for crd-hesap v4j multilevel-ILU — local-only, non-commercial license, build recipe, C API"
metadata: 
  node_type: memory
  type: reference
  originSessionId: d400f20c-d99c-4350-a4ef-ff0b4873d173
---

ILUPACK V2.4 is the apples-to-apples reference for crd-hesap **v4j** (multilevel ILU, inverse-based pivoting — Bollhöfer-Saad SISC 2006, the algorithm v4j-2 ports). Set up during the v4j-2 prelude (2026-05-26).

**License = non-commercial / scientific use ONLY, binary-only** (`http://ilupack.tu-bs.de/copyright.shtml`): "freely available for scientific (non-commercial) use ... only for the purpose of internal research excluding any commercial use." No source — precompiled `.a` libs. Must cite Bollhöfer & Saad, *Multilevel preconditioners constructed from inverse-based ILUs*, SIAM J. Sci. Comput. 27(5):1627-1650, 2006.

**Policy decision (user, 2026-05-26):** treat as a **local-only, non-distributed oracle behind a dev-only flag `CRD_BUILD_HESAP_VS_ILUPACK`** — NOT the permissive-OSS `CRD_BUILD_HESAP_VS_REFERENCE` path (Eigen/OpenBLAS). Lives under gitignored `external/` (added to `.gitignore`). NEVER vendored, NEVER in CI, NEVER linked into a shipped artifact. CI/shipping use Bollhöfer-Saad published numbers + AMGCL (BSD) as the buildable peer.

**Build (WSL/Linux only — libs are gfortran-compiled):** `bash scripts/setup-ilupack-ref.sh` (committed, idempotent: download + extract + link smoke-test). Needs `gfortran` (`sudo apt install gfortran`; user's WSL sudo pw is in [build_system](build-and-verification.md#memory-build_system) context — installed 2026-05-26). The official `tu-bs.de` site is reachable from WSL curl but NOT the Claude WebFetch proxy (ECONNREFUSED). Package = `ilupackV2.4_GNU64_MUMPS.zip` (MUMPS matching ⇒ **no HSL needed**; the MC64 variant requires HSL which is not distributed). Link line (link with gfortran, not gcc):
`-L .../lib/GNU64 -lilupack -lmumps -lamd -lmetis -lsparspak -llapack -lblaslike -lblas`. C objects compiled `-D__UNDERSCORE__`. Self-contained: bundles its own BLAS/LAPACK/METIS/AMD/MUMPS.

**C API (from `simple_examples/dmaingnl.c`):** `D{GNL,SPD,SYM}AMGinit(&A,&param)` → set fields → `DGNLAMGfactor(&A,&PRE,&param)` → `DGNLAMGsolver(&A,&PRE,&param,rhs,sol)` → `DGNLAMGdelete`. Key params: **`param.condest` = the inverse-based pivot bound κ** (v4j-2's core; condest≈5 ⇒ behaves like AMG), `param.droptol`/`droptolS`, `param.ordering="metisn"`, `param.matching`, `param.elbow` (fill budget, updated to report achieved fill), `param.maxit`, `param.amg="amli"`. Iteration count returned in `param.ipar[25]`. Sample matrices ship in the dist (`lnsp3937.rua`, `bcsstk17.rsa`, `aft02.cua`, …, Harwell-Boeing, read via `Dreadmtc`).

**Bench rule:** compare at MATCHED true residual `‖b−Ax‖/‖b‖`, not the recurrence residual — see [feedback_iterative_bench_matched_true_residual](numerics-and-performance.md#memory-feedback_iterative_bench_matched_true_residual). Port faithfully (reference is the floor, not ceiling) — [feedback_reference_implementations_are_the_floor](workflow-and-correctness.md#memory-feedback_reference_implementations_are_the_floor), [project_amd_port_cs_amd_faithfully](project-history.md#memory-project_amd_port_cs_amd_faithfully).


<!-- end-memory:reference_ilupack_oracle_local_only -->

<a id="memory-reference_lapack_via_openblas"></a>
## reference_lapack_via_openblas

---
name: reference_lapack_via_openblas
description: How to link/call LAPACK in hesap vs-reference benches — it ships inside OpenBLAS via C_LAPACK (no Fortran)
metadata: 
  node_type: memory
  type: reference
  originSessionId: 021fa904-7b98-4a07-9c4e-4f1cb715ea28
---

LAPACK is the accuracy oracle + floor reference for hesap v3 (SVD + dense eig). It is NOT a separate dependency — it builds **inside OpenBLAS** from the f2c-translated C sources, enabled in the gated block of root `CMakeLists.txt` (`BUILD_WITHOUT_LAPACK=OFF` + `C_LAPACK=ON` + `NOFORTRAN=ON` + `NO_LAPACKE=OFF`, all behind `CRD_BUILD_HESAP_VS_REFERENCE` which defaults OFF). No Fortran toolchain needed.

**Validated 2026-05-21** under MSVC in `build/win-vs-ref` (the dedicated manual dir, NOT a DoD preset → no cache pollution): `openblas_static` built clean (6839 steps), `openblas.lib` = 63 MB, 102 LAPACK symbol hits (`dsyevr`/`dgesdd`/`dgeev`/`dgebrd`), `lapacke.h` present.

**To call LAPACK from a vs-reference bench:**
- Link the existing OpenBLAS target (the lib already contains LAPACK + LAPACKE): `openblas.lib` at `_deps/openblas-build/lib/Release/openblas.lib`.
- Include `_deps/openblas-src/lapack-netlib/LAPACKE/include/lapacke.h` for the LAPACKE C interface (`LAPACKE_dsyevr`, `LAPACKE_dgesdd`, `LAPACKE_dgeev`, …) — handles row/col-major + workspace queries. Or call the raw Fortran-mangled names (`dsyevr_`, column-major, manual workspace) directly via `extern "C"`.
- Reference source for the study pass: `_deps/openblas-src/lapack-netlib/SRC/*.c` (f2c'd) + the netlib `*.f`.

**Caveat (same as BLAS):** OpenBLAS-on-MSVC rides the generic kernel path, so LAPACK is an **accuracy oracle on Windows** and a **fair-speed bench only on Linux CI**. Use it to assert our eigenvalues/singular-values/vectors match LAPACK within tolerance on every gate matrix; use Eigen as the primary head-to-head speed reference. See [project_hesap_v3_max_ambition_gate](project-history.md#memory-project_hesap_v3_max_ambition_gate) and [feedback_gated_vs_reference_benches_need_flag_to_validate](numerics-and-performance.md#memory-feedback_gated_vs_reference_benches_need_flag_to_validate).


<!-- end-memory:reference_lapack_via_openblas -->

