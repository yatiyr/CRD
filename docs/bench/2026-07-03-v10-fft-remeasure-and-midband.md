# 2026-07-03 — v10 FFT: full re-measure (f64 + f32) + the mid-band campaign opening

- **Trigger:** the user-directed FFT full-crush campaign (reopens ADR-0092's deferred parity).
- **Machine:** i9-14900K, WSL2, 1T pinned, MKL/FFTW/PocketFFT via `scripts/run_bench_fft.sh` +
  the new `runtime/examples/bench_fft_f32.cpp` (f32 vs MKL DFTI, best-of-15).

## The honest re-measured boards (2026-07-03)

### f64 (vs MKL; also beats/loses FFTW as noted)

| n | ratio vs MKL | note |
|---|---|---|
| 1K–32K | **0.29–0.47×** | ⚠ the June board listed these rows as "parity regime (NOT BENCHED)" — refuted by measurement |
| 64K | 0.46× | four-step opt-in measured FLAT — stays direct |
| **128K** | 0.39 → **0.50×** | **four-step opt-in banked (+12%)** |
| **256K** | 0.46 → **0.49×** | **four-step opt-in banked (+6%)** |
| 512K | 0.49× | four-step (default ≥512K) |
| 1M | 0.65× | |
| 2M | 0.87× | |
| 4M / 8M | 0.93× / 0.92× | beats FFTW 1.25×/1.27× here |

### f32 (vs MKL DFTI)

| n | ratio | note |
|---|---|---|
| 256K | 0.72× | |
| 512K | **0.55×** | worst row: P2 = 512-pt NON-hier sub-FFTs |
| 1M | 0.71× | June's "~0.90× with fusion" does not reproduce on today's harness/state |
| 2M | 0.78–0.80× | |
| 4M | 0.86–0.93× | |

**M16-B POC verdict (measured A/B):** identical to the banked default within noise on every size —
correctly never promoted; not a lever. (M19-A remains REJECTED per the in-code record.)

## The single diagnosis, now measured across both types

**Codelet quality is the wall everywhere it loses** (June's own L1-resident n=1024 finding, now
extended): f64 runs 12–27 GF/s at every size below 1M while FFTW/MKL run 27–58; the f32 512K trough
is exactly its non-hier P2. The proven counterexample INSIDE this codebase: the tuned f32 hier
codelets beat MKL batched (1.53×/1.42×/1.17×). The campaign work is therefore:

1. **f64 codelet regeneration** (gen_fft_codelets.py: the f32-style register-scheduled split-radix,
   Vec4d edition) — lifts leaves, hier stages AND Stockham passes engine-wide.
2. **P2 hier codelets for 64/128/256/512 points, f32 + f64** — closes the four-step troughs
   (f32 512K 0.55×, f64 128K–512K ~0.5×) the same way the f32 256K fix did (0.33→0.85× precedent).
3. Then re-sweep the four-step opt-in table with the new P2 kernels (64K likely flips to a win).

This is the deferred "Spiral-class" work, now scoped by measurement instead of assumption. It is a
dedicated multi-session campaign; the generator, the winning f32 exemplars, and today's boards are
the complete starting context.

## Session 2 (2026-07-03, same day): the wiring lifts — existing codelets, new dispatch

**Discovery:** the "missing" P2 speed was a WIRING gap, not a generation gap — `hier_codelets.hpp`
already ships f64 `codelet{16,32,64}_batched` (and the f32 256=16×16 hier), all verified
IN-PLACE-SAFE (every generated tile loads all inputs before its first store). The generator script
itself (`build/gen_subfft_m3.py`) is gone — the products remain.

| Lift | Result |
|---|---|
| **Batched-leaf gate**: n∈{16,32,64} batched forward → the generated leaf codelets (both types), in place, replacing bit-reversal + batched radix-8 | the P2 enabler |
| **f32 512K = 2048×256** (P2 = the existing 16×16 hier) | **1.496 → 1.049 ms (+30%), 0.55× → 0.69× MKL** |
| **f64 128K = 1024×128** (fused-P1) | 0.858 → **0.730 ms (+18%)**, 0.39 → 0.51× |
| **f64 256K = 1024×256** | 1.62 → **1.473 ms (+10%)**, 0.46 → 0.52× |
| **f64 64K = 1024×64** (fused-P1 + codelet64 P2) | 0.321 → 0.308 (+4%), 0.44× |
| f64 2048×64 / 4096×64 splits | measured ≈ flat (losing the 1024-only P1 gather-fusion cancels the P2 gain) — reverted to 1024-first splits |

Accuracy holds throughout (f64 ~1e-15, f32 ~1.3e-7 vs MKL).

**The measured f64 mid-band wall, precisely:** at 128K we now sit at the FOUR-memory-pass floor
(gather + P1 + twiddle/NT + P2 ≈ 16 MB of traffic ≈ 0.6 ms at stream rate; measured 0.73). MKL's
0.374 ms implies ~2 passes — their kernels fuse phases end-to-end. The remaining campaign items,
in value order: (1) gather-fused P2 for small n2 (the M13 fusion exists only for n2=1024),
(2) composed codelet128/256_batched (radix-2/4 combine over codelet64), (3) the f64 small-N
standalone codelets (1K–32K, still 0.29–0.47×) — the genuine generator work (the script must be
rebuilt; gen_fft_codelets.py is the surviving foundation).

## Session 3 (2026-07-03): the P2 pass — profiled, one lever refuted, the endgame named

**Profile (CRD_FFT_PROFILE, f64 four-step):** P2 sub-FFT = **43–45%** at 128K/256K; P2 gather 20%,
P2 scatter 10%, fused P1 ~27%. The assumed priority (gather/scatter fusion) was WRONG — the batched
128/256-point sub-FFT kernel itself dominates.

**Composed DIF 128/256 (radix-2/4 combine + codelet64_batched + interleave, block scratch): REFUTED**
— correct (suite green incl. the 2^17/2^18 oracle) but measured ~7% SLOWER than the batched radix-8
on both rows: the scratch round-trips double the tile footprint out of L2, erasing the leaf-kernel
gain. Reverted. This is the same lesson as the permute fused-outer and the June "8 attempts": at
these sizes, extra tile passes through a second buffer cost more than better kernels save.

**The endgame, now fully constrained by measurement:** the P2 crush requires GENERATED single-pass
in-place batched 128/256 codelets (straight-line, no scratch) — i.e., rebuilding the codelet
generator (`build/gen_subfft_m3.py` was scratch and is lost; `gen_fft_codelets.py` is the surviving
foundation and the f32 exemplars define the target form). That plus the f64 small-N standalone
codelets (1K–32K, 0.29–0.47×) are the two remaining items, both blocked on the same generator
rebuild — the single highest-value piece of work left in the FFT domain.

## Session 4 (2026-07-03): the generator REBUILT — and the straight-line ceiling measured

**`scripts/gen_fft_batched.py`** (tracked this time — the lost gen_subfft_m3.py lesson): split-radix
DAG + CSE + the register-pressure scheduler (typo-fixed from gen_fft_crush.py) + a NEW batched-SoA
Vec4d emitter in the exact hier_codelets.hpp house style — loads-first/stores-last (in-place-safe by
construction), numpy-validated in-script (8 random vectors, <1e-12) before emission. Output:
`batched_codelets_gen.hpp` (codelet128/256_batched f64, 4.8K lines), wired into the batched-leaf
gate; suite green (263, incl. the 2^17/2^18 four-step oracle), accuracy 1e-15.

**Measured:** codelet128 cuts the P2 sub-FFT phase ~14% at 128K (0.7→0.6 Mcyc; row-level ~1%,
inside noise); codelet256 is FLAT. **The straight-line ceiling is ~N=128** — matching the June
hand-war (N=64 already 0.83×): above it, peak-live growth turns the SSA tile into stack traffic
that eats the flop savings. The path forward for P2 at 256+ is the TWO-STAGE FUSED form
(f64 editions of codelet16_stage1_fused_16x16 etc.) — the next emitter increment on the now-rebuilt
generator (the DAG core + scheduler are shared; the stage-1 emitter adds fused twiddle + transposed
store).

**Campaign ledger for the day (all gated, accuracy held):** f32 512K **+30%**; f64 128K **+19%
cumulative** (0.858→0.718), 256K **+7%** (1.62→1.50), 64K +4%; four false leads permanently closed
(M16-B, M19, composed-DIF, 2048/4096-first splits); the generator capability RESTORED and tracked.

## Session 5 (2026-07-03): the STANDALONE-HIER breakthrough — the band transformed

**The insight:** a single n=n1·n2 transform in element-major memory IS a batch matrix — so the
standalone mid-band runs as: `codelet_n1_stage1_fused_sh` (generated: leaf + runtime twiddle table +
4×4-transposed store, ONE pass) → `codelet_n2_batched` (natural order out). TWO passes total — the
structural optimum for a 2-stage decomposition. The generator grew the `emit_stage1_fused_f64`
emitter (numpy-gated); splits: 1024=32×32 · 2048/4096=64× · 8192=64×128 · 16384=128×128 ·
32768=256×128 · **65536=256×256**. Suite green at every step; accuracy IMPROVED to 4-7e-16.

| n (f64) | morning GF/s | NOW | gain | vs MKL |
|---|---|---|---|---|
| 1024 | 22–27 | **33.7** | +30% | 0.61× |
| 2048 | 17.0 | **31.9** | **+88%** | **0.82×** |
| 4096 | 15.2 | **31.3** | **+106%** | 0.63× |
| 8192 | 11.7 | **27.3** | **+134%** | 0.53× |
| 16384 | 13.3 | **26.2** | +97% | 0.51× |
| 32768 | 14.3 | **23.7** | +66% | 0.47× |
| 65536 | 14.9 | **20.1** | +35% (four-step opt-in retired — 2-pass wins) | 0.57× |

Increments measured separately: 3-pass hier (+23..78%) → fused twiddle-transpose middle (+~15%) →
stage-1-fused 2-pass (+~20%); 1024 lost on 3-pass, WINS on 2-pass — each banked only after the gate.

**Remaining to MKL (0.47–0.82×), now precisely scoped:** both passes are compulsory; the residual is
LEAF QUALITY per pass (DAG scheduling/spills vs MKL's hand kernels) + their edge fusions. Next
mechanical extensions on the same vehicle: f32 editions (Vec8f emitter), deep splits for 128K+
(3-stage), leaf-scheduler tuning (Belady window), and the four-step P2 re-sweep with the fused
stage-1 kernels.

## Session 6 (2026-07-03): the f32 standalone-hier — Vec8f editions BANKED

**The generator grew the Vec8f emitters** (`emit_batched_f32` + `emit_stage1_fused_f32`, both
numpy-gated in-script, mirroring the f64 pair; lane width 8, `transpose8x8` added to crd-math
`vec8f.hpp` — the Vec4d `transpose4x4` mirror). Generated: f32 `codelet{128,256}_batched` (overloads;
32/64 already existed in hier_codelets.hpp) + f32 `codelet{32,64,128,256}_stage1_fused_sh`. Wired the
SAME standalone-hier gate for f32 1024–65536 (same splits as f64), batched-leaf gate extended to f32
128/256, `CRD_FFT_DISABLE_F32_SH` A/B escape hatch added. FFT suite green (277 asrt / 29 cases —
+1 new oracle test gating every f32 sh split vs the radix-2 reference + inverse round-trip).

**Matched-state A/B (one build script, same run, `taskset -c 4`, MKL 1T, best-of-200):**

| n (f32) | Stockham (no-sh) | sh 2-pass | call gain | vs MKL before → after |
|---|---|---|---|---|
| 1024 | 0.002 ms | 0.001 ms | ~2.0× | 0.18× → **0.51×** |
| 2048 | 0.004 | 0.002 | ~2.0× | 0.22× → **0.51×** |
| 4096 | 0.011 | 0.003 | **~3.2×** | 0.22× → **0.69×** |
| 8192 | 0.023 | 0.009 | 2.6× | 0.20× → **0.56×** |
| 16384 | 0.046 | 0.020 | 2.3× | 0.23× → **0.52×** |
| 32768 | 0.097 | 0.046 | 2.1× | 0.23× → **0.48×** |
| 65536 | 0.202 | 0.111 | 1.8× | 0.27× → **0.51×** |

Accuracy 2.0e-08–1.3e-07 (f32 class) on every row. Rows >64K identical paths both builds (drift ±5%).

**⚠ NEW measured cliff: f32 131072 = 0.17× MKL** (direct Stockham — f32 has NO four-step opt-in at
2^17 and the sh band tops at 65536). The 128K–1M deep-split pass attacks this next.

### Session 6b: f32 128K four-step opt-in, then the DEEP-SPLIT (n = A·B·C) — 128K–512K transformed

**f32 128K four-step opt-in banked first** (1024×128: gather-fused P1 hier + the new f32
codelet128_batched P2): 0.821 → 0.305 ms, 0.17× → 0.48× MKL.

**Then the DEEP-SPLIT**: n = A·B·C, THREE generated passes, all natural-order by construction —
S1 = `codeletA_stage1_fused_sh` (leaf + full W_n twiddle + transposed store) · S2 = NEW
`codeletB_fused_notr` emitter (leaf + BROADCAST W_BC twiddle from a compact B·C table — 32 KB, not an
n-sized stream + natural store) · S3 = NEW `codeletC_batched_strided` emitter (in-stride A, out-stride
A·B, per-kB L1/L2-resident blocks, writes land at d[kA + A·kB + AB·kC] = natural order, no scatter
pass). Index algebra verified end-to-end; suite-gated vs the radix-2 oracle at f64 1e-12 (both dirs).

**Matched-run A/B boards (canonical bench_fft_vs_refs / bench_fft_f32, `taskset -c 4`):**

| row | four-step | deep-split | verdict |
|---|---|---|---|
| f64 128K (32·64·64) | 0.735 ms / 0.51× | **0.520 ms / 0.65×** | **+41% BANKED** |
| f64 256K (64·64·64) | 1.647 ms / 0.47× | **1.027 ms / 0.75×** | **+60% BANKED** |
| f64 512K (64·64·128) | 3.337 ms / 0.52× | **3.012 ms / 0.55×** | **+11% BANKED** |
| f64 1M (64·128·128) | 6.489 ms / 0.68× | 10.545 ms | **−38% REVERTED** |
| f32 128K (32·64·64) | 0.286 ms / 0.51× | **0.209 ms / 0.69×** | **+37% BANKED** |
| f32 256K (64·64·64) | 0.497 ms / 0.76× | **0.467 ms / 0.77×** | **+6% BANKED** |
| f32 512K | 1.232 ms / 0.70× | 1.295 ms | **−5% REVERTED** (the fused 16×16-hier P2 four-step wins) |

f64 accuracy IMPROVED on the ds rows (7.5–8.1e-16, was 1.0–1.2e-15).

**The two reverts, mechanism pinned:** (1M f64) the deep-split streams 3 full n-round-trips + the
n-sized W_n S1 table (16 MB) through DRAM; the four-step keeps its sub-work L2-blocked and its
twiddle FACTORED (~√n tables) — at DRAM-resident sizes fewer full passes beats better kernels. Next
lever there = a FACTORED-twiddle S1 emitter variant (W_n^{k·u} = hi[k,u_hi]·lo[k,u_lo], ~N1·√BC-entry
tables) IF the mid-band ceiling ever matters at 1M; the pass-count argument says the four-step's
2-block structure is still the right frame at ≥1M. (f32 512K) the banked 2048×256 four-step's P2 is
ALREADY gather+scatter-fused (16×16 hier) — the ds 3-pass adds a full t2 round-trip it doesn't pay.

**Deep-split pass profile (TSC Mcyc/call, 30-call mean):** 128K 0.56/0.55/0.42 (balanced) · 256K
1.52/1.22/0.93 · 512K 4.59/4.56/4.49 — at 512K all three passes sit on the same memory-bound
plateau (~40% of stream bw; 64–128 concurrent strided streams per pass). 512K split note: B=128
measured S2=6.96 Mcyc (128+128 read/write streams); (64,64,128) rebalances to ~4.5 each.

### Session 6c: the leaf-scheduler pass — BELADY tiebreak, per-kernel verdict, "hybrid2" banked

Added a Belady-window tiebreak to the generator's freed-count scheduler (among equal freed-counts,
prefer the value consumed SOONEST = min remaining-indegree over consumers — shortest live range).
Full-belady A/B verdict was SPLIT along lane width:

- **f32 (Vec8f): wins broadly** — 4096 0.67→0.74–0.79× · 8192 0.56→0.60–0.63× · 65536 0.51→0.54–0.55×
  · 128K-ds 0.209→0.181 ms; zero f32 regressions.
- **f64 (Vec4d): wins on the 128-leaf rows** (8K +3–5% · 16K +5% · 64K +2.7%) and **COLLAPSES on the
  deep-split kernels** (256K −16%, 512K −68% — fused_notr/strided spill catastrophically at 4 lanes)
  and mildly on 256_batched (65536 −3–6% full-belady).

**Banked = "hybrid2" (the tracked generator default):** f32 all-belady; f64 belady ONLY for
codelet128_batched + codelet128/256_stage1_fused_sh; f64 greedy elsewhere. Verified: the affected f64
rows +1.4–5%, ds rows flat, f32 gains held (f32 256K-ds 0.453 ms / 0.77×, no regression). Suite green
throughout (281 asrt).

### Session 6d: the win-debug STACK-OVERFLOW scar → the dual-body (SIMD/lane-scalar) emission

The first Windows run of the new paths SEGFAULted two suites (0xC00000FD). Mechanism MEASURED, not
guessed: at `/Od` MSVC gives EVERY expression temporary its own un-reused stack slot ⇒ the
straight-line 256-point kernel frames are **1.17–1.4 MB each** (dumpbin `sub rsp` probe; ~236 B per
SSA value; 128-pt = 511 KB) — one call alone overflows the 1 MB Windows default stack. `#pragma
optimize("gt", on)` CANNOT re-enable optimization in a /Od compiland (tested: frames stayed ~1 MB) and
win-clang-cl debug exists ⇒ pragma paths dead. **Root fix, engine-side, all consumers:** the generator
now emits every kernel with TWO bodies — the SIMD tiles under `CRD_FFT_GEN_SIMD_BODY`
(`NDEBUG || __OPTIMIZE__`; byte-identical to the benched code) and a LANE-SCALAR edition otherwise:
the same DAG in the same order per column ⇒ **bit-identical results** (Vec ops are lane-wise IEEE ops;
`0−x` kept over unary minus for signed-zero identity), frames **23–57 KB** (measured, 20–25× smaller).
⚠ second scar inside the fix: ad-hoc `g++ -O3` bench builds carry NO `-DNDEBUG` — the first gate
(`NDEBUG` only) silently benched the scalar bodies (f32 8192 0.60×→0.11×); `__OPTIMIZE__` closes it.
**Final gates: linux-gcc-release 281 asrt green · win-debug ctest 27/27 green · SIMD boards re-verified
post-regen.**

## FINAL BOARDS (2026-07-03, end of session 6; machine drifts ±10% run-to-run — increment claims
## above are matched-run A/Bs, these are one-run closing states)

### f64 (vs MKL, canonical bench_fft_vs_refs, 1T pinned)

| n | GF/s | vs MKL | path |
|---|---|---|---|
| 1024 | 31.9 | 0.54× | sh 2-pass (32×32) |
| 2048 | 31.1 | 0.82× | sh 2-pass (64×32) |
| 4096 | 31.4 | 0.62× | sh 2-pass (64×64) |
| 8192 | 27.7 | 0.55× | sh 2-pass (64×128) |
| 16384 | 27.2 | 0.55× | sh 2-pass (128×128) |
| 32768 | 24.7 | 0.49× | sh 2-pass (256×128) |
| 65536 | 21.5–22.3 | 0.57× | sh 2-pass (256×256) |
| 131072 | 21.4–24.2 | 0.65–0.69× | **deep-split 32·64·64** |
| 262144 | 20.1–24.6 | 0.60–0.75× | **deep-split 64·64·64** |
| 524288 | 13.9–16.5 | 0.44–0.55× | **deep-split 64·64·128** |
| 1048576 | 16.2–17.7 | 0.65–0.70× | four-step (ds −38%, reverted) |
| 2M / 4M / 8M | 16.4–17.9 / 16.3–16.7 / 14.3–15.8 | 0.87–0.93× / 0.93–1.13× / 0.86–0.92× | four-step |

### f32 (vs MKL DFTI, 1T pinned)

| n | vs MKL | path |
|---|---|---|
| 1024–2048 | 0.50–0.52× | sh 2-pass |
| 4096 | 0.70–0.74× | sh 2-pass |
| 8192–65536 | 0.51–0.60× | sh 2-pass |
| 131072 | 0.64–0.70× | **deep-split 32·64·64** (was 0.17×) |
| 262144 | 0.65–0.77× | **deep-split 64·64·64** |
| 524288 | 0.66–0.75× | four-step 2048×256 (ds −5%, reverted) |
| 1M / 2M / 4M | 0.58–0.77× / 0.73–0.96× / 0.84–1.09× | four-step |

**Day ledger (matched-run gains, all suite-gated):** f32 1024–65536 **+80%..+220%** (Vec8f sh band,
was 0.18–0.27×) · f32 128K **2.7×→(+37% more via ds) ≈ 3.9× total** (0.17×→~0.70×) · f64 128K **+41%**
· f64 256K **+60%** · f64 512K **+11%** · f32 256K +6% · scheduler hybrid2 (f32 +6–13% band-wide) ·
the win-debug stack-overflow ROOT-FIXED (dual-body emission, bit-identical). **Losses pinned with
mechanism:** f64/f32 ≥1M deep-split (3 full DRAM round-trips + n-sized table vs the four-step's
L2-blocked 2-block structure — next lever = factored-twiddle S1 emitter, or accept the four-step
frame there); the remaining 0.5–0.8× band = per-pass leaf quality vs MKL's hand kernels (the
scheduler pass moved it; the next levers are FMA emission for the twiddle muls + a true
register-window scheduler).

## Session 7 (2026-07-04): THE CRUSH DAY — research-led, five levers banked, band moved to 0.6–0.98×

**Research first (the full-crush directive):** the existence proof found = **VectorFFT**
(github.com/Tugbars/VectorFFT — split-layout mixed-radix, an OCaml DAG compiler, beats MKL 1D C2C on
an i9-14900KF, OUR CPU class), plus FFTS (runtime specialization, IEEE TSP 2013) and the SPIRAL
short-vector papers. VectorFFT's inventory named our two missing weapons: split layout (interleave
paid ONCE at the boundary) and FMA codelets. Its autotuning "wisdom" is the one piece we
deterministically replace with measured-fixed plans (house doctrine).

**Lever 1 — FMA emission (banked, +4–9% f64 leaf band):** every generated cmul/twiddle became
2 mul + fma + fnmadd (was 4 mul + add + sub); scalar debug bodies mirror with std::fma —
single-rounded IEEE, bit-identical to the vector fallbacks, deterministic like std::sqrt. WASM note:
the SIMD128 fallback runs scalar fma (per-build determinism, same contract class as today).

**Lever 2 — split layout, the measurement arc (SoA → AoSoA):** pure SoA planes won ONLY L1-resident
(f64 1024 +13%) and LOST the stream-bound rows (f64 256K −16%, f32 128K −18%): planes DOUBLE the
stream count — outside L1, streams trump shuffles. The fix used by the fast libraries: **AoSoA
block-interleaved rows ([L×re | L×im] vector blocks)** — zero shuffles AND one stream per row.
Banked: f32 band +6–10%, f64 small +3–7%, ds rows recovered. The pipelines now pay interleave
conversion exactly twice per transform (the structural minimum).

**Lever 3 — FACTORED stage-1 twiddle (banked for n2 ≥ 1024):** the full n-entry S1 table streams as
many bytes as the data; W_n^{k·u} = hi[k,u>>msh]·lo[k,u&(M−1)] (EXACT index split, 2 extra FMAs)
shrinks it to L1/L2-resident tables. Wins where the table left L2 (f64 128K +4% / 256K +3% / 512K
+13% / f32 128K +14%, 256K → **0.88×**); LOSES 3–6% where it was cache-comfortable (n2 ≤ 256) — gate
kept at the ds sizes. **f32 512K re-measured under the new pipeline: the 2026-07-03 −5% exclusion
FLIPPED to +21% (0.953 ms, 0.84×) — ds re-banked there.** 1M re-tried WITH factored twiddle: STILL
loses (f64 9.7 vs 6.3 ms) ⇒ the 3× full-size DRAM round-trips are the ≥1M wall, mechanism now doubly
confirmed; the four-step stays.

**Lever 4 — hier_codelets FMA rewrite (`scripts/fma_rewrite_hier.py`):** shape-strict rewrite of the
hand-tracked file (1,118 const-cmuls + 1,774 lines total); suite-gated. Measured FLAT on its 1M–8M
targets (DRAM-traffic-bound, as the pass-count math predicts) — kept: no losses, whole file now on
the FMA convention.

**Lever 5 — the ds cascade DOWN (8K–64K, banked):** the worst f64 rows (32K/64K, 0.48–0.59×) rode
the spill-heavy 256-leaves in the sh 2-pass. New 16/32-point S2/S3 AoSoA emitters + splits
8K=32·16·16 · 16K=32·32·16 · 32K=32·32·32 · 64K=32·32·64: **f64 32K +21% (31.3 GF/s) · 64K +23%
(0.76×) · 16K +6% · 8K +4%; f32 32K +15% · 64K +23%**. f32 8192 ds measured −10% (16-pt leaves = 2
Vec8f rows, overhead wins) — reverted, sh keeps that row. ⚠ scar en route: the ds S1 dispatch called
the factored kernel unconditionally while 8K/16K built full tables ⇒ null-table SEGFAULT in-suite —
fixed with the same full/factored branch the sh gate has (the suite caught it immediately).
⚠ second scar: `-` + a negative emitted constant lexes as `--` (C2105) — only the MSVC-debug scalar
bodies compile that path; parenthesized.

## FINAL BOARDS 2026-07-04 (one matched run; drift ±10%)

### f64 vs MKL (also now BEATS FFTW on every row ≥ 512K)

| n | GF/s | vs MKL | vs FFTW | path |
|---|---|---|---|---|
| 1024 | 37.2 | 0.72× | 0.76× | sh 2-pass AoSoA+FMA |
| 2048 | 32.6 | 0.80× | 0.67× | sh |
| 4096 | 30.8 | 0.61× | 0.78× | sh |
| 8192 | 31.8 | 0.60× | 0.70× | **ds 32·16·16** |
| 16384 | 29.8 | 0.60× | 0.79× | **ds 32·32·16** |
| 32768 | 29.9 | 0.61× | 0.85× | **ds 32·32·32** |
| 65536 | 26.9 | 0.68× | 0.79× | **ds 32·32·64** |
| 131072 | 25.8 | 0.77× | 0.85× | ds 32·64·64 + factored |
| 262144 | 24.3 | 0.78× | 0.86× | ds 64·64·64 + factored |
| 524288 | 18.3 | 0.63× | **1.01×** | ds 64·64·128 + factored |
| 1048576 | 15.8 | 0.66× | **1.01×** | four-step |
| 2M / 4M / 8M | 15.7 / 16.3 / 15.4 | 0.83× / **0.98×** / 0.95× | 1.05× / **1.30×** / **1.22×** | four-step |

### f32 vs MKL DFTI

1024 0.57× · 2048 0.54× · 4096 0.76× · 8192 0.65× · 16K 0.63× · 32K 0.61× · 64K 0.69× · 128K 0.75×
· **256K 0.89×** · 512K 0.78× · 1M 0.67× · 2M 0.88× · **4M 0.93×**. Accuracy f64 3.7e-16–1.2e-15,
f32 ≤1.3e-7 on every row.

**Two-day cumulative:** the f64 mid-band went ~0.47–0.62× → **0.60–0.80×**, the f32 band 0.17–0.55×
→ **0.54–0.93×**, large-n f64 to 0.95–0.98× (and past FFTW everywhere ≥512K). **Remaining gaps,
mechanisms pinned:** (a) f64 4096–32K (0.60×) — MKL's ~50 GF/s implies near-peak FMA utilization in
a fused-register pipeline; ours pays 2–3 materialized passes + the S1 transpose shuffles; next lever
= in-register stage fusion (vector-radix butterflies between stages, the SPIRAL structure) or a
Belady-window true register scheduler. (b) f64/f32 1M (0.66×) — the four-step's gather/scatter
strided phases; next lever = the M16-class fused-phase rewrite with AoSoA bridges. (c) f32
1024–2048 (0.54×) — per-call overhead at 8-lane tiles; candidate = fully-fused single-pass small-n
kernels. Determinism held everywhere (fixed plans, fixed op order, single-rounded FMA); WASM story
unchanged (SIMD128 scalar-fma fallback, per-build determinism).

## Session 8 (2026-07-04): the SPILL measurement + STORE-EARLY scheduling

**Measured the compute-band wall directly (dumpbin/objdump static probe on the -O3 binary,
`build/crd_fft_spillprobe.sh`):** `execute()` with all hot kernels inlined carried **44,411 stack
spill/fill ops vs ~20K real data ops and ~51K arithmetic ops** — spill traffic > 2× the real memory
traffic; arithmetic density 25%. Mechanism: the emitters placed ALL output stores at each kernel's
end (an in-place-safety invariant), holding all N outputs live simultaneously — ~2N vector registers
against 16 YMM, guaranteed spills.

**The fix — STORE-EARLY scheduling:** the AoSoA pipeline kernels have DISJOINT in/out buffers and
never needed stores-last. The generator now emits each output store the moment its value completes
(single-output granularity; transpose-GROUP granularity in the fused stage-1 kernels), freeing the
register mid-DAG. Values are bit-identical (store position doesn't change arithmetic); the in-place
interleaved `codelet{128,256}_batched` (execute_batched leaf gate) keep stores-last.

**Banked (matched run): f64 2048 +8% · 4096 +12% (34.4 GF/s) · 16K +9% (32.5) · 32K +5% · 64K +7%
(28.7) · 512K +9% (19.9); f32 4096 → 0.82× · 64K → 0.74× · 128K → 0.81×.** Static spills only −10%
(the win is placement/live-range, and the static count is dominated by the big 128/256 kernels) —
the residual ~40K static spills say a true register-pressure scheduler still has headroom.

## BOARDS AFTER SESSION 8 (one matched run; drift ±10%)

f64 vs MKL: 1024 0.59× · 2048 **0.80×** · 4096 0.69× · 8192 0.59× · 16K 0.61× · 32K 0.58× · 64K
**0.74×** · 128K **0.84×** · 256K 0.76× · 512K 0.66× · 1M 0.66× · 2M **0.88×** · 4M **0.99×** · 8M
0.87×. (Beats FFTW on 512K–8M and 32K/64K this run.)

f32 vs MKL: 1024 0.58× · 2048 0.57× · 4096 **0.82×** · 8192 0.70× · 16K 0.66× · 32K 0.64× · 64K
**0.74×** · 128K **0.81×** · 256K **0.90×** · 512K 0.75× · 1M 0.68× · 2M **0.90×** · 4M **0.97×**.

**Next levers, in value order (the full-crush queue):** (1) a true register-window scheduler
(live-set-bounded list scheduling — the residual 40K static spills); (2) the 1M+ fused-phase
four-step v2 with AoSoA bridges (Van Loan six-step framework — book requested); (3) f32/f64 small-n
(1024–2048) fully-fused single-pass kernels; (4) split retunes (f64 8192/32K wobble between splits).

**4096 = 16·16·16 all-tiny-leaf: tried + REFUTED (2026-07-04):** f64 30.5 vs 34.4 GF/s, f32 0.72×
vs 0.82× — at L1-resident sizes the third pass costs more than spill-free leaves save; the 2-pass sh
keeps everything below 8K. (The 16-point stage-1 kernels stay emitted for future splits.)

**THE ARCHITECTURE VERDICT for the remaining mid-band gap (why not 1.0× yet):** we have now CAUGHT
the straight-line-codelet architecture ceiling — FFTW (the best of that class) runs 33–38 GF/s on
this machine and we sit at 30–34, beating it on several rows. MKL's 50–54 GF/s comes from a
DIFFERENT architecture: many tiny radix-4/8 passes streaming L1/L2 with 8–16 live registers each
(zero spills, near-perfect FMA density). The pass-count math matches its measured times exactly
(7 radix-4 passes over 16K's 512 KB at L2 bandwidth ≈ 19 µs ≈ MKL's 22 µs). **The endgame is
"Stockham v2": generated AoSoA FMA radix-4/8 pass pipelines** (first pass fuses the deinterleave,
last fuses the reinterleave, compact per-pass twiddles, ping-pong buffers) — replacing the June-era
non-FMA interleaved Stockham that measured 0.29–0.47×, and slotting under the existing dispatch as
the mid-band engine, with the four-step's P1/P2 running the same engine cache-blocked at ≥1M. A
dedicated session; Van Loan's frameworks book feeds the design.

## 2026-07-04 — ip4-AoS PROMOTED: the f64 mid-band board (default build, linux-gcc-release, 1T, i9-14900K/WSL2)

Engine: interleaved in-place radix-4 (COBRA gather + 3-layer fold, hybrid tables, k-unroll×2
block-pairs), f64 1K–64K both parities, forward+inverse. Design record: docs/research/fft-stockham-v2.md
rounds 1–18 (VTune-guided; DTLB 24.5%→2.0% of clockticks was the decisive fix).

| n | Cerid GF/s | FFTW | MKL | vs MKL | vs FFTW | maxrel |
|---|---|---|---|---|---|---|
| 1024 | 40.67 | 50.79 | 61.91 | 0.66× | 0.80× | 4.5e-16 |
| 2048 | 37.82 | 50.20 | 40.37 | 0.94× | 0.75× | 4.1e-16 |
| 4096 | 38.26 | 46.91 | 53.39 | 0.72× | 0.82× | 4.8e-16 |
| 8192 | 38.35 | 46.98 | 52.81 | 0.73× | 0.82× | 5.2e-16 |
| 16384 | 39.31 | 39.55 | 51.92 | 0.76× | 0.99× | 6.5e-16 |
| 32768 | 38.33 | 36.55 | 53.96 | 0.71× | **1.05×** | 7.2e-16 |
| 65536 | 34.16 | 35.07 | 36.64 | 0.93× | 0.97× | 7.6e-16 |

Native-to-native (Windows, VTune-verified same-silicon): 16K 40.6 vs MKL 47.9 = 0.85×. Prior banked
paths (sh/ds) on these rows: 0.56–0.74× — superseded. Radix-8 and huge-page variants built, measured
slower, retained-but-disabled with mechanisms recorded. f32 mid-band still on the sh path (Vec8f
edition = follow-up).

## 2026-07-04 (later) — f32 twin-fold edition + per-size dispatch (default build, 1T)

ip4-AoS f32 (Vec8f twin-unit fold) promoted for {2048, 16K, 32K, 64K}; sh keeps {1024, 4096, 8192}
(matched-state A/B, never-regress rule). All rows on measured winners, maxrel ≤ 1.4e-07:

| n | ratio vs MKL | engine |
|---|---|---|
| 1024 | 0.59× | sh |
| 2048 | 0.58× | ip4-AoS (was 0.52) |
| 4096 | 0.82× | sh |
| 8192 | 0.71× | sh |
| 16384 | 0.74× | ip4-AoS |
| 32768 | 0.74× | ip4-AoS (was 0.69) |
| 65536 | 0.81× | ip4-AoS (was 0.70) |

f64 inverse: conjugation via sgn-flip + negate-at-load; suite round-trips green both types.
