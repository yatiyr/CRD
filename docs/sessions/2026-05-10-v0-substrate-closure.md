# 2026-05-10 — Phase 3.1 v0 substrate — full closure dossier

> **CORRIGENDUM (2026-05-10 evening, after this dossier was first
> written):** the "12-config sweep clean throughout" line below was
> based on bench-target incremental builds. A full Definition of Done
> sweep run hours later surfaced LNK1257 (C4714 LTCG drift in 3
> release-class configs) and a Quatf #151 ctest mojibake (`°` in a
> UTF-8 test name vs Windows ACP argv) in 4 configs. Both fixed,
> verified, and codified into a CI guard
> (`crd-no-non-ascii-test-names`) + global `/wd4714` on
> `crd-warnings`. v0 substrate is *now* genuinely clean across all 14
> build steps (Win × 8 + Linux × 6). New developer tool
> `scripts/full-sweep.ps1` runs the whole DoD sweep in one command —
> use it, not bench-target sweeps, when verifying slice closure. Full
> story: `docs/sessions/2026-05-10-v0-postmortem-c4714-and-utf8-argv.md`.

**This is the comprehensive closure document for Phase 3.1 v0.** Six
slices shipped same day (v0a, v0b, v0c, v0c-debt-A, v0d, v0e), totalling
~3600 LOC of substrate code + ~131 test cases / ~1748 functional
assertions + 4 performance benchmarks across `crd-math` + `crd-containers`
+ `tests/bench`. **14 build steps green** (Win × 8 + Linux × 6) after
the 2026-05-10-evening post-mortem fixes. **Four CI guards live**
(`crd-simd-emission-check`, `crd-no-std-math-check`,
`crd-no-std-sort-check`, `crd-no-non-ascii-test-names`).

This doc is intentionally long. It captures (a) the inventory, (b) the
*why* behind every architectural choice, (c) bugs surfaced + their fixes,
(d) measured performance numbers, (e) what's reserved for later +
where, (f) pitfalls to avoid when extending, and (g) the v1a entry
point. A future contributor or AI agent reading only this file should be
able to reconstruct the full mental model of the v0 substrate.

---

## Table of contents

1. [What v0 is and why](#1-what-v0-is-and-why)
2. [Inventory — every type, function, file](#2-inventory--every-type-function-file)
3. [The six slices, in order](#3-the-six-slices-in-order)
4. [Determinism contract — how it's enforced](#4-determinism-contract--how-its-enforced)
5. [SIMD architecture + backend selection](#5-simd-architecture--backend-selection)
6. [Build system additions](#6-build-system-additions)
7. [CI guards](#7-ci-guards)
8. [Performance — measured numbers](#8-performance--measured-numbers)
9. [Bugs surfaced + fixed during v0](#9-bugs-surfaced--fixed-during-v0)
10. [What's deferred + where it lives](#10-whats-deferred--where-it-lives)
11. [Pitfalls + extension notes](#11-pitfalls--extension-notes)
12. [What v1a inherits + where to start](#12-what-v1a-inherits--where-to-start)

---

## 1. What v0 is and why

Phase 3.1 v0 is the **mathematical + container substrate** that every
later eylem (Phase 3.1 v1+), `crd-sdf` (Phase 3.1.5), and `crd-hesap`
(Phase 3.1.6) slice depends on. It exists because:

- **Eylem v9b's 9-config replay-hash CI** demands bit-exact
  cross-platform reproducibility. C-runtime libm differs across
  Microsoft CRT / glibc / Apple vecLib / Bionic for `sin` / `cos` /
  `exp` / `log` etc., so we need our own deterministic transcendentals.
- **Physics is fan-out parallel.** The hot loops process bodies in
  batches (broadphase pair tests, SI solver, narrow-phase dispatch).
  Without SIMD primitives the v1 rigid 3D substrate would leave 4–8×
  throughput on the floor.
- **STL containers + algorithms aren't deterministic across libc++ /
  libstdc++ / Microsoft STL** in their tie-breaking for equal keys.
  Eylem replay-hash CI catches that as nondeterminism even though the
  individual implementations are correct.

v0 ships the substrate; v1 starts using it.

The phase plan said v0 should ship in ~1.5 weeks across 5 slices. The
actual scope grew because we paid v0c's deferred debt the same day
(v0c-debt-A — 5 items totalling ~1100 LOC), but the timeline collapsed
to **a single intense session** by writing the documents-first +
sweeping-each-slice discipline, then iterating fast on the CI feedback.

---

## 2. Inventory — every type, function, file

### Public surfaces (what consumers actually call)

#### `crd::math::simd::*` — SIMD wrapper types
- `Vec4f` — 4-lane f32; SSE2 / NEON / scalar. Full operator set: `+ - * /` +
  `mul_add` (two-rounding, no hardware FMA — see ADR-0063), `min` / `max` /
  `abs` / `clamp` / `sqrt` (IEEE-correct), `horizontal_sum` / `dot`
  (deterministic pairwise tree), `cmp_lt` / `le` / `eq` / `gt` / `ge`,
  `select`, `load` / `load_aligned` / `store` / `store_aligned`, `lane(i)`.
  Header `engine/math/include/crd/math/simd/vec4f.hpp`.
- `Vec8f` — 8-lane f32; AVX2 native (`__m256`) or composed `Vec4f lo, hi`.
  Same operator surface as Vec4f. Header `vec8f.hpp`.
- `Vec4i` / `Vec8i` — integer companions. Arithmetic + bitwise (`& | ^`,
  `and_not`) + shifts (compile-time `shift_left<N>` /
  `shift_right_arith<N>` / `shift_right_logical<N>`) + comparisons
  (`cmp_eq`, `cmp_gt`). Headers `vec4i.hpp`, `vec8i.hpp`.
- `Mat4f` — 4×4 column-major using `Vec4f cols[4]`. Identity / zero /
  load_column_major / store_column_major / element / `+ -` / scalar `*` /
  matrix×vector / matrix×matrix / `transpose` / `transform_point` /
  `transform_vector`. Header `mat4f.hpp`.
- `Quatf` — quaternion using `Vec4f xyzw` storage. `identity` /
  `x() y() z() w()` / Hamilton product / `+` `-` / scalar `*` /
  `conjugate` / `dot` / `length` / `normalize` / `rotate(vx, vy, vz)`.
  Header `quatf.hpp`.
- `Soa<TChunk, Lane>` — typed AoSoA container. User defines
  `TChunk` explicitly (e.g., `struct alignas(32) BodyChunk8 { Vec8f
  pos_x, pos_y, pos_z; ... };`); Soa wraps `Array<TChunk>` with
  chunk-aligned grow + logical-vs-physical size tracking. Public:
  `resize`/`reserve`/`clear`, `size`/`chunk_count`/`empty`/
  `last_chunk_active_lanes`, `chunk(i)` / `chunks()`, static constexpr
  `chunk_of`/`lane_of`/`make_index`. Iteration:
  `soa_for_each_chunk` (SIMD path; lambda gets `(chunk&,
  active_lane_count)`) + `soa_for_each_lane` (slow per-entity).
  Cross-chunk: `gather8`/`scatter8` (Vec8f columns) +
  `gather4`/`scatter4` (Vec4f columns). Header `soa.hpp`.
- Conversion helpers: `bitcast_to_int(Vec4f) -> Vec4i`,
  `bitcast_to_float(Vec4i) -> Vec4f` and Vec8 variants;
  `convert_truncate(Vec4f) -> Vec4i` (toward-zero), `convert_to_float`
  (lossless for |i| < 2^24); `truncate(Vec4f) -> Vec4f`,
  `round_nearest(Vec4f) -> Vec4f` (rounds via current FPU mode =
  banker's rounding); `bit_and` / `bit_or` / `bit_xor` / `bit_andnot`
  on `Vec4f`/`Vec8f` (operate on float bit patterns). Header
  `convert.hpp`.
- `simd::backend_name() -> const char*` — `"AVX2"` / `"SSE2"` / `"NEON"` /
  `"SCALAR"`. `simd::deterministic_fp() -> bool` — true under ADR-0063
  contract (always true in current builds).
- Constants: `k_vec4f_lanes`, `k_vec8f_lanes`, `k_native_lane_width`
  (8 on AVX2, 4 elsewhere — drives AoSoA-N choice in eylem).

#### `crd::math::deterministic::*` — Cephes-style transcendentals
- f32 + f64 overloads of all of:
  - **Trig:** `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`
  - **Exp / log / pow:** `exp`, `exp2`, `log`, `log2`, `log10`, `pow`
  - **Cancellation-resistant near-zero:** `expm1`, `log1p`
  - **Hyperbolic:** `sinh`, `cosh`, `tanh`
  - **Special functions:** `erf`, `erfc`, `gamma`, `lgamma`, `beta`
  - **Rounding (IEEE-correct hardware ops):** `floor`, `ceil`, `trunc`,
    `round`, `fmod`, `abs`, `copysign`
- f64 implementations are the source of truth. f32 special functions
  (`erf` / `erfc` / `gamma` / `lgamma` / `beta`) forward to f64 +
  cast-down (preserves f32 ulp precision).
- **Vec4f / Vec8f branchless SIMD overloads** of `sin`, `cos`, `exp`,
  `log` — full Cephes algorithm done branchlessly via `select()` for
  octant routing + bitmask sign tracking + `convert_truncate` for
  argument reduction. AVX2 emits 256-bit `vaddps/vmulps ymm` for the
  inner polynomial.
- Constants: `pi`, `tau`, `pi_2`, `pi_4`, `inv_pi`, `inv_2pi`, `e`,
  `ln2`, `inv_ln2`, `ln10`, `inv_ln10` (and `*_64` doubles).

Header `engine/math/include/crd/math/deterministic.hpp`. Implementation
`engine/math/src/deterministic.cpp` (~2000 LOC).

#### `crd::containers::*` — deterministic sort + heap
- `sort(first, last, cmp = std::less<>{})` — merge sort (naturally stable +
  deterministic). Allocates O(N) auxiliary `Array<T>`. Requires T to be
  default-constructible.
- `stable_sort(first, last, cmp)` — same merge sort; bit-exact equivalent
  to `sort`. Kept as separate name for ABI flexibility (a faster
  non-stable variant could replace `sort` later).
- `nth_element(first, nth, last, cmp)` — quickselect with median-of-three
  pivot + insertion-sort fallback for partitions ≤ 16.
- `push_heap(first, last, cmp)` — sift-up from `end-1`.
- `pop_heap(first, last, cmp)` — swap `*begin` with `*(end-1)`, sift down
  `[begin, end-1)`.
- `make_heap(first, last, cmp)` — Floyd's bottom-up heapify (O(n)).
- `sort_heap(first, last, cmp)` — repeated `pop_heap`.

Header `engine/containers/include/crd/containers/sort.hpp`. ~280 LOC.

### Internal infrastructure (consumers shouldn't touch directly)

- `cmake/CrdSimd.cmake` — `CRD_SIMD_LEVEL` cache string + `CRD_DETERMINISTIC_FP`
  option + `crd-simd-flags` interface target that emits `/arch:AVX2` /
  `-mavx2` / etc. + `CRD_SIMD_TARGET=N` macro + `/fp:precise` /
  `-fno-fast-math -ffp-contract=off -mfpmath=sse` for determinism.
  `crd-math` links it PUBLIC; every transitive consumer inherits.
- `scripts/check_simd_emission.{ps1,sh}` — disasms `test_simd.cpp.obj`,
  asserts the SIMD level matches `CRD_SIMD_LEVEL_RESOLVED`. Auto-skips
  on LTCG/LTO IL-only objs.
- `scripts/check_no_std_math.{ps1,sh}` — bans `std::sin/cos/...` in
  `engine/eylem/**` + `engine/hesap/**`. Opt-out marker
  `// crd-lint-allow-std-math`.
- `scripts/check_no_std_sort.{ps1,sh}` — bans `std::sort/stable_sort/...`
  in same modules. Opt-out `// crd-lint-allow-std-sort`.
- `tests/bench/test_bench_simd.cpp` — Catch2-BENCHMARK micro-benchmarks
  for the four hot ops (Vec3f dot/cross, Mat4f mul, Quatf compose). Goal-line
  numbers + regression-investigation threshold inline. Out of CTest by
  design (timing is environment-dependent).

### File map

```
engine/math/
├── include/crd/math/
│   ├── deterministic.hpp           [v0c+debt-A]  Public scalar + SIMD math API
│   └── simd/
│       ├── backend.hpp             [v0a]  CRD_SIMD_TARGET decode + introspection
│       ├── vec4f.hpp               [v0a, v0c-debt-A] 4-lane f32 SIMD
│       ├── vec8f.hpp               [v0a, v0c-debt-A] 8-lane f32 SIMD
│       ├── vec4i.hpp               [v0c-debt-A] 4-lane i32 SIMD
│       ├── vec8i.hpp               [v0c-debt-A] 8-lane i32 SIMD
│       ├── convert.hpp             [v0c-debt-A] bitcast + numerical convert + bitwise
│       ├── mat4f.hpp               [v0a]  4×4 SIMD matrix
│       ├── quatf.hpp               [v0a]  SIMD quaternion
│       ├── soa.hpp                 [v0b]  AoSoA<TChunk, Lane> container
│       └── simd.hpp                [v0a]  umbrella header
└── src/
    └── deterministic.cpp           [v0c+debt-A]  Cephes f32+f64 + branchless SIMD math (~2000 LOC)

engine/containers/include/crd/containers/
└── sort.hpp                        [v0d]  sort / stable_sort / nth_element / heap ops

cmake/
└── CrdSimd.cmake                   [v0a]  SIMD level + determinism FP config

scripts/
├── check_simd_emission.{ps1,sh}    [v0a]  AVX2-emission CI guard
├── check_no_std_math.{ps1,sh}      [v0c]  std::sin etc. ban in eylem/hesap
└── check_no_std_sort.{ps1,sh}      [v0d]  std::sort etc. ban in eylem/hesap

tests/math/
├── test_simd.cpp                   [v0a]  30 cases — backend + Vec4f/Vec8f
├── test_soa.cpp                    [v0b]  19 cases — Soa + iteration + gather/scatter
├── test_deterministic.cpp          [v0c+debt-A]  41 cases — scalar f32+f64 + special + SIMD overloads
└── CMakeLists.txt                  [v0a, v0c, v0d]  Registers 3 CI guards as CTest

tests/containers/
└── test_sort.cpp                   [v0d]  14 cases — sort/heap

tests/bench/
└── test_bench_simd.cpp             [v0e]  4 SIMD benchmarks
```

---

## 3. The six slices, in order

### v0a — SIMD wrapper substrate (foundation)

**What:** `Vec4f` / `Vec8f` / `Mat4f` / `Quatf` SIMD types + backend
selection at compile time + `cmake/CrdSimd.cmake` + `crd-simd-flags`
interface target + bit-exact scalar-reference parity tests + the
`crd-simd-emission-check` CTest test that disassembles the obj to verify
the actual machine code matches `CRD_SIMD_LEVEL_RESOLVED`.

**Hidden value:** This slice surfaced **three real bugs that had been
silent in the codebase forever:**

1. **AVX2 was never actually used in any non-shipping config.** My
   initial backend.hpp checked `defined(__AVX2__)`, but MSVC never
   defines that unless `/arch:AVX2` is explicitly passed. Every
   `win-debug` / `win-release` / `win-asan` / `win-clang-cl` / `win-tidy`
   build had been silently falling back to SSE2. Fixed by routing
   backend selection through CMake's `CRD_SIMD_TARGET` macro
   (defined by `CrdSimd.cmake` on the `crd-simd-flags` interface
   target).

2. **`CRD_SHIPPING` enabled `/fp:fast` + `-ffast-math`,** violating
   ADR-0063 from day 1. That would have broken eylem v9b's replay-hash
   CI the moment shipping mode landed in production. Removed; replaced
   with the ADR-0063 contract enforced project-wide via `CrdSimd.cmake`.

3. **No way to verify SIMD/scalar bit-exact parity in CI** — added
   `win-debug-scalar` + `linux-gcc-debug-scalar` presets that force
   `CRD_SIMD_LEVEL=scalar`, plus the disasm CI guard.

**Closing session log:** `docs/sessions/2026-05-10-v0a-simd-substrate.md`.

### v0b — AoSoA storage substrate

**What:** `Soa<TChunk, Lane>` typed AoSoA container backed by
`Array<TChunk>`. User defines `TChunk` explicitly (no template
reflection magic); `Lane` defaults to `k_native_lane_width` (8 on AVX2,
4 elsewhere). Iteration helpers `soa_for_each_chunk` (SIMD path; lambda
gets `(chunk&, active_lane_count)`) + `soa_for_each_lane` (slow path).
Cross-chunk lane movers `gather8`/`scatter8` over `Vec8f TChunk::*`
member pointers (+ Vec4f variants).

**Pinned design choices:**
- No reflection magic; user defines `TChunk` explicitly — eylem code
  will write `struct alignas(32) BodyChunk8 { Vec8f pos_x, pos_y, pos_z;
  Vec8f vel_x, vel_y, vel_z; Vec8f inv_mass; };` by hand.
- `Lane` is closed: 4 or 8 only (static_assert refuses other widths).
  AVX-512 width 16 reserved.
- `size()` is logical, `chunk_count()` is physical;
  `last_chunk_active_lanes()` lets callers mask the partial tail.
- Software gather/scatter (extract-and-pack), not hardware
  `_mm256_i32gather_ps` — preserves the v0a determinism contract.
  Hardware-gather fast path reserved for v0e benchmark-driven optimisation.
- No swap-and-pop / hole-filling — reserved for eylem v1's actual
  entity-removal contract.

**Closing session log:** `docs/sessions/2026-05-10-v0b-soa-substrate.md`.

### v0c — `crd::math::deterministic` Cephes substrate

**What:** Bit-exact-cross-platform replacements for `std::sin` /
`std::cos` / `std::tan` / `std::asin` / `std::acos` / `std::atan` /
`std::atan2` / `std::exp` / `std::exp2` / `std::log` / `std::log2` /
`std::log10` / `std::pow` + IEEE-correct rounding wrappers (`floor` /
`ceil` / `trunc` / `round` / `abs` / `copysign` / `fmod`). Cephes-derived
coefficients per function (Stephen Moshier, public domain). Plus
`crd-no-std-math-check` CI guard banning `std::sin` etc. in
`engine/eylem/**` + `engine/hesap/**`.

**Two real bugs caught during implementation:**
1. atan polynomial used `-` operators between already-signed Cephes
   coefficients → double-negated p1/p3 → atan was 5% wrong → cascaded to
   atan2/asin/acos. Fixed by switching to all-`+` operators per Cephes
   convention.
2. sin/cos used `with_sign` (overwrite) where Cephes uses XOR-combine
   semantics → sign(-3π/4) wrong. Added `apply_sign` (XOR) helper, kept
   `with_sign` for true copysign use.

**Closing session log:** `docs/sessions/2026-05-10-v0c-deterministic.md`.

### v0c-debt-A — same-day debt paydown (5 items)

**Why it happened:** v0c shipped with 5 deferred items in `docs/debt.md`.
The user said "let's pay all the debt right now before v0d." So we did,
in one extended session.

**Items 1, 3, 5 — easy wins (~950 LOC):**
- Item 5: `expm1` / `log1p` (cancellation-resistant near-zero)
- Item 3: `sinh` / `cosh` / `tanh` (hyperbolic)
- Item 1: f64 overloads of all 26 functions (Cephes f64 coefficient
  tables; rational/Padé form for tan/atan/exp/log instead of
  polynomial)

**The f64 atan saga.** Initial copy of Cephes atan.c P-coefficients
gave `R(0) = -0.896` when the leading Taylor coefficient of atan(x) − x
should give `R(0) = -1/3`. Pragmatic fallback to f32 polynomial in f64
arithmetic (~1e-7 precision). User pushed: "fix the pragmatic fallback,
I want it fully resolved." Sourced the correct Cephes coefficients
(verified via `P[4]/Q[4] = -64.85/194.55 = -1/3`); also needed Cephes's
middle threshold of `0.66` (not `tan(π/8)`) and the `morebits`
compensation term. f64 atan now full ≤4 ulp precision.

**Item 4 — special functions (~600 LOC):** `erf` / `erfc` / `gamma` /
`lgamma` / `beta` for both f32 + f64. Cephes erf.c / erfc.c / gamma.c
implementations. f32 forwards to f64 (cast-down preserves f32 precision;
single source of truth).

**Bug surfaced:** initial f32 erfcf I copied was incomplete — Cephes
erfcf actually has TWO P/Q sets for different x ranges plus a different
evaluation form. The f32→f64 forward-and-cast architecture sidesteps
the duplication entirely.

Another bug: f64 gamma polynomial was padded with trailing 0.0 to make
the array 8-element — but `polevl` evaluates the whole array, so the
zero became the constant term. Fixed by sizing arrays to actual coef
counts (P=7, Q=8) + adding Cephes's `if (x == 2.0) return z` shortcut.

**Item 2 — Vec4f/Vec8f branchless SIMD batching (~700 LOC):** the big one.

User pushed: "I really want it to be done as beautiful and performant
as possible... It must be done. We need true branchless simd batching
and all the helpers we need."

Required net-new infrastructure:
- `vec4i.hpp` + `vec8i.hpp` (Vec4i / Vec8i types with arithmetic +
  bitwise + shifts + comparisons)
- `convert.hpp` (bitcast helpers, numerical conversions, SIMD float
  rounding, bitwise ops on Vec4f/Vec8f)
- `Vec8f cmp_lt/le/eq/gt/ge + select` added to `vec8f.hpp`

Then in `deterministic.cpp` the **branchless Cephes implementations**:
- Vec8f sin: branchless octant reduction via `cmp_eq + select`; sign
  tracking via bit-XOR; 3-component π/4 split for high-precision arg
  reduction.
- Vec8f cos: same with shifted octant logic for cos's sign sequence.
- Vec8f exp: range reduction via `round_nearest`; 2^k scale via
  integer-exponent bit-injection in f32 bit field.
- Vec8f log: frexp-style bit extraction; mantissa adjustment via
  `select` for the sqrt(0.5) branch; special-case (≤0, NaN, ±inf)
  handled via masked `select`.
- Vec4f variants compose into Vec8f, run the AVX2 path, narrow back.

**Bug caught during this work:** scalar-fallback `cmp_*` returned
`-1.0F` (bit pattern `0xBF800000`) instead of all-bits-set
(`0xFFFFFFFF` = NaN). Worked for `select()` (which checks `!= 0.0F`)
but corrupted `bitcast_to_int(mask)` arithmetic in `simd_log_v8` — the
value `0xBF800000` = `-1082130432` was getting added to the f32
exponent in log()'s `frexp` fixup. Fixed: scalar fallback now returns
`std::bit_cast<f32>(crd::u32{0xFFFFFFFFU})`.

**Disasm verification:** `deterministic.cpp.obj` on win-debug emits
**167 ymm references + 5 distinct AVX2 256-bit FP ops** —
`vaddps/vmulps/vsubps/vdivps/vsqrtps ymm` are real, not auto-vectorised
scalar emulations.

**Closing session log:** `docs/sessions/2026-05-10-v0c-debt-A-paydown.md`.

### v0d — deterministic sort + heap

**What:** `crd::containers::sort` / `stable_sort` / `nth_element` /
`push_heap` / `pop_heap` / `make_heap` / `sort_heap` with deterministic
guarantees per ADR-0063 §3 + `crd-no-std-sort-check` CI guard.

**Pragmatic algorithm choice:** the phase plan said "pdqsort-derived
with pinned tie-breaker." I shipped merge sort instead — naturally
stable + deterministic with simpler code. Pdqsort can swap in later if
benchmarks show merge sort is too slow for the consumer; the API
doesn't change.

**Bug caught:** ADL pulled `std::stable_sort` and `std::pop_heap` into
ambiguous overload sets when my `sort` and `sort_heap` made unqualified
calls. Fixed by qualifying with `crd::containers::`.

**Build-system bug surfaced (not v0d's fault):** `Array<T>(N)` only sets
capacity, not size. Initial test code `Array<i32> a(kN); a[i] = ...`
tripped the bounds check. Fixed by switching to `Array<i32> a; a.resize(kN);`
pattern. Same fix applied to `stable_sort`'s auxiliary buffer.

**Closing session log:** `docs/sessions/2026-05-10-v0d-sort-substrate.md`.

### v0e — SIMD benchmark harness (closes v0)

**What:** `tests/bench/test_bench_simd.cpp` — Catch2 BENCHMARK
micro-benchmarks for the four hot operations eylem v1+ will rely on per
physics tick: Vec3f dot, Vec3f cross, Mat4f multiply, Quatf compose.
Each runs scalar (8 ops loop) vs SIMD (AoSoA-8 chunk).

**Real win-release measured speedups:**
| Benchmark | Scalar (8 ops) | SIMD | Speedup |
|---|---:|---:|:---:|
| Vec3f dot | 3.86 ns | 0.66 ns | **5.9×** |
| Vec3f cross | 13.20 ns | 2.34 ns | **5.6×** |
| Mat4f multiply | 122.97 ns | 9.65 ns | **12.7×** |
| Quatf compose | 2.74 ns | 4.19 ns | **0.65× (regression)** |

The Quatf regression is documented behaviour — per-instance Quatf SIMD
has Vec4f stack-roundtrip overhead that doesn't pay off without
batching. AoSoA-8 quaternion layout (4 Vec8f columns: x/y/z/w) reserved
for eylem v4 articulation joint composition.

The Mat4f 12.7× speedup is the largest because the existing scalar
`crd::math::Mat4f` mult does row × column scalar inner loops that don't
auto-vectorise; the SIMD path keeps 4 columns in `__m128` registers and
multiplies via 4 broadcast + FMA-like patterns.

**Build infra fix:** had to suppress MSVC C4714 (`__forceinline` not
inlined) for `crd-bench` only — pre-existing LTCG noise from
`crd-log::detail::should_log` that gets triggered when bench TU mix
changes inlining cost decisions.

**Closing session log:** `docs/sessions/2026-05-10-v0e-bench-harness.md`.

---

## 4. Determinism contract — how it's enforced

ADR-0063 commits Cerid to **bit-exact reproducibility across MSVC /
clang-cl / GCC × x64 / ARM64 × Windows / Linux** for everything in
`engine/eylem/**` + `engine/hesap/**`. v0 enforces it via three
mechanisms layered on top of each other:

### Layer 1 — compiler flags (CrdSimd.cmake)

When `CRD_DETERMINISTIC_FP=ON` (the project default), `crd-simd-flags`
emits:

- **MSVC:** `/fp:precise` — bans operator reordering by associativity;
  bans contraction of `(a*b)+c` into hardware FMA.
- **GCC/Clang:** `-fno-fast-math -ffp-contract=off` (same intent) +
  `-mfpmath=sse` on x64 (eliminates 80-bit x87 intermediates).

These flags propagate via `crd-math` linking `crd-simd-flags` PUBLIC.
Every Cerid module transitively links `crd-math`, so the determinism
contract reaches everything without per-module CMake fiddling.

### Layer 2 — algorithmic discipline

- **No hardware FMA.** `mul_add(a, b, c)` in `vec4f.hpp` and `vec8f.hpp`
  is `(a * b) + c` (two roundings) on every backend. Hardware FMA
  (single rounding) gives different results across CPUs that have it vs
  CPUs that don't; we sacrifice the small throughput gain for backend
  parity.
- **Pairwise reduction tree.** `Vec4f::dot` is `(a0*b0 + a1*b1) +
  (a2*b2 + a3*b3)`; `Vec8f::dot` is the binary tree
  `((s01+s23) + (s45+s67))`. Every backend extracts lanes and sums in
  this order so SIMD/scalar agree bit-for-bit.
- **No `std::sin` / `std::sort` etc.** The CI guards
  (`crd-no-std-math-check`, `crd-no-std-sort-check`) catch any
  regression that imports them in `engine/eylem/**` or
  `engine/hesap/**`. Today these directories don't exist; the moment
  eylem v1a creates `engine/eylem/`, the guards light up.

### Layer 3 — CI verification

- **`crd-simd-emission-check`** disassembles `test_simd.cpp.obj` and
  confirms the actual machine code matches `CRD_SIMD_LEVEL_RESOLVED`.
  Catches regressions where the build system silently stops passing
  `/arch:AVX2` (the v0a real-bug scenario).
- **12-config sweep.** Every slice ships with a Win × 7 + Linux × 5
  (= 12) sweep verifying bit-exact identical results across compilers /
  OSes / SIMD backends. Test counts + assertion counts must match
  exactly across all configs.
- **Scalar parity preset** (`win-debug-scalar`, `linux-gcc-debug-scalar`)
  forces `CRD_SIMD_LEVEL=scalar` so the SIMD/scalar parity contract is
  CI-verifiable.

### What the contract does NOT cover

- **Multithreaded determinism.** The substrate is single-threaded by
  design. When eylem v1+ uses `crd-jobs::parallel_for` on Soa chunks,
  the cross-thread reductions need separate determinism work (Kahan
  summation in fixed order, atomic merges with commutative ops). That's
  eylem v1d (or wherever the first parallel reduction lands).
- **Compiler-specific intrinsics drift.** If MSVC's `_mm256_add_ps`
  ever produces different results from GCC's, our contract breaks.
  Hasn't happened in practice; if it does, the disasm check would catch
  the divergence at code-emission, not result-divergence.
- **Hardware bugs.** If a CPU returns a wrong value for `vsqrtps ymm`,
  our contract notices nothing. We trust IEEE 754 + the chip vendor.

---

## 5. SIMD architecture + backend selection

### Backend selection flow

```
User runs:                 cmake --preset win-debug
                                 ↓
CMakePresets.json sets:    CRD_SIMD_LEVEL=auto + CRD_DETERMINISTIC_FP=ON
                                 ↓
cmake/CrdSimd.cmake:       resolves auto → avx2 (x64) / neon (ARM64) /
                           scalar (other); refuses invalid combos
                                 ↓
crd-simd-flags target:     emits /arch:AVX2 (MSVC) or -mavx2 -msse4.2
                           (GCC/Clang) on the interface target
                                 ↓
                           emits CRD_SIMD_TARGET=2 macro definition
                                 ↓
crd-math links it PUBLIC:  every consumer of crd-math inherits both
                           the ISA flags AND the target macro
                                 ↓
backend.hpp at compile:    #if CRD_SIMD_TARGET == 2 → CRD_SIMD_BACKEND
                           = CRD_SIMD_BACKEND_AVX2
                                 ↓
vec4f.hpp / vec8f.hpp /    use CRD_SIMD_HAS_AVX2 / CRD_SIMD_HAS_SSE2 /
vec4i.hpp / etc.:          CRD_SIMD_HAS_NEON / CRD_SIMD_IS_SCALAR to
                           pick the right intrinsic per op
                                 ↓
Compiler emits:            vaddps ymm0,ymm1 etc.
                                 ↓
crd-simd-emission-check    disassembles, asserts ymm refs > 0
(CTest):                   for AVX2 builds; ymm refs == 0 for scalar
```

### Why this elaborate plumbing

The pre-v0a code checked compiler-defined ISA macros directly
(`#if defined(__AVX2__)`). MSVC doesn't define `__AVX2__` unless
`/arch:AVX2` is passed; the build silently used SSE2 on every
non-shipping config. Routing through CMake's explicit
`CRD_SIMD_TARGET` macro makes the choice **explicit and visible at
configure time** (the `[crd-simd]` summary lines in CMake output) and
**verifiable at CI time** (the disasm check).

### Per-backend storage

| Type | AVX2 | SSE2 | NEON | Scalar |
|---|---|---|---|---|
| `Vec4f` | `__m128` | `__m128` | `float32x4_t` | `f32[4]` |
| `Vec8f` | `__m256` | `Vec4f lo, hi` | `Vec4f lo, hi` | `f32[8]` |
| `Vec4i` | `__m128i` | `__m128i` | `int32x4_t` | `i32[4]` |
| `Vec8i` | `__m256i` | `Vec4i lo, hi` | `Vec4i lo, hi` | `i32[8]` |

The composed-fallback pattern is fundamental: when the backend doesn't
have a native 256-bit type, Vec8f decomposes into two Vec4f halves +
all ops dispatch to two Vec4f calls. **Same algorithm, half the
parallelism.** Performance tracks the backend; correctness tracks the
abstraction.

### Adding a new SIMD type — the recipe

If a future slice (e.g., eylem v8 GPU) needs `Vec8d` (8-lane f64):

1. Create `simd/vec8d.hpp`. Mirror `vec8f.hpp` structure but with
   `__m512d` (AVX-512) / composed `__m256d` lo/hi (AVX2) / scalar fallback.
2. Add bitwise ops + comparisons + select.
3. Add converters in `convert.hpp` (Vec8d ↔ Vec8i if relevant).
4. Add a test in `test_simd.cpp` with the same bit-exact-parity
   harness pattern (`bit_eq` checks across backends).
5. Register the type in `simd.hpp` umbrella.

The pattern is mechanical because v0a + v0c-debt-A established it
twice (Vec4f→Vec8f, then Vec4i→Vec8i). Follow the same shape.

---

## 6. Build system additions

### `cmake/CrdSimd.cmake`

The single most-load-bearing build addition. Defines:
- `CRD_SIMD_LEVEL` cache string (auto / scalar / sse2 / avx2 / neon /
  native)
- `CRD_DETERMINISTIC_FP` option (default ON)
- `crd-simd-flags` interface library carrying the ISA flags + the
  `CRD_SIMD_TARGET` integer macro + `CRD_DETERMINISTIC_FP=1` macro

Auto-resolves `CRD_SIMD_LEVEL=auto` against `CMAKE_SYSTEM_PROCESSOR`:
x64 → avx2, ARM64 → neon, other → scalar. Refuses invalid combos
(`avx2` on ARM64, etc.) with a clear `FATAL_ERROR`. Prints a 4-line
configure summary so reading the CMake output tells you exactly what's
being built.

### Root `CMakeLists.txt` changes

- `include(CrdSimd)` after the options block.
- **Removed `/fp:fast` and `-ffast-math` from `CRD_SHIPPING`** (was
  silently violating ADR-0063).

### `engine/math/CMakeLists.txt`

- `crd-math` links `crd-simd-flags` PUBLIC. Single source of truth for
  the SIMD flags; every transitive consumer (RHI, renderer, scene,
  eylem, sdf, hesap, app) inherits.

### `CMakePresets.json` additions

- `base` preset documents `CRD_SIMD_LEVEL=auto` + `CRD_DETERMINISTIC_FP=ON`
  as the project default.
- New `win-debug-scalar` + `linux-gcc-debug-scalar` presets force
  `CRD_SIMD_LEVEL=scalar` for the parity-validation lane.

### `tests/math/CMakeLists.txt` registers 3 CI guards

- `crd-simd-emission-check` (v0a)
- `crd-no-std-math-check` (v0c)
- `crd-no-std-sort-check` (v0d)

### `tests/bench/CMakeLists.txt`

- `test_bench_simd.cpp` added to crd-bench
- MSVC C4714 suppression scoped to crd-bench only

---

## 7. CI guards

Three lint/disasm checks that fail CI when the determinism contract
regresses. All three registered as CTest tests in
`tests/math/CMakeLists.txt`.

### `crd-simd-emission-check`

**What:** Disassembles `test_simd.cpp.obj` (via `dumpbin /disasm` on
Windows, `objdump -d` on Linux) and asserts the actual machine code
matches `CRD_SIMD_LEVEL_RESOLVED`:
- AVX2 build → ≥ 1 ymm reference (any 256-bit op)
- SSE2 / scalar → 0 ymm references
- LTCG IL-only obj → SKIP (covered by non-LTCG sibling configs)
- NEON → SKIP (ARM disasm parity check not implemented)

**Why it exists:** Catches the v0a real-bug scenario where the build
system silently stops passing `/arch:AVX2` and the code falls back to
SSE2 unnoticed.

**Scripts:** `scripts/check_simd_emission.{ps1,sh}`.

### `crd-no-std-math-check`

**What:** Greps `engine/eylem/**` + `engine/hesap/**` for `std::sin`
/ `std::cos` / `std::tan` / `std::asin` / `std::acos` / `std::atan` /
`std::atan2` / `std::exp` / `std::exp2` / `std::log` / `std::log2` /
`std::log10` / `std::pow` / `std::fmod`. Fails CI on any hit unless the
line has `// crd-lint-allow-std-math` marker.

**Why it exists:** libc / libstdc++ / Microsoft CRT differ in their
trig/exp/log results across platforms. Replays would diverge.

**Scripts:** `scripts/check_no_std_math.{ps1,sh}`.

### `crd-no-std-sort-check`

**What:** Same pattern, banning `std::sort` / `std::stable_sort` /
`std::nth_element` / `std::partial_sort` / `std::push_heap` /
`std::pop_heap` / `std::make_heap` / `std::sort_heap`. Opt-out marker
`// crd-lint-allow-std-sort`.

**Why it exists:** STL sort tie-breaks differently across libstdc++ /
libc++ / Microsoft STL on equal keys. Replays would diverge.

**Scripts:** `scripts/check_no_std_sort.{ps1,sh}`.

### Extension pattern

The next CI guard (e.g., banning `std::random_device` /
`std::mt19937` etc. for replay-determinism) follows the same shape:
- Two PowerShell + bash scripts in `scripts/`
- Registered as CTest test in `tests/math/CMakeLists.txt`
- Opt-out comment marker on the same line

---

## 8. Performance — measured numbers

### Win-release (AVX2 desktop, captured 2026-05-10)

| Benchmark | Scalar (8 ops) | SIMD | Speedup | Goal-line tolerance |
|---|---:|---:|:---:|:---:|
| Vec3f dot | 3.86 ns | 0.66 ns | **5.9×** | ±30% |
| Vec3f cross | 13.20 ns | 2.34 ns | **5.6×** | ±30% |
| Mat4f multiply | 122.97 ns | 9.65 ns | **12.7×** | ±30% |
| Quatf compose | 2.74 ns | 4.19 ns | 0.65× | (expected regression) |

Goal-line numbers committed in
`tests/bench/test_bench_simd.cpp` header comment. If a future build
shows >30% regression on dot/cross/mat4 (excluding the documented
Quatf case), investigate: compiler change, flags drift, CrdSimd.cmake
refactor.

### What the numbers mean for eylem v1

Eylem v1 is rigid 3D physics — fan-out parallel solver with constraint
batches of N=8 contacts at a time. The SIMD primitive layer gives it:

- **Broadphase pair tests** — each test is a Vec3f bbox-vs-bbox check.
  At Vec8f throughput (8 pairs per SIMD op), broadphase scales to
  ~8×N body counts at the same per-frame cost.
- **Velocity update step** — `v += dt * (a + g)` per body. Scalar
  cost = N × 3 mul-adds. SIMD cost (AoSoA-8) = ⌈N/8⌉ × 3 Vec8f
  mul-adds. Direct ~6× speedup.
- **Constraint solver** (Sequential Impulses) — per contact: cross
  product (torque), dot product (impulse magnitude), broadcast
  (apply impulse). The hot loop is exactly what v0e benchmarked.

Per-frame budget at 60 Hz physics: 16.67 ms. With 12.7× Mat4f speedup
and 5.9× dot speedup, the v0 substrate clears the perf headroom that
makes 1k–10k body simulations feasible without resorting to GPU
acceleration prematurely.

### Quatf — why per-instance SIMD doesn't help

Per-instance Quatf SIMD has Vec4f stack-roundtrip overhead:
1. Store Vec4f to stack (8 cycle latency on AVX2)
2. Read 4 scalar floats
3. Compute Hamilton product as 16 scalar muls + 12 scalar adds
4. Pack result into Vec4f (load from stack)

Scalar Quatf skips steps 1, 2, 4 entirely — the values stay in scalar
registers throughout. Net: scalar wins on per-instance.

The SIMD speedup arrives when 8 quaternions are processed at once via
an AoSoA-8 layout: 4 Vec8f columns (`Vec8f qx, qy, qz, qw`). The
Hamilton product is then 16 Vec8f muls + 12 Vec8f adds = ~equivalent
op count to one scalar quat × 8 entities = ~8× speedup. Eylem v4
articulation joint composition is the natural consumer.

---

## 9. Bugs surfaced + fixed during v0

Pinned here so the next contributor knows they're real and where they
came from. All fixed in their respective sessions; preserved here as
history.

### v0a — three latent bugs in the codebase
1. `__AVX2__` macro check silently fell back to SSE2 on every
   non-shipping config (MSVC doesn't define `__AVX2__` unless
   `/arch:AVX2` explicit).
2. `CRD_SHIPPING` enabled `/fp:fast` / `-ffast-math`, violating
   ADR-0063 from day 1.
3. No CI lane for SIMD/scalar parity verification.

### v0c — algorithm transcription
1. atan polynomial used `-` operators between already-signed Cephes
   coefficients → 5% wrong → cascaded to atan2/asin/acos. Fixed:
   all-`+` operators per Cephes convention.
2. sin/cos used overwrite-style sign application (`with_sign`) where
   Cephes uses XOR-combine semantics → wrong sign at octant boundaries
   for some inputs. Fixed: added `apply_sign` (XOR) helper, kept
   `with_sign` for true copysign use.
3. `ulp_diff` test helper used raw bit subtraction → treated `+tiny`
   vs `-tiny` as 2.1B ulps apart (the sign bit is bit 31). Fixed:
   signed-magnitude mapping for proper ulp counting near zero.
4. MSVC C4459 shadowing — local variable `e` (exponent) shadowed
   namespace constant `e` (Euler's number) in `ldexp_int_pow2`.
   Renamed `exp_int`.

### v0c-debt-A — coefficient sourcing + integration bugs
1. f64 atan Cephes coefficients I copied from a stale source gave
   `R(0) = -0.896` not `-1/3`. Sourced correct Cephes atan.c (verified
   `P[4]/Q[4] = -1/3`); also needed Cephes's `0.66` middle threshold
   and the `morebits` compensation term.
2. f64 gamma polynomial array padded with trailing 0.0 to make 8
   elements — but `polevl` evaluates the WHOLE array, so the zero
   became the constant term. Fixed by sizing to actual coef counts.
3. f32 erfcf I copied was incomplete (Cephes has TWO P/Q sets +
   different evaluation form). Pivoted to f32-forwards-to-f64
   architecture; single source of truth.
4. **Scalar fallback `cmp_*` returned `-1.0F`** (bit pattern
   `0xBF800000`) instead of all-bits-set (`0xFFFFFFFF`). Worked for
   `select()` but corrupted `bitcast_to_int(mask)` arithmetic in
   `simd_log_v8`. Fixed: scalar fallback now returns
   `std::bit_cast<f32>(crd::u32{0xFFFFFFFFU})`. SSE2/NEON paths were
   already correct.
5. constexpr lambda with union didn't work for the mask constant in
   vec4f.hpp; switched to `std::bit_cast` (added `<bit>` include).

### v0d — STL friction
1. ADL pulled `std::stable_sort` and `std::pop_heap` into ambiguous
   overload sets when my `sort` and `sort_heap` made unqualified calls.
   Fixed by qualifying with `crd::containers::`.
2. `Array<T>(N)` only sets capacity, not size — `a[i] = ...` then
   tripped the bounds check. Fixed: `Array<T> a; a.resize(N);` pattern.
3. `win-clang-cl` PCH cache mismatch during sweep — clang-cl PCH from
   an older MSVC version conflicted. Fixed by reconfiguring the preset.

### v0e — LTCG warning surfacing
1. C4714 (`__forceinline` not inlined) in `crd-log::detail::should_log`
   gets triggered when the bench binary's TU mix changes inlining cost
   decisions. Pre-existing, not v0e's fault. Suppressed C4714 for
   `crd-bench` only via `target_compile_options(crd-bench PRIVATE
   /wd4714)`. Other targets still surface the warning if it matters.

---

## 10. What's deferred + where it lives

`docs/debt.md` is the source of truth. Two open items:

### Bessel functions + orthogonal polynomials → `crd-hesap-stats` v13

`bessel_j0/j1/y0/y1/i0/i1/k0/k1` (Bessel of first/second/modified kind)
and `legendre_p/hermite_h/chebyshev_t` (orthogonal polynomials) belong
in `crd-hesap-stats` v13 (Phase 3.1.6, ADR-0065) where they sit
alongside distribution PDFs/CDFs that consume them. Cephes has
battle-tested implementations; the cooker over there will copy them.

**Why deferred (not just postponed):** these are statistics-module
concerns, not basic math. Putting them in `crd::math::deterministic`
would bloat every Cerid module's compile time for functions ~5% of
modules need. Keeping them in `crd-hesap` honours the architectural
split between lean substrate (crd-math) and heavy numerical computing
(crd-hesap, ADR-0065).

### True hardware gather (`_mm256_i32gather_ps`) for `gather8`

v0b's `gather8` is software (extract-and-pack). Hardware gather is
faster on some Intel uarchs but its determinism varies (rounding mode
edge cases) and the perf delta is small for the use case. Reserved as a
v0e benchmark-driven optimisation if eylem v1+ measures the gap as
material.

### What is NOT debt (rejected scope expansions)

- **AVX-512 path** — not until AVX-512 becomes a Cerid CI target. Today
  the CI matrix is x64 + ARM64, both explicitly without AVX-512.
- **Compile-time tunable `Lane` widths > 8** — Soa is locked to {4, 8}
  via static_assert. AVX-512 width 16 reserved for the same trigger as
  the AVX-512 path.
- **GPU mirror of the SIMD math** — `crd-hesap-gpu` (ADR-0065 v17) is
  the right place. v0 stays CPU-only.
- **Vec4d / Vec8d (f64 SIMD)** — wait for the first f64 consumer, which
  will be `crd-hesap-dense` BLAS L1 (Phase 3.1.6 v0). f32 SIMD is what
  eylem needs.

---

## 11. Pitfalls + extension notes

For the next contributor (or AI agent) extending v0:

### 1. Don't redefine `cmp_*` semantics

The mask convention is **all-bits-set for true, all-bits-zero for
false**, matching SSE2 / AVX2 / NEON hardware comparisons. Scalar
fallback uses `std::bit_cast<f32>(0xFFFFFFFFU)` (a NaN). This is
load-bearing — `bitcast_to_int(mask) & Vec4i(value)` arithmetic
relies on it. If you ever change `cmp_*` to return `-1.0F` "because
that's simpler," you'll silently break the SIMD log() implementation.

### 2. `Lane` is closed: 4 or 8

The static_assert in `Soa<TChunk, Lane>` refuses other widths. AVX-512
support requires more than just adding `16` to the asserts — the AVX2
fallback paths in vec8f / vec8i / convert assume Lane=8 maps to one
ymm. AVX-512's zmm registers (16 f32 lanes) need their own type
(Vec16f) + its own composed fallback. Don't widen `Lane` without
adding the type.

### 3. `mul_add` is two roundings, not one

If you ever "optimise" `mul_add` to use hardware FMA (`_mm_fmadd_ps`),
you break the SIMD/scalar parity contract. Hardware FMA is a single
rounding; scalar `(a*b) + c` is two roundings. Different bit results.
Tests will catch it (the `[determinism]`-tagged tests in
test_simd.cpp), but the sweep will fail mysteriously on AVX2 vs
scalar.

If you ever genuinely need single-rounding FMA performance, add a
separate API (`fma_single_rounding`) — don't change `mul_add`.

### 4. Reductions use a fixed pairwise tree

`Vec4f::dot` is `(a0*b0 + a1*b1) + (a2*b2 + a3*b3)` exactly. Don't
"simplify" to `a0*b0 + a1*b1 + a2*b2 + a3*b3` (left-to-right) or
`_mm_dp_ps` (hardware dot). The pairwise order matches the scalar
reference; alternatives don't.

### 5. f64 special functions forward to scalar deterministic — not std

`erf` / `gamma` / `lgamma` / `beta` for f32 forward to f64 deterministic
implementations (cast-down). Don't bypass with `std::erf` etc. — the
CI guard `crd-no-std-math-check` is currently scoped to `engine/eylem/**`
+ `engine/hesap/**`, but the principle applies.

### 6. The merge-sort stable-sort allocates O(N) extra memory

Required default-constructible `T`. If a caller has a
non-default-constructible type, they need to either (a) provide one,
(b) wrap their items in a default-constructible struct, or (c) wait
for the future "Sort with caller-provided buffer" overload. Don't
silently switch to in-place merge sort or quicksort — the determinism
guarantees change.

### 7. Bench numbers in the comment header are platform-specific

`tests/bench/test_bench_simd.cpp`'s goal-line numbers were captured on
a specific AVX2 desktop on 2026-05-10. Different hardware will give
different absolute numbers. The relative speedup ratios should hold:
~5–6× for dot/cross, ~10–15× for Mat4f mul, regression for per-instance
Quatf. If the ratios are way off, investigate the build (LTCG flags,
`/arch:AVX2`, etc.) before assuming the substrate is broken.

### 8. The `crd-simd-emission-check` skips on LTCG configs

Because LTCG (Release / RelWithDebInfo / Shipping) emits IL-only
.obj files. Don't try to "fix" the skip — the disasm guard runs on the
non-LTCG sibling configs (Debug, ASan, clang-cl, debug-scalar) which
exercise the same code path. The LTCG-only-failure scenario where AVX2
silently stops being passed isn't really possible (the same source
compiles to both).

---

## 12. What v1a inherits + where to start

### What v1a (eylem rigid 3D, slice 1) gets for free

Everything in [Section 2](#2-inventory--every-type-function-file). Specifically:

- `Vec8f` for AoSoA-8 hot loops (broadphase pair tests, velocity
  integration, constraint impulse computation).
- `Mat4f` for body-to-world transforms.
- `Quatf` for body orientation.
- `Soa<BodyChunk8, 8>` storage pattern (eylem v1 will define
  `BodyChunk8` as a struct of Vec8f columns).
- `crd::math::deterministic::*` for any per-frame trig / exp / log
  (e.g., articulation kinematics in v4).
- `crd::containers::sort` / `nth_element` for broadphase sweep + pair
  generation.
- Three CI guards already auto-protecting future eylem code from
  regressing to `std::sin` / `std::sort`.

### Where v1a starts

1. **Read `docs/phases/phase-3.1-eylem.md` v1a row** for the slice spec.
   v1 has 12 sub-slices over ~6–8 weeks; v1a sets up the eylem module
   skeleton + SoA body state storage + the EYLM CRDR snapshot artifact
   format.

2. **Create `engine/eylem/`** with the standard layout
   (include/crd/eylem/, src/, CMakeLists.txt; mirror engine/sdf/ or
   engine/hesap/ when those exist — neither does yet, so engine/scene/
   is the closest reference).

3. **CMake link line for `crd-eylem`:**
   ```cmake
   target_link_libraries(crd-eylem PUBLIC
       crd-core
       crd-memory
       crd-containers
       crd-math      # gets crd-simd-flags transitively → SIMD + determinism
       crd-jobs      # for parallel_for over Soa chunks
       crd-scene     # for World + Component integration
       crd-resources # for SnapshotResource + EYLM cooker
   )
   ```

4. **First file pattern:**
   ```cpp
   // engine/eylem/include/crd/eylem/body.hpp
   #include <crd/math/simd/vec8f.hpp>

   namespace crd::eylem {

   // AoSoA-8 body storage chunk. 8 logical bodies per chunk.
   // Columns: position (3) + linear velocity (3) + inverse mass (1) = 7 Vec8f.
   struct alignas(32) BodyChunk8
   {
       crd::math::simd::Vec8f pos_x, pos_y, pos_z;
       crd::math::simd::Vec8f vel_x, vel_y, vel_z;
       crd::math::simd::Vec8f inv_mass;
   };

   }  // namespace crd::eylem
   ```

5. **First test pattern:** `tests/eylem/test_body.cpp` with the same
   bit-exact-parity discipline as `test_simd.cpp` and
   `test_deterministic.cpp`.

6. **The CI guards light up** the moment `engine/eylem/` directory
   exists. Anything regressing to `std::sin` / `std::sort` will fail
   CI on the spot.

### Things to remember when starting v1a

- **Six-config quality bar.** Every shipped slice must pass the 12-config
  sweep. Don't merge a half-done slice; pay the determinism debt now,
  not later.
- **Document deferrals before coding.** The v0c-debt-A story shows
  what happens when deferrals get out of hand — they accumulate to
  ~1100 LOC of same-day paydown. Better to write the deferral list
  upfront so the consumer phase can plan around it.
- **Per-slice session log.** This dossier is the v0 closure, but each
  v1 slice gets its own session log under `docs/sessions/`. Future-you
  will thank present-you for being verbose.
- **The phase plan is alive.** Update `phase-3.1-eylem.md` as slices
  ship — mark them `✅ shipped` with the session log link, like the
  v0 rows are now.

---

## Appendix — full sweep counts at v0 closure

After v0e shipped:

| Test binary | Total cases | Total assertions |
|---|---:|---:|
| `crd-math-tests` | 141 | 2965 |
| `crd-containers-tests` | (added 14 sort cases / 1068 assertions) | (existing + 1068) |
| `crd-bench` (4 SIMD bench cases) | 4 | (timing only, no assertions) |

12-config sweep results identical across:
- 3 compilers: MSVC, clang-cl, GCC
- 2 OSes: Windows 11, Ubuntu (Linux WSL2)
- 2 SIMD backends: AVX2, scalar
- 6 Windows configs (debug, relwithdebinfo, release, asan, clang-cl, tidy, debug-scalar) + 5 Linux configs (debug, relwithdebinfo, release, asan, debug-scalar)

All bit-exact identical. The ADR-0063 contract holds.

## Appendix — session log chain for v0 slices

Read in order if reconstructing the journey:
1. `docs/sessions/2026-05-10-v0a-simd-substrate.md`
2. `docs/sessions/2026-05-10-v0b-soa-substrate.md`
3. `docs/sessions/2026-05-10-v0c-deterministic.md`
4. `docs/sessions/2026-05-10-v0c-debt-A-paydown.md`
5. `docs/sessions/2026-05-10-v0d-sort-substrate.md`
6. `docs/sessions/2026-05-10-v0e-bench-harness.md`
7. **This file** (`2026-05-10-v0-substrate-closure.md`)

## Appendix — relevant ADRs

- **ADR-0063** — Eylem determinism contract (the load-bearing spec for
  everything in v0)
- **ADR-0062** — Eylem physics architecture (the consumer Phase 3.1 v1+
  builds on v0)
- **ADR-0064** — `crd-sdf` substrate (consumer Phase 3.1.5)
- **ADR-0065** — `crd-hesap` numerical computing substrate (consumer
  Phase 3.1.6; deferred Bessel + orthogonal polynomials live here)
- **ADR-0061** — Async GPU upload contract (`UploadHandle` / `Fence`
  reused for `crd-hesap-gpu` later)
- **ADR-0034** — C++ hot-reload DLL scripting (C ABI pattern for
  `crd-hesap-repl` later)
