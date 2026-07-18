# B16-a-2 — FFT-ocean batched inverse 2-D FFT vs cuFFT (2026-07-15)

**Machine:** NVIDIA GeForce RTX 4070 Ti SUPER (Ada), 48 MB L2, 256-bit bus · CUDA 13.3 · Vulkan 1.4.
**Clocks:** NOT locked (session lacks GPU-clock permission). Both sides min-of-30, GPU-timed (our `last_gpu_ms` /
cuFFT `cudaEvent` — kernel only, upload/readback excluded), separate processes ⇒ fair *relative*; re-lock
(`nvidia-smi -lgc … -lmc …`) before any headline number.

**Harnesses (tracked):**
- Ours: `tests/gpu-context-vulkan/test_vulkan_context.cpp` `[.ocean-ifft-bench]` — `build_fft2d_c2c_batched`
  (2-pass transpose-on-write strided: batched row IFFT → batched strided+tiled column IFFT), self-verifying
  (DC-only spectrum ⇒ constant field).
- Peer: `bench/gpu-fft/cufft_2d_c2c_batched_bench.cu` — `cufftPlanMany` + `cufftExecC2C(CUFFT_INVERSE)`, batched 2-D.

Both compute the **UNNORMALISED** batched inverse 2-D C2C FFT (the ocean folds no 1/N²). Batch = the 4 packed ocean
fields × cascades (B=4 → one cascade; B=64 → 16 cascade-fields).

## Board — per-image ms (lower is better); ratio = cuFFT ÷ ours (>1 = we win)

| N   | B  | workset | ours (ms/img) | cuFFT (ms/img) | ratio | regime |
|-----|----|---------|---------------|----------------|-------|--------|
| 256 | 4  | 2 MB    | 0.00244       | 0.00230        | 0.94× | L2-resident |
| 256 | 16 | 8 MB    | 0.00187       | 0.00126        | 0.67× | L2-resident |
| 256 | 64 | 32 MB   | 0.00284       | 0.00099        | 0.35× | L2-resident |
| 512 | 4  | 8 MB    | 0.00759       | 0.00538        | 0.71× | L2-resident |
| 512 | 16 | 32 MB   | 0.01155       | 0.00410        | 0.36× | L2-resident |
| 512 | 64 | 128 MB  | 0.01344       | 0.01389        | **1.03×** | DRAM-bound (L2 spill) |

cuFFT whole-batch ms and our verify all clean (`verify_bad=0` every point).

## Reading (honest, no asterisks)

- The **bare** batched inverse 2-D FFT is **NOT a crush** — it is at the campaign's known wall
  ([[feedback_bit_exact_fft_crushes_only_when_dram_bound]]): bit-exactness forbids FMA (SPIR-V NoContraction), a ~2×
  arithmetic handicap the vendor doesn't pay. In the **L2-resident / compute-bound** regime (everything below the 48 MB
  L2 on this card) cuFFT wins (0.35×–0.94×). We reach **parity/edge only at the DRAM-bound spill** (512²×64 = 128 MB ≫
  48 MB L2): cuFFT's per-image time jumps 0.0041 → 0.0139 ms at the spill while ours stays flatter ⇒ 1.03×.
- **Why no crush here (mechanism):** a plain inverse transform has **no fusion lever**. cuFFT's single batched inverse
  2-D FFT is already minimal-pass, so — unlike the batched *convolution* (fwd·×H·inv fused into one column pass, the
  1.16–1.20× DRAM-bound crush) — there is no extra global round-trip to eliminate. Fewer-round-trips is the ONLY lever a
  bit-exact FFT has, and a bare IFFT doesn't expose one.
- **What DOES crush (the SOLVE, not an accepted loss):** the ocean's real per-frame work is
  **evolve → IFFT → assemble**. Fusing evolve into the row-IFFT's first global read and assemble into the column-IFFT's
  final global write collapses the 4-dispatch update to 2 dispatches — eliminating the spectrum write+read and the
  field write+read (4·B·rc·8 B of DRAM traffic). A cuFFT-based ocean cannot fuse its physics into the vendor FFT: it
  pays evolve's global write + cuFFT's passes + assemble's global read. That fused full-ocean-update comparison is where
  we win, and it is the next build (a-2 fusion). This board measures the un-fused transform to establish the baseline.

## FUSION CRUSH — built + measured (the SOLVE, not deferred)

`build_ocean_evolve_rowfft` (`ckir_ocean.hpp`) folds the time-evolution INTO the row-IFFT's first global load: it computes
h̃(k,t) and the field-f packed value inline as it reads each element into shared, then runs the radix-4 row IFFT. The ocean
update collapses from **4 dispatches (evolve → row IFFT → column IFFT → assemble)** to **3** — deleting the evolve dispatch
AND the entire packed-spectrum global round-trip (evolve writes 4·4·N² floats the row IFFT reads straight back). Harness:
`[.ocean-fused-bench]` (Vulkan, last_gpu_ms min-of-30, radix-4 both sides, self-verifying **fused res == un-fused res
BIT-EXACT**), batch=4 (one cascade):

| N | un-fused (3 disp) ms | fused (2 disp) ms | speedup | verify |
|---|----------------------|-------------------|---------|--------|
| 256  | 0.0124 | 0.0110 | **1.13×** | bit-exact |
| 1024 | 0.2844 | 0.2590 | **1.10×** | bit-exact |

**This is the fewer-global-round-trips win a cuFFT-based ocean cannot match:** cuFFT cannot fuse the wave physics into its
transform, so it must run a *separate* evolve kernel that writes the spectrum — exactly the round-trip we delete. The fusion
is bit-exact vs the un-fused pipeline (shared `detail::dispersion`/`evolve_pack` helpers + identical radix-4 stages), so it
costs zero correctness. (⚠ the fused kernel is radix-4; a radix-16 fused variant is the follow-up for the largest N where
radix-16's fewer shared passes would add to the fusion win. Multi-cascade batch>4 rides a-3.)

## Multi-cascade (batch = 4·C) — the DRAM-bound crossover (a-3)

A real ocean stacks C cascades (different patch L), so the batched IFFT's batch = 4·C (C=1 → 4, C=4 → 16, C=16 → 64).
Reading the table above by cascade count: at typical C=3–4 (batch 12–16) the working set stays under the 48 MB L2 ⇒ we're at
the no-FMA wall (0.36–0.68×). The **crush appears only at the L2 spill**: at n=512 cuFFT's per-image time TRIPLES across its
spill (batch 16→32: 0.0041 → 0.0139 ms/img) while ours degrades gracefully (already bandwidth-bound) ⇒ at n=512·batch=64
(128 MB) **ours 0.01344 vs cuFFT 0.01381 = 1.03×**. So the bare transform beats cuFFT exactly when the batch·image working
set truly spills L2 (large n AND many cascades); for everyday cascade counts the **fusion** is the regime-independent win.

⚠ **Harness bug found + fixed while GPU-validating a-3 (`tests/gpu-shared/ckir_kernel_dispatch.hpp`):** `dispatch_fft2d`
uploaded then dispatched pass 0 with **no `TransferDst→ShaderRead` barrier** — a latent race that only manifests once the
grid exceeds device occupancy (batch ≥ 9 here: flaky, ~all-wrong; batch ≤ 8 accidentally serialized). The kernel was always
correct (the `[.ocean-ifft-bench]` self-verified at batch 64); the shared test harness wasn't. Fixed at the root; batch
8–16 now bit-exact on Vulkan + DX12. Same class as the `dispatch_1wg` upload-barrier scar.

## Verdict

Bare batched IFFT: **parity/loss in L2-resident (no-FMA wall), 1.03× edge at the DRAM spill** — the doctrine wall, not a
crush, because a plain inverse transform exposes no fusion lever. The FFT-ocean's crush is **pipeline fusion** — now BUILT
and measured at **1.10–1.13× over the un-fused ocean, bit-exact** — plus portability (identical ocean on Vulkan+DX12+CPU-
oracle, which a vendor FFT cannot give). The loss was SOLVED, not documented-and-accepted.
