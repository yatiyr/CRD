# The tiled register-blocked 2-D FFT — execute-ready crush plan (2026-07-13)

> The path to crushing cuFFT at 1024²+ (single-image) 2-D FFT / FFT-convolution. Written after the fused-2-D-conv landed
> (256² **2.91×** crush, 1024² **0.40×** loss) so a fresh focused session executes without re-deriving. Reference board:
> `docs/bench/2026-07-13-gpu-fft-cufft-gold.md` §"Phase 3". User decision 2026-07-13: **commit to the tiled 2-D FFT** (the
> only lever that crushes at L2-resident sizes). Big-kernel work wants fresh context (the campaign's own gotcha) — this is it.

## ★ The measured gap (this is DATA, not theory — profile first, always)

The fused 2-D conv at 1024² is 0.118 ms vs cuFFT 0.047 ms = 0.40×. A per-pass profile (`[.fft2dconv-pass]`, run it again to
reconfirm) at 1024²:

| pass | ms | note |
|---|---|---|
| row FFT | 0.0235 | 16 MB global @ **~680 GB/s ≈ peak** — a single pass is already optimal |
| transpose re | 0.0081 | coalesced tile² transpose (already fixed this session) |
| transpose im | 0.0079 | |
| fused col conv | 0.0398 | fwd+inv FFT + ×H (≈ 2 FFT passes) |
| transpose re back | 0.0080 | |
| transpose im back | 0.0079 | |
| inv row FFT | 0.0236 | |

**Key facts the profile establishes:**
1. **Each single FFT pass is already at peak global bandwidth (~680 GB/s).** We cannot make one pass faster — only do FEWER.
2. **The transposes are only 27%** now (0.032 ms). Removing 100% of them → ~0.086 ms = ~0.55× — STILL a loss. Transpose-on-
   write ALONE does not crush; it fixes a 27% limiter while the 73% FFT floor remains.
3. **The FFT passes (0.087 ms) are the floor, and that floor already exceeds cuFFT's WHOLE conv (0.047 ms).** We and cuFFT do
   the same FFT work (~4 FFT-equivalents + 1 multiply), so cuFFT is ~1.85× more efficient *per FFT-equivalent*.

**Root cause (the ONLY thing that closes it):** our 2-D FFT is **row-FFT (global write) → transpose (global) → col-FFT (global
read)** = **3 global round-trips (48 MB)** for one 2-D FFT. cuFFT keeps the intermediate **on-chip** (register-blocked tiles,
transpose fused into the FFT kernels) → ~1 round-trip. cuFFT proves 0.047 ms is achievable ⇒ **not a wall**; the gap is our
3-round-trip decomposition. The crush = **fewer global round-trips for the 2-D FFT itself**, which needs on-chip tiling.

## ★ The technique — register-blocked FFT + transpose-on-write (what cuFFT does)

Two compounding levers, both required:

### 1. Register-blocked radix-4 FFT (reduce shared-memory traffic + free shared for tiling)
Today every radix-4 stage **ping-pongs the whole line through shared** (`SharedStore` → `Barrier` → `SharedLoad`), log₄(N)
times. A register-blocked FFT keeps points in **registers (SSA temps)** across MULTIPLE stages and touches shared only for the
inter-thread exchange:
- Each thread owns **R^K points** (e.g. R=4, K=2 ⇒ 16 points/thread) and does K radix-4 stages **entirely in registers** (a
  16-point sub-FFT), then ONE shared exchange for the next K stages. log₄(1024)=5 stages ⇒ ~2-3 shared exchanges instead of 5.
- CKIR is ready: the value graph already holds butterflies as SSA temps; the change is authoring the store/exchange pattern so
  a thread's R^K points live as temps across stages, `SharedStore`/`Barrier`/`SharedLoad` only at the K-stage boundary.
- **Bit-exactness:** identical op order + `precise` temps ⇒ still bit-exact (the register form is the same DAG, fewer shared
  round-trips). Verify with the existing `[kir][kernel][fft]` oracle gate FIRST, then both-GPU dispatch.

### 2. Transpose-on-write fused into the FFT (kill the separate transpose pass)
A workgroup processes a **TILE of rows** (not one), stages results in shared, and writes **coalesced transposed TILE×TILE
blocks** (the standard transpose write). This eliminates passes 1,2 (fwd) and 4,5 (inv): 2-D FFT goes 3 round-trips → 2.
- **The shared-budget problem that blocks the naïve version:** the current shared-heavy FFT needs 4·cols floats/row; a tile of
  rows blows 48 KB at cols=1024. **Register-blocking (lever 1) is the enabler** — points live in registers, so shared holds
  only the exchange staging + the transpose tile, freeing room for TILE rows.
- Consecutive threads write consecutive output columns ⇒ coalesced. +1 padded shared stride for the transposed read (as the
  fixed `build_transpose2d` already does).

Compose: **row-FFT-write-transposed (16 MB) → fused col-conv-write-transposed (24 MB) → inv-row-FFT (16 MB) = 56 MB, 3
dispatches** (was 88 MB, 7 dispatches). With register-blocking cutting per-pass shared traffic, the passes stay ≥ peak-BW ⇒
target ~0.06-0.07 ms at 1024² → **parity-to-crush** vs cuFFT 0.047 ms; the DRAM-bound batched regime then crushes outright.

## ★ Build order (each step bit-exact on the oracle THEN both GPUs before the next)

0. **Reconfirm the profile** — re-add the `[.fft2dconv-pass]` breakdown, verify the floor is still the FFT passes (guard
   against a stale premise; the transpose fix already moved the limiter once).
1. **Register-blocked radix-4 1-D FFT** (`build_fft1d_radix4_regblock`) — R=4, K=2 (16 pts/thread). Oracle bit-exact vs the
   existing radix-4 at N=16..1024; then both-GPU dispatch bit-exact. Measure a single 1-D pass — expect the same peak BW (it's
   already BW-bound) but LOWER shared traffic (headroom for tiling). This is the reusable core.
2. **Transpose-on-write row FFT** (`build_fft1d_batched_transpose_out`) — TILE rows/workgroup, register-blocked, coalesced
   transposed write. Oracle: composing it with a plain col FFT reproduces the 2-D DFT bit-exact. Both-GPU.
3. **3-dispatch 2-D FFT** (`build_fft2d_c2c` variant): row-FFT-T → col-FFT-T → (natural). Bit-exact both GPUs; re-bench raw 2-D
   FFT vs cuFFT-2D (new peer: `cufft_2d_bench.cu`, plain C2C, no conv).
4. **3-dispatch fused 2-D conv**: row-FFT-T → fused-col-conv-T → inv-row-FFT. Bit-exact both GPUs. Re-bench `[.fft2dconv-bench]`
   at 256²/1024² (clock-locked this time). Target: crush both.
5. **Batched-image board** (DRAM-bound): add an image offset; measure batched throughput vs `cufftPlanMany` 2-D conv — the
   regime where fewer-round-trips dominates (the 1-D crush was 1.99× here). Full peer board → `docs/bench`.
6. **R2C/C2R half-spectrum** (real images halve the work) + larger-N four-step (N>3072) as follow-ons.

## ★ Guardrails (the campaign's non-negotiables)
- **Bit-exact on CPU oracle + Vulkan + DX12 at every step** (the mission). `precise` temps; the oracle rounds every op.
- **Profile before optimizing; refute your own premise** (SANITY #5) — the floor moved once already (transposes 69%→27%).
- **Honest board, no partial-victory** — report losses next to wins; a loss is an open bug, not a closed slice.
- **Clock-locked min-of-N** for any headline (`nvidia-smi -lgc 2100,2100 -lmc 10501`, elevated; reset `-rgc/-rmc`).
- **NOT a wall** — cuFFT reaches 0.047 ms; it is achievable. Pin the peer, fix the one limiter (round-trips), autotune TILE/K.

## ★ Cheap-lever hypothesis — L2 residency — **TESTED 2026-07-13: REFUTED**

Hypothesis: our ~680 GB/s ≈ DRAM peak (vs cuFFT's L2-resident ~1.9 TB/s) meant the intermediate was **evicting to DRAM**
because the fused conv allocated **18 distinct image planes (~56 MB > 48 MB L2)**. **Test:** rewrote `build_fft2d_convolution`
to a **2-buffer ping-pong (A/B) + res-reuses-in** — working set 8 image planes = **32 MB < 48 MB L2** (correctness held, still
bit-exact all 3 backends). **Result: 1024² 0.118 → 0.116 ms — a ~2 % change. REFUTED.** The working set now fits L2 and the
pipeline did NOT speed up ⇒ single-image 1024² is **not L2-eviction-bound**. (The ping-pong reuse is KEPT — strictly better:
less memory, cleaner, tiny win.)

## ★ Reassessment after the refutation (READ before committing to the kernel build)

Two hypotheses are now **weakened by measurement**, so the register-blocked build is higher-risk than first scoped:
- **L2 residency — refuted** (above).
- **High-radix / fewer-stages — doubtful:** the raw-1D board already found **radix-8 == radix-4 (parity, both single-pass
  bandwidth-bound)** — fewer stages did NOT beat more stages. So a register-blocked / high-radix FFT may hit the **same
  bandwidth wall** and NOT deliver the ~2× needed. The per-pass profile shows each FFT pass at ~680 GB/s ≈ DRAM peak with
  ~100 % theoretical occupancy (16 KB shared, 256 threads ⇒ 8 wg/SM) — so it is NOT obviously occupancy-limited either.

**What this means:** the remaining gap (cuFFT does the SAME ~88 MB of conv traffic at ~1.9 TB/s L2 vs our ~745 GB/s) is
**cuFFT exploiting L2 bandwidth that our kernels do not** — and the cheap structural fixes (buffers, barriers, radix) did not
unlock it. Pinning WHY requires a **real GPU profiler on the Vulkan kernels** (Nsight Graphics), or porting our FFT through the
**CUDA emitter** and profiling with **Nsight Compute (ncu)** to read L2-hit-rate / DRAM-throughput / stall-reasons / achieved
occupancy directly. **Building the register-blocked kernel WITHOUT that profile is guessing (SANITY #5/#7) — and the evidence
says it may not pay off.** Do the profile first.

**Honest doctrine check:** this is the campaign's own stated position — *"raw cuFFT parity is the ceiling; the crush is
FUSION."* We CRUSH where cuFFT is overhead-bound (256²: **2.91×**). At L2-resident 1024² cuFFT is near its ceiling and beating
it means reproducing its register-blocked L2-efficient kernel (Order-3 of the crush mandate: reproduce the best public
hand-written result before claiming the limit). That is a genuine cuFFT-class effort, now with measured evidence that the
obvious levers don't shortcut it.

## ★★ THE ncu CAMPAIGN WAS RUN (2026-07-13, same session) — limiters PINNED, two kernel generations SHIPPED

The profile happened (`bench/gpu-fft/cufft_1024_profilee.cu` + CUDA-emitted our-kernels via `[.emit-fft-cuda]` →
`ckir_fft_profilee.cu`, ncu `--set basic`). Everything below is MEASURED (full numbers in the bench board §"ncu-driven
kernel campaign"):
- **cuFFT anatomy:** 2 kernels per 2-D FFT, `EPT<16>` register-blocked, 64-thr blocks, **4.2 KB shared**, transpose fused
  into the strided kernel; cold 18.8+25.3 μs at SM 21%/DRAM 69% (memory-bound); the real 47 μs rides inter-kernel L2 reuse.
- **Our diagnosis:** radix-4 kernels were INSTRUCTION-bound (SM 57%) ⇒ pinned at DRAM speed even L2-warm (our transposes
  already ran at 2.0 TB/s warm — the FFT kernels could not).
- **Shipped levers** (each bit-exact oracle+Vulkan first): `build_fft1d_radix16` + `build_fft1d_convolution16`
  (register-blocked [16,16,4], 16-pt DFT = two exact DFT4 layers + W₁₆ from the same table) then direct-global-I/O v2
  (stage-0 loads global→registers, last stage stores global, conv ×filter fused into fwd-last stage, barriers 11→5):
  **1024² 0.280 → 0.0933 ms (3.0× this session), 0.51× vs cuFFT; 256² 2.90× CRUSH held.**
- **Remaining walls (ncu-pinned):** occupancy ~20% (16 KB 4-array shared ping-pong ⇒ 5 blocks×64 thr/SM; cuFFT's 4.2 KB ⇒
  75%) + the 4 transpose passes (~32 μs warm, already at L2 speed — irreducible as separate passes).

## ★★ THE ENDGAME (the committed multi-session build — now fully specified by data)

1. **`Materialize` IR statement** (the register-residency substrate): force a value node into a named temp AT a statement
   position (all 5 emitters + oracle per-thread cache). Unlocks read-before-overwrite exchanges: ONE 4 KB shared staging
   array, re/im time-multiplexed (write re → barrier → read re → barrier → write im → …) = cuFFT's exchange. Occupancy
   20% → ~65-75%.
2. **Multi-row FFT blocks + tile-staged TRANSPOSED writes**: a block processes TILE rows (now affordable — 4 KB/row), the
   final stage stages a TILE×TILE tile in shared and writes coalesced transposed blocks ⇒ the 4 transpose passes DIE;
   pipeline 7 → 3 dispatches (row-FFT-T → fused-strided-conv → inv-row-FFT), 88 → 56 MB.
3. Re-bench clock-locked; batched-image DRAM-bound board (56 vs 88 MB ⇒ ~1.57× structural advantage there too).

**Projection: ~29-35 μs at 1024² ⇒ ~1.4-1.6× CRUSH** (cuFFT 47 μs). Every number above is measured, not estimated;
the only projection is composing them. Order: Materialize IR (+ its own oracle/emitter tests) → small-shared radix-16
(re-verify bit-exact) → multi-row transposed-write variant → the 3-dispatch conv → boards.
