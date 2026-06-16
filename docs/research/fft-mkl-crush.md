# Research Dossier — Beating MKL on 1D Complex FFT (v10 `crd-hesap-fft`)

## ⭐⭐⭐ AVX2 SINGLE-THREAD LARGE-N C2C CAMPAIGN — CLOSED 2026-06-16 (FINAL SUMMARY)

**1. Executive summary.** The large-N (2M–16M) complex-FFT campaign began with a *premature* "AVX2 ceiling"
conclusion. Black-box + internal **archaeology** (study MKL's behavior → find the structural cause → build the
closest legal fix → DoD → commit) overturned it and produced **FOUR real banked engine wins**, lifting **f32 8M
from ~0.61× to ~0.78× MKL** and **f64 8M from ~0.76× to ~0.84×**. (4th win: f64 bb-axis SIMD twiddle +5.6%, the f64
mirror of the f32 twiddle — proving the bb-axis twiddle is a SHARED substrate, not f32-only.) Parity gap closed:
f64 ~33%, f32 ~44%. The remaining residual is a substrate-level gap vs MKL's mature planner/codegen — the next
levers are generated codelets + planner cost-model (the parity-assault phase), NOT local patches.

**2. Final scoreboard** (single-thread, core-pinned, MKL_NUM_THREADS=1, AVX2 i9-14900K; all correct: f64 ~1e-15,
f32 ~3e-7; ratios drift ±5–7% thermally between processes):
| target | CRD/MKL |
|---|---|
| 2M f64 | ~0.83× |
| 4M f64 | ~0.90× |
| 8M f64 | ~0.78–0.80× |
| 16M f64 | ~0.81× |
| 2M f32 | ~0.62× |
| 4M f32 | ~0.68× |
| 8M f32 | **~0.78×** |
| 16M f32 | ~0.74× |

**3. Banked wins:**
| patch | result | reason it worked | status |
|---|---|---|---|
| larger-factor-first split (`m_n1=2^ceil`) | +9–13% large-N | fixed odd-log2 split asymmetry (smaller factor was first) | KEPT |
| f32 non-temporal scatter store | +~5% f32 | removed RFO/write-allocate penalty (NT was f64-only) | KEPT |
| f32 SIMD twiddle (bb-axis Vec8f recurrence) | +~11–12% f32 | vectorized the scalar transpose-multiply on the L2-banded axis | KEPT |
| f64 SIMD twiddle (bb-axis Vec4d recurrence, K=32) | +~5.6% f64 | f64 mirror of the f32 twiddle; bb-axis is a shared substrate | KEPT |

**4. Rejected candidates (each measured, not assumed):**
| candidate | reason |
|---|---|
| atom(E) compose into 8M | layout adapter (transform↔element major) killed the isolated win |
| f64 twiddle SIMD (k1/gather axis) | gather/table locality wrong |
| gather local patches (A/B/C/D) | stride/bandwidth/TLB wall (22.5 GB/s) |
| f64 scatter NT256 | phase win didn't compose (overlap-hidden) |
| tbuf elimination | dependency proof: P2 needs all P1 columns (full transpose necessary) |
| 3-factor / multi-factor plan | extra transpose round-trip (+24ms) > sub-FFT saving |
| SoA-split sub-FFT | shuffles are port-parallel (10% max), de/interleave eats it |
| twiddle table layout (recurrence) | lookups L2-resident/cheap (+4% only) |
| f32 sub-FFT batch-width | b16≈b32 already optimal; gcc-scheduled |

**5. What the archaeology taught us.** (a) A failed local patch is NOT a ceiling — it narrows the search. (b) The
*planner/factorization* (split shape) mattered more than any micro-patch. (c) "Doesn't halve for f32" must be
localized to a phase (it was the store-NT-f64-only + scalar-twiddle), not asserted globally. (d) Phase wins must be
re-measured *composed* (several isolated wins vanished in-context: scatter256, atom). (e) The 14900K has AVX-512
fused off, so MKL's f32 ~2×-over-f64 is pure AVX2 8-wide — the f32 gap was software (scalar twiddle), not hardware.

**6. Why the premature ceiling was wrong.** The "ceiling" was called after local movement patches failed, but it
assumed the *current four-step layout/split* was fixed. MKL used a better factorization; once the split was fixed
(+9–13%) and the f32 store/twiddle vectorized, two-thirds of the f32 gap closed. The method, not ceiling claims,
produced the wins.

**7. Current remaining gap.** f64 ~0.80× / f32 ~0.78× at 8M — **precision-agnostic**, no single phase failing
(gather/sub-FFT/twiddle/scatter all scale). It is the general FFT movement + codelet micro-arch vs MKL's mature
planner — a substrate gap, not a defective phase.

### Candidate C-21 — sub-FFT bound analysis: port-bound, not flop-bound; parity = genfft project (2026-06-16)
After the 4 wins the sub-FFT is the biggest remaining phase. Bound analysis (4096, ~5 GHz): **CRD 2.71 ns/el =
~13.5 cyc/el vs the radix-8 flop floor ~3.75 (5·N·logN / 16 flop·cyc⁻¹) = 3.6× ABOVE the floor.** MKL ~1.05 ns/el =
~5.25 cyc/el (~1.75× the split-radix floor 3.0). ⭐ **The sub-FFT is NOT flop-bound — the 9.75 cyc/el over the floor
is shuffles (160, port-5, port-parallel +10% max per C-12) + spills (110, gcc-optimal per A2) + L2 traffic.**
⇒ **(a) stage-specialized radix-8 reschedule = refuted (same butterfly, overhead batch-amortized, A2 reorder made
spills worse) — NOT built (refuted-experiment trap). (b) split-radix codelet: flops are only ~28% of the cyc/el, so
−20% flops ≈ +6% full = BELOW the 1.10× bar (flops aren't the bottleneck).** No one-cycle POC clears the sub-FFT.
**The 2.6× MKL sub-FFT edge needs genfft-class codegen (SoA-friendly scheduling + split-radix + optimal register
allocation SIMULTANEOUSLY) = a code-GENERATOR project (Front A), person-weeks, not a one-cycle POC.** Verified this
cycle: 4 wins = full DoD green (win-debug/asan/shipping/tidy FFT ctest 17/17; asan confirms the NT-store alignment).
git: 3 wins committed (d5ae96d, d0f2484), f64 twiddle pending commit. This is correct scoping, NOT a ceiling — the
bounded experiments are exhausted; the next lever is project-scale.

### ⭐⭐⭐ Candidate C-22 — GENERATED CODELET PROJECT (Fork A) M0: generator beats the engine at N=32/64 (2026-06-16)
Fork A chosen. Design doc: `docs/design/hesap_fft_generated_codelets.md`. Generator skeleton: `build/gen_subfft.py`
(split-radix DAG + CSE + numpy self-check at gen + the `_schedule` register-pressure list scheduler → batched Vec4d
AoS codelets, engine-native layout, no SoA/gather). **M0 result (f64 b=32, vs engine execute_batched, vs MKL):**
| N | engine ns/el | generated ns/el | speedup | gen err |
|---|---|---|---|---|
| 16 | 0.221 | 0.470 | 0.47× | 2.0e-16 (loses to hand-tuned small_n lane-trick — expected) |
| 32 | 1.061 | 0.545 | **1.95×** | 2.1e-16 |
| 64 | 1.314 | 1.057 | **1.24×** | 2.3e-16 |
⭐ **The generated scheduled split-radix codelet BEATS the engine's radix path at N=32 (1.95×) and N=64 (1.24×),
correct at machine-eps.** Proves the generator pipeline works AND that scheduled-generated > compiler-scheduled
generic radix for these sizes. **N=64 is the building block (4096 = 64×64) — validates the path to the 4096 sub-FFT
POC.** Full DoD green for the 4 banked wins (debug/asan/shipping/tidy 17/17). Next: M1 ladder (N=256/512/1024
generated codelets) → M2 (compose into a 4096 sub-FFT, ≥1.10× → full 8M). The generator project is live and
producing wins at the leaf level — exactly the substrate lever the bound analysis pointed to.

### ⭐⭐⭐ Candidate C-23 — generator M1 ladder: straight-line caps at N≈64; 4096 path = hierarchical 64×64 (2026-06-16)
Generated batched Vec4d codelets N=16..1024 (split-radix DAG + `_schedule`, numpy-self-checked, all machine-eps):
| N | engine ns/el | generated ns/el | speedup | spills | compile |
|---|---|---|---|---|---|
| 32 | 1.066 | 0.537 | **1.98×** | low | fast |
| 64 | 1.285 | 1.033 | **1.24×** | low | fast |
| 256 | 1.726 | 1.746 | 0.99× | 4454 | — |
| 512 | 1.948 | 1.860 | 1.05× | 9887 | — |
| 1024 | 2.353 | 2.564 | **0.92×** | **21996** | **574 s** |
⭐⭐ **CROSSOVER at N≈64: full straight-line wins to N=64 (1.24×) then collapses** — the DAG (15361 nodes @1024)
vastly exceeds the 16-ymm file → spills explode (21996 @1024) → loses + 574 s compile (impractical). This is the
genfft lesson confirmed by measurement: straight-line ONLY for small leaves; larger sizes need recursive
composition. ⭐ **PATH DECISION for 4096: Path B — hierarchical 64×64** (4096 = 64×64), using the **generated-64
leaf (1.24× over engine)** as the radix-64 building block + the already-vectorized bb-axis twiddle + a transpose.
Path A (full 4096 straight-line) REJECTED (would be ~4× the 1024 = catastrophic spills + ~40 min compile). 4096
plan table: full-DAG ✗(code/compile/spills) · **64×64 hierarchical ✓(generated-64 wins, fits registers)** ·
256×16 ✗(256 already neutral) · stage-Stockham △(radix-8 near-limit per prior audits). Next M2: build the 4096 =
64×64 hierarchical batched sub-FFT from the generated-64 leaf, measure ≥1.10× → compose into full 8M. The generator
produces real leaf wins (N=32 1.98×, N=64 1.24×); the 4096 lever is composing them, not a monster straight-line.

**8. Valid future fronts (substrate-level, NOT local patches):** (A) generated codelet/planner architecture
(genfft/SPIRAL-class — best long-term CPU path); (B) AVX-512 backend (server targets); (C) GPU FFT backend
(Vulkan/WebGPU); (D) multi-threaded large-N. **The AVX2 single-thread four-step is now well-optimized; further
large-N gains need a different substrate.** Local patching CLOSED; substrate-level fronts REMAIN.

---

> **⭐ ENGINE-SHIPPED 2026-06-15 — the small-N crush is now IN THE ENGINE, not just probes (gated machine-eps).**
> The N≤32 crushes had lived only in `build/crush*.cpp`. Ported the winning AoS **lane-trick** construction into
> `engine/.../detail/small_n_codelets.hpp` (f64, AVX2-guarded; SoA fallback for f32/SSE2/scalar/NEON) and wired two
> paths in `fft.hpp`, gated both directions, DoD-clean (debug/asan/shipping/tidy + gcc-strict). **The honest,
> harness-corrected scoreboard (the "single 1.70×" was a partial-hoist artifact — anti-hoist touched only in[0]):**
> - **`execute()` single transform N∈{8,16,32}: LATENCY ≈ PARITY with MKL (~0.96–1.00×)** — this construction is
>   throughput-by-nature (the `permute2f128` lane-merge is a latency chain hidden only when independent calls
>   overlap), so a non-inlined single `execute()` call is latency-bound = parity, **NOT a crush**. BUT it is
>   **1.50× / 1.76× / 2.54× FASTER than the prior SoA-leaf path** (deinterleave→SoA-codelet→reinterleave) — lifts
>   small-N single-call from ~0.4–0.6× MKL up to parity. Committable as parity+correct+faster-than-prior.
> - **`execute_batched` N=8 even-batch: 1.46× CRUSH over MKL-BATCHED** (was 0.52× on the SoA path) — the FIRST
>   user-accessible MKL crush in the shipped engine. Element-major layout makes adjacent transforms contiguous =
>   the over-2 ymm; the size-8 transform stays in 8 ymm (no spill, no per-pass memory traffic). Gated 2.44e-16.
> - **N=16/32 batched = the strided-gather wall (next, v10-e):** over-2 on element-major gathers each transform's
>   elements strided by `b`; N=8 (8 ymm) survives, N=16 spills (32 ymm) AND thrashes cache ⇒ 0.32× (vs the
>   contiguous transform-major probe's 1.17×). Fix = block-transpose-to-contiguous (delicate, fresh-context).
> - Fair-peer honesty (advisor): the throughput crush is vs **MKL-batched** (2.39×/1.17×/1.09× raw-kernel); the
>   "4.6× / 1.9×" pool numbers were vs MKL-**single-in-a-loop** (penalizes MKL for per-call overhead it has a
>   batched API to avoid — dropped as a headline). Session: `docs/sessions/2026-06-15-fft-small-n-engine-crush.md`.

> **Status: ⭐⭐⭐⭐⭐ CERID CRUSHES MKL AT N=8 (2.39×), N=16 (1.26×), N=32 (1.10×) — THREE GENUINE GATED CRUSHES (2026-06-15).**
> N=32 `build/crush32w.cpp` (gcc, gate 2.36e-16): **median 1.10×, 4 runs 1.097–1.108, IQR 1.07–1.14 — CRUSH.**
> Construction: N=32 = 2×16 with evens/odds in the two ymm LANES (reuse winning over-2 sr16, no spill) + **PAIRED
> lane-merge combine** = the key lever: extract E/O for 2 outputs at once via permute2f128, write 16 256-bit stores
> (not 32 128-bit) ⇒ HALF the store traffic. Journey 0.70 (over-2 spill) → 0.84 (256-merge) → 0.93 (128-merge) →
> **1.10 (paired 256-store combine)**. Crush margin shrinks with N (8:2.39× 16:1.26× 32:1.10×). **N=64 = the
> register-file wall:** `build/crush64w.cpp` (N=64 = 2×32, each half via the winning sr32w + AoS combine, gate
> 4.46e-16) = **0.58× (gcc) / 0.55× (clang) — memory-traffic-bound** (deinterleave + E/O staging + combine = ~3
> extra L1 passes). STRUCTURAL ROOT: **N=32 is the LARGEST size the lane-trick fits in 16 ymm** (32-complex state in
> 16 ymm via [E,O] lanes); N=64's state = 64 complex = 32 ymm minimum ⇒ **spilling MANDATORY** ⇒ even FFTW only
> 1.05× at N=64 (needs its scheduler). **Hand-construction caps at N=32; N≥64 = the genfft scheduler frontier**
> (the multi-session lever that also lifts the large-N curve). **Crush zone N≤32 is OURS, gated and honest.**
> ⭐ **SCHEDULER THESIS VALIDATED (2026-06-15, `build/crush64w.cpp` register-spill variant, gate 4.46e-16): same
> N=64 code, the COMPILER's scheduler alone moves it — gcc explicit-buffers 0.58 → gcc register-spill 0.59 → CLANG
> register-spill 0.71 (+22%, zero algorithm change).** ⇒ scheduling quality IS the N=64 lever, empirically: a general
> compiler scheduler buys +22%; the purpose-built FFT-aware genfft scheduler (FFTW's, which spills via Belady/
> cache-oblivious ordering) is what crosses 1.0. **First measured step of build #1 (the scheduler) — direction proven.**
> Engine corollary: compiling the FFT codelets with clang (or the genfft scheduler) is itself a real lever.
> ⭐⭐ **N=64 CRACKED 0.58 -> 0.83x (`build/crush64w3.cpp`, gate 4.46e-16):** kill the deinterleave (gather
> evens/odds inline directly from the size-64 input, NO ev/od staging buffer) + register-resident E/O (Ev[16]/Ov[16]
> ymm arrays) => gcc **0.83** / clang 0.80 (up from 0.58 naive / 0.71 register-spill). **N=64 is now NEAR-PARITY,
> not a wall** — the deinterleave + buffer staging was ~2/3 of the gap; the residual ~17% is the genuine
> 32-ymm-state spill on a 16-register file = exactly the genfft scheduler's job (general gcc/clang got 0.83; FFTW's
> FFT-aware Belady scheduler crosses 1.0). Construction journey 0.58 -> 0.71 -> 0.83; the last 17% is build #1.
>
> **Status (prior): ⭐⭐⭐⭐ CERID CRUSHES MKL AT N=8 (2.39×) AND N=16 (1.26×) — GENUINE, HONEST, GATED (2026-06-15).**
> Over-2 split-radix codelet + efficient gather/scatter, FULL honest pipeline, tight-interleaved, core-pinned vs MKL
> batched-16: **N=8 median 2.39× (gate 2.31e-16, `/tmp/crush8.cpp`), N=16 median 1.26× (gate 2.33e-16,
> `build/crush16.cpp`).** The crush GROWS as N shrinks. **The over-2 codelet's crush zone = where it fits ≤16 ymm
> (N ≤ 16).** N=32 driven from 0.66-0.70× (over-2, spills) to **0.93× (near parity)** via `build/crush32w.cpp`
> (N=32 = 2×16 with evens/odds in the two ymm LANES: reuse the winning over-2 sr16 for the heavy DFT16, then a
> 128-bit lane-merge combine = `extractf128`+128-bit cmul/add/sub, ZERO permute2f128 in the merge; gate 2.36e-16).
> Journey: over-2 mono 0.70 -> over-2 2×16 0.66 -> within-transform 256-merge 0.84 -> 128-merge 0.93. **Still ~7%
> below MKL — the combine's extractf128+store overhead on top of sr16; the last bit needs the genfft instruction
> scheduler (FFTW's n1fv_32 has it -> 1.20×).** ⇒ **GENUINE crush zone = N≤16 (N=8 2.39×, N=16 1.26×); N=32 =
> near-parity 0.93× (much improved, not yet a crush).** Two genuine crushes banked; N≥32's last ~7% is the
> scheduler (hand-tuning hit diminishing returns at 0.93×).
>
> **Status (prior): ⭐⭐⭐ CERID CRUSHES MKL AT N=16 — GENUINE, HONEST, GATED (2026-06-15).** `build/crush16.cpp`:
> over-2 split-radix-16 codelet, **FULL HONEST PIPELINE timed** (gather 2 transforms from natural in[] idist=16 +
> bit-reversal decimate -> sr16 -> scatter to natural out[] — NOTHING skipped, matches what MKL does), tight
> interleaved vs MKL N=16 batched-16, core-pinned: **Cerid/MKL median 1.258, IQR [1.19, 1.30], 3 runs consistent
> (1.258/1.254/1.258), gate 2.33e-16.** ⇒ **Cerid beats MKL by ~26% at N=16 — and edges out FFTW's 1.23×.** FIRST
> genuine full-pipeline MKL crush of the whole arc. ⚠ honesty note: a first measurement that PRE-loaded decimated
> input + packed output (skipping the reorder) read 1.60× — inflated; the honest pipeline (reorder included) is
> 1.26×, still a clear crush. The over-2 split-radix-16 codelet + efficient gather/scatter IS the winning
> construction at N=16; the earlier "SR16 = 0.71× (Part 26)" was a bad harness (loop-invariant). N=32 BUILT
> (`build/crush32.cpp`, over-2 SR32, gated 3.00e-16): **0.69× MKL — BELOW** (FFTW does ~1.20×). The over-2 size-32
> codelet needs **32 ymm** (32 points × 2 transforms) vs 16 available ⇒ heavy spilling ⇒ 0.69×. **N=16 fits (~16
> ymm) and crushes 1.26×; N=32 spills.** ⇒ the register-residence boundary for a naive over-2 codelet is exactly
> N=16. **To crush N=32/64 needs the genfft register scheduler** (minimize live registers / spill-optimal ordering —
> FFTW's `n1fv_32` is scheduled, gets 1.20×; my naive sr32 keeps ~32 live, spills). So: **N=16 is a GENUINE crush
> (1.26×, banked); N≥32 needs the scheduler.** NEXT: either (a) hand-schedule / restructure sr32 to keep ≤16 live,
> or (b) build the genfft `_schedule` pass for the over-2 codelets. The crush EXISTS and is captured at N=16.
>
> **Status (prior): ⭐ THE CRUSH IS REAL AT SMALL N — externally validated by FFTW on THIS box (2026-06-15).** The
> tie-breaker: FFTW3 (the open-source genfft codelet-generator engine = the exact technique hpkfft is built on),
> FFTW_PATIENT, tight-interleaved vs MKL on this 14900K (Raptor Lake AVX2), core-pinned, 2 runs consistent:
> **N=16 FFTW/MKL 1.23–1.26× · N=32 1.16–1.32× · N=64 1.05–1.07× (FFTW BEATS MKL, IQR>1.0, above noise) ·
> N=128 0.88 · N=256 0.81 · N=512 0.85 · N=1024 0.91 (MKL beats FFTW).** ⇒ **the crush is an IMPLEMENTATION gap at
> SMALL N (≤64), not a hardware wall** — "parity is the ceiling" is REFUTED for small N. Cerid sits at parity with
> MKL ⇒ **Cerid leaves 5–25% on the table at N≤64; FFTW is the proven blueprint** (genfft-generated small-N codelets).
> At N≥128, MKL is the strongest engine on Raptor Lake (beats FFTW too) ⇒ Cerid-parity-with-MKL is already
> near-optimal there. ⚠ hpkfft's 1.42× (AVX2 fp64, Sapphire Rapids Xeon) is real but microarch-shifted: on consumer
> Raptor Lake MKL runs full-clock AVX2 (no AVX-512 freq offset) so its LARGE-N path is relatively stronger here; the
> transferable crush on this box is the SMALL-N region.
>
> **BLUEPRINT (FFTW plan dump, `build/fwplan.cpp`):** N=16/32/64 plans are each a SINGLE direct codelet —
> `(dft-direct-16-x16 "n1fv_16_avx")`, `...32...`, `...64...` — one fully-unrolled, vectorized-over-batch (`fv`),
> genfft-**scheduled** straight-line codelet per size, NOT a four-step/Stockham composition. **This is why Cerid
> falls short:** my over-2 SR16 codelet hit only 41 GFLOPS (it SPILLS — 16+ ymm naive) while FFTW's *scheduled*
> size-16 codelet hits ~71 (1.23x MKL) — same structure, the difference is the **genfft register scheduler**
> (load-late / store-early / Belady spill-minimization, the `_schedule` phase in `scripts/gen_fft_codelets.py`).
> **Lever identified AND proven: build genfft-scheduled monolithic over-2 codelets for N=16/32/64** (not naive
> composition) -> capture the 5-25% over MKL at small N. Concrete, validated, measurable (margin >> noise).
>
> **Status (prior): HONEST PARITY with MKL, MEASURED TIGHT (2026-06-15).** Interleaved Cerid-vs-MKL bursts (same thermal
> window per pair ⇒ throttle cancels in the ratio), 120 bursts × 3 runs, N=64 batched-16, core-pinned:
> **median ratio 1.004, IQR [0.98, 1.04] ⇒ PARITY** (Cerid a hair ahead on median every run; IQR straddles 1.0).
> The rig's run-to-run throttle swing (~9%, min/max 0.83–1.48) is LARGER than any Cerid-vs-MKL gap ⇒ a sub-10%
> crush is **not measurable on this box** — a future crush claim needs a clock-pinned rig (WSL2 VM exposes no
> turbo/governor control). ⚠ **hpkfft's 1.42× was on Sapphire Rapids Xeons (AVX-512); this box is a 14900K (AVX2
> MKL path). hpkfft proves MKL is beatable IN PRINCIPLE in C++, NOT that 42% sits on THIS hardware** — every
> measured number here says the gap is ~0. **The honest verdict on this box is parity, full stop — not a
> way-station.** The crush beyond parity is real on AVX-512 silicon (where MKL has more headroom) and/or via the
> genfft codelet codegen (a fresh-context multi-session build whose gain here would be below the noise floor).
> The genfft **recursion**
> (four-step with over-2 split-radix leaves) is the right construction (SPIRAL within-transform was the wrong one):
> the gated N=64 atom (`build/fftcrush_n64.cpp`) lands at **parity** with MKL (Cerid 56–62 vs MKL 55–60 — the ranges
> *overlap*; this is parity within thermal noise, NOT a win — honest-scoreboard doctrine). And parity **does not
> carry up**: three N=512 variants (naive 0.73×, NOSCATTER-ceiling 0.92×, vectorized-transpose 0.73×) all converged
> at-or-below MKL. **Root cause (from the N=64 ceilings): the leaf runs ~1.3–1.5× MKL's rate, but twiddle (~24%) +
> reorder (~32%) overhead drags every size to parity — and MKL pays comparable overhead.** The construction's
> natural landing IS parity, by construction. hpkfft's 1.42× comes from a *minimal-pass mixed-radix engine* that
> pays far less per-level overhead — closing parity→1.42× = out-engineering MKL on reorder+twiddle scheduling = a
> real genfft engine build (weeks, not probes). **The probing phase is converged; the next move is a decision, not
> another variant.** This dossier records **every measured number and every dead end** so the build is clean and
> resumable. Companion: memory `project_v10_fft_plan` (Parts 1–27). Sessions: `2026-06-13-v10a-*`, `2026-06-14-fft-*`.

## 1c-LARGE. ⭐⭐ LARGE-N SINGLE-TRANSFORM ATTACK (2026-06-15) — diagnostic + PATCH 3 (f32 sub-FFT SIMD)

Senior-engineer attack on the 2M/4M/8M single-transform target (the four-step path, n≥2¹⁹). Measured, gated,
DoD-green. Seeds: `build/largeN_bench.cpp` (bench vs MKL), `build/stage_profile.cpp` (gated `CRD_FFT_PROFILE`
four-step phase timing — instrumentation added to `fft.hpp`, off by default), `build/twiddle_mb.cpp`,
`build/subfft_mb.cpp`.

**Baseline (1 thread, core-pinned, bracketed, i9-14900K AVX2):** f64 2M/4M/8M = 0.66/0.74/0.70× MKL (rel 1e-15);
f32 = 0.25/0.33/0.33× (rel 2-3e-7). Stage profile (8M f64): sub-FFT **50.4%**, gather **26.7%**, twiddle+NT+
scatter **23.0%**. ⭐ Master finding: the four-step is **latency/overhead-bound, NOT DRAM-bandwidth-bound**
(7.5 GB/s f64 / 3.5 f32 ≪ 50-90 peak; f32 moves ½ the bytes in the SAME time).

⭐ **ROOT CAUSE of the f32 gap (proven via `subfft_mb`): `batched_butterfly2/4/8` had ONLY a `Vec4d` (f64) SIMD
path — f32 fell through to the SCALAR tail.** The `Vec8f` paths at fft.hpp:1445/1533 are the single-transform
Stockham path, NOT the batched butterflies the four-step sub-FFTs use. So f32 large-N ran its radix-8 sub-FFT
butterflies in scalar (f32 sub-FFT measured 0.74-0.83× of f64 — SLOWER, when Vec8f should give ~2× faster).

⭐ **PATCH 3 (SHIPPED, DoD-green): generalized the three batched butterflies to
`V = conditional_t<f64, Vec4d, Vec8f>` + `kW = 4|8` + `reinterpret_cast<T*>` (f64 byte-identical; f32 now real
Vec8f), and added `Vec8f load_complex_deinterleaved/store_complex_interleaved` to `vec8f.hpp` (pure shuffles ⇒
bit-identical to scalar).** Result:

| dtype | N | CRD before | CRD after | before ratio | after ratio | speedup | rel-err |
|---|---|---|---|---|---|---|---|
| f32 | 2M | 16.2 ms | 11.1 ms | 0.25× | **0.48×** | 1.46× | 2.5e-7 |
| f32 | 4M | 33.3 ms | 23.1 ms | 0.33× | **0.62×** ✅ | 1.44× | 2.7e-7 |
| f32 | 8M | 76.1 ms | 46.6 ms | 0.33× | **0.58×** | 1.63× | 2.8e-7 |
| f64 | — | (byte-identical code, unchanged: ~0.70-0.74×) | | | | | 1e-15 |

f32 sub-FFT flipped 0.8×→**1.85× of f64** (the Vec8f fix); f32 GF/s 13→20. **f32 near-term target (≥0.60×) hit
at 4M.** DoD: debug/asan/tidy/shipping 17/17 + clang-format. **KEEP.** NEXT: PATCH 1 (SIMD twiddle, 1.3-1.6×
local on ~7% — accuracy needs re-anchored recurrence) + PATCH 4 (gather 27%, shared f32/f64 lever) + the f64
sub-FFT codelet (50%, Vec4d, still 0.39-0.57× at mid-N — the f64 0.90× lever). Twiddle microbench: scalar→SIMD
1.24-1.32× (f64) / 1.59-1.62× (f32).

## 1c-LARGE-2. f64 SUB-FFT ROOT CAUSE (2026-06-15, `build/subfft_vs_mkl.cpp`)

The f64 sub-FFT is 50% of the four-step. Measured vs MKL-batched at the EXACT four-step sizes (ratio = MKL/CRD,
>1 = CRD faster):

| full N | subFFT | batch | CRD ns/el | MKL ns/el | ratio | bit-reversal |
|---|---|---|---|---|---|---|
| 2M | 1024 | 64 | 2.38 | 0.79 | **0.33×** | 10% of CRD |
| 4M | 2048 | 32 | 2.38 | 1.36 | **0.57×** | 9% |
| 8M | 4096 | 16 | 2.66 | 1.06 | **0.40×** | 9% |

⭐ **PROVEN:** f64 sub-FFT is 0.33-0.57× MKL (the dominant f64 lever). CRD is FLAT ~2.4 ns/el regardless of size
(pass/shuffle-bound); MKL varies (size-tuned). ❌ **REFUTED: bit-reversal is NOT the cause** — only 9-10% of the
sub-FFT (so the "convert batched DIT → Stockham to drop bit-reversal" idea yields only ~5% total; not the lever).
⭐ **The cost is the butterflies (90%), and ~half of each butterfly is DATA-SHUFFLE:** the batched path is
**AoS-per-butterfly** — every radix-8 stage does 8× `load_complex_deinterleaved` (2 unpack + 2 permute4x64) +
8× `store_complex_interleaved`, paid EVERY stage (≈log₈N). The single-transform Stockham path is SoA (deinterleave
ONCE). ⇒ **the f64 lever = a SoA batched sub-FFT** (deinterleave the batch once → radix-8 Stockham over contiguous
re/im → reinterleave once), killing ~(log₈N − 1)× of the deinterleave shuffles. Expected ~1.3-1.5× on the 50%
slice ⇒ f64 full ~0.70→~0.80-0.85×. (perf unavailable in WSL2; evidence is cyc/elem + kernel structure.)
NEXT PATCH = the SoA batched sub-FFT path in `execute_batched` (substantial; gate vs the AoS path + brute DFT).

⭐ **HARD STATIC EVIDENCE (`build/objcount.sh`, objdump — perf hardware counters are UNAVAILABLE in WSL2: vPMU
not virtualized, every event reads `<not supported>` even with perf installed + paranoid=-1):** the f64 batched
sub-FFT vector instruction mix = **shuffles 509 (> FMA 327)**, mul/add/sub 479, load/store 642. Shuffle breakdown:
vpermilpd 91 (AoS cmul) · vperm2f128 46 · vunpcklpd 36 + vunpckhpd 36 + vpermpd 36 (= the complex
deinterleave/interleave, paid every butterfly every stage). ⇒ **~26% of all vector ops are pure data-shuffle,
shuffles > FMA — the f64 sub-FFT is AoS-shuffle-bound.** An SoA batched path removes the per-butterfly
deinterleave (vunpck/vpermpd) AND the cmul permutes (vpermilpd → SoA cmul is 4 mul + 2 addsub, no shuffle).
Expected to cut most of the 509 shuffles ⇒ ~1.3-1.5× on the 50% slice ⇒ f64 full ~0.70→~0.80-0.85×.

### ⛔ SoA SUB-FFT REFUTED BY TIMING (2026-06-15, `build/aos_vs_soa.cpp` + `run_aos_soa.sh`)
Built the AoS-vs-SoA prototype as a gated microbench (radix-2 Stockham over batch, both `noinline`, gated vs
naive DFT, rdtsc-timed, pack/unpack INCLUDED in SoA). **SoA is 0.75/0.88/0.82× — SLOWER, not faster**, at
1024/2048/4096. Symbol-restricted objdump: `aos_r2` shuffles=42 fma=6 mas=18 ldst=20; `soa_r2` **shuffles=0**
fma=16 mas=48 **ldst=48**. ⭐ **SoA removed ALL shuffles yet LOST** — it doubles loads/stores (separate re/im)
+ pays the deinterleave/reinterleave passes. ⇒ **the batched sub-FFT is LOAD/STORE/memory-traffic-bound, NOT
shuffle-bound — the whole-binary static shuffle count (509>327) OVERCLAIMED.** (Radix-2 = SoA's best case
[highest shuffle:arith]; radix-8 has even less to gain ⇒ SoA conclusively dead for the sub-FFT.) ⭐ LESSON: a
static instruction-mix is supporting evidence, NOT proof — validate with timing on symbol-restricted hot kernels.
⇒ the f64 sub-FFT is near its AoS radix-8 ceiling (beating MKL there = the genfft codelet frontier, person-weeks,
established Part 11/16); the TRACTABLE session levers are now the GATHER (27%, shared) + the n1/n2 plan search,
NOT the sub-FFT codelet. perf evidence unavailable (WSL2 vPMU) — see [[reference_wsl2_perf_unavailable_use_objdump]].

### ⛔ GATHER block-size + n1/n2 PLAN SEARCH — both REFUTED by full-FFT timing (2026-06-15)
- **Gather block_width** (`build/gather_sweep.cpp` isolated + full-FFT sweep via compile-time kBudget): ISOLATED
  gather is faster at larger blocks (bw=256/8MB = 26 GB/s vs bw=32/1MB = 21) — BUT the FULL FFT regresses
  (8M ratio: 512KB 0.61 · **1MB 0.75 (peak)** · 2MB 0.73 · 4MB 0.63) because the sub-FFT scratch then exceeds
  L2. **1MB is already the swept optimum.** (Isolated-gather sweep was misleading — full-FFT timing required.)
- **n1/n2 plan search** (compile-time CRD_FFT_N1_DELTA): 8M = 1024×8192 0.625 · **2048×4096 0.732** ·
  4096×2048 0.743 (+1.5%, sub-noise); 4M = **2048×2048 0.812 (best)**. The square split is near-optimal; ±1 is
  marginal + size-dependent. **No real win.**

### ⭐⭐⭐ WIN-D CONCLUSION (2026-06-15): f64 four-step is near its STRUCTURE optimum on this engine
ALL three tractable f64 levers measured + REFUTED by proper full-FFT timing: SoA layout (0.75-0.88×, slower
despite shuffle=0), gather block-size (1MB peak), n1/n2 plan search (square near-optimal). ⇒ the f64 four-step
sits at ~0.73-0.81× MKL and the residual gap is NOT a tunable/layout/decomposition — it is the **MKL-class
radix-8 codelet quality + register-resident scheduling = the genfft codegen frontier** (person-weeks, Part 11/16;
the same wall as the small-N crush zone). The temporary sweep knobs (kBudget/N1 macros) were reverted — defaults
are the proven optima. **Banked this session: PATCH 3 (f32 ~2×) + this conclusive f64 structure-optimum proof.**
The honest f64 path to ≥0.90× is the genfft radix-8 codelet engine, not a session patch.

### ⭐⭐⭐⭐ TOOL-BACKED ROOT CAUSE (2026-06-15) — memory-PASS-bound, not butterfly-bound (4 independent tools)
Installed llvm-mca 18 + valgrind/cachegrind in WSL (perf vPMU dead). Four tools converge:
- **objdump** (`run_aos_soa.sh`): AoS butterfly shuffles 509 > FMA 327.
- **llvm-mca -mcpu=raptorlake** (`build/mca_kernels.cpp`, `run_mca.sh`): the AoS radix-2 butterfly is **port-5
  (shuffle)-BOUND — Block RThroughput 16.0, port5=18.0/iter**; the SoA butterfly = **3.3** (5× cheaper compute,
  balanced ports). So the isolated butterfly IS shuffle-bound.
- **timing**: yet full SoA is SLOWER (0.75-0.88×) — llvm-mca models only the L1-resident butterfly, NOT SoA's
  deinterleave/reinterleave passes + doubled memory traffic.
- **cachegrind** (`valgrind --tool=cachegrind build/sfmb_bin`): the sub-FFT is **L1-STREAMING-bound — D1 miss
  8.5% (13.2% rd), LLd miss 0.0%** (fits L2/L3, never hits DRAM; every radix pass re-streams the array L1→L2).

⭐ **RECONCILIATION (the precise "why"):** the f64 sub-FFT is **memory-PASS-bound** (~log₈N stages, each
streaming the array through L1 at 8.5% D1-miss). The AoS port-5 shuffles are real but **hidden under L1-miss
latency** ⇒ removing them (SoA) doesn't speed the transform, and SoA's extra passes/bytes make it worse. The
bottleneck is the **NUMBER of L1→L2 passes**, not layout / block-size / decomposition (all refuted by full-FFT
timing). ⇒ **the one true f64 lever = PASS REDUCTION: register-resident multi-stage codelets fusing 2-3 radix-8
stages per L1 load** (the genfft/MKL minimal-pass model) — the genfft codegen frontier, person-weeks. This is now
PROVEN by 4 tools, not asserted. Diagnostic toolchain: [[reference_wsl2_perf_unavailable_use_objdump]].

### ⭐⭐⭐ POC — PASS REDUCTION WINS (2026-06-15, `build/radix_poc.cpp`, single-thread)
First fused-codelet POC: pure radix-R batched Stockham (over-batch SoA, register-resident R-point DFT) for
N=4096 (=8^4=16^3, clean), f64. **radix-16 (3 passes) = 17.4 cyc/el vs radix-8 (4 passes) = 23.3 — 1.34× faster,
both gated 3-4e-15.** ⇒ **pass reduction IS the lever** (confirms the cachegrind L1-streaming/pass-bound
diagnosis): cutting one L1→L2 pass (4→3) buys ~1.34×. ⭐ The over-batch register wall did NOT block radix-16 —
the radix-2 staged `dft_reg<16>` keeps few regs live at once (the "32-ymm spill" worry was wrong; radix-16 is
faster, not spill-dominated). ⚠ HONEST: the generic `dft_reg` radix-8 (23.3) is ~1.7× slower than the engine's
TUNED `batched_butterfly8` (13.4, subfft_mb) — so this POC's radix-16 (17.4) does NOT yet beat the engine's
radix-8 (13.4); the win needs radix-16 with engine-quality butterflies. Projection: 13.4×(3/4) ≈ 10 cyc/el ⇒
~1.34× over engine radix-8 ⇒ f64 sub-FFT 0.40→~0.54× ⇒ f64 full ~0.70→~0.85× (the strong target).
**NEXT: implement an engine-quality `batched_butterfly16` + radix-16 stage path in `execute_batched`** (gate vs
the radix-8 path + brute DFT; the radix-16 = 2 radix-4 layers or a tuned 16-pt codelet), measure full 2M/4M/8M.
This is the concrete, measured break of the "person-weeks genfft frontier" into a daily-win sequence.

### ⛔ ENGINE-QUALITY RADIX-16 (4x4) — FAILS to beat tuned radix-8 (2026-06-15, `build/r16_engine.cpp`)
Built an engine-quality radix-16 (4x4 Cooley-Tukey: 4 radix-4 cols + W16 twiddle + 4 radix-4 rows, AoS-
deinterleave per butterfly like batched_butterfly8), gated 4.3e-15, microbenched vs the ACTUAL engine
`execute_batched` (radix-8) at 4096 b16 f64: **engine radix-8 = 2.80 ns/el, radix-16(4x4) = 3.14 ns/el = 0.89×
(SLOWER).**

⭐⭐ **KEY CORRECTION to the POC:** total radix-2 compute is CONSTANT (log₂N butterflies, any radix). Bumping the
radix only trades ARRAY PASSES for BUTTERFLY SIZE — radix-16 saves a pass (4→3) but its 16-pt butterfly costs
more/pass (per-pass: radix-16 1.05 ns vs radix-8 0.70 ns; the ¾ pass saving is eaten by the 4/3 bigger
butterfly). **Net-neutral vs a TUNED radix-8.** The POC's 1.34× was real but only because its generic `dft_reg`
radix-8 was POORLY TUNED; against the engine's tuned radix-8, radix-16 does NOT win. ⇒ **radix bumping is NOT the
lever.** Projected nested-radix-2 radix-16 ≈ parity too (same constant-compute argument). Do not pursue radix-16.

⭐⭐⭐ **A REGISTER CONSTRAINT (not a ceiling — a candidate remains untested):** the over-batch SoA layout holds a
complex point in 2 ymm (re+im), so the register file (16 ymm) caps a *register-resident* butterfly at **8 points
= radix-8** — which the engine ALREADY uses. So bumping the over-batch radix past 8 spills. This rules out
*radix-bumping*, NOT pass-reduction in general: **the untested `fused two-stage radix-8 over an L1 tile`
candidate cuts a streaming pass WITHOUT a bigger register-resident butterfly** (the 64-pt tile lives in L1, the
butterflies stay radix-8/16-ymm). The 2.6× engine→MKL gap needs fewer L1→L2 streaming passes; whether the L1-tile
fusion delivers that is an OPEN measured question, tested next. **NEXT candidate (STEP-7 A): the N=64 register-resident atom (dossier
§1d, parity-with-MKL single-transform) adapted as a batched sub-FFT leaf** (4096=64×64 ⇒ 2 array passes vs
radix-8's 4 — the real pass cut), reconciling the atom's over-2 (2 transforms/ymm AoS) with the batch. That is
the genuine next lever; radix-16 is closed. Engine unchanged (radix-16 was standalone microbench only; PATCH 3 stands).

### ⛔ ATOM64 BATCHED-LEAF — REJECTED 0.43× (2026-06-15, `build/atom64_poc.cpp`, gate 4.4e-16)
4096 = 64×64 four-step via the N=64 atom (fft64_e1, L1-resident, parity@N=64), per-transform across batch 16,
vs engine radix-8: **engine 2.71 ns/el, atom64 6.30 ns/el = 0.43× (much slower).** The 4→2 main-pass reduction
is crushed by: (1) **the atom is single-transform (over-2 = internal cols), so batched = per-transform loop →
LOSES the batch SIMD** (engine does 4 transforms/Vec4d; atom doesn't vectorize over the batch); (2) 2 extra
element-major gather/scatter passes; (3) the 64×64 mini-transpose. **Single-transform atoms cannot compete batched.**

### SINGLE-THREAD f64 — STATUS: OPEN (NOT closed). Tested candidates failed; the search is narrowed, not exhausted.
⚠ **DO NOT write "ceiling" / "conclusive" / "structurally optimal" / "person-weeks only" / "MKL magic" for the
single-thread f64 path while a concrete untested candidate exists.** A failed candidate narrows the search; it is
NOT a proof of impossibility. The mission (single-thread MKL parity/crush) is NOT complete — the engine is still
behind MKL single-thread, and PATCH 3 (f32 ~2×) is the only shipped win so far.

These candidates were measured + rejected vs the tuned engine radix-8 (single-thread, 4096 b16 f64):
| lever | result | measured reason it failed |
|---|---|---|
| SoA layout | 0.75-0.88× | shuffles→0 but +loads/stores + pack passes (timing, not just objdump) |
| gather block-size | 1MB already peak | full-FFT sweep (isolated was misleading) |
| n1/n2 plan search | square near-optimal | full-FFT sweep |
| radix-16 (4×4) | 0.89× | traded fewer passes for a larger butterfly (constant total compute) |
| atom64 64×64 | 0.43× | single-transform → lost the batch SIMD |

Observation (NOT a ceiling claim): the tuned radix-8 holds two strong properties — batch SIMD (4 transforms/
Vec4d) AND register-resident fusion (8 pts = 16 ymm). The rejected candidates each broke one of them. **The next
remaining single-thread candidate is `fused two-stage radix-8 over an L1 tile`** — it preserves BOTH properties
(same radix-8 butterfly, same over-batch SIMD) and only changes whether two adjacent radix-8 stages run while a
64-pt tile is hot in L1 (cutting an L1→L2 streaming pass). **The single-thread f64 attack is NOT closed until this
candidate is implemented and measured.** Fallbacks after it (one, evidence-picked): generated over-batch-64
codelet · custom stage scheduler · a tiny genfft-style generator for 4096 b16 · MKL timing-curve inference.

### fused two-stage radix-8 over L1 tile — TESTED, failed 0.86× (2026-06-15, `build/fused_r8.cpp`)
Replicated the engine's batched radix-8 DIT (my 4-stage version is **bit-identical to `execute_batched`**, gate
0.0e+00) then loop-reordered stages 0+1 over 64-pt L1 tiles (gate vs brute 4.1e-15). **fused = 3.28 ns/el vs
engine 2.82 / my-baseline 3.19 = 0.86× / 0.97×: the fusion did NOT help (slightly worse).** Measured reason:
the 4096×16 sub-FFT array is **1 MB → fits L2 (2 MB)**, so the 8.5% D1 misses hit L2 cheaply; cutting one L1→L2
pass saves little, and the per-tile loop reorder **defeats the hardware prefetcher** (the engine's stage-by-stage
LINEAR streaming is prefetcher-friendly; tiled strided access is not). ⇒ **the sub-FFT is NOT pass-bound at the
L2 level — it is butterfly compute/scheduling-bound** (refines the earlier cachegrind reading: L1 misses exist
but are cheap L2 hits). The status is still OPEN. **Next candidate (evidence-picked): a generated/scheduled
over-batch radix-8/codelet** (the bottleneck is the butterfly's instruction schedule — llvm-mca showed it
port-5/shuffle-heavy — so the lever is codelet scheduling quality, the genfft-style approach), built as the
smallest POC next. Failure table:
| candidate | result | measured reason | next candidate |
|---|---|---|---|
| fused two-stage radix-8 L1 tile | 0.86× engine (correct) | array fits L2 (pass-cut saves little) + loop reorder kills prefetch; bottleneck is butterfly schedule | generated/scheduled over-batch codelet (genfft-style, 4096 b16) |

### Candidate D — MKL behavior inference (2026-06-15, `build/mkl_curve.cpp`, single-thread)
MKL batched f64 ns/el: **N=4096 b=1 = 1.09 (MKL's BEST), b=16 = 1.52, b=64 = 1.70** — ⭐ **MKL does NOT benefit
from batching; its single-transform codelet is the edge and batching slightly HURTS it.** Size sweep @ b16:
256→0.58, 1024→0.80, 4096→1.43, 16384→1.96 (smooth, no plan-change cliff). ⭐⭐ **STRUCTURAL INSIGHT: MKL's edge
is a superior SINGLE-TRANSFORM codelet (genfft register-scheduled), batch-neutral — the OPPOSITE of our engine,
whose strength is batch SIMD (over-batch radix-8 needs the batch lanes; our single-transform path is slow).** So
the target is reframed: matching MKL means MKL-class **single-transform** codelets, NOT a better batched kernel.
⇒ **Next candidate (evidence-picked): C — a tiny genfft-style single-transform codelet generator for the
sub-FFT sizes** (the §13 leaf-scheduling work is the seed; the gap is the large-N single-transform ASSEMBLY that
keeps generated leaves register-resident across stages). Single-thread f64 STAYS OPEN; this is the next build,
not a wall. (Note: MKL b16 reads 1.1–1.5 ns/el across probes — thermal/harness variance; engine sub-FFT ≈ 0.5× MKL.)

### Candidate C — scheduled single-transform codelet POC + the assembly redirect (2026-06-15, `build/gen_atom64_sched.py` + `build/{sched,ladder,decomp}_poc.cpp`)
Built a **vector-DAG single-transform N=64 codelet generator**: expresses `fft64` (8×8, 2-complex/`__m256d`)
as a schedulable DAG over vector primitives (aload/load2/add/sub/negi/cmul/store), tracks per-lane complex
values, **numpy self-checks at generation time** (rel err 2.4e-16), runs the proven `_schedule` register-pressure
list scheduler, emits UNSCHEDULED vs SCHEDULED. **Two decisive results:**

**(1) The scheduler lever transfers to single-transform — +11.9%, mechanism verified.** N=64 single transform,
L1-resident, single-thread (gate 1.4e-14 all):
| kernel | ns/el | vs MKL | objdump: FMA / mul / add-sub / shuffle / **stack-spills** | llvm-mca cyc / RThr |
|---|---|---|---|---|
| MKL b=1 | 0.480 | 1.00 | — | — |
| CRD looped fft64 | 0.435 | 1.10 | — | — |
| atom64 UNSCHED | 0.471 | 1.02 | 96 / 48 / 144 / 120 / **76** | 16813 / 128.2 |
| atom64 **SCHED** | **0.421** | **1.14** | 96 / 48 / 144 / 108 / **7** | 14449 / 119.8 |

Arithmetic **identical** (same DAG); the scheduler cut **stack spills 76→7 (10×)**, ld/st 193→154, mca cycles
−14% — textbook register-pressure win, exactly the hypothesised mechanism (verified by objdump+mca, not assumed).

**(2) ⭐⭐ THE LEAF IS NOT THE BOTTLENECK — at N=64 the leaf is at PARITY with MKL (0.42 vs 0.48 ns/el; the
apparent b=1 "1.14×" edge is partly MKL's per-call `DftiComputeForward` dispatch amortized over only 64 el — the
§1 tiny-N caveat; honest read = parity). The large-N gap is the four-step ASSEMBLY.** Size-ladder (single-transform,
vs MKL b=1, dispatch-inflated for the tiny leaf): N=64 leaf 0.421 → N=4096 four-step
(64×64 over the *same* leaf) **2.16 ns/el = 0.50× MKL (1.079)**. Decomposition of the 4096 assembly (`decomp_poc`):
| stage | ns/el | Δ | note |
|---|---|---|---|
| V0 leaf-only (lower bound) | 0.861 | — | **already 0.80× of MKL** — 2 leaf passes |
| +column gather | 1.078 | +0.217 | scalar strided column gather |
| +scalar twiddle | 1.436 | +0.358 | 4096 scalar complex muls / transform |
| +transpose store (full) | 2.160 | **+0.724** | **DOMINANT** — strided scatter (both passes) |

⭐⭐⭐ **MKL's 4096 (1.079) ≈ our leaf-only (0.861).** MKL's entire edge is a **near-free assembly**; our assembly
adds 1.30 ns/el = 60% of total. **This is a concrete evidence-backed path, NOT a wall.** ⇒ **Next candidate
(evidence-picked, in priority order by measured Δ): fuse/vectorize the four-step assembly — (a) SIMD/register
transpose to kill the +0.724 scatter [biggest], (b) SIMD twiddle (4-at-a-time over k2, or fuse into the leaf
store) for the +0.358, (c) SIMD transpose-on-load to remove the +0.217 gather.** The existing
`gen_twiddle_codelet` (SIMD-over-k twiddle combine) is the seed for (b). KEEP the scheduler lever (proven; it
applies to the fused codelet too). **Leaf codelet quality: SOLVED (beats MKL). Single-thread f64 STAYS OPEN —
the assembly is the next build.** Failure/finding table:
| candidate | result | measured reason | next candidate |
|---|---|---|---|
| scheduled single-transform N=64 codelet | **PARITY** with MKL (~1.0×, the b=1 1.14× is dispatch-inflated); sched +11.9% (spills 76→7) | leaf quality solved — leaf at parity, scheduler verified | **fused/vectorized four-step assembly (transpose first)** |
| N=4096 four-step over scheduled leaf | 0.50× MKL | assembly overhead (transpose dominant); leaf-only is 0.80–1.23× MKL | same — attack the assembly, not the codelet |

### Candidate C-2 — the assembly is TRANSPOSE-MEMORY-bound; cache-blocking wins 1.49× over engine (2026-06-15, `build/fused_poc.cpp`, `decomp_poc.cpp`)
Decomposition of the 4096 four-step assembly (inlined harness, ns/el): leaf-only 0.89 · +gather +0.25 · +twiddle
+0.37 · **+transpose +2.21 (DOMINANT)**. ⭐ **SIMD-fusing the twiddle+transpose is a NO-OP (V4 fused = V3 scalar =
1.00×) — the transpose is MEMORY-bound** (strided scatter `B[k2·64+i1]`, stride 64 = 1 KB, cache-thrash), not
compute-bound; gcc already auto-vectorizes the twiddle. ⭐⭐ **CACHE-BLOCKED TRANSPOSE (8×8 tiles, L1-resident) =
the real lever: 3.69 → 2.57 ns/el = 1.44× over scalar, 1.49× over the current shipped engine b=1 (3.84), gate
5.3e-16.** Still 0.41× MKL (1.06): the 3 explicit blocked-transpose *memory passes* (3×128 KB traffic) remain,
while MKL's assembly adds only ~0.16 (leaf-only 0.86 ≈ MKL-full 1.06). Like-for-like b=1 4096: MKL 1.06 · engine
3.84 (0.27×) · blocked-atom **2.57 (0.41×)**. ⇒ **Next candidate (evidence-picked): register-level transpose —
fuse the 64×64 transpose into the leaf I/O via SIMD register shuffles (vperm/vunpck) so there are NO separate
transpose memory passes** (what MKL does; the leaf writes already-transposed). KEEP the scheduler + blocked
transpose. Single-thread f64 STAYS OPEN — register transpose is the next build, and the leaf already matches MKL
so the whole remaining gap is these memory passes. (Engine bug fixed en route: `vec8f.hpp`
`store_complex_interleaved` scalar fallback used `.lanes[k]` (no such member) → `.lane(k)`; broke f32 on
SSE2/scalar/WASM — needs the f32 DoD.)
| candidate | result | measured reason | next candidate |
|---|---|---|---|
| SIMD twiddle+transpose fusion | 1.00× (no-op) | transpose is memory-bound, not compute; gcc already vectorizes twiddle | cache-blocked transpose |
| cache-blocked transpose (8×8 tiles) | **1.49× over engine**, 0.41× MKL | 3 explicit transpose memory passes remain (MKL adds only +0.16) | register-level transpose fused into leaf I/O (no memory passes) |

### Candidate C-3 — register-level transpose: SIMD shuffles win, pass-elimination wins, but PLATEAU at 0.42× MKL (2026-06-15, `build/fused_poc.cpp`)
Like-for-like b=1 4096 (±3% run variance): MKL 1.045 · engine 3.99 (0.26×) · leaf-only 0.875 (1.19×). Five new
variants vs the blocked-scalar baseline A (2.66):
| variant | ns/el | vs A | vs MKL | what it does |
|---|---|---|---|---|
| A blocked-scalar transpose | 2.66 | 1.00 | 0.39 | 3 scalar 8×8-tiled transposes |
| fuseB/C (elim 1 transpose pass, scalar tile) | 2.52–2.56 | 1.04–1.05 | 0.41 | fold one transpose into L1-hot tile write |
| **fuseD** (elim BOTH passes, scalar tile) | **2.47** | **1.07** | 0.42 | both inter-stage transposes fused, 8-wide contiguous bursts |
| **E** (3 SIMD `vperm2f128` transposes) | **2.47** | **1.07** | 0.42 | wide 256-bit loads/stores + register shuffle |
| F (fuseD + SIMD tile-write) | 2.97 | 0.89 | 0.35 | ⚠ REGRESSED — 2-wide strided SIMD store < 8-wide contiguous scalar |
⭐ **TWO independent ~1.07× levers over A (≈1.6× over the shipped engine b=1, gate 5.3e-16): SIMD register
transpose (E) and pass-elimination (fuseD)** — both real, both KEEP. ⚠ **They don't compose naively (F regressed):
the SIMD `vperm2f128` 2×2 block writes 2-wide strided, losing fuseD's 8-wide contiguous locality.** ⭐⭐ **PLATEAU:
every transpose variant lands ~2.47 = 0.42× MKL.** leaf-only 0.875 ≈ MKL-full 1.045, so MKL's assembly adds ~0.17
while ours adds ~1.6 no matter how the explicit transpose is done — because we do ~3 full-array reorders (64 KB
each) and MKL does ~none. ⇒ **Next candidate: ELIMINATE the explicit transposes structurally** — either (a) a
proper 8×8 register transpose (4 vec→4 vec via vunpck+vperm) so the fused tile-write is BOTH SIMD and 8-wide
contiguous (fixes F), or (b) fold the data reorder INTO the leaf's butterfly stores / a cache-oblivious recursive
four-step that never materializes a transpose (what makes MKL's assembly near-free). Single-thread f64 STAYS OPEN;
the engine b=1 path is now beaten 1.6× and the whole remaining gap to MKL is the transpose data movement.

### ⭐ Candidate C-4 — BATCHED b=16 (the REAL-workload regime) — the like-for-like that matters (2026-06-15, advisor-flagged)
The 4M/8M four-step runs its 4096 sub-FFTs BATCHED (`execute_batched`, b≈16), NOT b=1 — and **MKL is WORSE batched**
(measured b=1=0.97, b=16=1.19; candidate D saw b=16=1.52 — varies, but always worse than its b=1). So the b=1 line
was chasing MKL's best case in a regime the workload never uses. Measured b=16 4096 f64, single-thread, like-for-like:
| kernel | ns/el | vs MKL b=16 | vs engine |
|---|---|---|---|
| MKL b=16 | 1.19 | 1.00 | — |
| CRD engine b=16 (shipped batched) | 2.93 | 0.40× | 1.00 |
| **atom (E, SIMD transpose) ×16** | **2.45** | **0.48×** | **1.20× (BEATS engine)** |
⭐⭐ **The atom four-step (SIMD register transpose) BEATS the shipped engine batched path 1.20× in the real
regime** (and 1.44× at b=1) — so the register-transpose work is ON the critical path, a genuine shippable win over
the engine, NOT a b=1 detour. Still 0.48× MKL (better than the 0.39× b=1 number because MKL degrades with batch).
⭐ Note the engine's Stockham is ALREADY transpose-free yet SLOWER (2.93) than the transpose-heavy atom (2.45) — so
"transpose-free" is necessary-not-sufficient; the engine path has its own overheads. **Remaining gap to MKL is the
transpose data movement (~1.5 ns/el; MKL adds ~0.3).** ⇒ Next: the transpose-free / cache-oblivious four-step,
measured IN THE BATCHED REGIME (not b=1), and a composed full 4M/8M run. Single-thread f64 STAYS OPEN.

### ⭐⭐ Candidate C-5 — b=16 HARNESS HARDENED + 8M profile: atom is a MODEST engine win, NOT an MKL crush (2026-06-16, user-mandated audit)
**Harness audit found 2 real bugs:** (1) print bug (engine ns/el field printed the ratio); (2) **the engine was
fed TRANSFORM-major data when `execute_batched` expects ELEMENT-major** (`data[i·b+t]`, fft.hpp:461,483) — no
correctness gate caught it. Both fixed; added machine-eps gates (atom 5.65e-16, engine 7.49e-16 vs MKL b=16) +
reset-vs-no-reset (<2%, timing unbiased by in-place mutation). ⭐ **KEY CORRECTION: MKL is NOT worse batched** —
hardened MKL b=16 = **1.04–1.06** ≈ MKL b=1 (1.0); the earlier "b=16=1.19/1.52" was a harness artifact (no warmup
+ mutated buffers). The advisor's (and my) "MKL degrades with batch" premise was WRONG. Hardened b=16 4096 (4 runs,
reset): MKL 1.05 · engine 2.77 (0.38×) · atom(E) 2.37 (0.44×) — **atom beats engine 1.13–1.21× (mean 1.17×)**,
≥1.10× ⇒ KEEP per rules, but only **0.44× MKL** (not the 0.48× the unhardened run claimed).
⭐⭐⭐ **8M REALITY CHECK (the actual target): MKL 8.43 · engine 11.21 = 0.75× MKL** (NOT 0.4× — at 8M, MKL also
pays the DRAM-bound transpose so its lead shrinks). 8M four-step phase profile: **sub-FFT = 46.3%** (P1 21.5 + P2
24.8), **data movement = 53.7%** (gather 32.5 + twiddle 9.4 + scatter 11.9). ⇒ **EXPECTED-EFFECT of composing atom
→ sub-FFT: 1/((1−0.463)+0.463/1.17) = 1.07× on 8M (0.75→~0.80× MKL) — MODEST.** ⭐ **HONEST VERDICT: the 4096
single-transform atom work is a real but SMALL lever for the 8M target — the sub-FFT is <half of 8M, and the
DOMINANT cost is the 8M-level DATA MOVEMENT (54%: gather/twiddle/scatter), where the engine→MKL gap actually
lives.** The atom (KEEP, 1.17× sub-FFT) is worth ~1.07× composed; the bigger 8M lever is the gather/scatter/twiddle
data movement, not the sub-FFT codelet. Single-thread f64 STAYS OPEN; next focus = the 8M data-movement phases.

### ⭐⭐⭐ Candidate C-6 — atom COMPOSITION into 8M REFUTED by measurement + full scoreboard (2026-06-16, gated engine patch, reverted)
Composed atom(E) into the engine's 8M four-step pass2 sub-FFT (gated, both directions measured, machine-eps correct):
| composition | 8M ms | vs MKL | vs current CRD | verdict |
|---|---|---|---|---|
| current CRD | 82 | 0.86× | 1.00 | baseline |
| Patch A: atom + element↔transform adapter | 92 | 0.64× | 0.89× (SLOWER) | REVERT |
| Patch B: atom + fused strided gather/scatter | 247 | 0.28× | 0.33× (CATASTROPHIC) | REVERT |
⭐ **THE STANDALONE 1.17× ATOM WIN DOES NOT COMPOSE.** Patch A's element↔transform-major adapter = 2 extra
transpose passes (> the 1.17× saved). Patch B's strided gather from `tbuf` / scatter to `din` (stride n1=2048) =
DRAM cache-thrash. **STRUCTURAL ROOT: the atom is transform-major single-transform (its SIMD = the internal 8×8
register transpose); the engine sub-FFT is element-major BATCHED (its SIMD = the batch axis, gather kept contiguous
by design). They are transposes of each other — reconciling ALWAYS costs a transpose that exceeds the gain. In
element-major mode the atom degenerates to exactly `execute_batched` (no win).** The b=16 "1.17×" was real but
layout-dependent (atom ran on already-transform-major data); it is NOT exploitable in the 8M pipeline. Engine
patches reverted (source clean); the build/ POC stays as the proven STANDALONE result. ⭐⭐ **FULL TARGET SCOREBOARD
(single-thread, core-pinned, MKL 1 thread, all correct f64 1e-15 / f32 3e-7):**
| | 2M f64 | 4M f64 | 8M f64 | 2M f32 | 4M f32 | 8M f32 |
|---|---|---|---|---|---|---|
| CRD vs MKL | 0.712× | 0.776× | 0.759× | 0.584× | 0.646× | **0.611×** |
⭐ **f32 lags MORE than f64 (~0.6× vs ~0.75×) = the bigger opportunity.** The path to parity is NOT the sub-FFT
codelet (refuted) — it is the engine's existing DATA MOVEMENT (gather 32% + twiddle 9% + scatter 12% = 54% of 8M),
where MKL's near-free transpose beats ours, AND the f32 path (which lags ~0.15 more than f64). Single-thread STAYS
OPEN; next patch = engine data-movement (twiddle/scatter SIMD + scatter tiling) and the f32 gap, NOT the atom.

### Candidate C-7 — Patch 1 (SIMD twiddle) REVERTED: the twiddle is GATHER-bound (2026-06-16)
SIMD-vectorized the pass1 twiddle loop (fft.hpp:1011, Vec4d over k1, non-FMA to match scalar rounding, gated
`CRD_FFT_TWIDDLE_SIMD`). **Phase measured 1.34× SLOWER (P1 twid 27.1 → 36.3 Mcyc/call over 10 calls); full 8M
unchanged (82.9 vs 83.0 ms); correctness preserved (1.20e-15).** REVERT. ⭐ **Root: the twiddle is GATHER-bound,
not compute-bound** — per element it needs 4 non-contiguous table lookups (`hir/hii[a>>h]`, `lor/loi[a&mask]`,
`a=col·k1` ⇒ indices non-contiguous in k1) + a strided z load (`scratch[k1·bw+bb]`, stride bw). The Vec4d path
does **6 AVX2 gathers per 4 elements**, which serialize worse than the scalar version's pipelined loads; the
arithmetic (8 mul + 4 add/sub) was never the bottleneck. SIMD-ing compute can't fix a gather/strided-access phase.
⇒ The remaining movement phases (gather 32% contiguous bw-run memcpy = bandwidth-bound; scatter 12% NT-store =
bandwidth-bound) have limited SIMD headroom too — the 8M parity gap is a memory-BLOCKING problem (MKL's
cache-oblivious data movement), not a SIMD-the-compute problem. Next: Patch 5 (gather blocking/locality), the
largest slice; f32 (~0.61×) remains the larger-margin target. Scoreboard UNCHANGED (Patch 1 reverted):
| CRD vs MKL | 2M f64 | 4M f64 | 8M f64 | 2M f32 | 4M f32 | 8M f32 |
|---|---|---|---|---|---|---|
| current | 0.71× | 0.78× | 0.76× | 0.58× | 0.65× | 0.61× |

### Candidate C-8 — Patch 5 (gather attack) FAILED: bandwidth/TLB-bound at the 22.5 GB/s wall (2026-06-16)
Phase-exact P1 gather microbench (real 8M strides: din[i1·n2+i2..+bw] stride n2=4096, bw=32, 128 MB moved, NOT a
toy contiguous array). All candidates vs the current memcpy+pf8 (all bit-identical, diffs=0):
| candidate | phase speedup | measured reason |
|---|---|---|
| A pointer-increment | 0.99–1.02× | address muls already free/hidden by the OOO engine |
| B SIMD copy (explicit AVX) | 1.01–1.04× | marginal + sub-noise; glibc memcpy is already SIMD |
| C 4-row interleave (multi-stream) | 0.93–0.95× | 8 streams (4R+4W) > fill buffers; the 36.9 GB/s 4-stream win needs CONTIGUOUS streams, not 64KB-strided |
| D deeper prefetch (16/32/64/128) | 0.78–0.82× | over-prefetch evicts useful lines; pf=8 already optimal |
⭐ **The gather is at ~22.5 GB/s = the single-stream DRAM wall (≈11.6 GB/s real DRAM read after subtracting the
L2-resident scratch write) — latency/TLB-bound on the 64 KB-stride column reads, NOT compute.** Same wall class as
[[project_v7e2_lattice_cholmod_perf]] (1-stream 22.7 GB/s); the multi-stream lever that won there needs contiguous
streams, which the four-step's strided column-gather is not. Nothing kept; engine gather unchanged. The 8M f64
movement (gather 32% + scatter 12% + twiddle 9%) is bandwidth-bound; MKL's edge is a cache-oblivious transpose with
better locality (a structural rewrite, not a phase patch). Next per queue: scatter (12%, NT-store, also
bandwidth-bound — likely same wall). Scoreboard UNCHANGED. f64 8M parity is a memory-locality structural problem.

### Candidate C-9 — Patch 6 (scatter 256-bit NT) phase-win but FULL-NEUTRAL; structural prize quantified (2026-06-16)
Phase-exact scatter microbench (real strides, NT, bit-identical): **256-bit NT = 1.059× phase (34.2 vs 32.3 GB/s)**
vs current per-complex 128-bit NT; cached-256 was 0.39× (RFO confirms NT is right). Extended to BOTH scatter (12%)
AND the pass1 twiddle store (9%) under `CRD_FFT_NT256` (paired 256-bit NT, bit-identical, checksum-verified). **Full
8M f64 (low-noise CRD-only, interleaved): NEUTRAL-to-slightly-worse (base 77–86 / NT256 80–87 ms, checksum
identical).** The isolated phase win does NOT survive composition — in-context the NT writes overlap adjacent
blocks' sub-FFT/gather and saturate the same write bandwidth. REVERT (source clean). ⭐⭐ **ALL local phase patches
now fail: twiddle (gather-bound), gather (DRAM-wall), scatter (compose-neutral). The f64 8M gap is STRUCTURAL.**
⭐⭐⭐ **STRUCTURAL PROTOTYPE STARTED — movement-floor measured (`build/movement_floor.cpp`):**
| movement primitive | GB/s | note |
|---|---|---|
| seq copy 256MB (NT) | 27.7 | pure bandwidth floor (uncontended, sequential) |
| element transpose (best: 2-level 128/8) | 13.2 | strided-write pattern caps at ~½ the copy rate |
| engine gather (bw-blocked, isolated) | 22.7 | already good — bw-runs beat naive transpose |
| **in-context four-step movement** | **~12 (eff.)** | **~512MB over ~42ms — phases CONTEND for DRAM** |
⭐ **The lever is NOT a faster transpose (engine bw-blocking already ≈22.7) — it is CUTTING TOTAL DRAM TRAFFIC.**
The four-step moves ~512MB (din-read + tbuf-write + tbuf-read + din-write); the **tbuf round-trip is 256MB of
avoidable traffic**. A cache-oblivious / fused four-step that keeps a tile resident across pass1→pass2 (no full-array
tbuf) targets ~halving movement → ~21ms saved → 80→~59ms ≈ **0.76→~1.0× MKL = parity**. This is the remaining
build: a standalone fused four-step prototype (pass1+twiddle+pass2 per cache-resident tile-column, engine-native
layout, NO atom). Defined by measurement; the next concrete patch. Scoreboard UNCHANGED (Patch 6 reverted).
⚠⚠ **C-9's "cut the tbuf round-trip / ~21ms prize" is REFUTED by C-10 below — the dependency proof shows the tbuf
CANNOT be eliminated. Disregard the 21ms figure.**

### ⭐⭐⭐ Candidate C-10 — TBUF ELIMINATION PROVEN INVALID; f64 8M is near its STRUCTURAL CEILING (2026-06-16)
Did the dependency proof FIRST (before building), as the structure demanded. Exact index map from the shipped
`execute_four_step` (8M, n1=2048, n2=4096): din[i1·n2+i2] → P1 FFTs columns (len n1) → twiddle writes
tbuf[i2·n1+k1] (transpose) → P2 FFTs tbuf columns (len n2) → scatter din[k2·n1+k1]. **P2's k1-block reads
tbuf[i2·n1+k1] for ALL i2=0..n2−1, and each is produced by P1 on din-column i2 ⇒ every P2 block needs ALL n2 P1
columns done. The transpose dependency is TOTAL.** A tile/stripe cannot hold a complete P2 input without the full
tbuf or recomputing P1 per stripe (n1/S× blowup). ⇒ **FULL TBUF ELIMINATION INVALID; the full transpose
intermediate is necessary; the 512MB / 4-pass traffic (din-read + tbuf-write + tbuf-read + din-write) is
STRUCTURAL** for a two-pass matrix-FFT. ⭐ **Movement-only measurement (`CRD_FFT_NOSUBFFT` gate, 8M):** full 68ms,
movement+twiddle **35ms**, sub-FFT ~33ms (≈50/50). Pure movement ≈29ms over 512MB = ~17.7 GB/s; strided-achievable
floor (gather 22.7 / scatter 32, ~25 GB/s) ≈20.5ms; sequential floor (27.7) ≈18.5ms ⇒ **only ~8ms movement
headroom, and the gather/scatter LOCAL patches already failed to capture it (bandwidth/contention/cold-strided
bound).** ⭐⭐ **HONEST STRUCTURAL VERDICT: f64 8M at 0.76× MKL is near the structural ceiling for the four-step on
this AVX2 box.** The remaining ~13–16ms gap to MKL is split ~8ms movement (locally-uncapturable, strided/contended)
+ ~5–8ms sub-FFT (radix-8 micro-arch). No traffic-reducing valid partial exists (proven). MKL's edge is
micro-architectural transpose/sub-FFT tuning (and AVX-512 on capable hardware — this box is AVX2-only), NOT a
structural algorithm we're missing. Reaching parity would need either an in-place non-square transpose (random-
access, measured-slower class) or sub-FFT radix micro-tuning (excluded) — both major effort, uncertain payoff.
All FFT engine probes reverted; source clean. **Scoreboard UNCHANGED.**

### Candidate C-11 — Front A: f64 sub-FFT audit + scheduler reorder (A2) FAILED; sub-FFT is SHUFFLE+SPILL-bound (2026-06-16)
Reopened radix/sub-FFT (movement front closed). **A1 audit** — `execute_batched` 4096 b16 = **2.71 ns/el = 0.39×
MKL**; inlined-body mnemonic mix (952 insns): **shuffles 160 + stack-spills 110 = 270 > compute 200** (FMA 40 +
mul 40 + add/sub 80), ld/st 141, vbroadcast 20. ⭐ **The sub-FFT is SHUFFLE-bound (160, from the AoS element-major
`load/store_complex_*` deinterleave) + SPILL-bound (110, the radix-8 16-data + 14-twiddle = 30-live waist over 16
ymm) — NOT compute-bound.** **A2 (scheduler-only compute-store interleave of butterfly8 stage C, bit-identical,
gated `CRD_FFT_B8SCHED`): FAILED — spills 110→120, sub-FFT 2.59→2.63 ns/el, REVERT.** gcc already schedules this
(small) basic block optimally; constraining source order hurt (unlike the huge standalone atom codelet where it
helped). The 110 spills are gcc-optimal for 30 live values; source reorder can't cut them. ⭐⭐ **The sub-FFT levers
are: (1) the 160 SHUFFLES — removable ONLY by a SoA-split scratch (deinterleave once at gather → sub-FFT on split
re/im with zero per-butterfly shuffles → interleave at scatter), a layout change beyond "scheduler-only"; (2) the
spills — gcc-optimal, not source-fixable.** So the real sub-FFT win = SoA-deinterleave-once (cuts the 160-shuffle
tax that recurs every radix stage). This helps f64 AND more so f32 (Vec8f deinterleave is heavier). Scoreboard
UNCHANGED. Next front: B1 (f32 phase profile) — f32 is the bigger gap (0.58–0.61×) and the SoA-shuffle tax is
worse there; then the SoA-split sub-FFT scratch as the shared f64+f32 sub-FFT lever.

### ⭐⭐⭐ Candidate C-12 — SoA-split sub-FFT REJECTED; the 160 shuffles were NOT the bottleneck (2026-06-16)
Built the scoped SoA-split POC (`build/soa_subfft.cpp`, deinterleave-once → radix-8 on split re/im with ZERO
per-butterfly shuffles → interleave-once; correct vs MKL 6.48e-16). 4096 b16 f64:
| variant | ns/el | vs AoS | note |
|---|---|---|---|
| AoS engine (full) | 2.752 | 1.00 | current |
| SoA-split (full, separate de/interleave) | 4.549 | **0.605× (loses)** | de/interleave pack/unpack overhead = old SoA failure mode |
| SoA stages-only (radix-8 on split, NO shuffles, NO bitrev/de-interleave) | 2.482 | **1.109×** | the absolute best case |
⭐⭐ **KEY: removing ALL 160 shuffles AND skipping bitrev+de-interleave gives only 1.109× — so the shuffles were
NOT the bottleneck.** Port-5 shuffles (160) run in PARALLEL with port-0/1 compute (200 FMA/mul/add); eliminating
them shaves only ~10% because the critical path is compute+spills, not shuffles. And the de/interleave pack/unpack
(+2.07 ns/el) more than eats that 10% ⇒ full POC 0.605×. **This invalidates the C-11 premise that "SoA-deinterleave-
once is the real sub-FFT lever" — the shuffle count was a red herring (parallel-port).** REJECT (outcome B). ⭐⭐⭐
**CONVERGED VERDICT across the whole sprint: f64 8M sub-FFT is compute+spill-bound (best-case shuffle removal = 10%,
eaten by de/interleave), movement is bandwidth/cold-strided-bound (local patches failed), tbuf is provably
necessary. Every local + structural + layout lever tested and measured-negative. The remaining f64 levers are
radix micro-arch (diminishing — the radix-8 compute is the floor) or AVX-512 (this box is AVX2-only).** Engine
source clean (probe was build/-only). Scoreboard UNCHANGED. Next: f32 phase profile (B1) — bigger gap, but the
same port-parallel analysis likely applies; f32 worth profiling since it lags ~0.15 more.

### Candidate C-13 — f32 8M profile: same walls as f64, MKL just further ahead (2026-06-16)
f32 8M: CRD 52.9ms · MKL 30.4ms = **0.575×**. Phase profile: P1 gather 11.7 · sub-FFT 22.0 · twid 15.8 / P2
gather 11.4 · sub-FFT 24.9 · scatter 14.1 → sub-FFT 46.9% + movement 53% (same split as f64). ⭐ **Diagnosis: CRD
f32 is only 1.29× its f64 (52.9 vs 68ms); MKL f32 is 1.81× its f64 (30 vs 55ms).** MKL extracts the full ~2× from
half-width f32; CRD doesn't — the shortfall is the **movement+twiddle (53%) NOT halving for f32** (bandwidth/cold-
strided movement + scalar twiddle = the SAME walls f64 hit; only the Vec8f sub-FFT scales). f32 twid is 15.8% (vs
f64 9.4%) but f64 SIMD-twiddle already failed (gather-bound), so the f32 twiddle has the same wall. ⇒ f32 has NO
easy win CRD is missing; it's the same exhausted fronts with MKL further ahead (likely AVX-512 16-wide f32 on
capable HW — this box AVX2-only). ⭐⭐⭐ **FINAL SPRINT VERDICT (12+ measured candidates): the large-N FFT
(f64 0.71–0.78× · f32 0.58–0.65× MKL) is at the structural ceiling for the four-step on this AVX2 box.** Movement =
bandwidth/strided-bound (local patches all failed); tbuf = provably necessary; sub-FFT = compute+spill-bound (shuffle
removal only 10%, port-parallel, eaten by de/interleave); twiddle = gather-bound. The shipped WINS remain real and
ahead of MKL (small-N N≤32 crushes, real-FFT, DCT/DST, NUFFT — see dossier head). Large-N complex parity needs
AVX-512 or MKL-class movement micro-arch, both beyond AVX2 portable-C++. Engine source clean; all probes in build/.

### ⭐⭐⭐⭐ Candidate C-14 — BLACK-BOX MKL ARCHAEOLOGY: factorization (H1) WON — ceil-split KEPT, +9–13% (2026-06-16)
The "AVX2 ceiling" was PREMATURE. MKL archaeology (in-place vs oop, f64/f32, N=2^18..23) found the f64 CRD/MKL ratio
is **non-monotonic**: 256K 0.51 · 1M 0.68 · 2M 0.82 · **4M 0.945** · 8M 0.76 — peaking at **4M = 2^22 (SQUARE split
2048×2048)**. The gap concentrates at NON-square (odd-log2) N. Also: MKL in-place ≤ out-of-place at large N ⇒ MKL is
in-place-optimized (NOT streaming-scratch). **Hypothesis H1 (factorization) tested by an n1 sweep:**
| N | floor n1 (old) | ceil n1 (new) | speedup |
|---|---|---|---|
| 8M 2^23 | 2¹¹ (2048×4096) 80.9ms | **2¹² (4096×2048) 74.6ms** | +8.5% (re-runs +5–9%) |
| 2M 2^21 | 2¹⁰ 17.5ms | **2¹¹ 17.1ms** | +2.3% (scoreboard +13%) |
| 4M 2^22 (square) | 2¹¹ 34.2ms | 2¹¹ (same) | unchanged ✓ |
⭐⭐ **ROOT: the engine used `m_n1 = 2^floor(log2/2)` ⇒ for ODD log2 it put the SMALLER factor first; the LARGER
factor as n1 is faster.** Fix = `m_n1 = 2^ceil((log2+1)/2)` (one line, square for even log2 unchanged). **KEPT.**
Full scoreboard before→after (CRD absolute; MKL thermal-varies): 2M f64 17.74→15.75 (+13%) · 8M f64 86.9→79.7
(+9%) · 2M f32 11.13→10.47 (+6%) · 8M f32 55.7→50.6 (+10%) · 4M unchanged (square). Correctness: forward vs MKL
~1e-15, round-trip fwd+inv 2¹⁹–2²³ all ~2e-15 OK. **NEW SCOREBOARD: f64 2M ~0.83 · 4M ~0.85 · 8M ~0.80 (was 0.76) ·
f32 2M ~0.58 · 4M ~0.67 · 8M ~0.65 (was 0.62).** ⭐ The archaeology sprint produced the campaign's FIRST kept
large-N improvement — H1 (split shape), not the exhausted local patches. ⚠ engine change (m_n1 formula) PENDING the
full DoD before commit (correctness-validated; bit-accuracy class preserved). Next archaeology: whether a 3-factor
split helps the largest N further, and the f32 movement (still ~0.58–0.65×, MKL's f32 1.81×-over-f64 vs CRD ~1.3×).

### Candidate C-15 — Phase 3: 3-factor / multi-factor REJECTED by component measurement (2026-06-16)
After banking the larger-factor-first split, tested whether a 3-factor plan beats the kept 2-factor (8M=4096×2048).
Two measured components: **(1) sub-FFT ns/el sweep (b=16):** 128:1.71 · 256:1.78 · 512:2.05 · 1024:2.44 · 2048:2.54
· 4096:2.59 — smaller factors only **1.46× faster** (256 vs 4096). **(2) extra-transpose cost** (`movement_passes.cpp`,
8M blocked transpose round-trip 256MB): **+24.2 ms @ 10.3 GB/s.** ⭐ A 3-factor has the SAME total butterfly work
(N log N) split over **3 sub-FFT passes not 2** (3×1.8 = 5.3 ≥ 2×2.55 = 5.1 ns/el-equiv ⇒ sub-FFT net neutral)
PLUS **one extra global transpose (+24 ms measured)**. Predicted 3-factor 8M ≈ 104 ms vs 2-factor 80 ms = **~0.77×
(≈30% slower)**. The extra transpose alone exceeds the MAX achievable sub-FFT saving (~14 ms), and the extra
sub-FFT pass cancels even that. **REJECT (measured, not assumed).** ⇒ MKL is NOT using a 3-factor either (same
penalty). MKL's remaining ~20% 8M edge (CRD 0.80×) is the **movement/transpose micro-arch** (the naive transpose
runs 10.3 GB/s; the engine's bw-gather is 22.7 — MKL's is likely better still) and/or AVX-512. Engine clean
(probes build/-only). Next archaeology front: **f32-specific** (biggest gap 0.58–0.65×; MKL f32 1.81×-over-f64 vs
CRD 1.3× — the f32 movement/plan, not random SIMD) + the 16M square-split check (2^24 even → 4096² square, may hit
the 4M-class ~0.95× near-parity behavior).

### ⭐⭐⭐ Candidate C-16 — f32 archaeology: NT-store was f64-ONLY; f32 scatter NT fix KEPT (2026-06-16)
f32 phase profile vs f64 8M (Mcyc/call, ratio = f32/f64, 0.5 = halved): P1 gather 0.48✓ · P1 sub-FFT 0.57✓ ·
**P1 twiddle 0.91✗** · P2 gather 0.44✓ · P2 sub-FFT 0.56✓ · **P2 scatter 0.85✗**. ⭐ **The store-heavy phases
(twiddle, scatter) did NOT halve for f32; gather+sub-FFT did.** ROOT (smoking gun): `store_complex`'s NT path is
`if constexpr (is_same_v<T, f64>)` — **f32 had NO non-temporal store, paying read-for-ownership on every store.**
Fix: f32 complex = 8B < 16B NT-min, so **pair two f32-complex into one 16B `_mm_stream_pd`** in the scatter (drow
16B-aligned). **Measured: scatter phase 18.0→10.8 Mcyc @8M (HALVED, 0.60); f32 8M ~+5% (phase-derived; noisy
scoreboard showed up to +9.8%).** ⚠ The same NT applied to the TWIDDLE store was a NO-OP (25.6→26.1) — the twiddle
is **table-lookup-bound** (4 factored-table gathers/element, same count f32/f64), NOT store-bound; removed that
dead code. KEPT (scatter NT only). f64 unchanged (already had NT). DoD: win-debug + win-shipping FFT ctest 17/17,
correctness + run-twice determinism preserved (pure data move). ⭐ **NEXT: the TWIDDLE (table-lookup-bound, 10.5%
of f64 / 16% of f32) — a blocked/contiguous per-block twiddle panel (sequential lookups vs the 2-table gather)
would help BOTH precisions.** PENDING USER COMMIT (the f32 scatter NT, like the split). Updated scoreboard: f64
unchanged (2M 0.80 · 4M 0.85 · 8M 0.80); f32 ~+5% large-N (8M ~0.62→~0.65).

### Candidate C-17 — twiddle TABLE-LAYOUT archaeology REJECTED: lookups are NOT the bottleneck (2026-06-16)
Hypothesis: twiddle is table-lookup-bound (4 scattered factored-table gathers/element). Phase-exact microbench
(f32, 8M P1 layout): current 2.98 ns/el vs **hybrid recurrence (reseed from exact table every K, recurrence
between — removes ALL lookups): K=16 = 2.87 ns/el = only 1.04×** (rel_err 2.7e-7, at the f32 edge). ⭐⭐ **Removing
every table lookup buys only +4% ⇒ the lookups are NOT the bottleneck** — the factored √n tables are L2-resident and
pipeline fine. The lookup-bound premise (C-11/C-16) was WRONG. REJECT (below 1.10×, error grows with K). ⭐ **The
twiddle's real nature: it is a TRANSPOSE-MULTIPLY** (reads scratch[k1·bw+bb] strided-by-bw over k1, scalar complex
mul, writes tbuf contiguous-per-col) — scalar + strided-read bound, which is why it doesn't halve for f32 (scalar
ops don't shrink with precision) and why SIMD-ing it hits the inherent transpose scatter (the f64 SIMD-twiddle
already failed on exactly that). ⇒ The twiddle is not improvable by table layout. **POST-ARCHAEOLOGY STATE: two
banked wins (split +9–13%, f32 scatter NT +5%); f64 ~0.85× · f32 ~0.65× MKL.** Remaining f32 levers: a
SIMD-recurrence twiddle that also solves the transpose-write (high effort, transpose-bound like movement), or
AVX-512 (this box AVX2-only). Next front: f32 sub-FFT scales already (0.56, Vec8f active — little headroom), so the
honest remaining levers are the transpose-multiply twiddle (hard) and AVX-512 (hardware) — NOT table layout.

### ⭐⭐⭐⭐ Candidate C-18 — f32 SIMD twiddle (bb-axis Vec8f recurrence) WON +11%, KEPT (2026-06-16)
The transpose-multiply twiddle CAN be SIMD'd — on the **bb axis** (the one not tried; the failed f64 attempt used
the k1 gather axis). Loop-axis map: bb-inner = **contiguous scratch read** + Vec8f mul + transposed tbuf write that
hits **8-row L2-resident bands** (NOT DRAM-scattered, the key). Twiddle by **recurrence** (`w*=W_n^col`, 8 cols/
lane) reseeded from the factored table every K=8 (bounds f32 drift). Candidate table (phase-exact f32 microbench):
current scalar 1.16 ns/el → **candA bb-Vec8f 0.539 = 2.15×** (rel_err 3.0e-7). In-engine (gated→unconditional
AVX2): **twiddle phase 25.6→16.4 Mcyc @8M (1.56× in-context); clean interleaved f32 8M A/B = +11–12%** (checksum
bit-identical, the noisy scoreboard's −1% was thermal). Correctness 2.82e-7 (f32 class), round-trip clean. f64
unchanged (stays scalar — Vec4d gives less and f64 isn't the gap). DoD: win-debug + win-shipping FFT ctest 17/17.
KEPT (PENDING USER COMMIT). ⭐⭐ **THREE banked archaeology wins now: split (+9-13%), f32 scatter NT (+5%), f32 SIMD
twiddle (+11%). Scoreboard: f64 ~0.80–0.85× · f32 8M 0.61→~0.75× MKL** (f32 lifted most). Lesson: "transpose-
multiply can't be SIMD'd" (C-17) was wrong for the bb axis — the transposed write lands in L2 bands, not DRAM
scatter. Next front: the sub-FFT (now the biggest phase ~73 Mcyc f32; scales via Vec8f but check schedule headroom)
or accept the much-healthier state. The archaeology method keeps producing wins — NOT a ceiling.

### ⭐⭐ Candidate C-19 — f32 sub-FFT batch-width: no headroom; f32 large-N campaign CLOSES with 3 wins (2026-06-16)
f32 sub-FFT 4096 batch sweep: b=8 1.63 · **b=16 1.333 · b=32 1.340** (current) · b=64 1.350 ns/el. **b16≈b32 (0.5%,
noise) — the f32 block_width=32 is already optimal; no batch-width win.** The sub-FFT scales via Vec8f (f32/f64
0.56) and the C-11 f64 audit already showed scheduler reordering is gcc-optimal (no headroom). REJECT (no patchable
sub-FFT headroom). ⭐⭐⭐⭐ **POST-ARCHAEOLOGY BASELINE LOCKED (3 banked wins): f64 4M 0.92 · 8M 0.77 · 16M 0.81 ·
f32 4M 0.68 · 8M 0.78 · 16M 0.74. f32 8M caught up to f64 8M (both ~0.77) — the f32-specific gap is CLOSED.**
=== FINAL LARGE-N DOSSIER (8M, AVX2 14900K, single-thread) ===
| front | result |
|---|---|
| larger-factor-first split (planner) | ✅ KEPT (+9–13% f64+f32) |
| f32 non-temporal scatter store | ✅ KEPT (+5% f32) |
| f32 SIMD twiddle (bb-axis Vec8f recurrence) | ✅ KEPT (+11% f32) |
| 3-factor / multi-factor plan | ❌ rejected (extra transpose +24ms > sub-FFT saving) |
| twiddle table layout (recurrence) | ❌ rejected (lookups cheap, +4% only) |
| f32 sub-FFT batch-width / schedule | ❌ rejected (b16≈b32 optimal; gcc-scheduled) |
| atom / 3-factor / SoA-split / local gather / f64-SIMD-twiddle-wrong-axis | ❌ rejected earlier |
⭐ **The remaining gap (f64 ~0.80× · f32 ~0.78× at 8M) is now PRECISION-AGNOSTIC** — it's the general FFT
movement/micro-arch vs MKL, no single phase failing. Both precisions improved markedly from the archaeology (f64
8M 0.76→0.80, f32 8M 0.61→0.78). **Future fronts (beyond the AVX2 single-thread four-step, NOT local patches):**
AVX-512 backend · GPU FFT · generated codelet/planner architecture · multi-threaded large-N. The black-box
archaeology method produced 3 real banked wins where the prior "AVX2 ceiling" call had given up — the method, not
ceiling claims, is what worked.

## 1d. ⭐⭐⭐ THE GENFFT ATOM BEATS MKL — N=64 four-step, over-2 leaves (2026-06-14)

The redirect that cracked it (advisor-gated): **the over-2 split-radix codelet is the LEAF of a recursion, not a
standalone transform.** A four-step N=n1·n2 makes the *outer* DFTs land in different registers (stride-n2 →
lane-aligned, ZERO `permute2f128`); only the small leaf does any intra-register shuffle. This is hpkfft's actual
construction (their benchFFT range is N=2..8192, *single-transform L1-resident* — exactly where I'd measured my
all-in-one codelet at 48 < MKL). The all-in-one codelet was permute2f128-bound *because it did every stage
in-register including the lane-crossing ones*; the recursion does those as lane-aligned strided loads.

**The atom — `build/fftcrush_n64.cpp`, N=64=8×8:** Stage A = 4×(contiguous-load over-2 `sr8` + per-lane twiddle
`cmul` + contiguous store to sA); inline-gather transpose (`_mm256_set_m128d` of two 128-bit loads); Stage B =
4×(over-2 `sr8` + contiguous store to X). Output permutation **X[k2p·8+k1p]** places the two over-2 lanes adjacent
⇒ **only ONE transpose, no output transpose.** Gated vs brute DFT-64 = **2.87e-16**.

| measurement (N=64 batched×16, L1, 1 thread, best-of-30) | GFLOPS |
|---|---|
| Cerid four-step (full, gated) | **56–62** |
| **MKL N=64 batched** | **55–60** |
| **ratio** | **PARITY (ranges OVERLAP — within thermal noise, NOT a win)** |
| Cerid no-twiddle ceiling | 75 |
| Cerid no-transpose ceiling | 84 |
| Cerid leaf (over-2 sr8) isolated | ~56 ≈ 1.3–1.5× MKL's per-flop rate |

**Crucial measurement methodology fix:** Cerid's rate must be measured *bracketing* MKL (before AND after) in the
SAME binary — a first run read 0.63× because of a transient 14900K throttle; bracketed best-of-30 shows stable
parity. The transpose is **cheap** here (NOTRANS 34.8 ≈ REAL 35.6 in the throttled run; ceilings confirm). This
**clears the advisor's ≥0.9× go/no-go gate** ⇒ on the hpkfft 1.42× trajectory ⇒ generalize with confidence.

**N=512 = 8×64 — CONVERGED at/below MKL (`build/fftcrush_n512.cpp`, all gated 1.07e-15):** three variants measured,
all land at-or-below MKL:

| N=512 variant | Cerid | MKL | ratio |
|---|---|---|---|
| naive (8× `fft64` + scalar stride-8 scatter) | 40 | 53–56 | 0.73× |
| NOSCATTER ceiling (contiguous output, wrong order) | 57–59 | 63 | **0.92× — even a PERFECT output transpose < MKL** |
| FUSEDT (vectorized 2×2 `permute2f128` transpose) | 38.8 | 56 | 0.72× — **WORSE than scalar** |

**The decisive finding: vectorizing the transpose makes it WORSE (38.8 < 40).** The `permute2f128` ops are cheap,
but the vectorized transpose needs a full Yf buffer round-trip that costs more than the scalar scatter saved ⇒
**the reorder is fundamental DATA MOVEMENT, not an instruction-choice problem** (the recurring integration wall;
AVX2 has no efficient scatter). And the NOSCATTER ceiling (0.92×) proves *even a free output transpose is below
MKL* at N=512 — the twiddle+reorder overhead per level lands the construction at parity-or-below, **by
construction.** This is converged: another transpose variant will not move it.

**Why parity is the construction's natural ceiling (from the N=64 ceilings):** leaf ~1.3–1.5× MKL, but twiddle
(~24%) + reorder (~32%) drag every size to ~57 = parity, and MKL pays comparable overhead. The better leaf is
*exactly offset* by paying MKL-equivalent overhead. **Closing parity → 1.42× (hpkfft) is NOT a probe — it is a
multi-week minimal-pass genfft engine** (over-2 leaf kept register-resident across radix stages; contiguous over-2
output so the per-level transpose *vanishes* rather than being materialized; far less twiddle/reorder traffic per
level). hpkfft proves the crush exists but does not hand over the construction. **Decision point, not a tuning gap.**

## 0. The bar and why it's winnable here

- **Bar (user-ratified 2026-06-13): BEAT MKL** — the absolute speed king — not just PocketFFT/FFTW. Scope a→h.
- **Why winnable on the dev box (load-bearing):** the i9-14900K (Raptor Lake) has **NO AVX-512** (fused off on
  consumer chips) ⇒ MKL runs its **AVX2** path here ⇒ a level AVX2-vs-AVX2 fight. (Honest caveat: an AVX-512
  server lets MKL pull ahead; the reference-class policy benchmarks the dev box.)
- **Existence proof:** `docs/books/hpkfft-paper-2023.pdf` (Caprioli & Jenkins) — **hpk::fft BEATS the vendor
  (MKL) by geomean 1.6× on AVX2 fp64 in modern C++**, in the AoS layout ("two complex points = 32 B"). So
  beating MKL on this exact target in C++ is **proven-achievable, not a long shot.**

## 1. ⭐ THE DECISIVE RESULT — the crush is achievable; the entire gap is shuffle count

On the within-transform split-radix AoS radix-16 codelet (`build/sr16.cpp`), isolating the shuffle cost with
`#ifdef` probes (wrong results, but they delete the shuffle work to measure its cost):

| codelet variant | GFLOPS | delta |
|---|---|---|
| split-radix-16 AoS (correct) | **47.9** | baseline |
| `-DNOPERM` (delete cmul `permute_pd`) | 57.3 | +19% |
| `-DNOS0` (delete the `s0` `permute2f128`) | 66.2 | **+38% ← the bottleneck** |
| `-DNOPERM -DNOS0` (delete both) | **82.7** | **ABOVE MKL's 71** |

**Interpretation:** the codelet's *arithmetic alone* (shuffles removed) runs at **82.7 GFLOPS > MKL's 71.** The
compute clears MKL. **The entire remaining gap is the in-register SHUFFLE COUNT** — specifically `permute2f128`
(the 128-lane swap). MKL does ~3× fewer of them. Closing that = the crush.

## 1b. ⭐⭐⭐ THE BREAKTHROUGH (2026-06-14) — over-2 + SPLIT-RADIX threads the trilemma (56 GFLOPS, 0.79× MKL)

`build/aos2sr8.cpp`: **AoS-over-2-transforms (each ymm = `[t0_elem_i, t1_elem_i]`) + split-radix radix-8.**
**56 GFLOPS, correct 1.66e-16, stable** — **+22% over within-transform (46), 0.79× MKL.**

**This reverses the "over-2 is dead" verdict.** Over-2 was dead with *naive radix-2* (24–27) because of a serial
dependency chain (no ILP), NOT because of the layout. **Over-2 with SPLIT-RADIX has independent-sub-DFT ILP** —
and keeps the over-2 layout's killer advantage: **ZERO `permute2f128`** (the 2 complex per ymm are different
transforms, never combined → all butterflies lane-aligned). The only shuffles are the cmul `permute_pd` (2-port)
and ±i (`permute_pd`). Footprint 8 ymm (radix-8). **All three trilemma properties at once.**

**Radix sweet spot = 8** (`build/aos2sr8.cpp` 56 vs `aos2sr16.cpp` 54): over-2 radix-16 = 16 ymm spills at the
combine (no inter-codelet ILP) and drops to 54. **radix-8 (8 ymm, fits 2 codelets) is the sweet spot = 56.**

**Path to push 56 → past 71 (the crush):** the remaining overhead is the `permute_pd` (cmul + ±i, 2-port). The
shuffle-free ceiling is 82.7, so 56→82 is the `permute_pd` budget. Levers: **(1) conjugate-pair split-radix**
(`newsplit.pdf`/`ShaoJo07` — simplifies/reduces the twiddle multiplies ⇒ fewer cmul `permute_pd`); (2) CSE the
±i (`mneg_i` computed once); (3) better scheduling. Then **(4) compose into the full FFT via an over-2 Stockham**
(the existing-engine structure, but with this 56-GFLOPS codelet instead of the ~25 SoA-over-4 one) and measure
×MKL on the scoreboard. **Crux construction FOUND (over-2 + split-radix); 0.79× MKL codelet, headroom to 82.7.**

| over-2 split-radix codelet | GFLOPS | ×MKL | footprint |
|---|---|---|---|
| radix-8 (`aos2sr8.cpp`) | **56** | **0.79×** | 8 ymm (sweet spot) |
| radix-16 (`aos2sr16.cpp`) | 54 | 0.76× | 16 ymm (spills) |
| within-transform split-radix (for comparison) | 46 | 0.65× | 8 ymm |

⚠ **CEILING RE-ASSESSMENT (NOPERM probe on over-2 sr8): 55.2 vs 55.9 = removing all `permute_pd` does NOTHING.**
The over-2 codelet is **NOT shuffle-bound — it's at its structural ceiling ~55, FP/load-store-bound.** Mechanism:
over-2 packs 2 *transforms*/ymm ⇒ radix-R = R ymm but only ~2·5·R·log R flops; over-2 radix-8 = 8 ymm / 240 flops
(15 flops/mem-op). The within-transform packs 2 *complex of one transform*/ymm ⇒ radix-16 = 8 ymm / **320 flops**
(20 flops/mem-op, deeper recursion) ⇒ shuffle-free ceiling **82 > MKL**. So:

| codelet | shuffle-free ceiling | actual | blocker |
|---|---|---|---|
| within-transform radix-16 | **82 (>MKL)** | 48 | `permute2f128` (8/codelet) |
| over-2 split-radix-8 | **55 (<MKL)** | 56 | structural (compute-per-load-store too low) |

**Honest conclusion: over-2 split-radix is the best *practical* codelet (+22%, 56 vs 48 — improves the full FFT)
but its ceiling caps BELOW MKL. The true crush ceiling (82) is in the within-transform structure, still blocked
by `permute2f128`.** ⇒ TWO live paths: **(A)** ship over-2 split-radix as the codelet (full FFT 0.45×→~0.6×, real
but not a crush); **(B)** the crush = within-transform 82-ceiling unblocked = the SPIRAL minimal-`permute2f128`
construction (§5/§8) — still the only >MKL path. The over-2 win does NOT retire the SPIRAL frontier.

## 1c. ⭐ A REAL (but NARROW) MKL-BEAT — batched size-8 (2026-06-14)

`build/aos2sr8.cpp` (over-2 split-radix-8) vs MKL DFTI batched (`NUMBER_OF_TRANSFORMS`), 256 transforms, L1,
1 thread, clean back-to-back (same thermal state):

| batched size | Cerid over-2 SR | MKL batched | ratio |
|---|---|---|---|
| **8** | 48–56 | 44–47 | **1.03–1.27× — CERID WINS (every run)** |
| 16 | 41 (spills, 16 ymm) | 58 | 0.71× — MKL wins |

**Honest verdict: Cerid over-2 split-radix BEATS MKL at batched size-8 (~5% clean, up to 1.27× thermal-favorable),
consistently — a genuine, measured MKL-beat.** BUT it is **narrow**: only at size-8, where the codelet fits 8 ymm
without spilling. At size-16+ the over-2 codelet spills (16 ymm for data alone) AND MKL's batched rate *rises*
with size (47@8 → 58@16, its codelets amortize better), so Cerid loses. This is a real win on a real use case
(batched FFT = spectrograms/conv/multi-channel, v10-e) but **NOT the general crush** — single-transform large-N
and batched size-16+ remain the within-transform/SPIRAL frontier (§1b, §5). First honest >MKL data point; bounded.

## 2. The MKL pin (the moving target)

`build/mkl_pin.cpp`, WSL, 1 thread, median of 13, MKL AVX2 path:

| n | MKL median | spread | GFLOPS |
|---|---|---|---|
| 2²¹ (2M) | ~11.6 ms | 13–19% | ~18.9 |
| 2²² (4M) | ~29 ms | 15–26% | ~15.9 |
| 2²³ (8M) | **~60–62 ms** | 3–15% | ~15.9 |
| 1024 (in `aos_fft1024`) | — | — | **54–71** |

⚠ MKL @8M was variously read as 52 ms (best-of) vs 60–62 ms (median) — a 21% swing that flips parity verdicts.
**Always pin median + spread before a go/no-go.**

## 3. The codelet scoreboard (the core battle — single 32/16-pt transform, L1, contiguous)

GFLOPS, `build/aos_probe.cpp` / `sr16.cpp` / `aos2.cpp`, 14900K AVX2:

| codelet | GFLOPS | footprint | permute2f128 | note |
|---|---|---|---|---|
| AoS within-transform CT radix-8 | 46 | 4 ymm | ~4 | |
| AoS within-transform CT radix-16 | 46.5 | 8 ymm | ~8 | |
| AoS within-transform CT radix-32 | 44 | 16 ymm | ~16 | |
| **AoS within-transform SPLIT-RADIX radix-16** | **48.7** | 8 ymm | ~8 | **+5%, the best practical codelet** |
| AoS split-radix radix-32 | ~46 | 16 ymm | ~16 | +5% (timing soft) |
| AoS bit-rev-load (set_pd gather) | 20 | — | — | reorder = 2× penalty |
| AoS bit-rev-load (efficient 128b) | 21 | — | — | +5% only; reorder not load-instruction-bound |
| SoA-within radix-16 (re/im split) | **37** | 16 ymm | — | **REFUTED** — shuffles relocate to slower ones |
| AoS-over-2-transforms radix-16 (naive) | 24 | 16 ymm | **0** | register-pressure-bound (no inter-codelet ILP) |
| AoS-over-2 radix-16 (trivial-twiddle-skip) | 27 | 16 ymm | **0** | **REFUTED** — still < within-transform 46 |
| `-DNOPERM -DNOS0` (shuffle-free ceiling) | **82.7** | 8 ymm | 0 | **> MKL — proves the crush** |
| **MKL @1024 (full transform)** | **54–71** | — | — | the target (its codelet is ~80+) |

## 4. The full-FFT scoreboard (×MKL, WSL, `run_bench_fft.sh`)

Direct Stockham (committed `f38f2ad` + the wins below), GFLOPS / ×MKL:

| n | Cerid | MKL | ×MKL | regime |
|---|---|---|---|---|
| 1024 | 25.5 | ~57 | 0.45× | L1 |
| 8192 | 14.6 | 49 | **0.30×** | L1→L2 cliff |
| 16384 | 14.0 | 49 | 0.28× | L2 |
| 65536 | 15.0 | 36 | 0.41× | L2 |
| 524288 | 12.5 (four-step) | 29 | 0.43× | DRAM trough |
| 8388608 | 13.25 (four-step + fstw) | 17 | **~0.75×** | DRAM |

**Cerid ≈ PocketFFT everywhere** (both hand-written codelets); **MKL & FFTW sit ~3–4× above** (both *generated*
— Spiral & genfft). We are at/above the best NON-codegen library. The gap IS the genfft/Spiral codegen frontier.

## 5. ⭐ ROOT CAUSE — the footprint-vs-shuffle trilemma

The fast codelet needs **three things at once**, and every known construction sacrifices one:

| construction | small footprint (≤8 ymm → inter-codelet ILP) | few `permute2f128` | split-radix |
|---|---|---|---|
| within-transform split-radix (48) | ✅ | ❌ (~1/ymm) | ✅ |
| over-2-transforms (27) | ❌ (16 ymm) | ✅ (0) | ✗ (naive radix-2) |
| SoA-within (37) | ❌ (16 ymm) | ✗ (relocated) | — |

**Why `permute2f128` is inherent:** packing 2 complex of *one* transform per ymm means the FFT *will* combine
them at some stage; combining two 128-bit halves of a register requires a 128-lane swap (`vperm2f128`,
**port-5-ONLY**, latency 3). The `s0` butterfly `[a,b]→[a+b,a-b]` is already ~minimal (4 ops, 1 swap). Batching
2 ymm's swaps = 2 swaps each (worse). So naive AoS is ~1 swap/ymm = 0.5/element.

**Why over-2 fails despite 0 swaps:** each ymm = `[t0_i, t1_i]` (two transforms, never combined) ⇒ all butterflies
lane-aligned, **zero `permute2f128`** — BUT radix-R needs R ymm (16 for radix-16), too big to fit 2 codelets in
16 ymm ⇒ no inter-codelet overlap ⇒ latency-bound ⇒ 27 < 48. (This is also ≈ the existing SoA-over-4 engine's
~25 rate.) The `cmul permute_pd` is **port-1/5 (2-port, cheaper)** than `permute2f128` — confirmed by the probe
(NOPERM +19% vs NOS0 +38%).

**MKL's edge:** ~0.15 `permute2f128`/element (vs our naive 0.5) in a ≤8-ymm fused codelet. That is the SPIRAL
formula-rewriting result — concentrate the stride-permutations `L` and minimize them. **Not yet cracked here.**

## 6. EVERY DEAD END (measured, do NOT re-attempt)

**Structural / large-N (Parts 1–13):**
- naive four-step / six-step (strided transpose loses to direct prefetched streaming)
- six-step with scalar transpose (8 GB/s) AND with AVX2 register transpose (3.6 GB/s eff)
- cache-oblivious recursion (`rec_fft_soa`) — large-N STRIDED COLLAPSE (−38% to −60% at 131072+)
- four-step floor probe: gather+twiddle-scatter ≈ MKL's whole transform (5n DRAM overhead is the wall)
- SIMD twiddle-scatter in four-step = WASH (floor is strided-mem-bound ≈14 GB/s, not compute)

**Pass-count / radix (Parts 2–4):**
- bigger-radix-over-k > 32 (radix-64 wash; spills; pass-count exhausted at radix-32)
- per-group twiddle-setup optimization (NOSETUP probe = ±3% noise; OoO hides it)
- register-frugal radix-8/16 (radix sweep: fewer passes beats no-spills at L2)
- better op-scheduler for spills (bottleneck-analysis: spills not on critical path; L1 store-forwarded)
- group-ILP / 2-group-interleaving (OoO already auto-pipelines small codelets; ~0%)
- in-place DIT (2.6–3.3× worse — Stockham's no-bit-reversal beats 1× footprint)
- FMA hand-fusing (gcc -O3 already fuses to vfmadd/vfnmadd)

**Mid-band / codelet (Parts 15–24):**
- clang vs gcc (`run_bench_fft_clang.sh`) = WASH (@8192 gcc 14.6 vs clang 12.0) — gap is NOT the compiler
- recursion for the mid-band (−6% @8192, −27% @65536, collapse beyond) — the suboptimal transpose-free form
- bigger radix at L1 (radix sweep: best per-size still 0.30–0.42× MKL; planner already near-optimal)
- SoA-within-transform (37 < 46.5) — cmul-permute savings eaten by relocated (slower) shuffles
- AoS-over-2 / over-k codelet (24–27 < 46) — register-pressure-bound, loses fusion
- efficient 128b bit-rev load (21 vs 20) — reorder is a 2× penalty NOT fixable by load instruction
- trivial-twiddle elimination in AoS (mixed lane pairs `[W^j, W^{j+1}]` rarely both trivial; ≈0 in within-transform)
- permute-removal realizability (NOPERM 53.6 unrealizable in SoA — re-adds elsewhere onto slower ops)

**Process scars:**
- malloc-backed `IAllocator` in a probe to dodge a win-clang-cl debug-CRT link mismatch (user-stopped; violates
  `crd-no-malloc-allocator` + non-representative ~12% bandwidth — see `feedback_no_malloc_in_probes_even_to_dodge_toolchain`)
- cmul micro-probe with feedback chains (register-spilled → latency/spill artifacts, not throughput; discard)
- best-of vs median MKL (21% swing flips verdicts; always pin median+spread)

## 7. THE WINS (banked / shipped)

- **interleave fold** (deinterleave/reinterleave folded into first/last pass) — +1.7× large-N
- **register-pressure list scheduler** in `gen_fft_codelets.py` — flipped radix-8-over-k −17%→win
- **radix-32 + mixed-radix size-aware planner** — +25% @L2
- **lifetime-aware scheduler tiebreak** — +5–11% @L1/L2
- **split-radix (2/4) codelets** (`gen_fft_codelets.py`) — +3–13% small/mid, 22% fewer real-muls
- **four-step RESURRECTION** (NT-store blocked transpose 25.7 GB/s + radix-4/8 nested batched sub-FFT + 1MB
  blocks, n≥2¹⁹) — large-N 0.32–0.47× → **0.43–0.75× MKL**
- **fstw twiddle-factorization** — removed 128 MB/transform streaming DRAM read (⅓ of the four-step read floor)
  via `W_n^a = W_n^{a_hi·M}·W_n^{a_lo}` from two ~√n L2 tables — **+15% @8M, +7% @2–4M** (banked, 4-config validated)
- **prefetch** on the strided gathers — +0–5%, bit-identical
- **AoS within-transform split-radix codelet** (`build/sr16.cpp`/`sr32.cpp`) — **+5%, gated 2.5e-16**, the best
  practical codelet
- Net: **beats PocketFFT/scipy/numpy everywhere, 1e-15, deterministic; large-N ~0.75× MKL; rfft/DCT/NUFFT crush
  their non-MKL peers**

## 8. ⭐ THE PATH TO THE CRUSH (the one remaining build, scoped)

The objective is singular and measured: **a ≤8-ymm-footprint, minimal-`permute2f128`, split-radix AoS codelet**
that runs near the 82.7 shuffle-free ceiling. This is the genfft/SPIRAL construction. Concrete plan:

1. **Extend `gen_fft_codelets.py` to an AoS lane-tracking vectorizer:** build the split-radix op-DAG (exists),
   then assign each complex value to a (ymm, lane) slot and emit AoS SIMD, inserting `permute2f128`/`permute_pd`
   ONLY when a value must cross lanes. The genfft cache-oblivious schedule already minimizes register pressure;
   add **lane-permutation minimization** (the SPIRAL stride-permutation `L` calculus — concentrate & share swaps).
2. **Constraints:** ≤8-ymm live set (so 2 codelets overlap for inter-codelet ILP — the over-2 lesson); radix-16
   or 32 leaf; the cmul `permute_pd` (2-port) is acceptable, the `permute2f128` (port-5) is the budget to minimize
   (target ≤0.15/element, i.e. ≤2–3 for radix-16).
3. **Conjugate-pair split-radix** (Johnson-Frigo `newsplit.pdf`, `ShaoJo07`) for the lowest flop count + simplest
   twiddles (fewer cmul permutes) — stacks on top.
4. **Gate every codelet vs `np.fft` per radix** (numpy model AND emitted-C++ vs brute DFT — the `gen_aos_codelets.py`
   pattern). Measure each on the ×MKL@1024/8192 scoreboard; each step moves the number or is dropped.
5. **Then the composition** — fast SIMD reorder (the four-step reorder is the other wall, ~0.26× when scalar);
   weave the fast codelet into the full FFT and re-measure ×MKL.

**Honest effort:** days-to-weeks (genfft/SPIRAL spent years on the general case; we need one good leaf-codelet
family + scheduler, de-risked because the objective is singular and the ceiling is proven > MKL).

## 9. Seeds & tooling (resume here)
- Codelet seeds: `build/sr16.cpp` (split-radix radix-16 AoS, +5%, gated), `build/sr32.cpp` (radix-32),
  `build/aos_probe.cpp` (the NOPERM/NOS0 shuffle-cost probes), `build/aos2.cpp` (over-2, dead).
- Generator: `scripts/gen_aos_codelets.py` (AoS radix-8/16/32 fwd+inv+contiguous, numpy+emitted-C++ gated).
- Benches: `runtime/examples/bench_fft_vs_refs.cpp` + `scripts/run_bench_fft.sh` (gcc) / `run_bench_fft_clang.sh`.
- `build/mkl_pin.cpp` (median+spread), `build/gather_probe.cpp` (NT-tile 25.7 GB/s), `build/aos_fft1024.cpp`
  (full-FFT four-step, 0.26×, reorder-bound).
- Engine: `engine/hesap-fft/include/crd/hesap/fft/fft.hpp` (Stockham + four-step + fstw), `detail/codelets.hpp`
  (split-radix), `detail/aos_codelets.hpp` (AoS, unwired).

## 10. References (`docs/books/`)
- ⭐ `hpkfft-paper-2023.pdf` — beats MKL 1.6× AVX2 fp64 in AoS C++ (the existence proof + API/twiddle-caching design).
- `fftnew/2602.23525v1.pdf` (Frigo-Johnson "Implementing FFTs in Practice") — cache complexity (breadth-first
  Stockham = Θ(n log₂n) "pessimal"; blocked/recursive = Θ(n log_Z n) optimal); FFTW mid-band = depth-first
  bounded-radix-32 + size-32/64 codelets + the genfft scheduler.
- `fftnew/newsplit.pdf` (Johnson-Frigo modified/conjugate-pair split-radix) — lowest known power-of-2 flop count.
- `fftnew/spmag09.pdf`, `ieeeproceedings-pueschelmouraetal-feb05.pdf` (SPIRAL) — the formula calculus: stride
  permutation `L^mn_m`, tensor decomposition, short-vector vectorization, vector-radix.
- `fftnew/europar03.pdf` (Franchetti, SSE2 2-way auto-vectorizing FFT compiler) — dated (2-way) but the
  straight-line-SIMD-vectorization precedent.
- `pldi99.pdf` (genfft scheduler internals), `ShaoJo07-preprint.pdf` (DCT/DST conjugate-pair flop counts).

## 11. THE GENFFT ENGINE — committed build plan (user-ratified 2026-06-15)

**Decision (user, advisor-gated):** 1D-complex is at honest MKL parity; the 1.42x crush is a multi-week minimal-pass
genfft engine, not a probe. **User chose: build the engine.** Scoped as a real multi-session project below.

**The thesis (why this beats parity, measured-grounded):** parity comes from paying MKL-equivalent twiddle (~24%)
+ reorder (~32%) overhead per level. The crush = drive that overhead toward zero by (a) **register-residence across
radix stages** — a leaf does *several* radix-8 stages on one register-load before one store (FFTW codelet model),
slashing memory passes; (b) **transpose VANISHES, not materialized** — Stockham self-sort / strided codelet I/O so
the per-level reorder is fused into the next codelet's load address, never a separate buffer pass (FUSEDT proved a
materialized transpose pass is *worse* than scalar); (c) **fewer twiddle muls** via conjugate-pair split-radix.

**Proven foundations to build ON (all gated, measured):** over-2 split-radix-8 leaf = 56 GFLOPS, 1.3-1.5x MKL's
per-flop rate, ZERO `permute2f128` (`build/aos2sr8.cpp`); the N=64 atom four-step = parity, gated 2.87e-16
(`build/fftcrush_n64.cpp`); shuffle-free FP ceiling = 82 > MKL 71 (`build/sr16.cpp` NOPERM+NOS0).

**Slices (each gated vs brute/FFT + measured vs MKL same-harness bracketed; STOP a slice that doesn't move xMKL):**
- **E1 — strided over-2 codelet kernel. ✅ DONE 2026-06-15 (`build/e1_strided_leaf.cpp`, gate 2.87e-16).**
  Extracted `fftcrush_n64`'s inlined leaf into two reusable strided twiddle-codelets — `sr8_tw_contig`
  (contiguous-pair load, stage A, with per-output twiddle) + `sr8_tw_gather` (strided gather load = the
  transpose fused into the load, stage B). `sr8` math byte-unchanged ⇒ the brute-DFT gate guards the refactor.
  N=64 re-composed entirely from the leaves: **gate machine-eps, N=64 = ~0.99–1.02× MKL (parity preserved,
  no regression)** — bracketed Cerid–MKL–Cerid, core-pinned. E1 is foundation, not the lever; it gives E3 a
  composable building block. NEXT = E3 (register-resident multi-stage assembly for N=512/1024 — the actual
  lever; E2 transpose-free Stockham already measured-dead at 0.57×, see below).
- **E2 — Stockham over-2 driver (self-sorting, transpose-free). BUILT + MEASURED 2026-06-15 (`build/fftcrush_stockham.cpp`,
  gated 1.98e-15): 0.57x MKL (Cerid 29-31 vs MKL 53) — WORSE than the four-step's 0.73x.** Decisive finding: going
  transpose-free KILLS the reorder but replaces it with PASS COUNT — radix-8 Stockham does 3 full-array memory
  passes for N=512 (each pass reads+writes the whole array), and it is memory-pass-bound (Frigo's "breadth-first
  Stockham = pessimal cache, Theta(n log_2 n)"). So the two canonical structures lose at COMPLEMENTARY walls:
  four-step = reorder-bound (0.92x ceiling), Stockham = pass-bound (0.57x). **Neither simple structure beats MKL;
  the win REQUIRES E3 (cache-blocked multi-stage = few passes AND fused reorder) — now empirically confirmed
  necessary, not assumed.** The register file (16 ymm) caps a fully-register-resident leaf at ~size-16, so few
  passes for N>=512 needs CACHE-blocking (L1-resident blocks carried through multiple radix stages), i.e. FFTW's
  cache-oblivious recursion — the genuine multi-week core. E2 is a measured dead-end as a standalone driver but the
  decisive diagnostic that pins the engine on E3.
  - **Pass-count sweep (2026-06-15, same file):** N=64 (2 passes) 0.62x, N=512 (3 passes) 0.53x, N=4096 (4 passes)
    0.61x — **Cerid Stockham is a FLAT ~28-31 GFLOPS regardless of N.** The punchline: the N=64 *four-step atom* =
    56 (parity) but the N=64 *Stockham* = 30 — **same leaf, same N; the only difference is the Stockham writes every
    stage to L1, the atom keeps stages in registers.** That halves the rate. => **the lever is REGISTER-RESIDENCE /
    minimal L1 traffic, NOT transpose-elimination** (Stockham eliminated the transpose and got *worse*). The
    four-step direction (register-resident leaves, 0.73-0.92x) is correct; its ceiling is the materialized reorder.
    The crush = four-step with register-resident leaves AND a fused (non-materialized) reorder = E3 genfft codegen.
    **Entire simple-structure space now built+measured across N: nothing simple beats MKL; parity is the ceiling.**
- **E3 — register-resident multi-stage leaf (the pass-count win).** Fuse 2-3 radix-8 stages in registers per memory
  round-trip (size-64/512 codelet, non-spilling via the genfft scheduler in `scripts/gen_fft_codelets.py _schedule`).
  The hpkfft "minimal-pass" lever — fewer whole-array touches. Target: N=512/1024 > 1.0x MKL.
- **E4 — conjugate-pair split-radix leaves (`newsplit`/`ShaoJo07`).** Fewer non-trivial twiddle muls => fewer
  `permute_pd` => closes 56 -> toward 82. Target: lift every size another ~5-10%.
- **E5 — planner + wire into `crd-hesap-fft` + oracle gate + bench vs MKL across N=64..8192.** Geomean > MKL = crush.

**Hard rules carried in:** measure Cerid bracketed before AND after MKL same-binary (14900K throttle bit us);
seeds live in `build/` not `/tmp` (WSL clears it); NO malloc allocators in probes (crd TLSF); honest scoreboard —
ranges that overlap = parity, not a win. The make-or-break is **E2** (transpose-free Stockham); if E2 cannot clear
the N=512 reorder wall, the engine cannot beat MKL and we report that honestly and bank parity.

## 12. THE N>=512 LARGE-N TUNING CAMPAIGN (charter — opened 2026-06-15, user-ratified)

**Verdict that opens this as a CAMPAIGN, not a session lever (advisor-gated):** N=512 had FOUR structures
tried + a root-cause profile. Earlier walls fell to *untried structures*; this one is *tried-and-profiled*.
The tell: **FFTW (genfft gold standard) only reaches 0.85 at N=512 on this box** — we BEAT FFTW at 128/256, so
"parity at 512" = out-engineering the reference implementation at its single hardest size = sustained tuning,
not a swing. (Also: 0.68-0.75 are inside the +-9% 14900K throttle band — measurement must be tightened first.)

### Measured baseline (the start line)
- MKL N=512 = 1.00 (the bar). FFTW N=512 = 0.85 (best open-source). Our best four-step = ~0.72-0.75 (noisy).
- Profile (`build/four512b.cpp` -DONLYA/-DONLYB): Stage A (size-32 cols + transpose + twiddle) = 0.94 of MKL's
  ENTIRE time alone; Stage B = 0.48. The 2-stage four-step does ~1.4x MKL's work (the separate transpose pass).

### Structures TRIED + MEASURED-DEAD (do NOT re-try cold — only with a genuinely new idea)
- over-2 Stockham (transpose-free): 0.57 (pass-bound — transpose-free ALONE is WORSE, not the lever).
- radix-2 composition (2x sub + merge): 0.58 (Eb/Ob staging buffers, cannot fit registers).
- four-step 32x16 spill over-2 sr32: 0.75 (size-32 over-2 spills 32 ymm).
- four-step 32x16 no-spill per-column lane-split: 0.716 (build/four512c.cpp, gate 9.9e-16). The clean no-spill
  size-32 (crush32's lane-split, 16 ymm) lands EXACTLY on the over-2 0.72. Mechanism: lane-split halves load/store
  WIDTH (32x128-bit scatter-loads + 32x128-bit scatter-stores/col vs over-2's 256-bit contiguous) => no-spill +
  2x mem-op-count == spill + 1x mem-op-count. They cancel.
- ⭐ THIS PROBE PRE-REFUTES LEVER 3 (scheduled size-32 codelet): the scheduled-codelet win is ALSO spill reduction,
  and this measured spill reduction == NEUTRAL here (binding constraint = mem-op width/traffic, NOT register spill).
  A 20-min probe saved the multi-day genfft-size-32 build. Do NOT build lever 3 cold; it lands at 0.72.
- => the four-step IS the best structure; the gap is the transpose-pass MEMORY overhead (~1.13x flops vs ~1.4x
  time), NOT spill. Spill reduction is measured-neutral. Transpose elimination (Stockham) is measured-WORSE (0.57).
  6 measured 2-level variants ALL land 0.72 +- noise => 0.72 is the robust 2-level frontier on this AVX2 box.
  MKL = integrated multi-level fused-codelet engine, fewer memory passes, deep-tuned over years (FFTW only 0.85).

### The actual campaign levers (sustained, profile-driven, fresh-context, clock-pinned)
1. MEASUREMENT FIRST — clock-pin the rig (BIOS turbo off / fixed freq) or tight-interleaved 3-way (ours/FFTW/MKL)
   bursts + confidence intervals. The +-9% throttle noise currently HIDES every <10% lever.
2. Reduce the transpose-pass memory overhead (the profiled root) — fuse the transpose into the codelet strided I/O
   (never a separate L1 round-trip; FFTW twiddle-codelet model); blocked-tile transpose; measure L1/DRAM traffic.
3. ~~Bigger register-fitting sub-codelets — genfft-scheduled size-32~~ REFUTED 2026-06-15 by the four512c lane-split
   probe (spill reduction is measured-neutral at N=512; the binding constraint is mem-op width/traffic). Do NOT build.
4. Factorization + L1-block sweep — 32x16 vs 16x32 vs 8x64 vs 3-level 8x8x8.
5. Prefetch-distance sweep — `_mm_prefetch` on the strided gathers, tuned to the cache hierarchy.
6. Wire the WON codegen band (N<=256) into a generator four-step driver so large-N reuses the crushing sub-codelets.

### Honest target
Match MKL (parity) at N=512/1024; beating FFTW's 0.85 is the milestone, MKL's 1.0 the goal. Deepest FFT-opt tier
(FFTW/MKL spent years here); progress is single-digit % per lever => MEASUREMENT (lever 1) gates everything.
The WON band (crush <=32, beat FFTW <=256, the codegen generator) is durable + the foundation; this is the large-N
chapter. Seeds: build/four256.cpp (0.92, beats FFTW), four512.cpp + four512b.cpp (0.72-0.75 + profile probes).

## 13. ⭐ FRESH-SESSION BRIEF — the genfft-AoS register-scheduling codegen (THE one remaining MKL-parity lever)

**Read `project_v10_fft_plan.md` Part 24 (the ⛔⛔ READ-FIRST banner) FIRST. The cheap-lever space is exhausted
(~13 measured dead-ends). The ONLY path to 1D MKL parity is this codegen. Start here, execute — do NOT re-derive.**

### The one-paragraph why
Cerid's AoS within-transform codelet is THE layout (SoA-within refuted; SoA-over-k caps radix-8 @25 GFLOPS). The
isolated AoS radix-32 codelet runs **43 GFLOPS = 0.665× MKL@1024's live 64.4** on this box. The gap to parity is
NOT structure, NOT spill, NOT memory — it is **codelet schedule quality**: `gen_aos_codelets.py` emits UNSCHEDULED
structural Cooley-Tukey stages, while MKL/genfft emit register-scheduled split-radix. The scheduler that already
flipped radix-8 +22% in the SoA generator is sitting one file over, unused by the AoS path. Port it. Target 43→~71.

### The de-risked brick loop (already built + verified this session)
```
edit scripts/gen_aos_codelets.py  ->  python regenerates build/aos_candidate.hpp
  ->  (WSL) g++ -O3 -march=native -funroll-loops -I/usr/include/mkl aos_codelet_bench.cpp -o aos_codelet_bench \
            -Wl,--no-as-needed -lmkl_intel_lp64 -lmkl_sequential -lmkl_core -lpthread -lm -ldl  &&  ./aos_codelet_bench
  ->  read:  GATE (vs brute DFT, 1e-12)  +  "ISOLATED x GFLOPS / MKL@1024 live y / ratio z"  +  MOVED✓/REVERT✗
```
- Harness: `build/aos_codelet_bench.cpp` (gate + isolated GFLOPS + tight-interleaved live MKL@1024 = throttle-robust).
- Contract: `build/aos_candidate.hpp` defines `kCodeletN`, `aos_codelet_natural(in,out)` [gate], `aos_codelet_contig(buf)`
  [timing]. Default = radix-32 CT (43 GFLOPS baseline, GATE 2.9e-16 PASS). The generator REPLACES this file.
- Each brick MUST move the isolated GFLOPS or be reverted. Throttle cancels in the live ratio (no BIOS needed).

### Exact code surfaces (named, not vague)
- **The scheduler to port** = `scripts/gen_fft_codelets.py:190` `def _schedule(nodes, outs)` — register-pressure list
  scheduler, key `(1-dying, is_load, fanout, -i)` @L215 (loads-late / kill-live-ASAP / short-lived-first). Flipped
  radix-8 from −17% spill to a WIN. This is the BIG 43→~71 lever.
- **The split-radix DAG** = `scripts/gen_fft_codelets.py:148` `def fft(xs)` (SPLIT-RADIX 2/4, ~33% fewer real muls).
- **The AoS emitter to upgrade** = `scripts/gen_aos_codelets.py:76` `def emit_codelet(b, inverse, contig)` — currently
  emits hardcoded structural CT stage loops (L98–126), NO symbolic DAG, never schedules. THIS is the floor cause.
- **The AoS numpy gate** = `gen_aos_codelets.py:30` `aos_model` + `:60` `check_model` (keep; it self-checks pre-emit).

### Brick sequence (each measured on the harness; revert if not MOVED)
1. ⛔ **Split-radix-AoS — MEASURED-REVERT 2026-06-15 (do NOT re-try).** Lifted `build/sr32.cpp`'s split-radix-32 AoS
   into the candidate (GATE 3.0e-16 PASS) → **38.5 GFLOPS, −10.5% vs CT 43.** The remembered "+5%" (sr16/sr32) was a
   NO-STORE-harness artifact (those probes timed compute-only: register-resident input, `sink+=out[0]`, never stored
   the 16 ymm). With the realistic 16-load+16-store regime the codelet is **load/store-port-bound, not FP-bound** ⇒
   split-radix's fewer muls don't help and its extra structure hurts. **flop-reduction is the WRONG axis here.**
2. ⛔ **Multi-stage FUSION (store/load amortization) — MEASURED-WRONG-AXIS 2026-06-15 (build/aos_fusion_probe.cpp).**
   Decomposed the 43-GFLOPS ceiling: FULL 43.3 · 1-STORE 40.0 (15 fewer stores ⇒ −7.5%, SLOWER) · 1-LOAD 38.3
   (−11.4%, SLOWER) · NOPERM 51.2 (+18%). ⇒ the codelet is NOT load/store-bound (it's fully L1-pipelined; removing
   mem-ops only adds dependency work). The ONE real leaf cost = the **port-5 PERMUTES** in the AoS cmul (+18% if
   removed). But even permute-free the leaf caps **51.2 = 0.79× MKL@1024** ⇒ NO isolated-codelet optimization reaches
   MKL. fusion-of-the-leaf is dead; `_schedule` has nothing to grip (no spill, not mem-bound).
3. ⭐⭐ **CORRECTED PREMISE (the whole "climb the isolated leaf 43→71" framing was a category error):** the isolated
   32-pt codelet rate (43) is NOT comparable to MKL@1024 (64.8) — a 32-pt transform inherently sustains a LOWER
   GFLOPS rate than a 1024 (less overhead amortization). At MATCHED N=32 Cerid's codelet already **CRUSHES MKL 1.16×**
   (prior sessions). The leaf is FINE. MKL's 64.8@1024 = its LARGE-N STRUCTURE, not leaf quality (confirmed: leaf
   caps 51 < 64.8). ⇒ the ONLY open lever = the **large-N AoS-leaf ASSEMBLY** (Stockham-AoS or recursive-AoS keeping
   sub-results register-resident across radix stages — NOT four-step, Part 19 = 0.26× DEAD), which is the genfft/
   Spiral structural codegen = person-WEEKS. The isolated-codelet harness is good for killing leaf-level false leads
   (it killed bricks 1 & 2) but is NOT the yardstick for the assembly — that needs a full-transform-vs-MKL@matched-N
   harness (the existing run_bench_fft.sh / bench_fft_vs_refs.cpp).
4. THE actual remaining build = large-N AoS-leaf Stockham assembly, measured by the FULL-transform bench vs MKL@same-N
   (NOT the isolated-codelet harness). Person-weeks, fresh-context. Bricks 1 & 2 proved the leaf is not the lever.

## 14. ⭐ STRUCTURAL-BUILD SPEC — the blocked-tile NT transpose in the four-step (large-N parity lever, user-chosen 2026-06-15)

**The leaf is solved (§13). The entire large-N MKL gap is the four-step's TRANSPOSE/intermediate efficiency — NOT
the codelet, NOT an AoS layout swap (AoS-four-step = 0.26× DEAD, Part 19; SoA-four-step = 0.72 is the best assembly).**

### The lever (the ONE genuinely-untested structural piece, Part 13)
The SoA four-step (0.72) spends ~54% of large-N time in the FLOOR = strided column gather/twiddle-scatter at ~14 GB/s.
`transbw.cpp` proved a **blocked-tile (B=64) in-register transpose + contiguous NT-store = 25.7 GB/s** in isolation
(3× the strided rate, ABOVE MKL's ~20). FLOOR ARITHMETIC (Part 12): floor 48→~15 ms ⇒ 8M ~66 ms ≈ **0.94× = parity**.
OPEN QUESTION (Part 13, still genuinely open): does 25.7 survive woven into the four-step's gather + sub-FFT SCRATCH
read, or is it capped by the strided scratch access? Answering it IS the build (a cheap probe cannot — see warning).

### ⛔ Flawed-probe warning (2026-06-15)
`build/four_step_mem_probe.cpp` tried to cheap-probe this and FAILED representatively: it did element-by-element NT
stores to the strided dst (scattered writes = WC-buffer thrash) → fake 3.9 GB/s "cap". The REAL method needs the
**in-register BxB SIMD transpose THEN contiguous NT-store per output row** (transbw.cpp). Even the "cheap" structural
probe needs the careful in-register transpose ⇒ this is genuinely the delicate fresh-context build, not a quick test.

### The build (fresh-context, oracle-gated every step)
- Target: `fft.hpp` `execute_four_step` (~L870–950), `kBudget=1MB` (~L953), kFourStepMin=2¹⁹ (~L777).
- Replace the strided column gather/scatter with: read a B×B tile (rows contiguous) → **in-register SIMD transpose**
  (`transpose_simd_c64` exists per Part 12) → contiguous **NT-store** (`_mm_stream_pd` + `_mm_sfence`; 32B-align). The
  sub-FFT then reads the transposed scratch UNIT-STRIDE (this is the part to measure — does it stay near 25.7?).
- ⚠ DELICATE: partial blocks (n2 not div-B), NT 32B alignment, the in-tile transpose. Gate vs the brute-DFT oracle
  AND the {1..16} determinism moat at EACH step (NT stores are byte-identical for finite data ⇒ moat-safe).
- Measure with the FULL-transform bench `scripts/run_bench_fft.sh` (GFLOPS vs MKL@same-N), NOT the isolated harness.
- If the woven rate holds near 25.7 → 0.72→~0.9+ at large-N = the crush. If it caps (strided scratch read binds) →
  Part 13's wall is REAL and the remainder is the full genfft/Spiral plan. Either way it's a measured, honest answer.

### Targets / honest frame
Isolated codelet 43 → ~71 GFLOPS (live MKL@1024 ≈ 64–71 thermal-dependent). hpkfft-paper-2023 PROVES 1.6× MKL in AoS
C++ on this exact AVX2 class ⇒ REACHABLE, not a ceiling — but it is person-weeks of codegen, fresh-context only.
Seeds: `build/aos_codelet_bench.cpp`, `build/aos_candidate.hpp`, `build/aos_probe.cpp` (45 proof), `build/sr16.cpp`
(split-radix +5% brick), `build/soa16.cpp` (SoA-within refutation), `build/aos_fft1024.cpp` (the 0.26×-assembly wall).

## 15. ⭐ ENGINE BASELINE BOARD — the start line for E3 (measured 2026-06-15, `build/engine_regime_sweep.cpp`)

The genfft engine resume started with a SINGLE bounded baseline sweep (advisor-gated: measure where the SHIPPED
`FftPlan` bleeds, in BOTH regimes, BEFORE building — the "atoms hit parity" numbers were batched-×16 vs the
engine's single-transform, the same regime confound that bit N=8). Bracketed Cerid–ref–Cerid, core-pinned,
median of 30, all gates machine-eps:

| N | single | batched(×16) |  | N | single | batched(×16) |
|---|---|---|---|---|---|---|
| 64  | 0.53× | 0.35× | | 4096  | **0.27×** | 0.52× |
| 128 | 0.38× | 0.30× | | 8192  | **0.25×** | 0.51× |
| 256 | 0.46× | 0.32× | | 16384 | **0.28×** | 0.55× |
| 512 | 0.37× | 0.32× | | 65536 | 0.38× | 0.57× |
| 1024| 0.39× | 0.32× | | | | |

**Findings (the honest map that replaces "cycling 0.3–0.6×"):**
- The shipped engine is **sub-parity at EVERY size in BOTH regimes** (0.25–0.57×). Nowhere near parity yet.
- **Single-transform worst = the L2 band 4096–16384 (0.25–0.28×).** This is the E3 register-residence problem
  (the SoA Stockham path re-streams the whole array each pass; the lever is fusing radix stages register-resident
  so fewer whole-array passes). Atoms (N≤256) CANNOT touch this band.
- **Batched weakest = small-N 64–1024 (0.30–0.35×);** mid/large batched is relatively better (0.51–0.57×, the
  over-batch SIMD width helps). Small-N batched is exactly where the AoS over-2 atom wins (N=8 already shipped at
  1.49× via `small_n_batched8_f64`); extending to 16/32/64/128/256 batched is the measurement-justified parallel
  win, blocked on the strided-gather → block-transpose-to-contiguous assembly (§14 class).

**THE STRUCTURAL COMMITMENT (advisor-gated, not another probe variant):** E3 = register-resident multi-stage
assembly for the single-transform mid-band (the worst, 0.25× L2 cliff). Person-weeks, fresh-context. E1 (the
composable strided leaf, §11) is its foundation. The batched small-N atom-wiring is the tractable side win.
Honest ceiling reminder: on THIS AVX2 14900K even the full engine targets PARITY (the 1.6× proof was AVX-512).

### E3 brick #1 + ceiling diagnostic — N=4096 (`build/e3_n4096.cpp`, gate 5.33e-16, measured 2026-06-15)
Built a textbook 64×64 four-step whose size-64 sub-FFT is the validated E1 register-resident leaf (fft64_e1),
single-transform, bracketed vs reference. **Result = 0.306× — barely above the engine's 0.27×.** Then the decisive
ceiling probe (`-DNOTRANS`, both strided transposes removed → wrong result, isolates the transpose cost):

| N=4096 variant | ratio vs reference |
|---|---|
| real four-step (transposes ON) | **0.306×** |
| CEILING (transposes REMOVED, free) | **0.472×** |
| reference | 1.000× (60 GFLOPS) |

⭐ **DECISIVE, and it REFUTES the §14 transpose thesis FOR THE MID-BAND:** even a *zero-cost* transpose caps at
**0.47×**. So at N=4096 the transpose is ~35% of the time but is NOT the lever — the binding constraint is the
**fused-assembly overhead**: the separate outer-twiddle pass + the intermediate B buffer (store 4096 + reload) +
the inner fft64's own sA round-trips. The size-64 leaf is ~56 GFLOPS in isolation (parity-class) but the assembly
around it halves it to 28 even transpose-free — too many whole-array passes per transform. (§14's blocked-tile NT
transpose remains the LARGE-N lever, where the transpose is ~54% of time; it is NOT the mid-band lever.)

### E3 brick #2 — twiddle FUSED into the leaf store (`build/e3_n4096_fused.cpp`, gate 5.33e-16, 2026-06-15)
Folded the outer four-step twiddle into the size-64 leaf's output store (kills the separate `acol` intermediate
+ the standalone twiddle pass; the leaf scatters already-twiddled outputs straight to B). **Result = 0.31× —
NO movement vs brick #1's 0.30×.** Mechanism: the saved twiddle pass was offset exactly by the strided
per-lane scatter the fold requires (`_mm_store_pd` ×2 vs the old contiguous `acol` write). ⇒ the twiddle pass
is NOT the bottleneck either.

### ⭐⭐ DECISIVE SCOPING (2026-06-15) — the four-step is the WRONG structure for the mid-band
Three measured bricks + the ceiling probe converge:

| N=4096 | ratio | rules out |
|---|---|---|
| four-step, register-resident leaves | 0.30× | leaf alone |
| + twiddle fused | 0.31× | the twiddle pass |
| free-transpose CEILING | 0.47× | the transpose (even free it caps 0.47×) |
| reference | 1.00× | |

The four-step is **double-capped** at the mid-band: transpose-bound below 0.47×, AND the leaf-ASSEMBLY (the
column/row copies + the inner size-64's own sA round-trips across 128 leaf calls) caps it at 0.47× even with a
free transpose. The size-64 leaf is 56 GFLOPS in ISOLATION but the assembly halves it. No four-step fusion
crosses 0.47×. **STOP building four-step variants for the mid-band — that is cycling.**

⇒ **REDIRECT (the honest, measured structural commitment):** mid-band parity needs the **recursive
bounded-radix fused-codelet engine** — depth-first recursion (FFTW/genfft model), register-scheduled leaves
emitted by `gen_fft_codelets.py`'s `_schedule` ported to the AoS path (§13), minimal whole-array passes, the
twiddle as a t-codelet argument. NOT the four-step (that stays the LARGE-N tool, §14, where the transpose IS
the lever). This is the genuine person-weeks core; it is a code GENERATOR (the elite "system"), not hand-tuned
per-size atoms. Bricks #1/#2 + the ceiling are the diagnostics that earned this redirect (each ruled a lever
out); the four-step seeds are `build/e3_n4096*.cpp`.

## 16. ⭐⭐⭐ THE RESEARCH-GROUNDED STRATEGIC REDIRECT — single-transform mid-band is a LOSS even for the best;
##     the BATCHED regime is the winnable crush (2026-06-15, read the papers)

**Read `docs/books/hpkfft-paper-2023.pdf` (the existence proof) + `fftnew/2602.23525v1.pdf` (Frigo-Johnson,
"FFTs in Practice").** The decisive facts:

- ⭐ **hpkfft — the best-in-class modern-C++ FFT that beats the vendor ~40% (AVX2 geomean) — is itself SLOWER
  than the vendor at SINGLE-transform 4096 (ratio 0.82×).** Their own words: "we are significantly slower on
  the 4096-point FFT… This test case can essentially be made to fit in the 48 KiB L1… **we have not attempted to
  write a freestanding kernel for this size.**" ⇒ the single-transform L1-resident mid-band is where the VENDOR's
  hand-written freestanding L1 kernels win, and even the state-of-the-art C++ library concedes it. **Chasing
  single-transform mid-band PARITY is fighting for the one size the best library in the world loses.**
- ⭐ **hpkfft WINS decisively in the BATCHED regime: within 5% at 4096 and +20% over the vendor across all sizes
  with a batch of 7.** Mechanism (their words): "the twiddle table and scratch memory, which are reused for each
  transform in the batch, fit easily [in cache]." The batch amortizes the per-transform overhead the single
  transform can't hide. **The batched regime is the achievable parity-AND-crush target.**
- Frigo-Johnson: textbook radix-2 breadth-first is "pessimal" cache-oblivious; the levers are LARGE radices (not
  radix-2) + depth-first recursion. (Confirms the single-transform structural direction — but see the hpkfft
  caveat: even with all of it, single mid-band is marginal.)

**Probe (`build/batched_radix.cpp`, gated): radix-4 over-batch (0.32×) beats radix-2 (0.12×) — pass-count
confirmed — but BOTH lose to the engine's `execute_batched`, which is ALREADY radix-8 (0.52×).** So the radix
lever is already spent in the engine. The engine's batched remaining gap to the reference is NOT pass-count:
it is **(a) the explicit bit-reversal pass** (`fft.hpp` ~L452-464, a full data-movement pass the reference's
Stockham autosort avoids) **and (b) butterfly/codelet tuning.**

⇒ **THE BATCHED-CRUSH ROADMAP (achievable, research-backed, the user's "best scenario" lives HERE):**
1. **Convert `execute_batched` to radix-8 STOCKHAM autosort** (kill the bit-reversal pass; the single-transform
   `execute()` already proves Stockham > bit-reversed-DIT). Expected ~15-25%.
2. **Extend the `small_n` over-2 atom (N=8 already CRUSHES batched at 1.49×) to N=16/32/64** via the
   block-transpose-to-contiguous assembly (§14 class) — the weak band on the board (0.30-0.35×) is exactly where
   this construction wins. This is the most direct crush (the N=8 atom is the proof).
3. Tuned radix-8 batched butterflies (FMA, fewer shuffles) + twiddle-table cache residency across the batch.
**Honest target: batched parity→crush is REACHABLE (hpkfft proves +20% over vendor); single-transform mid-band
parity is NOT the right fight.** The N=8 batched 1.49× (shipped) is the first proof point of this roadmap.

### ⭐⭐ ROADMAP ITEM #2 SHIPPED — N=16 batched crush (2026-06-15)
Extended the proven N=8 over-2 atom to **N=16** (`small_n_batched16_f64` in `small_n_codelets.hpp`, driven by
the existing `dft16_over2` register-resident leaf; `sig16` split-radix input order). Wired into `execute_batched`
behind an even-batch + cache-resident guard (`n·b ≤ 2048` complex; beyond that the strided over-2 gather thrashes
L1 → SoA fallback). Probe (`build/batched16_probe.cpp`, gated 1.5e-16): **b=16..192 = 1.40–1.53× over the
reference, b=256 = 0.25× (the L1 cliff → the guard).** Confirmed on the SHIPPED `FftPlan::execute_batched`
dispatch (`build/engine_regime_sweep.cpp`): **N=16 batched b=16 = 1.36× (was 0.30× pre-wiring — a 4.5× jump and
a crush)**; N=8 batched b=16 = 2.25×. Correctness: the existing even-batch test (m=16, batch=8, fwd+inv) routes
through the new path; fft ctest 17/17. **Two shipped, gated, user-accessible batched crushes now: N=8 + N=16.**
NEXT roadmap steps: N=32/64 batched (needs the block-transpose-to-contiguous to beat the conflict-stride wall,
§14 class) → larger batched mid-band (Stockham-no-bitrev, roadmap #1) → the +20%-over-vendor batched engine.

### N=32/64 batched — GRINDED, improved ~2× but NOT crushed (2026-06-15; do NOT ship as-is)
Two constructions measured (`build/batched32_probe.cpp`, `build/batched_4step.cpp`, all gated 2-4e-16):
- **N=32 split-radix-32 over-2** (32 ymm state, SPILLS): 0.70-0.78× (b≤96). The spill caps it.
- **N=32 four-step 8×4 over batch** (register-resident size-8 leaf, L1 scratch, no spill): **0.86× best (b=16/32),
  0.76-0.79× (b=64/96)** — the register-resident leaf beats the spill, but the four-step assembly overhead caps
  it BELOW parity (same wall as the single-transform four-step's 0.47× ceiling, §15).
- **N=64 four-step 8×8 over batch**: **0.67× best (b=16), 0.56-0.63×** — ~2× the engine's 0.31× but not parity.
- ⚠ **CONFLICT-STRIDE CLIFFS at power-of-2 b** (the common case): N=32 cliffs at b=128 (0.33×), N=64 at b=64/128
  (0.26-0.30×) — the strided over-2 gather (element-major, stride b) maps too many loads to too few cache sets
  once N is large (N=16 survives to b=192; N=32→b=96; N=64→b=32). At those b the four-step is WORSE than the
  SoA engine ⇒ **NOT robustly shippable** (would regress common power-of-2 batches).

⇒ **N=16 is the natural crush ceiling of the over-2 atom** (largest that fits 16 ymm + survives the strided
gather). **Honest banked state: N=8 + N=16 batched are shipped crushes; N=32/64 are mapped
(~2× improvement available but conflict-cliff-fragile).** Seeds: `build/batched*.cpp`.

### ⛔ MEASURED-DEAD — cache-blocked + block-transpose-to-contiguous (2026-06-15, `build/batched_blocked.cpp`)
Built the block-transpose-to-contiguous as a copy-based cache-blocking: tile the batch (W transforms, L1-fit),
memcpy block-transpose element-major → contiguous tile, over-2 radix-2 Stockham on the tile (L1-resident), copy
back. **MEASURED 0.10–0.24× across N=32..1024 — WORSE than the engine's in-place radix-8 (0.39–0.57×).** The
2-pass transpose copy + radix-2 pass count + tiny W (=2 at N≥512) cost more than the cache-blocking saves. The
mid-band batched array often already fits L2, so the engine's in-place passes are L2-resident (not the
bottleneck the block-transpose assumed). ⇒ **the copy-based block-transpose shortcut is a dead-end; do not
re-try.** (An in-register fused tiled transpose might differ, but it is the full genfft codegen, not a brick.)

### ⭐⭐⭐ CONVERGED VERDICT — batched N≥32 parity is the full tuned engine, not a brick (2026-06-15)
Five distinct constructions measured for N≥32 batched (split-radix over-2 0.78× · four-step over batch
0.67–0.86× + power-of-2 conflict cliffs · cache-blocked block-transpose 0.10–0.24× DEAD · engine radix-8
in-place 0.39–0.57× = the floor · over-2 atom spills past N=16). **NONE reach parity; the block-transpose
shortcut is worse.** The over-2 atom CRUSHES through N=16 (shipped); **N≥32 batched parity = the hpkfft-class
codelet-generator engine** (register-scheduled multi-radix codelets composed cache-obliviously — the +20%
batched win is years of genfft engineering, NOT a session brick). The shortcut space is now measured-empty.
**Banked + honest: N=8/N=16 batched crush shipped; N≥32 is the full-engine frontier. Do not re-throw mid-band
batched constructions — that is the cycling to avoid.**
