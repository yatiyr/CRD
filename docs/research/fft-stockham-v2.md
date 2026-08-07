# Stockham v2 — the pass-pipeline FFT engine (the MKL-architecture build)

> **Outcome:** **adopted** — the v10 FFT engine shipped on this design (deterministic-plan Stockham + codelets). *(stamped 2026-08-07, doc-hygiene pass)*

## ★★ THE MKL ARCHAEOLOGY (2026-07-04 — sampled + disassembled ON THIS BOX; the definitive answer)

Software-clock perf sampling (`build/crd_mkl_perf.sh`; WSL2 vPMU absent but cpu-clock works) +
disassembly (`build/crd_mkl_disasm.sh`, `/tmp/mkl_sym.asm` from `/lib/x86_64-linux-gnu/libmkl_avx2.so`):

**Dispatch map (f64, 1T):** 4K–64K = ONE kernel `mkl_dft_avx2_owns_cRadix4FwdNorm_64fc` at 98–99%.
1M+: `cRadix4Fwd` (27–49%) + `cFft_BlkSplit`/`cFft_BlkMerge` (35–55% combined) + `cFftFwd_Fact4/8` +
`zBitRev1_Blk` (~3%) — a BLOCK split/merge architecture around the same radix-4 engine, with blocked
bit-reversal ordering.

**The mid-band kernel, disassembled (1,916 instructions TOTAL — looped, not straight-line):**
mix = 146 vfma*/vfmaddsub + 176 vmulpd + 534 vaddpd/vsubpd + 117 shuffles + 516 vmov + 0 broadcast +
0 prefetch + 0 NT; ~70 branches forming ~8-10 specialized loop nests. The hot loop body shows:
- **AoSoA split re/im** — paired ALIGNED loads/stores at +0x00/+0x20 (re-ymm | im-ymm adjacent):
  the SAME layout we converged on independently.
- **Pure add/sub/mul butterflies on separate re/im registers** — zero shuffles in the steady state
  (the 117 shuffles = the interleaved↔AoSoA conversion confined to entry/exit passes).
- **Specialized constant-twiddle passes**: W_8 angles as rip-relative constants (COS_1_8_/SIN_1_8_).
- **STRIDED multi-destination stores via one stride register** ((%r12), (%r12,%r10,1), (%r12,%r10,2)…):
  radix-4 passes write in-place-strided — the data NEVER moves wholesale ⇒ **no transposes exist**.
- Radix-4 live set ≈ 10-14 ymm ⇒ **zero spills** anywhere.

**Why they get 50 GF/s where our materialized-pass forms get 31-33:** they pay ~7 cheap STRIDED
radix-4 passes over an L2-resident array (bandwidth-free at these sizes) and in exchange have NO
transposes, NO layout conversions beyond entry/exit, NO spills, and tiny per-pass loop bodies. Our
June Stockham failed (0.29-0.47×) for four now-understood, individually-fixed reasons: SoA planes
(2× streams — theirs is AoSoA), radix-16/32 spilling codelets (theirs radix-4), non-FMA butterflies
(we now exceed them: fnmadd/fma on split registers beats their mul+mul+add+sub), and separate
interleave passes (they fuse entry/exit like our S1 does).

## strided-v3 ROUND 1 (2026-07-04, same session): FMA Stockham measured — THE IN-PLACE DELTA FOUND

Rebuilt the existing Stockham's butterflies (radix4_row/radix4_last FMA-ized; `CRD_FFT_STOCKHAM_R4`
= pure-radix-4 plans; `CRD_FFT_FORCE_STOCKHAM` isolates the engine at dispatch; ⚠ CRD_FFT_DISABLE_HIER
is bit-rotted — the M13 four-step branches reference gen:: unguarded). **Isolated measurement: 15-18
GF/s BOTH for radix-4-only and mixed radices — the radix cap + FMA did NOT unlock it ⇒ spills were
not the Stockham's binding constraint.** The remaining delta vs the MKL disassembly is now singular:
**MKL's passes are IN-PLACE** (the strided (%r12,%r10,…) stores overwrite the very locations loaded)
— per pass: n read + n written INTO RESIDENT LINES, ONE buffer. Our Stockham ping-pongs x→y in SoA
pairs = FOUR planes live = ~4× the cache footprint (16K: MKL 256 KB vs ours ~1 MB) ⇒ L2-capacity
bound at exactly the sizes we lose. THE mechanism, end of list.

**THE BUILD (in-place AoSoA radix-4 engine — next session, ~150 lines):**
1. Pass 0: interleaved → AoSoA into m_sh_t with the BIT-REVERSAL folded into the conversion scatter
   (m_rev exists) ⇒ subsequent DIT passes run natural-order, in place.
2. log4(n) IN-PLACE strided radix-4 DIT passes over m_sh_t: load 4 positions, FMA butterfly+twiddle
   (~12 live regs), store the SAME 4 positions. Per-pass twiddle tables (build_combine_twiddles
   pattern with rmax=2, DIT ordering).
3. Last pass fuses AoSoA → interleaved sequential store into the caller's buffer.
   Total: 2 conversion passes + log4(n) in-L2 in-place passes, ONE n-sized work buffer. Target =
   MKL's 45-55 GF/s at 4K-64K; then their Blk split/merge structure at ≥1M from the same engine.

## strided-v3 ROUND 2 (2026-07-04): IP4 BUILT — the FINAL mechanism isolated (register economy)

Built `execute_ip4` (CRD_FFT_IP4): in-place AoSoA radix-4 DIT, digit-reversed gather in, per-pass
VECTOR twiddle loads (zero broadcasts), ~4 pointers, sequential out. **Correct first try (oracle
4-7.6e-16) and instantly at our banked level (4K 28.5 / 16K 30.5 / 32K 33.1 / 64K 26.6 GF/s) — but
NOT 50.** With layout/radix/in-place/broadcasts/spills/FMA all eliminated, the cycle model closes on
ONE remaining variable: **LATENCY, not throughput** — the butterfly's ~20-25-cycle dependency chain
runs UNOVERLAPPED because the split-re/im form needs 8 data ymm per radix-4 iteration ⇒ two
iterations cannot coexist in 16 registers ⇒ no software pipelining (measured: ~25 cyc/iter × 1024
iters × 6 passes ≈ the observed 154+60 Kcyc at 16K, exactly).

**MKL's real secret (now fully decoded): INTERLEAVED-AoS butterflies — 2 complex per ymm, FOUR data
registers per radix-4 butterfly — so TWO butterflies pipeline in 16 ymm and the chain latency
vanishes.** Their 534 add/subs = interleaved ops; the dup/shuffle cmul idiom is the price (and their
117 shuffles + 176 muls match it); the win is register economy → iteration overlap → ~4 ops/cyc.

**THE FINAL BUILD (ip4-AoS — the true MKL clone, one session):**
1. crd::math::simd additions (exact, portable): `fmaddsub` (vfmaddsub, single-rounded; scalar
   fallback per-lane std::fma with alternating sign), `dup_even` (vmovddup), `dup_odd`
   (vpermilpd 0xF), `swap_pairs` (vpermilpd 0x5) — Vec4d + Vec8f editions.
2. execute_ip4 variant with INTERLEAVED vectors (2 complex f64 / 4 complex f32 per reg): loads =
   plain vmovupd of the natural array (NO conversion passes at all — in place over the caller's
   buffer + digit-reverse handled as in ip4); cmul = dup_even(w)·z fmaddsub swap_pairs(z)·dup_odd(w)
   (4 ops); butterfly = plain add/sub on interleaved regs; UNROLL×2 the k-loop (fits: 2×4 data
   + 3 twiddle + temps ≤ 16).
3. Twiddle tables stored PRE-DUPLICATED ([wr wr wi wi]-style pairs) for direct vector loads.
4. Same digit-reverse + pass structure as ip4 (proven correct today); expected 45-55 GF/s; then
   f32, odd-log2 (one radix-2 stage), and the ≥1M Blk split/merge from the same engine.

## strided-v3 ROUND 3 (2026-07-04): ip4-AoS BUILT — the LAST mile is TWO-LAYER PASS FUSION

Built `execute_ip4aos` (CRD_FFT_IP4AOS) + the exact portable primitives `swap_pairs`/`addsub`/
`fmaddsub` (vec4d.hpp): INTERLEAVED vectors (2 complex/ymm), cmul = fmaddsub(z, wr_dup,
swap_pairs(z)·wi_dup) with PRE-DUPLICATED tables (zero runtime dups — one better than MKL), ±i via
addsub(t1, ∓swap(t3)), len-4 pass FOLDED into the digit-reverse gather, last pass writes the
caller's buffer. **Correct first try (4-7.4e-16); 26-32 GF/s — IDENTICAL to the split-register ip4.**
perf-annotate shows an evenly-spread profile = pure µop-THROUGHPUT saturation (no stalls, no
latency wall): ~32 µops per 8 complex × 6 passes ≈ the measured 204 Kcyc at 16K, running at ~4.7
µops/cyc — **our per-pass kernel is already at the machine's issue limit for its pass count.**

**MKL's final trick, decoded by elimination + their loop-nest count: TWO radix-4 LAYERS FUSED PER
MEMORY PASS** (radix-16 as 4×4 in registers — 16 interleaved complex = 8 ymm fits; hence the name
"cRadix4" with ~8-10 specialized nests). Halves the passes ⇒ halves the load/store µops ⇒ the
remaining 2× (30 → ~50 GF/s). THE NEXT (final) increment: fuse layer pairs in execute_ip4aos —
load 4×(4 complex-pairs spanning q and q/4 strides), radix-4, twiddle, radix-4 again in-register,
store; ~2 sessions incl. the twiddle algebra for the fused second layer + odd-log2 + f32 + suite
promotion (the ip4 engines are experiment-flagged; neither yet beats the banked ds/sh so dispatch
is unchanged).

**Score of the day (all suite/oracle-gated):** two correct new engines (ip4, ip4-AoS) at
26-33 GF/s; the FMA Stockham kernels banked; the causal chain to MKL parity now has ONE remaining
link, precisely specified. Eliminated today with measurements: spills (probe), radix width,
broadcasts, group overhead, ping-pong footprint, pointer pressure (annotate), layout conversions,
register economy/latency (ip4 vs ip4aos identical scores prove the OOO window was already
overlapping iterations).

## strided-v3 ROUND 4 (2026-07-04): two-layer fused pass v1 — MEASURED MIXED/NEGATIVE

Built the fused (len, 4·len) pass in execute_ip4aos (16 loads → layer-1 ×4 with SHARED twiddles →
layer-2 ×4 in-register → 16 stores; the adjacent per-pass tables consumed verbatim, zero ctor
changes). Correct (3.7-8.1e-16). **Measured: 4096 +5% (33.9) · 16K −12% (28.1) · 64K −18% (21.5)**
— the ~27-vector live set (16 data + 6 tw + temps) spills in gcc's scheduling, and the layer-2
table read grows with len (6·2·len doubles per fused pass). v1 kept under the flag; the single-layer
ip4aos (30-32 GF/s) remains the engine's best form.

**Honest position after rounds 1-4:** every ARCHITECTURAL element of MKL's kernel is now replicated
and measured (in-place, interleaved, radix-4, fmaddsub idiom, pre-dup tables, fused entry) — we sit
at 30-34 GF/s vs their 50-55. The residual is INSTRUCTION-LEVEL SCHEDULING inside a tight µop/live-
set budget — their 1,916 instructions are hand-scheduled asm refined over decades. Our vehicle for
that class of scheduling is the GENERATOR (it already does DAG scheduling with live-set heuristics):
next session = emit the fused two-layer pass as a GENERATED kernel with explicit live-set-capped
scheduling (the j-loop hand-blocked so ≤14 vectors live, layer-2 twiddles restructured k-major to
localize the second table), plus the odd-log2/f32 editions. The ip4 engines stay experiment-flagged;
the shipped board is unchanged (banked ds/sh/fs6 paths, suite-green).

## strided-v3 ROUND 5 (2026-07-04): block-inner order refuted; the two REMAINING levers quantified

Tried the twiddle-hoisted K-OUTER/BLOCK-INNER order (zero steady-state twiddle loads): **REFUTED at
every stride** — 4096 18.1 / 16K 15.3 / 64K 12.4 GF/s, worsening with len. Mechanism: each k-sweep
re-walks the whole array through L1 at 32 B-per-line utilization ⇒ L1→L2 traffic ×(q/2) per pass.
The 4KB-stride hybrid also lost (23.4 @16K). **Corrected reading of the MKL disasm: their
stride-register stores are QUARTER ADDRESSING inside a k-inner loop — the same order as ours.**
Reverted; ip4-AoS k-inner stands at its best: **4096 32.0 · 8192 30.8 · 16K 32.9 · 32K 32.7 · 64K
26.3 GF/s** (oracle-correct throughout).

**The remaining 1.6× to MKL's 50-55, quantified from today's phase data:**
(a) the scalar digit-reverse gather = ~25-30% of the call at 16K — vectorize/block it (the rev
    structure gives 4 quarter-spaced streams; fold the len-16 second layer into it to amortize) ⇒
    expected → ~40-42 GF/s;
(b) the pass loop runs ~2.2 µops/cyc vs the machine's ~5 — the residual is instruction scheduling
    (gcc's rendering of the 32-µop body); the GENERATOR with explicit live-set-capped emission +
    possibly a software-pipelined 2-deep k-unroll is the vehicle. If (a)+(b) both land ⇒ 48-55.
Neither is architectural anymore; both are craftsmanship on a proven-correct engine.

## strided-v3 ROUND 6 (2026-07-04): levers (a)+(b) built — measured FLAT; the honest position

(a) VECTORIZED the fused gather+len-4 (rev(s+m) = rev(s)+m·n/4 ⇒ quarter-spaced loads; the DFT in
2-complex form via the P-trick X0X1 = u+P / X2X3 = u−P, signed-zero-exact; new concat_lo/hi +
mix_lo_hi primitives). (b) BLOCK-PAIR k-inner passes (one twiddle load per two butterflies, two
independent chains, streaming order kept). **Both correct (oracle ≤7e-16); both ~FLAT: 16K 32.9 →
33.5 (+2%), 4096 32.3, 32K 31.4, 64K 23.9.** The (a) premise was wrong: the 25-30% gather share was
measured on the OLD Stockham, never on ip4-AoS — and the gather's true cost is the scattered 16 B
loads themselves, which no surrounding vectorization removes. (b)'s twiddle-load halving didn't
move the bottleneck either ⇒ the pass loop is NOT load-count-bound.

**The honest position after 6 rounds:** ip4-AoS = 30-33.5 GF/s, architecture fully MKL-equivalent,
correct, deterministic. The 1.5× residual has survived every structural lever C++-through-gcc can
express (fusion, ordering, pairing, twiddle traffic, register economy, in-place). What remains is
the method MKL/FFTW actually USE for this last mile: GENERATED, per-(n,pass) SCHEDULED straight-line
loop bodies with measured iteration — our generator exists and already beat MKL's own batched
kernels once (M15: 1.53×/1.42×/1.17× isolated). Next session: emit the ip4-AoS pass bodies from the
generator (unrolled k-blocks, explicit schedule, per-pass specialization incl. the constant-twiddle
first combine), measure per emission variant, keep winners. ALSO instrument ip4-AoS with phase
counters FIRST (gather vs per-pass) — round 6's premise failure shows the phase split must be
measured, not remembered.

## strided-v3 ROUND 7 (2026-07-04): MEASURED phase split → 3-layer fold + COBRA — 36.95 @16K

Instrumented ip4-AoS with its own phase counters (the round-6 rule): **gather = 29-38%** of the
call (16K: 33.8K of 117.6 Kcyc TSC) — the crush blocker was real; passes-only extrapolates to
47-54 GF/s. Two evidence-driven cuts, both banked:
1. **3-layer gather fold**: the len-16 layer runs on the 16 in-register values with COMPILE-TIME
   W16^m twiddles (correctly-rounded literals, zero table loads) — deletes the len-16 combine pass
   entirely. → 4096 35.3 / 16K 34.75 / 64K 27.7.
2. **COBRA quad-unit**: rev(s + v·n/4) = rev(s) + v ⇒ the tb-quads {s, s+nq, s+2nq, s+3nq} consume
   COMPLETE io lines (io[rev(s) + t·n/16 + 0..3], t = 0..15); 64 complex staged through a 1 KB L1
   buffer — every io line fetched from L2 exactly once (the scattered form re-fetched each 4×).
   → **16K 36.95 GF/s (0.72× MKL, best ever) · 4096 35.9 · 64K 27.5.** Oracle ≤6.5e-16; both
   suites green.

Session trajectory at 16K: 32.9 → 33.5 → 34.75 → **36.95**. Remaining split ≈ gather 30% / passes
70%. For 50-55 the PASS side must now shrink: the quantified next levers are RADIX-8 passes (bf8
with W8 constants: 16K pass count 5 → ~3, ~25% fewer pass µops; needs mixed tables + a bf8 kernel)
and/or generator-emitted pass bodies. Engines remain experiment-flagged (banked ds/sh dispatch is
still the shipped default).

## strided-v3 ROUND 8 (2026-07-04): k-unroll×2 — 37.3 @16K, 30.4 @64K; radix-8 constraints derived

k-unroll×2 in both pass loops (block-pairs: 4 independent chains/iter; last pass: 2): **16K 37.32 ·
64K 30.36 (+10% — pass-heavy row) · 4096 36.29 · 32K 32.79**, oracle ≤6.5e-16, both suites green.
Campaign cumulative at 16K: 32.9 → 37.3. Gap to MKL: 1.45×.

RADIX-8 feasibility was derived before choosing the unroll: the COBRA quad-unit imposes structural
constraints on mixed plans — the FOLD fixes stages 1-2 to radix-4 (io strides n/4, n/16 automatic)
and the quad identity rev(s+v·nq) = rev(s)+v requires the LAST pass radix = 4. Legal plans
R = n/16 = 8^a·4^b with b ≥ 1: 4096 → 8·8·4 (3 passes vs 4), 16K → 8·8·4·4 (4 vs 5),
64K → 8·8·4·4·4 (5 vs 6). µop math ≈ wash on paper (bf8 ≈ 1.5 bf4-passes of work); the real gain =
fewer tb sweeps. Cost: mixed-radix reversal table + bf8 kernel + per-size plan bookkeeping — a full
careful session. The OTHER specified lever stands: generator-emitted pass bodies (2.3 µops/cyc vs
the machine's ~5 says scheduling, not µop count, is the pass wall).

## strided-v3 ROUND 9 (2026-07-04): 4K-aliasing pad REFUTED; chunk-hoisted loops banked

Hypothesis: in-place quarter strides are exact 4K multiples for len ≥ 1024 ⇒ 4K-alias stalls
between quarter-stream loads and pending stores explain the 2.3-vs-5 µops/cyc pass wall. Built the
skewed work image (slot s → s + 4·(s>>8), 64B pad per 4KB, pad-chunked loops so runs never cross
boundaries). **MEASURED WORSE: 16K 37.3→36.0, 64K 30.4→28.4, 4096 36.3→35.0 — consistent across
rows ⇒ 4K-aliasing is NOT the wall on Raptor Cove.** Reverted to the identity map; kept the
chunk-hoisted pointer structure (≈1% of drift vs round 8, MKL-normalized 0.676 vs 0.688; cleaner
base for mixed-radix work). Suites green; oracle ≤6.5e-16 throughout.

Standing at 0.68-0.69× on the best rows after 9 rounds. Eliminated so far as pass-wall mechanisms:
µop count, twiddle traffic, loop order, block pairing, k-unroll depth, register economy,
4K-aliasing. STILL UNTESTED: true generator-emitted straight-line pass bodies (explicit instruction
scheduling — the one dimension C++ source cannot control); radix-8 mixed plans (constraints derived
in round 8). Also worth a probe next session: hardware-counter-level diagnosis of the pass loop
under WSL2 alternatives (VTune on Windows native reads the same silicon — the bench harness is
Linux-side, but a Windows-native pass-loop microbench + VTune would name the stall class directly
instead of eliminating hypotheses one at a time).

## strided-v3 ROUND 10 (2026-07-04): VTune NAMES THE WALL — DTLB + L1 latency, NOT scheduling

Installed VTune 2026.0 natively (oneAPI offline image, vtune component only; build/vtune_run.ps1;
microbench = clang++ native ip4-AoS 16K at 36 GF/s ≡ the WSL gcc numbers ⇒ representative).
**uarch-exploration verdict: Retiring 44.3%, Front-End 1.8%, Bad-Spec 0%, Back-End 58.2% — inside:
DTLB Overhead = 24.5% of clockticks (all Load-STLB-Hit), L1 Latency Dependency 16.2%, FB Full 9.3%,
L3 9.6%, Store 7.7%.** The pass wall is MEMORY-LATENCY class. This REFUTES the generator-scheduling
theory (the instruction stream retires fine) — 10 rounds of code restructuring could never have
moved it.

Huge pages (the textbook DTLB fix) tested via THP=always + a MADV_HUGEPAGE-backed TlsfAllocator
pool (build/bench_fft_hp.cpp): **GLOBAL REGRESSION — and MKL's own rows drop ~20% on huge-paged
data too** (51→41 @16K). Mechanism: 2MB pages surrender 4K frame randomization ⇒ power-of-2 strides
map to IDENTICAL L2 sets ⇒ conflict misses swamp the DTLB savings. Re-testing the round-9 pad on
top (CRD_FFT_IP4_PAD, opt-in) did not rescue it (tb-only padding can't fix the huge-paged io/table
conflicts). THP restored to madvise; default build unaffected, suites green.

**Standing conclusions:** (1) the DTLB tax on 4K pages is structural for strided FFTs and likely
paid by MKL as well — VERIFY next by installing intel.oneapi.win.mkl.devel from the same extracted
image (scratchpad\oneapi_extracted) and running the SAME uarch-exploration on an MKL loop: the
comparative top-down (their DTLB/L1-lat/FB-full vs ours) is the precise remaining-gap decomposition.
(2) If MKL pays the same DTLB, the real deltas are L1-latency-dependency and FB-full — i.e., MKL
keeps more of its loads L1-RESIDENT (smaller per-pass footprint: their monolithic kernel's working
set per pass vs our full-array sweeps + big twiddle tables). Levers that attack THAT: fold more
layers per sweep (radix-8/16 with in-register second layer — fewer full-array passes), shrink
twiddle-table traffic (compute-on-the-fly recurrences or smaller factored tables), block passes to
L1-sized tiles. All now aimed by counters, not conjecture.

## strided-v3 ROUND 11 (2026-07-04): COMPARATIVE VTune — the tables were the TLB killer; +8-17%

Installed native MKL (oneAPI modify-install; component list is COLON-separated; a product already
installed needs --action modify) and profiled the SAME 16K loop both ways. **The decisive
comparison: DTLB overhead — Cerid 24.5% of clockticks vs MKL 2.3%. L3 bound: 9.6% vs 0.2%.** Their
L1-latency (22.1%) and FB-full (20.8%) are as bad as ours or worse — the ENTIRE net gap was
TLB + L3, and the cause was OUR TWIDDLE TABLES: 3-set pre-dup'd = 512 KB/transform (2× the data!)
streamed across ~128 pages every transform. MKL's tables are compact.

**Fix (aimed, not guessed): store only w1; compute w2 = w1², w3 = w1·w2 in-register** — on the
dup'd form this is pure elementwise FMA (no shuffles), and the port headroom (our retiring 44% vs
their 81% 3+-port cycles) absorbs it. Tables shrink 3×. **Result: 16K 39.76 (+8%, 0.73×) · 64K
35.51 (+17%, 0.87× — now BEATS FFTW 34.75) · 4096 38.70 (0.70×). Oracle ≤7.1e-16.** Session
cumulative at 16K: 32.9 → 39.8; at 64K: 26-27 → 35.5.

Note for the record: MKL native = 47.9 GF/s @16K (23.95 µs) — the WSL/native ratio holds; and a VS
2026 auto-update (toolset 14.50→14.51) invalidated every win build dir mid-session (CreateProcess
failures on 8.3 short paths; fix = delete + reconfigure, per the corrupted-build-dir doctrine).

**Remaining, by the same counters:** our residual DTLB (tables now ~170 KB — next: drop the dup
(halve again, runtime dup_even/dup_odd) or subsample one W_n table), and the L1-latency/FB-full
band that MKL ALSO pays (structural for strided FFT). Re-profile after the table shrink; if DTLB
lands at MKL's ~2%, the honest remaining delta is the last ~10-15% of pass-loop efficiency —
attack with layer-folding (radix-8/16 constraints derived in round 8) or accept the counter-proven
wall. THE METHOD CHANGED THIS ROUND: counters first, then code — two measurements, two wins.

## strided-v3 ROUND 12 (2026-07-04): non-dup tables (load_dup_pairs) — tables 6× smaller total

Added `load_dup_pairs` (vec4d: vpermpd 0x50 over a 16B load; exact) and switched the w1 tables to
NON-duplicated storage (q doubles/pass; total (n−4)/3 per array ≈ 43 KB @16K — 6× below round 10's
512 KB). One transcription bug caught by the oracle mid-round (the LAST-pass chunk pointer kept the
dup-era 2·kc offset — different indentation dodged a replace-all; symptom maxrel 2e+06 @16K; the
bisect-with-scalar-builds isolated indexing vs intrinsic in one run). Fixed: **oracle ≤7.4e-16;
4096 38.3 · 16K 39.6 (wash vs round 11) · 32K 34.4 (+7%) · 64K 34.2 (≈, within the row's drift).**
Suites green (linux 281/29; win-debug rebuilt fresh after the VS toolset update, 281/29 by direct
binary run — ctest granularity shifted 27→25 on reconfigure, discover artifact only).

Board vs MKL now: 4096 0.69× · 16K 0.74× · 32K 0.63× · 64K 0.82×. NEXT (in order): (1) re-run the
VTune uarch collection on the new binary — confirm DTLB ≈ MKL's 2-3% and read what now tops the
stack; (2) 32K is the weakest ip4 row — check its pass/gather split; (3) coverage for promotion
(odd log2 via one radix-2 stage, f32 Vec8f edition, inverse) once the f64 shape is final.

## strided-v3 ROUND 13 (2026-07-04): ODD-LOG2 SUPPORT — the whole mid-band on the new engine

Correction from round 12: 8192/32768 are odd-log2 and never ran ip4-AoS (their "moves" were drift
around the banked path). Built dual-parity support: **n = 2·4^k = two half-length ip4 transforms on
the even/odd decimations + one final radix-2 combine** (X[j] = E[j] ± W_n^j·O[j], sequential
streams, dup-at-load table). The rev table absorbs the ×2+parity io mapping; the odd gather
co-processes BOTH halves per stream (8 complex staged; lane (v,h) at 2·(2v+h)) so io lines stay
fully consumed; passes run per half automatically (blocks never straddle: 2·len ≤ nh) with a
per-half single-block final; the fold constants are parity-invariant. Correct on FIRST run:
**8192 37.03 (+21% vs banked, 0.71×) · 32768 37.25 (+8%, 0.69× — BEATS FFTW 36.1)**; 16K at
FFTW-parity (37.9 vs 38.4); oracle ≤8.0e-16; both suites green.

**The whole 4K-64K mid-band now runs ip4-AoS at 0.69-0.75× MKL (64K up to 0.87×)** — from
0.56-0.66× at the day's start. Remaining next steps: VTune re-profile of the new binary (DTLB
check), f32 Vec8f edition + inverse for dispatch promotion, and the 1K-2K band (currently sh).

## strided-v3 ROUND 14 (2026-07-04): 1K-2K on the engine (one line) + the CONFIRMATION PROFILE

Lowered the ip4 gate to n ≥ 1024 (both parities already supported): **1024 43.3 (0.67×) · 2048
38.5 (0.92× — MKL's weak row)**; same-run band: 8K 39.1 · 16K 39.4 · 32K 38.3 (beats FFTW) · 64K
34.8 (0.86×). Oracle ≤8e-16; suites green (win-debug rebuilt on the new toolset, 281/29).

**Re-profile of the round-13 binary (uarch + memory-access, native): DTLB 24.5% → 2.0% of
clockticks = EXACTLY MKL's 2.3%. L3 9.6% → 0.3%. FB-full 9.3% → 1.9% (MKL: 20.8 — ours BETTER).
L1-latency 15.4% (MKL 22.1 — ours better). The memory war is WON — every memory counter is at or
beyond MKL's level.** The remaining gap is now PURE INSTRUCTION COUNT: 211.0B retired vs MKL's
168.3B (+25%) at close CPI (0.321 vs 0.273). Prime suspects for the surplus: the gather's per-lane
vector builds (scalar-ctor lane assembly per fold unit), twpow's 8 ops/iter (the price of small
tables — net-positive but countable), the odd-size extra combine pass, and Bad-Spec 3.6% (gather
loop branchiness). THE ONE-SHOT-CRUSH TARGET: cut ~40B instructions — start with perf-annotate on
the gather (lane builds → 128-bit-pair loads + insertf128 if clang isn't already emitting them),
then the pass-loop's addressing arithmetic. f32 Vec8f edition + inverse remain for dispatch
promotion (unchanged plan).

## strided-v3 ROUND 15 (2026-07-04): the instruction cut — twpow was FREE; pair128 real but small

Two aimed edits at the 43B-instruction surplus: (1) HYBRID tables (3-set for len ≤ 1024, w1-only +
twpow above — kills ~30K instr/transform of twiddle arithmetic): **measured FLAT** ⇒ twpow's
instructions were port-absorbed, i.e. a chunk of our instruction surplus is FREE and the raw
retired-count delta vs MKL overstates the real gap. (2) `load_pair128` for the gather's fold builds
(3 µops vs 4-5 scalar assembles): **+1-2% real** — 16K 39.1 (0.757× normalized, best yet), 32K 38.6
(0.73×), 2048 37.9 (0.90×). Oracle ≤7.6e-16; both suites green. Hybrid kept (harmless, tables
still ≤ tens of KB).

**Honest end-of-day position: 0.71-0.90× across the whole 1K-64K mid-band on a single engine**
(morning: 0.56-0.85 scattered over three engines). The "one-shot crush" did not land — the
instruction-surplus theory decomposed into (a) free instructions (twpow), (b) small real wins
(pair128), and (c) an unpinned remainder. NEXT SESSION (in order): re-run VTune to RE-COUNT
instructions + read bad-spec/store-latency after these edits; fully unroll the gather staging loop
(16 iters × loop overhead + its branchiness = the last measured suspect); then f32 Vec8f + inverse
+ dispatch promotion. The counters, not conjecture, keep the aim.

## strided-v3 ROUND 16 (2026-07-04): the RECOUNT — instruction count is DEAD as a lever; 0.85× native

VTune recount on the round-15 binary: **Instructions 216.8B (UP from 211.0B) while clockticks fell
67.8→66.7B and CPI 0.321→0.308 — we run MORE instructions FASTER. The retired-count delta vs MKL
is now formally refuted as the gap's cause.** The profile has CONVERGED to MKL's: DTLB 2.0 (≡2.3),
L1-lat 23.1 (≈22.1), FB-full 3.9 (≪20.8), L3 0.5 (≡0.2), bad-spec 2.3 (vs 0). Remaining above
MKL: **Store Bound 13.3% vs 6.4%** (in-place store RFO latency; store-latency 14.9% of clockticks)
— the LAST counter with daylight. **Native-to-native 16K: 40.6 vs 47.9 GF/s = 0.85×.** (The WSL
harness ratio reads lower — 0.73-0.76 — because MKL's Linux build posts 52-54 there; both
measurements are honest, environments differ.)

Levers left, by the counters: (a) store-side — the in-place pass stores wait on RFO even for
L2-resident lines; candidates: nontemporal is wrong here (data is re-read), but pass-fusion reduces
STORE COUNT per element (the two-layer fusion from round 4 was register-starved — a 2×2-only
partial fusion of the FIRST two combine passes may fit registers and halves one pass's stores);
(b) bad-spec 2.3% — gather staging loop unroll; (c) the f32/inverse/promotion track (unchanged).
Expected combined ceiling from (a)+(b): ~5-8% ⇒ ~0.90× native. Beyond that, the remaining delta
is MKL's higher retiring density (61.6 vs 53.2) — closing IT means fundamentally fewer/wider work
per slot: the radix-8 mixed plan (constraints derived round 8) remains the only structural card.

## strided-v3 ROUND 17 (2026-07-04): RADIX-8 BUILT, MEASURED, REFUTED — the structural card played

Built the full mixed-radix machinery: plan [4,4-fold | 8,8 | 4…4-last] for nh ≥ 4096, generic
mixed-radix digit reversal (Horner from the last-pass radix; **the mixed rev is NOT an involution —
the gather needs the INVERSE map rev[slot(j)] = j**, a bug the pure-4 involution had masked),
7-set tables for the r8 passes (tiny — small lens), bf8 = 7 input cmuls + 2×bf4c (twiddle-free
core) + the W8 diagonal ((1−i)/√2 via swap/addsub/swap·r2; −i via swap·[1,−1]; −(1+i)/√2 via
addsub·[r2,−r2]). **Correct on first run (≤6.9e-16, accuracy IMPROVED — fewer twiddle
applications). Performance: v1 −3%, block-paired −1% vs the radix-4 plan** — the W8 shuffle
diagonal + 7-set twiddle traffic outweigh the saved pass sweep on Raptor Cove. REVERTED to pure
radix-4 by constant (nr8 = 0U; the machinery stays, tested and oracle-green — one flip re-enables
it for uarches with cheaper shuffles or dearer stores). Verified post-revert: 16K 39.24 · 32K 38.22
· 8K 38.31 · 2K 37.74; both suites green.

**CAMPAIGN POSITION after 17 rounds:** every counter-visible lever and the one structural card have
now been built and measured. The engine stands at 0.71-0.92× MKL across 1K-64K (native 16K 0.85×),
beats FFTW at 32K, matches it at 16K/64K, deterministic, allocation-free, WASM-portable. The
honest residual vs MKL: their higher retiring density on hand-scheduled asm — the one dimension
outside our portable-intrinsics mandate (ADR-0082; the WASM goal makes asm a non-option by policy,
per the microkernel decision memory). REMAINING WORK (promotion track, mechanical): f32 Vec8f
edition, inverse transforms, dispatch flip + full-board rerun + docs/bench entry.

## strided-v3 ROUND 18 (2026-07-04): PROMOTED — ip4-AoS is the shipped f64 mid-band engine

Inverse f64 built as template<bool INV>: conjugation via the fold's sgn high-pair flip (ZERO extra
ops — only t.hi feeds mix_lo_hi), ns-negated compile-time fold constants, X1/X3 swap in bf4/bf4c,
negate-at-load for table twiddles (free), conjugated W8 diagonal variants (r8 kept correct though
disabled). twpow propagates conjugation automatically (w2 = w1² of a conjugate). **Inverse passed
the suite round-trips on FIRST run.** Dispatch flipped: `CRD_FFT_IP4AOS` (opt-in) →
`CRD_FFT_DISABLE_IP4AOS` (opt-out); f64 1K..64K both parities BOTH directions now route to ip4-AoS
by default, superseding sh/ds on those rows. First MSVC compile of the engine: clean (25/25
win-debug). One transient gcc ICE, retry-passed. Default-build board: **1K 40.7 · 2K 37.8 · 4K
38.3 · 8K 38.4 · 16K 39.3 · 32K 38.3 (BEATS FFTW 36.6) · 64K 34.2; oracle ≤7.6e-16.**

REMAINING (follow-up session): the f32 Vec8f edition (fold geometry re-derivation for 4-complex
vectors) — f32 1K-64K still routes to the banked sh path; then retire the superseded f64 sh/ds
code once the full-sweep confirms nothing else references it, and the docs/bench board entry.

## strided-v3 ROUND 19 (2026-07-04): f32 TWIN-FOLD EDITION + per-size dispatch + retirement verdict

**f32 port via the twin-unit insight:** Vec8f = 4 interleaved complex = TWO 128-bit fold units per
vector. Every f64 128-bit-granularity fold op has a 64-bit-granularity f32 twin (concat_lo/hi →
unpack_c_lo/hi via unpacklo/hi_pd on ps-cast; mix_lo_hi → blend_c_odd 0xCC; load_pair128 →
load_c_quad via set_m128+permute4x64 0xD8; full stores → store_c_lo/hi halves), the lbuf index
formulas are IDENTICAL in T-units, pass loops are elementwise with k-strides in C = complex-per-
vector, and constants twin-duplicate across 128-halves (mkv builder). Even parity twins over v
(units v, v+1); odd parity twins over h (lane offsets 4v, 4v+2 adjacent). **PASSED THE FULL SUITE
ON FIRST COMPILE+RUN** (281/29, f32 fwd+inv+round-trips; maxrel ≤1.4e-07 on the bench).

**f32 matched-state A/B vs the Vec8f sh path:** wins 2048 (+.06), 32K (+.05), 64K (+.10), tie 16K;
LOSES 4096 (−.12), 8192 (−.07), 1024 (−.04) ⇒ per the never-regress rule, PER-SIZE dispatch: f32
takes ip4-AoS at {2048, 16K, 32K, 64K}, keeps sh at {1024, 4096, 8192} (still a pure function of
size ⇒ deterministic). Every f32 row on its measured winner: 0.58-0.82× board, no regressions.

**Retirement verdict:** sh/ds are NOT retirable — load-bearing for f32 {1K,4K,8K}, all >64K paths
(four-step/six-step), and the six-step/M13 fused branches reference sh structures directly.
Nothing is dead code. f64 sub-plan rows (four-step's row FFTs) now route through ip4-AoS
automatically via execute() — correctness suite-verified.

Gates: linux 281/29 ✓ win-debug MSVC 25/25 ✓ per-slice-check (win-debug+asan+shipping+tidy,
fresh-reconfigured after the VS toolset update) — run at close.

**THE ORIGINAL SPEC (superseded in part by the archaeology above):**
1. ONE monolithic engine: log4(n) radix-4 STRIDED passes over an AoSoA work image, in-place or
   Stockham ping-pong (autosort beats their bit-reversal — our ordering advantage).
2. First pass fuses interleaved→AoSoA (the existing S1 deinterleave discipline); last pass fuses
   AoSoA→interleaved + final ordering.
3. FMA-fused twiddles from per-pass tables (vector loads, no broadcasts — their pattern), plus
   specialized constant-twiddle passes for the W_4/W_8 angles.
4. Radix-4 body = ~12 live registers, zero spills by construction; radix-8 first pass optional.
5. Portable: all through crd::math::simd (the fallbacks exist); deterministic: fixed plan, fixed op
   order, single-rounded FMA. Expected: MKL-class 45-50+ GF/s in the 4K-64K band; then feed the
   block-split/merge structure at ≥1M from the same engine (their large-n architecture).


> Design dossier, written 2026-07-04 at the close of the 8-session FFT crush campaign. The measured
> boards + every banked/refuted lever live in `docs/bench/2026-07-03-v10-fft-remeasure-and-midband.md`.
> This file exists so the NEXT session can execute the build without re-deriving anything.

## The measured premise (why this architecture, not more codelet levers)

- After FMA + AoSoA + factored twiddles + store-early + deep-splits, the f64 mid-band sits at
  30–34 GF/s — **the straight-line-codelet architecture ceiling**: FFTW (the best of that class)
  runs 33–38 GF/s on this machine (i9-14900K, 1T, AVX2) and we beat it on several rows already.
- MKL runs 50–54 GF/s in the same band with a DIFFERENT architecture: **many tiny radix-4/8 passes
  streaming L1/L2, 8–16 live registers each** — zero spills, near-peak FMA density. The pass-count
  model reproduces its measured times (16K f64: 7 radix-4 passes × 512 KB r+w at L2 bandwidth
  ≈ 19 µs ≈ MKL's 22 µs).
- Our straight-line leaves inherently hold ~N values live (spill probe: 40K+ residual stack ops
  even after store-early — `build/crd_fft_spillprobe.sh`). No scheduler fully escapes that.
- The June Stockham v1 losses (0.29–0.47×) are each individually FIXED levers now: it was non-FMA,
  interleaved-with-shuffles-per-pass, used spilling radix-16/32 codelets, and streamed a big
  twiddle table. None of those defects is architectural.

## The reframing that makes this cheap to build

The deep-split (ds) IS this architecture with K=3 and big stage factors. **Stockham v2 = the
generalized K-stage driver** over the SAME machinery:

    n = A · B1 · B2 · … · Bk · Z
    S1   = codelet{A}_stage1_fused_sh_cs/_csf   (exists: deinterleave + W_n twiddle + transpose)
    Si   = codelet{Bi}_fused_notr_ss            (exists for 16/32/64/128; ADD 8; broadcast W_rem)
    Sz   = codelet{Z}_batched_sc / _strided_sc  (exists: reinterleave / natural-order finish)

- Middle stages are the `fused_notr_ss` AoSoA kernels — tiny factors (8/16/32) = tiny live sets =
  spill-free, FMA-dense; that IS the MKL pass shape, already emitted by our generator.
- The twiddle identity per stage: after S1 consumed W_n, stage Si over factor Bi applies
  W_{Bi·…·Bk·Z} (the remaining product) with the same broadcast-compact [k·C + v] table shape the
  3-stage ds already uses (`m_ds_twr` — becomes per-stage arrays).
- Buffers ping-pong m_sh_t ↔ m_sh_s (AoSoA, both exist). Stage plan per size = a FIXED table
  (deterministic, machine-independent), measured once and hardcoded like every split today.

## What to build (the session plan)

1. Generator: `emit_fused_notr_ss` for N=8 (both types; f32 lanes=8 means an 8-point stage is ONE
   vector row — verify it's not degenerate; if it is, f32 uses 16 as the minimum stage factor).
2. fft.hpp: generalize the ds branch into a K-stage driver — `m_ds_b/m_ds_c` become a small
   fixed-size stage array (factor + twiddle-table pointer per stage); the S2 switch becomes a loop.
3. Stage-plan sweep per size (4K–512K first, then re-try 1M+ cache-blocked): candidates per size =
   {2,3,4,5}-stage plans over factors {8,16,32,64}; measure, fix the winners into the table.
   Measured anchors: 2-stage wins ≤4K (L1-resident; 4096=16·16·16 REFUTED at 30.5 vs 34.4 GF/s);
   3-stage wins 8K–512K so far; the 4-stage-with-tiny-factors region is the unexplored MKL zone.
4. 1M+: run the same engine cache-blocked inside the four-step's P1/P2 (replace the hier sub-FFT
   kernels), or as a six-step variant — THIS is where Van Loan's frameworks book feeds directly.
5. Gates per increment: linux suite (oracle 1e-12 both dirs) + win-debug ctest + matched A/B vs the
   banked row — a loss or flat reverts and gets recorded (house doctrine).

## Future-proof / works-on-every-machine constraints (non-negotiable)

- All kernels through `crd::math::simd` only (AVX2 / SSE2 / WASM-SIMD128 / scalar backends);
  NO raw intrinsics in fft.hpp, no asm (ADR-0082 + the WASM goal). Lane widths parameterized:
  f64 L∈{4 (AVX2), 2 (SSE2/SIMD128)}, f32 L∈{8, 4} — the emitters take L from `_PREC`; the AoSoA
  layout and transposes need L=2 editions (transpose2x2 = one swap) when the SSE2/WASM tier lands.
- Dual-body emission stays (SIMD under `NDEBUG||__OPTIMIZE__`, bit-identical lane-scalar else) —
  the MSVC /Od 1.4 MB-frame scar.
- Determinism: the stage plan is a pure function of (size, type) — identical on every machine; op
  order fixed; FMA single-rounded (std::fma scalar mirror). Cross-ISA builds differ in bits exactly
  as today's contract states (per-build determinism; the {1..16} thread moat holds).
- No allocation in execute paths; plan-owned buffers only (house rule).

## ≥1M: FIVE-STEP BUILT + MEASURED + REFUTED (2026-07-04) → THE SIX-STEP IS THE BUILD

The five-step below was IMPLEMENTED same-day (`execute_five_step`, kept opt-in under
`CRD_FFT_ENABLE_FS5`): correctness proven (suite green, 9.3e-16), **perf REFUTED** — f64 1M 7.3 vs
the four-step's 6.3 ms, 8M 97.6 vs 62.7 (worse with size). Mechanism: its multirow FFT passes read
n_i rows STRIDED across the whole array = **64–256 concurrent streams** (the AoSoA/SoA stream-count
law again, at its worst). Takahashi's own framing confirms: the five-step's fixed-innermost-loop
targets VECTOR machines; **the SIX-step — transposes making every sub-FFT CONTIGUOUS — is the cache
variant and the correct ≥1M build**: T + contiguous-FFT + twiddle(fused) + T + contiguous-FFT + T,
every pass either a tiled transpose (near stream rate, our v14-d expertise) or an L1-local
contiguous transform (1–2 streams). REUSE from the fs5 build: the twiddle tables, m_tbuf ping-pong,
the pass drivers — only the FFT passes change from strided-multirow to transpose-then-contiguous.
Next session: implement six-step, A/B per row vs the four-step at 1M–8M, Van Loan §3.3 + Takahashi
6.3–6.4 give the blocking discipline.

## SIX-STEP v1 BUILT (2026-07-04, same session): correctness PROVEN, transpose quality = the gate

`execute_six_step` implemented (square 1M/4M: in-place tile-pairwise transposes + contiguous row
FFTs via the hoisted m-point sub-plan running the L1-resident sh 2-pass; step-3 twiddle fused into
the middle transpose from the four-step's factored tables; the T→FFT→T∘W→FFT→T algebra verified
symbolically to reproduce ω_m^{cq}·ω_n^{jq}·ω_m^{jp} = the DFT, natural order I/O). Suite green,
8.2e-16. **v1 measured SLOWER (f64 1M 8.8 vs 6.3 ms; f32 1M 0.30×): the ELEMENT-SCALAR transposes
are the entire gap** — strided 16B touches per cache line, no SIMD; at f32 1M the 3 transposes cost
~5 ms alone. Kept opt-in under `CRD_FFT_ENABLE_FS6`.

**SIX-STEP v2 BUILT + PARTIALLY BANKED (2026-07-04, same session):** the SIMD micro-tile pair-swap
transpose (kW×kW deint-load → per-plane register transpose → CONTIGUOUS interleaved stores on BOTH
halves; T2's W_n^{r·c} by per-row vector recurrence stepped W_n^{r·kW}, reseeded from the factored
tables per block; ktb 64 @m=1024 / 32 @m≥2048 for the L2 pair budget). **1M DEFAULT-ON, both types:
f64 6.28→5.38 ms (+17%, 19.5 GF/s, 0.79× MKL) · f32 2.75→2.26 ms (+22%, 0.82×) — the best 1M ever
measured here.** 4M measured WORSE (f64 33.5 vs 29.9; ktb=32 didn't recover it): at 64 MB the
in-place pair-transpose's TWO-SIDED far accesses defeat DRAM page locality — open fix = fuse T3
into sweep B's strided-out stores (one-directional streams; codelet strided-store shapes exist),
or out-of-place transposes through m_tbuf with the T3+copy fused the same way. Suite + win-debug
green; accuracy 1.1e-15 / 2.4e-7 (recurrence class ✓).

**Six-step v2 spec (as built; the 4M fix is the remaining item):**
1. SIMD micro-tile transposes from EXISTING primitives: per 4×4 (f64) / 8×8 (f32) complex block —
   `load_complex_deinterleaved` ×4/8 → `transpose4x4/8x8` on the re-set and im-set → 
   `store_complex_interleaved` ×4/8 — full-line utilization both sides; L2-blocked 64×64 outer
   tiles; this is exactly the v14-d tensor-permute discipline that CRUSHED HPTT 1.11–1.31×.
2. T2's twiddle w(r,c) = W_n^{r·c} is geometric along each row → per-row recurrence reseeded from
   the factored tables every K (the four-step twiddle pass already does this; ~1.5e-7/1e-15 class).
3. f32 needs the 8×8 micro-tile; rectangular sizes (2M/8M) via the out-of-place tiled transpose
   through m_tbuf (7th pass avoided by fusing the final transpose into sweep B's strided-out store —
   codelet strided-store shapes exist).
4. Expected: 3 transposes at near-stream (~0.8-0.9 ms each @1M f64) + 2 sweeps (~0.9 ms) ≈ 4.4 ms ≈
   MKL's 4.2. The sweeps are already proven at 30-39 GF/s (the sh engine).

## SESSION 9 (2026-07-04): T3-rechain refuted · THE 4-STAGE K-DRIVER BANKED 8K–1M

**T3-fusion rechain (2 oop transposes + 1 in-place): REFUTED** — 1M 5.38→6.69 ms, 4M 33.5→40.4.
Mechanism: at 1M the whole array is L3-RESIDENT; the all-in-place chain keeps every pass L3-hot,
the x↔t ping-pong doubles the live footprint and thrashes L3. `fs6_transpose_oop` kept for A/B.

**THE 4-STAGE DEEP-SPLIT (n = A·B1·B2·C) — the K-stage sweep, BANKED:** S2 = notr B1 (W_{B1B2C}
table), S3 = notr B2 per k1 block (W_{B2C}), S4 = strided C per (k1,k2) → d[kA+A·k1+AB1·k2+AB1B2·kC]
(recursion exact vs the 3-stage proof). 8-point notr/strided emitted. Winning plans (both types
unless noted): 8K=16·8·8·8 · 16K=16·16·8·8 · 32K=16·16·16·8 · 64K=16^4 · 128K=32·16·16·16 ·
256K=32·32·16·16 · 512K=32·32·32·16 · **1M=32^4 f32 ONLY (2.24 ms, 0.91× — beats the six-step's
0.82×; f64 1M keeps the six-step: 5.38 vs 5.88)**. Gains vs the 3-stage: f64 512K **+15%** (22.6
GF/s) · 256K +7% · 128K/64K/8K +5% · f32 512K **+20% (0.90×)** · 64K +8%. **≥2M REFUTED a third
time** (4-stage 4M 41.8 / 8M 97 vs four-step 29.9/62.7) — past L3, full-array multi-pass loses to
the four-step's blocked structure; that law is now settled (five-step, six-step@4M, 4-stage@≥4M).

**Boards after session 9 (one run):** f64 1024–8M: 0.60-0.63× (1K-2K) · 0.59-0.64× (4K-32K) ·
0.69-0.76× (64K-128K) · 0.72-0.75× (256K-512K) · **0.90× (1M)** · 0.86× (2M) · **0.95× (4M)** ·
0.93× (8M) — beats FFTW at 16K and everywhere ≥512K (1M by 1.23×). f32: 0.57-0.60 (1K-2K) ·
**0.81 (4096)** · 0.64-0.73 (8K-64K) · 0.71 (128K) · **0.88 (256K) · 0.85 (512K) · 0.80 (1M)** ·
0.77 (2M) · **0.95 (4M)**. Suites green throughout (linux 281/29 + win-debug 27/27).

**Remaining to full crush:** (a) f64 4K-64K plateau ~31-33 GF/s vs MKL ~50 — stages are now tiny
and spill-free; the residual = per-pass overheads (S1 transpose shuffles + 4 materialized passes vs
MKL's fused-register chains) — next lever: 5-stage trial + S1-transpose elimination (store
UNtransposed + notr variants reading strided-lanes) or in-register two-stage fusion; (b) f32/f64
1K-2K (0.57-0.63×) — single fully-fused kernels; (c) 2M (0.77-0.86×) — the four-step's last stand;
try f32 2M ds=32·32·32·64 measured flat — retune or accept.

## The five-step formulation (kept for reference; the six-step reuses its identity)

**Takahashi's FIVE-STEP FFT (his ref [23]) is the ≥1M architecture.** n = n1·n2·n3 (choose all
≈ n^(1/3); 1M f64 = 128·128·64):

    Pass A: n1·n2 simultaneous n3-point multirow FFTs      (batch axis CONTIGUOUS — our batched
                                                             AoSoA leaves, cache-blocked in strips)
    Pass B: twiddle ω_{n2n3}^{j2·k3} FUSED into a tiled transpose x(j1,j2,k3) -> x(k3,j1,j2)
    Pass C: n3·n1 simultaneous n2-point multirow FFTs
    Pass D: twiddle ω_n^{j1·k3}·ω_{n1n2}^{j1·k2} FUSED into the second tiled rearrangement
    Pass E: n3·n2 simultaneous n1-point multirow FFTs       (output natural order by the identity)

Why this wins where our four-step sits at 0.66×: **five fully SEQUENTIAL passes, zero strided
gather/scatter phases** (the four-step's stride-bound phases are the measured 1M wall). Traffic
model: 5 × 32 MB round-trips ≈ 160 MB ≈ 4.0 ms at stream rate — **MKL's measured 4.2 ms at 1M**.
Reuses: batched AoSoA FMA leaves (exist) · tiled blocked transpose (v14-d permute expertise +
transpose4x4/8x8) · factored/compact twiddle tables (exist). Cache-block Pass A/C/E over batch
strips (~512 KB working sets); NT stores on the transpose writes. Build AFTER the mid-band K-stage
sweep; A/B against the four-step per row (2M–8M too — the same structure).

Extracted text cached: `build/tak_ch6.txt` (five/six-step formulations), `build/vl_toc.txt`,
`build/tak_toc.txt`. Book PDFs: `C:\Users\abici\Downloads\tmpbooks\` (Van Loan · Takahashi · Chu).
Still to mine next session: Van Loan §1.7 (Stockham autosort frameworks, book p.49) + §3.3 (large
single-vector FFT, book p.139) for the mid-band multirow kernel forms; Takahashi ch. 6.4+ (blocked
six-step, if present — check pages after 45) for the block-size discipline.

## Resources

Fetchable (no help needed): Frigo "A Fast Fourier Transform Compiler" (PLDI'99, fftw.org) ·
FFTS paper (Blake/Witten/Cree, IEEE TSP 2013) · SPIRAL short-vector papers (spiral.ece.cmu.edu) ·
VectorFFT source (github.com/Tugbars/VectorFFT) · Bailey's four-step paper · Agner Fog tables.

Requested from the user (paywalled books, priority order):
1. **Van Loan — "Computational Frameworks for the Fast Fourier Transform" (SIAM, 1992)** — the
   definitive four-step/six-step/Stockham memory-framework treatment; feeds step 4 directly.
2. **Takahashi — "Fast Fourier Transform Algorithms for Parallel Computers" (Springer, 2019)** —
   the FFTE author's modern blocked/six-step/nine-step practice (FFTE beats FFTW/MKL on some sizes).
3. **Chu & George — "Inside the FFT Black Box" (CRC, 2000)** — Stockham variant taxonomy
   (self-sorting vs ordered, radix scheduling); useful, lower priority.
