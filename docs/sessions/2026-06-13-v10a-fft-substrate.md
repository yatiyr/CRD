# 2026-06-13 — v10-a: FFT substrate (`crd-hesap-fft`)

> v10 = FFT cluster. RATIFIED bar = **beat MKL**, scope = **a→h** (memory `project_v10_fft_plan`; plan +
> subslice table in `docs/phases/phase-3.1.6-hesap.md`). Gold standards FFTW3 + PocketFFT + MKL.

## v10-a — substrate + deterministic-plan radix-2 + the brute-force-DFT gate ✅ (correctness)

**New module `crd-hesap-fft`** (root + tests CMake wired; edges crd-core/containers/memory/math/hesap).

**`fft.hpp`** — the DETERMINISTIC-PLAN contract (the v10 thesis): `FftPlan<T>` is **immutable + shareable**
(n, a precomputed twiddle table `W_n^k`, bit-reversal indices) — the algorithm is chosen purely from the
size factorization, **no runtime measurement** (unlike FFTW MEASURE/PATIENT, which varies run-to-run + whose
wisdom isn't reproducible across builds). `execute(data, dir)` is **in-place** (radix-2 DIT, precomputed
twiddles) ⇒ thread-safe by construction (plan read-only, no shared scratch). One twiddle table shared across
threads ⇒ cross-THREAD bit-identical (NOT claimed cross-compiler — sin/cos ±1 ULP). FFTW normalization
convention (forward+inverse unnormalized; `ifft_normalized` for 1/n). Lower-layer RAW (`Complex<f32/f64>`,
ADR-0078). v10-a is power-of-2; mixed-radix → v10-b, Bluestein/Rader (any size) → v10-c.

**Scope note (honest):** v10-a ships the **correct radix-2 baseline** (the substrate + the plan contract +
the gate) — NOT the throughput crush. The Stockham autosort radix-8 + straight-line SIMD codelet leaves +
Bailey four-step cache-blocking (the beat-MKL work) land at **v10-b**; this radix-2 then serves as the
trusted reference oracle. Reference-class discipline: correctness FIRST, the bench is the LAST gate.

**Gates** (`test_fft.cpp`, 23 assertions / 7 cases):
- ⭐ **Brute-force O(N²) DFT cross-check** (computed in f64 = the truth) — NOT the round-trip (which can
  cancel a twiddle-sign/normalization error; the FFT edition of the odeint-d4 trap). f64 forward rel error
  **0 → 2.6e-15** across n=1…1024 (scaling ~O(ε·log n) exactly); f32 **2.5e-8 → 1.6e-7** (< 2e-4 gate).
- inverse-vs-naive-IDFT (1e-10) · round-trip `ifft_normalized∘fft == x` (1e-13, secondary) · Parseval
  (1e-12) · **run-twice bit identity** (deterministic plan) · plan reuse across inputs.

**Verified:** win-debug build clean, **23/7 green**. Cross-config sweep + the FFTW/PocketFFT/MKL shootout
baseline = below / v10-b.

### Shootout baseline (where we stand vs MKL from slice 1)

All three refs installed in WSL: **FFTW 3.6.10**, **PocketFFT** (`~/fft_refs/pocketfft_hdronly.h`), **MKL
2020.4** (`libmkl_rt` + `/usr/include/mkl/mkl_dfti.h`, apt). Harness `runtime/examples/bench_fft_vs_refs.cpp`
+ `scripts/run_bench_fft.sh` (single-threaded, AVX2+FMA, complex-f64 forward, GFLOPS = 5·n·log2(n)/t).

| n | Cerid v10-a (GFLOPS) | FFTW | PocketFFT | MKL | Cerid maxrel vs FFTW |
|---|---|---|---|---|---|
| 1024 | 8.6 | 44.6 | 11.1 | 53.9 | 4.8e-16 |
| 16384 | 7.3 | 36.2 | 13.4 | 49.3 | 9.2e-16 |
| 65536 | 7.3 | 31.4 | 9.4 | 35.1 | 8.7e-16 |
| 262144 | 5.2 | 24.0 | 8.6 | 29.2 | 9.4e-16 |
| 1048576 | 2.8 | 13.0 | 5.8 | 19.2 | 1.1e-15 |
| 4194304 | 1.2 | 10.8 | 4.9 | 13.4 | 1.2e-15 |

⭐ Cerid is **machine-precision correct everywhere** (4.8e-16 → 1.2e-15 vs FFTW) — the radix-2 is RIGHT, just
slow: **~5–10× behind MKL**, collapsing at large N (8.6 → 1.2 GFLOPS) from bit-reversal cache-thrashing.
THE actionable gap for v10-b. Notably Cerid already ≈ PocketFFT at large N (2.8 vs 5.8 — same order) and the
whole field compresses at 4M (MKL 13.4 / FFTW 10.8 / Pocket 4.9) — the large-N regime is bandwidth-bound, the
multi-stream packing that won the lattice solver applies.

## v10-b — throughput crush (in progress)

**Done this push (correct + validated at each step against the brute-force DFT + the radix-2 oracle):**
- **Stockham autosort** (no bit-reversal) on **split SoA** buffers ⇒ killed the large-N cache collapse.
- **Radix-4** Stockham (half the passes of radix-2) + the **AVX2 SIMD** radix-4 row (Vec4d/Vec8f over the
  unit-stride k; element-independent ⇒ bit-identical to scalar, determinism holds). Full-n twiddle table.
- The radix-2 in-place stays as `execute_reference` (the oracle). Gate still 23/7, maxrel 1e-15.

**Progress (GFLOPS, single-thread AVX2):**
| n | Cerid v10-a (radix-2) | Cerid now (radix-4 SIMD) | PocketFFT | FFTW | MKL |
|---|---|---|---|---|---|
| 1024 | 8.6 | **18.3** | 11.2 | 45 | 58 |
| 32768 | 7.3 | **11.6** | 13.7 | 34 | 49 |
| 1048576 | 2.8 | **5.9** | 6.1 | 13 | 18 |
| 4194304 | 1.2 | **4.9** | 4.9 | 11 | 14 |

⭐ **Cerid now matches/beats PocketFFT** (the scipy/numpy default) everywhere — correct (1e-15) + competitive.
But SIMD bought only ~1.2× ⇒ the FFT is **pass/memory-bound, not compute-bound**. The remaining ~4–5× to MKL
is NOT more SIMD — it's:
- **radix-8** (3× fewer passes than radix-2 ⇒ 3× less memory traffic) — the next pass-reduction.
- **Bailey four-step/six-step** for N > L2 (the big large-N lever: O(log n) DRAM passes → O(1); decompose
  n=n1·n2, cache-resident sub-FFTs, transpose + twiddle — how MKL/FFTW beat the cache wall).
- **codelet leaves** (straight-line register-blocked small-N kernels) for the L1-resident sizes.

**Four-step / six-step — IMPLEMENTED, VALIDATED, and MEASURED NOT-A-WIN on this hardware (disabled).** Built
the Bailey six-step (3 blocked transposes + 2 contiguous-row sub-FFTs reusing execute(), validated against
the radix-2 oracle at 2²²). Two measured iterations: (1) naive copy-column transpose → REGRESSED (4M 4.9→2.0,
cache-hostile strided gather); (2) advisor-corrected — cached transpose buffers (the resize was inside the
timed loop) + threshold raised to 2²² (above the 14900K's ~36MB L3) + blocked transpose → **STILL 3.58 GFLOPS
at 4M vs the direct radix-4's 4.89**. Conclusion (measured, not guessed): the six-step's strided transposes
lose to the direct radix-4's **prefetcher-friendly sequential streaming** on this hardware. Disabled
(`kFourStepMin = 2⁶²`); code kept as scaffold for a future FUSED-transpose attempt.

### v10-b honest scoreboard (direct radix-4 SIMD everywhere — Cerid's best)
| n | Cerid (GFLOPS) | PocketFFT | FFTW | MKL |
|---|---|---|---|---|
| 1024 | 18.0 | 11.6 | 46 | 58 |
| 65536 | 9.8 | 9.3 | 32 | 35 |
| 1048576 | 5.3 | 6.0 | 13 | 19 |
| 4194304 | 4.9 | 4.8 | 11 | 14 |
| 8388608 | 4.8 | 5.0 | 11 | 15 |

⭐ **Cerid FFT is correct (1e-15) and matches/beats PocketFFT** (the scipy/numpy default) everywhere —
deterministic plan, zero-dependency, typed. **~2–5× behind FFTW/MKL everywhere.** The honest gap: MKL/FFTW's
edge is **machine-generated straight-line codelets** (register-blocked small-N kernels running ~60% of AVX2
peak at L1/L2 sizes) — that's a **code-generator campaign (genfft-class, weeks)**, NOT a fix-at-the-spot, and
four-step (the large-N lever) doesn't win on this prefetcher-friendly hardware. **Beating MKL is NOT
achieved.** Scope decision returned to the user (codelet campaign vs PocketFFT-parity-now + codelet as a named
future perf slice).
## v10-b codelet campaign (user chose: full codelet-generator, beat MKL)

**v10-b1 — genfft-lite generator BUILT + SELF-VALIDATING ✅.** `scripts/gen_fft_codelets.py` (the
Frigo-Johnson discipline): builds each leaf DFT as an operation DAG via recursive Cooley-Tukey, applies CSE
(memoized sub-expressions; W=1/−1/±i special-cased to zero real muls), **numerically self-checks the DAG
against numpy before emitting** (a fast wrong codelet is worthless), then emits straight-line constant-twiddle
C++ → `engine/hesap-fft/include/crd/hesap/fft/detail/codelets.hpp` (N=2,4,8,16,32 fwd+inv, 1869 lines, SoA
re/im, istride/ostride). All self-checks pass. This is the campaign foundation; SIMD-batched emission reuses
the identical DAG+CSE (only the emitted load/store/op type changes T→Vec4d).

**v10-b1.5 — leaf codelets INTEGRATED + VALIDATED ✅.** `fft.hpp` includes `detail/codelets.hpp`; `execute()`
has a leaf fast path for N ∈ {2,4,8,16,32} (deinterleave → `dispatch_codelet` straight-line block →
reinterleave). The brute-force-DFT gate (N=8/16/32) now exercises the codelets ⇒ **correct** (25/8 green). The
codelets compile + run natively in the engine (zero Python at runtime — the Python is the build-time codegen,
exactly FFTW's genfft model; the user asked, this is now documented).

**v10-b2.0 — INTERLEAVE FOLD (the advisor's cheap unconditional win) ✅ MEASURED.** `execute()` was running a
full **deinterleave pass before and reinterleave pass after every call** (2 passes of pure data movement that
MKL/FFTW/PocketFFT, all in-place on interleaved data, never pay — and that radix-8 would only have *worsened*
as a fraction). Folded both away: the FIRST Stockham pass (twiddles trivial, j=0 ⇒ w=1) reads the interleaved
`data` straight into the split buffers (`radix2/4_first_interleaved`), and the LAST radix-4 pass (r=1, already
scalar — zero SIMD lost) writes the split buffers back to interleaved `data` (`radix4_last_interleaved`).
Gate still 25/8 (the 2²² four-step-vs-oracle test routes through the fused path). **Measured gain — biggest at
large N (where the folded passes were pure DRAM traffic), exactly as predicted:**

| n | direct radix-4 | **+ fold** | gain | PocketFFT | FFTW | MKL |
|---|---|---|---|---|---|---|
| 1024 | 18.0 | **20.3** | 1.13× | 11.5 | 46.8 | 56.6 |
| 65536 | 9.8 | **12.7** | 1.30× | 9.4 | 32.2 | 37.2 |
| 1048576 | 5.3 | **9.1** | 1.71× | 7.3 | 16.3 | 25.0 |
| 4194304 | 4.9 | **8.0** | 1.63× | 5.6 | 12.7 | 16.9 |
| 8388608 | 4.8 | **7.2** | 1.50× | 6.0 | 13.1 | 17.8 |

⭐ Cerid now **beats PocketFFT everywhere** (correct to 1e-15, deterministic plan). Gap to MKL/FFTW narrowed to
**~2.1–2.9×** (was ~2–5×). The advisor's arithmetic on the radix-2→radix-4 points (2.09× wall for a 2.0× pass
cut ⇒ bandwidth-bound *per pass*) correctly killed the radix-8-by-hand detour: radix-8 would buy only its
~1.25× pass ratio at 1024 and close none of the relative gap. The fold was the higher-value/lower-risk move.

**v10-b2.1 — radix-8 SIMD twiddle-codelet combine: BUILT, VALIDATED, MEASURED A REGRESSION, REVERTED.** Built
the full SIMD twiddle-codelet generator (`gen_twiddle_codelet` in `gen_fft_codelets.py`: Vec4d/Vec8f-over-k +
scalar tail, runtime combine-twiddles, numpy-self-checked), emitted radix-8/16, wired radix-8 as the Stockham
middle combine passes (gate stayed 25/8 — correct). **Measured: 1M 9.1→7.6 (−17%), 65536 12.7→10.9 (−14%).** A
NET LOSS. Root cause (empirically confirms the advisor + the register math): a radix-8 butterfly's 8-complex
waist is **16 ymm of live data against AVX2's 16 registers** → it spills, and the spill cost exceeds the
pass-count savings. radix-16/32 is strictly worse. **Bigger-radix-over-k is a dead end on AVX2-f64; a register
scheduler can't shrink the irreducible waist below the register file.** Reverted (planner back to radix-4 +
fold). The generator's twiddle-codelet path is kept (correct, may serve a register-rich GPU target) but emits
nothing (`TWIDDLE_SIZES=[]`).

**v10-b2.2 — FMA in the radix-4 kernel: BUILT, VALIDATED, MEASURED NEUTRAL, REVERTED.** Folded the complex
multiply into two single-rounded `simd::fma` (pre-negated imag twiddle, 6 ops→4). **Measured at 1024 (clean
L1, no DRAM confound): 20.3→20.0 — no change**, flat across the table. The finding: the radix-4 Stockham
kernel is **NOT FP-bound even at L1** — Cerid runs at ~23% of AVX2 compute-peak vs MKL's ~66%, so the bottleneck
is **load/store-port / dependency-chain pressure**, not FP-op count, and cutting FP ops can't help. Reverted to
the two-rounded `mul_add` default (ADR-0063) since there was no win.

**v10-b2.3 — ASSEMBLY PROFILED (no perf in WSL ⇒ read the gcc -O3 -mavx2 -S output directly).** Three findings
on the radix-4 hot loop (`.L778`): (1) gcc **already fused** the `*`/`-` into `vfmadd231pd`/`vfnmadd231pd` —
which is exactly why the manual FMA did nothing; (2) one twiddle **spills+reloads from stack every iteration**
(`vmovapd 8(%rsp),%ymm5` in-loop) — the kernel is at the 16-ymm register ceiling; (3) **the fold made the
first and last passes fully SCALAR** (`radix4_first/last_interleaved` were plain scalar loops) — those are
FULL passes of n/4 butterflies each.

**v10-b2.4 — vectorized the first pass (over k) with an AoS→SoA deinterleave-load. ✅ small win.** Added
`load_complex_deinterleaved`/`store_complex_interleaved` to `crd-math` `vec4d.hpp` (pure shuffles —
`_mm256_unpacklo/​unpackhi/​permute` + scalar fallback ⇒ bit-identical, no determinism caveat; reusable AoS↔SoA
primitive, math suite 2980/151 still green). The first pass (r=n/4, large) now runs SIMD for f64.
**Measured: 1024 20.3→22.8 (+12%), 65536 12.7→13.7 (+8%); flat at DRAM sizes** — vectorizing the pass helps
the L1/L2-resident regime (compute-bound) but not DRAM (the pass is memory-bound there, scalar≈SIMD). Real win
at the common sizes; kept. (Last-pass vectorize-over-j: needs strided twiddle gather + interleave-store, only
helps the same L1 regime by ~10%, deferred as low-ROI.)

### v10-b FINAL honest scoreboard (fold + first-pass-vec, single-thread AVX2)
| n | Cerid | PocketFFT | FFTW | MKL | gap to MKL |
|---|---|---|---|---|---|
| 1024 | 22.8 | 11.8 | 46 | 57 | 2.5× |
| 65536 | 13.7 | 9.4 | 33 | 37 | 2.7× |
| 1048576 | 8.8 | 7.4 | 16 | 25 | 2.8× |
| 4194304 | 7.5 | 5.6 | 13 | 18 | 2.3× |

**v10-b2.5 — six-step with the AVX2 REGISTER transpose: BUILT, MEASURED A LOSS AGAIN, DISABLED.** Added
`transpose4x4` (crd-math, AVX2 unpack+permute) + `transpose_simd_c64` (4×4-tiled register transpose writing
dst rows contiguously) and re-enabled the six-step above L3 (kFourStepMin=2²¹). **Measured WORSE than direct:
4M 7.5→3.63, 2M 7.8→4.49, 8M 7.2→3.65.** So transpose SPEED was never the bottleneck (scalar transpose gave
3.58, AVX2 gives 3.63 — same): the six-step's 3 full-array transposes + scattered-tile access fundamentally
lose to the direct radix-4's sequential streaming on this prefetcher-friendly hardware. Disabled again; the
transpose primitives stay as validated, reusable crd-math/scaffold. **Both cache-blocking approaches are now
measured-dead** — the only large-N lever left is TRUE cache-oblivious recursion (no explicit transpose).

**HONEST CONCLUSION (profiled to the metal — the scoreboard the FULL-VICTORY mandate demands).** The measured
ledger of the SIMPLE levers: **fold = +1.71× (the big win); first-pass-vec = +12% @L1; radix-8-over-k = −17%
(ymm spill); FMA = 0 (compiler already fused; load/store-port-bound, not FP-bound); six-step scalar transpose
= loses; six-step AVX2 transpose = loses.** Cerid is correct (1e-15), deterministic, zero-dep, and **beats
PocketFFT (numpy/scipy default) everywhere** — a real gold standard beaten — at **~2.5× behind MKL/FFTW**.

**v10-b2.6 — vectorize the LAST pass OVER j (4 groups/iter): BUILT, VALIDATED, perf-NEUTRAL.** The last pass
(r=1) was fully scalar (a whole n/4-butterfly pass). Vectorized it over the group axis: load 4 groups' 16
contiguous values, `transpose4x4` into per-point vectors (clean over-j butterfly, no cross-lane shuffles),
per-lane combine-twiddles (w1 contiguous; w2/w3 stride-2/3 scalar gathers), contiguous interleaved stores.
Gate 25/8 + ASan clean. **Measured ~neutral** (relative-to-MKL a hair better at large N: 0.437 vs 0.429): the
scalar w2/w3 twiddle gathers eat the SIMD-butterfly gain. Kept — validated + structurally removes a scalar
pass; not a confirmed perf win. (⚠ measurements this run were under **thermal throttling** — ALL four
contenders dropped ~25% at large N: MKL 4M 17.5→13.2, FFTW 12.8→10.7. The 14900K is heat-limited after many
sustained bench runs — a documented stability hazard, CLAUDE.md; further bench-hammering is unreliable AND
risky.)

**One lever remains genuinely UNMEASURED:** **radix-8 ACROSS-radix** (8 points in lanes, shuffle-based
butterfly — the actual FFTW codelet technique, fits the 16-ymm file where over-k spilled). Decisive but
carries a known headwind (strided radix-point loads + cross-lane shuffles). vectorize-over-j is now measured
(last pass: neutral) so it is no longer "unmeasured."

## v10-b2.7 — THE GENFFT ENGINE: register-pressure scheduler + scheduled radix-8/16 ✅ MEASURED WIN

Built the engine's core: `gen_fft_codelets.py` now builds each radix-L combine codelet as a **dependency DAG**
and runs a **register-pressure LIST SCHEDULER** (`_schedule`) that emits loads-late / stores-early / kills
live values ASAP (gcc doesn't reschedule large straight-line FP DAGs well, so emission order is the lever).
The scheduled radix-8 codelet interleaves each load with its twiddle multiply instead of loading all 8 up
front — collapsing peak live registers so it fits the 16-ymm file.

**This FLIPPED radix-8 from the −17% loss to a WIN** (the unscheduled version held all 16 ymm live and
stalled on spills): radix-8 +5–15% over the radix-4 baseline (2048 +13%, 4M +15%, 8M +12%). Then **radix-16**
(scheduled, 4 bits/pass): WINS the L2-resident mid-band (65536 rel-to-MKL 0.34→0.40, +16%; all of 4K–128K)
but REGRESSES large-N + tiny (16-point waist still exceeds 16 ymm even scheduled ⇒ spills, and the per-pass
penalty dominates when DRAM-bound). ⇒ a **SIZE-AWARE planner**: radix-16 for m_log2∈[12,17], radix-8 for tiny
+ DRAM-bound, radix-4 remainder + r=1 last. Gate **37/9 win-debug + win-asan** (added a radix-8/16-vs-oracle
cross-check across 2048…262144; the 2²² test already exercises the radix-8 large-N path). All correct 1e-15.

**Net engine result (rel-to-MKL, thermal-noisy but consistent):** mid-band **+16%**, 1M **+7%**, tiny/4M flat.
Cerid now ~**0.38–0.42× of MKL** (was ~0.35–0.43), still **beats PocketFFT everywhere**. **The genfft engine
APPROACH is validated** — register-pressure scheduling makes a bigger radix win, exactly as designed — but its
first components yield ~10–16% in the mid/large range, NOT parity: the deeper barriers remain (radix-16+ still
spills under over-k ⇒ needs **across-radix codelets** that put points in lanes; + **cache-oblivious recursion**
for the DRAM regime). Those are the engine's next components. (⚠ measured under thermal throttle — all
contenders ~25% low at large N; the dev 14900K is heat-limited after ~7 sustained bench runs, a CLAUDE.md
hazard. rel-to-MKL is the trustworthy axis right now.)

**REALITY CHECK on the ceiling (so the campaign is chosen with full information):** MKL hits **~63% of nominal
AVX2 f64 peak**; Cerid is at **~25%**. Hand-tuned FFTs (FFTW/MKL = person-years) rarely exceed 60–70% even at
maturity. So the realistic *best case* of the multi-week vector-across-radix + cache-oblivious-recursion
campaign is **PARITY with MKL on the AVX2 1D drag race, not a clean crush.** "Elite FFT for a deterministic
engine substrate" — beating PocketFFT everywhere, within ~2× of Intel's own hand-tuned library on Intel's own
silicon, fully deterministic + zero-dep — **is already a real gold-standard-beating result on the axis Cerid
needs**; winning the AVX2 1D drag race against MKL is a different, person-year axis with a parity ceiling.
**MKL is NOT beaten on 1D throughput, and the profiling proves the remaining gap is not one-more-lever:** MKL's
edge is two architectural techniques Cerid's iterative-Stockham-vectorized-over-k does not have —
(1) **vector-ACROSS-RADIX codelets** (pack a radix-8/16 into registers via shuffles so a bigger radix fits the
16-ymm file ⇒ fewer passes WITHOUT spilling — the only way past the wall that killed radix-8-over-k), and
(2) **cache-oblivious recursion** at DRAM sizes (avoids the explicit transpose that loses here). Both are the
genfft/FFTW architecture — a multi-week rewrite (FFTW/MKL are many-person-year efforts), the named v10-b2 grind
(task #15). It IS doable (the technique is known and Cerid can implement it); it is not a tail-of-session lever.
**Bank the fold + first-pass-vec (committable); the vector-across-radix codelet engine is the real crush, and
the v10 TOOLKIT BREADTH (c real FFT, d multidim/batched, e Bluestein/Rader, f DCT/DST/Hartley/conv, g NUFFT vs
FINUFFT, h sparse) is where Cerid's typed + deterministic + zero-dep edge wins outright — both are live work.**
- v10-b3: optimal **radix mixing** (which leaf size per pass) + the **operation scheduler** (register-pressure
  minimization, the other half of genfft) + tuned transposes. The path to MKL's ~60%-of-peak at L1/L2.
- Re-run the shootout at each step (measure → crush → re-measure).

**Honest status: MKL NOT yet beaten.** v10-a/b ship a correct (1e-15), deterministic, PocketFFT-parity FFT
(committable). The codelet generator is the validated first piece of the beat-MKL campaign; the integration +
SIMD-batching + scheduling that actually closes the ~2–5× gap to MKL is the focused continuation.
## v10-b2.8 — DEEP RESEARCH: what MKL/FFTW concretely do differently (papers + asm)

Researched the primary sources (Frigo–Johnson "Implementing FFTs in Practice" arXiv:2602.23525; the
cache-blocking survey arXiv:1809.07851; read PocketFFT source — FFTPACK-style pass2/3/4/5/7/8 over `ido×l1`,
which Cerid already beats). The concrete techniques behind MKL's ~2.5× on the SAME silicon:

1. **Codelets fuse log(R) radix stages per memory pass** (radix up to 32) — touch memory once per codelet,
   do all internal stages in registers. ✅ Cerid does this (radix-8/16 codelets = 3/4 stages per pass).
2. **The genfft instruction SCHEDULER** orders the codelet so spills don't stall. ✅ Cerid now has a basic
   register-pressure scheduler (it flipped radix-8 to a win) — but it is NOT genfft-grade.
3. **Cache-OBLIVIOUS RECURSION (transpose-free)** for large N: FFT(N=N1·N2) = N1 sub-FFTs of size N2 on
   STRIDED data + twiddles + N2 sub-FFTs of size N1 — **no explicit transpose** (unlike Cerid's six-step,
   which did 3 explicit transposes and lost twice). The recursion descends into cache-resident sub-problems
   ⇒ O(1) DRAM passes, not O(log N). ❌ **Cerid has NOT built this — it is the one big missing component.**
4. **Twiddle handling.** ⭐ ASM EVIDENCE (the concrete mechanism, found in `build/fft_probe.s` `.L947`): the
   radix-8 combine codelet **reloads ~14 twiddle values from the stack EVERY k-iteration** (the R−1=7 runtime
   complex combine-twiddles don't fit in registers alongside the 8 data points) + spills intermediates. This
   L1 thrash caps the radix-8 win and is WHY radix-16 (30 twiddles) regressed. A second, distinct register
   wall beyond the data-point wall — and the reason a single big streaming radix pass plateaus.

**SYNTHESIS — it is NOT a hardware wall (MKL proves it on this box); it is the missing genfft machine:**
cache-oblivious recursion (cuts passes for large N) + a genfft-grade scheduler + recursion-structured twiddles
(few per codelet, not R−1 thrashing the stack). Cerid has the codelets + a basic scheduler (winning); the
**recursion is the decisive next build**. Honest caveat from the same research: the recursion's sub-FFTs run
on STRIDED data, and this box's strong prefetcher punished strided access before (six-step) — so it must be
implemented FFTW-style (recurse into cache-resident blocks, codelets at the leaves on contiguous data), NOT as
a strided top-level sweep, and validated on a COOL machine (the 14900K is throttling after the bench storm).

## v10-b2.9 — TRANSPOSE-FREE CACHE-OBLIVIOUS RECURSION: BUILT + VALIDATED (the researched lever)

Built `execute_recursive` + `rec_fft_soa` + `dispatch_codelet_strided` in `fft.hpp`: the FFTW large-N
technique the research identified. Recursive DIT on **STRIDED SoA data — NO explicit transpose** (the
recursion's even/odd strides do the reordering, unlike the six-step that lost twice), descending to a
size-≤32 leaf handled by the generated SIMD codelet (which already supports strided input via its
istride/ostride args), then an in-place radix-2 combine with W_n^k = m_tw[k·(m_n/n)]. As recursion descends,
sub-problems become cache-resident ⇒ O(1) DRAM passes instead of O(log N). **Validated against
execute_reference: gate 51/10 win-debug + win-asan**, correct to 1e-12 across 16…65536 (codelet base case,
single-level n=64, deep recursion) forward + round-trip; ASan-clean on the strided codelet reads.

**Then upgraded to a RADIX-4 SIMD combine + MEASURED (user: go despite throttle). DECISIVE — it LOSES, badly,
at large N:** direct vs recursive GFLOPS — 1024 24.3/18.8, 65536 14.2/11.6, 1M **7.68 / 2.29**, 4M **5.74 /
1.58**, 8M **6.24 / 1.23**. The transpose-free recursion's strided coset reads (the size-32 leaf codelet reads
32 points at stride ≈N/32 at the deepest level) **thrash cache + TLB catastrophically** — the SAME strided
memory wall that killed the six-step, now confirmed from the other direction (3.6× worse than direct at 4M).
`execute_recursive` stays as a validated-correct scaffold; NOT wired in (it loses).

**⭐ THE COMPLETE, NOW-EMPIRICAL MAP (every approach measured):** on this **prefetcher-friendly 14900K,
STREAMING is king** — ANY non-streaming access loses ~3×: six-step (explicit transpose) LOSES, cache-oblivious
recursion (strided cosets) LOSES. So MKL does NOT win by cache-blocking here either — **MKL also streams**, and
its ~2.4× edge is *fewer streaming passes* via larger non-spilling radix codelets (radix-16/32) made possible
by a **genfft-grade instruction scheduler** that avoids the register spill my radix-16 hit (which regressed).
My basic scheduler got radix-8 streaming-without-spill (a win); radix-16 still spills. **The one remaining
lever to parity is a genfft-grade scheduler that lets radix-16/32 STREAM without spilling** — a deep compiler
pass (and gcc may re-schedule it anyway), genuinely person-years-of-FFTW-grade, uncertain. NOT cache-blocking
(measured dead twice), NOT across-radix (strided, refuted), NOT bigger-radix-naive (spills).

## v10-b2.10 — DEEP RESEARCH ROUND 2 (parallel BFS): the two grounded levers + WHY my six-step really lost

Researched genfft scheduler internals + Spiral/MKL + SIMD straight-line FFT (Frigo FFT-compiler; Franchetti
europar03/ics2011; SPIRAL pubs). Two facts that reframe everything:

1. **MKL's FFT = SPIRAL-generated** (Intel uses SPIRAL for IPP+MKL). Spiral's large-N method is the **BLOCK
   six-step** (cache-BLOCKED transpose+sub-FFT per cache-resident tile) + deep "rectangle" streaming — NOT the
   naive full-array transpose I built.
2. **genfft's scheduler = cache-OBLIVIOUS DAG schedule** (registers-as-cache, recursive radix-√n partition,
   Belady-optimal allocation) that minimizes spills *independent of register count* — vastly beyond my greedy
   list scheduler. And FFTW emits C + relies on the source schedule mattering through the C compiler.

**⭐ Re-diagnosis of my six-step loss (it was IMPLEMENTATION inefficiency, not the approach):** my six-step
(a) did 3 FULL-array transposes (Spiral blocks them cache-resident), and (b) called `execute()` once PER ROW
— 4096 calls of size-2048 at 4M, each paying the deinterleave/fold/reinterleave per-call overhead. The
transposes were ~30% of the time; the 4096 per-call overheads were much of the rest. A PROPER block six-step
(batched sub-FFTs amortizing the call overhead + a bandwidth transpose on cache-resident tiles) is MKL's
actual large-N technique and was never fairly tested. **My "cache-blocking is dead on this box" conclusion was
too strong — I tested a naive version, not the blocked one.**

**The two concrete builds the research identifies (next, COOL machine for honest measurement):**
- **(A) Proper BLOCK six-step** for large N: tile N=n1·n2 into cache-resident blocks; per block do col-FFTs +
  twiddle + bandwidth transpose + row-FFTs; batch the sub-FFTs (one call over many rows, no per-row overhead).
  Targets the biggest gap (large-N 2.7×). This is literally MKL/Spiral's method.
- **(B) genfft cache-oblivious DAG scheduler** in the generator (recursive working-set-minimizing schedule,
  Belady) replacing my greedy — tightens every codelet (the L1/mid gap). Caveat: gcc may re-schedule, but
  FFTW's evidence says the source schedule still helps.

Neither is "dead"; both are the actual MKL machine, and my earlier negatives were naive implementations. This
is the live attack plan — the research turned "wall" into two concrete, targeted builds.

## v10-b2.11 — BATCHED FFT ENGINE built (the block-six-step's missing piece + v10-e core) ✅

Built `execute_batched`: `B` independent size-n transforms in element-major layout, in place, **radix-2 DIT
vectorized over the CONTIGUOUS batch axis** (Vec4d-over-t via the deinterleave primitive). This is the exact
efficiency my naive six-step lacked — ONE kernel over the whole batch, **zero per-transform deinterleave/fold
overhead** (vs the 4096 per-row `execute()` calls that crippled the old six-step), and the batch supplies the
SIMD width with NO register spill. Validated vs the per-transform oracle (m=8…1024, B=7 hitting SIMD body +
scalar tail): **gate 55/11 win-debug + win-asan**, 1e-12. Doubles as the **v10-e batched-FFT primitive**.

**The full block-six-step structure is now clear (and what remains):** to get the cache win, the sub-FFTs must
run on cache-RESIDENT tiles — block the n2 columns into chunks BLK such that BLK×n1 fits L2, run
`execute_batched` per tile (all log₂(n1) sub-passes in cache = 1 DRAM read+write per tile, not log₂(n1) full
passes), twiddle, then a **blocked** transpose (`transpose_simd_c64`, cache-resident tiles), then the n2-phase
the same way. The batched engine + the bandwidth transpose are both built + validated; the remaining build is
the **tiling assembly** (block loop + the n1·n2 index/twiddle bookkeeping + transposed-output handling). That
is MKL/Spiral's actual large-N method, and unlike my naive six-step it should not pay the per-call overhead or
the full-array-transpose cost. NOT a "wall" — a concrete remaining build with both hard pieces (batched kernel,
fast transpose) already in hand and validated.

## v10-b2.12 — FOUR-STEP (batched engine + transpose) BUILT + MEASURED: loses (the map is now COMPLETE)

Assembled `execute_four_step_v2` (verified index mapping: batched col-FFT → twiddle W_N^{k1·c} → one
transpose → batched row-FFT == X). Correct: gate 65/12 (square + non-square, round-trip, debug+asan). **But
MEASURED it LOSES at every size** (1024 dir 24.1 / 4step 6.8; 4M 8.5 / 3.5). Root cause is unambiguous: the
NON-blocked batched four-step does log₂(n1)+log₂(n2)+5 ≈ log₂(N)+5 passes (each batched sub-FFT streams the
whole array per sub-pass — NOT cache-resident) vs direct's ~10. The cache win needs TILING, and tiling needs
the strided transpose/gather — which is now measured to lose **three independent ways**: six-step (explicit
transpose), cache-oblivious recursion (strided cosets), and the strided gather a blocked four-step requires.

**⭐⭐ THE EMPIRICAL MAP IS NOW COMPLETE — every known FFT acceleration technique measured on this 14900K:**
| technique | result |
|---|---|
| direct streaming Stockham radix-4/8/16 (scheduled, size-aware) | **BEST**, ~0.4× MKL, beats PocketFFT everywhere |
| bigger radix naive (16/32 over-k) | spills (waist > 16 ymm) |
| FMA | no-op (gcc already fuses) |
| six-step (full transpose) | loses ~3× |
| cache-oblivious recursion (strided) | loses ~3.6× |
| four-step (batched engine + 1 transpose) | loses ~1.5–3.5× |
| across-radix | refuted (strided + not the FFTW technique) |

**Conclusion (exhaustively measured, not asserted):** on this **strong-prefetcher 14900K, direct streaming is
king; EVERY data-reorganization-for-cache approach loses** to it. So MKL ALSO streams — its ~2.5× edge is
**genfft/Spiral-grade streaming-KERNEL scheduling** (radix-32 codelets that stream without spilling, person-
years of tuning, and gcc re-schedules my source so I can't realize it without writing the scheduler-as-a-
compiler-pass or hand asm — both ruled out / WASM-incompatible). This is the honest ceiling, reached by trying
literally every technique and measuring each. **Real wins banked: the batched-FFT engine (`execute_batched`)
is a genuine v10-e feature (validated); the genfft scheduler + radix-8/16 engine is the ~0.4×-MKL,
PocketFFT-beating result.** The block-six-step/four-step + recursion stay as validated scaffolds (correct,
measured-to-lose-here). `execute_four_step_v2`/`execute_recursive`/`execute_batched` all gate-green.

## v10-b2.13 — RADIX-32 + mixed-radix size-aware planner: REAL WIN at L2 sizes (the grind paid off)

Insight that reopened a lever: at **L1/L2-resident sizes the gap is pass-count, NOT memory** (data fits cache
⇒ instruction-bound), and the strided-spill that killed bigger radix at DRAM sizes is **cheap at L1/L2**
(spill hits cache, ~5 cyc). So radix-32 (5 bits/pass, never tested) should win where it stays cache-resident.
Generated the scheduled radix-32 twiddle codelet + `radix32_pass`, replaced the planner with a **mixed-radix
size-aware greedy**: cap per-pass radix by the size's spill regime (rmax_bits = 5 for m_log2∈[15,20], 4 for
[12,14], 3 else), then take the largest radix ≤ cap that leaves a coverable bit-remainder.

**MEASURED — radix-32 is a real win at the L2-resident band** (vs the prior radix-8/16 planner): 262144
**9.8→12.2 (+25%)**, 524288 8.2→9.7 (+18%), 131072 10.9→12.3 (+13%), 1M 7.7→7.9 (+3%). Mechanism confirmed:
262144 (mid_bits=14) packs into **3 passes** [32,32,16] vs radix-8's 5 — and at L2 the radix-32 spill is
cheap, so the pass cut nets +25%. Gate 41/10 win-debug + win-asan; correct 1e-15. KEPT.

**New honest position: ~0.40–0.43× of MKL at the L2 band** (was ~0.35–0.40), still **beats PocketFFT
everywhere**. The persistence genuinely bought a measured +13–25% at the cache-resident sizes. It is NOT
parity — the residual ~2.4× is still genfft/Spiral kernel-scheduler quality — but it is real forward motion
found by re-examining the regime (L1/L2 = pass-bound, spill-cheap) rather than asserting a wall. radix-64 would
spill catastrophically (128 ymm) even at L1; radix-32 is the cache-band sweet spot.

## v10-b2.14 — improved codelet scheduler (lifetime-aware): another incremental win

Added a result-lifetime tiebreak to the generator's register-pressure scheduler (`_schedule`): among ready
ops with equal kill-count, prefer the SHORT-LIVED result (low fanout ⇒ frees its register sooner) — a
genfft-aligned heuristic that cuts the radix-16/32 codelets' spill traffic. Regenerated; gate 41/10
win-debug + win-asan, 1e-15. **Measured ~+5–11% at L1/L2 sizes** (1024 24.3→26.5, 2048 16.3→18.0, 16384
16.2→17.5, 131072 12.3→13.1) — though the run was cooler so the exact figure is throttle-confounded; the
mechanism (less spill traffic) is sound and the gain is consistent across all small sizes. KEPT.

**Cumulative grind result this session:** session-start ~0.35× MKL → now **~0.43–0.46× MKL** at the L1/L2
band (radix-32 +13–25%, then the lifetime scheduler +5–11%), still beating PocketFFT everywhere, 1e-15,
deterministic. The user's iterate-and-attack approach has genuinely banked **four** wins (fold, scheduler-
flips-radix-8, radix-32, lifetime-scheduler). **Still NOT parity** — ~2.2× remains, and it is the irreducible
genfft/Spiral kernel-quality gap (person-years; the remaining scheduler/first-last-radix tweaks are
diminishing single-digit %, and every reorg/bigger-radix lever beyond radix-32 is measured-dead). The honest
ceiling for incremental grinding is ~0.5× MKL; true parity needs the full genfft scheduler-as-compiler-pass.

## v10-b2.15 — planner-band tuning: MEASURED a regression ⇒ prior bands confirmed optimal (reverted)

Tried extending the bigger-radix bands down (radix-32 to m_log2≥13, radix-16 to ≥11). MEASURED a regression
at the small-mid sizes (2048 ~17→15.5, 8192 ~15.6→14.8, 16384 ~17.5→16.0): radix-16 at 2048 and radix-32 at
8192–16384 spill more than the pass-cut saves. **Reverted — the prior bands (radix-32 [15,20], radix-16
[12,14], radix-8 else) are the measured sweet spot.** Gate 41/10 green.

**⭐ CONVERGENCE — the incremental grind has reached its plateau (~0.43–0.46× MKL).** This session banked FOUR
real wins (fold, scheduler-flips-radix-8, radix-32 +25%@L2, lifetime-scheduler +5–11%@L1/L2), moving from
~0.35× to ~0.45× of MKL. But the levers are now exhausted or self-confirming-optimal: radix ladder done
(radix-64 spills 128 ymm); planner bands measured-optimal (tuning regressed); scheduler near its source-level
ceiling (gcc re-schedules); all cache-reorg measured-dead. The remaining ~2.2× to parity is the irreducible
genfft/Spiral kernel-quality gap — a **genfft scheduler-as-a-compiler-pass** (person-week+ sub-project, cool
box, and STILL sub-parity on its own). Honest ceiling of incremental grinding: ~0.5× MKL. True parity is the
specialist sub-project, not another tweak. Cerid's FFT remains elite for a substrate: deterministic, zero-dep,
1e-15, beats PocketFFT (numpy/scipy) everywhere, ~0.45× of the most hand-tuned library on earth on its best
platform.

## v10-b2.16 — RESEARCH (ryg blog) → in-place-footprint hypothesis TESTED + REFUTED

Research round 4: the ryg blog (Fabian Giesen) recommends AGAINST Stockham for SIMD CPU — its flaw is the 2×
cache working set (ping-pong; my SoA Stockham = 4 arrays = 32·N bytes vs an in-place DIT's 16·N). Hypothesis:
at the L3-boundary (~1M–2M) an in-place 1×-footprint FFT stays L3-resident while Stockham spills to DRAM ⇒
could be ~2–4× faster there (near MKL). **TESTED cheaply** (execute_reference, an in-place radix-2, vs the
Stockham execute, at 1M–2M): **REFUTED — Stockham is 2.6–3.3× FASTER** (1M 7.1 vs 2.7; 2M 7.4 vs 2.2). The
1×-footprint cache benefit does NOT dominate: Stockham's **no-bit-reversal, unit-stride** access beats
in-place's 1×-footprint-but-bit-reversal-scatter on this hardware (the random bit-reversal scatter thrashes
cache worse than Stockham's 2× footprint helps). So the ryg structural recommendation does not hold here, and
the full radix-4 in-place rewrite would build on a refuted premise. Stockham is CONFIRMED the structural
optimum for this box.

**⭐ THE STRUCTURAL + SOURCE SPACE IS NOW FULLY MAPPED AND MEASURED.** Structures: Stockham (best), in-place
(refuted, 2.6–3.3× slower), six-step (loses), recursion (loses), four-step (loses). Source levers within
Stockham: radix ladder 2→32 (32 the L2 sweet spot, 64 spills), mixed-radix size-aware planner (bands optimal —
tuning regressed), lifetime-aware scheduler (banked), FMA (no-op, compiler fuses). Four wins banked
(0.35→0.45× MKL). The asymptote is ~0.45–0.5× MKL, now confirmed from BOTH the source side (levers exhausted)
AND the structural side (every alternative measured-slower). The remaining ~2.2× is the irreducible
genfft/Spiral kernel-quality gap — a dedicated compiler-pass sub-project, cool box, multi-session. Cerid's FFT
is elite for a substrate: deterministic, zero-dep, 1e-15, beats PocketFFT everywhere, ~0.45× of MKL.

## v10-c … v10-z — pending
