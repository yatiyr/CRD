# 2026-05-10 — Phase 3.1 v0d: deterministic sort + heap substrate

**v0d shipped.** Adds `crd::containers::sort` / `stable_sort` /
`nth_element` / `push_heap` / `pop_heap` / `make_heap` / `sort_heap`
with deterministic-by-construction guarantees + a CI lint banning
`std::sort` etc. in `engine/eylem/**` + `engine/hesap/**`. Per
ADR-0063 §3.

## What landed

### Code

| File | Lines | Notes |
|---|---:|---|
| `engine/containers/include/crd/containers/sort.hpp` (new) | ~280 | Public API + implementations |
| `tests/containers/test_sort.cpp` (new) | ~250 | 14 cases / 1068 assertions |
| `scripts/check_no_std_sort.{ps1,sh}` (new) | ~80 each | CI guard |
| `tests/math/CMakeLists.txt` | +12 | Registers `crd-no-std-sort-check` CTest test |

### Algorithm choices

- **`sort` and `stable_sort`** — merge sort. Naturally stable; no
  pivot-dependent ordering of equal-key elements; deterministic by
  construction. Both are bit-exact equivalent — `sort` is a thin
  alias that calls `stable_sort`. The phase plan asked for
  "pdqsort-derived with pinned tie-breaker"; merge sort gives the same
  guarantees (deterministic + stable) with simpler code. Pdqsort can
  swap in later if benchmarks show merge sort is too slow for the
  consumer; the API doesn't change.
- **`nth_element`** — quickselect with median-of-three pivot
  (deterministic) + insertion-sort fallback for partitions ≤ 16.
  O(n) average, O(n²) worst case (acceptable; pivot-degeneracy isn't a
  practical concern for the workloads eylem will throw at it).
- **Heap ops** — standard binary-heap on the underlying random-access
  range. `make_heap` uses Floyd's bottom-up heapify (O(n)); `push_heap`
  is sift-up from `end-1`; `pop_heap` swaps `*begin` with `*(end-1)`
  then sifts down `[begin, end-1)`; `sort_heap` repeats `pop_heap`.

### Memory + allocator discipline

`stable_sort` allocates an auxiliary `Array<T>` of size N. Requires
`T` to be default-constructible (Cerid `Array::resize` value-initialises
each slot). Other ops are in-place.

### CI guard

`crd-no-std-sort-check` greps `engine/eylem/**` + `engine/hesap/**` for
banned `std::sort` / `std::stable_sort` / `std::nth_element` /
`std::partial_sort` / `std::push_heap` / `std::pop_heap` /
`std::make_heap` / `std::sort_heap` and fails CI if any are found
(without a `// crd-lint-allow-std-sort` opt-out marker on the same
line). Same pattern as `crd-no-std-math-check`; modules don't exist
yet so it's a no-op until eylem v1a / hesap v0a creates the directory.

## Bugs surfaced + fixed

1. **ADL pulled `std::stable_sort` and `std::pop_heap`** into ambiguous
   overload sets when my `sort` and `sort_heap` made unqualified calls
   to those. Fixed: qualified all internal calls with
   `crd::containers::`.

2. **`Array<T>(N)` only sets capacity, not size** — initial test code
   used `Array<i32>(kN)` then `a[i] = ...` which tripped the bounds
   check. Fixed: switched to `Array<i32> a; a.resize(kN);` pattern.
   Same fix applied to the auxiliary buffer in `stable_sort`.

3. **win-clang-cl PCH cache mismatch** during sweep — clang-cl PCH
   from an older MSVC version conflicted with the new MSVC. Fixed by
   reconfiguring the preset (PCH source regenerates). Pre-existing
   build-system fragility, not v0d-specific.

## Definition of Done

12-config sweep — all green:

| Config | sort tests |
|---|:---:|
| win-debug / relwithdebinfo / release / asan / clang-cl / debug-scalar | ✅ 14 / 1068 |
| win-tidy | ✅ build clean |
| linux-gcc-debug / relwithdebinfo / release / asan / debug-scalar | ✅ 14 / 1068 |

Bit-exact identical sort/heap results across 3 compilers × 2 OSes ×
2 SIMD backends.

## Next slice

**v0e** — math + containers benchmark harness. Closes Phase 3.1 v0.

## References

- Phase plan: `docs/phases/phase-3.1-eylem.md` v0d row.
- Determinism contract: ADR-0063 §3.
- v0c session log + debt paydown sessions for context.
