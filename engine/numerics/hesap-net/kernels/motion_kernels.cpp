// The replicated-motion kernels of crd/hesap/net/motion_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), so no function
// of this unit is shared with general code or with another level.
//
// One body serves every level, written over the level's lanes (crd/math/simd): sixteen floats or eight doubles on
// AVX-512, eight and four on AVX2, four and four on SSE2, NEON and the portable level. A short tail runs the same body on
// a padded block, so no scalar path can round differently. The build pins contraction off and no body uses a fused
// operation, so each product rounds before its sum, as the references' scalar expressions do.

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/net/motion_kernels.hpp>
#include <crd/math/simd/interleave4.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec4f.hpp>

#if CRD_SIMD_HAS_AVX2
#include <crd/math/simd/vec8f.hpp>
#endif
#if CRD_SIMD_HAS_AVX512
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec8d.hpp>
#endif

#include <cstring>

namespace crd::hesap::net::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::f32;
using crd::f64;
using crd::usize;

template <class T>
struct Lanes;

#if CRD_SIMD_HAS_AVX512
template <>
struct Lanes<f32>
{
    using V = crd::math::simd::Vec16f;
    static constexpr usize kCount = 16U;
};
template <>
struct Lanes<f64>
{
    using V = crd::math::simd::Vec8d;
    static constexpr usize kCount = 8U;
};
#elif CRD_SIMD_HAS_AVX2
template <>
struct Lanes<f32>
{
    using V = crd::math::simd::Vec8f;
    static constexpr usize kCount = 8U;
};
template <>
struct Lanes<f64>
{
    using V = crd::math::simd::Vec4d;
    static constexpr usize kCount = 4U;
};
#else
template <>
struct Lanes<f32>
{
    using V = crd::math::simd::Vec4f;
    static constexpr usize kCount = 4U;
};
template <>
struct Lanes<f64>
{
    using V = crd::math::simd::Vec4d;
    static constexpr usize kCount = 4U;
};
#endif

using F = Lanes<f32>::V;
constexpr usize kFloats = Lanes<f32>::kCount;

// out[i .. i + lanes] = body(the lanes of each input at i), whole blocks and then a padded one.
template <class T, usize Inputs, class Body>
CRD_FORCEINLINE void over_blocks(const T* const (&inputs)[Inputs], usize count, T* out, Body body) noexcept
{
    using V = typename Lanes<T>::V;
    constexpr usize lanes = Lanes<T>::kCount;
    V values[Inputs];
    usize i = 0U;
    for (; i + lanes <= count; i += lanes)
    {
        for (usize k = 0U; k < Inputs; ++k)
        {
            values[k] = V::load(inputs[k] + i);
        }
        body(values).store(out + i);
    }
    if (i < count)
    {
        for (usize k = 0U; k < Inputs; ++k)
        {
            T padded[lanes] = {};
            std::memcpy(padded, inputs[k] + i, (count - i) * sizeof(T));
            values[k] = V::load(padded);
        }
        T last[lanes];
        body(values).store(last);
        std::memcpy(out + i, last, (count - i) * sizeof(T));
    }
}

template <class T>
void lerp(const T* a, const T* b, T t, usize count, T* out) noexcept
{
    using V = typename Lanes<T>::V;
    const V amount(t);
    const T* const inputs[] = {a, b};
    over_blocks<T>(inputs, count, out, [amount](const V* v) noexcept { return v[0] + ((v[1] - v[0]) * amount); });
}

template <class T>
void hermite(const HermiteSpan<T>& span, usize count, T* out) noexcept
{
    using V = typename Lanes<T>::V;
    const T t = span.t;
    const T t2 = t * t;
    const T t3 = t2 * t;
    const T two = 2;
    const T three = 3;
    const V h1((two * t3) - (three * t2) + 1);
    const V h2((-two * t3) + (three * t2));
    const V h3(t3 - (two * t2) + t);
    const V h4(t3 - t2);
    const V width(span.span);
    const T* const inputs[] = {span.p0, span.v0, span.p1, span.v1};
    over_blocks<T>(inputs, count, out,
                   [=](const V* v) noexcept
                   { return (h1 * v[0]) + (h2 * v[2]) + (h3 * (v[1] * width)) + (h4 * (v[3] * width)); });
}

template <class T>
void extrapolate(const T* position, const T* velocity, const T* acceleration, T dt, usize count, T* out) noexcept
{
    using V = typename Lanes<T>::V;
    const V step(dt);
    if (acceleration == nullptr)
    {
        const T* const inputs[] = {position, velocity};
        over_blocks<T>(inputs, count, out, [step](const V* v) noexcept { return v[0] + (v[1] * step); });
        return;
    }
    const V half(static_cast<T>(0.5) * dt * dt);
    const T* const inputs[] = {position, velocity, acceleration};
    over_blocks<T>(inputs, count, out,
                   [step, half](const V* v) noexcept { return v[0] + (v[1] * step) + (v[2] * half); });
}

// A block of quaternions, one lane per quaternion.
struct Quaternions
{
    F x;
    F y;
    F z;
    F w;
};

CRD_FORCEINLINE Quaternions load_quaternions(const f32* xyzw) noexcept
{
    Quaternions q;
    crd::math::simd::load_interleaved4(xyzw, q.x, q.y, q.z, q.w);
    return q;
}

CRD_FORCEINLINE void store_quaternions(const Quaternions& q, f32* xyzw) noexcept
{
    crd::math::simd::store_interleaved4(xyzw, q.x, q.y, q.z, q.w);
}

CRD_FORCEINLINE F dot(const Quaternions& a, const Quaternions& b) noexcept
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
}

// The body over whole blocks and then a padded one of quaternions.
template <class Body>
CRD_FORCEINLINE void over_quaternions(const f32* a, const f32* b, usize count, f32* out, Body body) noexcept
{
    usize i = 0U;
    for (; i + kFloats <= count; i += kFloats)
    {
        store_quaternions(body(load_quaternions(a + (4U * i)), load_quaternions(b + (4U * i))), out + (4U * i));
    }
    if (i < count)
    {
        const usize bytes = (count - i) * 4U * sizeof(f32);
        f32 last_a[4U * kFloats] = {};
        f32 last_b[4U * kFloats] = {};
        f32 last[4U * kFloats];
        std::memcpy(last_a, a + (4U * i), bytes);
        std::memcpy(last_b, b + (4U * i), bytes);
        store_quaternions(body(load_quaternions(last_a), load_quaternions(last_b)), last);
        std::memcpy(out + (4U * i), last, bytes);
    }
}

constexpr f32 kLargest = 3.4028234663852886e38F;

void nlerp(const f32* a, const f32* b, f32 t, usize count, f32* out) noexcept
{
    const F amount(t);
    over_quaternions(a, b, count, out,
                     [amount](const Quaternions& qa, const Quaternions& qb) noexcept
                     {
                         const F same_side = cmp_ge(dot(qa, qb), F::zero());
                         const Quaternions chord{qa.x + ((select(same_side, qb.x, -qb.x) - qa.x) * amount),
                                                 qa.y + ((select(same_side, qb.y, -qb.y) - qa.y) * amount),
                                                 qa.z + ((select(same_side, qb.z, -qb.z) - qa.z) * amount),
                                                 qa.w + ((select(same_side, qb.w, -qb.w) - qa.w) * amount)};
                         const F length2 = dot(chord, chord);
                         const F inverse = F::one() / sqrt(length2);
                         const F some = select(cmp_gt(length2, F::zero()), cmp_le(length2, F(kLargest)), F::zero());
                         return Quaternions{select(some, chord.x * inverse, F::zero()),
                                            select(some, chord.y * inverse, F::zero()),
                                            select(some, chord.z * inverse, F::zero()),
                                            select(some, chord.w * inverse, F::one())};
                     });
}

// Eberly's series for sin(t * angle) / sin(angle) in x = cos(angle) - 1: factor i is (t^2 - i^2) / (i * (2 * i + 1)) * x,
// each split into u[i] * t^2 - v[i]. Up to a right angle sixteen factors, the last scaled by his error-balancing
// constant (1.902 for this order, the minimum of the largest error), stay within 2.7e-8, below a float's rounding.
constexpr usize kFactors = 16U;
constexpr f32 kBalance = 1.902F;

struct SeriesTerms
{
    f32 u[kFactors];
    f32 v[kFactors];
};

constexpr SeriesTerms make_series_terms() noexcept
{
    SeriesTerms terms{};
    for (usize i = 1U; i <= kFactors; ++i)
    {
        const auto index = static_cast<f32>(i);
        const f32 odd = (2.0F * index) + 1.0F;
        const f32 scale = (i == kFactors) ? kBalance : 1.0F;
        terms.u[i - 1U] = scale / (index * odd);
        terms.v[i - 1U] = (scale * index) / odd;
    }
    return terms;
}

constexpr SeriesTerms kSeries = make_series_terms();

// sin(t * angle) / sin(angle) from x = cos(angle) - 1.
struct ChebyshevRatio
{
    F terms[kFactors];
    F t;

    explicit ChebyshevRatio(f32 amount) noexcept : t(amount)
    {
        const f32 square = amount * amount;
        for (usize i = 0U; i < kFactors; ++i)
        {
            terms[i] = F((kSeries.u[i] * square) - kSeries.v[i]);
        }
    }

    [[nodiscard]] CRD_FORCEINLINE F operator()(F x) const noexcept
    {
        F product = F::one();
        for (usize i = kFactors; i > 0U; --i)
        {
            product = F::one() + ((terms[i - 1U] * x) * product);
        }
        return t * product;
    }
};

// Cut to [0, 1], NaN to 0: outside that the series no longer follows the arc.
[[nodiscard]] constexpr f32 cut_to_unit(f32 t) noexcept
{
    if (!(t >= 0.0F))
    {
        return 0.0F;
    }
    return (t <= 1.0F) ? t : 1.0F;
}

void slerp(const f32* a, const f32* b, f32 t, usize count, f32* out) noexcept
{
    const f32 amount = cut_to_unit(t);
    const ChebyshevRatio to_b(amount);
    const ChebyshevRatio to_a(1.0F - amount);
    over_quaternions(a, b, count, out,
                     [&to_a, &to_b](const Quaternions& qa, const Quaternions& qb) noexcept
                     {
                         const F cosine = dot(qa, qb);
                         const F same_side = cmp_ge(cosine, F::zero());
                         const F magnitude = select(same_side, cosine, -cosine);
                         const F x = select(cmp_le(magnitude, F::one()), magnitude, F::one()) - F::one();
                         const F from_a = to_a(x);
                         const F raw_b = to_b(x);
                         const F from_b = select(same_side, raw_b, -raw_b);
                         return Quaternions{(from_a * qa.x) + (from_b * qb.x), (from_a * qa.y) + (from_b * qb.y),
                                            (from_a * qa.z) + (from_b * qb.z), (from_a * qa.w) + (from_b * qb.w)};
                     });
}

} // namespace
} // namespace crd::hesap::net::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::net
{

const MotionKernels CRD_KERNEL_TABLE(kMotionKernels) = {
    &CRD_MATH_LEVEL_NS::lerp<crd::f32>,        &CRD_MATH_LEVEL_NS::lerp<crd::f64>,
    &CRD_MATH_LEVEL_NS::hermite<crd::f32>,     &CRD_MATH_LEVEL_NS::hermite<crd::f64>,
    &CRD_MATH_LEVEL_NS::extrapolate<crd::f32>, &CRD_MATH_LEVEL_NS::extrapolate<crd::f64>,
    &CRD_MATH_LEVEL_NS::nlerp,                 &CRD_MATH_LEVEL_NS::slerp};

} // namespace crd::hesap::net
