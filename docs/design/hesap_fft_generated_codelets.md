# Hesap FFT — Generated Codelet / Planner Project (design)

> Status: OPEN (substrate project, multi-milestone). Opened 2026-06-16 after the AVX2 local-patch campaign closed
> with four banked archaeology wins (split, f32 scatter NT, f32 SIMD twiddle, f64 SIMD twiddle) lifting f64 8M
> 0.76→0.84× and f32 8M 0.61→0.78× MKL. The remaining sub-FFT gap is bound-analysis-proven to need genfft-class
> codegen, not a local patch. Fork A chosen by the user. Full measured history: `docs/research/fft-mkl-crush.md`.

## 1. Problem statement
Match (or beat) MKL single-thread AVX2 large-N C2C FFT. After four wins the residual is concentrated in the
**sub-FFT** (the per-block batched radix-8 inside the four-step). It is compiler-scheduled generic radix-8 and is
**3.6× above its own flop floor** — port-bound on shuffles + spills, not flops. MKL's sub-FFT is ~2.6× faster.

## 2. Current sub-FFT baseline (4096, the four-step block kernel)
| dtype | kernel | ns/el | cyc/el (~5GHz) | % of full 8M | known bottleneck |
|---|---|---|---|---|---|
| f64 | current 4096 sub-FFT | 2.71 | ~13.5 | ~46% | shuffles(160)/spills(110)/L2 |
| f32 | current 4096 sub-FFT | 1.33–1.34 | ~6.7 | largest remaining f32 phase | Vec8f but still overhead |

Floors: radix-8 flop floor ~3.75 cyc/el; split-radix ~3.0; MKL ~5.25 cyc/el. **CRD's 9.75 cyc/el over the floor is
the lever** (shuffles + spills, both at gcc's limit for hand-written/templated AoS radix-8).

## 3. Why local patches are closed (measured, not asserted)
Every bounded experiment was run and measured-negative: stage-specialized radix-8 reschedule (overhead
batch-amortized; A2 reorder made spills *worse*), split-radix-alone (~+6%, flops are only 28% of cyc/el),
SoA-split sub-FFT (de/interleave + bitrev cost > shuffle saving; shuffles are port-parallel = +10% max),
batch-width (b16≈b32 optimal). See dossier C-11..C-21.

## 4. Why a generator is justified
Closing the 9.75 cyc/el overhead requires attacking **shuffles + spills + flops simultaneously** — a SoA-friendly
schedule with optimal register allocation and split-radix flop reduction. No single source-level patch does this
(each prior patch fixed one axis and the others dominated). A code generator that searches schedules and emits
register-allocated straight-line codelets is the genfft/SPIRAL approach MKL/FFTW use. The small-N AoS lane-trick
codelets (N≤32) already CRUSH MKL — proof a generator path works in this codebase.

## 5. Target kernels
- f64 4096 sub-FFT (the four-step block kernel), engine-native element-major batched layout; AVX2 (Vec4d).
- f32 4096 sub-FFT (Vec8f) — port after f64.
- Ladder: N=64 (sanity) → 256 → 512 → 1024 → 4096.
- The current `execute_batched` remains the fallback; the generated kernel is gated/dispatched by size+dtype.

## 6. IR / schedule representation
A codelet = an op-DAG over complex values (`load`, `add`, `sub`, `mul_tw` const-twiddle, `neg/mulj/mulnj` ±1/±i),
built by recursive split-radix Cooley-Tukey, CSE'd, **numpy-self-checked at generation** (a fast wrong codelet is
worthless). The seed exists: `scripts/gen_fft_codelets.py` (split-radix DAG + CSE + numpy self-check + the
register-pressure list scheduler `_schedule` that already bought +22% on the radix-8-over-k twiddle path).

## 7. Cost model (flops are NOT the only term)
Per candidate schedule estimate: flop count, **shuffle count** (deinterleave/interleave per stage), **peak live
values** (→ predicted spills over the 16-ymm file), L1 temp stores, predicted cycles ≈ max(port pressures). Calibrate
constants against measured codelets. The current kernel's lesson: flops 28% of cyc/el → optimize shuffles+spills+flops
jointly, weighted by measured port throughput (port-5 shuffles vs port-0/1 FMA run in parallel).

## 8. Register-allocator strategy
The `_schedule` list scheduler (greedy: kill most inputs first, defer loads, prefer short-lived results) collapses
peak live so codelets fit 16 ymm without spilling — measured 76→7 spills on the scheduled N=64 atom. Extend it to
the batched layout + emit explicit L1 temporaries only where controlled stores beat compiler stack spills.

## 9. SIMD lane mapping
Batch axis = the SIMD lanes (Vec4d f64 / Vec8f f32), element-major so adjacent transforms are contiguous (the
engine-native layout; NOT the rejected SoA-split or transform-major atom). Deinterleave once per load via the
proven `load_complex_deinterleaved` helpers.

## 10. Twiddle strategy
Combine-twiddles as compile-time constants in the codelet (split-radix internal twiddles) + the four-step column
twiddle handled by the already-shipped bb-axis Vec4d/Vec8f recurrence (C-16/C-18 wins). No factored-table gather in
the hot codelet.

## 11. Correctness strategy
Generation-time numpy self-check (rel err ≤1e-11) before emit; build-time gate vs brute DFT (small) and vs MKL
(4096+); round-trip fwd+inv; run-twice determinism (the moat). f64 ~1e-15 class, f32 ~3e-7 class.

## 12. Benchmark strategy
Standalone build/ probe: generated codelet vs `execute_batched` at each ladder size, ns/el + spills (objdump) +
shuffles + llvm-mca, under the existing MKL comparison harness (core-pinned, MKL_NUM_THREADS=1, interleaved A/B to
beat thermal drift).

## 13. Integration strategy
Generated codelets emit to `engine/hesap-fft/include/crd/hesap/fft/detail/`; the planner dispatches to them by
(N, dtype, batch) with the current path as fallback. Gated until a size wins ≥1.10× sub-FFT AND ≥1.05× full 8M.

## 14. Milestones
- **M0 ✅ (2026-06-16):** design doc + generator skeleton (`build/gen_subfft.py`) + N=16/32/64 emitted, numpy-self-checked, benchmarked vs `execute_batched`. **Generated BEATS the engine at N=32 (1.98×) and N=64 (1.24×)** — proves the pipeline. Dossier C-22.
- **M1 ✅ (2026-06-16):** N=256/512/1024 ladder with the same schedule + register model. **Found the decisive crossover (see §15).** Dossier C-23.
- **M2 (NEXT):** hierarchical 64×64 4096 sub-FFT POC ≥1.10× → compose into full 8M behind a gate. Step plan in §16.
- **M3:** f32 port (Vec8f).
- **M4:** planner dispatch + full-suite DoD + 18-config sweep + parity dashboard close.

## 15. M1 result — the crossover (the architecture-deciding measurement)
Generated batched Vec4d codelets vs `execute_batched` (f64, b=32), all machine-eps correct:

| N | engine ns/el | generated ns/el | speedup | spills | compile |
|---|---|---|---|---|---|
| 16 | 0.221 | 0.470 | 0.47× (the hand-tuned `small_n` lane-trick wins — expected) | low | fast |
| 32 | 1.066 | 0.537 | **1.98×** | low | fast |
| 64 | 1.285 | 1.033 | **1.24×** | low | fast |
| 256 | 1.726 | 1.746 | 0.99× | 4454 | — |
| 512 | 1.948 | 1.860 | 1.05× | 9887 | — |
| 1024 | 2.353 | 2.564 | **0.92×** | **21996** | **574 s** |

**Crossover at N≈64.** Straight-line wins to N=64 then collapses: the DAG (15361 nodes @1024) vastly exceeds the
16-ymm file ⇒ spills explode (21996) ⇒ loses + a 574 s compile (impractical). This is the genfft lesson confirmed
by measurement — **straight-line ONLY for small leaves; larger sizes need recursive composition.**

**Path decision for 4096 → Path B (hierarchical 64×64).** 4096 = 64×64 from the generated-64 leaf (1.24×) as the
radix-64 building block + the already-vectorized bb-axis twiddle + a 64×64 transpose. Rejected: Path A full-4096
straight-line (≈4× the 1024 = catastrophic spills + ~40 min compile); 256×16 (256 already neutral); stage-Stockham
△ (radix-8 near the gcc limit). ⚠ The N=64 leaf win does NOT automatically compose — M2 must prove it.

## 16. M2 plan — hierarchical 64×64 4096 sub-FFT POC
Standalone f64 `build/` probe; baseline = current `execute_batched(4096)`. **Acceptance:** 4096 sub-FFT ≥1.10× →
compose into full 8M; full 8M ≥1.05× → keep behind a gate; else diagnose/revise (never revert to full straight-line).

1. **Lock baseline** — current `execute_batched(4096)` (f64 b16; f32 b32/b16) + the generated-64 leaf (ns/el, cyc/el, max_rel_err).
2. **64×64 decomposition map** — answer explicitly: does stage-1 output feed stage-2 directly · is a 64×64 transpose required · can transpose+twiddle fuse · can it stay L1/L2-resident · does it preserve the engine-native batched contract · is the extra movement < the saved generic-kernel overhead.
3. **Component benchmarks** — stage-1 leaf · stage-2 leaf · 64×64 transpose · twiddle · fused twiddle+transpose · predicted hierarchical total. (The leaf is 1.24× but 4096 only wins if transpose+twiddle don't eat it.)
4. **Build at most TWO variants** — A simple (leaf · explicit twiddle · explicit 64×64 transpose · leaf); B fused (twiddle while writing the transposed tile). No more variants this cycle.
5. **Assembly / pressure check** — spills · shuffles · loads/stores · code size · L1/L2 traffic · llvm-mca. Expected mechanism: avoid the full-4096 spill explosion, reuse the fast leaf, keep the 64×64 intermediate cache-resident. If the mechanism is not observed, do NOT compose into 8M.
6. **Correctness** — vs current CRD + MKL + impulse + sine/bin (f64 ~1e-15 class).
7. **Full-8M composition** — only if the sub-FFT ≥1.10×; keep/revert table at 2M/4M/8M/16M, no regression, determinism preserved.
8. **Parity dashboard** — update (pre-archaeology 0.76/0.61 → 4 wins 0.84/0.78 → hierarchical POC).

**If both variants lose:** diagnose (transpose cost / leaf-call overhead / layout mismatch / icache / twiddle /
shuffles) → choose one of {M2b fused layout · generated stage-Stockham · generated leaves inside the current radix
tree · close the generator path}. **STRICT non-goals (unchanged from Fork A):** no full-4096 straight-line DAG · no
atom transform-major detour · no global SoA rewrite · no engine integration before the 4096 phase win · no
report-only predictions.

## 17. Bank / clean status (2026-06-16)
The 4 archaeology wins — split / f32 NT scatter / f32 SIMD twiddle = **committed** (`d5ae96d`, `d0f2484`); **f64
bb-axis SIMD twiddle = UNCOMMITTED** (working-tree `fft.hpp`, +65 lines; the user commits). M0/M1 generator
artifacts isolated in gitignored `build/`. Engine hot path clean. The 4-win DoD (debug/asan/shipping/tidy 17/17)
was green at C-22. **➜ Commit the f64 twiddle before M2.**

## Parity arithmetic (target)
f64 8M 0.84× → parity needs ~1.19× full; sub-FFT is ~46% → if only sub-FFT improves, parity needs ~1.5× sub-FFT.
Sub-FFT headroom (13.5 → ~5.25 cyc/el = MKL) is 2.6×, so ~1.5× is within the generator's reach if it closes
half the shuffle+spill overhead. Hard but not absurd.
