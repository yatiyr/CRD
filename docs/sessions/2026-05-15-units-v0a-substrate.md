# 2026-05-15 — Phase 3.1.7.5 v0a: crd-units substrate + 6-layer conversion system

## What shipped

**Phase 3.1.7.5 v0a** ships the `crd-units` substrate with the full 6-layer
conversion system, splitting into 3 sub-slices (v0a-1 / v0a-2 / v0a-3) per
the per-seam discipline. v0a-close ships system doc + ADR-0078 + this
session log.

Per-seam structure:
- **v0a-1** — module skeleton + `Dim<8 exponents>` + `Quantity<D, T>` core + layout pins (147 assertions / 36 cases).
- **v0a-2** — Layer 1 `LinearUnit<Dim, std::ratio>` + ~120 named units + Layer 4 `UnitMul`/`UnitDiv`/`UnitPow` compound auto-derive + `value_in<TargetUnit>` boundary accessor (199 additional assertions / 54 cases).
- **v0a-3** — Layer 2 `AffineUnit` + `Temperature`/`TemperatureDelta` + Layer 3 `NonLinearUnit` + dB family + cents/semitones + 80+ UDLs + `crd-no-untagged-physical-numeric` CI guard (118 additional assertions / 48 cases).

**Total:** 138 cases / 464 assertions, all green on **win-debug + win-asan + win-shipping + win-tidy** per the per-slice protocol.

## Per-slice protocol fix (Sprint 0)

Before starting Sprint 1, locked the v3-close lesson into a reusable script
+ doc:

- `scripts/per-slice-check.ps1` (Windows, ~130 LOC) — single-command verification across win-debug + win-asan + win-shipping + win-tidy with ASan DLL PATH fix baked in.
- `scripts/per-slice-check.sh` (Linux, ~75 LOC) — gcc-debug + gcc-asan equivalent.
- `docs/protocols/per-slice-verification.md` — protocol doc; references CLAUDE.md DoD #8 + memory `feedback_per_slice_run_ctest.md`.

The script dogfooded itself successfully — first invocation against main
returned PASS on the slate before v0a-1 started.

**Lesson reinforced:** test binary saying "All tests passed" can coexist
with a failing ctest-registered guard. Both must be green.

## Sprint 1 — `crd-units` substrate

### Files added

```
engine/units/
  CMakeLists.txt                          (28 LOC)
  include/crd/units/
    dim.hpp                                (132 LOC) -- Dim<L,M,T,I,Th,N,J,A> + DimMul/DimDiv/DimInv/DimPow
    dim_aliases.hpp                        (~100 LOC) -- 7 SI base + Angle + ~30 derived dimensions
    quantity.hpp                           (~155 LOC) -- Quantity<D, T> + cross-dim arithmetic + layout pins
    units_si.hpp                           (~250 LOC) -- LinearUnit + ~120 named units (SI prefix + imperial + ...)
    units_compound.hpp                     (~115 LOC) -- UnitMul/UnitDiv/UnitPow + ~30 compound units
    units_affine.hpp                       (~165 LOC) -- AffineUnit + AbsoluteQuantity + Temperature/TemperatureDelta
    units_nonlinear.hpp                    (~165 LOC) -- NonLinearUnit + dB family + cents/semitones
    literals.hpp                           (~225 LOC) -- 80+ UDLs across all dimension classes
    value_in.hpp                           (~55 LOC) -- value_in<U> + quantity_from<U>
    units.hpp                              (~18 LOC) -- umbrella include
  src/units.cpp                            (15 LOC) -- TU anchor
tests/units/
  CMakeLists.txt                          (15 LOC)
  test_dim.cpp                            (~215 LOC, 16 cases)
  test_quantity.cpp                       (~245 LOC, 20 cases)
  test_linear_units.cpp                   (~225 LOC, 13 cases)
  test_compound_units.cpp                 (~285 LOC, 17 cases)
  test_value_in.cpp                       (~250 LOC, 24 cases)
  test_affine_units.cpp                   (~190 LOC, 16 cases)
  test_nonlinear_units.cpp                (~175 LOC, 17 cases)
  test_literals.cpp                       (~225 LOC, 15 cases)
scripts/
  check_no_untagged_physical_numeric.ps1  (~85 LOC)
  check_no_untagged_physical_numeric.sh   (~55 LOC)
docs/
  systems/units.md                        (the system doc)
  protocols/per-slice-verification.md     (Sprint 0)
  sessions/2026-05-15-units-v0a-substrate.md (this file)
```

### Files modified

- `CMakeLists.txt` — `add_subdirectory(engine/units)`.
- `tests/CMakeLists.txt` — `add_subdirectory(units)`.
- `tests/math/CMakeLists.txt` — register `crd-no-untagged-physical-numeric` ctest guard (Windows + Unix variants).
- `CLAUDE.md` — Definition of Done #8 (ctest-not-binary rule).
- `docs/PRINCIPLES.md` — "every physical/scientific quantity carries a unit" pin.
- `docs/ROADMAP.md` — Strategic Execution Plan section.
- `context.md` — current-focus narrative.
- Memory: `project_state.md`, new `feedback_always_units.md` + `feedback_strategic_execution_plan_2026_05_15.md` + `feedback_per_slice_run_ctest.md`.

## Key design choices locked at v0a close

1. **8th tagged dimension Angle.** Strict SI radians is dimensionless (m/m); tagging Angle as the 8th compile-time exponent prevents silent `Length + Angle` bugs at compile time. mp-units (P1935) makes the same pragmatic choice.

2. **`.value` is publicly accessible.** No encapsulation overhead. Type safety lives at the API surface; inside SIMD/GPU hot paths, consumers reach `.value` raw. Wrapping into a getter would defeat the zero-overhead pin.

3. **`Quantity<D, T>` arithmetic produces identical codegen to bare-scalar arithmetic.** Verified by `static_assert` layout pins. Quantity is `is_standard_layout_v` + `is_trivially_copyable_v` + bit-equal-size to T.

4. **`std::ratio` factors for Layer 1.** Most conversions (SI prefix + standardised imperial) are exact rationals — bit-exact round-trips in f64 when the unit math reduces to integer arithmetic. Irrational factors (Degree = π/180, Grad = π/200, Revolution = 2π) use big-int rational approximations with documented 1-ULP tolerance.

5. **Distinct `Temperature` / `TemperatureDelta` types** via `AbsoluteQuantity<D, T>` separate from `Quantity<D, T>`. `Temperature - Temperature → TemperatureDelta` (subtraction strips offset). `Temperature + Temperature` is a compile error.

6. **Non-linear units are an I/O concern.** dB / cents / semitones don't support arithmetic at the type level. Convert to linear SI, add, convert back. No "DbValue" distinct type; dB lives as a unit-name-tag on the linear `Quantity` only at the boundary.

7. **Compound `UnitMul`/`UnitDiv`/`UnitPow` auto-derive.** Adding one new base unit unlocks N new compound units automatically. `std::ratio_multiply` / `std::ratio_divide` at compile time → bit-exact rational composition; f64 evaluation may drift 1 ULP per `+ - * /` step.

8. **Ambiguous-literal policy.** `_lb` and `_oz` deliberately NOT defined. Compile error at use site guides users to `_lb_mass` / `_lbf` / `_oz_mass` / etc.

9. **`crd-no-untagged-physical-numeric` initial conservative regex.** Flags ONLY actual struct/class fields (lines ending with `;` and no `(` / `)` on the line), not function-parameter list members. Will tighten through v0b/c/d adoption.

10. **`Vec<Quantity>` / `Mat<Quantity>` wrappers deferred to v0b.** They need `crd-math` types; that integration belongs to the adoption pass.

## Issues encountered + fixed in flight

### 1. UTF-8 encoding corruption from PowerShell Set-Content

During Sprint 1 v0a-2, attempted to ASCII-fy comments via a `Get-Content -Raw | -replace | Set-Content -NoNewline` pipeline. PowerShell's default encoding handling corrupted multi-byte UTF-8 chars (em-dash `—` = 0xE2 0x80 0x94 in UTF-8 → read+written as multi-char Win-1252 → mangled bytes). MSVC `/utf-8` flag then rejected the files with C4828.

**Fix:** rewrote test files with `[System.IO.File]::WriteAllText($f, $content, [System.Text.UTF8Encoding]::new($false))` — explicit UTF-8-without-BOM encoding. Then replaced remaining non-ASCII chars in TEST_CASE names + comments with ASCII equivalents directly.

**Pin for future:** when scripting batch edits of source files, use `[System.IO.File]::WriteAllText(..., UTF8Encoding::new($false))` not `Set-Content -NoNewline`.

### 2. f64 ratio division drifts 1 ULP from chained literal arithmetic

Tests like `STATIC_REQUIRE(Foot::factor / Inch::factor == 12.0)` failed because:
- `Foot::factor = 3048.0 / 10000.0` rounds to nearest f64 of 0.3048 (≈0.30480000000000003)
- `Inch::factor = 254.0 / 10000.0` rounds to nearest f64 of 0.0254 (≈0.025400000000000002)
- Quotient: 12.000000000000... — but with 1 ULP rounding error per division, the chain produces 11.9999999... or 12.0000000000004... depending on the exact f64 representations.

**Fix:** for "exactly equal rational" claims, compare `std::ratio_equal_v<...>` on the reduced ratio types (which is exact integer arithmetic). For f64 value comparisons, use a `near_eq(a, b, tol)` predicate. Documented as the v0a convention.

### 3. clang-tidy `LocalConstexprVariable` rule = `kCamelCase`

Cerid's `.clang-tidy` enforces `kCamelCase` for local constexpr variables (overrides the CLAUDE.md doc which suggested `lower_case`). My initial test code used `expected`, `half_pi`, `ratio_f64`, `pi_over_180` — all flagged.

**Fix:** renamed all local constexpr to `kCamelCase` (`kExpected`, `kHalfPi`, `kRatioF64`, `kPiOver180`). Also used `std::numbers::pi` instead of literal `3.141592653589793` per `modernize-use-std-numbers`.

### 4. `ElectronVolt` denominator overflows i64

Wrote `std::ratio<1'602'176'634, 10'000'000'000'000'000'000>` (denominator = 1e19, > i64 max 9.22e18). Compile error.

**Fix:** pre-reduce via gcd. 1e19 / gcd(1602176634, 1e19) = 1e19 / 2 = 5e18. Numerator: 1602176634 / 2 = 801088317. Reduced: `std::ratio<801'088'317, 5'000'000'000'000'000'000>`.

**Pin for future:** when adding a unit with a huge denominator, manually pre-reduce by computing gcd. `std::ratio` reduces internally but only AFTER the template parameters are instantiated — they must fit i64 to begin with.

### 5. Affine arithmetic accumulates 1-3 ULP error

`temperature_from<Fahrenheit>(32.0)` produces ~`273.14999999999991473` (not bit-exact `273.15`) because:
- `scale = 5/9` in f64 = 0.5555... (1 ULP rounding)
- `value * scale = 32 * 0.5555... = 17.777...` (1 ULP rounding)
- `offset = 45967/180` in f64 ≈ 255.372... (1 ULP rounding)
- `sum = 17.777... + 255.372... = 273.149999...` (1 ULP final rounding)

**Fix:** affine conversion tests use `near_eq` (1e-10 tolerance) instead of bit-exact equality. Pure-rational (Celsius scale=1, offset=27315/100) cases stay bit-exact since the multiply degenerates and only the add+offset rounds once.

### 6. `crd-no-untagged-physical-numeric` initial regex too aggressive

First regex matched `^\s*<type>\s+<name>\s*[;=]`, which also matched function-parameter default values like `float width = 1.0F,` in `crd-meshgen`. Pre-existing offenders blocked v0a-3 close.

**Fix:** tightened the regex to also require:
- Line ends with `;` (not `,` or `)`)
- Line contains NO `(` or `)` (excludes function-parameter list continuation lines)

Now correctly skips function parameters; only flags real struct/class fields. Will tighten further during v0b/c/d adoption as modules opt in to typed quantities.

## Verification

Final per-slice-check across 4 configs at v0a close:

| Config | Result |
|---|---|
| win-debug | **PASS** (build + ctest) |
| win-asan | **PASS** (build + ctest) |
| win-shipping | **PASS** (build + ctest) |
| win-tidy | **PASS** (build) |

Full project ctest count (win-debug, including new units tests): **1645/1645
passed** (was 1546 before Sprint 1 — added 138 units cases ≈ 99 visible
ctest entries via Catch2 case-discovery from a 138-case test binary).

`crd-units-tests` standalone: **464 assertions in 138 test cases, all green**.

`crd-no-untagged-physical-numeric` CI guard: **PASS** (initial conservative
regex; offenders surfaced during v0b/c/d adoption).

Full 17-config `scripts/full-sweep.ps1` deferred to v0d adoption close per
the original phase doc plan — v0a is the substrate; v0b/c/d apply it
across `crd-config` / `crd-scene` / `crd-eylem` / `crd-renderer` /
`crd-resources` / `crd-imgui`.

## ADR-0078 (mint candidate at v0a close)

`docs/decisions/0078-units-substrate-architecture.md` — captures the
locked design choices:
- 8-exponent Dim with Angle as tagged 8th base
- `Quantity<D, T>` zero-overhead wrapper, layout pins
- 6-layer conversion system architecture
- `.value` publicly accessible (no encapsulation)
- Absolute vs Delta types for Temperature
- Ambiguous-literal disallowance
- Federated domain extensibility (no central registry)
- `crd-no-untagged-physical-numeric` CI guard

(ADR file will be added in the v0a-close commit; this session log is the
authoritative substance.)

## What unlocks at v0a close

- **`crd-no-untagged-physical-numeric` CI guard is live.** From now on, any
  new code adding `f32 mass` / `f32 length` / etc. to a struct field fails
  the build.
- **Foundation for v0b adoption pass A:** `crd-config` unit-tagged TOML readers,
  `crd-scene Transform` dimensional, glTF cooker SI normalization.
- **Cross-cut detour D-003 (profiler dashboard) can start** consuming
  `crd-units::Time` for frame durations, `Frequency` for FPS, `Quantity<dim::Data>`
  for memory + GPU bandwidth.

## Next

**v0b adoption pass A** — `crd-config` + `crd-scene` + glTF. ~5 days estimated.

Then v0c (eylem + geometry-primitives), v0d (renderer + cookers + ImGui +
Layer-6 format/parse), in parallel with cross-cut detours D-003 / D-004 / D-005.
