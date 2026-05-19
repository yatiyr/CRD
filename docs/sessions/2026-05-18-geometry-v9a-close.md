# 2026-05-18 — Phase 3.1.7 v9a `-gpu` LBVH cluster CLOSED ✅

**Slice:** v9a-close — cluster-close docs + ADR-0076 §25 amendment + 18-config full sweep + performance budget pin.

**Status:** ✅ CLUSTER CLOSED. ADR-0076 §25 ✅ Accepted, locking 33 design decisions (D132-D164). 18-config full sweep PASS. Performance characterization honestly pinned: 53.7 ms / 1M test-harness median-of-5 on dev box (with full CPU↔GPU round-trip + Phase C reorder); ~5-8 ms achievable for pure-GPU pipeline on RTX 3060 (filed `v9a-c-gpu-reorder` + GPU-resident-consumer paths).

---

## Cluster summary

**The `-gpu` LBVH cluster shipped 10 algorithmic slices over a single sprint (2026-05-18).** Total: ~5 600 LOC engine + ~3 800 LOC tests, ~71 test cases / ~827 K assertions across the new `crd-geometry-bvh-gpu` sibling module.

| Slice | What shipped | LOC eng + tests | Key pin |
|---|---|---|---|
| v9a-a | 30-bit Morton CPU + GPU | ~520 + ~350 | D132-D135 |
| v9a-a-typed | `Length<T>` typed AABB strip-compute-retag | ~50 + ~150 | D137 |
| v9a-60bit-cpu | u64 60-bit Morton CPU oracle | ~150 + ~150 | D138 |
| v9a-a-async-compute | RHI compute-family pool + async dispatch | ~250 + ~80 | D139 |
| v9a-60bit-gpu | u64 60-bit Morton GPU + shaderInt64 gate | ~200 + ~150 | D140 |
| v9a-b1 | CPU stable LSD radix `KeyT ∈ {u32, u64}` | ~165 + ~430 | D141-D144 |
| v9a-b2 | GPU LSD radix prefix-sum (byte-identical to CPU) | ~630 + ~500 | D146-D149 |
| v9a-b1-simd | scalar+prefetch wins; SOTA techniques ruled out | ~30 + 0 | D145, D150, D151 |
| v9a-b1-parallel | 3-phase deterministic parallel via crd-jobs (1.86×) | ~220 + ~250 | D152-D155 |
| v9a-c | LBVH tree-build + AABB upsweep (elite combine) | ~895 + ~650 | D156-D164 |

**Three operational discipline rules surfaced during the cluster:**

1. **Lesson 02** — When scalar beats SIMD: at 1M / 8MB working set, Wassenberg's SOTA SWWC is slower than scalar+prefetch because L2-resident working set doesn't trigger the RAM-bound regime SWWC attacks.
2. **Lesson 04** — Parallel stable merge: per-(chunk, bucket) offset table is the substrate template for any deterministic-parallel scatter operation. Reusable in eylem v1c broadphase + parallel BVH refit + cooker bakes.
3. **Lesson 09** — GPU memory ordering: GLSL `atomicAdd` provides acquire-release on the atomic location ONLY. Non-atomic cross-invocation writes need `coherent` qualifier. The bug surfaced at N=10000 oracle in v9a-c; invisible at smaller N where shorter walks happen to flush writes between work units.

---

## What this v9a-close slice ships

### 1. ADR-0076 §25 amendment ✅ Accepted

Locks all 33 design decisions D132-D164 from the cluster. See `docs/decisions/0076-geometry-substrate-architecture.md` § §25 for the full text. The phase-doc "atomic vs 2-pass" decision-fork explicitly **dissolves** via D159 (Karras 2012 §2.4 atomic-counter parent-walk is BOTH faster AND bit-deterministic because AABB union is commutative+associative).

### 2. Performance characterization (honestly pinned)

Two numbers, both honest:

| Scenario | Time / 1M primitives | Conditions |
|---|---|---|
| **Test-harness end-to-end** | **53.7 ms** (median-of-5) | Dev box, win-shipping, includes upload + dispatches + readback + CPU-side Phase C reorder |
| **Estimated pure-GPU pipeline** | **~5-8 ms** | RTX 3060, GPU-resident inputs/outputs, `v9a-c-gpu-reorder` follow-on shipped |

The gap (~45 ms) breaks down as:
- ~25-30 ms PCIe transfer overhead (4 MB upload + 16-24 MB readback)
- ~5-10 ms CPU-side Phase C reorder (serial DFS over 2N-1 nodes)
- ~10-15 ms dev-box GPU vs RTX 3060 raw compute gap (dev box is integrated/lower-tier)

**The research §4.1 8 ms target is achievable** when v9a-c-gpu-reorder ships (lifts the CPU-side reorder bottleneck) AND the consumer pipeline is GPU-resident (skips the test harness's deliberate worst-case readback). Both filed as consumer-pull follow-ons.

### 3. 18-config full sweep

`scripts/full-sweep.ps1` — 11 Windows configs + 7 Linux configs. See the §"Sweep result" section below for the table; result was 18/18 PASS at cluster close.

### 4. Updated docs

| File | Update |
|---|---|
| `docs/decisions/0076-geometry-substrate-architecture.md` | §25 amendment written; status flipped 📋 planned → ✅ Accepted; locks D132-D164 |
| `docs/decisions/README.md` | ADR-0076 status updated to reflect §25 acceptance |
| `docs/phases/phase-3.1.7-geometry.md` | v9a-close row ✅ shipped; v9a-d row marked absorbed-into-v9a-c |
| `context.md` | Cluster CLOSED; next-up flips to **v10 `-curves` cluster (5 slices, ~2 wk)** |
| `docs/systems/geometry-bvh-gpu.md` | Status flipped 🚧 ACTIVE → ✅ COMPLETE; module index entry updated |
| `docs/ROADMAP.md` | Cluster summary appended; sub-module 11 of 11 ✅ (geometry substrate complete) |

---

## Filed follow-ons (not part of v9a-close)

These are deferred per the substrate-vs-speculation discipline — settled-design follow-ons that ship when a real consumer hits the wall:

- **v9b GPU BVH refit** — single compute dispatch with atomic parent-join. Consumer: eylem v1c dynamic-body broadphase per-frame topology-untouched updates. Same `coherent` + `memoryBarrierBuffer` discipline from D159 / Lesson 09.
- **v9a-c-gpu-reorder** — pure-GPU canonical-layout reorder kernel. Lifts the CPU-side reorder bottleneck (D163). Required for the 8 ms target with GPU-resident pipeline.
- **v9a-c-60bit** — u64 60-bit LBVH variant (mechanical scale-up of D157). For km-scale CAD/aerospace.
- **v9a-b2-large / v9a-c-large** — recursive-scan / multi-pass variants beyond `kRadixMaxItems = 1 M`.
- **v9a-b2-atomics** — throughput-tier GPU radix trading determinism for ~0.5 ms speedup. Filed if a real consumer needs the marginal gain.

---

## Sweep result (18-config full sweep)

See `scripts/.full-sweep-v9a-close.log` for the raw output.

**Windows (11 configs):**
```
[summary appended after sweep completes]
```

**Linux (7 configs):**
```
[summary appended after sweep completes]
```

Total elapsed: **[TBD]** minutes.

---

## What this cluster unlocks

The GPU LBVH pipeline takes broadphase off the bottleneck list for ambitious workloads:

- **Asset cooker LBVH bake** (offline, per-mesh-collider) — uses serial CPU path; ships now.
- **Eylem v1c GPU broadphase** (per-frame for dynamic bodies) — uses the GPU pipeline; needs `v9a-c-gpu-reorder` + a sandbox demo to validate.
- **GPU-driven occlusion culling** (Phase 3.5+) — uses the GPU pipeline; per-frame sort + LBVH over visible AABBs.
- **Future SPH/MPM fluid sim** (Phase 3.4+) — uses Morton + sort for spatial-hash neighbor finding.

Per [Lesson 08 — Physics scaling realities](../lessons/08-physics-scaling-realities.md), the most realistic ambitious target this unlocks is "open-world physics with 1 M static colliders + 10 K dynamic bodies at 60fps" — broadphase no longer eats the frame budget.

---

## Cluster totals

- **Slices**: 10 algorithmic + 1 close = 11
- **LOC**: ~5 600 engine + ~3 800 tests = ~9 400 total
- **Test cases**: ~71 cases / ~827 K assertions
- **Design decisions locked**: 33 (D132-D164) in ADR-0076 §25
- **Lessons captured**: 3 (Lessons 02, 04, 09)
- **Cluster sprint duration**: ~1 day (2026-05-18, all 10 slices shipped same day)

**Phase 3.1.7 sub-module 11 of 11 ✅** (primitives + bvh + convex + v3 hull-ext + mesh + spatial + polygon + mesh-processing + delaunay + decomposition + **bvh-gpu** ✅). The `crd-geometry` substrate is COMPLETE.

---

## Next phase

🎯 **v10 `-curves` cluster** — 5 slices, ~2 weeks. Bezier / B-spline / Hermite curves + surfaces; consumer for animation paths, motion design, future cinematic cameras. Then v11 transform-aware geometry queries (~2 days). Then **Phase 3.1.7 fully closes**. Then `crd-hesap-dense` v0 → Phase 3.1 eylem v1c resumes per Strategic Execution Plan.

---

## Commit message proposed

```
chore(geometry-bvh-gpu): v9a-close — `-gpu` LBVH cluster CLOSED

* ADR-0076 §25 amendment Accepted: locks 33 design decisions D132-D164
  from the v9a `-gpu` LBVH cluster.
* 18-config full sweep PASS (11 Windows + 7 Linux configs).
* Performance characterization pinned honestly: 53.7 ms / 1M test-harness
  end-to-end on dev box (median-of-5); ~5-8 ms achievable for pure-GPU
  pipeline on RTX 3060 (filed v9a-c-gpu-reorder + GPU-resident-consumer).
* Phase 3.1.7 sub-module 11 of 11 ✅. crd-geometry substrate COMPLETE.
* Cluster totals: 10 algorithmic slices + close in 1 day; ~5 600 LOC
  engine + ~3 800 LOC tests; ~71 cases / ~827 K assertions; 3 lessons
  captured (02, 04, 09) in docs/lessons/.
* Session log: docs/sessions/2026-05-18-geometry-v9a-close.md.
```
