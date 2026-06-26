# 2026-06-24 — v12-f (fast samplers) + v12-g (QMC/ChaCha) close

**Slice:** Phase 3.1.6 `crd-hesap`, v12 Statistics cluster — v12-f + v12-g.
**State at entry:** v12-a…e shipped (special-fns + RNG suite, linux-gcc-verified); v12-f/g code written in the working tree but **never run to completion** (uncommitted, unverified). Whole v12 batch (a→g) uncommitted.
**Outcome:** v12-f/g verified GREEN on the 4 Windows configs + guards; one real bug + 7 tidy violations fixed; honest all-peers perf board captured.

## What was already in the tree (prior session, today)
- `engine/hesap-stats/include/crd/hesap/stats/`: `ziggurat.hpp`, `samplers.hpp` (v12-f), `qmc.hpp`, `chacha.hpp` (v12-g), wired into `stats.hpp` umbrella + the engine/tests CMake.
- `tests/hesap-stats/`: `test_samplers.cpp`, `test_qmc.cpp` + `qmc_refs.inc` (scipy-generated) + the MATLAB sampler bench.
- The algorithms are the NumPy gold methods: Marsaglia-Tsang ziggurat + gamma, gamma-ratio beta, Knuth+PTRS Poisson, BINV+BTPE binomial, Vose alias; Sobol (Joe-Kuo), Halton, rank-1 lattice, LHS, ChaCha20 (RFC 8439).

## The bug (win-debug hang → root cause → fix)
A full `--reporter compact` debug run never terminated (killed at **595 s CPU, 1 thread, no output**). Localised by tag: `[v12-e]` and `[v12-g]` finished fast; **`[v12-f]` hung**, narrowed to the **binomial** case.

`binomial()` dispatches by `n·min(p,1−p)`: `{n=500, p=0.95}` → `25 < 30` → `binomial_inversion`. But `binomial_inversion` was **not reflection-aware** — it used the raw `p=0.95`, so:
- `qn = q^n = 0.05^500` **underflows to 0** → `px` starts and stays 0,
- the inner `while (u > px)` never advances `u`, `x` climbs until `x > bound` (computed from `np_ = n·p = 475`),
- the break leaves `x > n` → the outer `for(;;)` retries → **infinite loop**.

`binomial_btpe` already reflected internally; the inversion path simply forgot. **Fix:** reflect internally too — `r = min(p, 1−p)`, sample `Binomial(n, r)`, return `p≤0.5 ? x : n−x`; resample on the `bound` overflow guard instead of returning a wrong value. (`samplers.hpp` `binomial_inversion`.)

**Sanity lesson (rule #3 boundary-adversary + rule #2 trust the artifact):** 282 539 passing assertions sailed over this — only the single adversarial parameter (`p>0.5` at large `n`) reached the dead branch, and only the **optimized** full-suite run exposed it (debug just hid the non-termination behind slowness; a "still running" debug build was a *hang*, not slow). Logged in `docs/SANITY.md`.

## Correctness — 4-config Windows DoD (all green)
| config | result |
|---|---|
| win-debug | 282539 assertions / 30 cases — exit 0 |
| win-asan | 282539 / 30 — exit 0, **zero ASan errors** |
| win-shipping | 282539 / 30 — exit 0 |
| win-tidy | build clean (engine + tests), 0 warnings-as-errors |
| guards (ctest) | no-non-ascii-test-names · no-std-sort · no-std-math · no-untagged-physical · no-malloc — all pass |

### Tidy debt cleared (same uncommitted batch)
- `engine/jobs/src/topology.cpp` (ADR-0094 worker-policy): local `constexpr kCap` → `cap` (newer clang-tidy classifies a local constexpr as `LocalConstant` = lower_case); function-local `static const cached` → `kCached` (`StaticConstant` = CamelCase + `k`); nested ternary in `pcore_worker_count()` extracted to a named `m`.
- `tests/hesap-stats/test_rng.cpp`: `kN` → `n` (local constant). `test_samplers.cpp`: split `c(5), d(5)` multi-decl; TEST_CASE name de-unicoded (em-dash/⇒ → ASCII, for the no-non-ascii guard).

## Perf — honest all-peers sampler throughput (i9-14900K, single-thread, ns/sample)
| sampler | Cerid | NumPy | MATLAB-1T | vs NumPy | vs MATLAB |
|---|---|---|---|---|---|
| normal | 3.50 | 7.34 | 4.425 | **2.10×** | 1.26× |
| exponential | 3.05–3.41 | 2.75–3.50 | 8.118 | ~par (0.90–1.04×) | 2.4× |
| gamma(2.5) | 8.62 | 12.67 | 32.020 | 1.47× | 3.7× |
| beta(2,5) | 15.99 | 24.06 | 66.167 | 1.50× | 4.0× |
| poisson(4) | 16.81 | 23.20 | 140.452 | 1.38× | 8.4× |
| poisson(30) | 17.79 | 25.14 | 321.448 | 1.41× | 18× |
| binomial(20,.3) | 16.66 | 29.38 | 98.993 | **1.76×** | 6.2× |
| binomial(1000,.5) | 17.93 | 21.50 | 4963.431 | **1.20×** | **277×** |

**vs MATLAB-1T: WIN 8/8** (1.26×–277×). **vs NumPy: WIN 7, exp ~parity** (~0.90–1.04× run-to-run). The **determinism moat** (every sampler bit-identical across {1,4,16} threads via counter-RNG, no cross-thread reduction) is the differentiator NumPy/MATLAB lack.

### Chasing the 2 NumPy losses (user-directed) — one closed, one explained
- **binomial(1000): 0.84× LOSE → 1.20× WIN** (and binomial(20): 0.98× parity → **1.76× WIN**). Root cause via a decomposition probe: the gap is **generator-independent** (~25 ns with Pcg64Dxsm/Xoshiro/SFC alike) ⇒ purely algorithmic. NumPy caches its BTPE/BINV setup in `binomial_t` state keyed on (n,p) and amortizes it over the whole array; Cerid recomputed the setup (a `sqrt`/`pow`) **every draw**. A cached-setup probe confirmed 25.3 → 15.9 ns. Fix: a stateful **`BinomialSampler`** (precompute once, NumPy's pattern) that the bench holds for repeated draws — the fair peer to NumPy's vectorized `r.binomial(n,p,size)`. The free `binomial(g,n,p)` now delegates to it (one-off setup), so there is ONE implementation; `test_samplers` gates `BinomialSampler.sample == binomial` **bit-for-bit** across BINV/BTPE/reflection (20000 checks).
- **exponential: ~parity, honest.** The decomposition probe shows the gap is the **generator**, not the ziggurat: `standard_exponential` = ~2.0 ns ziggurat + 1.47 ns `Pcg64Dxsm::next_u64`; NumPy's default plain PCG64 is ~1.0 ns. **DXSM does an extra 64-bit multiply for better statistical quality** than NumPy's default PCG64 — so Cerid "loses" exp by benching a *higher-quality* generator. Faster generators (Xoshiro/SFC, lower raw `next_u64`) made exp *slower* (register pressure from their larger state — measured, refuted the "swap generator" idea). Not chased further (doctrine rule #7) — shipping a weaker generator to win a microbench would be a quality regression. Tables also hoisted to namespace-scope `inline const` then **reverted** (no measured effect — the function-local-static guard wasn't the bottleneck).

v12-g (QMC) gates on correctness, not throughput: Sobol **bit-equal to scipy.stats.qmc** (16×4 KAT), ChaCha20 RFC all-zero-key keystream KAT, LHS perfect stratification, low-discrepancy integration beats 1/√N — all green.

## Pending / next
- **PENDING USER:** commit the v12 a→g batch + the 18-config CI sweep (the established pattern; v11 committed `768d8d9`).
- Honest follow-on (filed, not chased): close the 2 small NumPy sampler losses (exponential, binomial-1000).
- **NEXT slice:** v12-h — distribution framework + ~25 univariate continuous (pdf/logpdf/cdf/sf/ppf/rvs/moments/entropy/MLE-fit).

## Files touched this session
- `engine/hesap-stats/include/crd/hesap/stats/samplers.hpp` (binomial reflection fix)
- `engine/jobs/src/topology.cpp` (tidy)
- `tests/hesap-stats/test_rng.cpp`, `tests/hesap-stats/test_samplers.cpp` (tidy + non-ascii)
- `docs/phases/phase-3.1.6-hesap.md`, `context.md`, `docs/SANITY.md` (status + ledger)
