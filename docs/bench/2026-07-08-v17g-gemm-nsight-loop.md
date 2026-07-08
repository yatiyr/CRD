# 2026-07-08 — v17-g GEMM optimization: the Nsight profile→diagnose→fix→measure loop

**Board:** CUDA FP32 GEMM (N=2048, square), RTX 4070 Ti SUPER, clock-locked 2610 MHz, GPU-event timed (kernel-only, no
H2D/D2H), 20-iter average. Bench harness: `external/gemm_lab.cu`. Profiler: Nsight Compute 2026.2.1 (`ncu`), counter
access unlocked. This is the reference walkthrough for §H.3 of `docs/hints/crush-playbook.md`.

## The methodology correction that started it (⚠ read this first)
An earlier CKIR pass concluded "the block-tiled GLSL GEMM is 4× SLOWER than naive" and **that was a MEASUREMENT
ARTIFACT, not a kernel property.** It was measured through the test harness = wall-clock **including a 2MB H2D upload +
1MB readback per call**, on a **512³ (L2-resident) matrix** where the naive is cache-fed. Re-measured properly — GPU
events, no upload in the timed region, N=2048 (large enough to matter) — the tiled kernel is **6.6× FASTER**. **Rule:
time GPU kernels with `cudaEvent` (kernel-only), at a size that exceeds L2, never wall-clock-with-transfers on a small
matrix.** (The naive `precise` kernel is still the right CKIR *default* for correctness + L2-resident sizes; it's just
not the perf ceiling.)

## The measured progression (each step driven by the previous step's counters)

| # | kernel | GFLOP/s | vs naive | SM% | Mem(busy)% | Occup. | limiter (from `ncu`) |
|--|--------|--------:|---------:|----:|-----------:|-------:|----------------------|
| 0 | naive (1 thread/output) | 2,547 | 1.0× | ~9 | ~13 | 16% | latency-bound; Block-Limit-Registers=2 |
| 1 | tiled 4×4 (64×64×8, shared+regs) | 16,191 | 6.4× | 58 | 76 | 61% | **shared-mem bandwidth** (L2 hit 96%, DRAM 6.5%) |
| 2 | tiled 8×8 (128×128×8) | 17,383 | 6.8× | 48 | 63 | 30% | register-limited (2 blk/SM) — intensity↑ but occupancy↓ = wash |
| 3 | **vec float4 + transposed-A** | **21,728** | **8.5×** | 58 | 78 | 32% | shared-mem still; more work/instruction via 128-bit loads |
| 4 | db register-prefetch (double-buffer) | 20,924 | 8.2× | — | — | — | **NO help — SLIGHTLY SLOWER** (prefetch regs cost occupancy; see below) |
| 5 | **warptiled** (block→warp→thread, WNITER=4) | 24,090 | 9.5× | 59 | 48 | 16% | **short_scoreboard 0.90→0.11 CRUSHED** (intensity↑); now long_scoreboard 0.53 (global) |
| 6 | **warptiled NT=256** (WM64×WN32, 64 acc) | **25,416** | **10.0×** | 64 | 58 | **30%** | occupancy 16→30% (more warps); stalls long_scoreboard 0.46 + barrier 0.38 |

**Warptiling confirmed the hypothesis** — a 3-level tile (regM/regN loaded once per kk, reused across WMITER×WNITER
sub-tiles) raised arithmetic intensity ⇒ `short_scoreboard` (shared-read stall) **0.90 → 0.11**, exactly as predicted;
memory throughput 78→48%. The NT=256 config sweep (smaller per-thread tile, 64 vs 128 acc regs) doubled occupancy
16→30% for another +10%. **10.0× over naive, 58% of the ~44 TF FP32 peak, bit-exact throughout.** ptxas registers:
vec 127, warptiled-128 = 230, all at 2 blocks/SM (register-limited — that is NOT the blocker; cuBLAS also runs ~2 blk/SM).

### Stall breakdown (vec kernel, `smsp__..._stalled_*_per_issue_active`, cycles/issue of ~6.4)
`long_scoreboard 0.95` (global-mem latency, #1) · `short_scoreboard 0.90` (shared-mem latency, #2) · `barrier 0.62` ·
`mio_throttle 0.39` · `wait 0.26`. Scheduler: **40% "No Eligible"**, only **1.6 eligible warps/scheduler** ⇒
occupancy-starved (can't hide the stalls).

### Step 4 finding — double-buffering did NOT help (register-bound), a measured lesson
`long_scoreboard` (0.95) is the top stall, so register-prefetch double-buffering *should* hide it — but it measured
**~4% SLOWER** (20.9 vs 21.8 TF). Why: the kernel is already **register-limited (2 blocks/SM, 32% occupancy)**; the
prefetch's `la`/`lb` registers cut occupancy further, and the lost latency-hiding warps outweigh the prefetch benefit.
**Lesson: double-buffering costs registers — it only pays off with occupancy HEADROOM, which an 8×8-microtile kernel
doesn't have. The real last-mile lever here is NOT prefetch — it's raising effective occupancy/ILP** (warptiling: a
warp-level sub-tile that keeps intensity high while cutting per-thread registers + shared traffic; or a smaller microtile
+ deeper BK). That's CUTLASS territory — several more measured iterations, not a tweak.

All bit-exact vs the naive kernel (max|Δ| = 0 — same sequential-k FMA order; these are the **FMA fast tier**, NOT the
`precise` no-FMA bit-exact-vs-CPU-oracle tier).

## What each `ncu` reading told us + the fix it prescribed
1. **naive** — SpeedOfLight: SM 9% + Mem 13% both low ⇒ *latency-bound* (not compute/mem). Occupancy 16%, and the whole
   kernel re-reads A/B from global per output. Fix: **tile + reuse via shared memory + register accumulation.**
2. **tiled 4×4** — jumped to 16 TF. SpeedOfLight: Mem-busy **76%** (shared pipe), DRAM only 6.5%, **L2 hit 96%** ⇒
   *shared-memory-bandwidth-bound*, tiles cached in L2. Occupancy 61% (healthy). Fix: **raise arithmetic intensity**
   (more FMA per shared load) → try 8×8.
3. **tiled 8×8** — barely moved (17 TF). Occupancy **dropped to 30%** — Block-Limit-Registers=2: the 64-element acc tile
   costs ~80 regs ⇒ only 2 blocks/SM. Intensity↑ but latency-hiding↓ ⇒ net wash. Lesson: **microtile size is an
   intensity-vs-occupancy tradeoff, not a free win.** Fix: attack the shared bandwidth *directly* → vectorize.
4. **vec float4 + transposed-A** — 21.7 TF (≈49% of ~44 TF peak). 128-bit global loads (4× fewer load instructions) +
   store A *transposed* in shared so the register reads are contiguous conflict-free `float4`. Still Mem-busy 78% /
   occupancy 32% (register-limited).

## Step 7 — cp.async double-buffering: the technique WORKS, but exposes the swizzle wall (decisive finding)
Built `gemm_cpasync` (ping-pong shared + `cp.async.cg.shared.global` async prefetch, BK=8, wt_256 config). Bit-exact.
**cp.async DID its job — it hid the global load: `long_scoreboard 0.46 → 0.06`.** But throughput COLLAPSED to 8.7 TF
(from 25.4). `ncu` root cause: **`short_scoreboard 0.19 → 4.26`, 235 MILLION shared bank conflicts.** cp.async is a
straight byte-copy ⇒ it can't transpose ⇒ forces `As[m][k]`, and the strided `regM` read puts all 8 warp-rows on the
same bank. **The trap: cp.async needs 16-byte-ALIGNED shared writes (row-pad a multiple of 4), but conflict-free strided
reads need a pad that is NOT a multiple of 4 — contradictory under linear addressing** (a `+4` pad made it *worse*).
**The only fix is CUTLASS XOR-SWIZZLING of the shared layout** (permute the bank mapping so both the aligned cp.async
write AND the strided read are conflict-free) — the genuine final technique, intricate and error-prone. **Conclusion:
`wt_256` (25.4 TF, 10×, 58% peak, TRANSPOSED-A, no cp.async) remains the best kernel; cp.async only pays off WITH the
swizzle.** We now know the exact wall: the parity kernel = cp.async + XOR-swizzled shared + the warptile.

## Step 8 — XOR-swizzle: BOTH techniques proven, but the transpose tax caps the async path (the answer)
Fixed the cp.async bank conflicts with a **float4-chunk XOR-swizzle**: store/read chunk column `c ^ ((m>>3) & (BK/4-1))`
(permute at 16-byte granularity ⇒ cp.async stays aligned AND the strided read scatters across banks). BK=16 (4 chunks,
2-bit swizzle). Result: cp.async **8.7 → 16.7 TF**, bit-exact. `ncu` confirms BOTH stalls crushed: **long_scoreboard
0.46→0.02** (cp.async hid global), **short_scoreboard 4.26→0.24, bank conflicts 235M→33M** (swizzle worked). BUT
**SM-throughput rose to 75% while FLOP FELL** — the swizzle's per-read address math (XOR/shift/div/mod) burns the integer
pipe. **Decisive finding: cp.async can't transpose ⇒ the A-read is column-wise/strided ⇒ you pay EITHER bank conflicts
(8.7 TF) OR swizzle-address overhead (16.7 TF); the TRANSPOSED `wt_256` avoids both by layout (contiguous read, zero
swizzle tax) and stays the best at 25.4 TF.** (⚠ An earlier draft called 58%-of-peak "the practical hand-written ceiling"
and blamed a "SASS/tensor wall." **That was WRONG and is retracted** — see Step 9: the target was never 44 TF, and we're
at 90% of the real one. Recorded per `feedback_never_invoke_a_wall_a_peer_already_beat`.)

## Step 9 — ORDER 1: PIN THE TARGET (the correction that reframes everything)
Measured cuBLAS SGEMM on THIS box (clock-locked 2610, 2048³, cudaEvent, 50-iter): **cuBLAS = ~28,500 GFLOP/s stable**
(26.6–28.8 across runs). **The 40,000 GFLOP/s I'd been chasing was a PHANTOM I never measured** — FP32 SGEMM is
bandwidth-bound, so cuBLAS itself only reaches ~65% of the 44 TF FMA peak. **Our best `wt` (autotuned) = ~25,700 GFLOP/s
= ~90% of cuBLAS.** We were parity-class the whole time; I was flogging us against a number that doesn't exist on this
card. This is why Order 1 (pin the target FIRST) is the mandate's first order.

## Step 10 — ORDER 5: REVERSE-ENGINEER cuBLAS (`ncu` on the vendor kernel)
cuBLAS dispatches **`cutlass_80_simt_sgemm_256x128_8x4_nn_align1`** — it IS a CUTLASS SIMT kernel. Its config, read off
the profiler: **256×128 block, 8×4 thread, 256 threads, 202 registers/thread, 49 KB opt-in dynamic shared, 16.6%
occupancy (1 block/SM), 78% SM throughput.** The lesson is the opposite of my occupancy-chasing: **cuBLAS EMBRACES low
occupancy + very high registers and hides all latency in a deep multi-stage `cp.async` pipeline over a swizzled 49 KB
shared tile.** Register/occupancy is not the lever — pipeline DEPTH is.

## Step 11 — ORDER 6: AUTOTUNE (mode 11, ~20 configs swept)
Best survivor within the single/2-sync warptiled structure: **`wt<128,128,16,64,32,1,8,8,256>` ≈ 25,700 GFLOP/s** (90%
of cuBLAS). 256×128 configs were SLOWER (21–23 TF) — the big tile only pays WITH cuBLAS's deep pipeline, which a
single-buffer kernel lacks. (A `256,256,...,NT=256` config printed a bogus 45 TF then illegal-memory-accessed: it was an
INVALID config — 16 warps needed, 8 launched — caught by the correctness check. Removed.)

## Step 12 — register-prefetch double-buffer on the warptile (mode 12) — measured, doesn't beat single-buffer
`gemm_wt_db` (transposed, prefetch next K-tile's global into registers before compute): **20–23 TF, SLOWER than 25.7**.
Same lesson as Step 4 at the warptile scale — the prefetch registers cut occupancy; the 2-`syncthreads` structure isn't
cuBLAS's pipeline. Confirms: a shallow double-buffer is not the win; **only a DEEP (3–4 stage) cp.async pipeline is.**

## Where it stands — HONEST, measured, no walls
**~25,700 GFLOP/s = ~90% of cuBLAS (~28,500), bit-exact.** Public hand-written best (siboehm k10, autotuned) is ~93–96%
of cuBLAS, so we're in the real hand-written ballpark. Everything cheap is exhausted (autotune caps at 90%; register-
prefetch + shallow cp.async both measured slower). **The remaining ~10% is ONE specified, reverse-engineered technique:
cuBLAS's deep multi-stage `cp.async` pipeline** — 3–4 stage ring of 49 KB opt-in shared + XOR-swizzled layout + the
low-occupancy/high-register regime, so the swizzle-address tax is fully hidden behind the async loads. This is NOT a wall
(cuBLAS proves 28.5 TF is reachable and it's an open-source CUTLASS kernel); it is the next BUILD — the full CUTLASS SIMT
mainloop (`gemm_pipe` in the lab), same measure→profile→fix discipline, until we match or beat cuBLAS.

## Step 13-14 — the CUTLASS-style mainloop BUILT (cp.async pipeline + fragment-swizzled A), measured
Built `gemm_pipe` (2-stage cp.async) + `gemm_pipeN` (N-stage), non-transposed **XOR-swizzled A read as float4 FRAGMENTS**
(4 K's per load ⇒ swizzle computed once per chunk = 4× fewer int ops than the scalar swizzle). All bit-exact. The
profiler shows every technique WORKING:
- **cp.async killed global latency**: `long_scoreboard 0.46 → 0.02`.
- **fragment-swizzle killed bank conflicts**: 235M → **9,010** (`short_scoreboard 0.25`), and 4× less swizzle int-math than Step 8.
- Removing the redundant sync (true ping-pong needs 1/iter) took the 2-stage from 23.8 → **25.0 TF**; `barrier 0.60→0.44`.

**Result: 2-stage pipe = 25.0 TF ≈ the single-buffer wt (25.7). 3/4-stage = SLOWER (22–23.5)** — deeper stages force 1
block/SM (BK=16 ×3 = 48 KB) and our kernel lacks cuBLAS's 202-register ILP to hide the occupancy drop. **So the mainloop
is proven and correct, but ties (doesn't beat) the single-buffer at our register budget.**

## Honest standing (Order 3 NOT yet satisfied — no wall claim permitted)
**Best = ~25.7 TF = ~89% of cuBLAS (28.8, pinned).** Public hand-written best (siboehm k10, autotuned) is ~93–96% of
cuBLAS — **we are a few % BELOW the public hand-written bar, so per Order 3 we have NOT earned any "wall" claim.** The
89→~95% is finer autotuning toward siboehm's exact config + cuBLAS's holistic 256×128 / 202-reg / deep-pipeline / opt-in-
49KB-shared combination assembled together (each piece proven individually here; not yet combined). The last ~5% (cuBLAS's
edge over the public hand-written best) is the ONLY place a SASS/scheduling limit may live — and that is a hypothesis to
TEST (disassemble, compare instruction mix) AFTER reaching the public bar, never assume. **NEXT GRIND: (1) reproduce
siboehm-class config (256×128 + opt-in dynamic shared + the deep pipeline, combined); (2) then Order-3 test the residual.**

## Step 15 — cuBLAS's 256×128 geometry: two concrete obstacles (diagnosed, not walls)
Tried cuBLAS's exact geometry (256×128, opt-in dynamic shared up to 100 KB, deep pipeline). Two measured obstacles:
1. **Dynamic shared (extern smem + `float* As[NSTAGE]` pointer array) = 18 TF** (vs 25 static). The stage pointer indexed
   by runtime `rs` spills to local memory ⇒ every shared access becomes an indirect load. **Fix: resolve the stage index
   at compile time (unroll the K-loop by NSTAGE), not a runtime pointer array.**
2. **256×128 FAILS TO LAUNCH** (525 "TF" = failed launch, stale warmup data passes the max-check). Root cause: my
   fragment-cache `aFrag[WMITER*TM]` = 16 float4 = **64 registers** on top of 128 accumulators ⇒ **>255 regs, over the
   hardware cap.** cuBLAS fits 256×128 in 202 regs because it does NOT cache a big A-fragment — **its A-read is leaner.**

**Honest conclusion (Order 3 still NOT satisfied — no wall):** the fragment-swizzle mainloop is excellent at 128×128
(89% of cuBLAS) but does not scale to cuBLAS's 256×128 as written — a **design limit with a named fix**, not a hardware
wall. **NEXT GRIND: redesign the A-read to fit 256×128 under 255 regs (per-kk swizzled read, no big fragment cache) +
compile-time-unrolled dynamic-shared stages ⇒ the 256×128 deep pipeline that IS cuBLAS.** Every sub-technique
(cp.async, XOR-swizzle, fragment vectorization, multi-stage, opt-in shared) is now built and proven here individually;
the remaining work is assembling them in the register-lean form cuBLAS uses.

## Step 16 — assembled cuBLAS's geometry; the residual is now pure SASS inner-loop density (measured)
Fixed both Step-15 obstacles: **(1)** dynamic-shared indirection — replaced the runtime `float* As[NSTAGE]` pointer array
(spilled to local, 18 TF) with **inline compile-time-stride stage offsets** → recovered to 25 TF; **(2)** 256×128 launch —
used a **WMITER=1** config so the fragment cache is 8 float4 (32 regs) not 16, fitting under the 255-reg cap; **(3)** added
`#pragma unroll` throughout the inner loop → 256×128 pipe 22 → **25.2 TF**. The register-lean 256×128 pipeline now
**structurally MATCHES cuBLAS**: **219 registers (cuBLAS 202), 16.6% occupancy (cuBLAS 16.6%), all stalls low**
(barrier 0.18, long 0.03, short 0.15). Yet throughput is 25.2 vs cuBLAS 28.9–29.7 (**~85–89%**).

**The residual is precisely located and it is the ONLY thing left:** `sm__pipe_fma_cycles_active = 61%` (vs cuBLAS ~78%),
while `smsp__inst_executed_pipe_fma.sum = 269,340,672 = 2048³/32` — **the theoretical-minimum FMA count, zero wasted
FMAs.** So the kernel does exactly the right work; the FMA pipe simply isn't saturated because overhead instructions
(address math, the `float4` component extract, per-kk B reloads) share issue slots with FMAs. **This is SASS inner-loop
instruction density** — the one axis where cuBLAS's hand-tuned SASS beats what ptxas schedules from CUDA-C.

**Order-3 status (honest):** we are at ~87% ≈ below siboehm's public ~93–96%, so **NO wall claim is earned** — a few % of
FMA-density is still reachable in CUDA-C (register B-fragment scheduling, killing the float4 extract, FMA interleaving).
The final slice to cuBLAS (the 93–96%→100% that even the public best doesn't fully close) is the legitimate place a SASS
scheduling limit may live — to be TESTED (cuobjdump the two inner loops, compare FMA:overhead ratio) once we reach the
public bar, never assumed. **This is a WORLD away from the retracted "58% ceiling" — a structurally-cuBLAS-matched kernel
at ~87%, residual measured to the FMA-instruction.**

## The crush we already hold (axes cuBLAS can't touch)
Bit-exact determinism (T1 fixed-order / T2 reproducible) + portability across 6 backends. cuBLAS offers neither. Raw FP32
parity is the CUTLASS deep-pipeline grind (in progress); the determinism+portability crush is already ours.

## Reusable know-how (banked)
- **Profile counters FIRST, then fix the ONE dominant limiter.** Every step above was prescribed by the prior counters,
  not guessed. `ncu -c 1 -k <regex> --section SpeedOfLight --section Occupancy` is the ~seconds first look.
- **SpeedOfLight decodes the bound type:** both SM%+Mem% low ⇒ latency/occupancy; SM% high ⇒ compute; Mem-busy high +
  DRAM low + L2-hit high ⇒ *shared*-bandwidth-bound (not DRAM).
- **The tiled/vec kernels are the FMA fast tier** — for CKIR this is `DetTier::Fast`; the bit-exact `precise` tier stays
  the naive/no-FMA path. Port the winning schedule to the GLSL/HLSL/WGSL emitters as the T2 GEMM.
- Cross-refs: `feedback_ckir_tiled_gemm_occupancy_not_free` (now corrected — occupancy is the lever, and it CAN be
  fixed), `crush-playbook.md` §H.3, `external/gemm_lab.cu` (the lab).
