// Vec4f — 4-lane f32 SIMD wrapper. Phase 3.1 v0a.
//
// Lives in crd::math::simd to keep clear of crd::math::Vec4f (the 4D math
// vector). Storage is one 128-bit register on SSE2/AVX2/NEON, four-element
// array on the scalar fallback. All operators are bit-exact across backends
// per ADR-0063 (no hardware FMA, fixed pairwise reductions, hardware sqrt).

#pragma once

#include <crd/math/simd/backend.hpp>

#include <bit>
#include <cmath>

namespace crd::math::simd
{
struct alignas(16) Vec4f
{
#if CRD_SIMD_HAS_SSE2
    using Native = __m128;
#elif CRD_SIMD_HAS_NEON
    using Native = float32x4_t;
#else
    using Native = f32[4];
#endif

    Native v;

    Vec4f() noexcept = default;

    CRD_FORCEINLINE explicit Vec4f(f32 broadcast) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        v = _mm_set1_ps(broadcast);
#elif CRD_SIMD_HAS_NEON
        v = vdupq_n_f32(broadcast);
#else
        v[0] = broadcast; v[1] = broadcast; v[2] = broadcast; v[3] = broadcast;
#endif
    }

    CRD_FORCEINLINE Vec4f(f32 x, f32 y, f32 z, f32 w) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        // _mm_set_ps takes args in reverse order — high to low lane.
        v = _mm_set_ps(w, z, y, x);
#elif CRD_SIMD_HAS_NEON
        const f32 lanes[4] = { x, y, z, w };
        v = vld1q_f32(lanes);
#else
        v[0] = x; v[1] = y; v[2] = z; v[3] = w;
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec4f zero() noexcept { return Vec4f(0.0F); }
    [[nodiscard]] CRD_FORCEINLINE static Vec4f one()  noexcept { return Vec4f(1.0F); }

    [[nodiscard]] CRD_FORCEINLINE static Vec4f load(const f32* p) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        Vec4f r; r.v = _mm_loadu_ps(p); return r;
#elif CRD_SIMD_HAS_NEON
        Vec4f r; r.v = vld1q_f32(p); return r;
#else
        return Vec4f(p[0], p[1], p[2], p[3]);
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec4f load_aligned(const f32* p) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        Vec4f r; r.v = _mm_load_ps(p); return r;
#elif CRD_SIMD_HAS_NEON
        Vec4f r; r.v = vld1q_f32(p); return r;
#else
        return Vec4f(p[0], p[1], p[2], p[3]);
#endif
    }

    CRD_FORCEINLINE void store(f32* p) const noexcept
    {
#if CRD_SIMD_HAS_SSE2
        _mm_storeu_ps(p, v);
#elif CRD_SIMD_HAS_NEON
        vst1q_f32(p, v);
#else
        p[0] = v[0]; p[1] = v[1]; p[2] = v[2]; p[3] = v[3];
#endif
    }

    CRD_FORCEINLINE void store_aligned(f32* p) const noexcept
    {
#if CRD_SIMD_HAS_SSE2
        _mm_store_ps(p, v);
#elif CRD_SIMD_HAS_NEON
        vst1q_f32(p, v);
#else
        p[0] = v[0]; p[1] = v[1]; p[2] = v[2]; p[3] = v[3];
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE f32 lane(usize i) const noexcept
    {
        f32 tmp[4];
        store(tmp);
        return tmp[i];
    }
};

// ---- arithmetic ------------------------------------------------------------

CRD_FORCEINLINE Vec4f operator+(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_add_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vaddq_f32(a.v, b.v); return r;
#else
    return Vec4f(a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4f operator-(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_sub_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vsubq_f32(a.v, b.v); return r;
#else
    return Vec4f(a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4f operator*(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_mul_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vmulq_f32(a.v, b.v); return r;
#else
    return Vec4f(a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4f operator/(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_div_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vdivq_f32(a.v, b.v); return r;
#else
    return Vec4f(a.v[0] / b.v[0], a.v[1] / b.v[1], a.v[2] / b.v[2], a.v[3] / b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4f operator-(Vec4f a) noexcept { return Vec4f::zero() - a; }

CRD_FORCEINLINE Vec4f operator*(Vec4f a, f32 s) noexcept { return a * Vec4f(s); }
CRD_FORCEINLINE Vec4f operator*(f32 s, Vec4f a) noexcept { return Vec4f(s) * a; }
CRD_FORCEINLINE Vec4f operator/(Vec4f a, f32 s) noexcept { return a / Vec4f(s); }

// mul_add — (a*b) + c with two roundings on every backend (no hardware FMA,
// per the ADR-0063 determinism contract; backend parity is the priority).
CRD_FORCEINLINE Vec4f mul_add(Vec4f a, Vec4f b, Vec4f c) noexcept { return (a * b) + c; }
CRD_FORCEINLINE Vec4f mul_sub(Vec4f a, Vec4f b, Vec4f c) noexcept { return (a * b) - c; }

// Single-rounded IEEE 754 FMA (a*b + c with one rounding). Distinct from mul_add
// (two roundings, ADR-0063 default). Vec4f has no 256-bit hardware FMA path of
// its own — it is the half-decomposition of Vec8f::fma on the scalar / SSE2 /
// NEON backends (none of which expose a 256-bit float FMA). Per-lane std::fma is
// IEEE 754-2008 mandated to match the hardware FMA bit-for-bit, so Vec8f stays
// width-parity-exact. Do NOT rewrite as (a*b)+c — that is two-rounded and would
// break parity with Vec8f's AVX2 _mm256_fmadd_ps path.
CRD_FORCEINLINE Vec4f fma(Vec4f a, Vec4f b, Vec4f c) noexcept
{
    f32 af[4], bf[4], cf[4];
    a.store(af);
    b.store(bf);
    c.store(cf);
    return Vec4f(std::fma(af[0], bf[0], cf[0]), std::fma(af[1], bf[1], cf[1]),
                 std::fma(af[2], bf[2], cf[2]), std::fma(af[3], bf[3], cf[3]));
}

// ---- min / max / abs / sqrt -----------------------------------------------

CRD_FORCEINLINE Vec4f min(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_min_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vminq_f32(a.v, b.v); return r;
#else
    return Vec4f(a.v[0] < b.v[0] ? a.v[0] : b.v[0],
                 a.v[1] < b.v[1] ? a.v[1] : b.v[1],
                 a.v[2] < b.v[2] ? a.v[2] : b.v[2],
                 a.v[3] < b.v[3] ? a.v[3] : b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4f max(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_max_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vmaxq_f32(a.v, b.v); return r;
#else
    return Vec4f(a.v[0] > b.v[0] ? a.v[0] : b.v[0],
                 a.v[1] > b.v[1] ? a.v[1] : b.v[1],
                 a.v[2] > b.v[2] ? a.v[2] : b.v[2],
                 a.v[3] > b.v[3] ? a.v[3] : b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4f abs(Vec4f a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    // Mask off the sign bit (bit 31).
    const __m128 sign_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    Vec4f r; r.v = _mm_and_ps(a.v, sign_mask); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vabsq_f32(a.v); return r;
#else
    return Vec4f(std::fabs(a.v[0]), std::fabs(a.v[1]), std::fabs(a.v[2]), std::fabs(a.v[3]));
#endif
}

CRD_FORCEINLINE Vec4f clamp(Vec4f a, Vec4f lo, Vec4f hi) noexcept
{
    return min(max(a, lo), hi);
}

// IEEE-754 correctly rounded sqrt is bit-exact on every modern CPU; safe
// to use the hardware instruction directly.
CRD_FORCEINLINE Vec4f sqrt(Vec4f a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_sqrt_ps(a.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vsqrtq_f32(a.v); return r;
#else
    return Vec4f(std::sqrt(a.v[0]), std::sqrt(a.v[1]), std::sqrt(a.v[2]), std::sqrt(a.v[3]));
#endif
}

// ---- reductions (deterministic pairwise order) ----------------------------

// horizontal_sum uses (lane0+lane1) + (lane2+lane3) — fixed binary tree
// matched by every backend so SIMD and scalar agree bit-for-bit.
CRD_FORCEINLINE f32 horizontal_sum(Vec4f a) noexcept
{
    f32 lanes[4];
    a.store(lanes);
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
}

CRD_FORCEINLINE f32 dot(Vec4f a, Vec4f b) noexcept { return horizontal_sum(a * b); }

// ---- comparisons (mask-producing) -----------------------------------------
//
// Comparisons return a Vec4f where each lane is all-bits-set (NaN when
// reinterpreted as float, but used as a mask) for true and zero for false.
// Use select() to consume the mask without branching.

// Mask convention (matches SSE2 / AVX2 / NEON hardware comparisons):
//   true  → all-bits-set  (bit pattern 0xFFFFFFFF, value = NaN as float)
//   false → all-bits-zero (bit pattern 0x00000000, value = +0.0F)
// This lets the same mask work in `select()` AND in bitwise ops via
// `bitcast_to_int(mask)` (proper -1 / 0 in integer arithmetic).
namespace detail
{
inline constexpr f32 k_mask_true_f32 = std::bit_cast<f32>(crd::u32{0xFFFFFFFFU});
}  // namespace detail

CRD_FORCEINLINE Vec4f cmp_lt(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_cmplt_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vreinterpretq_f32_u32(vcltq_f32(a.v, b.v)); return r;
#else
    constexpr f32 t = detail::k_mask_true_f32;
    return Vec4f(a.v[0] < b.v[0] ? t : 0.0F,
                 a.v[1] < b.v[1] ? t : 0.0F,
                 a.v[2] < b.v[2] ? t : 0.0F,
                 a.v[3] < b.v[3] ? t : 0.0F);
#endif
}

CRD_FORCEINLINE Vec4f cmp_le(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_cmple_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vreinterpretq_f32_u32(vcleq_f32(a.v, b.v)); return r;
#else
    constexpr f32 t = detail::k_mask_true_f32;
    return Vec4f(a.v[0] <= b.v[0] ? t : 0.0F, a.v[1] <= b.v[1] ? t : 0.0F,
                 a.v[2] <= b.v[2] ? t : 0.0F, a.v[3] <= b.v[3] ? t : 0.0F);
#endif
}

CRD_FORCEINLINE Vec4f cmp_eq(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_cmpeq_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vreinterpretq_f32_u32(vceqq_f32(a.v, b.v)); return r;
#else
    constexpr f32 t = detail::k_mask_true_f32;
    return Vec4f(a.v[0] == b.v[0] ? t : 0.0F, a.v[1] == b.v[1] ? t : 0.0F,
                 a.v[2] == b.v[2] ? t : 0.0F, a.v[3] == b.v[3] ? t : 0.0F);
#endif
}

CRD_FORCEINLINE Vec4f cmp_gt(Vec4f a, Vec4f b) noexcept { return cmp_lt(b, a); }
CRD_FORCEINLINE Vec4f cmp_ge(Vec4f a, Vec4f b) noexcept { return cmp_le(b, a); }

CRD_FORCEINLINE Vec4f select(Vec4f mask, Vec4f true_v, Vec4f false_v) noexcept
{
#if CRD_SIMD_HAS_SSE2
    // (mask & true_v) | (~mask & false_v)
    Vec4f r;
    r.v = _mm_or_ps(_mm_and_ps(mask.v, true_v.v),
                    _mm_andnot_ps(mask.v, false_v.v));
    return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r;
    r.v = vbslq_f32(vreinterpretq_u32_f32(mask.v), true_v.v, false_v.v);
    return r;
#else
    f32 m[4]; mask.store(m);
    f32 t[4]; true_v.store(t);
    f32 fv[4]; false_v.store(fv);
    return Vec4f(m[0] != 0.0F ? t[0] : fv[0],
                 m[1] != 0.0F ? t[1] : fv[1],
                 m[2] != 0.0F ? t[2] : fv[2],
                 m[3] != 0.0F ? t[3] : fv[3]);
#endif
}

}  // namespace crd::math::simd
