# 2026-06-14 — FFT: fstw read-floor cut (+15% @8M), clang refuted, the paper-grounded mid-band crush plan

**Mandate (user, emphatic, standing):** FULL MKL CRUSH on 1D complex FFT — every size, not just beat PocketFFT.
This session banked a real large-N win, killed a cheap hypothesis with a clean probe, *read the actual papers*
(`docs/books/`), and converted "the mid-band needs genfft codegen" from a vague verdict into a **concrete,
paper-specified build plan**.

## Banked wins (gate-green 59 assertions, oracle-gated, determinism moat intact)

1. **fstw twiddle-factorization** (`fft.hpp`). The four-step inter-stage twiddle `W_n^{i2·k1}` was a stored
   n-sized table = **128 MB of streaming DRAM read per transform @8M** — a third of the four-step read floor,
   pure overhead MKL never pays. Replaced with `W_n^a = W_n^{a_hi·M}·W_n^{a_lo}` from two ~√n L2-resident
   tables (`m_ftw_hi/lo`, M = 1<<⌊log2 n/2⌋) + one complex multiply. **Measured +15% @8M (11.49→13.25 GFLOPS),
   +7% @2–4M.** Bit-changing but oracle-correct (1e-15) and a NEW deterministic computation (moat holds).
2. **Prefetch on the strided phase-1/2 gathers** (hint-only, bit-identical). +0–5% in-context (caution: the
   isolated probe showed +27% — micro-probe gains don't fully transfer; this bit prefetch and informs the
   pipelining risk).
3. **Extended the four-step oracle gate to the WHOLE crossover band 2¹⁹–2²²** (`test_fft.cpp`, was only 2²²).

## MKL pinned (the decision input) + the large-N pipelining probe

- **MKL @8M = ~60–62 ms** (15.9 GFLOPS, tight 3–15% spread, `build/mkl_pin.cpp`). Four-step now ~0.74× (vs
  MKL best-of) / ~0.84× (vs median).
- **Gather-bandwidth probe** (`build/gather_probe.cpp`, crd TLSF, STL-free): the strided gather is
  **latency-bound ~13 GB/s** (prefetch lifts to ~16, reads can't fill the bus); the **tiled B=32 NT-store
  transpose hits ~28 GB/s** (stable across runs) by overlapping the strided read with a concurrent NT write.
  ⇒ the large-N pipelining lever (overlap the DRAM floor with the L2-resident sub-FFT compute) is **alive**;
  `build/pipeline_probe.cpp` is armed to gate it. Queued as the SMALLER follow-on.

## ⭐ The clang A/B — a clean refutation (cheap probe kills a hypothesis)

KFR ("requires Clang for top performance") suggested the mid-band 8K–64K gap (Cerid ~14 vs MKL ~55 GFLOPS)
might be the gcc/MSVC scheduler. Built the EXACT representative bench (real crd TLSF, crd sources compiled WITH
clang, MKL/FFTW/PocketFFT refs) — `scripts/run_bench_fft_clang.sh`. **Result: clang ≈ gcc, a wash** (@8192 gcc
14.6 vs clang 12.0; @16384 14.0 vs 14.5). **The mid-band gap is NOT the compiler.**

⚠ En route I reached for a malloc IAllocator to dodge a win-clang-cl debug-CRT link mismatch — the user stopped
it (violates `crd-no-malloc-allocator` + non-representative, ~12% bandwidth). Lesson logged:
[[feedback_no_malloc_in_probes_even_to_dodge_toolchain]]. Fixed by compiling crd sources with clang directly.

**The clinching evidence (advisor) — the codegen-vs-handwritten dichotomy:**

| n | Cerid | PocketFFT | FFTW | MKL |
|---|---|---|---|---|
| 8192 | 12–14 | 12.9 | **48** | **55** |
| 32768 | 13.6 | 13.7 | **37** | **55** |

**Cerid ≈ PocketFFT (both hand-written codelets); MKL & FFTW sit together ~3–4× above (both *generated* —
Spiral & genfft).** We are at/above the best NON-codegen FFT library. The mid-band gap is precisely the
genfft/Spiral codelet frontier — there is **no cheap structural lever** (clang was the last one). We still
**beat PocketFFT everywhere**; "≈ PocketFFT" is a frontier diagnosis, NOT a regression.

## ⭐⭐ The papers (read this session) hand us the named method

**FFTW / Frigo-Johnson "Implementing FFTs in Practice" (`docs/books/fftnew/2602.23525v1.pdf`):**
- Cerid's mid-band runs **breadth-first Stockham** = cache complexity **Θ(n·log₂n) — the paper's word is
  "pessimal", "no temporal locality at all."** That IS the 8K→L2 cliff.
- MKL/FFTW use **depth-first recursion / blocking** = **Θ(n·log_Z n), "rigorously optimal"**; "immediately
  points us towards large radices (not radix 2!)". Once a sub-transform fits in cache, **zero further misses.**
- **§3.3 prescription, verbatim:** for moderate n, **depth-first recursion with bounded radix-32 + size-32/64
  hard-coded codelet base cases** (NOT radix-√n; that's only worth it at n≳2²⁰). The genfft cache-oblivious
  *register* scheduler makes machine-independent codelets "no slower than SPIRAL's machine-specific" ones.
  Size-64 codelet > four size-16 (more compute per datum, the log n factor).
- For n≳2²⁰: a single step of recursive **radix-√n four-step** — which is **exactly our four-step** (now +15%
  fstw). We're already on the right large-N algorithm.

**Johnson-Frigo modified split-radix (`docs/books/fftnew/newsplit.pdf`):** lowest known power-of-2 flop count,
~6% below the plain split-radix we shipped in Part 7 (their table: 8192 = 364680 vs 376840 ops).

**hpkfft-2023 (`docs/books/hpkfft-paper-2023.pdf`):** beats the vendor (MKL) by **>40% on AVX2 in modern C++**.
Existence proof — the crush is achievable on our exact target.

## ⭐⭐⭐ The mid-band crush plan (paper-grounded, concrete — the MAIN THRUST)

The seed exists: `rec_fft_soa` (`fft.hpp` L660–747) — depth-first **radix-4** split + SIMD radix-4 combine,
base ≤32, **strided `m_tw` combine-twiddle gathers**. Prior session shelved it for *large-N* collapse, but at
65536 it was only −9% (NOT a collapse) — abandoned for the wrong regime. Upgrade it per the paper:

| FFTW prescription | rec_fft_soa today | step |
|---|---|---|
| bounded radix-**32** | radix-**4** (many levels) | raise split radix ⇒ fewer combine passes |
| base **32/64** | base ≤32 | add a size-64 codelet leaf |
| good codelet leaves | plain split-radix | **modified split-radix** (newsplit, −6% flops) + scheduler |
| cache-friendly twiddles | **strided** `m_tw` gather | precompute per-level (the fstw trick) |

**Sequence (each step gated vs the radix-2 oracle AND measured on the ×MKL@mid-band scoreboard — measure-first;
the mid-band has paid for 8 wrong-lever dead-ends):**
1. Wire `rec_fft_soa` for the mid-band (8K–256K) behind a flag; re-confirm the −9% baseline vs Stockham.
2. Raise the recursion radix 4→8 (and try 16); measure.
3. Add a size-64 codelet base case; measure.
4. Fix the combine-twiddle strided gather (precomputed per-level table); measure.
5. Modified split-radix codelet leaves (`gen_fft_codelets.py`); numpy-self-check, regen, measure.
Honest: this is the genfft project — incremental, paper-specified, NOT a vague gamble. hpkfft proves the ceiling
is >MKL on AVX2. Each step must move ×MKL@mid-band or it's dropped.

## ⭐ MEASURED UPDATE — recursion is dead; the mid-band wall is CODELET QUALITY in isolation

Wired `rec_fft_soa` (the paper's depth-first structure) behind `CRD_FFT_REC`, gated correct (maxrel 1e-15),
measured on the ×MKL bench. **It LOSES at every mid-band size and collapses at large-N** (8192 −6%, 65536
−27%, 131072 −33%, 262144 −46%) — confirms the prior session, and it's the *suboptimal transpose-free* form
(strided access defeats the cache benefit on this prefetcher box). **Reverted the wiring** (rec_fft_soa kept as
a seed, unwired; one comment in `execute()` records the verdict).

⭐⭐ **The decisive measurement: n=1024 is fully L1-resident (16 KB) ⇒ ZERO cache penalty — and the direct
codelet path is still only ~26 GFLOPS vs MKL ~56 = 0.46×.** So the mid-band gap is **NOT cache structure**
(recursion/four-step can't fix it — even a perfectly cache-resident transform is 2× off). It is the **in-context
codelet rate**: the isolated radix-8 codelet hits 50–69 GFLOPS (prior mca), but in-context it runs ~26–30
because it is **load/store-port bound** — each Stockham pass reads+writes the whole SoA buffer, so too few
butterflies execute per buffer touch. MKL does **more compute per load/store** via **larger fused codelets**
(the FFTW paper: "a size-64 codelet > four size-16", more compute per datum).

⇒ **The mid-band crush lever (refined, measurement-grounded): LARGER FUSED CODELETS** — do more radix stages
in registers per buffer round-trip, kept non-spilling by the **genfft cache-oblivious register scheduler**
(treats the 16 ymm regs as a cache; loads-late/stores-early/Belady). NOT a recursive driver (dead), NOT flop
reduction alone (modified split-radix is ~6%, the gap is 2× and overhead-bound). **Test bench = n=1024** (L1,
isolates codelet quality from cache). This is the genfft codegen project — confirmed by elimination as the only
remaining lever, now with the constraint named precisely (load/store-port-bound in-context codelet).

## ⭐⭐ ROOT CAUSE (Part 16) — the small/mid 2× gap is SoA-over-k vs AoS-within-transform, NOT radix/cache/scheduler

Forced-radix sweep at the L1/mid band (`CRD_FFT_FORCE_RMAX`, reverted; bench rel-to-MKL):

| n | radix-8 | radix-16 | radix-32 | MKL | best Cerid |
|---|---|---|---|---|---|
| 1024 | 25.3 | 18.4 | 24.3 | 59 | 0.42× |
| 8192 | 12.6 | 14.5 | **15.3** | 50 | 0.30× |
| 32768 | 11.1 | 14.0 | 14.8 | 47 | 0.31× |

**Bigger radix is MAXED** — even the best per-size radix is ~0.30–0.42× MKL; the planner is already near-optimal
(only 8192 leaves ~5% by choosing radix-16 over radix-32). So "more fused stages via bigger radix" does NOT
crush. The scheduler (`_schedule` in `gen_fft_codelets.py`) is already a decent genfft-style register-pressure
greedy. Cache is irrelevant (1024 is L1-resident and still 0.42×).

⭐⭐⭐ **THE ROOT (by elimination + the register arithmetic):** Cerid's codelets **vectorize OVER k** (4
transforms' worth of k per ymm, split re/im). A radix-8 codelet = 8 complex × (re-vec + im-vec) = **16 ymm =
exactly the AVX2 file**; radix-32 over-k needs ~64 ymm ⇒ **catastrophic spill** (prior mca: 157 spill-stores).
That caps the fused codelet at radix-8 ⇒ too few butterflies per buffer load/store ⇒ **load/store-port bound at
~half the codelet's isolated 50–69 GFLOPS rate** (25 in-context @1024, no cache penalty). **MKL vectorizes
WITHIN one transform (AoS, 2 complex/ymm)** ⇒ a radix-32 codelet fits 16 ymm with NO spill ⇒ 5 stages per
buffer touch ⇒ ~½ the load/stores per point ⇒ near its isolated rate ⇒ the ~2×.

⇒ **THE MKL CRUSH for small/mid = AoS WITHIN-TRANSFORM fused codelets** (radix-32 in 16 ymm, the MKL/genfft
layout) — a NEW codelet kind + execute path, NOT a tweak to the SoA-over-k engine (which is capped at radix-8,
measured-confirmed dead for going bigger). Tradeoff to verify: AoS complex-mul needs shuffles, but the
no-spill register win should dominate (MKL proves it). This is the genfft architecture, now precisely scoped.
**It deserves FRESH CONTEXT — a SoA→AoS codelet path is delicate (shuffle-based complex mul, the in-register
DIT/DIF schedule, oracle-gating every codelet). First step next session: hand-write + mca an AoS radix-32
codelet, measure its in-context rate at 8192 vs the SoA-over-k one — if it clears ~35–40 GFLOPS the architecture
is validated and worth the full build; if shuffles cancel the register win, AoS is dead too (cheap probe first).**

## ⭐⭐⭐ AoS PROBE (Part 17) — the architecture is VALIDATED: AoS radix-32 codelet = ~45 GFLOPS, near MKL

Hand-wrote an **AoS (2-complex/ymm) within-transform radix-32 DIT codelet** (`build/aos_probe.cpp`, 5 radix-2
stages, AVX2; stage-0 intra-ymm butterfly bug found+fixed via the scalar-DFT gate). **Correct: maxerr 2.89e-16.**
**In-context throughput (contiguous L1 load/store, the real codelet regime): ~45 GFLOPS, stable (45.2/45.7/44.1).**

| building block | in-context | vs MKL mid-band ~47–50 |
|---|---|---|
| SoA-over-k (current engine, whole-FFT @1024) | ~25 | 0.5× = the wall |
| **AoS radix-32 codelet (probe)** | **~45** | **~0.95× — near parity** |

⇒ **THE AoS WITHIN-TRANSFORM ARCHITECTURE IS CONFIRMED** as the MKL-crush path. The AoS building block reaches
MKL-class rate where the SoA-over-k block is capped ~25. The ~45 is a **floor** — the codelet does full
complex-muls on the trivial stage-0 (W=1) and stage-1 (W∈{1,−i}) twiddles a split-radix codelet would skip;
tuned + non-spilling it has headroom. Honest caveat: 45 is a single-codelet repeated (one pass, L1-resident);
a FULL AoS FFT adds inter-pass structure + larger-than-L1 sizes, so the whole-FFT rate lands **below** 45 — but
the headroom over the SoA 25 is large ⇒ MKL parity/crush is reachable.

⭐ **NEXT BUILD (de-risked, the crush): the full AoS FFT engine** — (1) generate AoS radix-8/16/32 codelets in
`gen_fft_codelets.py` (AoS cmul = `fmaddsub(wr, x, wi·swap(x))`, the probe's `aos_cmul`; numpy-self-check); (2)
an AoS execute path (interleaved in-place, no SoA deinterleave); (3) wire for the small/mid band, oracle-gate,
measure full-FFT ×MKL @1024/8192. Then split-radix the AoS codelets (skip trivial twiddles) for the final edge.
Multi-session; each step moves ×MKL or drops. `build/aos_probe.cpp` is the validated seed codelet.

## ⭐⭐⭐ AoS CODELET GENERATOR BUILT (Part 18) — `gen_aos_codelets.py` + `aos_codelets.hpp`

Built the generator the crush needs: `scripts/gen_aos_codelets.py` emits AoS radix-8/16/32 DIT leaf codelets
(forward AND inverse) as straight-line AVX2 `__m256d`, generalizing the validated probe structure (bit-rev load
→ stage-0 intra-ymm → stages 1..b-1 lane-aligned AoS butterflies; `aos_cmul = fmaddsub(wr, x, wi·permute(x,5))`).
**Validated TWO ways:** (1) numpy model vs `np.fft` (radix-8/16/32 fwd+inv all ~1e-16); (2) the **actual emitted
C++** compiled + run vs a brute-force DFT (all ~2e-16, machine precision). Header generated to
`engine/hesap-fft/include/crd/hesap/fft/detail/aos_codelets.hpp` (571 lines), **compiles standalone in-engine**.
These are the ~45-GFLOPS building blocks (same stages as the probe).

⭐ **NEXT BUILD (the measurable crush — execute path): compose the AoS leaf codelets into a full FFT.** (1) a
CONTIGUOUS-load codelet variant (the bit-rev is structural in a full FFT, not per-codelet — the ~45 GFLOPS form);
(2) for n≤32, the leaf codelet IS the whole FFT (interleaved in-place, no SoA deinterleave); (3) for n>32, an AoS
four-step/recursion: n2 size-n1 AoS codelets → AoS twiddle → n1 size-n2 AoS codelets (an AoS twiddle-combine
codelet is the new piece); (4) wire small/mid, oracle-gate, measure full-FFT ×MKL @1024/8192 — THE crush number;
(5) split-radix the AoS codelets (skip trivial stage-0 W=1 / stage-1 W∈{1,−i} muls) for the final edge.
`build/aos_probe.cpp` (45 GFLOPS proof) + `aos_codelets.hpp` (validated generator output) = the foundation in hand.

## ⭐⭐ EXECUTE PATH (Part 19) — built + CORRECT, but the four-step composition is REORDER-bound (0.26× MKL), NOT a beat yet

Built a full n=1024 FFT via a 32×32 four-step using the AoS radix-32 codelets (`build/aos_fft1024.cpp`):
transpose → 32 inner codelets → twiddle → transpose → 32 outer codelets → transpose-out. **CORRECT vs brute DFT
(1.75e-15).** ⛔ **But 18.7 GFLOPS = 0.26× MKL (71)** — WORSE than the current SoA engine (0.42×).

⭐⭐ **THE HONEST FINDING (measured): the codelet's 45 GFLOPS is an ISOLATION number; the full FFT pays the
four-step REORDER overhead and it dominates.** Tried the contiguous-load codelet (the 45 form) + fused the
bit-reversal into the transposes — **NO change (0.26×)**, because fusing bit-rev into a transpose turns it INTO
a gather. The reorder (bit-rev + transpose between the two radix-32 passes) is **fundamental** — it has to
happen somewhere (codelet gather OR transpose), and at 1024 it's ~58% of the time (3 transposes + twiddle vs
the codelets' 42%) ⇒ the 45 halves to ~18. **The four-step composition is reorder-bound, the SAME overhead that
killed the SoA four-step at small N. The fast codelet does NOT survive naive composition.**

⇒ **The AoS within-transform codelet (a COMPLETE sub-transform) inherently needs the reorder; that's the wrong
granularity to beat MKL at small/mid by itself.** MKL's advantage is the INTEGRATED transpose-free structure
(radix *stages* / DIF-DIT that never materialize a transpose), not just the codelet. **Beating MKL needs that
integrated AoS structure — the deep engine, the same integration wall that's recurred all grind.** Honest
scoreboard: codelet 45 (isolation) ✓ · generator ✓ · full FFT correct ✓ · **but 0.26× = NOT an MKL beat.**

⭐ **NEXT (the real engine, multi-session, fresh context): an integrated transpose-free AoS FFT** — either
AoS radix *stages* (butterflies across the array, no complete-codelet reorder) kept in L1, or a single-reorder
structure with a SIMD (not scalar/gather) transpose. The codelet+generator are validated seeds; the COMPOSITION
is the remaining hard problem (reorder-bound). Probes: `build/aos_probe.cpp` (45 codelet), `build/aos_fft1024.cpp`
(0.26× four-step, the reorder wall measured).

## Status / loose ends
- **DoD (per-slice-check)**: win-debug build failed on a **stale PCH (C1853) in `tools/asset_cooker`** — pre-
  existing build-hygiene, UNRELATED to FFT (cook handlers). win-asan/shipping/tidy validate the FFT tests.
  TODO: clean the cooker PCH, re-run win-debug, then the wins are committable.
- **Commit pending** (user commits): fstw + prefetch + extended oracle gate, on top of split-radix + four-step.
  Message drafted in chat.
- Probes/scripts added: `build/{gather,pipeline}_probe.cpp`, `scripts/run_{gather,pipeline}_probe.sh`,
  `scripts/run_bench_fft_clang.sh`.
