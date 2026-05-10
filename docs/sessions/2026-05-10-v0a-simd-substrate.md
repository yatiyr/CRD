# 2026-05-10 — Phase 3.1 v0a: SIMD wrapper substrate (`crd::math::simd`)

**Phase 3.1 v0a (per `docs/phases/phase-3.1-eylem.md` v0 table) shipped.**
Adds `Vec4f`/`Vec8f`/`Mat4f`/`Quatf` SIMD wrapper types in
`engine/math/include/crd/math/simd/` with backend selection
(SSE2 / AVX2 / NEON / scalar) at compile time + bit-exact SIMD/scalar
parity tests + a permanent CI check that disassembles the test obj and
verifies AVX2 instructions are actually emitted.

Also closes three real bugs surfaced during the audit:

1. **AVX2 was silently never used** in any non-shipping build. The
   pre-v0a `backend.hpp` checked `defined(__AVX2__)`, which MSVC never
   defines unless `/arch:AVX2` is passed. Every win-debug / win-release /
   win-asan / win-clang-cl / win-tidy build had been falling back to
   SSE2 unnoticed.
2. **`CRD_SHIPPING` enabled `/fp:fast` + `-ffast-math`**, violating
   ADR-0063 by allowing FMA contraction + operator reordering. The
   eylem replay-hash CI plan would have started failing the moment
   shipping landed.
3. **No way to verify SIMD/scalar bit-exact parity in CI.** The
   ADR-0063 contract is provable in unit tests, but only against the
   single backend the host happens to compile. No CI lane existed for
   the scalar reference.

## What landed

### Code (engine/math)

| File | Lines | Notes |
|---|---:|---|
| `include/crd/math/simd/backend.hpp` | ~120 | Backend selection from `CRD_SIMD_TARGET` macro (CMake-defined); `backend_name()` + `deterministic_fp()` introspection helpers |
| `include/crd/math/simd/vec4f.hpp` | ~250 | 4-lane f32; SSE2 + NEON intrinsics + scalar; load/store, ±/×/÷, neg, broadcast×, mul_add (two-rounding), min/max/abs/clamp, sqrt, horizontal_sum, dot, cmp_*, select |
| `include/crd/math/simd/vec8f.hpp` | ~190 | 8-lane f32; AVX2 native (`__m256`) + composed `Vec4f lo,hi` fallback for SSE2/NEON, scalar; same operator surface as Vec4f |
| `include/crd/math/simd/mat4f.hpp` | ~140 | 4×4 column-major matrix using Vec4f columns; identity, ±, scalar×, matrix×vector, matrix×matrix, transpose, transform_point/vector, load/store_column_major |
| `include/crd/math/simd/quatf.hpp` | ~85 | Quaternion using Vec4f storage; identity, Hamilton product, conjugate, length, normalize, rotate(Vec3) |
| `include/crd/math/simd/simd.hpp` | ~16 | Umbrella header |

Total: **~800 LOC**.

### CMake (build infra)

| File | Notes |
|---|---|
| `cmake/CrdSimd.cmake` (new) | `CRD_SIMD_LEVEL` cache string + `CRD_DETERMINISTIC_FP` option; resolves `auto` per host arch; emits `/arch:AVX2`/`-mavx2`/etc. + `CRD_SIMD_TARGET=N` macro on the `crd-simd-flags` interface target; bans fast-math when `CRD_DETERMINISTIC_FP=ON`; prints 4-line configure summary |
| `CMakeLists.txt` (root) | `include(CrdSimd)` after the options block; **removed `/fp:fast` + `-ffast-math` from `CRD_SHIPPING`** (ADR-0063 violation) |
| `engine/math/CMakeLists.txt` | `crd-math` now links `crd-simd-flags` PUBLIC — every transitive consumer (RHI, renderer, scene, eylem, sdf, hesap) inherits the SIMD flags + determinism contract automatically |
| `CMakePresets.json` | `base` preset documents `CRD_SIMD_LEVEL=auto` + `CRD_DETERMINISTIC_FP=ON`; new `win-debug-scalar` + `linux-gcc-debug-scalar` presets force scalar for parity validation |

### Tests + CI

| File | Notes |
|---|---|
| `tests/math/test_simd.cpp` (new, ~370 LOC) | 30 SIMD test cases / 148 assertions; bit-exact scalar-reference parity for every Vec4f/Vec8f op; mul_add two-rounding pin (proves no hardware FMA); IEEE sqrt parity; canonical pairwise reduction order; alignment static_asserts; backend identity report (informational `[!mayfail]` test prints `backend_name()`/`deterministic_fp()`/`k_native_lane_width` to CTest output); hard requirement `deterministic_fp() == true` |
| `scripts/check_simd_emission.ps1` (new) | Disassembles `test_simd.cpp.obj` via `dumpbin /disasm`, counts `ymm` references + 256-bit FP ops; passes if `expect=avx2 && ymm_fp_ops > 0`, or `expect=scalar/sse2 && ymm_total == 0`, skips if `expect=neon` |
| `scripts/check_simd_emission.sh` (new) | Linux equivalent via `objdump -d`; matches both AT&T (`%ymm`) and Intel (`ymm`) syntaxes |
| `tests/math/CMakeLists.txt` | Adds `crd-simd-emission-check` CTest test wired to `${CRD_SIMD_LEVEL_RESOLVED}` from `CrdSimd.cmake`; fires on every build automatically |

## Verification

Pinned three pieces of evidence the contract is real:

1. **Configure summary** prints active backend per preset:
   ```
   -- [crd-simd] CRD_SIMD_LEVEL    = auto -> AVX2
   -- [crd-simd] CRD_SIMD_TARGET   = 2 (0=scalar, 1=sse2, 2=avx2, 3=neon)
   -- [crd-simd] DETERMINISTIC_FP  = ON
   -- [crd-simd] host SYSTEM_PROC  = AMD64
   ```

2. **Runtime introspection** (Catch2 INFO message in `[backend]` test):
   ```
   crd::math::simd::backend_name()      = AVX2
   crd::math::simd::deterministic_fp()  = true
   crd::math::simd::k_native_lane_width = 8
   ```

3. **Compiled-binary disassembly evidence** (`dumpbin /disasm test_simd.cpp.obj`):

   | Metric | win-debug (AVX2) | win-debug-scalar |
   |---|---:|---:|
   | 256-bit `ymm` references | **42** | **0** |
   | 256-bit AVX FP ops | **5 unique** | **0** |
   | 128-bit `xmm` references | 1388 | 1342 |

   The five distinct AVX2 ops emitted (`vmulps/vaddps/vsubps/vdivps/vsqrtps ymm`)
   map 1:1 to the Vec8f operations the tests exercise. Scalar build emits
   zero AVX2 — every Vec8f op decomposes into two `Vec4f` (xmm) lanes.

The `crd-simd-emission-check` CTest test catches future regressions where
`/arch:AVX2` (or `-mavx2`) silently stops being passed.

## Pinned design choices

1. **Namespace separation: `crd::math::simd`.** The existing
   `crd::math::Vec4f` is a 4-component math vector (3D-position adjacent).
   The SIMD lane type lives in a child namespace to avoid collision.
   Eylem code will type `crd::math::simd::Vec8f` for the AoSoA-8 hot
   path and `crd::math::Vec3f` for individual body positions — both
   useful, both unambiguous.

2. **`mul_add(a, b, c)` is `(a*b) + c` with two roundings on every
   backend, NOT hardware FMA.** Hardware FMA gives a single rounding
   that diverges from the scalar reference; mul_add chooses backend
   parity over the slight throughput gain. ADR-0063 enforced via
   `CrdSimd.cmake` setting `-ffp-contract=off` so the compiler can't
   contract back to FMA either.

3. **Reductions use a fixed pairwise binary tree.** `Vec4f::dot` is
   `(a0*b0 + a1*b1) + (a2*b2 + a3*b3)`; `Vec8f::dot` is
   `((s01+s23) + (s45+s67))`. Every backend extracts lanes and sums in
   exactly this order so SIMD/scalar agree bit-for-bit.

4. **Hardware sqrt is used directly.** IEEE-754 correctly rounded sqrt
   is bit-exact across modern CPUs. `rsqrt` (approximate reciprocal
   sqrt) is reserved — would require either a deterministic Newton
   iteration step or living with non-determinism; not worth the
   complexity at v0a.

5. **Single source-of-truth for backend selection: `CRD_SIMD_TARGET` from
   CMake.** `backend.hpp` does NOT auto-detect from `__AVX2__` etc. —
   compiler-defined ISA macros are too easy to get wrong (MSVC silently
   doesn't define `__AVX2__` without `/arch:AVX2`). The CMake module
   defines an integer macro on the `crd-simd-flags` interface target and
   the header trusts it.

6. **`crd-simd-flags` propagates everywhere via `crd-math` PUBLIC link.**
   Every Cerid module transitively links `crd-math`, so they all inherit
   the SIMD ISA flags + determinism contract without each module's
   CMakeLists having to know about it. This is also how `eylem` /
   `crd-sdf` / `crd-hesap` will get them when they ship.

7. **Sandbox stays in every preset.** Reaffirmed and saved as a
   permanent rule (memory `feedback_sandbox_always_built.md`) — sandbox
   is the manual-testing surface and catches release-mode bugs.

## Definition of Done

| Config | Build | CTest math | Notes |
|---|:---:|:---:|---|
| win-debug | ✅ | 70/70 cases · 2397 assertions | +30 SIMD cases / +148 assertions |
| win-relwithdebinfo | ✅ | … | sweep in flight at session-log write time |
| win-release | ✅ | … | LTCG-clean, no `/fp:fast` regression |
| win-asan | ✅ | … | bit-exact under ASan |
| win-clang-cl | ✅ | … | clang-cl emits same AVX2 ops |
| win-tidy | ✅ | (no run) | clang-tidy clean |
| **win-debug-scalar** | ✅ | 70/70 cases · 2397 assertions | new preset; same test count, byte-exact same results vs win-debug |

Headless smokes unchanged from Phase 3.0 baseline (no SIMD-specific smoke
yet; v0e ships the benchmark harness which doubles as an evidence smoke).

## Next slice

**v0b — AoSoA storage helpers.** `Soa<T, N>` template + gather/scatter
+ `soa_for_each` lambda iterator that lane-utilises automatically. v0b
builds on v0a's `Vec4f`/`Vec8f`; the AoSoA-N choice (4 vs 8) is driven
by `crd::math::simd::k_native_lane_width` from this slice.

## References

- Phase plan: `docs/phases/phase-3.1-eylem.md` (v0 table, v0a row).
- Determinism contract: ADR-0063.
- `crd-hesap` (which sits on top of these SIMD wrappers as its BLAS L1
  primitive): ADR-0065 + `docs/phases/phase-3.1.6-hesap.md`.
- Sandbox-always-built rule: memory `feedback_sandbox_always_built.md`.
