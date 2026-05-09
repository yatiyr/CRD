# 2026-05-09 — Phase 3.0 v1n4: five-layer resolver primitive (`resolve_preset<T>` + `apply_preset<T>`)

**Status at start:** v1n3 shipped — both concrete preset types (`QualityPreset` + `CameraPreset`) live; partial-override convention pinned; 12-config sweep at 826/826 (823 in optimised configs).

**Status at end:** v1n4 shipped — `resolve_preset<T>` + `apply_preset<T>` template helpers in `preset_resolver.hpp` capture ADR-0059 §2's five-layer resolution semantics. **Full 12-config sweep all green at 830/830** (827 in optimised configs). +4 cases / +36 assertions over the v1n3 baseline.

---

## What shipped

### `engine/preset/include/crd/preset/preset_resolver.hpp` (new)

```cpp
template <typename T>
[[nodiscard]] T resolve_preset(const PresetResource* resource,
                               const T*              runtime_override = nullptr) noexcept
{
    static_assert(static_cast<crd::u32>(T::fourcc) != 0U, "...");
    static_assert(static_cast<crd::u32>(T::version) >= 1U, "...");

    T value{};                                       // L0 — schema default
    if (resource != nullptr) {
        CRD_ASSERT_MSG(resource->fourcc()       == T::fourcc, "...");
        CRD_ASSERT_MSG(resource->bytes().size() == sizeof(T), "...");
        std::memcpy(&value, resource->bytes().data(), sizeof(T)); // L1+L2 (cook-resolved)
    }
    // L3 reserved.
    if (runtime_override != nullptr) {
        value = *runtime_override;                   // L4
    }
    return value;
}

template <typename T>
void apply_preset(IPresetTarget&        target,
                  const PresetResource* resource,
                  const T*              runtime_override = nullptr) noexcept
{
    target.apply(resolve_preset<T>(resource, runtime_override));
}
```

### Five-layer mapping (ADR-0059 §2)

| Layer | Source | When folded | v1n4 status |
|---|---|---|---|
| L0 — Schema default | `T{}` (compile-time defaults in the schema struct) | At runtime, by the resolver as the starting value | ✅ shipped |
| L1 — `extends` chain | Cooker walks the chain **deepest-first**, merges field-by-field, writes PDAT (shares Öbek resolver per ADR-0058) | At cook time | ⏳ cooker not built (v1n4 follow-up; the format already round-trips PCHN deps from v1n1) |
| L2 — Active preset | The cooked `PresetResource` selected by Profile (ADR-0060) — its PDAT bytes already encode L0+L1+L2 | At cook time | ✅ shipped (PDAT load works) |
| L3 — Per-instance | Per-entity in the scene; one camera ignores active QualityPreset | — | ⏳ reserved (Phase 4+ per ADR-0059 §"Open questions") |
| L4 — Runtime override | Caller-supplied full T value (CVar / ImGui slider / debug toggle) | At apply time | ✅ shipped (full-T replacement; partial-field override deferred to v1o3 sandbox quality slider) |

The resolver is **stateless** — every call rebuilds the value from inputs. ADR-0059 §2's caching contract ("targets cache the resolved value until the next apply event") is the **target's** responsibility, not the resolver's.

### Why a free function instead of a class

Considered a `PresetResolver<T>` builder with `.with_resource()` / `.with_override()` / `.apply_to()` chained methods. Rejected because:
- The resolution is functional (pure, no state). A class adds API surface for no semantic gain.
- Five layers fold into "two inputs" at runtime (the resource + the override). A two-arg function expresses this directly.
- Tests + consumers read more naturally as `apply_preset<T>(target, res)` than as `PresetResolver<T>{}.with_resource(res).apply_to(target)`.
- Mirrors the project's existing free-function style (`fnv1a`, `make_fourcc`, `crd::math::lerp`).

If a future need arises for mid-resolution introspection or partial-field override stacking, a class can be added then on top of the free function. v1n4 ships the simplest correct surface.

### Tests — `tests/preset/test_preset_resolver.cpp` (new, 4 cases / 36 assertions)

1. **L0 only** — `resolve_preset<T>(nullptr)` returns documented schema defaults for both QualityPreset and CameraPreset (5 + 5 default-field checks).
2. **L1+L2 from PDAT** — cook a populated QualityPreset, load it, resolve it without override; the resource's bytes win over schema defaults; `memcmp == 0` against source.
3. **L4 wins over L2** — cook a CameraPreset with one set of values, resolve with a different runtime override; the override replaces the resource value entirely. Cross-check: without the override, the resource value comes through.
4. **`apply_preset<T>` dispatches to the correct overload** — `DualRecordingTarget` (overrides both Quality + Camera apply slots) observes:
   - `apply_preset<QualityPreset>(target, nullptr)` → quality_count=1, applies defaults (2048).
   - `apply_preset<QualityPreset>(target, q_res)` → quality_count=2, applies cooked value (4096).
   - `apply_preset<CameraPreset>(target, c_res)` → camera_count=1, applies cooked value.
   - `apply_preset<CameraPreset>(target, c_res, &override)` → camera_count=2, applies override.
   - Camera + Quality dispatch are independent — no overload-resolution surprises.

### Six-configuration green — full sweep

**Windows (7/7):**
| Config | Status | Tests |
|---|---|---|
| win-debug | ✅ | 830/830 |
| win-asan | ✅ | 830/830 |
| win-relwithdebinfo | ✅ | 830/830 |
| win-release | ✅ | 827/827 |
| win-tidy | ✅ | build clean |
| win-clang-cl | ✅ | build clean |
| win-shipping | ✅ | build clean |

**Linux (5/5):**
| Config | Status | Tests |
|---|---|---|
| linux-gcc-debug | ✅ | 830/830 |
| linux-gcc-release | ✅ | 827/827 |
| linux-gcc-relwithdebinfo | ✅ | 830/830 |
| linux-gcc-asan | ✅ | 830/830 |
| linux-gcc-shipping | ✅ | build clean |

(830 = 814 baseline + 16 preset tests across v1n1..v1n4. 827 in optimised configs minus 3 debug-only `FiberState` tests.)

Total parallel sweep time: ~6 min (Windows ~1.5 min, Linux ~5 min).

---

## What's deliberately NOT in v1n4

- **No cooker handler.** `extends` chain resolution at cook time + TOML reader for `.preset.toml` files lands in a focused follow-up after v1n4 (see "Next" below). The format itself already supports the chain (PCHN round-trips from v1n1).
- **No L3 (per-instance) implementation.** Reserved for Phase 4+ per ADR-0059 §"Open questions" — needs a real consumer (rare case: one camera entity ignoring the active QualityPreset) before the API shape is locked.
- **No partial-field runtime override.** v1n4's L4 is full-T replacement (caller constructs the entire `T` they want). A field-mask / dirty-bitset variant for "tweak one field via ImGui" is reserved for v1o3 (sandbox quality slider) so the API is shaped by a real consumer rather than speculative design.
- **No `IRenderPath::apply(QualityPreset)` / `Camera::apply(CameraPreset)` consumer integration.** Those land in v1o3 (sandbox slice).

---

## Files touched

```
engine/preset/include/crd/preset/preset_resolver.hpp        created (~90 lines, header-only)
engine/preset/include/crd/preset/preset.hpp                 +1 line (umbrella)
tests/preset/CMakeLists.txt                                 +1 line
tests/preset/test_preset_resolver.cpp                       created (4 cases / 36 assertions)
docs/phases/phase-3.0-scene-ecs.md                          v1n4 row added
docs/sessions/2026-05-09-preset-v1n4-resolver.md            this file
context.md                                                  dashboard updated (local-only)
```

---

## Next: v1n5 — Profile substrate (ADR-0060)

`ProfileResource` + `ProfileLoader` (FourCC `'PROF'`) + closed predicate schema (`os` / `gpu_tier` / `domain` / `mode` / `target_fps` / `cpu_cores`). Drives "which presets compose for this runtime context". ~150 LOC, 4 tests. ~half day. The companion v1n6 (additive composition + runtime context detection) closes v1n's substrate-and-resolver phase before the cooker handler ships.

Alternative direction: **lift the `.preset.toml` cooker handler out of the v1n5/v1n6 schedule and ship it now** — it would let the demo flow ("author a preset in TOML → cook → load → resolve → apply") work end-to-end before Profile lands. Trade-off: ~250 LOC of cooker glue (TOML reader registration, `extends` chain walker reusing v1m3b's Öbek resolver, PCHN emit, `preset_cooker_inline()`) vs. continuing the substrate-first cadence. User decides.
