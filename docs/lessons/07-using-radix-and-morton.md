# Lesson 07 — Using radix and Morton in real consumers

> **The question that motivated this lesson:** "How would you use these — just tell me. Where, and for what?"

This lesson is concrete. For each planned consumer of `crd-geometry-bvh-gpu`, it shows the call sequence, which sort variant to use, and where the headache spots are. No theory; just recipes.

## TL;DR — pick the right variant

| Consumer | Sort variant | Why |
|---|---|---|
| GPU LBVH builder (v9a-c, planned) | `MortonRadixGpuPipeline::dispatch_radix_sort` | Data is GPU-resident; consumer is GPU-resident |
| Eylem broadphase initial build | `sort_morton_pairs_parallel<u32>` | One-shot at scene-load on CPU |
| Eylem broadphase per-frame | NO SORT (use `bvh_refit`) | Topology unchanged; sort isn't needed per-frame |
| Cooker LBVH bake | `sort_morton_pairs<u32>` (serial) | Offline, single-threaded cooker process |
| Mesh-collider bake | `sort_morton_pairs<u32>` (serial) | Same as above; per-mesh, one-shot |
| GPU-driven rendering culling | `MortonRadixGpuPipeline::dispatch_radix_sort` | Per-frame on GPU; data stays GPU-side |
| V-HACD convex-collider sort | `sort_morton_pairs<u32>` (serial) | Already shipped; per-asset cook |

## Recipe 1: GPU LBVH builder (the v9a-c slice that's coming next)

**Goal:** rebuild a BVH every frame from primitives whose AABBs are already in GPU memory (e.g., particle simulation, GPU-driven scene update).

**Pipeline:**

```cpp
// All buffers stay GPU-resident; no host round-trips between stages.

// Stage 1: Morton codes (v9a-a, already shipped)
MortonGpuPipeline morton_gpu(device, shader_dir);
morton_gpu.dispatch_morton_codes(
    /*aabbs_gpu_buffer=*/ scene_aabbs,
    /*scene_bounds=*/      scene_bounds,
    /*out=*/               morton_codes_gpu_buffer);

// Stage 2: Stable radix sort (v9a-b2, already shipped)
MortonRadixGpuPipeline radix_gpu(device, shader_dir);
auto sorted_pairs_gpu = radix_gpu.dispatch_radix_sort(
    morton_codes_gpu_buffer,
    alloc);

// Stage 3: Karras tree (v9a-c, PLANNED — does not exist yet)
LbvhGpuPipeline lbvh_gpu(device, shader_dir);
auto tree_gpu = lbvh_gpu.build_tree(sorted_pairs_gpu, alloc);

// Stage 4: AABB upsweep (v9a-d, PLANNED)
auto tree_with_aabbs = lbvh_gpu.upsweep_aabbs(tree_gpu, scene_aabbs, alloc);

// Stage 5: actual consumer — e.g., GPU frustum cull or raytracer dispatches
//          a compute shader that walks `tree_with_aabbs`.
```

**Pinning down N:** This works up to `kRadixMaxItems = 1 M` (D147 cap). Beyond requires the `v9a-b2-large` recursive-scan follow-on.

**Headache:** stage transitions need barriers (`cmd->buffer_barrier(...)` from `ComputeShaderWrite` → `ComputeShaderRead`). The radix internally handles its own barriers; consumer must add the barriers between stages 1→2 and 2→3.

**When this becomes critical:** Phase 3.1.7 v9a-c (next slice). Estimated arrival: ~4 days from now.

## Recipe 2: Eylem broadphase initial build (CPU)

**Goal:** at scene-load, build a BVH over all rigid bodies' AABBs on CPU. Per-frame, **refit** the existing tree (topology unchanged) — do NOT re-sort.

**Pipeline:**

```cpp
// One-time at scene load.
crd::jobs::init({.num_threads = num_workers});

// Step 1: get AABBs from the rigid-body component storage.
crd::containers::Array<AABB3<f32>> body_aabbs(alloc);
for (const auto& body : world.query<RigidBody3D>()) {
    body_aabbs.push_back(body.world_aabb());
}

// Step 2: Morton codes (CPU).
const AABB3<f32> scene_bounds = compute_scene_bounds(body_aabbs);
const auto morton_codes = compute_morton_codes_cpu(
    {body_aabbs.data(), body_aabbs.size()},
    scene_bounds,
    alloc);

// Step 3: PARALLEL sort (the v9a-b1-parallel path).
const auto sorted_pairs = sort_morton_pairs_parallel<u32>(
    {morton_codes.data(), morton_codes.size()},
    alloc,
    /*num_jobs=*/ 0U,                // 0 = auto (uses jobs::num_workers())
    /*threshold=*/ kDefaultParallelSortThreshold);

// Step 4: build the LBVH tree on CPU (uses sorted_pairs as input order).
//         The Karras tree-from-sorted-codes algorithm is the same on CPU
//         as on GPU; CPU implementation is the algorithm definition.
auto bvh_tree = bvh_build_from_lbvh(sorted_pairs, body_aabbs, alloc);

// (Optional) upload tree to GPU for visualization or GPU-side queries.
gpu_upload_bvh(bvh_tree);
```

**Per-frame** (NO sort, NO Morton):

```cpp
// Update each body's world AABB (each body's transform changed).
for (auto& body : world.query<RigidBody3D>()) {
    body.update_world_aabb();
}

// Refit the tree in place — same topology, just update AABBs bottom-up.
bvh_refit(bvh_tree, body_aabbs);

// Now do broadphase queries (overlap, raycast, etc.) against the refitted tree.
```

**When sort comes back into play:** if the scene radically changes (new objects added, old ones removed), do a full **rebuild**, not just a refit. Refit handles topology-stable updates; new topology needs new sort.

**Headache:** `crd::jobs::init` must be called before `sort_morton_pairs_parallel` for the parallel path to engage. If not init'd, the function silently falls back to serial (no error, just slower).

## Recipe 3: Cooker LBVH bake (offline, per-asset)

**Goal:** at asset-cook time, build a BVH for each mesh-collider and serialize it into the cooked artifact. Runtime loads the prebuilt tree — no Morton/sort/build at runtime.

**Pipeline:**

```cpp
// Inside the asset cooker, per mesh-collider:
const auto& triangles = source_mesh.triangles;
crd::containers::Array<AABB3<f32>> tri_aabbs(alloc);
tri_aabbs.resize(triangles.size());
for (usize i = 0; i < triangles.size(); ++i) {
    tri_aabbs[i] = aabb_of_triangle(triangles[i]);
}

const AABB3<f32> mesh_bounds = compute_bounds(tri_aabbs);
const auto morton_codes = compute_morton_codes_cpu(
    {tri_aabbs.data(), tri_aabbs.size()}, mesh_bounds, alloc);

// Serial sort — cooker is single-threaded; per-asset cook time is
// dominated by I/O, so the sort doesn't need to be fast.
const auto sorted_pairs = sort_morton_pairs<u32>(
    {morton_codes.data(), morton_codes.size()}, alloc);

auto bvh_tree = bvh_build_from_lbvh(sorted_pairs, tri_aabbs, alloc);

// Serialize the BVH into the cooked artifact.
cooker_output.write_section("BVH", bvh_tree.to_bytes());
```

**Why not parallel here?** Asset cookers usually run with the workload itself parallelized (one worker per asset), so the per-asset sort uses just one core's worth of CPU anyway. The parallel sort's overhead would cut into other concurrent cooks.

**Why not GPU?** Cookers run as host-side processes (often headless). Spinning up Vulkan + uploading data + waiting on a fence costs more than the sort itself. The serial CPU is the right tool.

**Headache:** the cooker output format needs to match the runtime's expected BVH layout. Cerid uses CRDR sections for this; the BVH layout is locked in `crd-geometry-bvh`'s public surface.

## Recipe 4: GPU-driven rendering occlusion culling (Phase 3.5+ planned)

**Goal:** per-frame, sort visible AABBs by something useful (depth, screen-space tile, render-state key) on the GPU, then process them in that order via an indirect-draw compute pass.

**Pipeline:**

```cpp
// CPU side just kicks off the GPU pipeline; no host data round-trip.

// Step 1 — a compute shader writes per-visible-object Morton codes from
//          screen-space coordinates (or whatever sort key applies).
ssd_morton_pipeline.dispatch(visible_aabbs_gpu, viewport_bounds);

// Step 2 — GPU radix sort.
auto sorted_visible = radix_gpu.dispatch_radix_sort(visible_morton_codes_gpu, alloc);

// Step 3 — indirect-draw compute shader walks sorted_visible, writes
//          VkDrawIndirectCommand structs based on visibility / culling rules.
indirect_draw_setup.dispatch(sorted_visible, draw_commands_buffer);

// Step 4 — actual rendering uses the prebuilt draw commands.
cmd->draw_indexed_indirect_count(draw_commands_buffer, ...);
```

**Why GPU radix here?** Everything stays on the GPU. No transfers. The sort's pure-compute cost (~1-2 ms) is the only cost.

**Headache:** the GPU radix has a 1 M element cap (D147). For scenes with > 1 M visible objects per frame (rare, but possible), need the `v9a-b2-large` follow-on.

**When this becomes critical:** Phase 3.5+ when the renderer adds GPU-driven culling. Not soon.

## Recipe 5: Spatial hash / particle simulation sort

**Goal:** sort particles by spatial cell for efficient neighbor lookup (SPH/PIC/MPM, fluids, cloth).

**Pipeline (CPU variant):**

```cpp
// Particle positions live in a `Vec3<f32>` array.
crd::containers::Array<u32> cell_keys(alloc);
cell_keys.resize(particles.size());
for (usize i = 0; i < particles.size(); ++i) {
    cell_keys[i] = spatial_hash(particles[i].pos, cell_size);  // returns u32
}

// Sort particles by cell key. Same algorithm; "Morton code" generalizes
// to "any 32-bit sort key."
const auto sorted_pairs = sort_morton_pairs_parallel<u32>(
    {cell_keys.data(), cell_keys.size()}, alloc);

// Now iterate sorted_pairs: particles with the same cell key are
// consecutive in the sorted array. Build a per-cell start-index table
// in one pass and use it for O(1) cell-neighbor lookups.
```

**Pipeline (GPU variant):**

Same shape but using GPU dispatches. Especially good for SPH because the particle simulation is already GPU-resident.

**Why this works for non-Morton keys:** `sort_morton_pairs<KeyT>` doesn't actually care that the key is a Morton code. The name is historical; the function is a "stable radix sort of (u32, u32) pairs." Any spatial hash, depth key, or 32-bit sort key works.

(If we ever need a generic name, we'd rename. For now the codebase is small enough that the `morton`-prefixed identifiers are clear.)

## Recipe 6: V-HACD convex-collider sort (shipped at v9c)

**Goal:** when V-HACD produces N convex hulls for a mesh, sort them for stable iteration order in physics queries.

**Already in production.** See `engine/geometry-decomposition/`. Uses serial CPU sort because cooker-side, per-asset, fast enough.

## Part — Common pitfalls and how to avoid them

### Pitfall 1: Using `sort_morton_pairs` (serial) on the per-frame hot path

If you have a CPU-resident BVH that needs a full rebuild per frame (rare, but possible — e.g., topology changes from particle birth/death), use **`sort_morton_pairs_parallel`**, not the serial version. The 1.86× speedup matters at 60 Hz.

### Pitfall 2: Forgetting `crd::jobs::init`

The parallel sort falls back to serial when jobs isn't initialized. **No error**; just slower. If you expect parallelism and aren't getting the speedup, check that your test binary has the Catch2 listener pattern:

```cpp
struct YourJobsListener final : Catch::EventListenerBase {
    void testRunStarting(...) override {
        crd::jobs::init(crd::jobs::Config{.num_threads = 8U});
    }
    void testRunEnded(...) override { crd::jobs::shutdown(); }
};
CATCH_REGISTER_LISTENER(YourJobsListener)
```

For runtime code, `crd::jobs::init` should be called once in `Application::run()` before the first frame.

### Pitfall 3: Round-tripping data through the GPU "just to use GPU radix"

If your producer is CPU and consumer is CPU, **do not** push the data to GPU just to sort it. The upload + readback (~1-2 ms) erases any GPU-compute savings. Use the parallel CPU sort.

### Pitfall 4: Mixing 30-bit and 60-bit codes within one pipeline

The sort is templated over `KeyT ∈ {u32, u64}`. Both work; both ship explicit instantiations. But the downstream Karras tree builder (v9a-c) will be coded for a specific bit width. Don't sort 60-bit codes and then feed them to a 30-bit tree builder — the bits don't line up. Pick a width at scene/system-init time and stick with it.

### Pitfall 5: Re-allocating output buffers per call

Every `sort_morton_pairs*` call allocates two N-pair buffers (output + ping-pong aux). At 1 M elements that's 16 MB. If you're calling this every frame, **pass a long-lived TLSF allocator** so the memory is recycled, not re-mmapped. Cerid's standard pattern: per-frame arena + scratch allocator.

## What to read next

- [Lesson 01 — Morton codes, radix sort, and the LBVH pipeline](01-morton-codes-and-lbvh-pipeline.md) — what these consumers compose into.
- [Lesson 05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) — the underlying reason GPU radix isn't always the right answer.
- [Lesson 04 — Parallel stable merge](04-parallel-stable-merge.md) — the substrate template that makes per-frame CPU rebuilds tractable.
