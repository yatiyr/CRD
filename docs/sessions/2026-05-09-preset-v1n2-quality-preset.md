# 2026-05-09 — Phase 3.0 v1n2: `QualityPreset` first concrete preset type

**Status at start:** v1n1 just shipped — `crd-preset` module substrate (`PresetResource` / `PresetLoader` / `PresetRegistry` / `PresetArtifactBuilder` / empty `IPresetTarget`) at 819/819 across both platforms. Working tree on the v1n1 commit.

**Status at end:** v1n2 shipped — `QualityPreset` schema struct (FourCC `'PRQL'`, version 1) lands as the first concrete preset type and IPresetTarget gets its `apply(QualityPreset)` overload. **win-debug 823/823, linux-gcc-debug 823/823 (+4 cases, +38 assertions over the 819/819 baseline).**

---

## What shipped

### Schema struct — `engine/preset/include/crd/preset/quality_preset.hpp` (new)

```cpp
struct alignas(8) QualityPreset
{
    static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P', 'R', 'Q', 'L');
    static constexpr crd::u32 version = 1U;

    crd::u32                    shadow_resolution = 2048U;
    crd::u8                     msaa_samples      = 4U;
    crd::u8                     ssr_quality       = 2U;
    crd::u8                     ssao_quality      = 2U;
    crd::u8                     post_fx_count     = 0U;
    crd::u8                     _reserved[8]      = {};
    crd::resources::ResourceId  post_fx[8]        = {};
};
static_assert(sizeof(QualityPreset)  == 144);
static_assert(alignof(QualityPreset) == 8);
```

**Layout pinned at version=1:** 4 (shadow) + 4 (`u8 ×4`) + 8 (reserved) + 128 (`ResourceId ×8`) = 144 bytes. The static_assert is the contract — any layout change is a schema-version bump, and the loader's payload-size check converts a mismatch into `LoadState::Failed` rather than silent corruption (no migration tables in v1n; that's a v1n+1 follow-up per ADR-0059 §"Open questions").

### `IPresetTarget::apply(const QualityPreset&)` — overload landed on the base

```cpp
class IPresetTarget
{
public:
    virtual ~IPresetTarget() = default;
    // ... rule-of-five deletions ...

    virtual void apply(const QualityPreset& /*preset*/) {}    // ← NEW

protected:
    IPresetTarget() noexcept = default;
};
```

The default body is intentionally empty — targets opt in by overriding. v1n3 will append `apply(const CameraPreset&)` next to it; future Phase 4/5/6 modules append their consumer-specific types in the same fashion. No string lookup at runtime, no virtual-dispatch surprises beyond the standard overload set.

### Tests — `tests/preset/test_quality_preset.cpp` (new, 4 cases / 38 assertions)

1. **Schema defaults + identity** — fourcc / version / sizeof / alignof match ADR-0059 §1; documented field defaults (2048 / 4 / 2 / 2 / 0 / null-array) verified.
2. **Registry registers `QualityPreset`** — `register_type<QualityPreset>("Quality")` mints a `PresetTypeInfo` with the canonical FourCC / size / alignment / loader; `find(fourcc)` and `find(name)` return the same backing record.
3. **Bit-exact round-trip** — fully-populated source (non-default scalars + 3 of 8 post_fx slots populated with distinct ResourceIds) → `PresetArtifactBuilder` → `PresetLoader` → `memcmp == 0`. Catches any silent padding / endianness drift.
4. **`IPresetTarget::apply(QualityPreset)` dispatches** — a `RecordingTarget` that overrides `apply()` observes both the call count and the preset value across two distinct invocations; a `SilentTarget` that doesn't override calls the base default body twice with no observable effect (proves the default body compiles + runs cleanly).

### Architectural decisions pinned

1. **Schema types live in `crd-preset`, not in their consumer modules.** `IPresetTarget::apply(QualityPreset)` is a method on the base class in `crd-preset` — adding it requires the full `QualityPreset` definition. If `QualityPreset` lived in `crd-renderer` it'd force `crd-preset → crd-renderer → crd-rhi`, which inverts the desired DAG. Concrete *targets* (a render path's apply override) still live in their consumer module; only the schema struct sits in `crd-preset`.

2. **`alignas(8)` on the struct + explicit reserved-byte padding.** Locks the on-disk byte pattern. The cooker writes raw bytes; if compilers chose different padding strategies for the `u8` cluster, on-disk bytes would diverge. Explicit `_reserved[8]` makes the layout invariant.

3. **`PostFXRef` modeled as `crd::resources::ResourceId`.** The ADR sample uses `PostFXRef`. Since the `PostFXPreset` type doesn't ship until Phase 3.5+, `ResourceId` is the right v1n stand-in: it's already 16-byte aligned, deterministic on disk, and the future `PostFXRef` typedef can shadow `ResourceId` without a binary change.

4. **`post_fx_count` semantics: only the first `count` entries are honored.** The remaining slots stay zero-valued, but the loader preserves them bit-exactly (test #822 verifies all 8 slots round-trip even when only 3 are "live"). Cookers and consumers must clear unused slots before serialisation if determinism is required across edits — ADR-0059 §"Determinism" already mandates this.

5. **Default constructor is implicit aggregate-init.** No explicit ctor — the schema struct is a POD aggregate. Consumers can `QualityPreset p{};` for defaults, `QualityPreset p{.shadow_resolution = 4096}` for designated-init customisations.

### Six-configuration green

- **win-debug**: 823/823 (was 819 post-v1n1; +4)
- **linux-gcc-debug**: 823/823 (was 819 post-v1n1; +4)

Other configs not separately rerun — v1n2 only adds new files (one schema header, four test cases) plus one virtual method on a class with no other consumers yet. No surface affecting other configs.

---

## What's deliberately NOT in v1n2

- **No `crd-renderer` integration.** ADR-0059 names `IRenderPath::apply(QualityPreset)` as the eventual consumer; v1n2 ships the base interface override hook + the schema. A render-path implementation will inherit `IPresetTarget` and override `apply()` when rendering integration lands (likely v1o3, the sandbox slice).
- **No `apply()` driver / dispatcher.** The substrate is "given a `PresetResource*`, hand bytes to a target's apply overload"; v1n2 doesn't ship the driver that does this for you because the resolution stack (v1n4) is what owns the dispatch logic. v1n2 just proves the apply hook fires when called directly.
- **No TOML reader.** The cooker handler that parses `quality.preset.toml` lands when the cooker integration ships — slated for after v1n3 once both initial concrete types exist (single TOML reader pattern handles all preset types via the registry).
- **No PostFXPreset / `'PRPP'`.** Phase 3.5+ per ADR-0059 §7.

---

## Files touched

```
engine/preset/include/crd/preset/quality_preset.hpp          created (76 lines)
engine/preset/include/crd/preset/preset_target.hpp           +14 lines (apply overload)
engine/preset/include/crd/preset/preset.hpp                  +1 line (umbrella)
tests/preset/CMakeLists.txt                                  +1 line
tests/preset/test_quality_preset.cpp                         created (4 cases / 38 assertions)
docs/phases/phase-3.0-scene-ecs.md                           v1n2 row added
docs/sessions/2026-05-09-preset-v1n2-quality-preset.md       this file
context.md                                                   dashboard updated (local-only)
```

---

## Next: v1n3 — `CameraPreset` second concrete type

`CameraPreset` schema struct (FourCC `'PRCM'`, version 1) carrying FOV / near / far / lens model / exposure curve. `IPresetTarget` gains `apply(const CameraPreset&)` overload. Even smaller than v1n2 because the substrate is reused — ~80 LOC, 3 tests. Once v1n3 ships, the pattern is fully demonstrated and v1n4 (five-layer resolution + extends chain) can build on top with two real consumer types in hand.
