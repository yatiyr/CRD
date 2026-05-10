# 2026-05-10 — Phase 3.1 v0b: AoSoA storage substrate (`crd::math::simd::Soa`)

**Phase 3.1 v0b (per `docs/phases/phase-3.1-eylem.md` v0 table) shipped.**
Adds `Soa<TChunk, Lane>` typed AoSoA container + `soa_for_each_chunk` /
`soa_for_each_lane` iteration helpers + `gather8` / `scatter8` /
`gather4` / `scatter4` cross-chunk lane movers in
`engine/math/include/crd/math/simd/soa.hpp`.

Builds on v0a: `Lane` defaults to `k_native_lane_width`
(8 on AVX2, 4 on SSE2/NEON/scalar). User defines `TChunk` explicitly
as a struct of `Vec8f` (or `Vec4f`) columns — no template reflection
magic. Storage backed by `crd::containers::Array<TChunk>` with
chunk-aligned grow semantics.

## What landed

### Code

| File | Lines | Notes |
|---|---:|---|
| `engine/math/include/crd/math/simd/soa.hpp` (new) | ~240 | `Soa<TChunk, Lane>` template + iteration helpers + gather/scatter |
| `engine/math/include/crd/math/simd/simd.hpp` | +1 | umbrella header now pulls in soa.hpp |
| `tests/math/test_soa.cpp` (new) | ~290 | 19 test cases / 345 assertions |
| `tests/math/CMakeLists.txt` | +3 | adds test_soa.cpp; links `crd-memory` + `crd-containers` |

Total: **~530 LOC** (slightly above the ~400 estimate — extra came from
the per-test-case sizing-math fixtures, not from API surface).

### Public surface (`crd::math::simd`)

```cpp
template <typename TChunk, usize Lane = k_native_lane_width>
class Soa
{
    static constexpr usize lanes_per_chunk = Lane;
    using ChunkType = TChunk;

    explicit Soa(crd::memory::IAllocator* = crd::memory::default_allocator());

    // sizing
    void  resize(usize logical_size);  // rounds chunk array up to ceil(n/Lane)
    void  reserve(usize logical_size);
    void  clear();

    usize size()        const;          // logical entity count
    usize chunk_count() const;
    bool  empty()       const;
    usize last_chunk_active_lanes() const;  // 0 if empty, 1..Lane otherwise

    // chunk access (the SIMD path)
    TChunk&       chunk(usize i);
    const TChunk& chunk(usize i) const;
    Span<TChunk>      chunks();
    ConstSpan<TChunk> chunks() const;

    // index decomposition (constexpr)
    static constexpr usize chunk_of (usize global_idx);
    static constexpr usize lane_of  (usize global_idx);
    static constexpr usize make_index(usize chunk_idx, usize lane_idx);
};

// iteration
template <Fn> void soa_for_each_chunk(Soa&,       Fn&&);  // SIMD path
template <Fn> void soa_for_each_chunk(const Soa&, Fn&&);
template <Fn> void soa_for_each_lane (Soa&,       Fn&&);  // slow path
template <Fn> void soa_for_each_lane (const Soa&, Fn&&);

// gather / scatter (Vec8f columns)
Vec8f gather8 (const Soa&, Vec8f TChunk::* member, const u32 (&)[8]);
void  scatter8(      Soa&, Vec8f TChunk::* member, const u32 (&)[8], Vec8f);

// gather / scatter (Vec4f columns)
Vec4f gather4 (const Soa&, Vec4f TChunk::* member, const u32 (&)[4]);
void  scatter4(      Soa&, Vec4f TChunk::* member, const u32 (&)[4], Vec4f);
```

### Static layout pins

```cpp
static_assert(Lane > 0);
static_assert(Lane == 4 || Lane == 8);          // v0b accepts both widths
static_assert(alignof(TChunk) >= 16);           // SIMD safety
```

### Iteration semantics

**`soa_for_each_chunk`** — calls the lambda once per chunk with
`(TChunk& chunk, usize active_lane_count)`. For all chunks except the
last, `active_lane_count == Lane`. The last chunk reports the actual
active count (1..Lane) so callers can mask off the partial tail when
the operation has lane-sensitive correctness (e.g. accumulating into a
global counter).

**`soa_for_each_lane`** — calls the lambda once per logical entity with
`(TChunk& chunk, usize lane_idx)`. Slow path; for ad-hoc lookups,
debug inspection, and small-N code paths where SIMD-setup overhead
exceeds the work.

### Gather / scatter semantics

Software implementation (extract-and-pack). Hardware gather
(`_mm256_i32gather_ps`) is faster on some micro-architectures but its
performance characteristics + edge-case rounding vary across CPUs.
Substrate keeps the determinism contract straightforward by going
scalar-by-scalar. Hardware-gather fast path is reserved for v0e
benchmark-driven optimisation, not v0b substrate work.

`scatter8` round-trips each target column through a stack array
(store → modify single lane → load) because `Vec8f` doesn't expose a
per-lane write API. That's deliberate: per-lane SIMD writes vary across
back-ends and would break the bit-exact parity contract from v0a. The
8-extract / 8-insert path is fast enough for graph-coloured constraint
solvers (the dominant scatter consumer in eylem v1).

## Pinned design choices

1. **No reflection magic.** `TChunk` is a user-defined struct;
   `Soa<TChunk, Lane>` is a thin container. The substrate doesn't try
   to "lift" an AoS user type into AoSoA layout via template
   metaprogramming — that scope explosion isn't warranted for v0b.
   Eylem will define `BodyChunk8` etc. by hand.

2. **`Lane` is closed: 4 or 8 only.** v0b refuses other widths with a
   `static_assert`. AVX-512 (`Lane=16`) is reserved until AVX-512
   becomes a Cerid CI target.

3. **`size()` is logical, `chunk_count()` is physical.** A user with 17
   entities has `size() == 17`, `chunk_count() == 3`,
   `last_chunk_active_lanes() == 1`. Iteration helpers report
   `active_lane_count` so the caller can decide whether to mask the
   tail (correctness-sensitive ops) or run the full chunk and ignore
   the inactive lanes (idempotent ops like `pos += vel * dt` where
   the tail is also valid storage).

4. **`Array::resize` value-initialises new chunks.** New `TChunk`
   slots get zero-filled `Vec8f`/`Vec4f` columns (from the
   `T()` value-init in `Array::resize`). User can rely on that: a
   freshly-`resize`d `Soa` reads back zeros, not garbage.

5. **`gather8` / `scatter8` over `Vec8f TChunk::*` member pointers.**
   The user-facing API takes a pointer-to-data-member of the chunk
   struct. Compile-time checked, no string lookup. Eylem v1
   constraint code looks like:
   ```cpp
   const Vec8f vx_a = gather8(bodies, &BodyChunk8::vel_x, body_a_indices);
   ```

6. **No swap-and-pop / hole-filling in v0b.** Entity destruction
   semantics (which entity moves where, who notifies the index, etc.)
   land with eylem v1 when the actual contract becomes concrete.
   Resize-to-N is enough for v0b's "the substrate exists" milestone.

## Verification

| Aspect | Coverage |
|---|---|
| Static layout | `STATIC_REQUIRE` on `alignof`/`sizeof` for both `Body8` (32 B / 64 B total) and `Body4` (16 B / 32 B total) |
| Sizing math | `resize(0..24)` + cross-checked `chunk_count` + `last_chunk_active_lanes` for both Lane=4 and Lane=8 |
| Index decomposition | `chunk_of(c*Lane + l) == c && lane_of(c*Lane + l) == l` for the full `[0, 64)` range |
| Iteration coverage | `soa_for_each_chunk` visits exactly `chunk_count` chunks; sum of `active` equals logical `size()` |
| Iteration correctness | `soa_for_each_chunk` emits `Lane` for full chunks + partial value for last; empty Soa is a no-op |
| Lane iteration | `soa_for_each_lane` calls lambda exactly `size()` times |
| Gather8 | reads from arbitrary indices spanning multiple chunks |
| Scatter8 | round-trips with gather8; leaves untouched lanes intact |
| Gather4/scatter4 | same on Vec4f columns with Lane=4 |
| End-to-end SIMD step | `pos += vel * dt` over 20 entities (3 chunks: 8 + 8 + 4) — bit-exact result |

19 test cases / 345 assertions. All pass on win-debug.

Plus: full math suite is now **91 test cases / 2744 assertions** (was
72/2399 after v0a; +19 cases / +345 assertions for v0b).

## Definition of Done

Full 12-config sweep (Win × 7 + Linux × 5, including the new
`win-debug-scalar` + `linux-gcc-debug-scalar`) — see "Test counts"
section in `context.md` after this session's commit.

The v0a `crd-simd-emission-check` CTest test continues to pass on every
config. SOA itself doesn't add new emission instructions (the `Vec8f`
ops in test_soa are exactly what test_simd already exercised), so v0b
inherits the AVX2-emission guarantee without extending the check.

## Next slice

**v0c — `crd::math::deterministic`**: Cephes-style polynomial
sin/cos/tan/atan2/asin/acos/exp/log/pow with bit-exact CI test across
MSVC/clang/gcc × x64/ARM (ADR-0063 §2). The deterministic stdlib
substitutions that eylem will use everywhere `std::sin` etc. would
otherwise sneak in.

## References

- Phase plan: `docs/phases/phase-3.1-eylem.md` (v0 table, v0b row).
- v0a session log: `docs/sessions/2026-05-10-v0a-simd-substrate.md`.
- ADR-0063 — determinism contract (inherited; v0b's `gather8`/`scatter8`
  scalar-by-scalar implementation respects it by construction).
- `docs/systems/math-simd.md` — system overview (now lists `Soa` in the
  inventory).
