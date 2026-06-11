# crd-hesap-stats — statistics substrate (v12 cluster; created early)

> **Status: SKELETON (one primitive).** Created 2026-06-10 as a **v12-pull**: v7-i needed a counter-based RNG
> for reproducible minibatch sampling with the full determinism moat, and pulling the tiny frozen-interface
> primitive forward beat shipping v7-i with an asterisk. The rest of the v12 stats cluster (distributions,
> estimators, hypothesis tests, …) grows here; this overview will be rewritten when the cluster ships.

## What it contains today

`crd/hesap/stats/philox.hpp` — the **Philox4x32-10 counter-based RNG** (Salmon-Moraes-Dror-Shaw SC'11; the
Random123 generator JAX/XLA and cuRAND standardize on):

- `philox4x32(counter[4], key[2]) → PhiloxBlock` — the constexpr PURE block function. Verified against the
  three **published Random123 known-answer vectors** (`tests/hesap-stats/test_philox.cpp`).
- `PhiloxRng(seed, stream)` — convenience wrapper: 64-bit seed × 64-bit stream × 64-bit position; sequential
  `next_u32/u64/f64/f32` (53/24-bit-mantissa uniforms in [0,1)), unbiased `next_below(bound)`, and
  **O(1) random access** via `jump_to_block`.
- `shuffle(span, rng)` — deterministic Fisher-Yates (the reproducible-minibatch primitive).

**Why counter-based (the moat fit):** the generator is a pure function of (counter, key) — no sequential hidden
state — so the value at any position exists independently of execution order or worker count. Same (seed,
stream, position) ⇒ same value, by construction. First consumer: `crd-hesap-opt`'s `MinibatchSampler` (v7-i),
whose epoch-keyed streams make every epoch's permutation reproducible in any visit order (replay semantics).

Edges: `crd-core` + `crd-containers` only (a leaf primitive).
