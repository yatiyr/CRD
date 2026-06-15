# Research Dossier — Beating MKL on 1D Complex FFT (v10 `crd-hesap-fft`)

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
- **E1 — strided over-2 codelet kernel.** Generalize `sr8`/`fft64` to a leaf taking input-stride + output-stride +
  a twiddle pointer (the FFTW "twiddle codelet" shape). Composes WITHOUT a materialized transpose. Gate: reproduces
  `fft64`. Measure: N=64 still parity.
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
- four-step 32x16 no-spill per-column lane-split: 0.72 (per-column gather offsets the spill-fix).
- => the four-step IS the best structure; the gap is the transpose-pass MEMORY overhead (~1.13x flops vs ~1.4x
  time). MKL = integrated, fewer memory passes, deep-tuned over years.

### The actual campaign levers (sustained, profile-driven, fresh-context, clock-pinned)
1. MEASUREMENT FIRST — clock-pin the rig (BIOS turbo off / fixed freq) or tight-interleaved 3-way (ours/FFTW/MKL)
   bursts + confidence intervals. The +-9% throttle noise currently HIDES every <10% lever.
2. Reduce the transpose-pass memory overhead (the profiled root) — fuse the transpose into the codelet strided I/O
   (never a separate L1 round-trip; FFTW twiddle-codelet model); blocked-tile transpose; measure L1/DRAM traffic.
3. Bigger register-fitting sub-codelets — a genfft-register-scheduled size-32 (the `_schedule` lifted N=16 +22%).
4. Factorization + L1-block sweep — 32x16 vs 16x32 vs 8x64 vs 3-level 8x8x8.
5. Prefetch-distance sweep — `_mm_prefetch` on the strided gathers, tuned to the cache hierarchy.
6. Wire the WON codegen band (N<=256) into a generator four-step driver so large-N reuses the crushing sub-codelets.

### Honest target
Match MKL (parity) at N=512/1024; beating FFTW's 0.85 is the milestone, MKL's 1.0 the goal. Deepest FFT-opt tier
(FFTW/MKL spent years here); progress is single-digit % per lever => MEASUREMENT (lever 1) gates everything.
The WON band (crush <=32, beat FFTW <=256, the codegen generator) is durable + the foundation; this is the large-N
chapter. Seeds: build/four256.cpp (0.92, beats FFTW), four512.cpp + four512b.cpp (0.72-0.75 + profile probes).
