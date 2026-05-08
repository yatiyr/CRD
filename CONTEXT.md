# Cerid — Live Context

> Engine'in kısa-vadeli hafızası — "Şu an neredeyiz?" sorusuna cevap verir.
> "Master plan ne?" sorusunun cevabı `docs/ROADMAP.md` ve oradan dallanan dosyalardadır.
>
> Bu dosya bir DASHBOARD'dur, append-only changelog değildir. Her milestone session log'a yazılır
> (`docs/sessions/YYYY-MM-DD-*.md`); buradaki "Last shipped milestone" sadece o session log'a link verir.
> Session log'ların listesi de `docs/sessions/` klasörünün kendisindedir, burada değil.

---

## Current focus

**Phase 3.0 — Scene/ECS foundation.** v1a–v1m all shipped 2026-05-06 / 07 / 08. Phase expanded from 14 → **17 slices** on 2026-05-08 to land the elite-tier authoring substrate (Öbek + Preset + Profile) before Phase 3.0 closes (ADR-0058 Öbek ✅ realised, ADR-0059 Preset / ADR-0060 Profile pending v1n).

3 slices remaining: **v1n (Preset + Profile system)** ← next → v1o (sandbox renderer integration with the full Öbek + Preset + Profile stack) → v1p (reserved-slot + API surface freeze, closes Phase 3.0).

The architecture is the **8-layer slot-shaped ECS** designed for million-entity scenes, agents-as-components-with-scripts, UI on the same machinery (game and editor), with every novel ECS extension as a registered slot:

```
L8 Reflection / Editor          (Phase 7)        — API reserved
L7 Scripting & Behaviors        (Phase 4.0+)     — API reserved
L6 Replication / Networking     (Phase 4.2)      — API reserved
L5 Indexes                      (Phase 3.0+)
   ChangeDetect, AsyncAware                       — live in 3.0
   History, SpatialBVH, GpuResident               — API only in 3.0
L4 Query · System · Schedule    (Phase 3.0)     — live
L3 Relations                    (Phase 3.0)     — live (6 built-ins)
L2 Storage backends             (Phase 3.0)     — live (Archetype + SparseSet hybrid)
L1 Entity / SlotMap             (Phase 3.0)     — live
L0 Memory · Containers · Jobs   (already shipped)
```

Cerid signature: a uniform `IComponentIndex` extension framework where every novel ECS extension (history, change detect, spatial, GPU-mirror, async, replication, scripts, reflection) is a registered slot consuming the same component-lifecycle event stream. Adding the next extension is a one-day job.

Active phase doc: `docs/phases/phase-3.0-scene-ecs.md`.

## Coming up next — v1n / v1o / v1p

**v1n — Preset + Profile system (~3–4 days, ~800 LOC, ~20 tests).** ADR-0059 Preset substrate (`PresetResource` + per-type `PresetLoader` + `PresetRegistry` closed by C++ types; `register_type<T>` registers FourCC + schema + reader + apply dispatch; five-layer resolution stack: default → extends → preset → instance → runtime; `extends` chain shares the Öbek resolver). First concrete preset types ship: `QualityPreset` (FourCC `'PRQL'`) wired into `IRenderPath::apply(QualityPreset)` and `CameraPreset` (FourCC `'PRCM'`) wired into `Camera::apply(CameraPreset)`. ADR-0060 Profile substrate (`ProfileResolver` with closed typed predicates: `os` / `gpu_tier` / `domain` / `mode` / `target_fps` / `cpu_cores`; **additive composition** — Cerid-unique vs Unreal first-match-wins; priority-sorted stack composes all matching profiles cleanly). Hot-reload + atomic swap.

**v1o — Sandbox renderer integration with Öbek + Preset + Profile (~2–3 days, ~400 LOC).** Visual proof of the full authoring stack. `SandboxLayer` registers `IRenderPath` + `Camera` as `IPresetTarget` impls; app boot resolves `ProfileContext` and applies bundle; sandbox loads `assets/sources/sandbox.scene.toml` referencing 2–3 demo öbeks. ImGui adds: profile picker, quality slider (Low / Medium / High / Ultra runtime override at L4), "override window" with revert buttons at field/component/entity/all granularities, "Unpack öbek" button. Ships demo `assets/profiles/default.profile.toml` with cross-domain baselines.

**v1p — Reserved-slot + API surface freeze (~1 day).** Phase 3.0 closer. Confirms reserved traits (`History`, `SpatialBVH`, `GpuResident`, `Replication`, `Reflection`, `ScriptComponent`) accepted by `register_component`; reserved DSL operators round-trip; **Öbek + Preset + Profile API surfaces formally frozen** — Phase 3.5+ consumer phases implement against them but cannot change them.

## Active detour

_none — D-001 closed 2026-05-07; main roadmap resumed at v1d, subsequently shipped through v1m on 2026-05-08._

> When a detour opens, this section names it (e.g. "D-001: investigate shader-cache corruption") and the main roadmap pauses until it closes. Detour file: `docs/detours/D-NNN-<slug>.md`. Queue rules: `docs/detours/README.md`.

## Last shipped milestone

**2026-05-08 — Phase 3.0 v1m FULLY DELIVERED.** Twelve sub-slices, ~2700 LOC, 58 öbek tests across the v1m series. All five published v1m sub-slices closed: substrate (v1m1), override patches + OCHN (v1m2), full ObekCooker (v1m3a–d), InheritPolicy + DontInherit (v1m4), Inherit transparent CoW backend (v1m4b1–3), revert/unpack/enumerate + AAAA reservations (v1m5a–b). Hot-reload watcher + `obekc extract` CLI deferred to post-Phase-3.0 follow-up (task #108).

Closing session log: `docs/sessions/2026-05-08-scene-v1m5-revert-batch.md` (links the v1m1..v1m5 chain inline). Earlier in-phase milestones: `docs/sessions/2026-05-06-scene-v1a-slotmap.md` … `docs/sessions/2026-05-08-scene-v1l-cooker.md`.

## Test counts (post-v1m close, 2026-05-08)

- win-debug:          814/814
- win-relwithdebinfo: 814/814
- win-release:        811/811 (clean rebuild required after `ObekInstantiation` field addition)
- win-asan:           814/814
- win-clang-cl:       814/814
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config. (Release count is 3 fewer than debug: debug-only `FiberState` tests excluded by `#if CRD_ENABLE_ASSERTS`.)

## Open questions

- `crd-config` hot-reload remains 1.6b unless ImGui integration proves it should move earlier.

## Pointers (lazy-load reference)

Agents: don't read everything. Use these breadcrumbs.

- **Hub:** `docs/ROADMAP.md` (small navigation page; safe to read fully)
- **Principles:** `docs/PRINCIPLES.md` (read every session, short)
- **Active phase only:** `docs/phases/phase-3.0-scene-ecs.md`
- **Other phases:** `docs/phases/phase-<X>.md` (read ONLY when relevant)
- **Specific decision:** `docs/decisions/<NNNN>-<slug>.md` (find via `docs/decisions/README.md` tag index)
- **Last session detail:** the single file linked in "Last shipped milestone" above, not the whole `docs/sessions/` folder
- **Module overview:** `docs/systems/<module>.md` (when working on that module)
- **Module deep-dive:** `docs/<module>/<MODULE>_FILE.md` (only when doing surgery)
- **Open debt:** `docs/debt.md`
- **Detour queue + rules:** `docs/detours/README.md`

When in doubt, ASK before reading large files.

## Session log (rolling, last 5)

- **2026-05-08** — Phase 3.0 v1m5 (revert/unpack/enumerate APIs + AAAA-tier batch reservations) — closes v1m. `docs/sessions/2026-05-08-scene-v1m5-revert-batch.md`.
- **2026-05-08** — Phase 3.0 v1m4b (InheritPolicy::Inherit transparent CoW backend; demonstrated 2× memory savings on 1000-trees-from-same-öbek pattern). `docs/sessions/2026-05-08-scene-v1m4b-cow-backend.md`.
- **2026-05-08** — Phase 3.0 v1m4 (InheritPolicy enum + DontInherit semantics). `docs/sessions/2026-05-08-scene-v1m4-inherit-policy.md`.
- **2026-05-08** — Phase 3.0 v1m3d (cook-time `overrides=[...]` → OOVR; closes v1m3). `docs/sessions/2026-05-08-scene-v1m3d-cook-time-overrides.md`.
- **2026-05-08** — Phase 3.0 v1m3c (nested öbek refs, recursive walk, ChildOf splice). `docs/sessions/2026-05-08-scene-v1m3c-nested-obek.md`.

> Older entries: `docs/sessions/` (one file per session, chronologically named).
