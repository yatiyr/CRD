# B-compute GPU FFT — the crush campaign plan (2026-07-13)

> Execute-ready dossier for the hesap-GPU FFT (D-007 **B-cmp**), the compute primitive B13-b's FFT-convolution
> bloom consumes. Written at the close of the orientation+research session so the build starts without re-deriving.
> User mandate (2026-07-13): **build the COMPLETE general CKIR shared-memory IR first** (not minimal — the substrate
> every future on-chip compute kernel reuses); deliver **both** a general 1D/batched complex FFT **and** the 2D
> image FFT-convolution; **FULL CRUSH**; benchmarks the crd-hesap way; reference the CPU FFT; research 2026 SOTA.

## ★ The strategic frame — the crush is FUSION, not raw throughput

The GPU-gotchas doctrine (`docs/hints/v17-kir-gpu-gotchas.md`, the GEMM campaign) is decisive: **cuBLAS/cuFFT run at
~90% of hardware peak; you cannot crush a 90%-of-peak flagship kernel — parity is the raw ceiling. The crush is
FUSION.** 2026 SOTA confirms it for FFT specifically:
- **TurboFNO** (SC'25, arxiv 2504.11681) — fused **FFT→CGEMM→iFFT** single kernel, dataflow-aligned, custom FFT with
  built-in truncation/zero-padding by modifying the global I/O layout. The FNO analogue of our bloom convolution.
- **Kernel-fused single-dispatch FFT pipelines** (arxiv 2604.03585, 2026) — SAR imaging 8 s → 370 ms purely by fusion.
- **Overlap-and-save convolution in shared memory** (arxiv 1910.01972) — the on-chip convolution method.

⇒ **The crown = the fused 2D real-image convolution: forward 2D FFT → ×PSF spectrum → inverse 2D FFT in ONE
on-chip dispatch, beating cuFFT-fwd + separate-multiply + cuFFT-inv end-to-end** (the vendor pays 3 global
round-trips; we pay ~1). This is exactly B13-b. Raw 1D/2D FFT we target cuFFT/VkFFT **parity**; the fused
convolution we **crush**.

## ★ The algorithm (2026 SOTA + our CPU FFT)

- **Stockham autosort** — coalesced global reads, contiguous per-thread access (both our CPU research `fft-stockham-v2.md`
  AND the 2026 papers converge here; avoids the bit-reversal scatter).
- **Radix-8 butterflies + two-tier register↔threadgroup(shared)-memory decomposition** — "Beating vDSP: 138 GFLOPS
  Radix-8 Stockham FFT on Apple Silicon" (arxiv 2603.27569, 2026): register-level radix-8 butterflies, shared memory
  for the inter-pass transpose, **bank-conflict padding** (a known VkFFT/cuFFT under-utilization the paper fixes).
- Radix-4/2 for the odd-log2 cleanup stages (as the CPU ip4-AoS does).
- Twiddle factors: correctly-rounded literal tables + on-the-fly recurrence (the CPU campaign's round-11 win: compact
  tables beat the DTLB/L3). On GPU: precomputed twiddle buffer or computed from `sincos`.
- **Reference implementation to port**: `engine/hesap-fft` (radix-4 Stockham/DIT, twiddle algebra, digit-reversal —
  the 0.71–0.95× MKL engine). The butterfly math + twiddle indexing transfer directly.

## Phase 0 — the CKIR imperative compute-kernel / shared-memory IR (the substrate) ★ BUILD FIRST, COMPLETE

CKIR today is a **whole-array functional dataflow IR** (`input`/`scan`/`reduce`/`gather`/`scatter`/`broadcast` over a
`Shape`); the real shared-memory kernels (tiled GEMM, reduce) are **hardcoded per-backend emitter special-cases**
(`emit_contract_tiled_glsl` bakes `shared float As[512]; … barrier();`). To author a performant FFT (and every future
on-chip kernel — scan/sort/stencil/conv) IN the IR → all backends, add a first-class **workgroup-kernel** layer:

**New KOps / KEntry / KGraph additions (append-only, per the DType::U32 rule):**
1. **`SharedAlloc`** — declare a workgroup-shared array leaf: element `KType` + compile-time length. (GLSL `shared T a[N];`
   · HLSL `groupshared` · CUDA `__shared__` · MSL `threadgroup` · WGSL `var<workgroup>`.) Optional `+pad` field for
   bank-conflict avoidance.
2. **`SharedLoad(arr, idx)` / `SharedStore(arr, idx, val)`** — dynamic-indexed shared access.
3. **`StorageAlloc`** (generalize the B1-f binding-0 `StorageLoad`): a bound global buffer at `(set, binding)`, R / W / RW,
   element type. **`StorageLoad(buf, idx)` / `StorageStore(buf, idx, val)`** — indexed global I/O (multiple buffers).
4. **`Barrier(scope)`** — control + memory barrier: `execution` (GLSL `barrier()` / HLSL
   `GroupMemoryBarrierWithGroupSync()` / CUDA `__syncthreads()` / MSL `threadgroup_barrier(mem_threadgroup)`), plus a
   memory-only variant.
5. **`KEntry` (Compute) additions**: `local_size` (uvec3 workgroup dims) + an **imperative statement body** — an ordered
   list of `(SharedStore | StorageStore | Barrier | For{body} | If{body})` effect-statements, distinct from the pure
   value graph. (The value graph feeds the RHS of stores; the statement list sequences the effects + barriers.)
6. **CPU oracle** (`eval_cpu`): model a workgroup as a serial loop over `local_size` invocations with a shared-array
   scratch and barrier = a full-workgroup sync point (execute all invocations up to the barrier, then continue) — so a
   shared-memory kernel is CPU-evaluable + bit-exact-testable, exactly like the functional ops.
7. **Emitters**: GLSL/HLSL/CUDA/MSL/WGSL emit the shared decls, the `layout(local_size)`/`numthreads`/`<<<>>>` sizing,
   the indexed loads/stores, the barriers, and the statement sequence. **All five backends** (the mission).
8. **Determinism**: `precise` temps (SPIR-V NoContraction) for bit-exact add/sub/mul; div/transcendentals ULP-tolerant on
   Vulkan, bit-exact on CUDA `--prec-div --fmad=false` (the v17-b/c split). Barriers make workgroup ordering deterministic.

**DoD Phase 0:** a hand-authored shared-memory kernel in CKIR (start with a **workgroup reduction** and a **shared-memory
transpose** — the FFT's two primitives) → bit-exact CPU oracle + runs on Vulkan + DX12 (+ CUDA) → tidy + canary held.
Then a shared-memory **radix-2 butterfly pass** as the FFT unit test.

## Phase 1 — 1D / batched complex FFT (parity target: cuFFT 1D)

Radix-8 Stockham, shared-memory two-tier, forward+inverse, batched. Sizes 2^8–2^24 (workgroup FFT ≤ 4096/8192; multi-pass
global for larger — the "four-step"/six-step frameworks the CPU FFT already uses, `Van Loan`/`Takahashi`). Bit-exact vs the
CPU FFT oracle (matched op order) + both backends. **Bench** vs cuFFT (via the `external/bench_vendor.cu` harness), VkFFT
(if buildable), our CPU FFT, FFTW — full board, min-of-N runs, clock-locked (`nvidia-smi -lgc`, per the gotchas).

## Phase 2 — 2D FFT (row + column; the image transform)

Row FFTs → transpose (shared-memory tiled, the v14-d permute discipline) → column FFTs. Real→complex (`R2C`) forward +
`C2R` inverse (half-spectrum, the Hermitian-symmetry economy). Sizes 256²–2048². Bench vs cuFFT 2D.

## Phase 3 — the FUSED FFT-convolution (THE CRUSH) ★

Fwd 2D FFT → ×PSF spectrum (the `complex_mul` already in `ckir_bloom.hpp`) → inv 2D FFT, keeping the tile on-chip
(overlap-save for images > workgroup tile). **Crush metric: end-to-end fused vs cuFFT-fwd + cublas/thrust-mul +
cuFFT-inv.** This is B13-b's FFT-convolution bloom, delivered. Also wire the B13-b FFT-glare path onto it.

## Benchmark doctrine (the crd-hesap golden rules — non-negotiable)

- **Bench ALL peers, no cherry-pick** (cuFFT PEDANTIC + default, VkFFT, our CPU FFT, FFTW) — the full honest scoreboard.
- **min-of-N ≥ 3 runs**, clock-locked; report the WORST (most conservative for us). Boost/thermal variance ±10–20%.
- **Matched accuracy** — same precision (f32/f64), report maxrel vs the f64 oracle; a crush must be at matched accuracy.
- **Every measured board → `docs/bench/` at measurement time.** Source must match the honest scoreboard (grep for Nx claims).
- **NEVER partial-metric victory**; **SOLVE losses, never document-and-accept**; crush persists (research, don't retreat).
- Gold standards: cuFFT (GPU), VkFFT (GPU portable), MKL/FFTW (CPU). Our CPU FFT is 0.71–0.95× MKL — the GPU FFT's crush is
  the FUSION, and raw-throughput cuFFT parity where the hardware allows.

## Immediate next step

Build **Phase 0** (the CKIR shared-memory/barrier/storage imperative-kernel IR, complete, all 5 backends) — start with the
workgroup-reduction + shared-transpose primitives, bit-exact + both backends, then the radix-2 butterfly pass. Fresh
focused session per the "big-kernel work wants fresh context" gotcha.
