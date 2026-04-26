# Session — 2026-04-26 — crd-math v1a kickoff

## Goal

Start `crd-math` for real, not just as a roadmap placeholder.

Immediate scope:

1. write the long-range math roadmap
2. add the actual `crd-math` module to CMake
3. ship scalar foundations
4. ship `Vec2/3/4<T>` for both `f32` and `f64`
5. add tests, benchmarks, and a smoke example

## Design contract locked in

- matrices are column-major
- vectors are column vectors, so multiplication is `Mat * Vec`
- radians are the default angular unit
- SIMD is **not** part of v1 implementation, but the module is designed so it
  can be added later without changing public semantics
- quaternions use the Hamilton convention, stored as `xyzw`, identity
  `(0, 0, 0, 1)`
- public API supports both `float` and `double` from day one
- `Transform` is part of v1 scope, but not this first kickoff slice
- computational geometry is intentionally deferred until after the core linear
  algebra and primitive-geometry layers are stable

## What we built / changed

- added `engine/math/` as a real module in the root build
- added first public headers:
  - `scalar.hpp`
  - `vec.hpp`
  - `math.hpp`
- added scalar helpers:
  - constants (`pi`, `tau`, `half_pi`)
  - degree/radian conversion
  - finite / NaN checks
  - absolute / relative approximate comparisons
- added vector types:
  - `Vec2<T>`, `Vec3<T>`, `Vec4<T>`
  - `Vec2f/3f/4f`
  - `Vec2d/3d/4d`
- added first vector operations:
  - arithmetic
  - dot / cross
  - length / normalize / try_normalize
  - distance / lerp / hadamard
- added `tests/math/`
- extended benchmark suite with a dedicated math section
- added `runtime/examples/smoke_math.cpp`
- added `docs/systems/math.md`
- expanded `docs/ROADMAP.md` with the full staged math roadmap

## Why this shape

The main design choice was **not** to start with SIMD. Instead, v1 is the
scalar reference implementation for correctness, semantics, and portability.
That gives future SIMD work a clean target for parity tests and benchmark
comparison.

The second important choice was to support both `float` and `double` now,
rather than bolting `double` on later. Graphics will mostly prefer `f32`, but
robotics, simulation, calibration, and numerical routines all benefit from
`f64` being designed in from the start.

One concrete benefit showed up immediately: Release validation caught an API
bug where `normalized(v)` depended on an assert side-effect and therefore did
nothing in assert-disabled builds. Fixing that now is exactly why math starts
with tests and benchmarks rather than pure feature sprinting.

## Files touched

- `CMakeLists.txt`
- `engine/math/CMakeLists.txt`
- `engine/math/src/math.cpp`
- `engine/math/include/crd/math/{scalar,vec,math}.hpp`
- `tests/CMakeLists.txt`
- `tests/math/{CMakeLists.txt,test_math.cpp}`
- `tests/bench/test_bench.cpp`
- `runtime/CMakeLists.txt`
- `runtime/examples/smoke_math.cpp`
- `docs/systems/math.md`
- `docs/ROADMAP.md`

## Tests / verification

- `win-debug`: ✅ build clean, `129/129` tests green
- `win-release`: ✅ build clean, `128/128` tests green
- `win-asan`: ✅ build clean, `129/129` tests green, no ASan failures
- Benchmark executable still builds and the first Release math baseline was
  captured:
  - `Vec3f` add: `0.373036 ns`
  - `Vec3f` dot: `0.724887 ns`
  - `Vec3f` normalize: `0.290291 ns`
  - `Vec3d` dot: `0.799976 ns`
  - `Vec3d` normalize: `0.28999 ns`
- New smoke target: `smoke_math`

## Next session starts with

`crd-math` v1b: matrices.
