# Lesson 06 — Substrate vs speculation

> **The question that motivated this lesson:** "I think we'll need parallel radix in the future. Should we build it now? Honestly, will it help us?"

This is the most common engineering trap in modular system design: building things "we'll probably need" before any real consumer has shown up. The cost is real (~600 LOC, days of work, debt risk), and the value is speculative (the consumer might never come, or might want something different).

The rule we've settled on: **substrate ships proactively; speculative consumer-specific paths defer.** This lesson explains the distinction with a worked example.

## TL;DR

When you're tempted to build something "because we'll need it later," ask three questions:

1. **Is the design settled?** Substrate work has a known, well-understood pattern (e.g., the per-(chunk, bucket) offset table for parallel stable merge). Speculative work has unsettled tradeoffs that only a real consumer can resolve.
2. **Is the test cheap?** Substrate work has clear discriminating tests (e.g., "byte-identical to scalar at num_jobs ∈ {1,2,4,8,16}"). Speculative work needs consumer scenarios to validate.
3. **Does the API stay stable?** Substrate work doesn't change the public surface for non-users (e.g., `sort_morton_pairs` stayed serial; `sort_morton_pairs_parallel` is opt-in). Speculative work often requires changing existing APIs in ways future consumers might reject.

If all three answers are "yes," ship now. If any is "no," defer until the consumer arrives.

## Part 1 — The trap and the refined rule

Cerid started with a strict "real workload before optimization" principle (from `docs/PRINCIPLES.md`). This is correct for raw perf tuning: don't write SIMD code for a kernel that no consumer has measured.

But this principle, applied naively, leads to **substrate paralysis** — refusing to build foundational pieces because "no one is asking yet." We hit this trap multiple times early in Phase 3.1.7. The refined rule (from `feedback_ship_at_consumer_template_from_day_one`):

> **Substrate work with settled designs + cheap tests ships proactively.** Speculative consumer-specific paths with unsettled designs defer.

The distinction is in the three questions above. Let me show how they apply.

## Part 2 — Worked example: v9a-b1-parallel decision

Here's the decision matrix we worked through this session, when the user asked "should we build parallel radix?":

| Question | Answer | Substrate or speculative? |
|---|---|---|
| Is the design settled? | YES. 3-phase parallel radix (per-chunk histogram + serial offset table + disjoint scatter) is textbook. crd-jobs exists as the substrate; deterministic merge has a known pattern. | Substrate |
| Is the test cheap? | YES. Reuse the existing 10K random + cross-chunk equal-keys + num_jobs sensitivity patterns from `test_bvh_parallel`. `bit_compare` against scalar reference is the discriminating oracle. | Substrate |
| Does the API stay stable? | YES. Public `sort_morton_pairs<KeyT>` stays serial+prefetch with no jobs dependency. `sort_morton_pairs_parallel<KeyT>` is opt-in, callers who don't want parallelism never see it. | Substrate |
| Does it serve future consumers without speculation? | YES. eylem v1c broadphase + parallel BVH refit + cooker LBVH bake will all need parallel stable merge. The 3-phase pattern is the template they'll copy. | Substrate |

All four answers: substrate. **Ship it.**

Counter-example: should we ship a `crd-net` substrate now, before any networking consumer? 

| Question | Answer | Substrate or speculative? |
|---|---|---|
| Is the design settled? | NO. UDP vs TCP, reliable vs unreliable, lockstep vs rollback, snapshot vs delta — all consumer-specific. | Speculative |
| Is the test cheap? | NO. Networking needs simulated latency / loss / reorder; the test patterns are huge. | Speculative |
| Does the API stay stable? | UNKNOWN. The right API depends on whether the consumer is a deterministic-rollback game or a streaming-data robotics setup. | Speculative |

All speculative. **Defer.** A future consumer (multiplayer FPS, distributed robotics) will tell us what they need; we'll build it then with their workload validating each decision.

## Part 3 — Why "we'll need it later" isn't enough

The user's reasoning was: "I think it will in the future." That's an honest hunch, and often correct in spirit. But hunches alone don't justify ~600 LOC of complexity. What justified building v9a-b1-parallel was the *combination*:

- ✅ The user's hunch ("we'll need this")
- ✅ The settled design (textbook 3-phase pattern)
- ✅ The cheap test (existing patterns from `test_bvh_parallel`)
- ✅ The stable API (opt-in entry point)
- ✅ The genuine substrate value (jobs-bandwidth-stress validation, parallel-merge template)

If only the first point had been true ("I think we'll need this"), I'd have pushed back: "let me file it as a debt entry, you can prioritize when a consumer surfaces." The other four made it worth building proactively.

## Part 4 — Anti-patterns we've seen and avoided

### Anti-pattern 1: "Future-proof" API design

Bad: adding template parameters, callback hooks, or extension points "so we can support X later." Result: the API is harder to use for everyone today, and when X arrives it doesn't fit the speculative shape anyway.

Cerid avoids this by **using existing patterns** when they exist (e.g., `bvh_build_parallel` set the parallel API shape; `sort_morton_pairs_parallel` follows it exactly) and **adding new patterns only when shipping a new consumer** that justifies them.

### Anti-pattern 2: Dual code paths "for compatibility"

Bad: shipping a "fast" version alongside a "compatible" version. Result: every test runs both; every bug fix lands in both; every future engineer wonders which to use.

Cerid's quality bar (`feedback_quality_bar`): **single path, single canonical implementation.** When we found SWWC was slower than scalar+prefetch, we **deleted** the SWWC code. We didn't ship it as a fallback "for portability" or "in case someone benchmarks it." Negative findings get a memory entry and a comment; they don't get shipped code.

### Anti-pattern 3: Premature optimization disguised as substrate

Bad: claiming "this is substrate" when the actual motivation is "I want to try AVX2 and need an excuse." Result: complex code that solves a non-problem.

We almost fell into this. The original v9a-b1-simd debt entry said "implement AVX2 SoA sub-histograms" — that's a *technique*, not a substrate. The actual substrate was clear: "scalar+prefetch + parallel-via-jobs are the elite CPU paths at this scale, and the deterministic-merge pattern is the future template." Once we framed it that way, the work to ship was clear.

Distinguishing these requires honesty: **am I building this because the system needs it, or because I want to try a thing?** If the latter, file it as an experiment in a branch; don't merge until a consumer shows up.

## Part 5 — When the rule fails: known bugs in the system

Sometimes a "speculative" feature ends up shipping because a *correctness* issue was discovered only after the substrate landed. Example from Cerid history:

- The Shewchuk `insphere_exact` adaptive predicate was deferred at v3 because "no consumer was surfacing cospherical pathology to validate against" (good substrate-vs-speculation reasoning).
- v8c (3D Bowyer-Watson Delaunay) was that consumer, and it surfaced the issue exactly as predicted.
- BUT the upgrade ended up being more complex than estimated (~1000 LOC instead of 300) because the abstract structure laid down at v3 made some Stage D arithmetic awkward.

Lesson: even **settled-design substrate** sometimes becomes harder to retrofit when the consumer arrives. The defense isn't "build everything proactively"; it's "leave the right hooks." For v9a-b1-parallel, the right hook is the `_parallel` suffix — opt-in, doesn't lock in any specific worker-count or scheduling decisions, can be reimplemented inside the function as long as the contract holds.

## Part 6 — The practical decision flow

When considering whether to build a speculative feature now, use this flow:

```
       Is the consumer named, dated, and specced?
                       ↓
             YES → build it now, full slice
              NO ↓
       Does the design have unsettled tradeoffs
       only a real consumer can resolve?
                       ↓
             YES → file in docs/debt.md, defer
              NO ↓
       Is the test surface clear and cheap?
                       ↓
              NO → file in docs/debt.md, defer
              YES ↓
       Does the new API stay backward-compatible
       (opt-in, behind a name, no behavior change for non-users)?
                       ↓
              NO → file in docs/debt.md, defer
              YES ↓
       Will the substrate genuinely help ≥2 future consumers?
                       ↓
             YES → SHIP NOW as substrate (this is the case for v9a-b1-parallel)
              NO ↓
       File in docs/debt.md, defer until first consumer arrives
```

Most candidates exit at "file in debt" — that's the safe default. The ones that survive all four filters are the substrate worth proactively building.

## Part 7 — Reading the user's signal

The user often phrases substrate requests as speculation ("I think we'll need this"). The right move isn't to take that as a literal command, and it isn't to argue with the hunch. It's to **run the four-filter check** and **report back the result alongside the work**.

For v9a-b1-parallel, the report I gave the user before coding was:

> Realistic outcome on this 8-core dev box for 1M u32: ~2.5-3 ms (2.5× speedup; bandwidth-bound). We will not hit 1ms on CPU; that's a GPU number. But here's why it's still worth building as substrate (not speculation): [four substrate reasons listed].

This:
- Set realistic expectations (no false hope of 5×)
- Made the substrate case explicitly (not just "you asked, I'll do it")
- Gave the user grounds to abort if the tradeoff felt wrong

The user confirmed. We built it. It landed at 1.86× (within the predicted range). The session log captures both the prediction and the measurement — future engineers can see we estimated honestly.

## What to read next

- [Lesson 04 — Parallel stable merge](04-parallel-stable-merge.md) — the substrate template v9a-b1-parallel established.
- [Lesson 07 — Using radix and Morton in real consumers](07-using-radix-and-morton.md) — the consumers that justified ranging this substrate before they shipped.
