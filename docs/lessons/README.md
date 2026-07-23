# Cerid Lessons

> Systemic, architectural, and programming lessons captured while building the engine. Written so the next engineer (human or AI) reads the *reasoning*, not just the *outcome*. The phase docs say what shipped; these say **why**, **what would have been wrong**, and **what pattern to reach for next time**.

This folder is the meta-reflective sibling of `docs/sessions/`. Sessions are *what we did on date D*. Lessons are *what we learned that we want to keep doing / avoid doing forever*.

## When to add a lesson here

- A non-obvious decision came up multiple times during a slice and the reasoning is reusable.
- A "clever" attempt failed and the failure is itself instructive (negative findings have teaching value).
- A measurement upended an assumption (e.g. "AVX2 must be faster than scalar" → it isn't, here's when).
- A pattern showed up in two systems and deserves a name (e.g. "per-(chunk, bucket) offset table").
- The user asked "teach me" and the answer is broader than one session log can hold.

## When NOT to add a lesson here

- Pure "what shipped" status → that's a session log.
- Stable architectural decisions → that's an ADR.
- Build commands / coding conventions → that's `CLAUDE.md` / `AGENTS.md`.
- A single one-line rule with no rationale → that's a `feedback_*.md` memory entry.

## Reading order

The lessons are independent — read what's relevant. For a new engineer joining the project, this order tells the story coherently:

1. [01 — Morton codes, radix sort, and the LBVH pipeline](01-morton-codes-and-lbvh-pipeline.md) — *what Morton codes are, why we sort them, and how the spatial-hierarchy build pipeline composes*
2. [02 — When scalar code beats SIMD](02-when-scalar-beats-simd.md) — *Wassenberg's 89%-of-peak is a GB-scale number; at MB scale the calculus inverts*
3. [03 — Measuring performance correctly](03-measuring-performance-correctly.md) — *win-release vs win-shipping flags; median-of-5 vs single-shot; what cold-cache noise hides*
4. [04 — Parallel stable merge — the per-(chunk, bucket) offset table](04-parallel-stable-merge.md) — *the template for any deterministic-parallel scatter operation*
5. [05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) — *why GPU radix is "fast" only when downstream stays on GPU*
6. [06 — Substrate vs speculation](06-substrate-vs-speculation.md) — *the refined "ship-at-consumer-template-from-day-one" rule with a working example*
7. [07 — How to use radix + Morton in real consumers](07-using-radix-and-morton.md) — *concrete consumer recipes: GPU LBVH, BVH refit, mesh-cooker bake, eylem broadphase*
8. [08 — Physics scaling realities](08-physics-scaling-realities.md) — *what "millions of particles at very small cost" actually means, and where the LBVH stops being the bottleneck*
9. [09 — GPU memory ordering gotchas](09-gpu-memory-ordering-gotchas.md) — *atomicAdd is not enough: when you need `coherent` + memoryBarrierBuffer to make cross-invocation writes visible*
10. [10 — API shape sets the performance floor](10-api-shape-sets-the-perf-floor.md) — *same algorithm + same hardware, 14× faster from one API change; chasing kernel perf when the API forces CPU data movement is a dead end*
11. [11 — The shader-stage frontier](11-the-shader-stage-frontier.md) — *a lecture on all 14 shader stages (vertex→fragment, tessellation, geometry, mesh/task, ray tracing, compute), where each is used, the cutting-edge technique riding on it, which backends light up, and how the node editor fits — plus the frontier gaps we added to D-007*
12. [12 — The CKIR deploy pipeline](12-ckir-deploy-pipeline.md) — *from graph to shippable GPU bytecode: the `.kgph`/`.crdr` files, the five stages (author→cook→permutate→load→hot-reload), and the one content hash that is cache key, dedup key, and change detector at once*
13. [13 — Shaders, pipelines, materials & lighting, the Cerid way](13-shaders-pipelines-materials-lighting.md) — *the concepts primer: what each word means and how CKIR realizes it — compute kernels, pipelines, conventional & neural materials, the surface-vs-transport split, and the "one IR is the meeting point" thesis*
14. [14 — Variants, permutation & specialization](14-variants-permutation-and-specialization.md) — *the deep dive: übershaders/übergraphs, the two authoring styles unified through one cook path, the `specialize_kernel` primitive step by step, `optimize()`'s const-fold/DCE/CSE, content-hash dedup, and the resource-leaf scar that hid in `optimize` until the first kernel was specialized*
15. [15 — Materials, end to end](15-materials-end-to-end.md) — *what a material is (surface response, not lighting — ADR-0102), the OpenPBR surface slab, the ~97-node MaterialX library, deferred G-buffer vs forward, frequency lowering, a real material through the variant matrix (with the stage-aware-cook / pin-all / vector-Select-fold scars), and how conventional & neural materials relate*
16. [16 — Autotuning: finding the fastest way to run a kernel](16-autotuning-the-gpu-schedule.md) — *why one computation has many correct GPU implementations that differ 10–50× in speed, the warehouse-tiling intuition, schedule-vs-graph, the enumerate→cost-model→measure→oracle-validate→cache pipeline (the AS band), what makes it CKIR's autotuner (determinism-gated + offline-DB replay), the register-occupancy & FMA-flag scars, and how it crushed cuBLAS 2.4–2.9× on fused MLP*

## Index by topic

- **GPU / shader architecture**: [11](11-the-shader-stage-frontier.md) (all 14 stages, the frontier techniques, backends, node-editor plan), [13](13-shaders-pipelines-materials-lighting.md) (kernels · pipelines · materials · lighting concepts)
- **CKIR shader system (D-007 D1–D5)**: [12](12-ckir-deploy-pipeline.md) (author→cook→cache→load→hot-reload + the content-hash spine), [14](14-variants-permutation-and-specialization.md) (variants, übergraphs, `specialize_kernel`, dedup, the `optimize` resource-leaf scar), [15](15-materials-end-to-end.md) (materials: OpenPBR slab, MaterialX nodes, lowering, materials through the variant matrix, neural), [13](13-shaders-pipelines-materials-lighting.md) (conventional & neural materials, surface-vs-transport)
- **Performance engineering**: [02](02-when-scalar-beats-simd.md), [03](03-measuring-performance-correctly.md), [05](05-cpu-vs-gpu-perf-tiers.md), [10](10-api-shape-sets-the-perf-floor.md)
- **Architecture**: [01](01-morton-codes-and-lbvh-pipeline.md), [04](04-parallel-stable-merge.md), [06](06-substrate-vs-speculation.md), [07](07-using-radix-and-morton.md), [08](08-physics-scaling-realities.md), [10](10-api-shape-sets-the-perf-floor.md)
- **Programming patterns**: [04](04-parallel-stable-merge.md) (deterministic merge), [02](02-when-scalar-beats-simd.md) (`_mm_prefetch`), [09](09-gpu-memory-ordering-gotchas.md) (`coherent` + `memoryBarrierBuffer`), [10](10-api-shape-sets-the-perf-floor.md) (GPU-input vs CPU-input dispatch overloads)
- **Decision hygiene**: [03](03-measuring-performance-correctly.md), [06](06-substrate-vs-speculation.md), [08](08-physics-scaling-realities.md), [10](10-api-shape-sets-the-perf-floor.md) (empirical data > prediction)
- **GPU concurrency**: [05](05-cpu-vs-gpu-perf-tiers.md), [09](09-gpu-memory-ordering-gotchas.md), [10](10-api-shape-sets-the-perf-floor.md)
