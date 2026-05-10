# ADR-0063 — Eylem determinism contract

**Status:** Accepted (2026-05-10)
**Tags:** [arch] [physics] [eylem] [determinism] [ci] [fp]
**Related ADRs:** ADR-0005 (Math v1), ADR-0033 (`crd-jobs`), ADR-0035
(Networking architecture — the deterministic-replay consumer), ADR-0058
(Öbek system — also deterministic), ADR-0062 (Eylem physics
architecture).
**Phase:** Phase 3.1 — Eylem (Cerid-native physics).

---

## Context

Cerid's "determinism is a first-class option" cornerstone (PRINCIPLES.md)
is concrete only when the substrate guarantees it. Eylem is the first
module where determinism is the *primary* design driver: rollback netcode,
robotics RL training, cinematic reproducibility, replay-based bug repro,
and lockstep multiplayer all consume it.

The research (`docs/research/cerid-eylem.md` § *Determinism*) maps the
two real options:

1. **Bake in from day 1.** Pin FP contract, replace stdlib trig / sort /
   hash from day 1, design merges to be commutative, design queues to be
   FIFO regardless of arrival thread. Cost: ~1–3% perf.
2. **Bolt on later via a flag.** Jolt's `JPH_CROSS_PLATFORM_DETERMINISTIC`
   pattern. Cost: ~8% perf, plus *months* of rewrite to reach the same
   guarantees because the original code was written without the
   constraint.

ADR-0062 picks option 1. This ADR fixes the contract every eylem
sub-module honours.

The companion question — *what level of determinism do we promise?* —
also gets answered: **bit-exact world-snapshot hashes across MSVC /
clang / gcc × x64 / ARM × Windows / Linux**, when running with the
deterministic configuration. This matches Box2D v3 + Rapier
`enhanced-determinism` + Jolt with the flag.

## Decision

### 1. The deterministic configuration (compile-time + runtime)

A build is "eylem-deterministic" when *all* of the following hold. CI
asserts it via the v9b matrix.

**Compile flags (per CMake target on `crd-math`, `crd-eylem`, every
sub-module):**
- `-ffp-contract=off` (GCC / Clang) and `/fp:precise` + no
  `/fp:contract` (MSVC). No FMA contractions.
- No `-ffast-math` / `/fp:fast`. Anywhere. Ever.
- No x87 codegen. `-mfpmath=sse` on 32-bit (mostly a non-issue on x64
  / ARM but worth pinning for portability).
- `-fno-finite-math-only` (default; pin to be safe). NaN/Inf must
  round-trip.

**Runtime FPU state (set once at thread startup; CI checks):**
- Rounding mode: round-to-nearest, ties-to-even (IEEE default).
- Denormal handling: enabled (no FTZ / DAZ). Sub-normals must
  round-trip identically across platforms.
- Exceptions masked (no SIGFPE on inexact / underflow / etc).

**Source-level rules (lint-checked; `clang-tidy` custom check):**
- No `std::sin` / `std::cos` / `std::tan` / `std::atan2` / `std::asin`
  / `std::acos` / `std::exp` / `std::log` / `std::pow` in eylem code.
  Use the Cerid-internal versions in `crd::math::deterministic` (built
  on Cephes-style polynomials, bit-exact across libms).
- `std::sqrt` / `std::fabs` / `std::floor` / `std::ceil` are *allowed*
  (IEEE-754 mandates bit-exact results across libcs).
- No `std::sort` / `std::stable_sort` / `std::nth_element` /
  `std::push_heap` / `std::pop_heap` / `std::make_heap`. Use the
  deterministic variants `crd::containers::sort` etc. (introsort with
  pinned tie-breaker).
- No `std::hash` / `std::unordered_*`. Use `crd::containers::HashMap`
  with the engine's deterministic hash (FNV-1a 64).
- No reliance on `std::map` iteration order across runs (it's
  deterministic per-run but not portable; just don't depend on order).

**Algorithmic rules (architectural; reviewed in ADR / PR):**
- All cross-thread reductions are commutative + associative
  (bit OR, fixed-position index push, atomic max). No "first-thread-
  to-finish-wins" patterns.
- Iteration order over entities, contacts, islands, etc. is by stable
  id, never by pointer or memory address.
- Random numbers via stateless splittable PCG, seeded per-island from
  `(world_seed, island_id, frame)`. No global RNG.
- Sleeping uses absolute energy thresholds, not history-window
  averages (history is sensitive to non-deterministic frame
  scheduling).
- Contact warm-starting cache key = `(body_a_id, body_b_id, feature)`
  — never pointer-based.
- Time integration is fixed-step (60 Hz default). The variable-rate
  presentation step interpolates the last two physics snapshots — it
  never feeds back into the simulation.

### 2. Cerid-internal deterministic stdlib substitutions

A new sub-namespace `crd::math::deterministic` provides bit-exact
replacements for the trig / transcendental functions banned above.
Implementation references:

- **sin / cos / tan** — Cody-Waite reduction + minimax polynomial; the
  Cephes implementation Box2D v3 ships verbatim is the reference.
- **atan2** — argument-quadrant decomposition + minimax polynomial.
  Box2D v3's `b2Atan2` is the reference.
- **asin / acos** — derived from atan2 with bounds clamping.
- **exp / log / pow** — Sollya-fitted minimax polynomials. The Sleef
  library ([sleef.org](https://sleef.org/)) is the open-source reference;
  we port the scalar paths only (SIMD lives in `crd-math`).

These functions live in `crd-math` (introduced in Phase 3.1 v0c) and
have a CI test that asserts bit-exact equality across MSVC / clang /
gcc × x64 / ARM. If a libm change breaks a test, the test catches it
before it becomes a downstream simulation divergence.

### 3. Cerid-internal deterministic sort / hash / heap

`crd-containers` (Phase 3.1 v0c, sibling to the math substrate) gains:

- `crd::containers::sort` / `stable_sort` / `nth_element` —
  pdqsort-derived introsort with a pinned tie-breaker (compare by
  `(key, original_index)` so equal keys sort by source position
  deterministically).
- `crd::containers::HashMap` — already exists with FNV-1a 64; no
  change.
- `crd::containers::push_heap` / `pop_heap` / `make_heap` — same
  signature as `<algorithm>` heap ops, deterministic tie-break.

The lint check forbids the `std::` versions in `crd-eylem` source
trees. `crd-scene`, `crd-resources`, and other consumers may
*optionally* adopt the deterministic versions if they participate in
deterministic features (Öbek already uses deterministic ordering;
ADR-0058 §4-§5).

### 4. Cross-thread merge discipline

When eylem's solver / broadphase / island-detection writes shared state
from multiple jobs:

- **Bit OR** for boolean / mask aggregations (commutative + associative
  → order-independent).
- **Fixed-position index push** for variable-length arrays (each thread
  writes to a pre-reserved range; merge is a fixed-order memcpy of the
  ranges in id-stable order).
- **Atomic max / min** for "deepest penetration wins" type aggregations.
- **NEVER** `atomic_fetch_add` on a counter to get "next free index" —
  that order depends on thread scheduling. Pre-reserve and write to a
  fixed slot.
- **NEVER** lock + first-in-wins. The schedule is supposed to be
  reproducible.

### 5. Replay-hash CI matrix

The contract above is meaningless without a CI matrix that exercises it.

**v1j (in v1, the rigid-3D substrate)** ships the harness:

- `eylem::record(scene, inputs, seed) → ReplayLog`
- `eylem::replay(scene, ReplayLog) → SnapshotHash`
- A test that records 10 seconds of "100 falling boxes + 1 ragdoll +
  1 character running around" then asserts the snapshot hash matches
  a golden reference checked into the repo.

**v9b (in v9, the determinism hardening slice)** expands it to the
full CI matrix:

```
9 configs = {MSVC, clang-cl, gcc} × {x64, ARM64} × {Windows-debug,
                                                     Linux-debug,
                                                     Linux-release}
```

Each config runs the v1j replay test; the resulting snapshot hash must
match the golden reference. Any divergence is a P0 — the contract has
broken somewhere in the dependency chain.

The CI job logs the hash on every run so a regression is bisectable.

### 6. Optional fixed-point fallback (v9c)

For lockstep multiplayer (esports tier — Photon Quantum / Delta Strike
class) where bit-exact across hardware is non-negotiable and FP
determinism's ~3% margin is too much risk, eylem v9c adds an *optional*
fixed-point world (`crd-eylem-fp`). Q42.20 fixed-point wrappers around
the SI solver, no SIMD, no transcendentals (lookup tables instead).
Performance is ~30–50% slower; deterministic guarantees are absolute
(integer arithmetic is bit-exact across IEEE-754 hardware by
definition).

This is **not v1**. v1 ships FP-deterministic; v9c is the
extra-paranoia fallback.

### 7. Snapshot serialisation format

The world snapshot is a CRDR artifact (FourCC `'EYLM'`) carrying:

- Body pool: AoSoA SoA → flat dump in stable id order.
- Shape pools: per-shape SoA flat dump in stable id order.
- Joint pool: id-stable.
- Contact warm-start cache: id-stable.
- Sleep state: id-stable.
- World config (seed, gravity, iteration counts).

The hash is FNV-1a 64 over the flat snapshot bytes. The format is
versioned (`schema_version`) so a v2 layout change doesn't invalidate
v1 replay logs without explicit migration — same discipline as
ADR-0055 / ADR-0058 / ADR-0059.

## Rationale

### Why bake in from day 1

The retrofit cost ratio (~5–6× the bake-in cost) plus the months-of-
rewrite latency makes the math obvious. The harder question is whether
eylem genuinely *needs* determinism. The answer is yes for at least
four downstream consumers (rollback netcode, robotics RL, cinematic
reproducibility, bug repro). Building the substrate without it forecloses
all four.

### Why FP-deterministic, not fixed-point first

Box2D v3, Jolt, Rapier all prove that FP-deterministic is achievable
across MSVC / clang / gcc × x64 / ARM with a careful contract. Performance
parity is good (~1–3% from the bake-in) and downstream code stays in
floats — every other Cerid module assumes float math. Fixed-point is the
right answer for esports lockstep where bit-exactness is contractual; it's
the wrong default because every consumer pays the perf + ergonomics tax
without using the guarantee.

### Why a custom trig library

libm divergence between MSVC's `sinf`, glibc's `sinf`, and musl's `sinf`
is the single largest source of cross-platform FP non-determinism in
practice (Box2D v3 cites it as the motivation for `b2Sin`/`b2Cos`).
Replacing them with a single Cephes-style polynomial — same bytecode
across compilers — eliminates the source.

### Why splittable PCG, not Mersenne Twister

PCG is small (state = 16 bytes), splittable (deriving a child stream
from `(parent_state, salt)` is a single multiply + add), and proven
statistically. MT carries ~2.5 KB of state and isn't splittable —
seeding per-island is awkward. PCG is what JAX uses for its
parallel-RNG primitives, which is the same problem domain (deterministic
parallel simulation).

### Why the lint check is non-negotiable

Six months from now, someone adds `std::sin` to a hot path because they
forgot the contract. Without a `clang-tidy` check, the snapshot-hash CI
catches it eventually but only across one of nine configs and the
bisect points at "some commit in the last week." With the lint check,
the PR fails before merge with a clear pointer at the violation.

## Consequences

- **`crd-math`** grows `crd::math::deterministic` namespace in Phase 3.1
  v0c (sin / cos / tan / atan2 / asin / acos / exp / log / pow as
  Cephes-style polynomials). CI tests bit-exactness across the v9b
  matrix.
- **`crd-containers`** grows `crd::containers::sort` /
  `stable_sort` / `nth_element` / `push_heap` / `pop_heap` /
  `make_heap` in Phase 3.1 v0c.
- **`crd-eylem`** carries the contract in its module documentation; PR
  reviews check for violations.
- **`.clang-tidy`** root file gains a custom check (or a curated set of
  `readability-identifier-naming` + `cppcoreguidelines-*` rules) that
  bans `std::sin` etc. in `engine/eylem/**` paths.
- **CMake presets** for `crd-eylem` set `-ffp-contract=off` /
  `/fp:precise` explicitly.
- **CI v9b** runs the 9-config replay-hash matrix. New job in
  `.github/workflows/ci.yml`.
- **Snapshot artifacts** are CRDR `EYLM` containers carrying the body
  / shape / joint / contact pools.
- The 8% perf number from Jolt's flag is the *upper* bound on the
  contract's cost; designing in saves us most of that.

## References

- `docs/research/cerid-eylem.md` — full research, esp. § *Determinism*
- `docs/decisions/0062-eylem-physics-architecture.md` — sister ADR
- `docs/phases/phase-3.1-eylem.md` — phased slice plan; v0c =
  deterministic stdlib substitutions, v1j = replay harness, v9b =
  9-config CI matrix, v9c = optional fixed-point fallback
- ADR-0035 — Networking (the rollback consumer that needs this)
- ADR-0058 — Öbek (already follows similar discipline for cooker
  reproducibility)
- [Box2D Determinism, Catto 2024](https://box2d.org/posts/2024/08/determinism/)
- [Rapier `enhanced-determinism` docs](https://rapier.rs/docs/user_guides/rust/determinism/)
- [Sleef library](https://sleef.org/) — reference SIMD math
- [Cephes Mathematical Library](https://www.netlib.org/cephes/) —
  reference scalar transcendentals
