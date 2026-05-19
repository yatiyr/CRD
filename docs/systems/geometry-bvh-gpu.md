# crd-geometry-bvh-gpu

GPU implementation of the LBVH (Karras 2012) build pipeline. **Sibling** module to `crd-geometry-bvh` (CPU-only), mirroring the `crd-rhi` / `crd-rhi-vulkan` split pattern: the existing CPU BVH module stays CPU-only (no Vulkan dependency); GPU kernels + dispatch live here.

> Module path: `engine/geometry-bvh-gpu/`
> Target: `crd-geometry-bvh-gpu`
> Namespace: `crd::geometry::bvh_gpu`
> Opened: Phase 3.1.7 v9a-a (2026-05-18)
> Status: ✅ **CLUSTER CLOSED 2026-05-18** — v9a-a Morton ✅ + 4 follow-ons ✅ + v9a-b1 CPU sort ✅ + v9a-b2 GPU sort ✅ + v9a-b1-simd scalar+prefetch ✅ + v9a-b1-parallel 3-phase crd-jobs ✅ + v9a-c LBVH tree+upsweep (elite combine, absorbs v9a-d) ✅ + **v9a-close ADR-0076 §25 ✅ Accepted + 18-config full sweep PASS** all shipped. Phase 3.1.7 sub-module 11 of 11 ✅.

## Public surface

| Header | Purpose |
|---|---|
| `crd/geometry/bvh_gpu/morton.hpp`        | CPU `compute_morton_codes_cpu` + `spread_bits_30` / `morton3_30bit_*` / `quantize_to_morton_grid` primitives |
| `crd/geometry/bvh_gpu/morton_60bit.hpp`  | CPU u64 60-bit oracle (`spread_bits_60` + `morton3_60bit_*` + `quantize_to_morton_grid_20bit`) |
| `crd/geometry/bvh_gpu/morton_typed.hpp`  | Strip-compute-retag wrappers around 30-bit CPU + GPU paths (ADR-0078 §5 D34) |
| `crd/geometry/bvh_gpu/morton_sort.hpp`   | `MortonPair<KeyT>` + `sort_morton_pairs<KeyT>` stable LSD radix sort (v9a-b1) |
| `crd/geometry/bvh_gpu/radix_sort.hpp`    | `MortonRadixGpuPipeline` GPU LSD radix sort + `kRadix*` constants (v9a-b2) |
| `crd/geometry/bvh_gpu/dispatch.hpp`      | `MortonGpuPipeline` (30-bit) + `MortonGpu60BitPipeline` (60-bit) compute pipelines |

## v9a-a — 30-bit Morton-code generation

```cpp
namespace crd::geometry::bvh_gpu {

[[nodiscard]] Array<u32>
compute_morton_codes_cpu(ConstSpan<AABB3<f32>> aabbs, IAllocator* alloc);

[[nodiscard]] Array<u32>
compute_morton_codes_cpu(ConstSpan<AABB3<f32>> aabbs,
                          const AABB3<f32>& scene_aabb, IAllocator* alloc);

class MortonGpuPipeline {
  MortonGpuPipeline(rhi::Device&, StringView shader_dir);

  [[nodiscard]] Array<u32>
  dispatch_morton_codes(ConstSpan<AABB3<f32>> aabbs,
                        const AABB3<f32>& scene_aabb,
                        IAllocator* alloc);
};

} // namespace crd::geometry::bvh_gpu
```

### Algorithm

Per AABB centroid, normalise into the scene AABB and quantise each axis to 10 bits ⇒ `[0, 1023]`. Bit-interleave the three 10-bit components ⇒ 30-bit `u32` Morton code with bit pattern `z9 y9 x9 z8 y8 x8 ... z0 y0 x0`. Pure deterministic function of input.

**The CPU implementation IS the algorithm definition.** The GLSL shader (`engine/geometry-bvh-gpu/shaders/compute_morton_codes.comp`) is a verbatim mechanical translation of the per-element kernel body — same bit pattern, same FP quantisation, same scene-AABB normalisation. Any divergence is a bug. Test: `bit_compare(cpu, gpu)` must be byte-identical for any finite input.

### Scope: 30-bit vs 60-bit (D-132)

30-bit Morton at 1024³ resolution per axis means primitives smaller than `scene_extent / 1024` along any axis collide into the same Morton bin (tiebreak via `original_index` in the sort key). Practical breakpoints:

| Scene extent | Resolution per axis |
|---|---|
| 100 m  | ~10 cm |
| 1 km   | ~1 m   |
| Orbital scale | ~km |

30-bit is the v9a-a lock — game / sim / CAD scenes at ≤ 100 m extent. Eylem-aero (planned) and CAM at km-scale hit this wall; **`v9a-60bit` follow-on slice filed** (u64 60-bit Morton, 20 bits per axis ≈ 1M³ resolution).

### Determinism

CPU + GPU paths are both bit-deterministic given builder rejects NaN/Inf at the API boundary. `gpu_determinism_check(3 rounds)` is part of the v9a-a test corpus — passes on RTX 4070 Ti SUPER (validated 2026-05-18); contract holds for any GPU honouring IEEE arithmetic on a pure-function kernel.

### Performance

Per-dispatch cost is CPU-side staging + GPU kernel + readback. Pure GPU kernel is sub-millisecond on 256k AABBs (RTX 4070); end-to-end wall-clock budget at v9a-a is 200 ms / 256k AABBs (generous, includes staging upload + fence wait + readback). The published `<0.5 ms / 1M prims` GPU-only budget at v9a-close requires separating kernel timing from staging/readback — done at v9a-close with timestamp queries.

### Dispatch lifecycle

`MortonGpuPipeline` caches the heavyweight Vulkan objects (compute pipeline, descriptor set layout, pipeline layout, compiled SPIR-V module, descriptor allocator) across dispatches. Compiling SPIR-V + creating a `VkPipeline` is in the tens-of-milliseconds range; reusing the pipeline across many `dispatch_morton_codes` calls means the per-dispatch overhead is just buffer upload + recording + fence wait.

Sync-compute submission goes through `graphics_queue()` — `create_command_buffer()` allocates from the graphics-family command pool, so submission MUST go to a same-family queue. On GPUs with a dedicated compute family (e.g. RTX 4070), `compute_queue()` is a DIFFERENT queue family and would trigger `VUID-vkQueueSubmit-pCommandBuffers-00074`. True async-compute (compute-family command pool + submit) is the **`v9a-a-async-compute` follow-on** when the RHI surfaces a compute-family command pool.

## Pinned design decisions (carried for ADR-0076 §25 amendment at v9a-close)

- **D132 (v9a-a)** — Module structure: NEW sibling module `crd-geometry-bvh-gpu`, NOT additions to existing `crd-geometry-bvh`. Mirror of `crd-rhi` / `crd-rhi-vulkan` split. Keeps the CPU BVH module CPU-only (no Vulkan dependency); future GPU geometry kernels land here too.
- **D133 (v9a-a)** — 30-bit Morton bit depth as v9a-a default. 60-bit u64 path filed as `v9a-60bit` follow-on for scenes at km-scale.
- **D134 (v9a-a)** — CPU reference IS the algorithm definition. Write CPU sequential first; GPU GLSL is a mechanical translation. Any divergence in GPU output vs CPU output is a bug, asserted by `bit_compare`.
- **D135 (v9a-a)** — `MortonGpuPipeline` value-type, ctor-cached pipeline objects, sync-compute via `graphics_queue()` (compute-family command pool absent from RHI). True async-compute = `v9a-a-async-compute` follow-on.
- **D136 (v9a-a close, 2026-05-18 — REVISED same day)** — v9a-a follow-ons ship IN-LINE before v9a-b1: `-typed` + `-60bit-cpu` + `-async-compute` + `-60bit-gpu`. Original deferral (D136 v1) overturned per user direction: "building substrate, not consumer-specific paths; ship fully now while harness is fresh." Substrate work ≠ speculative consumer-specific work — see refined [[ship-at-consumer-template-from-day-one]] rule. Forward-compat for v9a-b1 sort key width still achieved via `sort_morton_pairs<KeyT>` template, NOW with both `KeyT=u32` AND `KeyT=u64` instantiated and tested from day 1 (not just one).
- **D137 (v9a-a-typed)** — `morton_typed.hpp` strip-compute-retag wrappers shipped at v9a-a in-line. CPU + GPU entries; Morton codes stay raw `u32` (dimensionless bit indices, not lengths). `AABB3T<D, T>` typed AABB type at the API boundary.
- **D138 (v9a-60bit-cpu)** — u64 60-bit Morton CPU oracle shipped at v9a-a in-line. 20 bits per axis (~1M³ resolution). CPU sequential IS the algorithm definition (D134 discipline scales); GPU 60-bit (D140) is mechanical translation. CALIBRATION-FIRST tests + km-scale discriminator (30-bit collides where 60-bit resolves).
- **D139 (v9a-a-async-compute)** — `Device::create_command_buffer_for_queue(Queue&)` virtual appended (D135-compliant). Routes by pointer-identity per D9 contract. Vulkan backend lazy-creates a compute-family `VkCommandPool` when dedicated compute family exists. `MortonGpuPipeline::dispatch_morton_codes_async` opt-in path; output byte-identical to sync path, ValidationCapture stays silent under cross-queue-family submit.
- **D140 (v9a-60bit-gpu)** — `Device::supports_shader_int64()` virtual appended (D135-compliant). `shaderInt64` Vulkan core 1.0 feature probed + enabled at device init when supported. New `MortonGpu60BitPipeline` sibling class graceful-degrades to invalid pipeline if feature unavailable; consumer falls back to 30-bit `MortonGpuPipeline`. Sync-only at v9a (compute-family submit could be added by D139 pattern when a consumer asks).

## Follow-on slices — ✅ ALL 4 PAID 2026-05-18 (same day as v9a-a base)

Per user direction at v9a-a close: "We are building the engine substrate; ship it fully." Decision was to ship all four follow-ons in-line BEFORE v9a-b1, which we did. 5-config DoD PASS in 39 s.

| Follow-on | Status | Summary |
|---|---|---|
| `v9a-a-typed` | ✅ 2026-05-18 | `morton_typed.hpp` strip-compute-retag wrappers around `compute_morton_codes_cpu` + `MortonGpuPipeline::dispatch_morton_codes`. ADR-0078 §5 D34. ~50 LOC + 3 tests / 16 round-trip assertions. |
| `v9a-60bit-cpu` | ✅ 2026-05-18 | CPU oracle u64 60-bit path: `spread_bits_60` + `morton3_60bit_from_ints` + `quantize_to_morton_grid_20bit` + batch driver. CALIBRATION-FIRST lane-pinned tests + km-scale discriminator (30-bit collides at 1m/bin; 60-bit resolves at 1mm/bin). 7 tests / 59 assertions. |
| `v9a-a-async-compute` | ✅ 2026-05-18 | RHI surface: new `Device::create_command_buffer_for_queue(Queue&)` virtual (appended at END per D135). Vulkan backend lazy-creates compute-family `VkCommandPool`. `MortonGpuPipeline::dispatch_morton_codes_async` opt-in path. Cross-queue-family ValidationCapture silent on RTX 4070 dedicated compute family. 1 test / 9 assertions. |
| `v9a-60bit-gpu` | ✅ 2026-05-18 | New GLSL shader `compute_morton_codes_60bit.comp` + RHI `Device::supports_shader_int64()` capability accessor. `MortonGpu60BitPipeline` sibling class (graceful skip when feature unavailable). bit_compare<u64> CPU vs GPU byte-identical. 2 tests / 12 assertions. |

**Combined v9a-a + follow-ons**: 26 tests / 175 assertions across the new module. ValidationCapture silent everywhere. Pinned D137-D140 for ADR-0076 §25 amendment at v9a-close.

## v9a-b1 — CPU stable LSD radix sort of (Morton, index) pairs

```cpp
namespace crd::geometry::bvh_gpu {

template <typename KeyT>
struct MortonPair { KeyT code; u32 index; };

template <typename KeyT>
[[nodiscard]] Array<MortonPair<KeyT>>
sort_morton_pairs(ConstSpan<KeyT> codes, IAllocator* alloc) noexcept;

// Explicit instantiations: u32 (v9a-a 30-bit) + u64 (v9a-60bit-cpu 60-bit).
} // namespace crd::geometry::bvh_gpu
```

### Algorithm — stable LSD radix sort, 8-bit digit

Pure-integer stable LSD radix sort. 4 passes on u32 (4 bytes), 8 passes on u64 (8 bytes). Per pass: 256-bin histogram → exclusive prefix sum → stable scatter. Ping-pong buffer alternates `out` ↔ `aux`; pass count is even for both KeyT widths so the final result terminates back in `out` (static_assert enforces). One auxiliary allocation of `N × sizeof(MortonPair<KeyT>)` bound to caller's `IAllocator`.

**Stability contract** (phase-doc + `morton.hpp` tiebreak): equal Morton codes ⇒ lower input index wins. Built into the input pair sequence `{codes[i], u32(i)}` with monotonic-ascending `i` — LSD-radix's natural stability delivers the contract for free.

**Divergence from phase-doc literal text (D141):** the phase-doc row reads "Sort `(morton_code, original_index)` pairs via `crd::containers::sort`". We chose the other tagged option (slice-start pin: "CPU sort directly **or radix-sort wrapper**"). Reason: published 1 ms / 1 M-element budget is unattainable under O(N log N) merge sort (~50–100 ms measured). LSD radix is O(N k) at k=4 (u32) / k=8 (u64) — measured ~24 ms shipping for u32 1 M on the dev box, well within the tiered budget. Karras 2012 §4 names radix sort as the canonical LBVH sort step.

### Determinism

Pure deterministic function of the input span. No FP, no hashing, no platform-conditional code paths. Bit-identical output across MSVC / GCC / clang × x64 / ARM64. The discriminating tests:

- 10 000-element random input, byte-identical (`memcmp == 0`) against `crd::containers::sort` over a lexicographic `(code, index)` comparator (the oracle).
- Two-runs determinism test — same input, two invocations, byte-identical results.
- u64 upper-32-bits discriminator — three pairs sharing low 32 bits but differing in upper 32, sort by upper bits proves passes 5–8 are wired correctly. A bit-shift typo silently passing the u32 suite would loudly fail here.

### Performance

Tiered budget per `feedback_v9_gpu_sanity_harness` (NDEBUG vs win-debug):

| KeyT | NDEBUG budget | Debug budget | Why |
|---|---|---|---|
| u32 (4 passes) | 20 ms / 1 M | 2000 ms / 1 M | ~4 × headroom over realistic shipping (~5 ms) |
| u64 (8 passes) | 40 ms / 1 M | 4000 ms / 1 M | 2× pass count; same headroom |

The headline "1 ms / 1 M" target from research §4.1 is aspirational — single-thread scalar code at 8-bit digit hits ~3–5 ms shipping on a modern x64 box. Filed `v9a-b1-simd` follow-on for AVX2 vectorised histogram + scatter when a consumer demands it.

## Pinned design decisions (carried for ADR-0076 §25 amendment at v9a-close)

- **D132 (v9a-a)** — Module structure: NEW sibling module `crd-geometry-bvh-gpu`, NOT additions to existing `crd-geometry-bvh`. Mirror of `crd-rhi` / `crd-rhi-vulkan` split. Keeps the CPU BVH module CPU-only (no Vulkan dependency); future GPU geometry kernels land here too.
- **D133 (v9a-a)** — 30-bit Morton bit depth as v9a-a default. 60-bit u64 path filed as `v9a-60bit` follow-on for scenes at km-scale.
- **D134 (v9a-a)** — CPU reference IS the algorithm definition. Write CPU sequential first; GPU GLSL is a mechanical translation. Any divergence in GPU output vs CPU output is a bug, asserted by `bit_compare`.
- **D135 (v9a-a)** — `MortonGpuPipeline` value-type, ctor-cached pipeline objects, sync-compute via `graphics_queue()` (compute-family command pool absent from RHI). True async-compute = `v9a-a-async-compute` follow-on.
- **D136 (v9a-a close, 2026-05-18 — REVISED same day)** — v9a-a follow-ons ship IN-LINE before v9a-b1: `-typed` + `-60bit-cpu` + `-async-compute` + `-60bit-gpu`. Original deferral (D136 v1) overturned per user direction: "building substrate, not consumer-specific paths; ship fully now while harness is fresh." Substrate work ≠ speculative consumer-specific work — see refined [[ship-at-consumer-template-from-day-one]] rule. Forward-compat for v9a-b1 sort key width still achieved via `sort_morton_pairs<KeyT>` template, NOW with both `KeyT=u32` AND `KeyT=u64` instantiated and tested from day 1 (not just one).
- **D137 (v9a-a-typed)** — `morton_typed.hpp` strip-compute-retag wrappers shipped at v9a-a in-line. CPU + GPU entries; Morton codes stay raw `u32` (dimensionless bit indices, not lengths). `AABB3T<D, T>` typed AABB type at the API boundary.
- **D138 (v9a-60bit-cpu)** — u64 60-bit Morton CPU oracle shipped at v9a-a in-line. 20 bits per axis (~1M³ resolution). CPU sequential IS the algorithm definition (D134 discipline scales); GPU 60-bit (D140) is mechanical translation. CALIBRATION-FIRST tests + km-scale discriminator (30-bit collides where 60-bit resolves).
- **D139 (v9a-a-async-compute)** — `Device::create_command_buffer_for_queue(Queue&)` virtual appended (D135-compliant). Routes by pointer-identity per D9 contract. Vulkan backend lazy-creates a compute-family `VkCommandPool` when dedicated compute family exists. `MortonGpuPipeline::dispatch_morton_codes_async` opt-in path; output byte-identical to sync path, ValidationCapture stays silent under cross-queue-family submit.
- **D140 (v9a-60bit-gpu)** — `Device::supports_shader_int64()` virtual appended (D135-compliant). `shaderInt64` Vulkan core 1.0 feature probed + enabled at device init when supported. New `MortonGpu60BitPipeline` sibling class graceful-degrades to invalid pipeline if feature unavailable; consumer falls back to 30-bit `MortonGpuPipeline`. Sync-only at v9a (compute-family submit could be added by D139 pattern when a consumer asks).
- **D141 (v9a-b1)** — CPU sort algorithm = stable LSD radix sort with 8-bit digit, **NOT** `crd::containers::sort` (merge sort). Documented divergence from phase-doc literal text "via `crd::containers::sort`" — phase-doc explicitly tags both options at slice start; radix chosen for the 1 ms / 1 M budget that O(N log N) merge sort cannot meet (~50–100 ms measured). Karras 2012 §4 names radix sort as the canonical LBVH sort step. AVX2 vectorisation filed as `v9a-b1-simd` follow-on (consumer-pull, not substrate).
- **D142 (v9a-b1)** — Pair layout = AoS `struct MortonPair<KeyT> { KeyT code; u32 index; }`. `sizeof(MortonPair<u32>) == 8` (4+4); `sizeof(MortonPair<u64>) == 16` (8+4+4 trailing padding for u64 align). Static_asserted to lock the GPU SSBO layout contract for v9a-b2. SoA gain is marginal at 8-bit digit + L1-fitting histogram + 8/16-byte pair; AoS keeps the v9a-b2 GPU upload one-shot.
- **D143 (v9a-b1)** — Stability achieved via LSD-radix's natural property + monotonic-ascending input-index construction. Equal Morton codes ⇒ lower input index wins emerges automatically; no separate tiebreak pass. The discriminating test is the all-equal-keys case + the partial-tie case + the 10 k oracle cross-check against lex `(code, index)` sort.
- **D144 (v9a-b1)** — Pair index width fixed at `crd::u32`. 4 B-element ceiling (input.size() ≤ UINT32_MAX) asserted at entry. Lifts to `u64` if/when a downstream consumer needs > 4 G primitives; not speculative-engineered today.
- **D146 (v9a-b2)** — GPU radix scatter = prefix-sum scatter, NOT atomic-counter scatter. Output is bit-deterministic across runs *and* byte-identical to the v9a-b1 CPU oracle. Divergence from phase-doc "throughput-tier (non-deterministic by atomic ordering)" original description: pinned at slice start because at 4-bit digit + 1024 items/block, the atomic-counter throughput advantage is marginal vs the value of byte-identical-to-CPU conformance (cheap `bit_compare` oracle catches GPU bugs immediately). A `v9a-b2-atomics` future variant can trade determinism for throughput if a real consumer ever asks.
- **D147 (v9a-b2)** — Hard-cap N at `kRadixMaxItems = 1 M` today. Scan kernel handles exactly `kRadixMaxBlocks = 1024` blocks per dispatch in a single workgroup via per-bin Hillis-Steele scan (5 KiB shared memory). Beyond requires a recursive-scan kernel — filed as `v9a-b2-large` follow-on.
- **D148 (v9a-b2)** — Pipeline geometry: workgroup size 256 × items/thread 4 = 1024 items per block; scan workgroup 256 × items/thread 4 = 1024 block-entries per scan; histogram and scatter each dispatch `num_blocks` workgroups. Shared memory budget (scan): `s_scan[1024]` (4 KiB) + `s_thread_sum[256]` (1 KiB) + `s_chunk_sum[256]` (1 KiB) + `s_bin_total[16]` (64 B) + `s_bin_offset[16]` (64 B) ≈ 6 KiB — comfortable inside the Vulkan-portable 16 KiB floor. Constants live in `radix_sort.hpp` as `kRadix*` with paired static_asserts that mirror the GLSL hard-coded forms.
- **D149 (v9a-b2)** — Pair SSBO layout: `MortonPair<u32>` ↔ flat `uint pairs[]` array as `(code, index)` at offsets `[2i+0]`, `[2i+1]`. sizeof 8, alignof 4 locked by static_assert in `morton_sort.hpp`. Pairs ping-pong from pass 0 (init kernel packs into `pairs_a`, then `pair_in/pair_out` alternates by parity each pass). Even pass count (8 for 30-bit/4-bit-digit) ⇒ final output terminates in `pairs_a` — same parity rule as CPU v9a-b1.

## v9a-b1-parallel — 3-phase parallel CPU radix sort via crd-jobs

```cpp
namespace crd::geometry::bvh_gpu {

inline constexpr u32 kDefaultParallelSortThreshold = 65536U;

template <typename KeyT>
[[nodiscard]] Array<MortonPair<KeyT>>
sort_morton_pairs_parallel(ConstSpan<KeyT> codes, IAllocator* alloc,
                           u32 num_jobs = 0U,                                // 0 = auto via num_workers()
                           u32 parallel_threshold = kDefaultParallelSortThreshold) noexcept;
} // namespace crd::geometry::bvh_gpu
```

**Algorithm — 3-phase deterministic parallel radix, one pass at a time:**

```
Phase 1 (parallel, num_jobs workers):
  Each chunk i scans input[i*N/nj .. (i+1)*N/nj)
  -> hist_tiles[i][bucket] = local count

Phase 2 (serial, num_jobs * 256 ops):
  bucket_total[B] = Σ over chunks of hist_tiles[chunk][B]
  bucket_start[B] = exclusive_prefix(bucket_total)
  scatter_off[c][B] = bucket_start[B] + Σ_{j<c} hist_tiles[j][B]
  // Chunk c writes to dst[scatter_off[c][B] ..< scatter_off[c+1][B]] in bucket B.

Phase 3 (parallel, same chunking):
  Each chunk i scatters input[i*N/nj .. (i+1)*N/nj)
  into its disjoint output range using scatter_off[i][...].
```

**Stability across worker boundaries (D153)** — the load-bearing invariant. Within a chunk: src visited L→R, per-bucket offset bumped monotonically. Across chunks: `scatter_off[c+1][B] > scatter_off[c][B]` strictly, because chunk c+1's offset adds chunk c's count. So equal-key items from earlier chunks land before later chunks → global monotonic-input-index stability holds. The discriminating tests are 4 096 equal keys spanning all workers + two equal keys placed across-chunk + byte-identity at num_jobs ∈ {1, 2, 4, 8, 16}.

**Performance** (1 M u32 win-shipping median-of-5, 8-core dev box):
- Serial scalar+prefetch (v9a-b1-simd): 4.77 ms
- **Parallel 8 workers (v9a-b1-parallel): 2.56 ms → 1.86×**
- Budget headroom: 7.8× over 20 ms NDEBUG budget

Bandwidth-bound, not core-bound: all cores share L3/RAM, so the 8-core ratio caps below 8× (D155). Realistic single-machine ceiling at this scale is ~2-3×; for sub-ms throughput use the GPU path (v9a-b2).

**Fallback path** — the function transparently falls back to `sort_morton_pairs<KeyT>` (serial scalar+prefetch) when:
1. `crd::jobs` not initialised (`num_workers() == 0`).
2. `num_jobs <= 1` (no parallelism to extract).
3. `codes.size() < parallel_threshold` (per-pass parallel overhead exceeds the saving).

This means consumers can call `sort_morton_pairs_parallel` unconditionally — it never errors on missing-jobs-init and never underperforms serial on tiny inputs.

## v9a-b2 — GPU LSD radix sort

```cpp
namespace crd::geometry::bvh_gpu {

class MortonRadixGpuPipeline
{
public:
    MortonRadixGpuPipeline(rhi::Device&, StringView shader_dir) noexcept;
    bool is_valid() const noexcept;

    // Sort `codes` ascending; equal-key pairs by ascending input index.
    // Output byte-identical to v9a-b1 `sort_morton_pairs<u32>` (D146).
    // N <= kRadixMaxItems = 1M (D147 cap); 25 dispatches + fence-wait readback.
    Array<MortonPair<u32>> dispatch_radix_sort(
        ConstSpan<u32> codes, IAllocator* alloc) noexcept;
};

constexpr u32 kRadixWorkgroupSize    = 256U;
constexpr u32 kRadixItemsPerThread   = 4U;
constexpr u32 kRadixItemsPerBlock    = 1024U;
constexpr u32 kRadixMaxBlocks        = 1024U;
constexpr u32 kRadixMaxItems         = 1U << 20;
constexpr u32 kRadixDigitBits        = 4U;
constexpr u32 kRadixBins             = 16U;
constexpr u32 kRadixNumPassesU32     = 8U;
} // namespace crd::geometry::bvh_gpu
```

### Pipeline body — 25 dispatches per sort

```
1. upload codes ─stage→ codes_gpu
2. init kernel:        pack (code, gl_GlobalInvocationID.x) → pairs_a
3. for pass in 0..7 (8 passes × 3 dispatches = 24):
     a. histogram(pair_in)        → block_hist          (256 thr × 4 itm; atomicAdd on shared)
     b. scan(block_hist)          → scatter_off         (single-WG per-bin Hillis-Steele scan + bin-offset patch)
     c. scatter(pair_in, scatter_off) → pair_out        (per-slot bin tag + serial-rank-within-bin + scatter)
     swap pair_in / pair_out roles
4. After 8 (even) passes, final output is in pairs_a.
5. copy_buffer pairs_a → readback (host visible).
6. fence wait + map + memcpy into caller-owned Array<MortonPair<u32>>.
```

### Stability discipline (three layers)

1. **Init** sets `pair.index = gl_GlobalInvocationID.x` — the same monotonic-i sequence CPU v9a-b1 uses.
2. **Scatter computes local rank within bin via per-slot serial scan** over `s_bin_of_slot[1024]` (count prior slots with the same bin), NOT an atomic counter. Equal-bin items within a workgroup land in monotonic thread-id (= input slot) order.
3. **Cross-workgroup ordering** comes from the scan kernel's per-bin global prefix — `scatter_off[wg, bin]` is the running sum of prior workgroups' counts for that bin. Equal-bin items across workgroups land in monotonic workgroup-id order.

End result: byte-identical to CPU LSD radix, asserted by `bit_compare<MortonPair<u32>>(cpu, gpu)` over the full 1 M dataset.

### Test corpus (9 cases / 8 257 assertions)

| Section | Cases | Notes |
|---|---|---|
| **CALIBRATION** | 1 | N=16 hand-rolled, GPU byte-identical to CPU oracle. Failure ⇒ everything downstream is meaningless. |
| Trivial shape | 2 | empty / N=1 |
| **STABILITY discriminator** | 1 | 4 096 identical codes spanning 4 workgroups — output indices must be 0..4095 both within AND across WGs. |
| **Bullet-proof oracle** | 2 | 10 000 + 262 144 random codes, `bit_compare<MortonPair<u32>>` byte-identical to v9a-b1 CPU oracle. |
| Determinism | 1 | `gpu_determinism_check` 3 dispatches byte-identical (D146 contract). |
| Integration | 1 | `compute_morton_codes_cpu` → `dispatch_radix_sort` end-to-end LBVH-pipeline candidate. |
| **Perf budget** | 1 | `CRD_PERF_BUDGET_LE` 30 ms NDEBUG / 30000 ms debug for 1 M items end-to-end. |

`ValidationCapture` asserted silent on every dispatching test.

## Follow-on slices — ✅ ALL 4 PAID 2026-05-18 (same day as v9a-a base)

Per user direction at v9a-a close: "We are building the engine substrate; ship it fully." Decision was to ship all four follow-ons in-line BEFORE v9a-b1, which we did. 5-config DoD PASS in 39 s.

| Follow-on | Status | Summary |
|---|---|---|
| `v9a-a-typed` | ✅ 2026-05-18 | `morton_typed.hpp` strip-compute-retag wrappers around `compute_morton_codes_cpu` + `MortonGpuPipeline::dispatch_morton_codes`. ADR-0078 §5 D34. ~50 LOC + 3 tests / 16 round-trip assertions. |
| `v9a-60bit-cpu` | ✅ 2026-05-18 | CPU oracle u64 60-bit path: `spread_bits_60` + `morton3_60bit_from_ints` + `quantize_to_morton_grid_20bit` + batch driver. CALIBRATION-FIRST lane-pinned tests + km-scale discriminator (30-bit collides at 1m/bin; 60-bit resolves at 1mm/bin). 7 tests / 59 assertions. |
| `v9a-a-async-compute` | ✅ 2026-05-18 | RHI surface: new `Device::create_command_buffer_for_queue(Queue&)` virtual (appended at END per D135). Vulkan backend lazy-creates compute-family `VkCommandPool`. `MortonGpuPipeline::dispatch_morton_codes_async` opt-in path. Cross-queue-family ValidationCapture silent on RTX 4070 dedicated compute family. 1 test / 9 assertions. |
| `v9a-60bit-gpu` | ✅ 2026-05-18 | New GLSL shader `compute_morton_codes_60bit.comp` + RHI `Device::supports_shader_int64()` capability accessor. `MortonGpu60BitPipeline` sibling class (graceful skip when feature unavailable). bit_compare<u64> CPU vs GPU byte-identical. 2 tests / 12 assertions. |

**Combined v9a-a + follow-ons**: 26 tests / 175 assertions across the new module. ValidationCapture silent everywhere. Pinned D137-D140 for ADR-0076 §25 amendment at v9a-close.

## Coming slices

Plan: `docs/phases/phase-3.1.7-geometry.md`. ADR-0076 §25 amendment locks v9a cluster decisions at v9a-close.

| Slice | Status | Summary |
|---|---|---|
| v9a-a  Morton codes      | ✅ 2026-05-18 | 30-bit Morton CPU+GPU, byte-identical conformance. |
| v9a-b1 CPU radix sort    | ✅ 2026-05-18 | Stable LSD radix sort, template KeyT ∈ {u32, u64}. 20 tests / 40 134 assertions. |
| v9a-b1-simd scalar+prefetch | ✅ 2026-05-18 | `_mm_prefetch` 8 iters ahead in scatter; +7% at no complexity cost. SOTA techniques ruled out via research. D145/D150/D151. |
| v9a-b1-parallel 3-phase | ✅ 2026-05-18 | Opt-in `sort_morton_pairs_parallel<KeyT>` via crd-jobs. Per-(chunk, bucket) offset-table stability invariant. **1.86× → 2.56ms/1M u32**. 8 tests / 400 025 assertions. D152-D155. |
| v9a-b2 GPU radix sort    | ✅ 2026-05-18 | 4-bit-digit × 8 passes, prefix-sum scatter ⇒ byte-identical to v9a-b1 (D146). 9 tests / 8 257 assertions. |
| v9a-c LBVH tree + upsweep | ✅ 2026-05-18 | Elite-combine slice (absorbs originally-separate v9a-d). Karras 2012 §2.2 tree-build + §2.4 atomic-counter parent-walk upsweep. **`coherent` + `memoryBarrierBuffer()` load-bearing** (Lesson 09). 12 tests / 379 021 assertions. D156-D164. |
| v9a-c  LBVH tree         | 📋 planned    | Karras 2012 binary tree from sorted morton codes. |
| v9a-d  AABB upsweep      | 📋 planned    | Bottom-up parent-AABB propagation; atomic-on-parent. |
| v9a-close                | 📋 planned    | §25 amendment + first-light smoke + 18-config sweep. |

## Bug surfaced

Phase 3.1.7 v9a-a is the FIRST consumer to attach a `ValidationCapture` to a real `VkInstance`. It surfaced a pre-existing rhi-vulkan bug: device unconditionally enables `VK_KHR_swapchain` without the instance enabling `VK_KHR_surface` first (VUID-vkCreateDevice-ppEnabledExtensionNames-01387). When GLFW is initialised, `glfwGetRequiredInstanceExtensions` adds VK_KHR_surface; without GLFW (typical compute-only tests), nothing did. **Fixed in `engine/rhi-vulkan/src/vulkan_backend.cpp` as part of v9a-a** by unconditionally adding `VK_KHR_surface` to enabled instance extensions when available, defensively. Per `feedback_never_defer_solve`: solve, don't defer.
