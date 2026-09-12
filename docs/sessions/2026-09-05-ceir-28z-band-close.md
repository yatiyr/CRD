# 2026-09-05 — CEIR-28 band close (the §80 selection policy: autotune + config cache + deterministic locked mode)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

CEIR-28 is the SELECTION POLICY over CEIR-27's mechanism (§80 autotuning + §81 PGO + §82 cost model). Proof
(roadmap): a "target-specific configuration cache" + a "deterministic locked configuration mode". It re-instantiates
the v17 kir-autotune loop (enumerate → cost-rank → measure → oracle-gate → checked-in DB → replay; "tune offline,
replay forever, never at runtime") over the CEIR schedule space, made PORTABLE (Vk + DX12 + lavapipe) and AUTHORABLE
(the cache is a `.ceir` asset). Full per-slice detail: `docs/detours/D-007-ceir-tracker.md`.

## Slices (row per slice)

| slice | what | gate |
|---|---|---|
| 28-0 | band-open census: v17 is the reference FLOOR (not wirable — CUDA-only measurer); slice-0 space = the 4 `PlanOptions` configs; cost model VACUOUS at 4 (measure all) | advisor-verified facts |
| 28a | `ceir.tune` dialect (`tune.entry` = one cache ROW, 6 required attrs) + module-wide `find_tune_misuse` (DuplicateKey) + `plan_options_from_tune_cache` full-key loader | device-free #834 |
| 28b-1 | `program_hash` = fnv1a-64 of the canonical print of the POST-expansion payload (u64) | device-free (pre/post-expand discriminator) |
| 28b-2a | the portable Vk MEASURER: 4-config median of `last_gpu_ms` + free oracle-gate + plan-sig winner-collapse + emit + replay | RTX #4775 (69 assns) + lavapipe |
| 28b-2b | the committed device-keyed cache + anti-drift (device-gated) | RTX #4776 + lavapipe |
| 28c | DETERMINISTIC LOCKED MODE: `plan_tensor_pipeline_cached` + `TunePolicy{Fallback,Locked}` + typed `TuneCacheLockedMiss` | device-free #839 (MSVC+gcc) |
| 28d | the DX12 measurer mirror (migrated=both-backends); cross-backend planner determinism | DX12 #4873 (78 assns) |
| 28z-1 | warmup unify (Vk 3→5); `Require` DEFERRED (no-speculative) | 28b-2a/2b @ 5 warmups |
| 28z-2 | the 3-row cross-backend `tune_cache.ceir` + per-row field anti-drift, one test per backend TU | Vk #4776 + DX12 #4874 + lavapipe (92 assns) |
| 28z-3 | close: bench→docs, cross-config, deferral ledger, 2 scars, flip ✅ | this record |

## Cross-config boards (row per config)

| config | result |
|---|---|
| win-debug (MSVC 14.50) | 7/7 CEIR-28 tests (28a/28b-1/28c device-free + 28b-2a/28b-2b/28d/28z-2 device) |
| RTX 4070 Ti SUPER · Vulkan | medians tt/tf/ft/ff = 0.008096/0.008128/0.009184/0.009120 ms; winner {fuse,share}; fuse ~12% |
| RTX 4070 Ti SUPER · DX12 | medians 0.017344/0.017344/0.018432/0.018432 ms; winner {fuse,share}; fuse ~6%; ~9µs readback floor |
| WSL lavapipe (LLVM 20.1.2) · Vulkan | `[tune]` 169 assns; `median>0`; winner + program_hash + plan_sig identical to RTX |
| WSL linux-gcc-debug (gcc `-Werror`) | device-free `[tune]` 44 assns clean |

Benchmark board: `docs/bench/2026-09-05-ceir28-autotune-medians.md`. Findings: fusion saves ~1.088 µs on BOTH real
backends (same absolute margin); DX12 base ~2× Vk = a ~9 µs readback-copy floor inside its timestamp bracket;
`share` inert; `program_hash` + both `plan_sig`s identical across all 3 backends **for the MLP payload** (portable
identity + EVIDENCE — one payload, not a general proof — that stage-selection/aliasing has no backend branch ⇒ ONE
cross-backend cache artifact; the general no-backend-branch claim is a CEIR-29+ census question if native providers add one).

## Deferral ledger (each with a trigger — no speculative build)

- **PGO** (§81 runtime feedback loop) → a CEIR program with a runtime hot loop.
- **roofline cost model** (§82) → a config space too large to exhaust (4 configs = measure all; cost model vacuous).
- **env driver/compiler-version split** (§80 lists them; `env` bundles them) → a driver update flips the winner on the same device.
- **`TunePolicy::Require`** → a caller that must distinguish 'cache MISSING' from 'cache present but planner IGNORED it'.
- **WARP cache row** → a CI runner without a GPU runs the DX12 suite (WARP = D3D12's llvmpipe; its key is correct when it appears).
- **readback-floor subtraction** → a consumer that compares medians across backends.
- **the "device with no row" else-branch** → a 4th device (unexercised on the RTX/lavapipe boxes; kept, not faked).

## Scars (2 new memory files)

- `feedback_autotuner_winner_collapse_to_plan_equivalence_class` — a measurer over plan-INERT knobs must collapse the
  winner to its plan-class, else it emits timing noise into the committed row (anti-drift fires on jitter).
- `feedback_committed_asset_antidrift_through_the_printer_not_bytes` — compare through the canonical printer (single-row)
  or the loader's fields (multi-row), never file-bytes vs a fresh emit.

## Uncommitted batch (the user commits)

- `engine/ceir/ops/tune.ceirop.toml` + `engine/ceir/generated/crd/ceir/gen/tune_ops.{cpp,hpp,json,md}` + `tests/ceir/generated/test_tune_gen_smoke.cpp`
- `engine/ceir/include/crd/ceir/tune.hpp`
- `engine/ceir-gpu/include/crd/ceir/gpu/tensor_pipeline.hpp` + `engine/ceir-gpu/src/tensor_pipeline.cpp`
- `assets/ceir/tune_cache.ceir` (⚠ the intermediate single-row `tune_cache_vk_rtx4070ti_super.ceir` was NEVER committed — nothing to delete; do NOT add the stale name)
- `tests/ceir-gpu/test_tensor_pipeline.cpp` + `tests/ceir-gpu-vulkan/test_ceir_pipeline_vulkan.cpp` + `tests/ceir-gpu-dx12/test_ceir_pipeline_dx12.cpp`
- `docs/bench/2026-09-05-ceir28-autotune-medians.md` (+ README index) + `docs/detours/D-007-ceir-tracker.md` + `context.md` + this log + the 2 memory files

## Proposed commit message (Conventional Commits — NO AI co-author trailer, per CLAUDE.md/AGENTS.md)

```
feat(ceir): CEIR-28 — autotune + target-specific config cache + deterministic locked mode

The §80 selection policy over CEIR-27's mechanism: a ceir.tune dialect whose
tune.entry rows ARE a portable, authored config cache; a program_hash key (fnv1a
of the canonical print); a portable GPU measurer (Vk + DX12) that median-times the
4 PlanOptions configs, oracle-gates each BIT-EXACT to default, and emits the winner
(collapsed to its plan-equivalence class); a committed 3-row cross-backend
tune_cache.ceir with per-row device-gated anti-drift; and plan_tensor_pipeline_cached
with TunePolicy{Fallback,Locked} (a locked miss is a typed PlanReject — ships nothing
unmeasured). Fusion is the measured winner on Vk + DX12 by ~1µs; program_hash and
plan_sig are identical across Vk + DX12 + lavapipe (portable identity + cross-backend
planner determinism for the MLP payload). The intermediate single-row
tune_cache_vk_rtx4070ti_super.ceir was never committed (superseded before commit by
the 3-row cache), so this adds tune_cache.ceir rather than renaming.
Bench: docs/bench/2026-09-05-ceir28-autotune-medians.md.
```
