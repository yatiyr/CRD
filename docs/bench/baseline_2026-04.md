# Benchmark Baseline — 2026-04

Build flavour: `win-release`

Machine:

- CPU: `Intel(R) Core(TM) i9-14900K`
- RAM: `63.7 GiB`
- OS: `Microsoft Windows NT 10.0.26200.0`

Notes:

- Benchmarks come from `tests/bench/test_bench.cpp` via `crd-bench.exe [bench]`.
- Numbers below are Catch2 means from the first committed baseline.
- This is a small codebase baseline; use it for regression tracking, not for
  broad engine-performance claims.

## Results

| Benchmark | Mean |
| --- | ---: |
| Disabled `CRD_LOG_TRACE` call | `0.191572 ns` |
| Async log producer push | `396.515 ns` |
| `Array<u32>::push_back` 1k amortised | `853.786 ns` |
| `HashMap<u32,u32>` insert 1M | `45.2836 ms` |
| `HashMap<u32,u32>` find 1M | `12.0909 ms` |
| `HashMap<u32,u32>` erase 1M | `26.6381 ms` |
| `String` SSO construct + assign | `2.9374 ns` |
| `String` heap construct + assign | `48.0579 ns` |
| `Vec3f` add | `0.373036 ns` |
| `Vec3f` dot | `0.724887 ns` |
| `Vec3f` normalize | `0.290291 ns` |
| `Vec3d` dot | `0.799976 ns` |
| `Vec3d` normalize | `0.28999 ns` |
| `Mat4f * Vec4f` | `2.35544 ns` |
| `Mat4f * Mat4f` | `10.0339 ns` |
| `Mat4d * Vec4d` | `0.8028 ns` |
| `Mat4d * Mat4d` | `0.801791 ns` |
| `Quatf` multiply | `2.75813 ns` |
| `Quatf` rotate `Vec3f` | `11.7329 ns` |
| `Transformf` compose | `14.6563 ns` |
| `Transformf` point | `12.8228 ns` |

## Interpretation

- Disabled trace logging is effectively free in Release, as designed.
- HashMap numbers are now pinned early, before math/platform grow the codebase.
- `String` SSO still shows a large gap versus heap-backed text construction,
  which is the behavior we want to preserve.
- The first math slice already has `f32` and `f64` benchmark numbers pinned,
  so future SIMD and matrix work will have a scalar baseline to compare against.
- Matrix baselines are now pinned too, which gives the future quaternion /
  transform slice a clean lower-level substrate to regress against.
- Quaternion and rigid-transform baselines are now pinned as well, which means
  the upcoming primitive-geometry slice will build on a benchmarked spatial
  substrate instead of a purely semantic one.
