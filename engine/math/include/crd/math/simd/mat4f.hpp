// Mat4f — 4x4 column-major matrix using SIMD Vec4f columns. Phase 3.1 v0a.
//
// Storage is four Vec4f columns: m_cols[0..3]. Column-major matches the
// existing crd::math::Mat4 + Vulkan/glTF default. Multiplication is the
// standard cache-friendly pattern: a * b where each output column is a
// linear combination of `a`'s columns weighted by `b`'s column lanes.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4f.hpp>

namespace crd::math::simd
{
struct alignas(16) Mat4f
{
    Vec4f cols[4];

    Mat4f() noexcept = default;

    CRD_FORCEINLINE Mat4f(Vec4f c0, Vec4f c1, Vec4f c2, Vec4f c3) noexcept
    {
        cols[0] = c0; cols[1] = c1; cols[2] = c2; cols[3] = c3;
    }

    [[nodiscard]] CRD_FORCEINLINE static Mat4f identity() noexcept
    {
        return Mat4f(Vec4f(1.0F, 0.0F, 0.0F, 0.0F),
                     Vec4f(0.0F, 1.0F, 0.0F, 0.0F),
                     Vec4f(0.0F, 0.0F, 1.0F, 0.0F),
                     Vec4f(0.0F, 0.0F, 0.0F, 1.0F));
    }

    [[nodiscard]] CRD_FORCEINLINE static Mat4f zero() noexcept
    {
        const Vec4f z = Vec4f::zero();
        return Mat4f(z, z, z, z);
    }

    [[nodiscard]] CRD_FORCEINLINE static Mat4f load_column_major(const f32* p) noexcept
    {
        return Mat4f(Vec4f::load(p),     Vec4f::load(p + 4),
                     Vec4f::load(p + 8), Vec4f::load(p + 12));
    }

    CRD_FORCEINLINE void store_column_major(f32* p) const noexcept
    {
        cols[0].store(p);
        cols[1].store(p + 4);
        cols[2].store(p + 8);
        cols[3].store(p + 12);
    }

    [[nodiscard]] CRD_FORCEINLINE f32 element(usize row, usize col) const noexcept
    {
        return cols[col].lane(row);
    }
};

// ---- arithmetic ------------------------------------------------------------

CRD_FORCEINLINE Mat4f operator+(const Mat4f& a, const Mat4f& b) noexcept
{
    return Mat4f(a.cols[0] + b.cols[0], a.cols[1] + b.cols[1],
                 a.cols[2] + b.cols[2], a.cols[3] + b.cols[3]);
}

CRD_FORCEINLINE Mat4f operator-(const Mat4f& a, const Mat4f& b) noexcept
{
    return Mat4f(a.cols[0] - b.cols[0], a.cols[1] - b.cols[1],
                 a.cols[2] - b.cols[2], a.cols[3] - b.cols[3]);
}

CRD_FORCEINLINE Mat4f operator*(const Mat4f& m, f32 s) noexcept
{
    return Mat4f(m.cols[0] * s, m.cols[1] * s, m.cols[2] * s, m.cols[3] * s);
}

CRD_FORCEINLINE Mat4f operator*(f32 s, const Mat4f& m) noexcept { return m * s; }

// Matrix-vector multiply: result = m * v (column vector convention).
//   result_col = sum over i of m.cols[i] * v_lane_i
// Computed as a linear combination of `m`'s columns scaled by `v`'s lanes.
CRD_FORCEINLINE Vec4f operator*(const Mat4f& m, Vec4f v) noexcept
{
    f32 lanes[4];
    v.store(lanes);
    Vec4f r = m.cols[0] * lanes[0];
    r = mul_add(m.cols[1], Vec4f(lanes[1]), r);
    r = mul_add(m.cols[2], Vec4f(lanes[2]), r);
    r = mul_add(m.cols[3], Vec4f(lanes[3]), r);
    return r;
}

// Matrix-matrix multiply: result = a * b (column-major).
//   result.cols[j] = a * b.cols[j]
CRD_FORCEINLINE Mat4f operator*(const Mat4f& a, const Mat4f& b) noexcept
{
    return Mat4f(a * b.cols[0], a * b.cols[1], a * b.cols[2], a * b.cols[3]);
}

// ---- transpose / transform helpers ----------------------------------------

CRD_FORCEINLINE Mat4f transpose(const Mat4f& m) noexcept
{
#if CRD_SIMD_HAS_SSE2
    // _MM_TRANSPOSE4_PS is the standard 4-shuffle SSE transpose macro.
    Mat4f r = m;
    _MM_TRANSPOSE4_PS(r.cols[0].v, r.cols[1].v, r.cols[2].v, r.cols[3].v);
    return r;
#elif CRD_SIMD_HAS_NEON
    const float32x4x2_t t01 = vtrnq_f32(m.cols[0].v, m.cols[1].v);
    const float32x4x2_t t23 = vtrnq_f32(m.cols[2].v, m.cols[3].v);
    Mat4f r;
    r.cols[0].v = vcombine_f32(vget_low_f32(t01.val[0]),  vget_low_f32(t23.val[0]));
    r.cols[1].v = vcombine_f32(vget_low_f32(t01.val[1]),  vget_low_f32(t23.val[1]));
    r.cols[2].v = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
    r.cols[3].v = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
    return r;
#else
    Mat4f r;
    for (usize i = 0; i < 4; ++i)
    {
        for (usize j = 0; j < 4; ++j)
        {
            r.cols[i].v[j] = m.cols[j].v[i];
        }
    }
    return r;
#endif
}

// Transform a 3D point: appends w=1, then projects back.
CRD_FORCEINLINE Vec4f transform_point(const Mat4f& m, f32 x, f32 y, f32 z) noexcept
{
    return m * Vec4f(x, y, z, 1.0F);
}

// Transform a 3D vector (no translation): w=0.
CRD_FORCEINLINE Vec4f transform_vector(const Mat4f& m, f32 x, f32 y, f32 z) noexcept
{
    return m * Vec4f(x, y, z, 0.0F);
}

}  // namespace crd::math::simd
