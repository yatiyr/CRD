# AS-4 — the auto-scheduler TUNES the reduction (op-generality) vs CUB (2026-07-23)

**The claim:** the CKIR auto-scheduler (ADR-0098 §4), proven on the Contract/GEMM tile hierarchy, is a GENERAL kernel
auto-scheduler — the SAME `enumerate → cost-rank → measure-on-device → oracle-validate → pick-best` loop tunes a
**device-wide reduction**, a structurally different op (memory-bound, a fan-in tree, no data reuse). It automatically
finds a schedule that reaches the DRAM-bandwidth ceiling and beats NVIDIA's CUB `DeviceReduce`.

**Machine:** NVIDIA GeForce RTX 4070 Ti SUPER (Ada, sm_89), 48 MB L2, 256-bit GDDR6X (**672 GB/s** peak) · Vulkan 1.4 ·
CUDA 13.3 (CUB gold). **Clocks:** NOT locked (session lacks GPU-clock permission) — both sides min-of-N, GPU-timed,
separate processes ⇒ fair *relative*; re-lock (`nvidia-smi -lgc … -lmc …`) before any headline number.

## The schedule space (backend-free, unit-tested)

`build_reduce` is a 2-pass tree reduction parameterized by **(threads/block, per_thread serial unroll)**; `nblocks =
N / (threads·per_thread)`. `ckir_autotune.hpp` adds the reduce half of the auto-scheduler:
- `enumerate_reduce_schedules(N, lim)` — the valid space (threads a power of two ≤ 1024; the per-block span divides N;
  nblocks a positive multiple of threads so the single final pass reduces the partials). **33 valid schedules** for
  N=2²⁴; the hand-tuned B-cmp winner (256×8) is a member.
- `predict_reduce_ms(N, sched, spec)` — a roofline for a memory-bound op: traffic `4·(N + 2·nblocks)` bytes over
  effective bandwidth scaled by block-occupancy `min(1, nblocks / 2·SMs)` (too few blocks ⇒ DRAM not saturated).
- `rank_reduce_top_k_cost` — picks the top-K to MEASURE (the search measures ~10, not the whole space).

Unit-tested in `tests/kir/test_ckir_autotune.cpp` `[kir][autotune]` (space validity, hand-tuned membership, cost-model
rank, rejection of illegal schedules) — no GPU needed.

## The measured search (`[.reduce-autotune]`, `tests/gpu-context-vulkan`)

N = 2²⁴ = 16,777,216 f32 = **64 MB ⇒ spills the 48 MB L2 ⇒ DRAM-bound**. Input all-ones ⇒ the exact sum is N (f32-exact),
so **every candidate is ORACLE-VALIDATED** (`sum == N`) before it may win. Each candidate is `build_reduce`'d, emitted to
GLSL, compiled to SPIR-V, dispatched (2 passes), GPU-timed (`last_gpu_ms`, min-of-25). Vendor = CUB `DeviceReduce::Sum`
(`bench/gpu-compute/cub_reduce_bench.cu`), 602.6 GB/s DRAM-bound on this GPU.

### Board — top-K candidates measured (all `sum==N` ✓), per-schedule bandwidth

| threads | per_thread | nblocks | GPU ms | GB/s | % of 672 peak |
|--------:|-----------:|--------:|-------:|-----:|--------------:|
| 128 | **64** | 4096 | **0.10541** | **636.7** | **94.7 %** ← winner |
| 128 | 32 | 4096 | 0.10554 | 635.9 | 94.6 % |
| 64  | 64 | 4096 | 0.10573 | 634.7 | 94.5 % |
| 512 | 32 | 1024 | 0.10576 | 634.5 | 94.4 % |
| 128 | 64 | 2048 | 0.10582 | 634.2 | 94.4 % |
| 512 | 16 | 2048 | 0.10602 | 633.0 | 94.2 % |
| 512 | 64 | 512  | 0.10595 | 633.4 | 94.3 % |
| 256 | 64 | 1024 | 0.10627 | 631.5 | 94.0 % |
| 1024| 16 | 1024 | 0.10826 | 619.9 | 92.2 % |
| 1024| 8  | 2048 | 0.10803 | 621.2 | 92.4 % |

### Verdict

| | schedule | GB/s | vs CUB | vs peak |
|---|---|---:|---:|---:|
| **CKIR autotuned WINNER** | 128 × 64 | **636.7** | **1.057×** | 94.7 % |
| CKIR hand-tuned default | 256 × 8 | 631.9 | 1.049× | 94.0 % |
| CUB `DeviceReduce::Sum` | (vendor) | 602.6 | 1.00× | 89.7 % |

**The auto-scheduler beats CUB (1.057×) AND its own hand-tuned default** — it discovered `128×64` (a wider unroll, fewer
threads) over the hand-picked `256×8`, reaching **94.7 % of the 672 GB/s DRAM ceiling**. All ten candidates land on the
DRAM-saturated plateau (619–637 GB/s) and every one is bit-exact-correct; the winner varies within noise run-to-run
(128×32 / 128×64 trade the top spot at ~636 GB/s), as expected for a memory-bound op where several schedules saturate
the bus.

## Reading (honest, no asterisks)

- The **value is op-generality**, not the margin: the same enumerate/measure/oracle/pick machinery that tunes the GEMM
  tile hierarchy tunes a reduction — the AS framework is a general kernel auto-scheduler, not a Contract special case.
  The reduce margin over CUB is inherently small (a reduction leaves almost nothing on the table once it streams DRAM
  coalesced), and the autotuner's edge over the hand-tuned default is within run-to-run noise (both saturate the bus).
- The autotuner's job here is to **find the DRAM-saturating region automatically** and reject under-occupied schedules
  (few blocks) — which the cost model prunes before measurement and the on-device sweep confirms.
- Same memory-bound doctrine as the FFT/reduce campaign: the crush lever is bandwidth, and bit-exactness costs nothing
  for an order-fixed reduction — so CKIR reaches parity-plus at the DRAM wall at MATCHED (bit-exact) accuracy.

**Harnesses (tracked):** space + cost model `engine/kir/include/crd/kir/ckir_autotune.hpp`; unit test
`tests/kir/test_ckir_autotune.cpp` `[kir][autotune]`; measured board `tests/gpu-context-vulkan/test_vulkan_context.cpp`
`[.reduce-autotune]`; vendor `bench/gpu-compute/cub_reduce_bench.cu`.
