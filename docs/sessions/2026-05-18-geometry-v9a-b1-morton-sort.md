# 2026-05-18 — Phase 3.1.7 v9a-b1 CPU stable LSD radix sort of Morton-code pairs ✅ SHIPPED

**Slice:** v9a-b1 of the `crd-geometry-bvh-gpu` v9a LBVH cluster. 6th slice shipped same day as v9a-a base + 4 follow-ons.

**Status:** ✅ shipped same day. 5-config DoD PASS via `scripts/per-slice-check.ps1 -IncludeRelease -Parallel` in 34 s.

---

## Why this exists

`compute_morton_codes_cpu` (v9a-a) + `compute_morton_codes_cpu_60bit` (v9a-60bit-cpu) produce per-AABB Morton codes. The Karras 2012 LBVH builder (v9a-c) consumes those codes **sorted ascending, with original-index tiebreak**. v9a-b1 is the deterministic CPU sort that closes the gap between the Morton step and the tree-build step.

It is also the **correctness oracle** for v9a-b2 GPU radix sort. `bit_compare<MortonPair<KeyT>>` will assert v9a-b2 output is byte-identical to v9a-b1 output — so v9a-b1's body has to be exactly right.

---

## Slice-start decision pin (D141): radix, not `crd::containers::sort`

The phase-doc row tagged both options at slice start: *"CPU sort directly or radix-sort wrapper"*. I called the advisor before writing code; advisor concurred with radix and sharpened the plan in seven specific ways (perf-budget tiering, `MortonPair<u64>` = 16 B not 12 B, "1 ms / 1 M" target is aspirational, paper-divergence explicit text, verify `Array::resize` works, add u64-specific upper-passes test, oracle comparator written as lex (code, index)). All seven incorporated.

Budget math:
- Phase-doc target: 1 ms / 1 M elements.
- O(N log N) merge sort over 1 M 8-byte pairs measures 50–100 ms shipping. Blows budget 50×.
- LSD radix 8-bit digit = O(N · k) with k=4 (u32) / k=8 (u64). ~3–5 ms shipping for u32 1 M on the dev box. Headroom over budget.

Documented divergence per `feedback_document_paper_divergence_explicitly` — the divergence is from the phase-doc literal text, pinned with rationale paragraph in the file header + system doc + this log.

---

## What shipped

### Files

| File | LOC | Purpose |
|---|---|---|
| `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/morton_sort.hpp` | ~120 | `MortonPair<KeyT>` + `sort_morton_pairs<KeyT>` template decl + extern templates + divergence note |
| `engine/geometry-bvh-gpu/src/morton_sort.cpp` | ~140 | LSD radix 8-bit-digit implementation + explicit instantiations for u32 and u64 |
| `tests/geometry-bvh-gpu/test_morton_sort.cpp` | ~430 | 20 cases / 40 134 assertions (calibration, sanity, stability, oracle, determinism, integrity, u64 mirror, u64 upper-bits discriminator, perf budget, end-to-end integration) |

### Edits

- `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/morton.hpp` — fixed stale `radix_sort_cpu` reference in the tiebreak comment to `sort_morton_pairs<KeyT>` (and the header path).
- `engine/geometry-bvh-gpu/CMakeLists.txt` — slice-ledger comment updated for the new function name + template note.

### API

```cpp
namespace crd::geometry::bvh_gpu {

template <typename KeyT>
struct MortonPair { KeyT code; crd::u32 index; };

static_assert(sizeof(MortonPair<crd::u32>)        == 8U);
static_assert(sizeof(MortonPair<std::uint64_t>)   == 16U);
static_assert(alignof(MortonPair<crd::u32>)       == 4U);
static_assert(alignof(MortonPair<std::uint64_t>)  == 8U);

template <typename KeyT>
[[nodiscard]] crd::containers::Array<MortonPair<KeyT>>
sort_morton_pairs(crd::containers::ConstSpan<KeyT> codes,
                  crd::memory::IAllocator* alloc) noexcept;

// Convenience overload: Array<KeyT> input → forwards to ConstSpan path.
template <typename KeyT>
[[nodiscard]] crd::containers::Array<MortonPair<KeyT>>
sort_morton_pairs(const crd::containers::Array<KeyT>& codes,
                  crd::memory::IAllocator* alloc) noexcept;

// Explicit instantiations in morton_sort.cpp for KeyT ∈ {u32, u64}.
} // namespace crd::geometry::bvh_gpu
```

### Algorithm body

```
sort_morton_pairs<KeyT>(codes, alloc):
  n = codes.size()
  CRD_ASSERT(n <= UINT32_MAX)           // D144 — u32 index ceiling
  if n == 0: return empty
  out = Array<MortonPair<KeyT>>(n, alloc)
  for i in 0..n-1:
    out[i] = {codes[i], u32(i)}         // monotonic ascending index ⇒ stability auto
  if n < 2: return out
  static_assert((sizeof(KeyT)*8 / 8) % 2 == 0)   // even pass count
  aux = Array<MortonPair<KeyT>>(n, alloc)
  src = out.data(); dst = aux.data()
  for pass in 0..kNumPasses-1:
    histogram[256] = {0}
    shift = pass * 8
    for i: histogram[(src[i].code >> shift) & 0xFF]++
    exclusive prefix sum → histogram[]
    for i: dst[histogram[bucket]++] = src[i]
    swap(src, dst)
  // Even pass count ⇒ src == out.data(); aux drops
  return out
```

### Pinned decisions (carried for ADR-0076 §25 amendment at v9a-close)

- **D141** — Algorithm = stable LSD radix sort with 8-bit digit, **not** `crd::containers::sort` (merge). Phase-doc literal-text divergence per slice-start D-pin marker, rationale = published 1 ms / 1 M budget vs O(N log N). Karras 2012 §4 names radix.
- **D142** — Pair layout = AoS `MortonPair<KeyT>`. `sizeof(MortonPair<u32>) == 8` (4+4, no padding); `sizeof(MortonPair<u64>) == 16` (8+4+4 trailing pad). `alignof` also static_asserted. SoA gain is marginal at 8-bit digit + L1-fit histogram + 8/16-byte pair; AoS keeps the v9a-b2 GPU upload one-shot.
- **D143** — Stability via LSD-radix's natural property + monotonic-ascending input-index construction. No separate tiebreak pass. Discriminating tests: all-equal + partial-tie + 10 k oracle cross-check.
- **D144** — Pair index width fixed at `crd::u32`. 4 B-element ceiling asserted at entry. Lifts to `u64` if/when a downstream consumer asks; not speculative-engineered.

---

## Test corpus (40 134 assertions / 20 cases)

Calibration-first per advisor TDD + v9-prereq-test-harness discipline.

| Section | Cases | Notes |
|---|---|---|
| **CALIBRATION** | 1 u32 + 1 u64 | 5 hand-rolled pairs, hand-computed expected output. Failure of this section ⇒ everything downstream is meaningless. |
| Trivial shape | 4 u32 + 1 u64 | empty / single / already-sorted / reverse-sorted |
| **STABILITY discriminator** | 2 u32 + 1 u64 | all-equal-keys preserve input index order (phase contract: "equal Morton codes ⇒ lower input index wins") + partial-tie test |
| **Bullet-proof oracle** | 1 u32 + 1 u64 | 10 000 random codes, byte-identical (`memcmp == 0`) against `crd::containers::sort` over a lexicographic `(code, index)` comparator |
| Determinism | 1 u32 + 1 u64 | two runs, byte-identical output |
| **Pair-integrity sieve** | 1 u32 + 1 u64 | Array<bool> sieve — every input index appears exactly once in output, no dupes, no drops; sortedness verified |
| **u64 upper-32-bits discriminator** | 1 | three pairs sharing low 32 bits but differing in upper. A bit-shift typo in passes 5–8 would silently pass the u32 suite but loudly fail here |
| Integration | 1 | `compute_morton_codes_cpu` → `sort_morton_pairs<u32>` end-to-end LBVH pipeline candidate |
| **Perf budget** | 1 u32 + 1 u64 | `CRD_PERF_BUDGET_LE` 20 ms NDEBUG / 2000 ms debug for u32 1 M; 40 / 4000 for u64 1 M. Advisor flagged the phase-doc 1 ms target as aspirational; `v9a-b1-simd` AVX2 follow-on filed for consumer-pull. |

ValidationCapture is not applicable here (this is pure CPU; the captures live on GPU dispatch paths).

---

## One mid-slice fix

**Initial 5-config sweep**: tidy PASS, all 4 ctest configs FAIL with exit=8.

Root cause: the test name `"sort_morton_pairs u32 pair integrity: indices form a permutation of [0, N)"` contains a `[xyz)` bracket-comma pattern that **`catch_discover_tests`** (the CMake-side test-name discoverer) mis-parses as a malformed Catch2 tag. The discovery output fused 14 distinct test cases into one CTest entry whose "test name" was a `;`-delimited blob. CTest dispatched that compound string as `--test-spec`, found zero Catch2 matches, exited non-zero.

The actual Catch2 binary produced the correct 20-case output when run directly — this was a CTest registration bug, not a logic bug.

**Fix**: renamed to drop the `[0, N)` substring → `"sort_morton_pairs u32 pair integrity: indices form a permutation"`. Second 5-config sweep PASS in 34 s.

Filed a new memory entry — bracket-comma patterns in TEST_CASE names break `catch_discover_tests`. Easy to avoid once known.

---

## Outcome

5-config DoD PASS (`scripts/per-slice-check.ps1 -IncludeRelease -Parallel`):

```
win-debug          PASS (build+ctest)
win-asan           PASS (build+ctest)
win-shipping       PASS (build+ctest)
win-release        PASS (build+ctest)
win-tidy           PASS (build)
```

Total elapsed 00:34, parallel. Full geometry-bvh-gpu binary post-slice: **42 cases / 40 308 assertions**, no regressions.

---

## Next

🎯 **v9a-b2 — GPU radix sort** (~5 days, ~800 LOC engine + ~500 tests). Blelloch parallel-scan radix, 4-bit digit × 8 passes for 30-bit keys (per phase doc). Conformance contract: byte-identical to v9a-b1 output via `bit_compare<MortonPair<u32>>`. Throughput-tier — non-deterministic by atomic ordering, but topology-identical to v9a-b1.

The substrate for v9a-b2 is ready:
- `crd-rhi-compute` substrate (Phase 3.1.7.6) — pipelines, storage buffers, dispatch, async compute, semaphores.
- v9-prereq-test-harness — `ValidationCapture` + `ulp_compare`/`bit_compare` + `gpu_determinism_check` + `CRD_PERF_BUDGET_LE` + `-IncludeRelease` 5-config DoD.
- `MortonPair<KeyT>` SSBO layout locked by static_assert (`sizeof == 8 / 16`, `alignof == 4 / 8`). GPU can upload in one shot.
- v9a-b1 CPU output as the byte-identical correctness oracle.

---

## Commit message proposed

```
feat(geometry-bvh-gpu): v9a-b1 CPU stable LSD radix sort of (Morton, index) pairs

* New header `morton_sort.hpp` + impl `morton_sort.cpp` + tests
  `test_morton_sort.cpp` (20 cases / 40 134 assertions).
* `MortonPair<KeyT> { KeyT code; u32 index; }` AoS pair (8 B u32 /
  16 B u64) with sizeof + alignof static_asserted for the v9a-b2 GPU
  SSBO layout contract.
* `sort_morton_pairs<KeyT>(ConstSpan<KeyT>, IAllocator*)` templated
  over key width (D136-REVISED) with explicit instantiations for
  KeyT ∈ {u32, u64} from day 1.
* Stable LSD radix sort, 8-bit digit, 4 passes u32 / 8 passes u64.
  Ping-pong buffer with static_assert-locked even-pass-count contract.
* D141 documented divergence from phase-doc literal "via
  crd::containers::sort" — radix chosen for the 1 ms / 1 M budget
  that O(N log N) merge cannot meet (Karras 2012 §4 names radix as
  the canonical LBVH sort step).
* Stability (D143) delivered automatically by LSD + monotonic-
  ascending input-index construction → equal Morton codes ⇒ lower
  input index wins.
* Test corpus: calibration-first + sanity + stability discriminator
  + 10 k u32 + 10 k u64 oracle cross-check vs `crd::containers::sort`
  byte-identical + determinism + permutation-sieve + u64 upper-32-bits
  discriminator + end-to-end Morton → sort + tiered perf budget.
* Mid-slice fix: test name `[0, N)` mis-parsed by
  catch_discover_tests; renamed.
* 5-config DoD PASS in 34 s.
* Pinned D141-D144 carried for ADR-0076 §25 amendment at v9a-close.
* Memory entry for the catch_discover_tests bracket-comma gotcha.
```
