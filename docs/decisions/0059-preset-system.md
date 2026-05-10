# ADR-0059 — Preset System: typed system-config bags with five-layer resolution and `extends`-chain composability

**Status:** Accepted
**Date:** 2026-05-08
**Tags:** scene, resources, cooker, arch, renderer, audio, physics, input, config

---

## Context

The engine has system-level configuration scattered across renderer hard-coded defaults, `crd-config` TOML knobs, and ad-hoc per-system structs. As Cerid grows into Phases 4 (input + project boot), 5 (audio), 6 (physics), and the renderer accumulates per-target quality knobs (shadow res, MSAA, post-fx pipeline), this scattering becomes unmanageable: there is no uniform mechanism for *quality-tier presets*, *device profiles*, *project templates*, or *user-saved tuning bundles*.

Elite engines all converge on the same shape:

- **Unreal**: Scalability groups (Low/Medium/High/Epic/Cinematic) + DeviceProfiles → CVars.
- **Unity**: Preset assets + Preset Manager + Volume profiles + Quality Settings.
- **Godot**: Resource-based (no prototypal inheritance — community pain point).
- **Bitsquid**: Lua tables + table-merge inheritance.

Cerid generalises and beats all four: a **typed Resource preset with schema-bound apply callback, five-layer resolution stack, prototypal `extends` inheritance, profile-driven activation** (ADR-0060), and *one cooker pipeline shared with scenes (ADR-0055) and öbeks (ADR-0058)*.

This ADR is paired with ADR-0058 (Öbek) and ADR-0060 (Profile). Together they form the Phase 3.0 authoring substrate: Öbek = entity-graph templates; Preset = system-config bags; Profile = selector mapping runtime context → preset bundle.

---

## Decision

### 1. A Preset is a typed Resource with a schema-bound apply callback

```cpp
struct QualityPresetSchema
{
    static constexpr u32 fourcc  = make_fourcc('P','R','Q','L');
    static constexpr u32 version = 1;

    u32        shadow_resolution = 2048;
    u8         msaa_samples      = 4;
    u8         ssr_quality       = 2;
    u8         ssao_quality      = 2;
    u8         post_fx_count     = 0;
    PostFXRef  post_fx[8]        = {};
};

class IPresetTarget
{
public:
    virtual void apply(const QualityPreset&) {}
    virtual void apply(const CameraPreset&)  {}
    // overloads added per-type as new preset types ship
};
```

**No RTTI. No reflection. No string lookup at runtime.** `apply()` is a compile-time overload set; targets implement only the overloads they care about.

### 2. Five-layer resolution stack — generalises Unreal's scalability + Unity's Volume

```
Highest precedence
  L4 ── Runtime override   (CVar / ImGui slider / debug toggle) — never persists
  L3 ── Per-instance       (rare; e.g. one camera ignores quality preset) — persists in scene
  L2 ── Active preset      (resolved by Profile system; ADR-0060)
  L1 ── extends chain      (deepest extends wins per field)
  L0 ── Schema default     (compile-time)
Lowest precedence
```

Resolution happens at **apply time**, not query time. The applied target caches the resolved value until the next apply event. Means: zero per-frame resolution cost; debugging is trivial (inspect target's stored value).

### 3. Composable via `extends` — prototypal inheritance

```toml
# preset/quality_ultra.preset.toml
extends = "preset/quality_high.preset.toml"

# Override only what changes; everything else inherits from base.
shadow_resolution = 4096
ssr_quality       = 3
```

Same `extends` resolver as Öbek (shared cooker code per ADR-0058). Variant chains of variants of variants supported. Cycle-detected at cook time. Cooker emits a `PCHN` (preset chain) chunk listing every transitive dependency for hot-reload watching.

### 4. Per-type FourCC, registered at engine init

```cpp
preset_registry.register_type<QualityPreset>("Quality");      // FourCC 'PRQL'
preset_registry.register_type<CameraPreset>("Camera");        // FourCC 'PRCM'
// Phase 4+ additions live in their consumer modules:
preset_registry.register_type<AudioDevicePreset>("AudioDevice");  // 'PRAD'
preset_registry.register_type<PhysicsPreset>("Physics");          // 'PRPH'
preset_registry.register_type<InputMapPreset>("InputMap");        // 'PRIN'
preset_registry.register_type<ProjectTemplate>("Project");        // 'PRPJ'
```

The registry maps:
- TOML key (preset type name) → schema FourCC.
- TOML reader function (rides on the v1l SceneCooker's `ComponentTomlReaderFn` pattern).
- Field schema (default + range + units, for editor sliders + cook-time validation).
- Per-type apply callback dispatch to live `IPresetTarget` impls.

New preset types ship without core changes — same plug-point grammar as `IComponentIndex` (ADR-0053). The registry is closed by C++ types: no string-keyed runtime registration that could silently fail.

### 5. Hot-reload — atomic swap with last-good fallback

Same pattern as shader / material / öbek. Watcher on `*.preset.toml` files; cooker re-runs on change; new `PresetResource` payload swapped atomically into `ResourceManager`; `IPresetTarget` impls receive a re-apply event with the new payload. Failed cook keeps last-good (ADR-0048 shader pattern).

The `PCHN` chunk lets the watcher detect upstream changes — change a `quality_high.preset.toml`, all `extends`-derived ultra/cinematic presets re-cook and re-apply.

### 6. CRDR layout for cooked presets

Each preset type has its own FourCC. Three chunks per cooked preset:

```
type_fourcc = <per-type, e.g. 'PRQL' for QualityPreset>

PINF — schema_version + flags (extension bits reserved for future per-type metadata)
PDAT — flat payload bytes (variant chain pre-resolved; field ordering matches schema declaration)
PCHN — extends dependency list (canonical paths + content hashes; for hot-reload watcher)
```

CRDR sorts chunks by FourCC → deterministic byte order. Same source files + same registration order = bit-exact bytes.

### 7. Cross-cutting categories — shipped over time

| Type | FourCC | Phase | Consumer |
|---|---|---|---|
| `QualityPreset` | `'PRQL'` | 3.0 v1n | `IRenderPath` |
| `CameraPreset` | `'PRCM'` | 3.0 v1n | `Camera` |
| `PostFXPreset` | `'PRPP'` | 3.5+ | `IRenderPath` post-fx slot |
| `AudioDevicePreset` | `'PRAD'` | 5 | `crd-audio` |
| `PhysicsPreset` | `'PRPH'` | 3.1 | `crd-eylem` |
| `InputMapPreset` | `'PRIN'` | 4 | `crd-input` |
| `ProjectTemplate` | `'PRPJ'` | 4 | `crd-app` boot |

Each new type is ~150 LOC of registration + apply implementation; the substrate stays unchanged. The `CrdResourceLoader<PresetResource>` pattern handles all types uniformly through the registry.

### 8. Authoring schema — TOML

```toml
# preset/quality_ultra.preset.toml — full feature surface

extends = "preset/quality_high.preset.toml"   # variant chain (optional, depth unbounded)

[meta]
description = "Cinematic / RTX-class quality preset"
target_fps  = 60
target_gpu  = ">=high"

[preset]
shadow_resolution = 4096
msaa_samples      = 8
ssr_quality       = 3
ssao_quality      = 3

post_fx = [
    { type = "Bloom",         intensity = 0.6 },
    { type = "ChromaticAb",   intensity = 0.15 },
    { type = "FilmGrain",     intensity = 0.3 },
]
```

### 9. Reserved API surface — frozen at v1n

```cpp
namespace crd::preset
{
class IPresetTarget;
class PresetRegistry;                    // global, init at app start
class PresetResource;                    // typed payload; FourCC per concrete type
class PresetLoader;                      // ILoader, dispatched per FourCC

struct PresetApplyEvent;                 // emitted on hot-reload to live IPresetTargets

// Per-type registration grammar
template <typename T>
void PresetRegistry::register_type(StringView name);

// Boot-time resolution (driven by Profile, ADR-0060)
void apply_preset_bundle(ConstSpan<ResourceId> presets,
                         ConstSpan<IPresetTarget*> targets);
}
```

This API is frozen at v1n. Phase 4/5/6 consumer modules add new preset types; they do not change the substrate.

---

## Comparison with elite engines

| Capability | Unreal | Unity | Godot | **Cerid Preset** |
|---|---|---|---|---|
| Typed schema with compile-time validation | partial (CVar = string) | partial (RTTI) | ✗ | **✓ (FourCC + struct)** |
| `extends` chain (prototypal inheritance) | partial (groups composition) | ✗ (variant for components only) | ✗ (community pain) | **✓ (cycle-detected, depth-unbounded)** |
| Hot-reload graph-aware | ✗ | partial | partial | **✓ (PCHN watcher)** |
| Per-type apply callback (no string lookup) | ✗ (CVar dispatch is string-keyed) | partial | ✗ | **✓ (overload set)** |
| Five-layer resolution (default → extends → preset → instance → runtime) | partial | partial (Volume System) | ✗ | **✓ (uniform across all preset types)** |
| Single cooker pipeline shared with prefabs/scenes | ✗ | ✗ | ✗ | **✓ (rides ADR-0055/0058)** |
| Deterministic byte output | ✗ | ✗ | ✗ | **✓ (CRDR sort + canonical field order)** |

---

## Consequences

### Positive
- One uniform mechanism for every cross-cutting setting (quality, camera, audio, physics, input, project template).
- Type-safe; compile-time-validated; zero string lookup at runtime.
- Cooker-pipeline reuse — preset cooker is ~200 LOC sitting on the v1l/v1m substrate, not a separate system.
- Hot-reload comes free from the existing watcher pattern.
- Cross-domain ergonomics — same engine running games, sims, DAWs picks the right preset bundle (paired with ADR-0060 Profile).

### Negative / costs
- Each new preset type is per-type registration + apply implementation in its consumer module (~150 LOC). Acceptable cost — the work is local and uses existing patterns.
- Per-type CrdRespource specialisation explodes the loader registry slightly; mitigated by the shared `PresetLoader` dispatcher.

### Open questions / debt
- **Schema-version migration** — when a preset type bumps its version (e.g. `QualityPreset.version = 2` adds a field), need migration tables. Reserved as a v1n+1 follow-up; for v1n the pattern is "version field present, defaults backfilled on missing fields, no cross-version migration."
- **Per-instance overrides (L3)** — v1n ships L0/L1/L2/L4. Per-entity preset overrides (one camera ignores quality preset) reserved for Phase 4+ when a real consumer surfaces.
- **Preset blending** (interpolating two presets) — Unity's Volume System feature. Reserved for renderer-driven phases (e.g. interior/exterior camera blends).

---

## References

- ADR-0048 — Material system (two-tier Template/Instance pattern that Preset generalises)
- ADR-0053 — Component-index framework (registration grammar that Preset registry mirrors)
- ADR-0055 — Scene serialization (shared cooker substrate)
- ADR-0058 — Öbek system (shared `extends` resolver)
- ADR-0060 — Profile system (companion ADR; Profile drives Preset activation)
- v1l session log — `docs/sessions/2026-05-08-scene-v1l-cooker.md` (the cooker pattern Preset extends)
