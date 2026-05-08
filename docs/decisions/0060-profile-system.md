# ADR-0060 — Profile System: typed predicate selectors with additive composition for cross-domain runtime configuration

**Status:** Accepted
**Date:** 2026-05-08
**Tags:** scene, resources, cooker, arch, config, networking, app

---

## Context

Cerid is multipurpose: the same engine binary can host games, robotic / aerospace simulations, DAWs, and offline cinematic pipelines. Each domain has a fundamentally different runtime profile:

- **Game** — real-time forward+ at 60 fps; gamepad input; ambient mixing.
- **Simulation** — fixed-step deterministic; potentially headless; replay-friendly.
- **DAW** — low-latency audio scheduler is highest priority; renderer minimal.
- **Cinematic** — maximum quality; longer frame budget acceptable; offline render farms possible.

Plus the standard hardware axes: Windows / Linux / macOS, GPU tier (low / mid / high / ultra), CPU cores, target fps. And the standard mode axes: editor / runtime / headless.

Without a unified selector, each consumer module hardcodes detection logic and per-tier branches; this is the path Unreal already trod and ended up with `BaseDeviceProfiles.ini` carrying thousands of lines of CVar overrides per device.

ADR-0059 ships the Preset substrate. This ADR ships the **Profile** — a selector that maps `(os, gpu_tier, domain, mode, target_fps, cpu_cores, …)` → an ordered bundle of presets to activate. Together, ADRs 0058/0059/0060 form Phase 3.0's authoring substrate.

---

## Decision

### 1. Profile = match-rule + ordered preset bundle

```toml
# assets/profiles/default.profile.toml — full feature surface

[profile.windows_high_end_game]
priority = 100
match    = { os = "windows", gpu_tier = ">=high", domain = "game" }
apply    = ["preset/quality_ultra.preset.toml",
            "preset/camera_default.preset.toml",
            "preset/input_keyboard_mouse.preset.toml"]

[profile.simulation_headless]
priority = 50
match    = { domain = "simulation", mode = "headless" }
apply    = ["preset/quality_minimal.preset.toml",
            "preset/physics_deterministic.preset.toml"]

[profile.daw_realtime]
priority = 50
match    = { domain = "daw" }
apply    = ["preset/audio_lowlatency.preset.toml",
            "preset/quality_minimal_renderer.preset.toml"]

[profile.cinematic_offline]
priority = 200
match    = { domain = "cinematic" }
apply    = ["preset/quality_ultra.preset.toml",
            "preset/camera_cinematic.preset.toml",
            "preset/post_fx_film.preset.toml"]
```

A profile's `match` block is a typed predicate set; `apply` is an ordered list of preset references; `priority` orders profiles in the activation stack.

### 2. Predicate schema — closed and typed for v1n; extensible by registration

| Predicate | Type | Operators | Source |
|---|---|---|---|
| `os` | `enum {windows, linux, macos}` | `==`, `in [...]` | platform detection |
| `gpu_tier` | `enum {low, mid, high, ultra}` | `==`, `>=`, `<=`, `in [...]` | RHI capability detection |
| `domain` | `enum {game, simulation, daw, cinematic}` | `==`, `in [...]` | app init |
| `mode` | `enum {editor, runtime, headless}` | `==`, `in [...]` | app init |
| `target_fps` | `i32` | `==`, `>=`, `<=` | config / runtime |
| `cpu_cores` | `i32` | `==`, `>=`, `<=` | `std::thread::hardware_concurrency` |

Closed schema for v1n. **Better than Unreal's DeviceProfile**, which uses untyped INI key/value — Cerid's predicates are typed, validated at cook time, no runtime parse cost.

Extension hook reserved (`profile_registry.register_predicate<T>(...)`) for Phase 4 when domain-specific predicates appear (e.g. `network_role = "host" | "client"`).

### 3. Additive profile composition — Cerid-unique vs Unreal first-match-wins

Unreal's DeviceProfile picks ONE profile and stops. Cerid's resolver:

```
1. Match all profiles where every predicate evaluates true against the runtime context.
2. Sort matched profiles by priority ascending.
3. Apply preset bundles in order — later profiles override earlier.
4. Same field overridden by two profiles → highest-priority wins (deterministic).
```

Why additive: a "windows" profile + "high-end" profile + "game" profile compose cleanly. With single-pick (Unreal), you end up duplicating `windows_high_end_game`, `linux_high_end_game`, `windows_mid_game`, etc. Cerid lets you author them factored:

```toml
[profile.platform_windows]
priority = 10
match    = { os = "windows" }
apply    = ["preset/input_keyboard_mouse.preset.toml"]

[profile.tier_high]
priority = 20
match    = { gpu_tier = ">=high" }
apply    = ["preset/quality_high.preset.toml"]

[profile.domain_game]
priority = 30
match    = { domain = "game" }
apply    = ["preset/camera_game.preset.toml"]
```

Run on Windows + high-end + game → all three match → applied in priority order → resulting bundle composes.

### 4. Runtime context resolution

```cpp
ProfileContext ctx;
ctx.os         = detect_os();
ctx.gpu_tier   = detect_gpu_tier();             // queries RHI capabilities
ctx.domain     = ProjectDomain::Game;            // declared at app init
ctx.mode       = AppMode::Runtime;
ctx.target_fps = config.get<i32>("frame.target_fps", 60);
ctx.cpu_cores  = static_cast<i32>(std::thread::hardware_concurrency());

ConstSpan<ResourceId> bundle = profile_resolver.resolve(ctx);
preset_applicator.apply_bundle(bundle);
```

Context is built at app init; can be re-resolved on signal (e.g. user toggles `domain = "cinematic"` in editor → profile re-evaluates → presets re-apply). Reactivity is signal-driven, not polled.

### 5. CRDR layout for cooked profiles

```
type_fourcc = 'PROF'

FINF — schema_version + rule_count + bundle_count + flags
FRLE — per-rule: { priority, predicate_count, predicate_records[] }
       predicate_records contain: { field_id, operator, typed_value }
FBND — per-bundle: { rule_idx, preset_id_count, preset_ids[] }
```

CRDR sorts chunks by FourCC → deterministic byte order. Same source files + same registration = bit-exact.

### 6. Hot-reload — change the profile, presets reapply atomically

Same watcher + atomic-swap pattern as ADR-0058/0059. Profile re-cook → re-resolve against current context → apply diff (presets dropped + presets added + presets re-applied with new payloads). `IPresetTarget` impls receive the unified re-apply event.

### 7. Determinism — match order is canonical

Same context → same matched profiles in same order → same applied bundle. Predicate evaluation order is fixed by FRLE chunk byte order; tie-breaking on equal priority falls back to FRLE record order. No floating-point in predicates; all enum / integer comparisons.

### 8. Authoring schema — TOML

Already shown in §1. Notable conventions:

- `priority` is mandatory; sets activation order.
- `match` is mandatory; empty match (`match = {}`) means "always-on baseline profile."
- `apply` is mandatory; empty apply is allowed (a profile that contributes only its match-side effect — useful for activating other systems via profile-tag observation).
- `meta.description` optional; for editor display.

### 9. Reserved API surface — frozen at v1n

```cpp
namespace crd::profile
{
struct ProfileContext
{
    OperatingSystem os;
    GpuTier         gpu_tier;
    ProjectDomain   domain;
    AppMode         mode;
    i32             target_fps;
    i32             cpu_cores;
};

class ProfileResolver
{
public:
    [[nodiscard]] ConstSpan<ResourceId> resolve(const ProfileContext&) const;
};

class ProfileResource;     // typed Resource payload (FourCC 'PROF')
class ProfileLoader;       // ILoader for FourCC 'PROF'

void apply_profile_bundle(const ProfileResolver&,
                          const ProfileContext&,
                          ConstSpan<crd::preset::IPresetTarget*> targets);
}
```

This API is frozen at v1n. Phase 4 (input + project template) and beyond add domain-specific predicates through the registration hook; they do not change the resolver.

---

## Comparison with elite engines

| Capability | Unreal DeviceProfile | Unity Quality Settings | Godot | **Cerid Profile** |
|---|---|---|---|---|
| Typed predicates | ✗ (CVar = string) | partial | ✗ | **✓ (closed schema, cook-validated)** |
| Additive composition | ✗ (first-match-wins) | partial | ✗ | **✓ (priority-sorted stack)** |
| Cross-domain (game / sim / DAW / cinematic) | ✗ (game-only) | ✗ (game-only) | ✗ | **✓ (domain predicate is first-class)** |
| Hot-reload | ✗ | partial | ✗ | **✓ (atomic swap + diff apply)** |
| Deterministic resolution | ✗ | ✗ | ✗ | **✓ (canonical predicate order)** |
| Single cooker pipeline shared with presets/öbeks/scenes | ✗ | ✗ | ✗ | **✓ (rides ADR-0058/0059)** |

---

## Consequences

### Positive
- One uniform selector mechanism across game / sim / DAW / cinematic — Cerid's USP.
- Authored profiles factor cleanly (platform / tier / domain) instead of cross-product duplication.
- Type-safe predicates; cook-time validation catches typos before runtime.
- Hot-reload picks up profile changes mid-session.

### Negative / costs
- Boot-sequence dependency: profile resolver must run before any consumer module reads its applied preset. Documented as: profiles resolved at app init right after `Preset::Registry` setup, before module-specific init.
- Predicate schema is closed for v1n; extension via registration adds boilerplate (acceptable cost for type safety).

### Open questions / debt
- **Per-domain default profile shipping** — engine should ship `default.profile.toml` with sensible game / sim / DAW / cinematic baselines? Reserved for v1o (sandbox) — user-facing demo-quality profile lives there.
- **Predicate negation** (`os != "macos"`) — not supported in v1n; rule sets compose around it via positive matching. Reserved if a real use case appears.
- **Reactive context updates** — v1n re-resolves on explicit signal. Per-frame polling for `gpu_tier` change (driver swap mid-session?) is not currently in scope.

---

## References

- ADR-0048 — Material system (registration grammar precedent)
- ADR-0053 — Component-index framework (registration hook pattern Profile mirrors)
- ADR-0055 — Scene serialization (shared cooker substrate)
- ADR-0058 — Öbek system (shared `extends` resolver pattern)
- ADR-0059 — Preset system (companion ADR; Profile drives Preset activation)
- Unreal scalability and DeviceProfile docs (the system Cerid generalises and improves on)
