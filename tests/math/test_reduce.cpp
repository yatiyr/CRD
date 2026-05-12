// crd::math::simd::reduce_argmax_with_lex_tiebreak (Phase 3.1.7 v0e, the
// determinism pin ADR-0076 sec4 #10 -- what geometry v3 Quickhull needs).
// Covers: the `argmax_lex_beats` comparator (score / lex / index / NaN /
// invalid), the Vec8f & Vec4f chunk folds, partial lanes, and the key property
// -- the result is independent of how the lanes are partitioned into chunks.

#include <crd/math/simd/reduce.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace crd;
using namespace crd::math::simd;

namespace
{
Vec8f v8(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f, f32 g, f32 h)
{
    return Vec8f(Vec4f(a, b, c, d), Vec4f(e, f, g, h));
}
const f32 k_nan = std::numeric_limits<f32>::quiet_NaN();

// Reference scalar argmax-with-lex over a flat array (the ground truth).
struct Ref
{
    i32 index{-1};
    f32 score{}, x{}, y{}, z{};
};
Ref ref_argmax(const f32* s, const f32* px, const f32* py, const f32* pz, i32 n)
{
    Ref best;
    for (i32 i = 0; i < n; ++i)
    {
        ArgmaxLex cand{i, s[i], px[i], py[i], pz[i]};
        ArgmaxLex cur{best.index, best.score, best.x, best.y, best.z};
        if (argmax_lex_beats(cand, cur))
        {
            best = Ref{i, s[i], px[i], py[i], pz[i]};
        }
    }
    return best;
}
} // namespace

TEST_CASE("argmax_lex_beats -- score / lex / index / NaN / invalid", "[math][simd][reduce]")
{
    const ArgmaxLex inv = argmax_lex_invalid();
    REQUIRE_FALSE(inv.index >= 0);
    const ArgmaxLex a{3, 1.0F, 0.0F, 0.0F, 0.0F};
    REQUIRE(argmax_lex_beats(a, inv));       // any valid beats the sentinel
    REQUIRE_FALSE(argmax_lex_beats(inv, a)); // and not vice versa
    REQUIRE_FALSE(argmax_lex_beats(inv, inv));

    // higher score wins
    REQUIRE(argmax_lex_beats(ArgmaxLex{0, 2.0F, 9, 9, 9}, ArgmaxLex{1, 1.0F, 0, 0, 0}));
    REQUIRE_FALSE(argmax_lex_beats(ArgmaxLex{1, 1.0F, 0, 0, 0}, ArgmaxLex{0, 2.0F, 9, 9, 9}));

    // equal score -> lexicographically-smaller (x, y, z) wins
    REQUIRE(argmax_lex_beats(ArgmaxLex{5, 1.0F, -1.0F, 9, 9}, ArgmaxLex{0, 1.0F, 0.0F, 0, 0})); // x smaller
    REQUIRE(
        argmax_lex_beats(ArgmaxLex{5, 1.0F, 1.0F, -2.0F, 9}, ArgmaxLex{0, 1.0F, 1.0F, 0.0F, 0})); // x tie, y smaller
    REQUIRE(argmax_lex_beats(ArgmaxLex{5, 1.0F, 1.0F, 2.0F, -3.0F}, ArgmaxLex{0, 1.0F, 1.0F, 2.0F, 0.0F})); // z
    REQUIRE_FALSE(argmax_lex_beats(ArgmaxLex{0, 1.0F, 1.0F, 0, 0}, ArgmaxLex{5, 1.0F, -1.0F, 9, 9}));

    // full key tie -> lower index wins (so the higher-index one does NOT beat)
    REQUIRE_FALSE(argmax_lex_beats(ArgmaxLex{7, 1.0F, 1, 2, 3}, ArgmaxLex{2, 1.0F, 1, 2, 3}));
    REQUIRE(argmax_lex_beats(ArgmaxLex{2, 1.0F, 1, 2, 3}, ArgmaxLex{7, 1.0F, 1, 2, 3}));

    // NaN score never wins; a NaN running-best is replaced by a valid candidate
    REQUIRE_FALSE(argmax_lex_beats(ArgmaxLex{0, k_nan, 0, 0, 0}, ArgmaxLex{1, 1.0F, 0, 0, 0}));
    REQUIRE(argmax_lex_beats(ArgmaxLex{1, 1.0F, 0, 0, 0}, ArgmaxLex{0, k_nan, 0, 0, 0}));
}

TEST_CASE("reduce_argmax_with_lex_tiebreak -- Vec8f single chunk", "[math][simd][reduce]")
{
    SECTION("plain argmax")
    {
        const ArgmaxLex r = reduce_argmax_with_lex_tiebreak(argmax_lex_invalid(), v8(1, 5, 2, 9, 3, 4, 0, -2),
                                                            v8(0, 0, 0, 0, 0, 0, 0, 0), v8(0, 0, 0, 0, 0, 0, 0, 0),
                                                            v8(0, 0, 0, 0, 0, 0, 0, 0), 0U, 8U);
        REQUIRE(r.index == 3); // score 9 at lane 3
        REQUIRE(r.score == 9.0F);
    }
    SECTION("score tie -> lex on (x, y, z)")
    {
        // lanes 1, 3, 6 all score 7; their x are 2, -1, 5 -> lane 3 (x = -1) wins
        const ArgmaxLex r = reduce_argmax_with_lex_tiebreak(argmax_lex_invalid(), v8(1, 7, 2, 7, 3, 4, 7, 0),
                                                            v8(0, 2, 0, -1, 0, 0, 5, 0), v8(0, 0, 0, 0, 0, 0, 0, 0),
                                                            v8(0, 0, 0, 0, 0, 0, 0, 0), 0U, 8U);
        REQUIRE(r.index == 3);
    }
    SECTION("full tie -> lowest lane index")
    {
        const ArgmaxLex r = reduce_argmax_with_lex_tiebreak(argmax_lex_invalid(), v8(7, 7, 7, 7, 7, 7, 7, 7),
                                                            v8(1, 1, 1, 1, 1, 1, 1, 1), v8(2, 2, 2, 2, 2, 2, 2, 2),
                                                            v8(3, 3, 3, 3, 3, 3, 3, 3), 0U, 8U);
        REQUIRE(r.index == 0);
    }
    SECTION("partial lanes (lanes < 8) ignore the tail")
    {
        // lane 5 has the highest score but lanes=4 => only lanes 0..3 considered
        const ArgmaxLex r = reduce_argmax_with_lex_tiebreak(argmax_lex_invalid(), v8(1, 2, 3, 4, 99, 99, 99, 99),
                                                            v8(0, 0, 0, 0, 0, 0, 0, 0), v8(0, 0, 0, 0, 0, 0, 0, 0),
                                                            v8(0, 0, 0, 0, 0, 0, 0, 0), 0U, 4U);
        REQUIRE(r.index == 3);
        REQUIRE(r.score == 4.0F);
    }
    SECTION("global index = base_index + lane")
    {
        const ArgmaxLex r = reduce_argmax_with_lex_tiebreak(argmax_lex_invalid(), v8(1, 2, 3, 4, 5, 6, 7, 8),
                                                            v8(0, 0, 0, 0, 0, 0, 0, 0), v8(0, 0, 0, 0, 0, 0, 0, 0),
                                                            v8(0, 0, 0, 0, 0, 0, 0, 0), 100U, 8U);
        REQUIRE(r.index == 107); // 100 + 7
    }
}

TEST_CASE("reduce_argmax_with_lex_tiebreak -- partition-independent (the determinism property)", "[math][simd][reduce]")
{
    // 16 elements: some score ties, some full ties, NaN sprinkled in.
    const f32 s[16] = {1, 5, 5, 9, 3, 9, 0, 9, 9, 9, 9, 2, 4, k_nan, 9, 9};
    const f32 px[16] = {0, 2, 1, 7, 0, 7, 0, 7, 7, 7, 3, 0, 0, 0, 7, 7};
    const f32 py[16] = {0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 5, 0, 0, 0, 0, 0};
    const f32 pz[16] = {0, 0, 0, 9, 0, 9, 0, 8, 9, 9, 5, 0, 0, 0, 9, 9};
    const Ref ground = ref_argmax(s, px, py, pz, 16);

    auto load8 = [&](int o)
    {
        return v8(s[o], s[o + 1], s[o + 2], s[o + 3], s[o + 4], s[o + 5], s[o + 6], s[o + 7]);
    };
    auto load8x = [&](const f32* a, int o)
    {
        return v8(a[o], a[o + 1], a[o + 2], a[o + 3], a[o + 4], a[o + 5], a[o + 6], a[o + 7]);
    };

    // (a) two 8-lane chunks
    {
        ArgmaxLex r = argmax_lex_invalid();
        r = reduce_argmax_with_lex_tiebreak(r, load8(0), load8x(px, 0), load8x(py, 0), load8x(pz, 0), 0U, 8U);
        r = reduce_argmax_with_lex_tiebreak(r, load8(8), load8x(px, 8), load8x(py, 8), load8x(pz, 8), 8U, 8U);
        REQUIRE(r.index == ground.index);
        REQUIRE((r.score == ground.score || (r.score != r.score && ground.score != ground.score)));
        REQUIRE(r.x == ground.x);
        REQUIRE(r.y == ground.y);
        REQUIRE(r.z == ground.z);
    }
    // (b) four 4-lane chunks (Vec4f overload)
    {
        ArgmaxLex r = argmax_lex_invalid();
        for (u32 o = 0; o < 16U; o += 4U)
        {
            r = reduce_argmax_with_lex_tiebreak(
                r, Vec4f(s[o], s[o + 1], s[o + 2], s[o + 3]), Vec4f(px[o], px[o + 1], px[o + 2], px[o + 3]),
                Vec4f(py[o], py[o + 1], py[o + 2], py[o + 3]), Vec4f(pz[o], pz[o + 1], pz[o + 2], pz[o + 3]), o, 4U);
        }
        REQUIRE(r.index == ground.index);
        REQUIRE(r.x == ground.x);
    }
    // (c) one 8 + one partial-of-2 (8 used, then a chunk with lanes=2 starting at base 8 -- but only
    //     elements 8,9 there; the rest of that "chunk" is garbage that lanes=2 must ignore)
    {
        ArgmaxLex r = argmax_lex_invalid();
        r = reduce_argmax_with_lex_tiebreak(r, load8(0), load8x(px, 0), load8x(py, 0), load8x(pz, 0), 0U, 8U);
        // a Vec8f whose first 2 lanes are elements 8,9 and the rest junk; lanes=2 => junk ignored
        r = reduce_argmax_with_lex_tiebreak(r, v8(s[8], s[9], 999, 999, 999, 999, 999, 999),
                                            v8(px[8], px[9], 9, 9, 9, 9, 9, 9), v8(py[8], py[9], 9, 9, 9, 9, 9, 9),
                                            v8(pz[8], pz[9], 9, 9, 9, 9, 9, 9), 8U, 2U);
        // now elements 10..15
        for (u32 i = 10; i < 16U; ++i)
        {
            const ArgmaxLex cand{static_cast<i32>(i), s[i], px[i], py[i], pz[i]};
            if (argmax_lex_beats(cand, r))
            {
                r = cand;
            }
        }
        REQUIRE(r.index == ground.index);
    }
}
