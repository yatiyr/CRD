# 2026-05-09 — Phase 3.0 v1n5: Profile substrate (`crd-profile` module + types + load)

**Status at start:** v1n4 shipped (resolver primitive). 12-config sweep at 830/830 (827 in optimised configs). Working tree carries v1n1..v1n4 uncommitted.

**Status at end:** v1n5 shipped — new `crd-profile` module with types, predicate schema, `ProfileResource`, `ProfileLoader` (FourCC `'PROF'`), and `ProfileArtifactBuilder`. **Full 12-config sweep all green at 834/834** (831 in optimised configs). +4 cases / +79 assertions over the v1n4 baseline.

---

## What shipped

### New module `engine/profile/`

```
engine/profile/
├── CMakeLists.txt
├── include/crd/profile/
│   ├── profile_umbrella.hpp           # umbrella include for all crd-profile public headers
│   ├── profile_context.hpp            # ProfileContext + 4 closed enums
│   ├── profile_predicate.hpp          # PredicateField / PredicateOp / PredicateRecord
│   ├── profile.hpp                    # runtime Profile struct (priority + predicates + apply_bundle)
│   ├── profile_resource.hpp           # ProfileResource payload + FINF layout + chunk FourCCs
│   ├── profile_loader.hpp             # ILoader for 'PROF'
│   └── profile_artifact_builder.hpp   # test-public; cooker promotes when TOML reader lands
└── src/
    ├── profile_loader.cpp
    └── profile_artifact_builder.cpp
tests/profile/
├── CMakeLists.txt
└── test_profile.cpp                   # 4 cases / 79 assertions
```

Module dependencies: `crd-core` + `crd-containers` + `crd-memory` + `crd-resources`. **NOT** linked against `crd-preset` — Profile only stores `ResourceId`s; consumers interpret them as preset references. The `apply_profile_bundle(targets...)` API surface that bridges to `IPresetTarget*` lands in v1n6.

### Closed predicate schema (ADR-0060 §2)

```cpp
enum class PredicateField : crd::u8 {
    Os = 0, GpuTier = 1, Domain = 2, Mode = 3, TargetFps = 4, CpuCores = 5,
};
enum class PredicateOp : crd::u8 {
    Equal = 0, GreaterEq = 1, LessEq = 2, InMask = 3,
};
struct PredicateRecord {
    PredicateField field;        // 1 byte
    PredicateOp    op;           // 1 byte
    crd::u8        _reserved[2]; // 2 bytes (zero on disk)
    crd::u32       value;        // 4 bytes — see field interpretation below
};
static_assert(sizeof(PredicateRecord) == 8 && alignof(PredicateRecord) == 4);
```

Field interpretation:
- `Equal`/`GreaterEq`/`LessEq` on enum fields: `value = static_cast<u32>(enum_value)`
- `Equal`/`GreaterEq`/`LessEq` on integer fields (`TargetFps`, `CpuCores`): `value = static_cast<u32>(i32 comparand)` (bitcast; sign preserved)
- `InMask` on enum fields: `value = bitmask`, bit `n` set ⇔ enum value `n` is allowed (closed enums of ≤ 5 values comfortably fit)

The resolver's evaluation logic (which lookup to perform per field × op pair) lands in v1n6 alongside the `ProfileResolver::resolve(ProfileContext)` API.

### CRDR layout (ADR-0060 §5)

```
type_fourcc = 'PROF'

FBND (variable) — per-bundle entries, sequential walk:
                    repeating { u32 rule_idx; u32 preset_id_count;
                                ResourceId preset_ids[preset_id_count]; }
FINF (16 bytes) — ProfileFileInfo: schema_version + rule_count + bundle_count + flags
FRLE (variable) — per-rule entries, sequential walk:
                    repeating { u32 priority; u32 predicate_count;
                                PredicateRecord predicates[predicate_count]; }
```

Chunks sorted alphabetically by `CrdrWriter` → on-disk order is FBND < FINF < FRLE. v1n5 enforces `rule_count == bundle_count` (one bundle per rule); `FBND.rule_idx` always equals its sequence index. The split exists so future schema versions can decouple bundle storage (e.g. share a bundle across multiple rules) without breaking the format.

### `Profile` runtime struct

```cpp
struct Profile {
    crd::u32                                          priority = 0U;
    crd::containers::Array<PredicateRecord>           predicates;
    crd::containers::Array<crd::resources::ResourceId> apply_bundle;
};
```

Move-only (the contained Arrays are move-only). Constructed with the loader's allocator so all backing storage routes through the same `IAllocator`.

### Tests — `tests/profile/test_profile.cpp` (4 cases / 79 assertions)

1. **Layout pin** — every closed-enum byte value verified (`Os == 0`, `MacOS == 3`, `Ultra == 4`, `Cinematic == 4`, `Headless == 3`); `PredicateRecord` size/alignment + `ProfileFileInfo` size pinned; `ProfileContext` defaults verified (`target_fps == 60`, `cpu_cores == 1`).
2. **Single-profile round-trip** — one rule, priority 100, two predicates (`os == Windows`, `gpu_tier >= High`), bundle of 2 `ResourceId`s. Builder → loader → bit-equal.
3. **Multi-profile round-trip** — three rules with mixed predicate counts (0, 1, 3) and mixed bundle sizes (1, 3, 0); rule order preserved; FBND `rule_idx` cross-links validated.
4. **ProfileLoader rejects mismatched / malformed input** — empty bytes, wrong-FourCC blob (built as `'BADX'`), and confirmation that an empty PROF artifact loads cleanly.

### Architectural decisions pinned

1. **Module is a peer of `crd-preset`, not a child.** Profile depends on `crd-resources` (for `ResourceId`) but not on `crd-preset`. This keeps the dependency DAG one-way: in v1n6, the `apply_profile_bundle` driver will live in `crd-preset` (or a new `crd-profile-driver` thin layer) and pull both modules together. A single combined module would force `crd-preset` to depend on `crd-profile`'s context detection — backwards.

2. **Closed enums append-only.** `OperatingSystem`/`GpuTier`/`ProjectDomain`/`AppMode` and `PredicateField`/`PredicateOp` all reserve slot 0 as `Unknown` and append new values at the end. On-disk byte representation stays stable across schema versions; no migration tables needed for additions.

3. **`InMask` operator stored but not yet evaluated.** v1n5 parses + serialises the byte representation; v1n6's resolver implements the lookup (`(1U << static_cast<u32>(ctx.gpu_tier)) & predicate.value`). Splitting the work so v1n5 can ship the data layer cleanly without a half-built resolver.

4. **`PredicateRecord::value` is a `u32` regardless of field type.** Avoids per-field unions / template specialisation. Sign extension for `i32` fields handled at evaluate time via reinterpret-cast in v1n6.

5. **No registry yet.** Unlike `PresetRegistry::register_type<T>()` (which dispatches per concrete preset type), the Profile system has no per-type registration — there's only ONE profile schema (FourCC `'PROF'`). A single `ProfileLoader` instance handles every profile artifact. Registration hook for domain-specific predicates is reserved (ADR-0060 §2) but not exposed in v1n.

### Six-configuration green — full sweep

**Windows (7/7):**
| Config | Status | Tests |
|---|---|---|
| win-debug | ✅ | 834/834 |
| win-asan | ✅ | 834/834 |
| win-relwithdebinfo | ✅ | 834/834 |
| win-release | ✅ | 831/831 |
| win-tidy | ✅ | build clean |
| win-clang-cl | ✅ | build clean |
| win-shipping | ✅ | build clean |

**Linux (5/5):**
| Config | Status | Tests |
|---|---|---|
| linux-gcc-debug | ✅ | 834/834 |
| linux-gcc-release | ✅ | 831/831 |
| linux-gcc-relwithdebinfo | ✅ | 834/834 |
| linux-gcc-asan | ✅ | 834/834 |
| linux-gcc-shipping | ✅ | build clean |

(834 = 814 baseline + 20 preset/profile tests across v1n1..v1n5: 5+4+3+4+4. 831 in optimised configs minus 3 debug-only `FiberState` tests.)

Total parallel sweep time: ~9.5 min (Windows ~9 min including Vulkan SDK reconfigure for the new module; Linux ~9.5 min).

---

## What's deliberately NOT in v1n5

- **No resolver.** `ProfileResolver::resolve(ProfileContext)` (predicate evaluation + additive composition) is v1n6.
- **No runtime context detection.** `detect_os()` / `detect_gpu_tier()` / etc. helpers are v1n6.
- **No `apply_profile_bundle` driver.** The bridge to `IPresetTarget*` (which calls into `crd-preset`'s `apply_preset<T>`) is v1n6.
- **No TOML reader.** `.profile.toml` cooker handler is a separable follow-up; the same status as the `.preset.toml` cooker.
- **No hot-reload integration.** Watcher / atomic swap / re-apply event lands when the cooker handler is built.
- **No per-domain default profile shipping.** ADR-0060 §"Open questions" reserves `default.profile.toml` with sensible game/sim/DAW/cinematic baselines for v1o (sandbox demo).

---

## Files touched

```
engine/profile/CMakeLists.txt                                created (~20 lines)
engine/profile/include/crd/profile/profile_umbrella.hpp      created (umbrella)
engine/profile/include/crd/profile/profile_context.hpp       created (~60 lines)
engine/profile/include/crd/profile/profile_predicate.hpp     created (~70 lines)
engine/profile/include/crd/profile/profile.hpp               created (~30 lines)
engine/profile/include/crd/profile/profile_resource.hpp      created (~80 lines)
engine/profile/include/crd/profile/profile_loader.hpp        created (~45 lines)
engine/profile/include/crd/profile/profile_artifact_builder.hpp created (~55 lines)
engine/profile/src/profile_loader.cpp                        created (~150 lines)
engine/profile/src/profile_artifact_builder.cpp              created (~110 lines)
tests/profile/CMakeLists.txt                                 created
tests/profile/test_profile.cpp                               created (4 cases / 79 assertions)
CMakeLists.txt                                               +1 line (add_subdirectory(engine/profile))
tests/CMakeLists.txt                                         +1 line (add_subdirectory(profile))
docs/phases/phase-3.0-scene-ecs.md                           v1n5 row added
docs/sessions/2026-05-09-profile-v1n5-substrate.md           this file
context.md                                                   dashboard updated (local-only)
```

---

## Next: v1n6 — additive composition + runtime context detection + hot-reload (closes v1n)

Last sub-slice of v1n. Ships:
- `ProfileResolver::resolve(const ProfileContext&) → Array<ResourceId>` — predicate evaluation against context for each profile, additive composition (priority-sorted stack, deepest priority wins per field).
- `detect_os()` / `detect_gpu_tier()` / `detect_cpu_cores()` helpers via `crd-platform`.
- `apply_profile_bundle(resolver, ctx, targets)` driver — bridges `crd-profile` to `crd-preset`'s `apply_preset<T>`.
- Hot-reload integration: profile re-cook → re-resolve → diff apply (presets dropped + presets added).

~150 LOC, 4 tests. Half day. Closes Phase 3.0 v1n; v1o (the sandbox slice) is next.
