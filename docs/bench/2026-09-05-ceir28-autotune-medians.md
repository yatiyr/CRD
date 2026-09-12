# CEIR-28 §80 autotuner — measured schedule medians (Vk RTX / DX12 RTX / Vk lavapipe)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

The §80 autotuner (`measure_tune_entry`) times the FULL 4-config `PlanOptions` space (fuse × share) on a real
device and emits the winner as a `tune.entry` cache row. This board records the measured medians that drove
the committed `assets/ceir/tune_cache.ceir` rows. ⛔ These are a **schedule-selection** board (which of 4
equivalent-output plans is fastest), NOT a peer-crush — there is no external peer; the "reference" is the
default schedule, and every config is BIT-EXACT to it (the free oracle-gate).

## Machine / config

- **GPU:** NVIDIA GeForce RTX 4070 Ti SUPER (real device) + llvmpipe (LLVM 20.1.2, 256 bits) software Vulkan (WSL).
- **Windows:** win-debug preset, MSVC 14.50.35717; **Linux:** `~/cerid-build/linux-gcc-debug`, gcc `-Werror`, WSL2.
- **Harness:** `measure_tune_entry` in `tests/ceir-gpu-{vulkan,dx12}/test_ceir_pipeline_*.cpp` — **N=5 warmup + K=7 timed, MEDIAN** of `IComputeContext::last_gpu_ms()` (2 device timestamps bracketing the ONE `submit_and_wait` command buffer). Re-runnable via `scripts/run-ctest.bat <build> 28b-2a` / `28d` (add catch2 `-s` for the printed emit).
- **Payload:** the 26e-3b fp32 2-layer MLP (x[4,8]·W1[8,16] → relu → ·W2[16,2]), `program_hash = -4961225458425459157` (i64) / `13485518615284092459` (u64) — **identical on all three backends** (the hash is the fnv1a of the canonical print, platform-independent).

## Boards (medians in ms; the 4 configs are tt/tf/ft/ff = fuse×share)

| board | tt (fuse,share) | tf (fuse,¬share) | ft (¬fuse,share) | ff (¬fuse,¬share) | winner | fuse plan_sig | no-fuse plan_sig |
|---|---|---|---|---|---|---|---|
| **Vk RTX 4070 Ti SUPER** | 0.008096 | 0.008128 | 0.009184 | 0.009120 | `{fuse,share}` | `0x83e33779c4351c48` | `0x4e7803adf4d43172` |
| **DX12 RTX 4070 Ti SUPER** | 0.017344 | 0.017344 | 0.018432 | 0.018432 | `{fuse,share}` | `0x83e33779c4351c48` | `0x4e7803adf4d43172` |
| **Vk lavapipe (software)** | — (not a perf number) | | | | `{fuse,share}` | `0x83e33779c4351c48` | `0x4e7803adf4d43172` |

lavapipe: software rendering — its timings are not a performance signal, so no medians are recorded as such;
the lavapipe board's value is that **`median > 0`** (llvmpipe implements timestamp queries → the measurer's
hard gate does not falsely redden CI) and that its winner + `program_hash` + both `plan_sig`s are **identical**
to the RTX boards (the portability proof). Fuse still won there too.

## Findings

1. **Fusion wins on both real backends, and by the SAME absolute margin.** Vk: 0.009184 − 0.008096 = **1.088 µs** (~12%).
   DX12: 0.018432 − 0.017344 = **1.088 µs** (~6%). The percentage differs ONLY because the base differs; the
   absolute saving is ~1 µs — the one relu VizDispatch (3rd stage) the GemmRelu fold removes.
2. **The DX12 base is ~2× the Vk base on the same GPU/plan** (~17.3 vs ~8.1 µs) ⇒ a **~9 µs fixed readback floor**
   inside the DX12 `begin→submit` timestamp bracket: D3D12 forbids a UAV on a READBACK heap, so the portable
   readback is `copy(GpuOnly→GpuToCpu)` INSIDE the bracket, whereas Vulkan maps the GpuToCpu buffer directly.
   ⛔ **DX12 and Vk medians are NOT comparable except after subtracting this floor** (it cancels across configs,
   so the fuse/no-fuse COMPARISON within a backend is sound).
3. **`share` is inert on this MLP** (26f-3a: no disjoint-lifetime pair) — tt≡tf and ft≡ff produce byte-identical
   plans (same `plan_sig`), so their medians are within timer noise (Vk) or exactly equal (DX12's coarser timer).
   The winner is collapsed to the canonical plan-equivalence-class member (`{fuse,share}`) so a noise gap between
   plan-identical configs cannot flip the committed row (the anti-drift's stability guarantee).
4. **Cross-backend planner determinism (for this payload):** the two `plan_sig` values are BIT-IDENTICAL across
   Vk + DX12 + lavapipe **for the 4×8×16×2 MLP** (gemm/relu vocab). This is EVIDENCE — one payload is one point,
   not a general proof — that `plan_tensor_pipeline` carries no backend-dependent branch in stage-selection/aliasing,
   which is why `tune_cache.ceir` is ONE cross-backend artifact, not three. The general no-backend-branch claim is a
   CEIR-29+ census question if native graph providers introduce a backend-specific lowering.

## Verdict (quoted by the session log / phase table)

CEIR-28 §80 autotuner: **fusion is the measured winner on Vk + DX12 (real RTX 4070 Ti SUPER) by ~1 µs, and on
lavapipe**; every config BIT-EXACT to the default (free oracle-gate); `program_hash` + `plan_sig` identical
across all three backends for the MLP payload (portable program identity + evidence — one payload — of cross-backend
planner determinism). No losses — this is a selection board, and the selected schedule is correct on every backend.
