# Lesson 03 — Measuring performance correctly

> **The question that motivated this lesson:** "You measured on win-release. Are you sure we have all the optimizations we need? Why didn't you try win-shipping? Check optimization parameters too."

That critique was right, and it changed the answer. The original single-shot win-release scalar baseline was 9.5 ms. After measuring properly on win-shipping with median-of-5, the same code came in at **4.99 ms** — a ~50% difference that came entirely from measurement methodology, not from any code change.

## TL;DR

Four rules for measuring perf in Cerid (and most C++ engines):

1. **Use win-shipping, not win-release**, when chasing peak perf numbers — `/OPT:ICF` + `/OPT:REF` + `/Gw /Gy /Zc:inline` do measurable work on top of `/O2 /LTCG`.
2. **Median-of-5 runs with one warmup discarded**, not single-shot. Cold-cache / page-fault noise is ±15-25% at the working-set sizes we care about.
3. **Identify what you're measuring** — is it the algorithm, or is it page-fault-on-first-write of a fresh allocation?
4. **The number you should publish is the median**, not the minimum (which is "best case under unrealistic conditions") and not the mean (which is dragged by outliers).

## Part 1 — win-release vs win-shipping: what's actually different

Both presets set:
- `CMAKE_BUILD_TYPE = Release` → `/O2` (max optimization), `/Ob2` (inline-able functions inlined), `/Oi` (intrinsics enabled), `/Ot` (favor speed over size)
- `CMAKE_INTERPROCEDURAL_OPTIMIZATION = ON` → `/GL` + `/LTCG` (link-time code generation; cross-TU inlining and dead-code elimination)

`win-shipping` additionally sets `CRD_SHIPPING = ON`, which the top-level `CMakeLists.txt` translates to:
- `/Gw` — each global in its own COMDAT (linker can drop unreferenced globals)
- `/Gy` — function-level linking (linker can drop unreferenced functions; redundant with `/O2` but harmless)
- `/Zc:inline` — strip inline-only functions that are never called externally
- `/DEBUG:FULL` — emit full PDB for crash dump symbolication
- `/OPT:REF` — strip unreferenced functions at link time
- `/OPT:ICF` — **fold identical COMDATs** (binary size reduction; can improve I-cache locality)

The big one is `/OPT:ICF`. It folds functions with identical machine code into a single copy. For template-heavy code (Cerid is full of those), this often eliminates 20-40% of the binary size — and because the surviving copies are denser in memory, the **instruction cache pressure drops measurably**.

For our 1 M-element scalar radix:
- win-release: 5.32 ms (median-of-5)
- win-shipping: 4.99 ms (median-of-5)

That's a ~6% difference from `/OPT:ICF` + friends alone. For tight inner loops, ICF can land bigger wins because it consolidates the call-site landing addresses for the most-common code paths.

**The rule:** if you're publishing a perf number to compare against another engine, against a paper, or against your own past measurement, **measure on win-shipping**. win-release is for development sanity checks ("did I break codegen?"). win-shipping is for performance reality ("how fast does this actually go?").

## Part 2 — Single-shot vs median-of-5

Our first GPU-side benchmark told us the scalar radix was 9.5 ms. We re-measured with median-of-5 and got 5.32 ms.

What happened: the single-shot run was the **first** invocation in the test binary's life. That run paid for:
- TLSF allocator first-touch (mmap'd memory pages, OS lazy-zeros them on first write)
- Test-runner setup (Catch2 listener, scheduler init)
- Cold instruction cache for the test binary's recently-loaded sections
- Cold data cache for the freshly-allocated input array

None of those are the algorithm's cost. They're measurement noise. A second invocation of the *same* code in the *same* process measures the algorithm proper.

### The harness pattern we use

```cpp
auto run_n_median = [&](auto&& fn) {
    double samples[6];
    for (int r = 0; r < 6; ++r) { samples[r] = crd::perf::measure_ms(fn); }
    // Discard sample 0 (warmup); median of remaining 5.
    double s[5] = {samples[1], samples[2], samples[3], samples[4], samples[5]};
    for (int i = 0; i < 5; ++i)
        for (int j = i+1; j < 5; ++j)
            if (s[j] < s[i]) std::swap(s[i], s[j]);
    return s[2];  // median
};
```

Six samples, drop the first (warmup), median of the remaining five. This:
- Discards the first-touch / cold-cache cost.
- Median (not mean) ignores one outlier in either direction — and there's *always* an outlier from a context switch or interrupt or background work.
- Five real samples is enough that the median is stable (±2-3% across re-runs).

**Why not mean of 5?** Because if the OS preempts you for 50 ms during one sample, the mean is permanently skewed. The median is invariant to up to two outliers in a 5-sample set.

**Why not min of 5?** Because the minimum is "best case under unrealistic conditions" — sometimes the CPU happens to have everything in L1, branch predictors warmed perfectly, the OS scheduler giving you uninterrupted time. That's not what production runs look like. The median is closer to a representative production sample.

### What single-shot DOES tell you

Single-shot is fine for:
- "Did this regress catastrophically?" (e.g., went from 10 ms to 100 ms)
- "Does this even pass the budget?" (`CRD_PERF_BUDGET_LE` with a generous threshold)
- CI gating where 15-25% noise is acceptable because the budget has headroom

Single-shot is NOT fine for:
- "Is A faster than B by 10%?" — that's within the noise floor.
- "Did my optimization help?" — you need to distinguish signal from noise.
- "Should I publish this number?" — never publish a single-shot.

## Part 3 — What you're actually measuring

The `measure_ms` lambda boundary matters. Consider these two:

```cpp
// VERSION A — measures the algorithm proper
const auto in = build_input(N);
const auto t = measure_ms([&]{
    const auto out = sort_morton_pairs(in, &alloc);
    REQUIRE(out.size() == N);
});

// VERSION B — measures the algorithm + allocation + input construction
const auto t = measure_ms([&]{
    const auto in = build_input(N);
    const auto out = sort_morton_pairs(in, &alloc);
    REQUIRE(out.size() == N);
});
```

Version A is what you usually want. Version B *also* charges you for the input construction — useful only if that's part of your hot path. For sort itself, version A.

Inside `sort_morton_pairs`, two `Array` allocations happen (the output + the aux ping-pong buffer). The first invocation touches those pages for the first time — page-fault cost. The second invocation reuses the TLSF allocator's already-faulted pages. Warmup discards this delta.

**The rule:** know what you're including. If your timing includes allocator first-touch, your number is "first-call cost", not "steady-state cost". Both are valid; just don't confuse them.

## Part 4 — Other measurement gotchas seen in this codebase

### Catch2 budget assertions don't print on success

`CRD_PERF_BUDGET_LE("name", budget_ms, [&]{ ... })` checks the budget but doesn't show the measured number on a passing run. If you want to see the timing for analysis, instrument explicitly:

```cpp
const double t = crd::perf::measure_ms([&]{ ... });
UNSCOPED_INFO("[label] measured = " << t << " ms");
CHECK(t <= budget_ms);
```

`UNSCOPED_INFO` only prints on a failed `CHECK`, but Catch2's `-s` ("show successful") flag will print it on success too:

```
& crd-tests.exe "perf_test_name" -s --reporter compact
```

### Building with the wrong preset

Easy mistake: you fix a function in win-debug, then run a perf test against the win-release binary which was last built before your fix. The number you see is for the *old* code. Always rebuild the preset you measure on.

### Different machines, different numbers

Cerid's CI runs on cloud Linux boxes; the dev box is a desktop Ryzen. A 5 ms number on the dev box might be 8 ms on CI or 3 ms on a workstation with DDR5-7200. Publish the **hardware** alongside the number, or it's meaningless. The session logs always include "win-shipping median-of-5 on dev box" — those parameters are part of the measurement.

### CI soft mode — a hard ms budget is not a portable correctness gate

Because of exactly this hardware spread, `CRD_PERF_BUDGET_LE` is **soft in CI**. A dev-box-calibrated absolute-millisecond budget hard-asserted on a shared cloud runner *will* trip on clock/bandwidth variance and first-touch page faults inside the timed region — and on Linux a failed `CRD_ASSERT` traps as **SIGILL**, killing the whole suite (case study: `sort_morton_pairs` 1M, 20 ms budget, linux-gcc-relwithdebinfo, 2026-05-21). When `CRD_PERF_BUDGET_SOFT` or the standard `CI` env var is set, an over-budget result logs a stderr warning instead of asserting. The measured lambda — including any inner `REQUIRE`/`CHECK` — still runs, so **correctness is always enforced; only the timing gate softens.** Local dev (neither var set) keeps the hard assert, which is where you actually catch regressions. Conclusion: treat `CRD_PERF_BUDGET_LE` as a *local* gate + a CI *observability* signal, not a CI correctness gate. If you need CI to catch a perf regression, assert a **ratio against a same-machine baseline**, not absolute ms.

## Part 5 — The discipline, summarized

When you measure performance in Cerid, your default protocol is:

1. Build on win-shipping (`cmake --build --preset win-shipping --target <name>`).
2. Instrument with `crd::perf::measure_ms` inside the test, wrapping just the algorithm under test.
3. Run 6 samples, drop the first, take the median of the remaining 5.
4. Publish the median, the working-set size, the machine, the preset. If you publish a ratio (e.g., "1.86× speedup"), publish the absolute numbers too.
5. If the result surprises you (e.g., SIMD slower than scalar), re-measure with different seed values + different N to confirm it's not a fluke.

Once you have the number, remove the instrumentation. The shipped tests should be `CRD_PERF_BUDGET_LE` for CI gating, not full-precision benchmarks — Catch2 isn't a benchmarking tool.

## What to read next

- [Lesson 02 — When scalar beats SIMD](02-when-scalar-beats-simd.md) — the case study where measurement discipline saved us from shipping slower code.
- [Lesson 06 — Substrate vs speculation](06-substrate-vs-speculation.md) — measurements are what distinguish a real optimization from a speculative one.
