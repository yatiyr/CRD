# 2026-05-09 — Phase 3.0 v1n3: `CameraPreset` second concrete preset type + partial-override convention

**Status at start:** v1n2 just shipped — `QualityPreset` is the first concrete type, `IPresetTarget::apply(QualityPreset)` overload landed on the base. Tests at 823/823 across both platforms.

**Status at end:** v1n3 shipped — `CameraPreset` is the second concrete type, `IPresetTarget::apply(CameraPreset)` overload appended. **Full 12-config sweep (Win × 7, Linux × 5) all green: 826/826 in assert-enabled configs, 823/823 in optimised configs (debug-only `FiberState` tests gated out).**

---

## What shipped

### Schema struct — `engine/preset/include/crd/preset/camera_preset.hpp` (new)

```cpp
enum class LensModel    : crd::u8 { Perspective = 0, Orthographic = 1 };
enum class ExposureMode : crd::u8 { Manual      = 0, AutoEV100    = 1 };

struct alignas(4) CameraPreset
{
    static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P','R','C','M');
    static constexpr crd::u32 version = 1U;

    // Projection
    crd::f32 fov_y_radians      = 1.0471975512F; // ≈ 60°
    crd::f32 near_plane         = 0.1F;
    crd::f32 far_plane          = 1000.0F;

    // Physical exposure triplet
    crd::f32 aperture_f_stop    = 2.8F;
    crd::f32 shutter_seconds    = 1.0F / 60.0F;
    crd::f32 iso                = 100.0F;
    crd::f32 exposure_comp_ev   = 0.0F;

    // Auto-exposure clamps (only honored when exposure_mode == AutoEV100)
    crd::f32 ev100_min          = -8.0F;
    crd::f32 ev100_max          = 16.0F;

    // Mode flags + explicit padding
    LensModel    lens_model     = LensModel::Perspective;
    ExposureMode exposure_mode  = ExposureMode::Manual;
    crd::u8      _reserved[2]   = {};
};
static_assert(sizeof(CameraPreset)  == 40);
static_assert(alignof(CameraPreset) == 4);
```

**Layout pinned at version=1:** 9 × f32 (36 B) + 2 × enum-u8 (2 B) + 2-byte padding = 40 bytes. Same migration contract as `QualityPreset`: any layout change bumps `version`, the loader's payload-size check converts a mismatch into `LoadState::Failed`.

### `IPresetTarget::apply(const CameraPreset&)` — second overload

```cpp
class IPresetTarget {
public:
    // ...
    virtual void apply(const QualityPreset& /*preset*/) {}    // v1n2
    virtual void apply(const CameraPreset&  /*preset*/) {}    // v1n3 (this slice)
    // ...
};
```

### Tests — `tests/preset/test_camera_preset.cpp` (new, 3 cases / 39 assertions)

1. **Schema defaults + identity** — every documented default verified, sizeof/alignof pinned.
2. **Bit-exact round-trip** — fully-populated source (custom values for all 11 fields including non-default enums for both `LensModel` and `ExposureMode`) → builder → loader → `memcmp == 0`. Catches any silent enum-storage / padding / endianness drift.
3. **Independent overload dispatch** — a `DualRecordingTarget` overrides BOTH `apply(QualityPreset)` and `apply(CameraPreset)`; the test fires both kinds of presets across 3 calls and verifies independent counters and last-applied values. A `SilentTarget` exercises the empty default body of both overloads.

### Architectural decision pinned — partial-override convention

GCC's `-Woverloaded-virtual` (treated as `-Werror` in the project's Linux configs) flags a real C++ name-hiding rule: when a derived class overrides ONE of multiple base virtual overloads, the others get hidden by name lookup unless explicitly re-imported. The first sweep failed:

```
preset_target.hpp:45:18: error: 'virtual void IPresetTarget::apply(const CameraPreset&)' was hidden
note: by 'virtual void RecordingTarget::apply(const QualityPreset&)'
```

This is engine-wide: any consumer that overrides only some of the apply overloads (e.g. an `IRenderPath` that consumes only `QualityPreset`) hits the same warning. Two options were considered:

| Option | Verdict |
|---|---|
| Suppress `-Woverloaded-virtual` project-wide | Rejected — it's a real name-hiding bug class, not just a stylistic warning. |
| Codify the `using IPresetTarget::apply;` convention | **Adopted.** Standard C++ idiom; documented in `preset_target.hpp`. |

The header now carries a "Partial-override convention" comment block with the canonical pattern. v1n2's `RecordingTarget` was retroactively updated; the v1n3 `DualRecordingTarget` overrides BOTH so the convention isn't required there. Future consumers (renderer's `IRenderPath`, audio's mixer, etc.) follow the same pattern.

MSVC didn't catch this even under `/W4 /WX` because C4263/C4264 aren't fatal at default severity. The asymmetry is exactly what cross-platform CI is for.

### Six-configuration green — full sweep

**Windows (7/7):**
| Config | Status | Tests |
|---|---|---|
| win-debug | ✅ | 826/826 |
| win-asan | ✅ | 826/826 |
| win-relwithdebinfo | ✅ | 826/826 |
| win-release | ✅ | 823/823 |
| win-tidy | ✅ | build clean |
| win-clang-cl | ✅ | build clean |
| win-shipping | ✅ | build clean |

**Linux (5/5):**
| Config | Status | Tests |
|---|---|---|
| linux-gcc-debug | ✅ | 826/826 |
| linux-gcc-release | ✅ | 823/823 |
| linux-gcc-relwithdebinfo | ✅ | 826/826 |
| linux-gcc-asan | ✅ | 826/826 |
| linux-gcc-shipping | ✅ | build clean |

(826 = 814 baseline + 12 preset tests; 823 in optimised builds = 826 minus 3 debug-only `FiberState` tests gated by `#if CRD_ENABLE_ASSERTS`.)

Total parallel sweep time: ~7.5 minutes (Windows ~1.5 min, Linux ~7.5 min).

---

## What's deliberately NOT in v1n3

- **No registry registration test** — same machinery as v1n2 (`PresetRegistry::register_type<CameraPreset>` works identically); not worth a separate test case.
- **No `Camera::apply(CameraPreset)` consumer integration** — the `Camera` class lives in `crd-renderer` and will inherit `IPresetTarget` when the apply hook ships in v1o3 (sandbox integration).
- **No TOML reader for `.preset.toml`** — cooker handler lands when both initial concrete types exist (now they do); slated for a focused follow-up after v1n4.
- **No five-layer resolution** — that's v1n4, which builds atop having two concrete types.

---

## Files touched

```
engine/preset/include/crd/preset/camera_preset.hpp           created (74 lines)
engine/preset/include/crd/preset/preset_target.hpp           +18 lines (apply overload + partial-override convention doc)
engine/preset/include/crd/preset/preset.hpp                  +1 line (umbrella)
tests/preset/CMakeLists.txt                                  +1 line
tests/preset/test_camera_preset.cpp                          created (3 cases / 39 assertions)
tests/preset/test_quality_preset.cpp                         +1 line (`using IPresetTarget::apply;`)
docs/phases/phase-3.0-scene-ecs.md                           v1n2 row reformatted; v1n3 row added
docs/sessions/2026-05-09-preset-v1n3-camera-preset.md        this file
context.md                                                   dashboard updated (local-only)
```

---

## Pattern fully demonstrated — v1n4 unblocked

After v1n1 (substrate) + v1n2 (`QualityPreset`) + v1n3 (`CameraPreset`), the per-type registration grammar has two real schema types passing through it. **The pattern is fully demonstrated.** Adding a third type (e.g. `AudioDevicePreset` in Phase 5) requires one schema header, one `IPresetTarget::apply()` overload, and per-type tests — no substrate changes.

## Next: v1n4 — five-layer resolution stack + `extends` chain

Resolution stack: schema default → extends chain → active preset → per-instance → runtime override. The `extends` resolver shares the Öbek implementation from v1m3b (cycle-detected, deepest-first apply, PCHN entries written by the cooker per-extends-link). ~100 LOC, 4 tests. ~half day.
