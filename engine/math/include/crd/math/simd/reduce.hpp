#pragma once

// ---------------------------------------------------------------------------
// crd::math::simd — horizontal reductions with deterministic tiebreaks.
//
// `reduce_argmax_with_lex_tiebreak` — "which lane has the largest score?", but
// when two lanes tie on score the winner is the one whose (x, y, z) is
// lexicographically smallest, and when *that* ties too the lowest global index
// wins. This is the determinism pin ADR-0076 §4 #10 needs: Quickhull (geometry
// v3) picks the furthest point from a face; ties must resolve identically on
// every SIMD width / platform or the hull's triangulation diverges. The
// reduction is a small scalar scan over the extracted lanes — SIMD vectorising a
// horizontal argmax with a 4-key comparator buys nothing, and a scalar scan is
// trivially identical to the scalar-backend path.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <limits>

namespace crd::math::simd
{
// A candidate / running-best for the argmax-with-lex-tiebreak reduction:
// `index` is the global element index (`< 0` ⇒ "no candidate yet"), `score` the
// quantity being maximised, `(x, y, z)` the lexicographic tiebreak key.
struct ArgmaxLex
{
    crd::i32 index{-1};
    crd::f32 score{-std::numeric_limits<crd::f32>::infinity()};
    crd::f32 x{0.0F};
    crd::f32 y{0.0F};
    crd::f32 z{0.0F};
};

// The "no candidate yet" sentinel — beaten by any valid candidate.
[[nodiscard]] constexpr ArgmaxLex argmax_lex_invalid() noexcept
{
    return ArgmaxLex{};
}

// Does `a` strictly beat `b` for argmax-with-lex-tiebreak?
//   1. an invalid `b` (no candidate) is beaten by any valid `a`;
//   2. a NaN score never wins (and a NaN running-best is replaced by a valid a);
//   3. higher `score` wins;
//   4. equal score → lexicographically-smaller (x, y, z) wins;
//   5. equal key too → the lower `index` wins (so `a` does NOT beat an equal `b`).
[[nodiscard]] constexpr bool argmax_lex_beats(const ArgmaxLex& a, const ArgmaxLex& b) noexcept
{
    if (a.index < 0)
    {
        return false;
    }
    if (b.index < 0)
    {
        return true;
    }
    const bool a_nan = a.score != a.score;
    const bool b_nan = b.score != b.score;
    if (a_nan)
    {
        return false;
    }
    if (b_nan)
    {
        return true;
    }
    if (a.score != b.score)
    {
        return a.score > b.score;
    }
    if (a.x != b.x)
    {
        return a.x < b.x;
    }
    if (a.y != b.y)
    {
        return a.y < b.y;
    }
    if (a.z != b.z)
    {
        return a.z < b.z;
    }
    return a.index < b.index;
}

// Fold the first `lanes` (1..8) lanes of (score, x, y, z) — global index =
// `base_index + lane` — into the running best, returning the updated best.
// Lanes scanned in ascending order so the index tiebreak is deterministic.
[[nodiscard]] inline ArgmaxLex reduce_argmax_with_lex_tiebreak(ArgmaxLex running, const Vec8f& score, const Vec8f& x,
                                                               const Vec8f& y, const Vec8f& z, crd::u32 base_index,
                                                               crd::u32 lanes) noexcept
{
    alignas(32) crd::f32 s[8];
    alignas(32) crd::f32 px[8];
    alignas(32) crd::f32 py[8];
    alignas(32) crd::f32 pz[8];
    score.store(s);
    x.store(px);
    y.store(py);
    z.store(pz);
    const crd::u32 n = lanes < 8U ? lanes : 8U;
    for (crd::u32 lane = 0U; lane < n; ++lane)
    {
        const ArgmaxLex cand{static_cast<crd::i32>(base_index + lane), s[lane], px[lane], py[lane], pz[lane]};
        if (argmax_lex_beats(cand, running))
        {
            running = cand;
        }
    }
    return running;
}

// Vec4f chunk overload.
[[nodiscard]] inline ArgmaxLex reduce_argmax_with_lex_tiebreak(ArgmaxLex running, const Vec4f& score, const Vec4f& x,
                                                               const Vec4f& y, const Vec4f& z, crd::u32 base_index,
                                                               crd::u32 lanes) noexcept
{
    alignas(16) crd::f32 s[4];
    alignas(16) crd::f32 px[4];
    alignas(16) crd::f32 py[4];
    alignas(16) crd::f32 pz[4];
    score.store(s);
    x.store(px);
    y.store(py);
    z.store(pz);
    const crd::u32 n = lanes < 4U ? lanes : 4U;
    for (crd::u32 lane = 0U; lane < n; ++lane)
    {
        const ArgmaxLex cand{static_cast<crd::i32>(base_index + lane), s[lane], px[lane], py[lane], pz[lane]};
        if (argmax_lex_beats(cand, running))
        {
            running = cand;
        }
    }
    return running;
}

} // namespace crd::math::simd
