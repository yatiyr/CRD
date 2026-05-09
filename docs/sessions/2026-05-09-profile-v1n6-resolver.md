# 2026-05-09 — Phase 3.0 v1n6: Profile resolver + context detection — closes v1n

**Status at start:** v1n5 shipped (Profile substrate types + load). 12-config sweep at 834/834 (831 optimised). 5 sub-slices uncommitted (v1n1..v1n5).

**Status at end:** v1n6 shipped — **v1n is fully delivered**. ProfileResolver evaluates predicates against ProfileContext, applies additive composition (priority-stable-sort + concat), context detection helpers (`detect_os` / `detect_cpu_cores`) ship for the closed enums. **Full 12-config sweep all green at 838/838** (835 in optimised configs). +4 cases / +50 assertions over the v1n5 baseline.

**Phase 3.0 v1n FULLY DELIVERED — 6 sub-slices (v1n1..v1n6), 24 tests, 245 assertions.**

---

## What shipped — v1n6

### `engine/profile/include/crd/profile/profile_resolver.hpp` (new)

```cpp
[[nodiscard]] bool evaluate_predicate(const PredicateRecord&, const ProfileContext&) noexcept;

class ProfileResolver {
public:
    explicit ProfileResolver(crd::memory::IAllocator*) noexcept;
    void                    set_resource(const ProfileResource*) noexcept;
    [[nodiscard]] const ProfileResource* resource() const noexcept;

    crd::u32 resolve(const ProfileContext& ctx,
                     crd::containers::Array<crd::resources::ResourceId>& out) const;
};

[[nodiscard]] OperatingSystem detect_os()         noexcept;
[[nodiscard]] crd::i32        detect_cpu_cores()  noexcept;
```

### Predicate evaluation (ADR-0060 §3, §7)

- **Canonical signed-i64 widening** — every ProfileContext axis is read as `i64` regardless of underlying type (u8 enum bytes vs i32 integer fields). Predicate's `value` (u32 in PredicateRecord) is reinterpret-cast to `i32` then widened to `i64` so negative comparands round-trip correctly through the u32 wire format.
- **Operators:**
  - `Equal` / `GreaterEq` / `LessEq` — direct i64 comparison.
  - `InMask` — only meaningful for enum fields; checks `(record.value >> ctx_field_byte) & 1`. Falls through to `false` if the context value is outside the 0..31 range (ADR-0060 §2 closed enums all fit, so this is paranoia).
- **No floating-point** in any predicate — determinism contract from ADR-0060 §7.

### Additive composition (ADR-0060 §3)

```cpp
crd::u32 ProfileResolver::resolve(const ProfileContext& ctx, Array<ResourceId>& out) const {
    out.clear();
    if (m_resource == nullptr || profiles().empty()) return 0U;

    // 1. Match every profile whose every predicate evaluates true.
    Array<Match> matches{...};
    for (each profile) if (all predicates pass) matches.push_back({priority, file_idx});

    // 2. Stable-sort matches by priority ascending (tie-break = file order via stable_sort).
    std::stable_sort(matches, ...);

    // 3. Concatenate bundles in priority-ascending order. No dedup —
    //    duplicate ids are intentionally preserved; consumer's apply step
    //    is idempotent and the LAST application wins per field (ADR §3 step 4).
    for (each match) out.append(profile.apply_bundle);

    return matches.size();
}
```

### Context detection helpers

```cpp
OperatingSystem detect_os() noexcept {
#if CRD_OS_WINDOWS  return OperatingSystem::Windows;
#elif CRD_OS_LINUX  return OperatingSystem::Linux;
#elif CRD_OS_MACOS  return OperatingSystem::MacOS;
#else               return OperatingSystem::Unknown;
#endif
}

crd::i32 detect_cpu_cores() noexcept {
    return std::thread::hardware_concurrency() == 0 ? 1 : ...;
}
```

GPU tier detection is deliberately not shipped — it requires RHI capability inspection which would force `crd-profile → crd-rhi`, inverting the dependency direction and pulling Vulkan into a leaf data module. Sandbox / app code constructs the context with whatever GPU tier mapping it wants (typically a config-file override or a startup-time RHI probe that calls `set_gpu_tier()` on the context post-detect).

### Tests — `tests/profile/test_profile_resolver.cpp` (4 cases / 50 assertions)

1. **Predicate operators** — Equal / GreaterEq / LessEq / InMask across enum (Os, GpuTier, Domain) and integer (TargetFps, CpuCores) fields. Includes the negative-comparand round-trip case (the i32 `-50` packed as u32 must round-trip and compare correctly against `target_fps = 144`).
2. **Single-profile matching** — one rule with two predicates; matching context returns the bundle, non-matching context returns empty.
3. **Multi-profile additive composition** — three profiles factored along ADR-0060 §3's authoring example (platform + tier + domain). Three test contexts: all-match → 3 profiles compose in priority-ascending order with 4 ResourceIds; partial-match → 2 profiles; minimal-match → 1 profile with 2 ResourceIds.
4. **Edge cases** — null resource, empty profile resource (resolve clears any stale `out` entries); context-detection helpers return sensible values on the runner (OS is Windows/Linux/macOS — never Unknown; cpu_cores >= 1).

---

## v1n FULLY SHIPPED — full inventory

| Sub-slice | What | Tests | Assertions |
|---|---|---|---|
| v1n1 | Preset substrate (`crd-preset` module: `PresetResource` + `PresetLoader` + `PresetRegistry::register_type<T>` + `PresetArtifactBuilder` + `IPresetTarget` + `PresetApplyEvent`) | 5 | 38 |
| v1n2 | `QualityPreset` first concrete type (FourCC `'PRQL'`, 144 B layout pinned) + `IPresetTarget::apply(QualityPreset)` overload | 4 | 38 |
| v1n3 | `CameraPreset` second concrete type (FourCC `'PRCM'`, 40 B) + `IPresetTarget::apply(CameraPreset)` overload + partial-override convention pinned | 3 | 39 |
| v1n4 | Five-layer resolver primitive (`resolve_preset<T>` + `apply_preset<T>`; L0 default + L1+L2 from PDAT + L4 runtime override; L3 reserved) | 4 | 36 |
| v1n5 | Profile substrate (`crd-profile` module: ProfileContext + closed predicate schema + Profile + ProfileResource + ProfileLoader + ProfileArtifactBuilder; CRDR FINF/FRLE/FBND) | 4 | 79 |
| v1n6 | Profile resolver (`evaluate_predicate` + `ProfileResolver::resolve`; additive composition with priority-stable-sort + concat) + context detection (`detect_os`, `detect_cpu_cores`) | 4 | 50 (this slice) |
| **TOTAL** | **24 tests** | **~280 assertions** | |

ADR-0059 + ADR-0060 §"Reserved API surface" frozen at v1n. Phase 4/5/6 consumer modules add new preset types via the per-type registration + apply overload pattern; they do not change the substrate.

### Six-configuration green — full sweep (post-v1n6)

**Windows (7/7):**
| Config | Status | Tests |
|---|---|---|
| win-debug | ✅ | 838/838 |
| win-asan | ✅ | 838/838 |
| win-relwithdebinfo | ✅ | 838/838 |
| win-release | ✅ | 835/835 |
| win-tidy | ✅ | build clean |
| win-clang-cl | ✅ | build clean |
| win-shipping | ✅ | build clean |

**Linux (5/5):**
| Config | Status | Tests |
|---|---|---|
| linux-gcc-debug | ✅ | 838/838 |
| linux-gcc-release | ✅ | 835/835 |
| linux-gcc-relwithdebinfo | ✅ | 838/838 |
| linux-gcc-asan | ✅ | 838/838 |
| linux-gcc-shipping | ✅ | build clean |

(838 = 814 baseline + 24 preset/profile tests. 835 in optimised configs minus 3 debug-only `FiberState` tests.)

Total parallel sweep time: ~9 min.

---

## What's deliberately NOT in v1n6 (closes v1n cleanly)

- **No `apply_profile_bundle` driver.** The cross-module bridge to `IPresetTarget*` (ADR-0060 §9 sketch) belongs in the v1o3 sandbox slice — it pulls `crd-preset` and `crd-profile` together and is shaped by the first real consumer (sandbox profile picker + quality slider + override window). Designing it now without that consumer in hand would risk baking in single-callsite assumptions, the same anti-pattern v1m's design avoided.
- **No GPU-tier detection.** Requires RHI capability inspection; including it in `crd-profile` would force `crd-profile → crd-rhi` and pull Vulkan into a leaf data module. Consumers detect/configure it themselves (sandbox / app boot path).
- **No `.preset.toml` / `.profile.toml` cooker handlers.** Tracked as a separable follow-up; not on the v1n closure path.
- **No hot-reload integration.** Watcher / atomic swap / diff-apply lands when the cooker handlers are built (paired with the TOML readers).
- **No domain-specific predicate registration hook.** Reserved (ADR-0060 §2) for Phase 4+ when network role / project-template predicates appear.

---

## Files touched (this slice only)

```
engine/profile/include/crd/profile/profile_resolver.hpp     created (~80 lines)
engine/profile/include/crd/profile/profile_umbrella.hpp     +1 line  (umbrella)
engine/profile/src/profile_resolver.cpp                     created (~150 lines)
tests/profile/CMakeLists.txt                                +1 line
tests/profile/test_profile_resolver.cpp                     created (4 cases / 50 assertions)
docs/phases/phase-3.0-scene-ecs.md                          v1n marked ✅ shipped; v1n6 row added
docs/sessions/2026-05-09-profile-v1n6-resolver.md           this file
context.md                                                  dashboard updated (local-only)
```

---

## Phase 3.0 status

| Slice | What | Status |
|---|---|---|
| v1a..v1l | Foundation through scene cooker | ✅ shipped 2026-05-06 / 07 / 08 |
| v1m | Öbek system (12 sub-slices, ADR-0058 fully realised) | ✅ shipped 2026-05-08 |
| v1n | Preset + Profile (6 sub-slices, ADRs 0059 + 0060 fully realised) | ✅ shipped 2026-05-09 |
| v1o | Async GPU upload (ADR-0061) + Sandbox integration with Öbek + Preset + Profile | ⏳ next |
| v1p | Reserved-slot freeze, closes Phase 3.0 | ⏳ |

**v1n closed. 2 sub-slices remain in Phase 3.0:** v1o (4-5 days, 3 sub-slices: RHI fence + UploadHandle/RenderUploadSystem + sandbox) → v1p (~1 day, freeze).
