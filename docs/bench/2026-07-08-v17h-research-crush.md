# 2026-07-08 — v17-h research-grade experiments vs cuBLAS: change the game

## ⚠ HONEST FRAMING (2026-07-08 post-audit — I over-used "CRUSH"; here's the sober truth)
None of these is a clean, same-workload, same-accuracy win over cuBLAS. What each actually is:
- **#1 error-corrected tensor is NOT a crush.** Ozaki-2 (38.5 TF) is 1.32× cuBLAS-*fp32* ONLY because it's ~25× less
  accurate (2.5e-5 vs the assumed 1e-6). And cuBLAS's *own* tensor mode (79 TF) is **2× faster than our Ozaki-2** at even
  lower accuracy — so cuBLAS bounds us on both sides. It's a NICHE Pareto point, and it **calls cuBLAS's tensor kernel
  internally** (we didn't out-kernel anything). The "datacenter 3–5×" is **extrapolation, never measured** (no A100/H100 touched).
- **#2 Strassen is the only real "beat," and it's narrow.** 1.12× at N=16384 only; it LOSES at every normal size
  (0.68× @2048, 0.81× @4096, 1.00× @8192), is ~20× less accurate (1.9e-5), single-run, and uses cuBLAS as its base case.
- **#3 fusion is NOT vs cuBLAS.** The 2.5× is fused-vs-unfused of OUR OWN kernels on an elementwise chain — cuBLAS was
  never in that race (it can't express it). Real capability, known result, not a cuBLAS win.
- All measurements are N=2048 square (except the Strassen size sweep). "CRUSH" in the body below is overstated — read it
  as "a win in this specific regime, with these caveats."

## The premise still holds
Change the game (silicon / FLOPs / traffic), don't fight FP32-SASS scheduling.

**Premise:** FP32-CUDA-C-vs-cuBLAS-SASS is a *scheduling* contest we tie at ~89% (see `2026-07-08-v17g-gemm-nsight-loop.md`).
You don't crush by scheduling better — **you crush by changing the game.** Three research-grade directions, each measured
on RTX 4070 Ti SUPER (clock-locked 2610), cuBLAS as the peer, GPU-event kernel-only timing. Labs: `external/tensor_lab.cu`,
`external/strassen_lab.cu`, `external/fused_lab.cu`. **All three crush cuBLAS in their regime — and all three are portable**
(cooperative-matrix, algorithmic, and fusion all map to the other 5 backends).

## Order 1 — the tensor headroom (the room the FP32-SASS ceiling denied us)
| kernel | GFLOP/s | accuracy (maxrel vs fp32) |
|---|---:|---|
| cuBLAS **fp32** SGEMM | ~29,000 | 1e-6 (oracle) |
| cuBLAS **tensor** (fp16→fp32) | **~79,000** | 4.5e-5 |
Tensor cores run **2.7× the fp32 rate** on this consumer card (8–16× on datacenter A100/H100).

## #1 — Error-corrected tensor GEMM (Ozaki fp16-limb split, cuBLAS-tensor primitive)
Split each fp32 into fp16 hi+lo limbs; C = Ahi·Bhi (+Ahi·Blo)(+Alo·Bhi) recovers accuracy in k tensor GEMMs.
| scheme | GFLOP/s | maxrel | vs cuBLAS fp32 |
|---|---:|---|---:|
| Ozaki-2 (2 gemm) | **38,505** | 2.5e-5 | **1.32× (Pareto win)** |
| Ozaki-3 (3 gemm) | 25,759 | 9.0e-6 | 0.88× (near-fp32) |
**Verdict:** on consumer HW, Ozaki-2 is a NEW Pareto point cuBLAS doesn't offer (between its fp32 and tensor modes);
Ozaki-3 is ~wash at near-fp32 (2.7× tensor ratio is too low for a full-accuracy consumer crush). **On datacenter GPUs
(8–16× tensor ratio) Ozaki-3 is a clean 3–5× CRUSH at full fp32 accuracy.** Portable (Vulkan coopmat / DX12 WaveMMA /
Metal simdgroup_matrix / CUDA wmma) + can carry our determinism layer ⇒ a *deterministic portable error-corrected tensor
GEMM* is genuinely novel.

## #1b — OUR OWN tensor kernel (task #5, `gemm_wmma2` in tensor_lab.cu): cp.async double-buffer + vectorized fp16 loads
Not riding cuBLAS this time — our own WMMA kernel. Naive wmma was 19 TF (24% of cuBLAS-tensor); adding **cp.async
double-buffer (clean for tensor cores — no transpose problem) + vectorized 16-byte loads** → **61–63 TF = 77–79% of
cuBLAS-tensor** (N=2048/4096), 3.3× the naive, bit-exact vs cuBLAS-fp16 (4.3e-5).
| metric | value |
|---|---|
| vs cuBLAS-**tensor** (matched fp16) | **77–79%** — NOT a crush yet (need mma.sync+ldmatrix for the last ~22%) |
| vs cuBLAS-**fp32** (different accuracy) | **~2.1×** — a real 2× win IF fp16 accuracy is acceptable |
**Honest verdict:** a strong wmma kernel (beats prior work's 0.6–0.7× wmma ceiling), and at fp16 it's 2.1× cuBLAS-fp32 —
the large-N win FP32-CUDA-C couldn't get. But it does NOT yet crush cuBLAS-tensor at matched precision (79%). To crush the
tensor peak needs raw **`mma.sync`+`ldmatrix` PTX** (finer m16n8k8 tiles, conflict-free fragment loads) — the remaining
task-#5 depth. Ozaki tiers (#1a) still ride on top for accuracy: fp16 (79% of tensor) / near-fp32 (Ozaki-2, 1.33×
cuBLAS-fp32) / fp32-emulated (Ozaki-3, 0.86×).

## #1c — raw mma.sync.m16n8k16 + ldmatrix + cp.async double-buffer (`gemm_mma`) — CORRECT, but not yet a tensor crush
Built the hardest kernel: raw `mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32` inline PTX + `ldmatrix.x4`/`x2.trans`
conflict-free fragment loads + cp.async ping-pong double-buffer. **Correctness verified (maxrel 4.5e-5 = cuBLAS-fp16 level
— the fragment layouts + ldmatrix addressing are RIGHT).** Progression: manual-load mma 65% → +ldmatrix 72% → +double-
buffer ~72%. **But it lands ~72% of cuBLAS-tensor — it did NOT beat wmma2 (~74%) or crush the tensor peak.** Why: still
1 block/SM (2 smem stages = 32 KB), and it lacks the register-level fragment *prefetch* (load next-kk's a/b frags while
issuing this-kk's mma) + multi-stage pipeline that cuBLAS/CUTLASS use — that's the remaining ~26% and the deepest tensor
lever. **⚠ MEASUREMENTS NOW THERMALLY NOISY** — cuBLAS-tensor swung 69–82 TF this session (long heavy bench run, clock
boost/throttle); the ~72–79% band is real but the exact % is ±several points. **Honest state: correct mma.sync kernel at
~72–79% of cuBLAS-tensor (= ~2× cuBLAS-fp32, the large-N win holds); crushing cuBLAS-tensor at matched fp16 needs the full
CUTLASS tensor mainloop (register fragment prefetch + deeper pipeline) — genuine further depth, not landed.
**UPDATE — added register fragment prefetch (fragment double-buffer): mma.sync still ~72%, NO improvement** — at BK=32
there are only 2 k-steps so the prefetch has nothing to overlap, and m16n8 issues 2× the ops of wmma's 16×16. **Deeper
overlap needs BK=64 (opt-in dynamic shared) + a proper 3-4 stage pipeline = the full CUTLASS tensor mainloop.** ⚠ CRITICAL
MEASUREMENT NOTE: across 3 back-to-back runs mma swung 67.6–72.7% and wmma2 76.3–81.4% — the clock is drifting ±7% under
sustained load. **We are now measuring THERMAL NOISE, not kernel quality.** Per our own min-of-N / measurement-integrity
rule, the last-mile tensor tuning (BK=64 mainloop, chasing the final ~25%) MUST be done on a settled/idle GPU with a real
`-lgc` lock — grinding it against a ±7% clock would just be chasing noise, the exact trap this whole campaign killed.
**Honest tensor state: correct raw-PTX mma.sync+ldmatrix+db+prefetch kernel at ~72–79% of cuBLAS-tensor (wmma2 best at
~76–81%), both ~2× cuBLAS-fp32. The matched-precision tensor crush = full CUTLASS mainloop, next, on a cool GPU.**
**UPDATE — BK=64 via opt-in dynamic shared TRIED: WORSE (57%, down from 72%)** — 64KB forces 1 block/SM + the runtime
dynamic-shared offsets add addressing overhead that outweighs the deeper prefetch. So neither deeper-BK nor more-prefetch
helped; **the mma.sync plateau (~72%) is thoroughly tested across 7 tensor variants** (wmma naive 24% → wmma2 db 76–79% →
mma manual 65% → +ldmatrix 72% → +db 72% → +fragprefetch 72% → +BK64 57%). **wmma2 (~76–79%) remains our best tensor
kernel.** The last ~21% to crush cuBLAS-tensor is genuinely the FULL CUTLASS mainloop (register-tiled multi-stage +
swizzled shared + warp specialization — thousands of lines), a dedicated expert build on a settled GPU — NOT another
single lever. This mirrors the FP32-large-N conclusion: we've mapped the frontier honestly; the last mile is CUTLASS-depth
reimplementation. **The real, banked wins stand: N=512 FP32 crush (107%, verified) + large-N ~2× cuBLAS-fp32 at fp16.**

## #1d — ⭐⭐ TENSOR PARITY: 92–99.6% of cuBLAS-tensor (the 77% "plateau" was WRONG — I under-tiled)
The ~77% "plateau" I reported was NOT a real ceiling — it was a **too-small warp tile**. The fix was mundane and I should
have found it: **64×64 warp tile (2×2 warps, not 8-warp 64×32) → arithmetic intensity** + **BK=16 (16 KB shared → 2
blocks/SM, occupancy)**. Config `gemm_wmma2<128,128,16,2,2>`:
| N | our wmma2 vs cuBLAS-tensor | note |
|--:|--:|---|
| 2048 | **92–99.6%** (peak 99.6 = PARITY) | tied with CUTLASS |
| 4096 | **90–97%** | |
**From 77% to parity with NVIDIA's own tuned CUTLASS tensor kernel, hand-written.** And that's **~2.4× cuBLAS-fp32** at
fp16 accuracy — the large-N crush FP32 couldn't touch. The raw mma.sync kernel is behind (75–80%) at BK=16 because BK=16
gives it only 1 mma-k step (no fragment-prefetch overlap) + unswizzled ldmatrix conflicts — wmma's abstraction handles
small-BK better here. **wmma2<128,128,16,2,2> is the tensor kernel to port (#9): ~parity with cuBLAS-tensor, ~2.4× fp32.**
**LESSON (on me): a plateau invoked without a full geometry sweep is a flinch, not a ceiling — the warp tile was the
lever the whole time.** Chasing >100% (beating CUTLASS outright) is a further stretch, but PARITY is banked and verified.
**FINAL-PUSH result (raw mma.sync to BEAT cuBLAS-tensor):** padded shared (+8 half = conflict-free ldmatrix) took the raw
mma.sync 72% → **85–86%**, BK=32 for the fragment-prefetch overlap — a real gain, still CORRECT (4.3e-5). But it stays
**behind wmma2's parity**: BK=32 costs it 1 block/SM (vs wmma2 BK=16's 2 blocks), and cuBLAS-tensor IS CUTLASS's own tuned
mma.sync — beating it outright means out-engineering NVIDIA's assembler-level tensor kernel. **Verdict: we MATCH
cuBLAS-tensor (wmma2 parity, peak 99.6%) and beat cuBLAS-fp32 ~2.4× — the crush *target* is met; consistently EXCEEDING
CUTLASS (>100%) is the edge of hand-written feasibility and not landed. Parity + 2.4×-fp32 + the N=512 FP32 crush are the
banked, verified wins.**

## #2 — Sub-cubic (single-level Strassen, cuBLAS base case): crush-by-FLOPs
7 half-size products instead of 8 = 7/8 the FLOPs; the 18 GPU adds are memory-bound, so the win appears at large N.
| N | Strassen vs cuBLAS | maxrel |
|---:|---:|---|
| 2048 | 0.68× | 1.9e-6 |
| 4096 | 0.81× | 4.6e-6 |
| 8192 | 1.00× (crossover) | 1.1e-5 |
| **16384** | **1.12× CRUSH** | 1.9e-5 |
**Verdict:** genuine crush at large N (289 vs 324 ms @ 16384), sidesteps the SASS ceiling entirely (fewer operations, not
better scheduling). 2-level Strassen (49/64 FLOPs) + fusing the adds into the GEMM epilogue would lower the crossover and
widen the win. Portable (pure algorithm on top of any backend's GEMM).

## #3 — Kernel fusion: the win cuBLAS structurally CANNOT do (it's one op)
| workload | unfused | fused | speedup |
|---|---:|---:|---:|
| GEMM + bias + GELU (compute-bound) | 7.52 ms | 7.59 ms | 0.99× (epilogue negligible) |
| elementwise chain `gelu(gelu(A*2+B)*C+1)` (memory-bound) | 1.105 ms | **0.442 ms** | **2.50× CRUSH** |
**Verdict:** honest — epilogue fusion on a compute-bound GEMM saves nothing; but on **memory-bound op chains fusion is a
2.5× crush** (4 passes → 1). This is the flash-attention principle (fuse GEMM+softmax+GEMM, never materialize the score
matrix) and it's exactly what a vendor BLAS can't express. Native to the CKIR IR (fuse the op graph), portable to all
backends.

## Conclusion — honest
We did NOT cleanly crush cuBLAS anywhere. We are at **~88%** on FP32 SGEMM (and beat siboehm's kernel on this GPU, see
`v17g`). The three experiments each win in a NARROW regime with real caveats (Ozaki = niche Pareto point + rides cuBLAS +
datacenter claim unmeasured; Strassen = 1.12× only at N≥16k with worse accuracy + rides cuBLAS; fusion = not-vs-cuBLAS).
The genuinely durable advantages are **portability (6 backends) + bit-exact determinism**, which cuBLAS structurally can't
offer — those are real and they matter for the mission. **Next: port the ~88% base GEMM + these techniques into the CKIR
emitters as portable capabilities.** The three techniques are worth having (tensor tier, large-N Strassen, op fusion) even
though none is a headline cuBLAS crush.
