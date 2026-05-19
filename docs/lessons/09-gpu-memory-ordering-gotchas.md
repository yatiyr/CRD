# Lesson 09 — GPU memory ordering: atomicAdd is not enough

> **The bug that motivated this lesson:** v9a-c LBVH upsweep on GPU. 11 of 12 tests passed. The N=10000 random-AABB oracle failed with **0.5+ unit differences in bounds** — not 1-ULP rounding, hundreds of millions of ULPs. The implementation looked right. The fix was a single GLSL keyword.

## TL;DR

**`atomicAdd` in GLSL provides acquire-release semantics on the atomic location ONLY. It does not synchronize non-atomic memory writes by other invocations.**

To make non-atomic cross-invocation writes visible, you need one or both of:
- **`coherent` qualifier** on the buffer declaration — tells the implementation to bypass caching for this buffer.
- **`memoryBarrierBuffer()`** intrinsic — flushes pending writes before the next memory operation.

If you have an atomic-coordinated reduction or scan where one invocation writes to buffer X then signals via an atomic, and another invocation reads the atomic then reads buffer X, you NEED one of those two. The atomic alone is not enough.

This bug is **invisible at small N** because shorter walks finish within work-unit timelines where the implementation happens to flush. It surfaces at scale.

## Part 1 — What we tried and what happened

The Karras 2012 §2.4 AABB upsweep pattern:

```glsl
// Each leaf thread walks up to root via parent chain.
void main() {
    uint node = leaf_id;
    while (node != 0) {
        uint p = parent[node];
        uint c = atomicAdd(children_done[p], 1u);
        if (c == 0u) { return; }            // I'm first — sibling not done
        // Second arriver: BOTH children's bounds should be visible
        bounds[p] = union(bounds[left_child[p]], bounds[right_child[p]]);
        node = p;
    }
}
```

The intuition: `atomicAdd` returning `1` means another invocation already passed through this code. That invocation finished its bounds-write before incrementing the counter. Therefore my read of bounds should see the latest data.

**The intuition is wrong.** GLSL's memory model gives `atomicAdd` acquire-release on the atomic memory location — meaning subsequent atomic operations are ordered relative to it. It does NOT give "happens-before" guarantees for non-atomic operations.

In our buffer, `bounds[]` is a regular (non-atomic) storage buffer. The first thread's `bounds[p] = ...` is a normal store, which the implementation may cache in its L1/SLM and only flush when the workgroup ends, when memory pressure forces eviction, or at any other implementation-chosen time. **There is no guarantee** that the second thread's `load bounds[...]` from a different invocation sees that store.

## Part 2 — The discriminating diagnostic

Our test suite caught this because we had **multiple sizes of oracle test**:

| Test | N | Result |
|---|---|---|
| Calibration | 4 | PASS — bounds match within 1 ULP |
| Degenerate (N=1, 2, 64) | small | PASS |
| 8-corner | 8 | PASS |
| End-to-end pipeline | 4096 | PASS |
| **Oracle random** | **10000** | **FAIL — bounds off by 0.5+** |
| Determinism check | 1000 | PASS — same buggy output 3 times |
| Perf budget | 1M | PASS (only checks budget, not correctness) |

The N=10000 oracle was the only test that caught it. Two factors why:

1. **Tree depth grows as log₂(N).** N=10000 → depth ~14. Each leaf walks up ~14 parent steps. That's 14 atomic-then-read sequences per leaf, 140K total sequences. The probability that ONE of them races increases linearly with depth.

2. **At larger N, more workgroups dispatch and execute in parallel.** Workgroup A's bounds writes are NOT visible to workgroup B's reads without explicit synchronization. At small N (1-2 workgroups), everything happens in one execution unit and caching tends to be consistent. At 10000 elements, ~40 workgroups race, and the bug becomes observable.

**If you only test small N, your code looks correct.** The bug ships. The first consumer at production scale hits it.

## Part 3 — The fix (one GLSL keyword)

```glsl
// BEFORE — broken:
layout(set = 0, binding = 0) buffer Bounds { float bounds[]; } bounds_buf;

// AFTER — correct:
layout(set = 0, binding = 0) coherent buffer Bounds { float bounds[]; } bounds_buf;
```

The `coherent` qualifier tells the implementation: **"writes to this buffer must be visible to all invocations without further synchronization."** It disables coalescing/caching for this specific buffer, forcing writes to propagate through a globally-visible memory level.

Plus a defensive `memoryBarrierBuffer()` between the bounds write and the next atomic up the chain:

```glsl
store_aabb(p, plo, phi);
memoryBarrierBuffer();      // flush bounds[p] write before next atomicAdd
node = p;
```

Both together provide the "happens-before" guarantee the atomic alone didn't.

## Part 4 — When you need `coherent`

You need `coherent` (or equivalent memoryBarrier) whenever you have:

1. **One invocation writes to buffer X.**
2. **Another invocation (different workgroup, different timestep) reads from buffer X.**
3. **The coordination between them goes through a different memory location** (atomic, fence, separate kernel dispatch with barrier).

Examples from the v9a cluster:

| Use case | Needs coherent? | Why |
|---|---|---|
| Per-thread output to disjoint buffer slots | ❌ No | No cross-invocation read |
| Histogram via `atomicAdd(hist[bucket], 1)` | ❌ No (just on hist) | The atomic IS the synchronization |
| Karras upsweep: bounds-write coordinated by separate atomic counter | ✅ **YES** | Atomic + non-atomic write at different locations |
| Multi-pass radix where pass N+1 reads pass N's output | ❌ No | The CPU-side buffer-barrier between dispatches synchronizes |
| Lock-free queue, value written before tail-pointer atomic update | ✅ **YES** | Classic publish-via-atomic pattern |

## Part 5 — Cerid's discipline going forward

For every Cerid GPU shader that does **atomic-coordinated cross-invocation memory access**:

1. **The buffer being written non-atomically must be declared `coherent`.**
2. **A `memoryBarrierBuffer()` MUST precede the next dependent atomic.**
3. **The discriminating test must be at N ≥ tree_depth × workgroup_count.** Small-N tests don't exercise this code path.
4. **A `gpu_determinism_check` 3-round test is NOT enough** — it would just produce the same wrong answer three times.

The fix to lesson 09's bug is documented inline in `lbvh_aabb_upsweep.comp`:

```glsl
// **`coherent` keyword is load-bearing here.** Without it, the atomic-counter
// coordination does not guarantee that one thread's bounds write is visible
// to another thread's subsequent read. GLSL's atomicAdd provides acquire-
// release on the atomic location only — `coherent` extends the visibility
// guarantee to all reads/writes of this buffer across invocations.
layout(set = 0, binding = 0) coherent buffer Bounds { float bounds[]; } bounds_buf;
```

If you write a new GPU shader that has the atomic-coordinated pattern (D159 in v9a-c, future eylem v1c GPU broadphase, future GPU-driven culling), **copy this comment**. The discipline must be visible at the call site, not buried in a docs/lessons/ file three months from now.

## Part 6 — Vulkan-level alternative

Beyond GLSL `coherent`, Vulkan also provides:

- **`VkBufferMemoryBarrier`** between dispatches — synchronizes ALL pending memory ops on a buffer before the next dispatch begins. We already use this between the build and upsweep dispatches (`cmd->buffer_barrier(...)`).
- **`atomicCounter` SPIR-V opcodes** with `Volatile` decoration — finer-grained than `coherent` but more verbose.

For Cerid's purposes, `coherent` is the right tool — it's local to the buffer declaration and clearly documents intent. We don't need Vulkan-level atomics for this case; the GLSL keyword is sufficient.

## Part 7 — The broader principle

> **Atomic operations synchronize the atomic memory location. To synchronize *anything else*, you need a separate mechanism — `coherent`, `memoryBarrier`, or an inter-dispatch barrier.**

This is a general principle of memory models — CPU C++ memory model, GPU GLSL/HLSL, Vulkan, CUDA — they all have it. The mistake is assuming the atomic carries more weight than it does.

When in doubt, ask: "What memory ordering guarantee am I relying on, and does the language spec actually give it to me?" If you can't answer in one sentence, you have a bug waiting to surface at scale.

## What to read next

- [Lesson 04 — Parallel stable merge](04-parallel-stable-merge.md) — the CPU pattern that AVOIDS this whole class of bug by using disjoint per-worker output ranges (no atomics needed).
- [Lesson 05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) — why GPU's "more parallelism" comes with more memory-model complexity.
