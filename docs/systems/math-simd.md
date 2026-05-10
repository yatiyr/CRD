# `crd::math::simd` — SIMD + AoSoA storage substrate

**Module:** `crd-math` (sub-namespace `crd::math::simd`).
**Shipped:** Phase 3.1 v0a (2026-05-10) — SIMD wrapper types;
v0b (2026-05-10) — AoSoA storage container + iteration + gather/scatter.
**Specs:** ADR-0063 (deterministic FP contract), ADR-0065 (`crd-hesap`
substrate that consumes these as BLAS L1 primitives).
**Plan:** `docs/phases/phase-3.1-eylem.md` v0 table.
**Closing sessions:** `docs/sessions/2026-05-10-v0a-simd-substrate.md`,
`docs/sessions/2026-05-10-v0b-soa-substrate.md`.

## What this is

The lane-level SIMD primitives the AoSoA-N hot paths in eylem (Phase 3.1)
and `crd-hesap` (Phase 3.1.6) are built on, **plus** the typed AoSoA
container (`Soa<TChunk, Lane>`) that holds them. Five types live in
`crd::math::simd`:

| Type   | Lanes          | Storage                                                                                          | Alignment |
|--------|----------------|--------------------------------------------------------------------------------------------------|----------:|
| `Vec4f` | 4 × `f32`     | `__m128` (SSE2/AVX2) · `float32x4_t` (NEON) · `f32[4]` (scalar)                                  | 16 B |
| `Vec8f` | 8 × `f32`     | `__m256` (AVX2) · `Vec4f lo,hi` composed (SSE2/NEON/scalar)                                       | 32 B |
| `Mat4f` | 4 × `Vec4f`   | column-major; `cols[4]`                                                                          | 16 B |
| `Quatf` | 1 × `Vec4f`   | `(x, y, z, w)` packed; Hamilton convention                                                       | 16 B |
| `Soa<TChunk, Lane>` | `Lane` per chunk × N chunks | `Array<TChunk>` (chunk-aligned grow) + logical-size counter | inherits `alignof(TChunk)` |

These are NOT the user-facing math types. `crd::math::Vec4f` (in
`crd/math/vec.hpp`) is the 4-component math vector — semantic data.
`crd::math::simd::Vec4f` is the SIMD lane primitive — packed compute.
Different namespaces, different jobs.

## Backend selection

Compile-time via `CRD_SIMD_TARGET` macro (defined by
`cmake/CrdSimd.cmake` on the `crd-simd-flags` interface target,
propagated PUBLIC through `crd-math`).

| `CRD_SIMD_LEVEL` | Resolves to | `Vec4f` storage | `Vec8f` storage | Compile flag |
|---|---|---|---|---|
| `auto` (x64 host) | `avx2` | `__m128` | `__m256` (native) | MSVC `/arch:AVX2` · GCC/Clang `-mavx2 -msse4.2` |
| `auto` (ARM64 host) | `neon` | `float32x4_t` | composed Vec4f×2 | (NEON is baseline on AArch64) |
| `auto` (other) | `scalar` | `f32[4]` | `f32[8]` | (none) |
| `avx2` | `avx2` | as above | as above | as above |
| `sse2` | `sse2` | `__m128` | composed Vec4f×2 | (none on MSVC; `-msse2` on GCC/Clang) |
| `neon` | `neon` | as above | as above | (none) |
| `scalar` | `scalar` | as above | as above | (none) |
| `native` | host best | per arch | per arch | MSVC `/arch:AVX2` · GCC/Clang `-march=native` |

CMake refuses invalid combos (`-DCRD_SIMD_LEVEL=avx2` on ARM64 etc.) at
configure time with a `FATAL_ERROR`.

## Determinism contract (ADR-0063)

`crd-simd-flags` enforces three things by default
(`CRD_DETERMINISTIC_FP=ON` in every preset):

1. **No fast-math.** MSVC `/fp:precise`; GCC/Clang `-fno-fast-math
   -ffp-contract=off`. Forbids the compiler from contracting `(a*b)+c`
   into a hardware-FMA single rounding, reordering ops by associativity,
   or assuming no NaN/Inf.
2. **No x87.** GCC/Clang `-mfpmath=sse` on x64; MSVC x64 ABI is SSE2 by
   default. Eliminates 80-bit intermediate-precision drift.
3. **No FMA hardware unit even when available.** `mul_add(a, b, c)`
   compiles to two operations (mul + add) with two roundings on every
   backend, NOT `_mm_fmadd_ps`. Backend parity beats the small
   throughput gain.

Reductions (`horizontal_sum`, `dot`) use a fixed pairwise binary tree:

- `Vec4f::dot(a,b) = (a0*b0 + a1*b1) + (a2*b2 + a3*b3)`
- `Vec8f::dot(a,b) = ((s01+s23) + (s45+s67))` where `sij = ai*bi + aj*bj`

Every backend extracts lanes and sums in this order so SIMD/scalar agree
bit-for-bit. The `[determinism]` test tag in `tests/math/test_simd.cpp`
pins each rule with a dedicated test case.

## Operator surface

Each of `Vec4f`/`Vec8f` exposes (with `CRD_FORCEINLINE`):

```cpp
// Construction
Vec4f();                                // default-uninitialized
explicit Vec4f(f32 broadcast);
Vec4f(f32 e0, f32 e1, f32 e2, f32 e3);
static Vec4f zero();
static Vec4f one();
static Vec4f load(const f32* p);          // unaligned
static Vec4f load_aligned(const f32* p);  // 16 B aligned

// Storage / lane access
void store(f32* p) const;
void store_aligned(f32* p) const;
f32  lane(usize i) const;

// Lane-wise arithmetic
operator+(Vec4f, Vec4f)  operator-(Vec4f, Vec4f)  operator*(Vec4f, Vec4f)
operator/(Vec4f, Vec4f)  operator-(Vec4f) /* negate */
operator*(Vec4f, f32)    operator*(f32, Vec4f)    operator/(Vec4f, f32)

// FMA-shaped (two-rounding, ADR-0063 compliant)
mul_add(Vec4f a, Vec4f b, Vec4f c)  // (a*b) + c
mul_sub(Vec4f a, Vec4f b, Vec4f c)  // (a*b) - c

// Lane-wise min/max/abs/clamp
min(Vec4f, Vec4f)  max(Vec4f, Vec4f)  abs(Vec4f)  clamp(Vec4f, lo, hi)

// IEEE sqrt (correctly rounded, hardware)
sqrt(Vec4f)

// Reductions (deterministic pairwise tree)
horizontal_sum(Vec4f) -> f32
dot(Vec4f, Vec4f)     -> f32

// Mask-producing comparisons + branchless select
cmp_lt / cmp_le / cmp_eq / cmp_gt / cmp_ge
select(mask, true_v, false_v)
```

Vec8f mirrors this surface 1:1 (8-lane variants of the same ops; on
SSE2/NEON/scalar paths, every op is just `Vec4f` applied to `lo` and
`hi`).

`Mat4f` adds: `identity()`, `zero()`, `load_column_major(p)`,
`store_column_major(p)`, `element(row, col)`, `+ - operator*`,
matrix-scalar `*`, **matrix×vector** (`Mat4f * Vec4f`), **matrix×matrix**
(`Mat4f * Mat4f`), `transpose`, `transform_point(x,y,z)`,
`transform_vector(x,y,z)`.

`Quatf` adds: `identity()`, `x()`/`y()`/`z()`/`w()` accessors,
**Hamilton product** (`Quatf * Quatf`), `+ -`, scalar-product, `conjugate`,
`dot(Quatf, Quatf)`, `length`, `normalize`, **rotate vector**
(`rotate(q, vx, vy, vz) -> Vec4f`).

## Introspection

Two compile-time-known helpers:

```cpp
crd::math::simd::backend_name()      // -> "AVX2" / "SSE2" / "NEON" / "SCALAR"
crd::math::simd::deterministic_fp()  // -> bool; true under ADR-0063 contract
crd::math::simd::k_native_lane_width // -> 8 (AVX2) or 4 (others)
crd::math::simd::k_vec4f_lanes       // -> 4
crd::math::simd::k_vec8f_lanes       // -> 8
```

These let smokes / tests / startup logs verify what the binary was
actually compiled with — not just what the configure summary advertised.

## CI guardrails

Three layers of regression catch:

1. **Configure summary** prints `CRD_SIMD_LEVEL`, `CRD_SIMD_TARGET`, and
   `DETERMINISTIC_FP` on every reconfigure. Visual smoke test for what
   you're about to build.

2. **Bit-exact parity tests** (`tests/math/test_simd.cpp`, 30 cases /
   148 assertions). Every Vec4f/Vec8f op compares against an explicit
   scalar reference in the canonical reduction order. Tests run on every
   preset; same pass/fail on `win-debug` (AVX2) and `win-debug-scalar`
   (scalar) proves the parity contract.

3. **`crd-simd-emission-check` CTest test** (driven by
   `scripts/check_simd_emission.{ps1,sh}`). Disassembles the
   `test_simd.cpp` obj via `dumpbin /disasm` (Windows) or `objdump -d`
   (Linux) and asserts the actual emitted machine code matches
   `CRD_SIMD_LEVEL_RESOLVED`:

   | Resolved level | Pass condition |
   |---|---|
   | `avx2` | obj contains ≥1 `vaddps/vmulps/...ps ymm` instruction |
   | `sse2` | obj contains zero `ymm` references |
   | `scalar` | obj contains zero `ymm` references |
   | `neon` | (skipped — ARM disasm parity check not implemented) |
   | (LTCG/LTO obj — IL-only) | (auto-skipped; covered by non-LTCG sibling configs) |

   Catches the class of bugs where the CMake plumbing silently stops
   passing `/arch:AVX2` (the real bug found in v0a's audit).

## Integration with downstream substrates

| Module | Uses |
|---|---|
| `crd-math` (existing scalar types) | unchanged; SIMD types are sibling additions |
| **`crd-eylem` (Phase 3.1 v1+)** | Vec8f for AoSoA-8 body state hot path; Mat4f for transforms; Quatf for body orientation |
| **`crd-sdf` (Phase 3.1.5)** | Vec8f for parallel voxel sampling in the bake step; Vec4f for per-voxel gradient |
| **`crd-hesap` (Phase 3.1.6 v0)** | Vec4f / Vec8f as BLAS L1 vector primitive; pairwise reduction tree reused for `dot`/`nrm2`/`asum` to preserve cross-platform numerical reproducibility |
| **`crd-renderer`** | (no migration needed — renderer uses scalar `crd::math::Mat4f` for one-off transforms; SIMD is for batched / per-element work) |

## AoSoA container — `Soa<TChunk, Lane>` (v0b)

Typed AoSoA storage. User defines `TChunk` explicitly as a struct of
`Vec8f` (or `Vec4f`) columns; `Soa` stores `Array<TChunk>` with chunk-
aligned growth and tracks the logical entity count separately.

```cpp
// User-defined chunk: 8 logical entities per chunk, 7 columns.
struct alignas(32) BodyChunk8
{
    Vec8f pos_x, pos_y, pos_z;
    Vec8f vel_x, vel_y, vel_z;
    Vec8f inv_mass;
};

Soa<BodyChunk8, 8> bodies(world_allocator);
bodies.resize(world_body_count);   // rounds chunk array up to ceil(n/8)

// SIMD path: lambda gets (chunk&, active_lane_count). Last chunk's
// active count is 1..Lane for partial tail; Lane for full chunks.
soa_for_each_chunk(bodies, [&](BodyChunk8& c, usize active)
{
    c.pos_x = mul_add(c.vel_x, dt_v8, c.pos_x);
    c.pos_y = mul_add(c.vel_y, dt_v8, c.pos_y);
    c.pos_z = mul_add(c.vel_z, dt_v8, c.pos_z);
});

// Cross-chunk gather/scatter for graph-coloured constraint solvers etc.
const u32 idx[8] = { 0, 7, 8, 15, 1, 6, 9, 14 };
const Vec8f vx_a = gather8(bodies, &BodyChunk8::vel_x, idx);
// ... compute impulse ...
scatter8(bodies, &BodyChunk8::vel_x, idx, new_vx);
```

**API surface:**

```cpp
template <typename TChunk, usize Lane = k_native_lane_width>
class Soa {
    static constexpr usize lanes_per_chunk = Lane;
    explicit Soa(IAllocator* = default_allocator());
    void  resize(usize logical);                 // chunk array → ceil(n/Lane)
    void  reserve(usize logical);
    void  clear();
    usize size()        const;                   // logical count
    usize chunk_count() const;
    bool  empty()       const;
    usize last_chunk_active_lanes() const;       // 0 / 1..Lane
    TChunk&       chunk(usize i);                // SIMD path
    const TChunk& chunk(usize i) const;
    Span<TChunk>      chunks();
    ConstSpan<TChunk> chunks() const;
    static constexpr usize chunk_of (usize global_idx);
    static constexpr usize lane_of  (usize global_idx);
    static constexpr usize make_index(usize chunk_idx, usize lane_idx);
};

template <Fn> void soa_for_each_chunk(Soa&, Fn&&);  // SIMD path
template <Fn> void soa_for_each_chunk(const Soa&, Fn&&);
template <Fn> void soa_for_each_lane (Soa&, Fn&&);  // slow path (one call per logical entity)
template <Fn> void soa_for_each_lane (const Soa&, Fn&&);

// Cross-chunk gather/scatter — 8 lanes for Vec8f columns, 4 for Vec4f.
Vec8f gather8 (const Soa&, Vec8f TChunk::* member, const u32 (&)[8]);
void  scatter8(      Soa&, Vec8f TChunk::* member, const u32 (&)[8], Vec8f);
Vec4f gather4 (const Soa&, Vec4f TChunk::* member, const u32 (&)[4]);
void  scatter4(      Soa&, Vec4f TChunk::* member, const u32 (&)[4], Vec4f);
```

**Pinned closed set:**
- `Lane` ∈ {4, 8} (static_assert refuses other widths; AVX-512 width 16
  reserved until AVX-512 becomes a Cerid CI target).
- `TChunk` must be `alignof >= 16`.
- Gather/scatter is software (extract-and-pack); hardware
  `_mm256_i32gather_ps` reserved for v0e benchmark-driven optimisation.

## Files

```
engine/math/
├── include/crd/math/simd/
│   ├── backend.hpp     (~120 LOC) — selection + introspection
│   ├── vec4f.hpp       (~250 LOC) — 4-lane f32
│   ├── vec8f.hpp       (~190 LOC) — 8-lane f32
│   ├── mat4f.hpp       (~140 LOC) — 4×4 matrix
│   ├── quatf.hpp       (~85  LOC) — quaternion
│   ├── soa.hpp         (~240 LOC) — AoSoA container + iteration + gather/scatter
│   └── simd.hpp        (~17  LOC) — umbrella
└── (existing scalar headers untouched)

cmake/
└── CrdSimd.cmake      — CRD_SIMD_LEVEL + CRD_DETERMINISTIC_FP + crd-simd-flags target

scripts/
├── check_simd_emission.ps1   — Windows CI guard (uses dumpbin)
└── check_simd_emission.sh    — Linux CI guard (uses objdump)

tests/math/
├── test_simd.cpp     (~370 LOC, 30 cases / 148 assertions)
└── test_soa.cpp      (~290 LOC, 19 cases / 345 assertions)
```

## When this changes

- **v0b (next slice)** — adds `Soa<T, N>` AoSoA storage helpers + gather/
  scatter + `soa_for_each` lambda iterator that auto-utilises lanes via
  `k_native_lane_width`. No changes to v0a's surfaces; only consumer-side
  additions.
- **v0c–v0e** — deterministic stdlib substitutions, deterministic sort,
  benchmark harness. Independent of the SIMD types themselves.
- **Eylem v1+** — first big consumer; will surface any operator-surface
  gaps (e.g. swizzle, blend, gather) that v0a deliberately deferred.
  Add them when the demand is concrete.
- **`crd-hesap` v0a (Phase 3.1.6)** — wraps these primitives as BLAS L1
  ops (`axpy`, `dot`, `nrm2`, `scal`...). Will likely add `Vec4d` /
  `Vec8d` (`f64` doubles) at that point — currently `f32` only.

## Reserved (not v0a work)

- `Vec4d` / `Vec8d` (f64 SIMD) — wait for the first `f64` consumer
  (likely `crd-hesap-dense` for double-precision LA).
- `Vec16f` (AVX-512) — wait for AVX-512 to be a Cerid CI target.
- Hardware FMA (`fma_single_rounding`) — explicit opt-out of the
  deterministic contract for the (rare) case where it's profiled
  worth it. Leave the door open via a separate API name.
- `rsqrt` (approximate reciprocal sqrt) — non-deterministic across CPUs;
  add only with a Newton iteration step that closes the gap.
- Swizzle / shuffle / blend / gather / scatter — add when eylem or hesap
  surfaces a concrete need; resist speculative API growth.
