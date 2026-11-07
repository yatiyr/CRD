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
#include <crd/math/libm.hpp>
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

#include <crd/math/transcendental.hpp>

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

// ---- Dead reckoning (crd/hesap/net/dead_reckoning.hpp) --------------------------------------------------------------

using D = Lanes<f64>::V;
constexpr usize kDoubles = Lanes<f64>::kCount;

// The entire functions of x = beta^2 behind IEEE 1278.1's R1 and R2, beta = |rate| * |dt|. Up to kSeriesEnd their
// closed forms cancel, so their alternating Taylor series answers instead, in lanes; past it each lane needs its own
// sincos. Each coefficient is the correctly rounded quotient of two exact doubles (22! is the largest factorial a
// double holds exactly).
constexpr usize kSeriesTerms = 10U;
constexpr f64 kSeriesEnd = 1.0;

struct AlternatingSeries
{
    f64 c[kSeriesTerms];
};

// c[k] = (numerator + 2 * k * step) / (factorial_of + 2 * k)!
constexpr AlternatingSeries make_alternating_series(usize factorial_of, f64 numerator, f64 step) noexcept
{
    AlternatingSeries series{};
    f64 factorial = 1.0;
    for (usize i = 2U; i <= factorial_of; ++i)
    {
        factorial *= static_cast<f64>(i);
    }
    for (usize k = 0U; k < kSeriesTerms; ++k)
    {
        series.c[k] = (numerator + (2.0 * static_cast<f64>(k) * step)) / factorial;
        const auto reached = static_cast<f64>(factorial_of + (2U * k));
        factorial *= (reached + 1.0) * (reached + 2.0);
    }
    return series;
}

constexpr AlternatingSeries kVelocityAxis = make_alternating_series(3U, 1.0, 0.0);      // (beta - sin) / beta^3
constexpr AlternatingSeries kSinc = make_alternating_series(1U, 1.0, 0.0);              // sin / beta
constexpr AlternatingSeries kVersine = make_alternating_series(2U, 1.0, 0.0);           // (1 - cos) / beta^2
constexpr AlternatingSeries kAccelerationAxis = make_alternating_series(4U, 3.0, 1.0);  // (1/2 - plane) / beta^2
constexpr AlternatingSeries kAccelerationPlane = make_alternating_series(2U, 1.0, 1.0); // (cos + beta sin - 1) / beta^2
constexpr AlternatingSeries kAccelerationCross = make_alternating_series(3U, 2.0, 1.0); // (sin - beta cos) / beta^3

[[nodiscard]] CRD_FORCEINLINE D alternating(const AlternatingSeries& series, D x) noexcept
{
    D value(series.c[kSeriesTerms - 1U]);
    for (usize k = kSeriesTerms - 1U; k > 0U; --k)
    {
        value = D(series.c[k - 1U]) - (x * value);
    }
    return value;
}

// What each algorithm does, by its three letters.
[[nodiscard]] constexpr bool rotates(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Rpw || a == DeadReckoningAlgorithm::Rvw || a == DeadReckoningAlgorithm::Rpb ||
           a == DeadReckoningAlgorithm::Rvb;
}

[[nodiscard]] constexpr bool accelerates(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Rvw || a == DeadReckoningAlgorithm::Fvw || a == DeadReckoningAlgorithm::Rvb ||
           a == DeadReckoningAlgorithm::Fvb;
}

[[nodiscard]] constexpr bool body_axes(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Fpb || a == DeadReckoningAlgorithm::Rpb || a == DeadReckoningAlgorithm::Rvb ||
           a == DeadReckoningAlgorithm::Fvb;
}

struct Vectors
{
    D x;
    D y;
    D z;
};

CRD_FORCEINLINE Vectors load_vectors(const f64* const (&column)[3], usize i) noexcept
{
    return {D::load(column[0] + i), D::load(column[1] + i), D::load(column[2] + i)};
}

CRD_FORCEINLINE void store_vectors(const Vectors& v, f64* const (&column)[3], usize i) noexcept
{
    v.x.store(column[0] + i);
    v.y.store(column[1] + i);
    v.z.store(column[2] + i);
}

CRD_FORCEINLINE Vectors operator+(const Vectors& a, const Vectors& b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

CRD_FORCEINLINE Vectors operator*(const Vectors& a, D k) noexcept
{
    return {a.x * k, a.y * k, a.z * k};
}

CRD_FORCEINLINE D dot(const Vectors& a, const Vectors& b) noexcept
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

CRD_FORCEINLINE Vectors cross(const Vectors& a, const Vectors& b) noexcept
{
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

// One block's turn, per lane: the rate, with a rate too large for its square to be finite taken as none; the half-angle
// quaternion's parts; and the coefficients of R1 and R2.
struct Integrals
{
    D v_axis;
    D v_plane;
    D v_cross;
    D a_axis;
    D a_plane;
    D a_cross;
};

struct Turn
{
    Vectors rate;
    D half_sine;   // sin(beta / 2) / |rate|, with dt's sign
    D half_cosine; // cos(beta / 2)
    Integrals integrals;
};

template <bool WithIntegrals> CRD_FORCEINLINE Turn turn_of(const Vectors& given_rate, D t) noexcept
{
    const D given_x = dot(given_rate, given_rate) * (t * t);
    const D usable = cmp_le(given_x, D(1.7976931348623157e308));
    const D x = select(usable, given_x, D::zero());
    Turn turn{};
    turn.rate = {select(usable, given_rate.x, D::zero()), select(usable, given_rate.y, D::zero()),
                 select(usable, given_rate.z, D::zero())};
    const D quarter = x * D(0.25);
    turn.half_sine = t * D(0.5) * alternating(kSinc, quarter);
    turn.half_cosine = D::one() - (quarter * alternating(kVersine, quarter));
    Integrals parts{};
    if constexpr (WithIntegrals)
    {
        parts = {alternating(kVelocityAxis, x),      alternating(kSinc, x),
                 alternating(kVersine, x),           alternating(kAccelerationAxis, x),
                 alternating(kAccelerationPlane, x), alternating(kAccelerationCross, x)};
    }

    f64 squares[kDoubles];
    x.store(squares);
    bool past_series = false;
    for (const f64 square : squares)
    {
        past_series = past_series || square > kSeriesEnd;
    }
    if (past_series)
    {
        const D beta = sqrt(x);
        const D in_series = cmp_le(x, D(kSeriesEnd));
        f64 betas[kDoubles];
        beta.store(betas);
        f64 half_sines[kDoubles] = {};
        f64 half_cosines[kDoubles] = {};
        for (usize lane = 0U; lane < kDoubles; ++lane)
        {
            if (squares[lane] > kSeriesEnd)
            {
                crd::math::sincos(betas[lane] * 0.5, half_sines[lane], half_cosines[lane]);
            }
        }
        const D half_sin = D::load(half_sines);
        const D half_cos = D::load(half_cosines);
        turn.half_sine = select(in_series, turn.half_sine, (t * half_sin) / beta);
        turn.half_cosine = select(in_series, turn.half_cosine, half_cos);
        if constexpr (WithIntegrals)
        {
            // sin and 1 - cos of beta from its half angle, where 1 - cos does not cancel.
            const D sine = D(2.0) * half_sin * half_cos;
            const D versed = D(2.0) * half_sin * half_sin;
            const D plane = ((beta * sine) - versed) / x;
            const D cube = beta * x;
            parts.v_axis = select(in_series, parts.v_axis, (beta - sine) / cube);
            parts.v_plane = select(in_series, parts.v_plane, sine / beta);
            parts.v_cross = select(in_series, parts.v_cross, versed / x);
            parts.a_axis = select(in_series, parts.a_axis, (D(0.5) - plane) / x);
            parts.a_plane = select(in_series, parts.a_plane, plane);
            parts.a_cross = select(in_series, parts.a_cross, (sine - (beta * (D::one() - versed))) / cube);
        }
    }
    if constexpr (WithIntegrals)
    {
        const D t2 = t * t;
        const D t3 = t2 * t;
        turn.integrals = {t3 * parts.v_axis,        t * parts.v_plane,  t2 * parts.v_cross,
                          (t2 * t2) * parts.a_axis, t2 * parts.a_plane, t3 * parts.a_cross};
    }
    return turn;
}

template <DeadReckoningAlgorithm Algorithm>
CRD_FORCEINLINE void reckon_block(const DeadReckoningInput& in, usize i, const DeadReckoningOutput& out) noexcept
{
    constexpr bool turns = rotates(Algorithm);
    constexpr bool speeds = accelerates(Algorithm);
    constexpr bool on_body = body_axes(Algorithm);

    const D dt = D::load(in.dt + i);
    const Vectors position = load_vectors(in.position, i);
    const Vectors velocity = load_vectors(in.velocity, i);
    Vectors acceleration{D::zero(), D::zero(), D::zero()};
    if constexpr (speeds)
    {
        acceleration = load_vectors(in.acceleration, i);
    }
    Vectors q{D::zero(), D::zero(), D::zero()};
    D qw = D::zero();
    if constexpr (on_body || turns)
    {
        q = load_vectors({in.orientation[0], in.orientation[1], in.orientation[2]}, i);
        qw = D::load(in.orientation[3] + i);
    }

    Vectors moved{D::zero(), D::zero(), D::zero()};
    if constexpr (turns)
    {
        const Turn turn = turn_of<on_body>(load_vectors(in.rate, i), dt);
        const Vectors& rate = turn.rate;
        const Vectors half_turn = rate * turn.half_sine;
        const D half_cosine = turn.half_cosine;
        const Vectors turned = (half_turn * qw) + (q * half_cosine) + cross(q, half_turn);
        turned.x.store(out.orientation[0] + i);
        turned.y.store(out.orientation[1] + i);
        turned.z.store(out.orientation[2] + i);
        ((qw * half_cosine) - dot(q, half_turn)).store(out.orientation[3] + i);
        if constexpr (on_body)
        {
            const Integrals& r = turn.integrals;
            moved = (rate * (dot(rate, velocity) * r.v_axis)) + (velocity * r.v_plane) +
                    (cross(rate, velocity) * r.v_cross);
            if constexpr (speeds)
            {
                moved = moved + (rate * (dot(rate, acceleration) * r.a_axis)) + (acceleration * r.a_plane) +
                        (cross(rate, acceleration) * r.a_cross);
            }
        }
    }
    if constexpr (on_body)
    {
        if constexpr (!turns)
        {
            moved = velocity * dt;
            if constexpr (speeds)
            {
                moved = moved + (acceleration * (D(0.5) * dt * dt));
            }
        }
        const Vectors twice = cross(q, moved) * D(2.0);
        store_vectors(position + (moved + (twice * qw) + cross(q, twice)), out.position, i);
    }
    else if constexpr (speeds)
    {
        store_vectors(position + (velocity * dt) + (acceleration * (D(0.5) * dt * dt)), out.position, i);
    }
    else
    {
        store_vectors(position + (velocity * dt), out.position, i);
    }
    if constexpr (speeds)
    {
        store_vectors(velocity + (acceleration * dt), out.velocity, i);
    }
}

template <DeadReckoningAlgorithm Algorithm>
void reckon_all(const DeadReckoningInput& in, usize count, const DeadReckoningOutput& out) noexcept
{
    usize i = 0U;
    for (; i + kDoubles <= count; i += kDoubles)
    {
        reckon_block<Algorithm>(in, i, out);
    }
    if (i == count)
    {
        return;
    }
    // A short tail runs the same body on whole lanes: a used column's values, then zeros.
    const usize bytes = (count - i) * sizeof(f64);
    DeadReckoningInput tail = in;
    f64 padded[3U + 3U + 3U + 3U + 4U + 1U][kDoubles] = {};
    usize next = 0U;
    const auto pad = [&](const f64*& column) noexcept
    {
        if (column != nullptr)
        {
            std::memcpy(padded[next], column + i, bytes);
            column = padded[next];
            ++next;
        }
    };
    for (usize k = 0U; k < 3U; ++k)
    {
        pad(tail.position[k]);
        pad(tail.velocity[k]);
        pad(tail.acceleration[k]);
        pad(tail.rate[k]);
    }
    for (const f64*& column : tail.orientation)
    {
        pad(column);
    }
    pad(tail.dt);

    f64 last[3U + 3U + 4U][kDoubles];
    const DeadReckoningOutput into{
        {last[0], last[1], last[2]}, {last[3], last[4], last[5]}, {last[6], last[7], last[8], last[9]}};
    reckon_block<Algorithm>(tail, 0U, into);
    for (usize k = 0U; k < 3U; ++k)
    {
        std::memcpy(out.position[k] + i, last[k], bytes);
        if constexpr (accelerates(Algorithm))
        {
            std::memcpy(out.velocity[k] + i, last[3U + k], bytes);
        }
    }
    if constexpr (rotates(Algorithm))
    {
        for (usize k = 0U; k < 4U; ++k)
        {
            std::memcpy(out.orientation[k] + i, last[6U + k], bytes);
        }
    }
}

void dead_reckon(DeadReckoningAlgorithm algorithm, const DeadReckoningInput& in, usize count,
                 const DeadReckoningOutput& out) noexcept
{
    switch (algorithm)
    {
        case DeadReckoningAlgorithm::Static:
            for (usize k = 0U; k < 3U && count != 0U; ++k)
            {
                std::memmove(out.position[k], in.position[k], count * sizeof(f64));
            }
            return;
        case DeadReckoningAlgorithm::Fpw:
            reckon_all<DeadReckoningAlgorithm::Fpw>(in, count, out);
            return;
        case DeadReckoningAlgorithm::Rpw:
            reckon_all<DeadReckoningAlgorithm::Rpw>(in, count, out);
            return;
        case DeadReckoningAlgorithm::Rvw:
            reckon_all<DeadReckoningAlgorithm::Rvw>(in, count, out);
            return;
        case DeadReckoningAlgorithm::Fvw:
            reckon_all<DeadReckoningAlgorithm::Fvw>(in, count, out);
            return;
        case DeadReckoningAlgorithm::Fpb:
            reckon_all<DeadReckoningAlgorithm::Fpb>(in, count, out);
            return;
        case DeadReckoningAlgorithm::Rpb:
            reckon_all<DeadReckoningAlgorithm::Rpb>(in, count, out);
            return;
        case DeadReckoningAlgorithm::Rvb:
            reckon_all<DeadReckoningAlgorithm::Rvb>(in, count, out);
            return;
        case DeadReckoningAlgorithm::Fvb:
            reckon_all<DeadReckoningAlgorithm::Fvb>(in, count, out);
            return;
    }
}

template <class T>
CRD_FORCEINLINE void blend_block(const ProjectiveBlendSpan<T>& span, const T* acceleration, usize i, T* position_out,
                                 T* velocity_out) noexcept
{
    using V = typename Lanes<T>::V;
    const V from_position = V::load(span.from_position + i);
    const V from_velocity = V::load(span.from_velocity + i);
    const V position = V::load(span.position + i);
    const V velocity = V::load(span.velocity + i);
    const V rate = (acceleration != nullptr) ? V::load(acceleration + i) : V::zero();
    const V elapsed = V::load(span.elapsed + i);
    const V width = V::load(span.span + i);

    // elapsed / span cut to [0, 1], and 1 when there is no span to blend across.
    const V ratio = elapsed / width;
    const V floored = select(cmp_ge(ratio, V::zero()), ratio, V::zero());
    const V capped = select(cmp_le(floored, V::one()), floored, V::one());
    const V fraction = select(cmp_gt(width, V::zero()), capped, V::one());

    const V change = velocity - from_velocity;
    const V blended = from_velocity + (change * fraction);
    const V accelerated = V(static_cast<T>(0.5)) * rate * elapsed * elapsed;
    const V projected = from_position + (blended * elapsed) + accelerated;
    const V difference = (position + (velocity * elapsed) + accelerated) - projected;
    (projected + (difference * fraction)).store(position_out + i);

    // The path's own derivative while it blends; at the end the path is the target's alone.
    const V gained = rate * elapsed;
    const V target_rate = velocity + gained;
    const V projected_rate = blended + (change * fraction) + gained;
    const V blending_rate = projected_rate + ((target_rate - projected_rate) * fraction) + (difference / width);
    select(cmp_lt(fraction, V::one()), blending_rate, target_rate).store(velocity_out + i);
}

template <class T>
void projective_blend(const ProjectiveBlendSpan<T>& span, usize count, T* position, T* velocity) noexcept
{
    constexpr usize lanes = Lanes<T>::kCount;
    usize i = 0U;
    for (; i + lanes <= count; i += lanes)
    {
        blend_block<T>(span, span.acceleration, i, position, velocity);
    }
    if (i == count)
    {
        return;
    }
    const usize bytes = (count - i) * sizeof(T);
    constexpr usize input_count = 7U;
    T padded[input_count][lanes] = {};
    const T* const columns[input_count] = {span.from_position, span.from_velocity, span.position, span.velocity,
                                           span.acceleration,  span.elapsed,       span.span};
    for (usize k = 0U; k < input_count; ++k)
    {
        if (columns[k] != nullptr)
        {
            std::memcpy(padded[k], columns[k] + i, bytes);
        }
    }
    const ProjectiveBlendSpan<T> tail{padded[0], padded[1], padded[2], padded[3], padded[4], padded[5], padded[6]};
    T last_position[lanes];
    T last_velocity[lanes];
    blend_block<T>(tail, (span.acceleration != nullptr) ? tail.acceleration : nullptr, 0U, last_position,
                   last_velocity);
    std::memcpy(position + i, last_position, bytes);
    std::memcpy(velocity + i, last_velocity, bytes);
}

} // namespace
} // namespace crd::hesap::net::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::net
{

const MotionKernels CRD_KERNEL_TABLE(kMotionKernels) = {&CRD_MATH_LEVEL_NS::lerp<crd::f32>,
                                                        &CRD_MATH_LEVEL_NS::lerp<crd::f64>,
                                                        &CRD_MATH_LEVEL_NS::hermite<crd::f32>,
                                                        &CRD_MATH_LEVEL_NS::hermite<crd::f64>,
                                                        &CRD_MATH_LEVEL_NS::extrapolate<crd::f32>,
                                                        &CRD_MATH_LEVEL_NS::extrapolate<crd::f64>,
                                                        &CRD_MATH_LEVEL_NS::nlerp,
                                                        &CRD_MATH_LEVEL_NS::slerp,
                                                        &CRD_MATH_LEVEL_NS::dead_reckon,
                                                        &CRD_MATH_LEVEL_NS::projective_blend<crd::f32>,
                                                        &CRD_MATH_LEVEL_NS::projective_blend<crd::f64>};

} // namespace crd::hesap::net
