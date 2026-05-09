# 2026-05-09 — Phase 3.0 v1n1: Preset substrate (`crd-preset` module + registry + loader + artifact builder)

**Status at start:** Phase 3.0 v1m fully delivered; v1n is the next slice. Off-the-record session before this one closed CI (12/12 green) and locked ADR-0061 (async GPU upload contract → v1o1+v1o2). Working tree clean.

**Status at end:** v1n1 shipped — new `crd-preset` module with the full substrate from ADR-0059. **win-debug 819/819, linux-gcc-debug 819/819 (+5 tests, +38 assertions over the 814/814 baseline).** No concrete preset types yet; those are v1n2 (`QualityPreset`) and v1n3 (`CameraPreset`).

---

## Sub-slice plan + result

v1n is now sliced as 6 reviewable parts. v1n1 is "substrate only — no concrete types".

| v1n sub-slice | Scope | Status |
|---|---|---|
| **v1n1** | `crd-preset` module: `PresetResource` (PINF/PDAT/PCHN), `PresetLoader` (per-type runtime-configured), `PresetRegistry::register_type<T>()`, `PresetArtifactBuilder`, `IPresetTarget` base, `PresetApplyEvent` | ✅ shipped (this session) |
| v1n2 | `QualityPreset` first concrete type (FourCC `'PRQL'`); apply hook into `IRenderPath::apply(QualityPreset)` | ⏳ next |
| v1n3 | `CameraPreset` (FourCC `'PRCM'`); apply hook into `Camera::apply(CameraPreset)` | ⏳ |
| v1n4 | Five-layer resolution stack (default → extends → preset → instance → runtime); `extends` chain shares Öbek resolver | ⏳ |
| v1n5 | Profile substrate (ADR-0060): `ProfileResource` + `ProfileLoader` (`'PROF'`) + closed predicate schema | ⏳ |
| v1n6 | Additive composition + runtime context detection + hot-reload with atomic swap | ⏳ |

---

## What shipped — v1n1

### New module `engine/preset/`

```
engine/preset/
├── CMakeLists.txt
├── include/crd/preset/
│   ├── preset.hpp                    # umbrella
│   ├── preset_resource.hpp           # payload + PINF/PDAT/PCHN structs
│   ├── preset_loader.hpp             # ILoader specialization
│   ├── preset_registry.hpp           # closed-by-types registration
│   ├── preset_artifact_builder.hpp   # test-only public; cooker promotes later
│   ├── preset_target.hpp             # empty IPresetTarget base
│   └── preset_apply_event.hpp        # POD struct for hot-reload events
└── src/
    ├── preset_resource.cpp           # placeholder TU; header-only otherwise
    ├── preset_loader.cpp
    ├── preset_registry.cpp
    └── preset_artifact_builder.cpp
tests/preset/
├── CMakeLists.txt
└── test_preset_registry.cpp          # 5 tests / 38 assertions
```

Module dependencies: `crd-core` + `crd-containers` + `crd-memory` + `crd-resources`. Sits next to `crd-scene` in the build graph; consumer modules (`crd-renderer`, future `crd-audio` / `crd-physics` / etc.) will link `crd-preset` PUBLIC.

### CRDR layout

```
type_fourcc = <per-concrete-type, e.g. 'PRQL' for QualityPreset>

PCHN  (variable) — extends-chain dependencies (sorted first by FourCC):
                     u32 entry_count + u32 reserved
                     + PresetChainEntry[entry_count]   (16 B per entry)
                   `PresetChainEntry { u64 path_hash; u64 content_hash; }`
                   Used by the hot-reload watcher to detect upstream changes
                   through the variant chain.

PDAT  (variable) — flat schema payload bytes (sizeof(T)). Variant chain
                   is pre-resolved at cook time; field order matches schema
                   declaration.

PINF  (16 bytes) — PresetInfo: schema_version + flags + payload_size +
                   reserved. payload_size is a sanity check vs the
                   registered type's sizeof at load time.
```

Chunks are written via `crd::resources::CrdrWriter` which sorts by FourCC at `finish()` — deterministic on-disk byte order (PCHN < PDAT < PINF lexicographically). Same source files + same registration order → bit-exact bytes.

### `PresetRegistry::register_type<T>()` — closed-by-types

Schema-struct contract for a registered type T:

```cpp
struct MyPresetSchema
{
    static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P','R','M','Y');
    static constexpr crd::u32 version = 1U;
    // ... fields with default values, in canonical declaration order
};

reg.register_type<MyPresetSchema>(crd::containers::StringView{"My"});
```

`register_type<T>` static-asserts that T provides the right contract, then mints a `PresetLoader` instance configured for `(fourcc, version, sizeof(T), alignof(T))` and stores it in `m_owned_loaders`. The created `PresetTypeInfo` carries a non-owning `PresetLoader*` view that callers can hand to `ResourceManager::register_loader`.

Idempotency contract (matches `ComponentRegistry`, ADR-0050 §1):
- Re-registering the same `T::fourcc` returns the existing `PresetTypeInfo` reference.
- The second call's `name` is **ignored**.
- The original loader instance is preserved (no replacement, no double-destruction).
- Result: libraries can register defensively without coordination.

### `PresetLoader` — runtime-configured per type

Each registered preset type gets its own `PresetLoader` instance because `crd::resources::ILoader::type_fourcc()` returns one FourCC per loader (the contract enforced by `ResourceManager`). The instance captures its FourCC + schema version + payload size at construction; `load()` validates the CRDR header (`type_fourcc` match), the PINF chunk (16 B exact, `payload_size == sizeof(T)`), the PDAT chunk (`size == payload_size`), and parses the optional PCHN chunk. Hard-fails (returns nullptr → `LoadState::Failed`) on any inconsistency. No soft fallback in v1n1.

### Architectural decisions pinned

1. **One loader per registered type, owned by the registry.** Alternative (templated loader specialised on schema struct) was considered; runtime configuration won because it matches the project's "no template explosion at the loader registry" principle. Each `register_type<T>` adds one `PresetLoader` instance + one `PresetTypeInfo` entry to the registry.

2. **`PresetResource` is type-erased; consumers reinterpret-cast `bytes()` to `T*`.** This avoids adding a virtual interface for "give me the typed view" and lets the same `PresetResource` flow through `ResourceManager` regardless of the concrete schema. Consumers that know the schema (the matching `IPresetTarget::apply` overload) cast and read; FourCC + payload_size sanity checks at load time make the cast safe.

3. **Aggregate brace-init for `PresetTypeInfo`.** MSVC under `/WX` rejects value-initialization (`PresetTypeInfo info{};`) of structs containing a `crd::containers::String` member because `String`'s default constructor is `explicit` (only direct-init / direct-list-init is allowed). Switched to positional aggregate init at the call site (`PresetTypeInfo{fourcc, version, …, String(name, alloc), loader}`) which is unambiguously direct-init.

4. **`HashMap::find()` returns `V*`, not an iterator.** The project's `crd::containers::HashMap` uses pointer-or-nullptr return semantics (matches std's `find_if` style at the value level). Caught at first compile attempt; loader/registry both use `if (auto* p = m_by_fourcc.find(fc); p != nullptr)` instead of the std-iterator pattern.

5. **PCHN written only when chain entries exist.** Keeps no-extends presets minimal (PINF + PDAT only). The reader accepts a missing PCHN chunk as "zero chain dependencies".

6. **`PresetArtifactBuilder` ships in v1n1 as test-only public API.** Mirrors the v1k `SceneArtifactBuilder` and v1m1 `ObekArtifactBuilder` pattern — the cooker handler in v1n2+ will wrap it once concrete types provide the TOML reader.

### Tests (`tests/preset/test_preset_registry.cpp` — 5 cases / 38 assertions)

1. **`register_type<T>` populates TypeInfo and is queryable** — fourcc / version / size / alignment / name / loader pointer all match the schema; `find(fourcc)` and `find(name)` return non-null.
2. **`register_type<T>` is idempotent** — second call returns same `&TypeInfo`; second call's name is ignored; registering a second distinct type extends the registry to size 2.
3. **`PresetArtifactBuilder + PresetLoader` round-trip schema bytes** — build PINF/PDAT bytes, load, verify fourcc + version + bytes are bit-equal.
4. **`PresetLoader` rejects mismatched FourCC** — a loader configured for `AlphaSchema` returns nullptr on a `BetaSchema` artifact; the matching `BetaSchema` loader accepts it.
5. **PCHN chain dependencies round-trip** — three chain entries written and read back bit-exactly (path_hash + content_hash).

### Six-configuration green

- **win-debug**: 819/819 (was 814; +5)
- **linux-gcc-debug**: 819/819 (was 814; +5)

Other configs not separately rerun this session — the changes are all new files (no existing-code modification beyond the two CMakeLists `add_subdirectory` lines), so the surface that could affect other configs is minimal. The full sweep pass landed in the previous session.

---

## What's deliberately NOT in v1n1

- **No concrete preset types.** `QualityPreset` is v1n2; `CameraPreset` is v1n3. v1n1 is just the dispatch machinery.
- **No `IPresetTarget::apply()` overloads.** The base class is empty in v1n1; v1n2 adds the `QualityPreset` overload, v1n3 adds `CameraPreset`. Targets gain overloads as the concrete types ship.
- **No five-layer resolution.** `default → extends → preset → instance → runtime` lands in v1n4. v1n1 is single-layer (the cooked payload bytes are what `apply()` would receive raw).
- **No `extends` chain resolution.** PCHN format is written/read in v1n1 (so the runtime can carry chain dependencies through hot-reload), but the cook-time resolver that fills it lands with v1n4 (it shares the Öbek resolver from v1m3b).
- **No Profile substrate.** v1n5+ (ADR-0060).
- **No hot-reload watcher integration.** `PresetApplyEvent` is declared so consumers can prepare for it, but the dispatch wiring lands later.
- **No TOML reader.** Concrete types in v1n2/v1n3 will register their own readers via the v1l SceneCooker reader-registry pattern.
- **No `ResourceManager::register_loader` integration.** Loaders are minted by the registry but caller-side wiring (sandbox + tests) is per-consumer; v1n3 will demonstrate the pattern alongside `CameraPreset`.

---

## Files touched

```
engine/preset/CMakeLists.txt                                 created
engine/preset/include/crd/preset/preset.hpp                  created (umbrella)
engine/preset/include/crd/preset/preset_resource.hpp         created
engine/preset/include/crd/preset/preset_loader.hpp           created
engine/preset/include/crd/preset/preset_registry.hpp         created
engine/preset/include/crd/preset/preset_artifact_builder.hpp created
engine/preset/include/crd/preset/preset_target.hpp           created
engine/preset/include/crd/preset/preset_apply_event.hpp      created
engine/preset/src/preset_resource.cpp                        created
engine/preset/src/preset_loader.cpp                          created
engine/preset/src/preset_registry.cpp                        created
engine/preset/src/preset_artifact_builder.cpp                created
tests/preset/CMakeLists.txt                                  created
tests/preset/test_preset_registry.cpp                        created (5 cases)
CMakeLists.txt                                               +1 line (add_subdirectory(engine/preset))
tests/CMakeLists.txt                                         +1 line (add_subdirectory(preset))
docs/phases/phase-3.0-scene-ecs.md                           v1n marked active; v1n1 row added
docs/sessions/2026-05-09-preset-v1n1-substrate.md            this file
context.md                                                   dashboard updated (local-only)
```

---

## Next: v1n2 — `QualityPreset` first concrete type

`QualityPreset` schema struct (FourCC `'PRQL'`, version 1) carrying shadow_resolution / msaa_samples / ssr_quality / ssao_quality / post_fx_count + post_fx[8]. `IPresetTarget` gains the `apply(const QualityPreset&)` overload. Sandbox / test target proves the apply hook fires. ~150 LOC, 4 tests. ~half day.
