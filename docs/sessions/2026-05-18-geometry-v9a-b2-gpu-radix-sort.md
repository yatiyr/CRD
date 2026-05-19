# 2026-05-18 — Phase 3.1.7 v9a-b2 GPU LSD radix sort of Morton-code pairs ✅ SHIPPED

**Slice:** v9a-b2 of the `crd-geometry-bvh-gpu` v9a LBVH cluster. 7th slice shipped same day as v9a-a base + 4 follow-ons + v9a-b1.

**Status:** ✅ shipped same day. 5-config DoD PASS via `scripts/per-slice-check.ps1 -IncludeRelease -Parallel` in 33 s.

> Session note: the work was already mid-flight when the dev box crashed. Recovery handoff: shaders + hpp + cpp + test_radix_sort.cpp were all on disk with the build cache valid (test binary timestamp 5/18/2026 17:00, all 4 SPV shaders present). On resume, the local binary already produced "All tests passed (8257 assertions in 9 test cases)" for `[radix]` — the algorithm body had been validated before the crash. The only remaining work to close the slice was: (1) one ctest-encoding fix surfacing during the first sweep, (2) clearing two transient PDB-lock / ICE artefacts left over from the crash-interrupted build dirs, (3) the docs.

---

## Why this exists

v9a-b1 shipped the deterministic CPU stable LSD radix sort + the `MortonPair<KeyT>` SSBO layout contract. v9a-b2 ships the GPU twin so the v9a-c LBVH builder can consume sorted Morton pairs entirely on the GPU without a host round-trip.

The contract from v9a-b1: GPU output is **byte-identical** to CPU output for any input — `bit_compare<MortonPair<u32>>(cpu, gpu)` is the discriminating oracle. The CPU implementation IS the algorithm definition; the GPU is a mechanical translation.

---

## Slice-start decision pins (D146 — D149)

The phase-doc row originally described v9a-b2 as *"throughput-tier (non-deterministic by atomic ordering); ULP-conformance test compares against v9a-b1 CPU output"*. The pre-implementation decision pinned the **other variant**: **prefix-sum scatter, not atomic-counter scatter**. Output is bit-deterministic across runs *and* byte-identical to CPU.

**D146 — Determinism contract.** At 4-bit digit + 1024 items/block, the atomic-counter throughput advantage is marginal vs the value of byte-identical-to-CPU conformance. A future throughput-tier variant (`v9a-b2-atomics`) can trade determinism for higher throughput if a real consumer ever asks; v9a-b2 ships the deterministic flavor.

**D147 — Sizing pin.** Hard-cap N at `kRadixMaxItems = 1 M` today. Beyond requires a recursive scan kernel — filed as `v9a-b2-large` follow-on. Cap is shared between host + shader via `kRadix*` constants in `radix_sort.hpp` with paired static_asserts.

**D148 — Pipeline pin.** Workgroup size 256, items per thread 4 ⇒ 1024 items per block. Scan workgroup 256 × 4 items/thread ⇒ 1024 block-entries; per-bin Hillis-Steele scan inside one workgroup. Shared memory budget ≈ 5 KiB — well inside the Vulkan-portable 16 KiB floor.

**D149 — Pair format pin.** `MortonPair<u32>` layout locked by static_assert in `morton_sort.hpp` (sizeof = 8, alignof = 4). GPU upload is one shot; pairs ping-pong from pass 0; even pass count (8 for 32-bit / 4-bit-digit) so final result terminates in `pairs_a` — same parity rule as CPU v9a-b1.

---

## What shipped

### Files

| File | LOC | Purpose |
|---|---|---|
| `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/radix_sort.hpp` | ~165 | `MortonRadixGpuPipeline` class + shared `kRadix*` constants + static_asserts mirroring shader layout |
| `engine/geometry-bvh-gpu/src/radix_sort.cpp` | ~465 | Pipeline ctor (load 4 SPV, build pipelines + layouts + descriptor allocator) + `dispatch_radix_sort` (25-dispatch pipeline with sync fence wait + readback) |
| `runtime/examples/shaders/radix_sort_init.comp` | ~50 | Pack `{codes[i], gl_GlobalInvocationID.x}` into pairs_a |
| `runtime/examples/shaders/radix_sort_histogram.comp` | ~65 | Per-workgroup 16-bin histogram of current 4-bit digit |
| `runtime/examples/shaders/radix_sort_scan.comp` | ~180 | Single-workgroup per-bin Hillis-Steele scan + bin-offset patch |
| `runtime/examples/shaders/radix_sort_scatter.comp` | ~115 | Per-slot bin tagging + local-rank-within-bin serial scan + final scatter |
| `tests/geometry-bvh-gpu/test_radix_sort.cpp` | ~500 | 9 cases / 8257 assertions: calibration + trivial shapes + stability + 10 k/262 k/1 M oracle + determinism + integration + perf budget |

### Edits

- `engine/geometry-bvh-gpu/CMakeLists.txt` — slice ledger comment updated for v9a-b2.
- `tests/geometry-bvh-gpu/CMakeLists.txt` — 4 radix shader sources added to `crd_compile_glsl` list.
- `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/morton.hpp` — tiebreak doc-comment updated from stale `radix_sort_cpu` to `sort_morton_pairs<KeyT>` (header path).

### API

```cpp
namespace crd::geometry::bvh_gpu {

// Shared constants — host + shader contract.
constexpr u32 kRadixWorkgroupSize    = 256U;
constexpr u32 kRadixItemsPerThread   = 4U;
constexpr u32 kRadixItemsPerBlock    = 1024U;   // = WG × items/thread
constexpr u32 kRadixMaxBlocks        = 1024U;
constexpr u32 kRadixMaxItems         = 1U << 20; // 1,048,576 (D147)
constexpr u32 kRadixDigitBits        = 4U;
constexpr u32 kRadixBins             = 16U;
constexpr u32 kRadixNumPassesU32     = 8U;       // 32 / 4

class MortonRadixGpuPipeline
{
public:
    MortonRadixGpuPipeline(rhi::Device&, StringView shader_dir) noexcept;
    bool is_valid() const noexcept;

    // Sort `codes` ascending; equal-key pairs by ascending input index.
    // Output byte-identical to v9a-b1 `sort_morton_pairs<u32>`.
    // N <= kRadixMaxItems (D147 cap); 25 dispatches + fence-wait readback.
    Array<MortonPair<u32>> dispatch_radix_sort(
        ConstSpan<u32> codes, IAllocator* alloc) noexcept;
};
} // namespace crd::geometry::bvh_gpu
```

### Dispatch pipeline body

```
1. upload codes ─stage→ codes_gpu
2. init kernel:        pack (code, gl_GlobalInvocationID.x) → pairs_a
3. for pass in 0..7 (8 passes × 3 dispatches = 24):
     a. histogram(pair_in)  → block_hist        (256 thr × 4 itm; atomics on shared)
     b. scan(block_hist)    → scatter_off       (single-WG per-bin scan + bin-offset patch)
     c. scatter(pair_in, scatter_off) → pair_out (per-slot bin tag + serial-rank + scatter)
     swap(pair_in, pair_out)
4. After 8 (even) passes, final output is back in pairs_a.
5. copy_buffer pairs_a → readback (host visible).
6. fence wait + map + memcpy into caller-owned Array<MortonPair<u32>>.
```

---

## Stability discipline (D146)

CPU LSD radix is stable because the input pair sequence `{codes[i], u32(i)}` is monotonic-ascending in `i`, and LSD-radix's natural stability preserves relative order across equal keys.

GPU twin preserves the same property in three steps:
1. **Init kernel** sets `pair.index = gl_GlobalInvocationID.x` — the same monotonic-i sequence.
2. **Per-workgroup local rank** in scatter is computed via a serial prefix scan over `s_bin_of_slot[1024]` (count-prior-equal-bin), **not** an atomic counter — equal-bin items within a workgroup land in monotonic thread-id order.
3. **Cross-workgroup ordering** comes from the scan kernel's per-bin global prefix — `scatter_off[wg, bin]` is the running sum of prior workgroups' counts for that bin; equal-bin items across workgroups land in monotonic workgroup-id order.

End result: equal Morton codes ⇒ lower input index wins, byte-identical to CPU.

---

## Test corpus (9 cases / 8257 assertions)

Calibration-first per advisor TDD + v9-prereq-test-harness discipline.

| Section | Cases | Notes |
|---|---|---|
| **CALIBRATION** | 1 | N=16 hand-rolled, GPU byte-identical to CPU oracle. If this fails everything downstream is meaningless. |
| Trivial shape | 2 | empty / N=1 |
| **STABILITY discriminator** | 1 | 4096 identical codes spanning 4 workgroups — output indices must be 0..4095 monotonic, both within and ACROSS workgroups. |
| **Bullet-proof oracle** | 2 | 10 000 + 262 144 random codes, byte-identical to v9a-b1 CPU oracle via `bit_compare<MortonPair<u32>>`. |
| Determinism | 1 | `gpu_determinism_check` 3 dispatches, byte-identical (D146 contract). |
| Integration | 1 | `compute_morton_codes_cpu` → `dispatch_radix_sort` end-to-end LBVH-pipeline candidate. |
| **Perf budget** | 1 | `CRD_PERF_BUDGET_LE` 30 ms NDEBUG / 30000 ms debug for 1 M items end-to-end (upload + 25 dispatches + readback + fence wait). |

ValidationCapture is asserted silent (`error_count() == 0`, `warning_count() == 0`) on every dispatching test.

---

## Mid-slice fixes during recovery

**Fix #1 — Non-ASCII arrow in two TEST_CASE names.** The original test names contained the Unicode `⇒` (U+21D2):
- `"v9a-b2 GPU radix empty input ⇒ empty output"`
- `"v9a-b2 GPU radix N=1 ⇒ single pair {code, 0}"`

The `crd-no-non-ascii-test-names` guard test fired (3rd-party of: ctest argv via the Active Code Page mojibakes the arrow on Turkish Windows, Catch2 filter then misses the test entirely, "No tests ran" + non-zero exit). Renamed to:
- `"v9a-b2 GPU radix empty input yields empty output"`
- `"v9a-b2 GPU radix N=1 yields single pair {code, 0}"`

The existing memory entry `MEMORY.md` already documented the bug class. The guard caught it correctly on the first sweep — its job is to keep this rule durable across slices.

**Fix #2 — Stale PDB / PCH artefacts left over from the crash.** Initial post-recovery sweep failed win-asan + win-tidy with `C1033: program database cannot be opened` and `LNK1285: PDB corrupted`. Cause: the OS crash mid-link left the affected PDB files unflushed; subsequent builds couldn't open them. Fix: nuke the `CMakeFiles/crd-geometry-bvh-gpu.dir/` + `CMakeFiles/crd-geometry-bvh-gpu-tests.dir/` directories for the two presets + corrupted link PDB + ilk, reconfigure, rebuild. Standard recovery.

**Fix #3 — Transient MSVC LTCG/clang-tidy ICE on `test_kd_range_aabb.cpp`.** A separate spatial-tests source crashed cl.exe in `cl!CloseTypeServerPDB`/`cl!DllGetObjHandler` during win-tidy. Same class as `feedback_transient_msvc_ltcg_ice_accept` + `feedback_transient_clang_tidy_crash`. Per memory rule: retry on a clean rebuild — the second build (`cmake --build --preset win-tidy`) PASSED.

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

Total elapsed 00:33, parallel. Full geometry-bvh-gpu binary post-slice: **51 cases / 48 565 assertions**, no regressions (was 42/40 308 at v9a-b1 close → +9 cases / +8 257 assertions for v9a-b2).

---

## Pinned decisions (carried for ADR-0076 §25 amendment at v9a-close)

- **D146** — Determinism contract: prefix-sum scatter, not atomic-counter scatter. Output is bit-deterministic across runs *and* byte-identical to CPU v9a-b1 oracle. Throughput-tier variant `v9a-b2-atomics` deferred to consumer-pull.
- **D147** — N hard-cap at `kRadixMaxItems = 1 M`. Recursive-scan follow-on `v9a-b2-large` filed.
- **D148** — Workgroup 256, items/thread 4 ⇒ 1024 items/block. Scan single-WG per-bin Hillis-Steele; shared memory ≈ 5 KiB.
- **D149** — `MortonPair<u32>` SSBO layout (sizeof 8, alignof 4) locked by static_assert. Pairs ping-pong from pass 0; even pass count → final output in pairs_a.

---

## Next

🎯 **v9a-c — GPU LBVH tree (Karras 2012)**. Binary-tree-from-sorted-Morton-codes; each internal node determined by 2 keys via prefix-length comparison — fully parallel, no atomics. Outputs flat `Array<BvhNode>` matching CPU `BvhTree` layout. ~600 LOC engine + ~400 tests, ~4 days.

The substrate is now in place:
- `compute_morton_codes_{cpu,gpu}` (v9a-a) — Morton codes 30-bit and 60-bit
- `sort_morton_pairs<KeyT>` (v9a-b1) — CPU oracle, byte-identical contract
- `MortonRadixGpuPipeline::dispatch_radix_sort` (v9a-b2) — GPU sort with byte-identical-to-CPU conformance

End-to-end the test_radix_sort integration test demonstrates `compute_morton_codes_cpu` → `dispatch_radix_sort` already working. v9a-c only needs to consume the sorted output.

---

## Commit message proposed

```
feat(geometry-bvh-gpu): v9a-b2 GPU LSD radix sort, byte-identical to v9a-b1 CPU oracle

* New header `radix_sort.hpp` + impl `radix_sort.cpp` + 4 compute shaders
  (init/histogram/scan/scatter) + tests `test_radix_sort.cpp` (9 cases /
  8257 assertions).
* MortonRadixGpuPipeline caches 4 compute pipelines + descriptor
  allocator; dispatch_radix_sort runs 25 dispatches per sort
  (1 init + 8 passes x 3) + sync fence wait + readback.
* Algorithm: 4-bit-digit LSD radix, 8 passes for 30-bit u32 keys.
  Workgroup 256 thr x 4 items/thr = 1024 items/block (D148).
  N capped at kRadixMaxItems = 1M (D147); v9a-b2-large filed for recursive
  scan beyond.
* D146 determinism contract: prefix-sum scatter (NOT atomic-counter)
  ⇒ output is bit-deterministic across runs AND byte-identical to v9a-b1
  CPU oracle. Throughput-tier atomic variant deferred to consumer-pull.
* Stability preserved via three layers: (1) init packs index =
  gl_GlobalInvocationID.x (same monotonic-i as CPU); (2) scatter computes
  local-rank-within-bin via per-slot serial scan, not atomic counter;
  (3) cross-workgroup ordering via scan kernel's per-bin global prefix.
* D149 pair layout: MortonPair<u32> (sizeof 8, alignof 4) locked by
  static_assert in morton_sort.hpp; pairs ping-pong from pass 0; even
  pass count parity rule matches CPU v9a-b1.
* Test corpus: calibration-first (N=16 hand-rolled byte-identical to CPU)
  + trivial shapes (empty / N=1) + STABILITY discriminator (4096 equal
  codes spanning 4 WGs preserve monotonic index order) + 10k + 262k
  bullet-proof oracle (bit_compare vs CPU) + gpu_determinism_check 3
  rounds + ValidationCapture silent + end-to-end compute_morton_codes_cpu
  → dispatch_radix_sort + CRD_PERF_BUDGET_LE 30 ms NDEBUG / 30000 ms
  debug for 1M items.
* Pinned D146-D149 carried for ADR-0076 §25 amendment at v9a-close.
* Mid-slice fix: two TEST_CASE names contained the Unicode arrow ⇒
  (U+21D2) — caught by crd-no-non-ascii-test-names guard; renamed to
  ASCII "yields" form.
* 5-config DoD PASS in 33 s.
```
