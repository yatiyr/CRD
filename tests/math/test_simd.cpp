// SIMD wrapper tests — Phase 3.1 v0a.
//
// Strategy: scalar-reference parity. Every SIMD op is computed with the
// production type (which selects backend at compile time) and bit-exact
// compared against an explicit scalar reference computed with plain f32
// arithmetic in the canonical order. ADR-0063 contract: same input → same
// f32 bit-pattern across SSE2 / AVX2 / NEON / scalar.

#include <catch2/catch_test_macros.hpp>

#include <crd/math/simd/simd.hpp>

#include <bit>
#include <cmath>

using crd::f32;
using crd::math::simd::Vec4f;
using crd::math::simd::Vec8f;
using crd::math::simd::Mat4f;
using crd::math::simd::Quatf;

namespace
{
// Bit-exact float compare: catches the case where two values are
// numerically equal but differ in NaN payload / zero sign etc.
[[nodiscard]] bool bit_eq(f32 a, f32 b) noexcept
{
    return std::bit_cast<crd::u32>(a) == std::bit_cast<crd::u32>(b);
}

[[nodiscard]] bool vec4_bit_eq(Vec4f v, f32 e0, f32 e1, f32 e2, f32 e3)
{
    f32 lanes[4]; v.store(lanes);
    return bit_eq(lanes[0], e0) && bit_eq(lanes[1], e1)
        && bit_eq(lanes[2], e2) && bit_eq(lanes[3], e3);
}

[[nodiscard]] bool vec8_bit_eq(Vec8f v, const f32 (&expected)[8])
{
    f32 lanes[8]; v.store(lanes);
    for (int i = 0; i < 8; ++i)
    {
        if (!bit_eq(lanes[i], expected[i])) return false;
    }
    return true;
}
}  // namespace

// ===========================================================================
// Vec4f
// ===========================================================================

TEST_CASE("simd Vec4f construct/store roundtrip", "[simd][vec4f]")
{
    const Vec4f v(1.0F, 2.0F, 3.0F, 4.0F);
    REQUIRE(vec4_bit_eq(v, 1.0F, 2.0F, 3.0F, 4.0F));

    f32 buf[4];
    v.store(buf);
    REQUIRE(buf[0] == 1.0F);
    REQUIRE(buf[1] == 2.0F);
    REQUIRE(buf[2] == 3.0F);
    REQUIRE(buf[3] == 4.0F);

    REQUIRE(v.lane(0) == 1.0F);
    REQUIRE(v.lane(3) == 4.0F);
}

TEST_CASE("simd Vec4f broadcast / zero / one", "[simd][vec4f]")
{
    REQUIRE(vec4_bit_eq(Vec4f(7.0F),    7.0F, 7.0F, 7.0F, 7.0F));
    REQUIRE(vec4_bit_eq(Vec4f::zero(),  0.0F, 0.0F, 0.0F, 0.0F));
    REQUIRE(vec4_bit_eq(Vec4f::one(),   1.0F, 1.0F, 1.0F, 1.0F));
}

TEST_CASE("simd Vec4f load / load_aligned / store / store_aligned", "[simd][vec4f]")
{
    alignas(16) f32 src[4]    = { 1.0F, 2.0F, 3.0F, 4.0F };
    alignas(16) f32 dst[4]    = {};
    f32             dst_un[4] = {};

    const Vec4f a = Vec4f::load_aligned(src);
    const Vec4f b = Vec4f::load(src);
    REQUIRE(vec4_bit_eq(a, 1.0F, 2.0F, 3.0F, 4.0F));
    REQUIRE(vec4_bit_eq(b, 1.0F, 2.0F, 3.0F, 4.0F));

    a.store_aligned(dst);
    a.store(dst_un);
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(dst[i]    == src[i]);
        REQUIRE(dst_un[i] == src[i]);
    }
}

TEST_CASE("simd Vec4f arithmetic parity vs scalar", "[simd][vec4f][parity]")
{
    const Vec4f a(1.5F, -2.25F,  3.0F, 4.5F);
    const Vec4f b(0.5F,  1.0F,  -1.5F, 2.0F);

    REQUIRE(vec4_bit_eq(a + b,
                        1.5F + 0.5F, -2.25F + 1.0F,  3.0F - 1.5F, 4.5F + 2.0F));
    REQUIRE(vec4_bit_eq(a - b,
                        1.5F - 0.5F, -2.25F - 1.0F,  3.0F + 1.5F, 4.5F - 2.0F));
    REQUIRE(vec4_bit_eq(a * b,
                        1.5F * 0.5F, -2.25F * 1.0F,  3.0F * -1.5F, 4.5F * 2.0F));
    REQUIRE(vec4_bit_eq(a / b,
                        1.5F / 0.5F, -2.25F / 1.0F,  3.0F / -1.5F, 4.5F / 2.0F));
    REQUIRE(vec4_bit_eq(-a,
                        -1.5F, 2.25F, -3.0F, -4.5F));

    REQUIRE(vec4_bit_eq(a * 2.0F,
                        1.5F * 2.0F, -2.25F * 2.0F, 3.0F * 2.0F, 4.5F * 2.0F));
    REQUIRE(vec4_bit_eq(2.0F * a,
                        1.5F * 2.0F, -2.25F * 2.0F, 3.0F * 2.0F, 4.5F * 2.0F));
}

TEST_CASE("simd Vec4f mul_add uses two roundings (no hardware FMA)", "[simd][vec4f][determinism]")
{
    // mul_add(a,b,c) MUST equal (a*b)+c with two roundings — not std::fma.
    // Pick values where (a*b)+c diverges from std::fma(a,b,c).
    constexpr f32 a_lane = 1.0F + (1.0F / 8388608.0F);  // 1 + 2^-23
    constexpr f32 b_lane = 1.0F + (1.0F / 8388608.0F);
    constexpr f32 c_lane = -1.0F;

    const Vec4f a(a_lane);
    const Vec4f b(b_lane);
    const Vec4f c(c_lane);

    const Vec4f      sim_result = mul_add(a, b, c);
    const f32        ref_result = (a_lane * b_lane) + c_lane;

    f32 lanes[4]; sim_result.store(lanes);
    REQUIRE(bit_eq(lanes[0], ref_result));
    REQUIRE(bit_eq(lanes[1], ref_result));
    REQUIRE(bit_eq(lanes[2], ref_result));
    REQUIRE(bit_eq(lanes[3], ref_result));
}

TEST_CASE("simd Vec4f min / max / abs", "[simd][vec4f]")
{
    const Vec4f a( 1.0F, -2.0F,  3.0F, -4.0F);
    const Vec4f b(-5.0F,  6.0F, -7.0F,  8.0F);

    REQUIRE(vec4_bit_eq(min(a, b), -5.0F, -2.0F, -7.0F, -4.0F));
    REQUIRE(vec4_bit_eq(max(a, b),  1.0F,  6.0F,  3.0F,  8.0F));
    REQUIRE(vec4_bit_eq(abs(a),     1.0F,  2.0F,  3.0F,  4.0F));
    REQUIRE(vec4_bit_eq(clamp(a, Vec4f(-1.0F), Vec4f(1.0F)),
                        1.0F, -1.0F, 1.0F, -1.0F));
}

TEST_CASE("simd Vec4f sqrt is IEEE-correct (bit-exact)", "[simd][vec4f][determinism]")
{
    const Vec4f a(4.0F, 9.0F, 16.0F, 25.0F);
    REQUIRE(vec4_bit_eq(sqrt(a), 2.0F, 3.0F, 4.0F, 5.0F));

    // Non-perfect square: hardware sqrt must match std::sqrt bit-exactly.
    const Vec4f b(2.0F);
    f32 lanes[4]; sqrt(b).store(lanes);
    REQUIRE(bit_eq(lanes[0], std::sqrt(2.0F)));
}

TEST_CASE("simd Vec4f horizontal_sum / dot use canonical pairwise order",
          "[simd][vec4f][determinism]")
{
    const Vec4f v(1.0F, 2.0F, 3.0F, 4.0F);
    const f32   ref_sum = (1.0F + 2.0F) + (3.0F + 4.0F);
    REQUIRE(bit_eq(horizontal_sum(v), ref_sum));

    const Vec4f a(1.0F, 2.0F,  3.0F,  4.0F);
    const Vec4f b(5.0F, 6.0F,  7.0F,  8.0F);
    const f32   ref_dot = (1.0F * 5.0F + 2.0F * 6.0F) + (3.0F * 7.0F + 4.0F * 8.0F);
    REQUIRE(bit_eq(dot(a, b), ref_dot));
}

TEST_CASE("simd Vec4f comparisons + select", "[simd][vec4f]")
{
    const Vec4f a(1.0F, 5.0F, 3.0F, 7.0F);
    const Vec4f b(2.0F, 4.0F, 3.0F, 8.0F);
    const Vec4f t = Vec4f::one();
    const Vec4f f = Vec4f::zero();

    REQUIRE(vec4_bit_eq(select(cmp_lt(a, b), t, f), 1.0F, 0.0F, 0.0F, 1.0F));
    REQUIRE(vec4_bit_eq(select(cmp_le(a, b), t, f), 1.0F, 0.0F, 1.0F, 1.0F));
    REQUIRE(vec4_bit_eq(select(cmp_eq(a, b), t, f), 0.0F, 0.0F, 1.0F, 0.0F));
    REQUIRE(vec4_bit_eq(select(cmp_gt(a, b), t, f), 0.0F, 1.0F, 0.0F, 0.0F));
    REQUIRE(vec4_bit_eq(select(cmp_ge(a, b), t, f), 0.0F, 1.0F, 1.0F, 0.0F));
}

TEST_CASE("simd Vec4f alignment is 16 bytes", "[simd][vec4f]")
{
    STATIC_REQUIRE(alignof(Vec4f) == 16);
    STATIC_REQUIRE(sizeof(Vec4f) == 16);
}

// ===========================================================================
// Vec8f
// ===========================================================================

TEST_CASE("simd Vec8f construct/store roundtrip", "[simd][vec8f]")
{
    const Vec8f v(1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F);
    const f32   expected[8] = { 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F };
    REQUIRE(vec8_bit_eq(v, expected));

    REQUIRE(v.lane(0) == 1.0F);
    REQUIRE(v.lane(7) == 8.0F);
}

TEST_CASE("simd Vec8f construct from two Vec4f", "[simd][vec8f]")
{
    const Vec4f lo(1.0F, 2.0F, 3.0F, 4.0F);
    const Vec4f hi(5.0F, 6.0F, 7.0F, 8.0F);
    const Vec8f v(lo, hi);
    const f32   expected[8] = { 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F };
    REQUIRE(vec8_bit_eq(v, expected));
}

TEST_CASE("simd Vec8f arithmetic parity vs scalar", "[simd][vec8f][parity]")
{
    const Vec8f a(1.0F, -2.0F, 3.0F, -4.0F, 5.0F, -6.0F, 7.0F, -8.0F);
    const Vec8f b(0.5F,  1.5F, 2.5F,  3.5F, 4.5F,  5.5F, 6.5F,  7.5F);

    f32 ea[8], eb[8];
    a.store(ea); b.store(eb);

    f32 expected_add[8], expected_sub[8], expected_mul[8], expected_div[8];
    for (int i = 0; i < 8; ++i)
    {
        expected_add[i] = ea[i] + eb[i];
        expected_sub[i] = ea[i] - eb[i];
        expected_mul[i] = ea[i] * eb[i];
        expected_div[i] = ea[i] / eb[i];
    }

    REQUIRE(vec8_bit_eq(a + b, expected_add));
    REQUIRE(vec8_bit_eq(a - b, expected_sub));
    REQUIRE(vec8_bit_eq(a * b, expected_mul));
    REQUIRE(vec8_bit_eq(a / b, expected_div));
}

TEST_CASE("simd Vec8f horizontal_sum is canonical-tree", "[simd][vec8f][determinism]")
{
    const Vec8f v(1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F);
    const f32   s01 = 1.0F + 2.0F;
    const f32   s23 = 3.0F + 4.0F;
    const f32   s45 = 5.0F + 6.0F;
    const f32   s67 = 7.0F + 8.0F;
    const f32   ref = (s01 + s23) + (s45 + s67);
    REQUIRE(bit_eq(horizontal_sum(v), ref));
}

TEST_CASE("simd Vec8f sqrt", "[simd][vec8f]")
{
    const Vec8f a(1.0F, 4.0F, 9.0F, 16.0F, 25.0F, 36.0F, 49.0F, 64.0F);
    const f32   ref[8] = { 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F };
    REQUIRE(vec8_bit_eq(sqrt(a), ref));
}

TEST_CASE("simd Vec8f alignment is 32 bytes", "[simd][vec8f]")
{
    STATIC_REQUIRE(alignof(Vec8f) == 32);
}

// ===========================================================================
// Mat4f
// ===========================================================================

TEST_CASE("simd Mat4f identity behaves as identity under multiply", "[simd][mat4f]")
{
    const Mat4f I  = Mat4f::identity();
    const Mat4f M(Vec4f(1.0F, 2.0F, 3.0F, 4.0F),
                  Vec4f(5.0F, 6.0F, 7.0F, 8.0F),
                  Vec4f(9.0F, 10.F, 11.F, 12.F),
                  Vec4f(13.F, 14.F, 15.F, 16.F));

    const Mat4f IM = I * M;
    f32 lanes[4];
    for (int c = 0; c < 4; ++c)
    {
        IM.cols[c].store(lanes);
        f32 mlanes[4]; M.cols[c].store(mlanes);
        for (int r = 0; r < 4; ++r) REQUIRE(bit_eq(lanes[r], mlanes[r]));
    }
}

TEST_CASE("simd Mat4f matrix-vector multiply matches naive scalar", "[simd][mat4f][parity]")
{
    const Mat4f M(Vec4f(1.0F, 2.0F, 3.0F, 4.0F),
                  Vec4f(5.0F, 6.0F, 7.0F, 8.0F),
                  Vec4f(9.0F, 10.F, 11.F, 12.F),
                  Vec4f(13.F, 14.F, 15.F, 16.F));
    const Vec4f v(1.0F, 0.5F, -1.0F, 2.0F);

    // Reference: column-major Mv = sum_i (M.cols[i] * v[i]).
    // In our chain we do col0 + ((col1*v1) + ((col2*v2) + (col3*v3))) but
    // mul_add is left-associative; use the same call sequence as Mat4f:
    f32 v_lanes[4]; v.store(v_lanes);
    f32 c0[4], c1[4], c2[4], c3[4];
    M.cols[0].store(c0); M.cols[1].store(c1);
    M.cols[2].store(c2); M.cols[3].store(c3);

    f32 ref[4];
    for (int r = 0; r < 4; ++r)
    {
        // matches Mat4f::operator*(Vec4f): r = (c0*v0); r = c1*v1 + r; ...
        f32 acc = c0[r] * v_lanes[0];
        acc = c1[r] * v_lanes[1] + acc;
        acc = c2[r] * v_lanes[2] + acc;
        acc = c3[r] * v_lanes[3] + acc;
        ref[r] = acc;
    }

    const Vec4f result = M * v;
    REQUIRE(vec4_bit_eq(result, ref[0], ref[1], ref[2], ref[3]));
}

TEST_CASE("simd Mat4f transpose is involutive", "[simd][mat4f]")
{
    const Mat4f M(Vec4f(1.0F, 2.0F, 3.0F, 4.0F),
                  Vec4f(5.0F, 6.0F, 7.0F, 8.0F),
                  Vec4f(9.0F, 10.F, 11.F, 12.F),
                  Vec4f(13.F, 14.F, 15.F, 16.F));

    const Mat4f Mt  = transpose(M);
    const Mat4f Mtt = transpose(Mt);

    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            REQUIRE(bit_eq(Mtt.element(r, c), M.element(r, c)));
            REQUIRE(bit_eq(Mt.element(r, c),  M.element(c, r)));
        }
    }
}

TEST_CASE("simd Mat4f load/store column-major roundtrip", "[simd][mat4f]")
{
    f32 src[16];
    for (int i = 0; i < 16; ++i) src[i] = static_cast<f32>(i + 1);

    const Mat4f M = Mat4f::load_column_major(src);

    f32 dst[16] = {};
    M.store_column_major(dst);
    for (int i = 0; i < 16; ++i) REQUIRE(bit_eq(dst[i], src[i]));
}

TEST_CASE("simd Mat4f alignment is 16 bytes", "[simd][mat4f]")
{
    STATIC_REQUIRE(alignof(Mat4f) == 16);
}

// ===========================================================================
// Quatf
// ===========================================================================

TEST_CASE("simd Quatf identity is (0,0,0,1)", "[simd][quatf]")
{
    const Quatf q = Quatf::identity();
    REQUIRE(bit_eq(q.x(), 0.0F));
    REQUIRE(bit_eq(q.y(), 0.0F));
    REQUIRE(bit_eq(q.z(), 0.0F));
    REQUIRE(bit_eq(q.w(), 1.0F));
}

TEST_CASE("simd Quatf identity is multiplicative identity", "[simd][quatf]")
{
    const Quatf I = Quatf::identity();
    const Quatf q(0.1F, 0.2F, 0.3F, 0.927F);  // ~unit quaternion

    const Quatf left  = I * q;
    const Quatf right = q * I;

    REQUIRE(bit_eq(left.x(),  q.x()));
    REQUIRE(bit_eq(left.y(),  q.y()));
    REQUIRE(bit_eq(left.z(),  q.z()));
    REQUIRE(bit_eq(left.w(),  q.w()));
    REQUIRE(bit_eq(right.x(), q.x()));
    REQUIRE(bit_eq(right.y(), q.y()));
    REQUIRE(bit_eq(right.z(), q.z()));
    REQUIRE(bit_eq(right.w(), q.w()));
}

TEST_CASE("simd Quatf Hamilton product matches expected", "[simd][quatf][parity]")
{
    // 90° rotation about Z: q = (0, 0, sin(45°), cos(45°))
    const f32   inv_sqrt2 = 1.0F / std::sqrt(2.0F);
    const Quatf q90(0.0F, 0.0F, inv_sqrt2, inv_sqrt2);

    // q90 * q90 should be 180° about Z = (0, 0, 1, 0) within tolerance.
    const Quatf q180 = q90 * q90;
    REQUIRE(std::abs(q180.x() - 0.0F) < 1.0e-6F);
    REQUIRE(std::abs(q180.y() - 0.0F) < 1.0e-6F);
    REQUIRE(std::abs(q180.z() - 1.0F) < 1.0e-6F);
    REQUIRE(std::abs(q180.w() - 0.0F) < 1.0e-6F);
}

TEST_CASE("simd Quatf normalize unit quaternion is invariant (within ulps)",
          "[simd][quatf]")
{
    const Quatf q(0.1F, 0.2F, 0.3F, 0.4F);
    const Quatf qn = normalize(q);
    const f32   len_sq = qn.x() * qn.x() + qn.y() * qn.y()
                       + qn.z() * qn.z() + qn.w() * qn.w();
    REQUIRE(std::abs(len_sq - 1.0F) < 1.0e-6F);
}

TEST_CASE("simd Quatf rotate vector by 90 deg around Z", "[simd][quatf]")
{
    const f32   inv_sqrt2 = 1.0F / std::sqrt(2.0F);
    const Quatf q90(0.0F, 0.0F, inv_sqrt2, inv_sqrt2);

    // X-axis rotated 90° about Z should land near +Y.
    const Vec4f rotated = rotate(q90, 1.0F, 0.0F, 0.0F);
    f32 lanes[4]; rotated.store(lanes);
    REQUIRE(std::abs(lanes[0] - 0.0F) < 1.0e-6F);
    REQUIRE(std::abs(lanes[1] - 1.0F) < 1.0e-6F);
    REQUIRE(std::abs(lanes[2] - 0.0F) < 1.0e-6F);
}

TEST_CASE("simd Quatf alignment is 16 bytes", "[simd][quatf]")
{
    STATIC_REQUIRE(alignof(Quatf) == 16);
    STATIC_REQUIRE(sizeof(Quatf) == 16);
}

// ===========================================================================
// Backend introspection
// ===========================================================================

TEST_CASE("simd backend macros define exactly one path", "[simd][backend]")
{
    constexpr int picked = (CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_SCALAR ? 1 : 0)
                         + (CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_SSE2   ? 1 : 0)
                         + (CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2   ? 1 : 0)
                         + (CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_NEON   ? 1 : 0);
    STATIC_REQUIRE(picked == 1);
}

TEST_CASE("simd backend reports its compile-time identity", "[simd][backend][!mayfail]")
{
    // !mayfail tag: this is informational, not pass/fail. The INFO message
    // surfaces in CTest output (`ctest --output-on-failure -V`), giving us
    // a one-line confirmation per binary that the build picked the SIMD
    // level the configure summary advertised.
    INFO("crd::math::simd::backend_name()      = " << crd::math::simd::backend_name());
    INFO("crd::math::simd::deterministic_fp()  = " << (crd::math::simd::deterministic_fp() ? "true" : "false"));
    INFO("crd::math::simd::k_native_lane_width = " << crd::math::simd::k_native_lane_width);

    // Always succeeds; the INFO above carries the payload.
    REQUIRE(crd::math::simd::backend_name() != nullptr);
}

TEST_CASE("simd build is under ADR-0063 deterministic FP contract",
          "[simd][backend][determinism]")
{
    // Hard requirement for eylem + crd-hesap. If this regresses, the next
    // build of either module's replay-hash CI will start failing nondet
    // assertions. Catch the regression here instead.
    REQUIRE(crd::math::simd::deterministic_fp());
}
