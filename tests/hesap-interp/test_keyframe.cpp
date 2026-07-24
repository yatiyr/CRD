// test_keyframe.cpp — GEO-8 (D-007 row 73): the ONE curve engine's gates. glTF-exact semantics: key times return
// EXACT stored values (bit-equal — the bit-stable-resample contract's foundation), Step holds left, Linear lerps,
// CubicHermite reproduces the spec's split-tangent Hermite (hand-computed oracle), out-of-range clamps.

#include <crd/containers/span.hpp>
#include <crd/hesap/interp/keyframe.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;
using namespace crd::hesap::interp;

namespace
{
template <typename T> containers::ConstSpan<T> sp(const T* p, usize n) { return {p, n}; }
} // namespace

TEST_CASE("keyframe: key times return EXACT stored values, out-of-range clamps", "[hesap][interp][keyframe][geo8]")
{
    const f32 times[3]  = {0.5F, 1.25F, 3.0F};
    const f32 values[6] = {1.0F, -2.0F, 0.125F, 7.5F, 100.0F, 0.0033F}; // 2 components per key
    for (const KeyInterp interp : {KeyInterp::Step, KeyInterp::Linear})
    {
        usize cache = 0;
        for (u32 k = 0; k < 3U; ++k)
        {
            for (u32 lane = 0; lane < 2U; ++lane)
            {
                const f32 v = sample_track(sp(times, 3U), sp(values, 6U), 2U, lane, interp, times[k], cache);
                CHECK(v == values[k * 2U + lane]); // BIT-exact at keys
            }
        }
        // clamp: before-first and after-last return the boundary values exactly
        CHECK(sample_track(sp(times, 3U), sp(values, 6U), 2U, 0U, interp, -5.0F, cache) == values[0]);
        CHECK(sample_track(sp(times, 3U), sp(values, 6U), 2U, 1U, interp, 99.0F, cache) == values[5]);
    }
}

TEST_CASE("keyframe: Step holds left, Linear lerps the midpoint exactly", "[hesap][interp][keyframe][geo8]")
{
    const f32 times[2]  = {1.0F, 3.0F};
    const f32 values[2] = {10.0F, 30.0F};
    usize     cache     = 0;
    CHECK(sample_track(sp(times, 2U), sp(values, 2U), 1U, 0U, KeyInterp::Step, 2.9F, cache) == 10.0F);
    CHECK(sample_track(sp(times, 2U), sp(values, 2U), 1U, 0U, KeyInterp::Linear, 2.0F, cache) == 20.0F);
    CHECK(sample_track(sp(times, 2U), sp(values, 2U), 1U, 0U, KeyInterp::Linear, 1.5F, cache) == 15.0F);
}

TEST_CASE("keyframe: CubicHermite reproduces the glTF split-tangent Hermite (hand oracle)",
          "[hesap][interp][keyframe][geo8]")
{
    // one segment [0, 2]: v0=1 (out-tangent b0=3), v1=5 (in-tangent a1=-1). glTF basis at u:
    // p = h00·v0 + Δ·h10·b0 + h01·v1 + Δ·h11·a1, Δ=2.
    // per key: [in, value, out] → key0: {9, 1, 3}, key1: {-1, 5, 7} (unused tangents arbitrary)
    const f32 times[2]  = {0.0F, 2.0F};
    const f32 values[6] = {9.0F, 1.0F, 3.0F, -1.0F, 5.0F, 7.0F};
    usize     cache     = 0;

    // u = 0.5: h00=0.5, h10=0.125, h01=0.5, h11=-0.125 → 0.5·1 + 2·0.125·3 + 0.5·5 + 2·(−0.125)·(−1) = 4.0
    const f32 mid = sample_track(sp(times, 2U), sp(values, 6U), 1U, 0U, KeyInterp::CubicHermite, 1.0F, cache);
    CHECK(mid == 4.0F);
    // key times return the VALUE element exactly (never a tangent)
    CHECK(sample_track(sp(times, 2U), sp(values, 6U), 1U, 0U, KeyInterp::CubicHermite, 0.0F, cache) == 1.0F);
    CHECK(sample_track(sp(times, 2U), sp(values, 6U), 1U, 0U, KeyInterp::CubicHermite, 2.0F, cache) == 5.0F);
    // the derivative at the left key is the OUT-tangent: finite difference check at small h
    const f32 eps = 1.0F / 1024.0F;
    const f32 d0  = (sample_track(sp(times, 2U), sp(values, 6U), 1U, 0U, KeyInterp::CubicHermite, eps, cache) - 1.0F)
                   / eps;
    CHECK(d0 > 2.9F);
    CHECK(d0 < 3.1F);
}

TEST_CASE("keyframe: validate_track rejects the malformed classes", "[hesap][interp][keyframe][geo8]")
{
    const f32 good_t[2] = {0.0F, 1.0F};
    const f32 good_v[2] = {1.0F, 2.0F};
    CHECK(validate_track(sp(good_t, 2U), sp(good_v, 2U), 1U, KeyInterp::Linear) == InterpStatus::Ok);

    const f32 bad_t[2] = {1.0F, 1.0F}; // not strictly increasing
    CHECK(validate_track(sp(bad_t, 2U), sp(good_v, 2U), 1U, KeyInterp::Linear) == InterpStatus::NotIncreasing);
    CHECK(validate_track(sp(good_t, 2U), sp(good_v, 1U), 1U, KeyInterp::Linear) == InterpStatus::BadInput);
    // cubic needs 3× the values
    CHECK(validate_track(sp(good_t, 2U), sp(good_v, 2U), 1U, KeyInterp::CubicHermite) == InterpStatus::BadInput);
    const f32 nan_v[2] = {1.0F, 0.0F / (good_t[0] + 0.0F)}; // NaN smuggled arithmetically
    CHECK(validate_track(sp(good_t, 2U), sp(nan_v, 2U), 1U, KeyInterp::Linear) == InterpStatus::BadInput);
}
