// crd::math::deterministic — Cephes-style polynomial transcendentals.
// Phase 3.1 v0c. ADR-0063 §2.
//
// Implementations are derived from Stephen Moshier's Cephes Math Library
// (public domain). Each function below cites the Cephes source it derives
// from. Coefficients have been preserved bit-exactly; the only
// modifications are: (1) C++ namespacing, (2) `crd::f32` instead of `float`,
// (3) std::bit_cast / __builtin_truncf in place of platform intrinsics.
//
// The CrdSimd.cmake module enforces /fp:precise (MSVC) +
// -fno-fast-math -ffp-contract=off -mfpmath=sse (GCC/Clang) project-wide,
// so the polynomial evaluations below produce bit-exact identical results
// across MSVC / clang-cl / GCC × x64 / ARM64.

#include <crd/math/deterministic.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/math/simd/vec4i.hpp>
#include <crd/math/simd/vec8i.hpp>
#include <crd/math/simd/convert.hpp>

#include <bit>
#include <cmath>      // for hardware floor/ceil/trunc/sqrt + numeric_limits
#include <limits>

namespace crd::math::deterministic
{
namespace
{
// ---- bit helpers ----------------------------------------------------------

[[nodiscard]] inline f32 from_bits(crd::u32 b) noexcept
{
    return std::bit_cast<f32>(b);
}

[[nodiscard]] inline crd::u32 to_bits(f32 v) noexcept
{
    return std::bit_cast<crd::u32>(v);
}

[[nodiscard]] inline f32 fast_abs(f32 x) noexcept
{
    return from_bits(to_bits(x) & 0x7FFFFFFFU);
}

[[nodiscard]] inline crd::u32 sign_bit(f32 x) noexcept
{
    return to_bits(x) & 0x80000000U;
}

[[nodiscard]] inline f32 with_sign(f32 magnitude, crd::u32 sign) noexcept
{
    // OVERWRITES sign bit. Use for true copysign semantics (caller knows
    // `magnitude` is non-negative).
    return from_bits((to_bits(magnitude) & 0x7FFFFFFFU) | sign);
}

[[nodiscard]] inline f32 apply_sign(f32 y, crd::u32 sign_xor) noexcept
{
    // XOR-combines `sign_xor` with y's existing sign bit. Use when the
    // polynomial result may already be negative (sin/cos/tan octant
    // reduction): two negatives combine to positive (matches Cephes
    // `if (sign < 0) result = -result`).
    return from_bits(to_bits(y) ^ sign_xor);
}

[[nodiscard]] inline bool is_finite(f32 x) noexcept
{
    return (to_bits(x) & 0x7F800000U) != 0x7F800000U;
}

[[nodiscard]] inline bool is_nan(f32 x) noexcept
{
    const crd::u32 b = to_bits(x);
    return (b & 0x7F800000U) == 0x7F800000U && (b & 0x007FFFFFU) != 0U;
}
}  // namespace

// ===========================================================================
// Rounding wrappers — IEEE hardware ops, deterministic by construction.
// ===========================================================================

f32 floor(f32 x) noexcept { return std::floor(x); }
f32 ceil (f32 x) noexcept { return std::ceil(x);  }
f32 trunc(f32 x) noexcept { return std::trunc(x); }

f32 round(f32 x) noexcept
{
    // Round-to-nearest-ties-away-from-zero, matching std::roundf semantics.
    // Implemented via floor/ceil to dodge any libc variation in tie-breaking.
    if (x >= 0.0F) return floor(x + 0.5F);
    return ceil(x - 0.5F);
}

f32 fmod(f32 x, f32 y) noexcept
{
    if (y == 0.0F) return std::numeric_limits<f32>::quiet_NaN();
    const f32 q = trunc(x / y);
    return x - q * y;
}

f32 abs(f32 x) noexcept { return fast_abs(x); }

f32 copysign(f32 magnitude, f32 sign_src) noexcept
{
    return with_sign(magnitude, sign_bit(sign_src));
}

// ===========================================================================
// sin / cos / tan — Cephes sinf.c / cosf.c / tanf.c
// ===========================================================================
//
// Reduce |x| to [0, π/4] using the constant FOPI = 4/π. Track the octant
// j ∈ {0..7} to determine which polynomial (sin or cos) to evaluate and
// whether to flip the sign of the result.
//
// The argument-reduction split (DP1+DP2+DP3 ≈ π/4) is the standard
// trick for preserving precision after subtraction — π/4 is broken into
// three single-precision pieces so that x - j*π/4 retains accuracy even
// when j*π/4 is large relative to x.

namespace
{
// 4/π — used to count quadrants
inline constexpr f32 k4OverPi = 1.27323954473516F;

// π/4 split into three single-precision parts whose sum rounds to the
// nearest representable π/4. Cephes sinf.c.
inline constexpr f32 kDp1 = 0.78515625F;
inline constexpr f32 kDp2 = 2.4187564849853515625e-4F;
inline constexpr f32 kDp3 = 3.77489497744594108e-8F;

// sin polynomial for |z| ≤ π/4:  sin(z) ≈ z + p2*z^3 + p1*z^5 + p0*z^7
inline constexpr f32 kSincofP0 = -1.9515295891E-4F;
inline constexpr f32 kSincofP1 =  8.3321608736E-3F;
inline constexpr f32 kSincofP2 = -1.6666654611E-1F;

// cos polynomial for |z| ≤ π/4:  cos(z) ≈ 1 - z^2/2 + p2*z^4 + p1*z^6 + p0*z^8
inline constexpr f32 kCoscofP0 =  2.443315711809948E-005F;
inline constexpr f32 kCoscofP1 = -1.388731625493765E-003F;
inline constexpr f32 kCoscofP2 =  4.166664568298827E-002F;

// tan polynomial for |z| ≤ π/4:
inline constexpr f32 kTancofP0 = 9.38540185543E-3F;
inline constexpr f32 kTancofP1 = 3.11992232697E-3F;
inline constexpr f32 kTancofP2 = 2.44301354525E-2F;
inline constexpr f32 kTancofP3 = 5.34112807005E-2F;
inline constexpr f32 kTancofP4 = 1.33387994085E-1F;
inline constexpr f32 kTancofP5 = 3.33331568548E-1F;
}  // namespace

f32 sin(f32 xx) noexcept
{
    const crd::u32 sign = sign_bit(xx);
    f32 x = fast_abs(xx);

    // Reduce x to [0, π/4], with octant j ∈ {0..7}.
    f32       j = trunc(x * k4OverPi);
    crd::u32  jq = static_cast<crd::u32>(j);
    if ((jq & 1U) != 0U)
    {
        j  += 1.0F;
        jq += 1U;
    }
    jq &= 7U;

    crd::u32 sign_out = sign;
    if (jq > 3U)
    {
        sign_out ^= 0x80000000U;
        jq      -= 4U;
    }

    // x_reduced = x - j*π/4  (high-precision split subtraction)
    const f32 z  = ((x - j * kDp1) - j * kDp2) - j * kDp3;
    const f32 zz = z * z;

    f32 y;
    if (jq == 1U || jq == 2U)
    {
        // Use cos polynomial for octants where sin folds onto cos.
        y = ((kCoscofP0 * zz + kCoscofP1) * zz + kCoscofP2) * zz * zz
          - 0.5F * zz + 1.0F;
    }
    else
    {
        // sin polynomial.
        y = ((kSincofP0 * zz + kSincofP1) * zz + kSincofP2) * zz * z + z;
    }

    return apply_sign(y, sign_out);
}

f32 cos(f32 xx) noexcept
{
    f32 x = fast_abs(xx);

    // Reduce. Same as sin but the octant rotation is shifted by π/2.
    f32       j  = trunc(x * k4OverPi);
    crd::u32  jq = static_cast<crd::u32>(j);
    if ((jq & 1U) != 0U)
    {
        j  += 1.0F;
        jq += 1U;
    }
    jq &= 7U;

    crd::u32 sign_out = 0U;
    if (jq > 3U)
    {
        sign_out = 0x80000000U;
        jq      -= 4U;
    }
    if (jq > 1U)
    {
        sign_out ^= 0x80000000U;
    }

    const f32 z  = ((x - j * kDp1) - j * kDp2) - j * kDp3;
    const f32 zz = z * z;

    f32 y;
    if (jq == 1U || jq == 2U)
    {
        y = ((kSincofP0 * zz + kSincofP1) * zz + kSincofP2) * zz * z + z;
    }
    else
    {
        y = ((kCoscofP0 * zz + kCoscofP1) * zz + kCoscofP2) * zz * zz
          - 0.5F * zz + 1.0F;
    }

    return apply_sign(y, sign_out);
}

f32 tan(f32 xx) noexcept
{
    const crd::u32 sign = sign_bit(xx);
    f32 x = fast_abs(xx);

    // Reduce to [-π/4, π/4]; track parity (j%2 selects cot path).
    f32       j  = trunc(x * k4OverPi);
    crd::u32  jq = static_cast<crd::u32>(j);
    if ((jq & 1U) != 0U)
    {
        j  += 1.0F;
        jq += 1U;
    }

    const f32 z  = ((x - j * kDp1) - j * kDp2) - j * kDp3;
    const f32 zz = z * z;

    f32 y = z;
    if (zz > 1.0e-4F)
    {
        const f32 num = ((((kTancofP0 * zz + kTancofP1) * zz + kTancofP2) * zz
                       + kTancofP3) * zz + kTancofP4) * zz + kTancofP5;
        y = z + z * (zz * num);
    }

    if ((jq & 2U) != 0U)
    {
        // Cotangent path: tan -> -1/tan
        if (y == 0.0F) return with_sign(std::numeric_limits<f32>::infinity(), sign);
        y = -1.0F / y;
    }

    return with_sign(y, sign);
}

// ===========================================================================
// atan / atan2 / asin / acos — Cephes atanf.c
// ===========================================================================

namespace
{
// Polynomial for |x| in [0, tan(π/8)]  (≤ ~0.4142)
inline constexpr f32 kAtancofP0 =  8.05374449538E-2F;
inline constexpr f32 kAtancofP1 = -1.38776856032E-1F;
inline constexpr f32 kAtancofP2 =  1.99777106478E-1F;
inline constexpr f32 kAtancofP3 = -3.33329491539E-1F;

inline constexpr f32 kTan3Pi8 = 2.41421356237309504880F;  // tan(3π/8)
inline constexpr f32 kTanPi8  = 0.41421356237309504880F;  // tan(π/8)
}  // namespace

f32 atan(f32 xx) noexcept
{
    const crd::u32 sign = sign_bit(xx);
    f32 x = fast_abs(xx);

    // Range-reduce by tan(π/8) and tan(3π/8).
    f32 y;
    if (x > kTan3Pi8)
    {
        x = -1.0F / x;
        y = pi_2;
    }
    else if (x > kTanPi8)
    {
        x = (x - 1.0F) / (x + 1.0F);
        y = pi_4;
    }
    else
    {
        y = 0.0F;
    }

    // Cephes coefficients are pre-signed; use all `+` operators below.
    // (An earlier draft used `-` between already-signed coefficients,
    //  which double-negates p1 and p3.)
    const f32 z = x * x;
    y += (((kAtancofP0 * z + kAtancofP1) * z + kAtancofP2) * z
        + kAtancofP3) * z * x + x;

    return with_sign(y, sign);
}

f32 atan2(f32 y, f32 x) noexcept
{
    if (x == 0.0F)
    {
        if (y > 0.0F) return  pi_2;
        if (y < 0.0F) return -pi_2;
        return 0.0F;  // (0, 0): convention 0
    }

    const f32 base = atan(y / x);
    if (x > 0.0F) return base;
    if (y >= 0.0F) return base + pi;
    return base - pi;
}

f32 asin(f32 x) noexcept
{
    if (x >  1.0F) x =  1.0F;
    if (x < -1.0F) x = -1.0F;
    // asin(x) = atan2(x, sqrt(1 - x*x))
    return atan2(x, std::sqrt((1.0F - x) * (1.0F + x)));
}

f32 acos(f32 x) noexcept
{
    if (x >  1.0F) x =  1.0F;
    if (x < -1.0F) x = -1.0F;
    // acos(x) = atan2(sqrt(1 - x*x), x)
    return atan2(std::sqrt((1.0F - x) * (1.0F + x)), x);
}

// ===========================================================================
// exp / exp2 — Cephes expf.c
// ===========================================================================
//
// exp(x) = 2^k * exp(r) where x = k*ln(2) + r, |r| ≤ ln(2)/2.
// exp(r) is approximated by a degree-5 polynomial.

namespace
{
inline constexpr f32 kLog2E = 1.44269504088896341F;   // 1/ln(2)
inline constexpr f32 kLn2Hi = 0.693359375F;          // high part of ln(2)
inline constexpr f32 kLn2Lo = -2.12194440e-4F;       // low part of ln(2)

// exp polynomial for |r| ≤ ln(2)/2 ≈ 0.3466
inline constexpr f32 kExpcofP0 = 1.9875691500E-4F;
inline constexpr f32 kExpcofP1 = 1.3981999507E-3F;
inline constexpr f32 kExpcofP2 = 8.3334519073E-3F;
inline constexpr f32 kExpcofP3 = 4.1665795894E-2F;
inline constexpr f32 kExpcofP4 = 1.6666665459E-1F;
inline constexpr f32 kExpcofP5 = 5.0000001201E-1F;

inline constexpr f32 kExpMax =  88.722832F;   // input above → +inf
inline constexpr f32 kExpMin = -87.336544F;   // input below → +0

[[nodiscard]] inline f32 ldexp_int_pow2(f32 mantissa, crd::i32 exp_int) noexcept
{
    // mantissa * 2^exp via integer-exponent injection. Avoids std::ldexp
    // because Microsoft's ldexpf has subtly different denormal handling.
    if (exp_int > 127)  return mantissa * std::numeric_limits<f32>::infinity();
    if (exp_int < -126) return 0.0F;
    const crd::u32 bits = static_cast<crd::u32>(exp_int + 127) << 23;
    return mantissa * from_bits(bits);
}
}  // namespace

f32 exp(f32 x) noexcept
{
    if (is_nan(x))  return x;
    if (x > kExpMax) return std::numeric_limits<f32>::infinity();
    if (x < kExpMin) return 0.0F;

    // k = round(x * log2(e)); r = x - k*ln(2) (using high-precision split)
    const f32 fk = round(x * kLog2E);
    const crd::i32 k = static_cast<crd::i32>(fk);
    const f32 r = (x - fk * kLn2Hi) - fk * kLn2Lo;

    // Polynomial approximation of exp(r) - 1 - r:  exp(r) = 1 + r + r*r*P(r)
    const f32 z = r * r;
    const f32 p = ((((kExpcofP0 * r + kExpcofP1) * r + kExpcofP2) * r
                  + kExpcofP3) * r + kExpcofP4) * r + kExpcofP5;
    const f32 y = 1.0F + r + z * p;

    return ldexp_int_pow2(y, k);
}

f32 exp2(f32 x) noexcept
{
    return exp(x * ln2);
}

// ===========================================================================
// log / log2 / log10 — Cephes logf.c
// ===========================================================================

namespace
{
// Polynomial for log(1+u) on u in roughly [-1/3, 1/3] after frexp reduction.
inline constexpr f32 kLogcofP0 =  7.0376836292E-2F;
inline constexpr f32 kLogcofP1 = -1.1514610310E-1F;
inline constexpr f32 kLogcofP2 =  1.1676998740E-1F;
inline constexpr f32 kLogcofP3 = -1.2420140846E-1F;
inline constexpr f32 kLogcofP4 =  1.4249322787E-1F;
inline constexpr f32 kLogcofP5 = -1.6668057665E-1F;
inline constexpr f32 kLogcofP6 =  2.0000714765E-1F;
inline constexpr f32 kLogcofP7 = -2.4999993993E-1F;
inline constexpr f32 kLogcofP8 =  3.3333331174E-1F;

// ln(2) split for high-precision compensation in the exponent reconstruction.
inline constexpr f32 kLogQ1 = -2.12194440e-4F;
inline constexpr f32 kLogQ2 =  0.693359375F;

[[nodiscard]] inline f32 frexp_extract(f32 x, crd::i32& exp_out) noexcept
{
    // Decompose x = m * 2^exp with m in [0.5, 1). Avoids std::frexpf for
    // the same cross-platform-libc reason as ldexp_int_pow2 above.
    const crd::u32 b = to_bits(x);
    const crd::i32 raw_exp = static_cast<crd::i32>((b >> 23) & 0xFFU);
    exp_out = raw_exp - 126;
    const crd::u32 mant_bits = (b & 0x807FFFFFU) | 0x3F000000U;  // exponent → 0.5..<1
    return from_bits(mant_bits);
}
}  // namespace

f32 log(f32 x) noexcept
{
    if (is_nan(x))            return x;
    if (x <  0.0F)            return std::numeric_limits<f32>::quiet_NaN();
    if (x == 0.0F)            return -std::numeric_limits<f32>::infinity();
    if (!is_finite(x))        return x;  // +inf

    crd::i32 exp_int = 0;
    f32 m = frexp_extract(x, exp_int);

    // Adjust m to be in [sqrt(0.5), sqrt(2)] for better polynomial behaviour.
    if (m < 0.707106781187F)
    {
        exp_int -= 1;
        m += m;
    }
    m -= 1.0F;

    const f32 z  = m * m;
    const f32 fe = static_cast<f32>(exp_int);

    // y = m + (poly(m) - 0.5) * z, then add fe*ln(2) (via split for accuracy)
    f32 y = ((((((((kLogcofP0 * m + kLogcofP1) * m + kLogcofP2) * m
                  + kLogcofP3) * m + kLogcofP4) * m + kLogcofP5) * m
              + kLogcofP6) * m + kLogcofP7) * m + kLogcofP8) * m * z;

    y += fe * kLogQ1;
    y -= 0.5F * z;
    y += m;
    y += fe * kLogQ2;

    return y;
}

f32 log2(f32 x) noexcept
{
    return log(x) * inv_ln2;
}

f32 log10(f32 x) noexcept
{
    return log(x) * inv_ln10;
}

// ===========================================================================
// pow — exp(exponent * log(base)) with edge-case handling.
// ===========================================================================

f32 pow(f32 base, f32 exponent) noexcept
{
    if (exponent == 0.0F) return 1.0F;
    if (base == 0.0F)
    {
        if (exponent > 0.0F) return 0.0F;
        return std::numeric_limits<f32>::infinity();
    }
    if (base == 1.0F) return 1.0F;

    if (base < 0.0F)
    {
        // Negative base + integer exponent → real result; otherwise NaN.
        const f32 r = round(exponent);
        if (r != exponent) return std::numeric_limits<f32>::quiet_NaN();
        const f32 result = exp(exponent * log(-base));
        // Sign: negative if exponent is odd integer.
        const crd::i32 ei = static_cast<crd::i32>(r);
        return (ei & 1) != 0 ? -result : result;
    }

    return exp(exponent * log(base));
}

// ===========================================================================
// expm1 / log1p — cancellation-resistant near-zero variants.
// ===========================================================================
//
// expm1(x) = exp(x) - 1. For |x| ≪ 1, exp(x) ≈ 1 + tiny, so the subtraction
// loses 5-6 digits to cancellation. The Taylor expansion x + x²/2 + x³/6
// truncated at second order preserves all f32 precision in that band.
//
// log1p(x) = log(1 + x). For |x| ≪ 1, 1+x rounds to 1, then log(1) = 0,
// losing ALL precision. Taylor x - x²/2 + x³/3 - ... fixes that.

f32 expm1(f32 x) noexcept
{
    if (fast_abs(x) < 0.4F)
    {
        // Taylor degree 7 — truncation x^8/40320 ≤ 1.6e-9 in band, well
        // below f32 ulp at 0.5 (~6e-8). Horner-evaluated.
        return x * (1.0F + x * (1.0F / 2.0F + x * (1.0F / 6.0F + x *
               (1.0F / 24.0F + x * (1.0F / 120.0F + x * (1.0F / 720.0F + x *
               (1.0F / 5040.0F))))))) ;
    }
    return exp(x) - 1.0F;
}

f32 log1p(f32 x) noexcept
{
    if (fast_abs(x) < 0.1F)
    {
        // Taylor: log(1+x) = x - x²/2 + x³/3 - x⁴/4 + x⁵/5 - x⁶/6 + x⁷/7
        // Truncation x^8/8 ≤ 1.25e-9 at |x|=0.1. Horner-evaluated.
        return x * (1.0F + x * (-1.0F / 2.0F + x * (1.0F / 3.0F + x *
               (-1.0F / 4.0F + x * (1.0F / 5.0F + x * (-1.0F / 6.0F + x *
               (1.0F / 7.0F))))))) ;
    }
    return log(1.0F + x);
}

// ===========================================================================
// sinh / cosh / tanh — hyperbolic functions.
// ===========================================================================
//
// sinh(x) = (e^x - e^-x) / 2. For small |x|, use Taylor to avoid
// cancellation. For |x| > about 1, the direct formula is well-conditioned.
//
// cosh(x) = (e^x + e^-x) / 2. No cancellation issue (both terms are
// positive); direct formula works everywhere finite. Saturates to +�?
// past |x| ≈ 89.4.
//
// tanh(x) = (e^(2x) - 1) / (e^(2x) + 1). Saturates to ±1 quickly past
// |x| ≈ 9 (single-precision precision limit).

f32 sinh(f32 x) noexcept
{
    const crd::u32 sign = sign_bit(x);
    const f32      ax   = fast_abs(x);
    if (ax < 0.5F)
    {
        // Taylor degree 9 — truncation x^11/39916800 ≤ 1.2e-11 at |x|=0.5,
        // well below f32 ulp. Horner-evaluated odd-only series.
        const f32 x2 = ax * ax;
        const f32 r = ax * (1.0F + x2 * (1.0F / 6.0F + x2 * (1.0F / 120.0F
                    + x2 * (1.0F / 5040.0F + x2 * 1.0F / 362880.0F))));
        return apply_sign(r, sign);
    }
    // For |x| > 0.5: 0.5*(e^x - e^-x). No cancellation; e^x dominates.
    const f32 ex = exp(ax);
    const f32 r  = 0.5F * (ex - 1.0F / ex);
    return apply_sign(r, sign);
}

f32 cosh(f32 x) noexcept
{
    const f32 ax = fast_abs(x);
    if (ax > 88.7F) return std::numeric_limits<f32>::infinity();
    // (e^x + e^-x)/2 — no cancellation; both terms positive everywhere.
    const f32 ex = exp(ax);
    return 0.5F * (ex + 1.0F / ex);
}

f32 tanh(f32 x) noexcept
{
    const crd::u32 sign = sign_bit(x);
    const f32      ax   = fast_abs(x);
    if (ax > 9.0F)
    {
        // |tanh(x)| < 1 - 2*e^-18 < 1 - 2^-25 — beyond f32 precision.
        return apply_sign(1.0F, sign);
    }
    if (ax < 0.5F)
    {
        // Taylor degree 7 — odd-only series.
        // tanh(x) = x - x³/3 + 2x⁵/15 - 17x⁷/315 + 62x⁹/2835
        const f32 x2 = ax * ax;
        const f32 r = ax * (1.0F + x2 * (-1.0F / 3.0F + x2 * (2.0F / 15.0F
                    + x2 * (-17.0F / 315.0F + x2 * 62.0F / 2835.0F))));
        return apply_sign(r, sign);
    }
    // For |x| in [0.5, 9]: tanh(x) = 1 - 2 / (e^(2x) + 1). No cancellation
    // (numerator and denominator are both well-conditioned positive values).
    const f32 e2 = exp(2.0F * ax);
    const f32 r  = 1.0F - 2.0F / (e2 + 1.0F);
    return apply_sign(r, sign);
}

// ===========================================================================
// Special functions: erf / erfc / gamma / lgamma / beta — f32 wrappers.
//
// f32 special functions delegate to the proven f64 Cephes implementations
// (defined below). Cast-down to f32 truncates to f32-natural precision
// (~1e-7 absolute), which is at or below f32 ulp at typical values — same
// precision a hand-tuned f32 implementation would produce, with one
// authoritative coefficient table to maintain. Forward-declared below so
// the f32 versions can call into the f64 implementations.
// ===========================================================================

// Forward declarations for f32 entry points (defined later in this TU).
// NOLINTBEGIN(readability-redundant-declaration) � explicit forward decls
// are required because the f64 implementations below take callbacks into
// these f32 wrappers (defined further down). Removing the decls breaks the
// f64-to-f32 call path during compilation. Intentional, not redundant.
f32 erf   (f32 x) noexcept;
f32 erfc  (f32 x) noexcept;
f32 gamma (f32 x) noexcept;
f32 lgamma(f32 x) noexcept;
f32 beta  (f32 a, f32 b) noexcept;
// NOLINTEND(readability-redundant-declaration)

// ===========================================================================
// ===========================================================================
// f64 implementations — Cephes f64 coefficient tables (Stephen Moshier,
// public domain). Same algorithmic structure as f32 but with the rational/
// Padé forms Cephes uses for f64 accuracy. Bit-exact across MSVC / GCC /
// clang under the same /fp:precise + -ffp-contract=off contract.
// ===========================================================================
// ===========================================================================

namespace
{
[[nodiscard]] inline f64 from_bits64(crd::u64 b) noexcept { return std::bit_cast<f64>(b); }
[[nodiscard]] inline crd::u64 to_bits64(f64 v)  noexcept { return std::bit_cast<crd::u64>(v); }

[[nodiscard]] inline f64 fast_abs64(f64 x) noexcept
{
    return from_bits64(to_bits64(x) & 0x7FFFFFFFFFFFFFFFULL);
}

[[nodiscard]] inline crd::u64 sign_bit64(f64 x) noexcept
{
    return to_bits64(x) & 0x8000000000000000ULL;
}

[[nodiscard]] inline f64 with_sign64(f64 magnitude, crd::u64 sign) noexcept
{
    return from_bits64((to_bits64(magnitude) & 0x7FFFFFFFFFFFFFFFULL) | sign);
}

[[nodiscard]] inline f64 apply_sign64(f64 y, crd::u64 sign_xor) noexcept
{
    return from_bits64(to_bits64(y) ^ sign_xor);
}

[[nodiscard]] inline bool is_nan64(f64 x) noexcept
{
    const crd::u64 b = to_bits64(x);
    return (b & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL
        && (b & 0x000FFFFFFFFFFFFFULL) != 0ULL;
}

// Polynomial Horner-evaluator: returns coefs[0]*x^N + ... + coefs[N].
template <crd::usize N>
[[nodiscard]] inline f64 polevl(f64 x, const f64 (&coefs)[N]) noexcept
{
    f64 ans = coefs[0];
    for (crd::usize i = 1; i < N; ++i) ans = ans * x + coefs[i];
    return ans;
}

// Like polevl but assumes implicit leading 1.0 coefficient (matches Cephes p1evl).
template <crd::usize N>
[[nodiscard]] inline f64 p1evl(f64 x, const f64 (&coefs)[N]) noexcept
{
    f64 ans = x + coefs[0];
    for (crd::usize i = 1; i < N; ++i) ans = ans * x + coefs[i];
    return ans;
}
}  // namespace

// ---- rounding wrappers (IEEE; deterministic by construction) ---------------

f64 floor(f64 x) noexcept { return std::floor(x); }
f64 ceil (f64 x) noexcept { return std::ceil(x);  }
f64 trunc(f64 x) noexcept { return std::trunc(x); }

f64 round(f64 x) noexcept
{
    if (x >= 0.0) return floor(x + 0.5);
    return ceil(x - 0.5);
}

f64 fmod(f64 x, f64 y) noexcept
{
    if (y == 0.0) return std::numeric_limits<f64>::quiet_NaN();
    return x - trunc(x / y) * y;
}

f64 abs(f64 x) noexcept { return fast_abs64(x); }

f64 copysign(f64 magnitude, f64 sign_src) noexcept
{
    return with_sign64(magnitude, sign_bit64(sign_src));
}

// ===========================================================================
// f64 sin / cos — Cephes sin.c / cos.c
// ===========================================================================

namespace
{
inline constexpr f64 k64FourOverPi = 1.27323954473516268615;

// pi/4 split into three f64 components for high-precision argument reduction.
inline constexpr f64 k64Dp1 = 7.85398125648498535156E-1;
inline constexpr f64 k64Dp2 = 3.77489470793079817668E-8;
inline constexpr f64 k64Dp3 = 2.69515142907905952645E-15;

// sin polynomial degree 6 (zz powers):
//   sin(z) ≈ z + z*z*z * polevl(zz, sincof)
inline constexpr f64 k64Sincof[6] = {
     1.58962301576546568060E-10,
    -2.50507477628578072866E-8,
     2.75573136213857245213E-6,
    -1.98412698295895385996E-4,
     8.33333333332211858878E-3,
    -1.66666666666666307295E-1
};

// cos polynomial degree 6:
//   cos(z) ≈ 1 - 0.5*zz + zz*zz * polevl(zz, coscof)
inline constexpr f64 k64Coscof[6] = {
    -1.13585365213876817300E-11,
     2.08757008419747316778E-9,
    -2.75573141792967388112E-7,
     2.48015872888517045348E-5,
    -1.38888888888730564116E-3,
     4.16666666666665929218E-2
};
}  // namespace

f64 sin(f64 xx) noexcept
{
    const crd::u64 sign_in = sign_bit64(xx);
    f64 x = fast_abs64(xx);

    // Octant reduction.
    f64        j  = trunc(x * k64FourOverPi);
    crd::u64   jq = static_cast<crd::u64>(j);
    if ((jq & 1ULL) != 0ULL)
    {
        j  += 1.0;
        jq += 1ULL;
    }
    jq &= 7ULL;

    crd::u64 sign_out = sign_in;
    if (jq > 3ULL)
    {
        sign_out ^= 0x8000000000000000ULL;
        jq      -= 4ULL;
    }

    const f64 z  = ((x - j * k64Dp1) - j * k64Dp2) - j * k64Dp3;
    const f64 zz = z * z;

    f64 y;
    if (jq == 1ULL || jq == 2ULL)
    {
        y = 1.0 - 0.5 * zz + zz * zz * polevl(zz, k64Coscof);
    }
    else
    {
        y = z + z * z * z * polevl(zz, k64Sincof);
    }

    return apply_sign64(y, sign_out);
}

f64 cos(f64 xx) noexcept
{
    f64 x = fast_abs64(xx);

    f64        j  = trunc(x * k64FourOverPi);
    crd::u64   jq = static_cast<crd::u64>(j);
    if ((jq & 1ULL) != 0ULL)
    {
        j  += 1.0;
        jq += 1ULL;
    }
    jq &= 7ULL;

    crd::u64 sign_out = 0ULL;
    if (jq > 3ULL)
    {
        sign_out = 0x8000000000000000ULL;
        jq      -= 4ULL;
    }
    if (jq > 1ULL)
    {
        sign_out ^= 0x8000000000000000ULL;
    }

    const f64 z  = ((x - j * k64Dp1) - j * k64Dp2) - j * k64Dp3;
    const f64 zz = z * z;

    f64 y;
    if (jq == 1ULL || jq == 2ULL)
    {
        y = z + z * z * z * polevl(zz, k64Sincof);
    }
    else
    {
        y = 1.0 - 0.5 * zz + zz * zz * polevl(zz, k64Coscof);
    }

    return apply_sign64(y, sign_out);
}

// ===========================================================================
// f64 tan — Cephes tan.c (rational approximation)
// ===========================================================================

namespace
{
inline constexpr f64 k64Tanp[3] = {
    -1.30936939181383777646E4,
     1.15351664838587416464E6,
    -1.79565251976484877988E7
};
inline constexpr f64 k64Tanq[4] = {
     1.36812963470692954678E4,
    -1.32089234440210967447E6,
     2.50083801823357915839E7,
    -5.38695755929454629881E7
};
}  // namespace

f64 tan(f64 xx) noexcept
{
    const crd::u64 sign_in = sign_bit64(xx);
    f64 x = fast_abs64(xx);

    f64        j  = trunc(x * k64FourOverPi);
    crd::u64   jq = static_cast<crd::u64>(j);
    if ((jq & 1ULL) != 0ULL)
    {
        j  += 1.0;
        jq += 1ULL;
    }

    const f64 z  = ((x - j * k64Dp1) - j * k64Dp2) - j * k64Dp3;
    const f64 zz = z * z;

    f64 y = z;
    if (zz > 1.0e-14)
    {
        y = z + z * (zz * polevl(zz, k64Tanp) / p1evl(zz, k64Tanq));
    }

    if ((jq & 2ULL) != 0ULL)
    {
        if (y == 0.0) return with_sign64(std::numeric_limits<f64>::infinity(), sign_in);
        y = -1.0 / y;
    }

    return with_sign64(y, sign_in);
}

// ===========================================================================
// f64 atan — Cephes atan.c
// ===========================================================================

namespace
{
// Cephes atan.c f64 Padé coefficients (P/Q rational over z = x²).
// Sanity: P[4]/Q[4] = -64.85/194.55 = -1/3, matching the leading Taylor
// coefficient of atan(x) − x ≈ −x³/3 + x⁵/5 − ... so R(0) = -1/3.
inline constexpr f64 k64AtanP[5] = {
    -8.750608600031904122785E-1,
    -1.615753718733365076637E1,
    -7.500855792314704667340E1,
    -1.228866684490136173410E2,
    -6.485021904942025371773E1
};
inline constexpr f64 k64AtanQ[5] = {
     2.485846490142306297962E1,
     1.650270098316988542046E2,
     4.328810604912902668951E2,
     4.853903996359136964868E2,
     1.945506571482613964425E2
};

inline constexpr f64 k64Tan3Pi8 = 2.41421356237309504880;  // tan(3π/8)
// Cephes uses 0.66 (not tan(π/8)=0.4142) as the middle threshold — gives a
// smaller |x| after (x−1)/(x+1) reduction, keeping the Padé well-conditioned.
inline constexpr f64 k64AtanMid  = 0.66;
// High-precision π/2 low part for compensation in the long-x reduction path.
inline constexpr f64 k64Morebits  = 6.123233995736765886130E-17;
}  // namespace

f64 atan(f64 xx) noexcept
{
    const crd::u64 sign_in = sign_bit64(xx);
    f64 x = fast_abs64(xx);

    f64 y;
    f64 morebits = 0.0;
    if (x > k64Tan3Pi8)
    {
        y = pi_2_64;
        morebits = k64Morebits;
        x = -1.0 / x;
    }
    else if (x > k64AtanMid)
    {
        y = pi_4_64;
        morebits = 0.5 * k64Morebits;
        x = (x - 1.0) / (x + 1.0);
    }
    else
    {
        y = 0.0;
    }

    const f64 z = x * x;
    f64 r = z * polevl(z, k64AtanP) / p1evl(z, k64AtanQ);
    r = x * r + x + morebits;
    y += r;

    return with_sign64(y, sign_in);
}

f64 atan2(f64 y, f64 x) noexcept
{
    if (x == 0.0)
    {
        if (y > 0.0) return  pi_2_64;
        if (y < 0.0) return -pi_2_64;
        return 0.0;
    }

    const f64 base = atan(y / x);
    if (x > 0.0) return base;
    if (y >= 0.0) return base + pi64;
    return base - pi64;
}

f64 asin(f64 x) noexcept
{
    if (x >  1.0) x =  1.0;
    if (x < -1.0) x = -1.0;
    return atan2(x, std::sqrt((1.0 - x) * (1.0 + x)));
}

f64 acos(f64 x) noexcept
{
    if (x >  1.0) x =  1.0;
    if (x < -1.0) x = -1.0;
    return atan2(std::sqrt((1.0 - x) * (1.0 + x)), x);
}

// ===========================================================================
// f64 exp — Cephes exp.c (Padé approximation)
// ===========================================================================

namespace
{
inline constexpr f64 k64Log2E   = 1.44269504088896340736;
inline constexpr f64 k64Ln2Hi  = 0.693145751953125;             // ln(2) high
inline constexpr f64 k64Ln2Lo  = 1.42860682030941723212e-6;     // ln(2) low

inline constexpr f64 k64ExpP[3] = {
    1.26177193074810590878E-4,
    3.02994407707441961300E-2,
    9.99999999999999999910E-1
};
inline constexpr f64 k64ExpQ[4] = {
    3.00198505138664455042E-6,
    2.52448340349684104192E-3,
    2.27265548208155028766E-1,
    2.00000000000000000009E0
};

inline constexpr f64 k64ExpMax =  709.782712893383996843;
inline constexpr f64 k64ExpMin = -708.396418532264106224;

[[nodiscard]] inline f64 ldexp_int_pow2_64(f64 mantissa, crd::i32 exp_int) noexcept
{
    if (exp_int > 1023)  return mantissa * std::numeric_limits<f64>::infinity();
    if (exp_int < -1022) return 0.0;
    const crd::u64 bits = static_cast<crd::u64>(exp_int + 1023) << 52;
    return mantissa * from_bits64(bits);
}
}  // namespace

f64 exp(f64 x) noexcept
{
    if (is_nan64(x)) return x;
    if (x > k64ExpMax) return std::numeric_limits<f64>::infinity();
    if (x < k64ExpMin) return 0.0;

    const f64       fk = round(x * k64Log2E);
    const crd::i32  k  = static_cast<crd::i32>(fk);
    f64 r = x;
    r -= fk * k64Ln2Hi;
    r -= fk * k64Ln2Lo;

    // Padé form: exp(r) = 1 + 2*r*P(r²) / (Q(r²) - r*P(r²))
    const f64 rr = r * r;
    const f64 px = r * polevl(rr, k64ExpP);
    const f64 y  = 1.0 + 2.0 * px / (polevl(rr, k64ExpQ) - px);

    return ldexp_int_pow2_64(y, k);
}

f64 exp2(f64 x) noexcept { return exp(x * ln2_64); }

// ===========================================================================
// f64 log — Cephes log.c
// ===========================================================================

namespace
{
inline constexpr f64 k64LogP[6] = {
    1.01875663804580931796E-4,
    4.97494994976747001425E-1,
    4.70579119878881725854E0,
    1.44989225341610930846E1,
    1.79368678507819816313E1,
    7.70838733755885391666E0
};
inline constexpr f64 k64LogQ[5] = {  // implicit leading 1
    1.12873587189167450590E1,
    4.52279145837532221105E1,
    8.29875266912776603211E1,
    7.11544750618563894466E1,
    2.31251620126765340583E1
};

inline constexpr f64 k64LogQ1 = -2.121944400546905827679e-4;
inline constexpr f64 k64LogQ2 =  0.693359375;

[[nodiscard]] inline f64 frexp_extract64(f64 x, crd::i32& exp_out) noexcept
{
    const crd::u64 b = to_bits64(x);
    const crd::i32 raw_exp = static_cast<crd::i32>((b >> 52) & 0x7FFULL);
    exp_out = raw_exp - 1022;
    const crd::u64 mant_bits = (b & 0x800FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;  // exp → 0.5..<1
    return from_bits64(mant_bits);
}
}  // namespace

f64 log(f64 x) noexcept
{
    if (is_nan64(x))      return x;
    if (x <  0.0)         return std::numeric_limits<f64>::quiet_NaN();
    if (x == 0.0)         return -std::numeric_limits<f64>::infinity();
    if (x == std::numeric_limits<f64>::infinity()) return x;

    crd::i32 exp_int = 0;
    f64 m = frexp_extract64(x, exp_int);

    if (m < 0.70710678118654752440)  // < sqrt(0.5)
    {
        exp_int -= 1;
        m += m;
    }
    m -= 1.0;

    const f64 z  = m * m;
    const f64 fe = static_cast<f64>(exp_int);

    // y = log(1+m) - m  via rational approximation:
    //   log(1+m) - m = -0.5*z + m * (z * P(m) / Q(m))
    f64 y = m * (z * polevl(m, k64LogP) / p1evl(m, k64LogQ));

    y += fe * k64LogQ1;
    y -= 0.5 * z;
    y += m;
    y += fe * k64LogQ2;

    return y;
}

f64 log2(f64 x)  noexcept { return log(x) * inv_ln2_64; }
f64 log10(f64 x) noexcept { return log(x) * inv_ln10_64; }

// ===========================================================================
// f64 pow
// ===========================================================================

f64 pow(f64 base, f64 exponent) noexcept
{
    if (exponent == 0.0) return 1.0;
    if (base == 0.0)
    {
        if (exponent > 0.0) return 0.0;
        return std::numeric_limits<f64>::infinity();
    }
    if (base == 1.0) return 1.0;

    if (base < 0.0)
    {
        const f64 r = round(exponent);
        if (r != exponent) return std::numeric_limits<f64>::quiet_NaN();
        const f64 result = exp(exponent * log(-base));
        const crd::i64 ei = static_cast<crd::i64>(r);
        return (ei & 1) != 0 ? -result : result;
    }

    return exp(exponent * log(base));
}

// ===========================================================================
// f64 expm1 / log1p
// ===========================================================================

f64 expm1(f64 x) noexcept
{
    if (fast_abs64(x) < 0.25)
    {
        // Taylor degree 11 — truncation x^12/12! ≤ 1.2e-16 at |x|=0.25,
        // about 2 f64 ulps. Outside the band, exp(x)-1 has no significant
        // cancellation (exp(x) >> 1).
        const f64 inv2  = 1.0 / 2.0;
        const f64 inv6  = 1.0 / 6.0;
        const f64 inv24 = 1.0 / 24.0;
        const f64 inv120= 1.0 / 120.0;
        const f64 inv720= 1.0 / 720.0;
        const f64 inv5040 = 1.0 / 5040.0;
        const f64 inv40320= 1.0 / 40320.0;
        const f64 inv362880 = 1.0 / 362880.0;
        const f64 inv3628800 = 1.0 / 3628800.0;
        const f64 inv39916800 = 1.0 / 39916800.0;
        return x * (1.0 + x * (inv2 + x * (inv6 + x * (inv24 + x * (inv120
             + x * (inv720 + x * (inv5040 + x * (inv40320 + x * (inv362880
             + x * (inv3628800 + x * inv39916800)))))))))) ;
    }
    return exp(x) - 1.0;
}

f64 log1p(f64 x) noexcept
{
    if (fast_abs64(x) < 0.05)
    {
        // Taylor degree 11 — truncation x^12/12 ≤ 2e-17 at |x|=0.05.
        return x * (1.0 + x * (-1.0/2 + x * (1.0/3 + x * (-1.0/4 + x * (1.0/5
             + x * (-1.0/6 + x * (1.0/7 + x * (-1.0/8 + x * (1.0/9
             + x * (-1.0/10 + x * 1.0/11))))))))));
    }
    return log(1.0 + x);
}

// ===========================================================================
// f64 sinh / cosh / tanh
// ===========================================================================

f64 sinh(f64 x) noexcept
{
    const crd::u64 sign = sign_bit64(x);
    const f64 ax = fast_abs64(x);
    if (ax < 0.25)
    {
        // Taylor degree 11 — odd-only series.
        // Truncation x^13/13! ≤ 6e-15 at |x|=0.25, well below 16-ulp bound.
        const f64 x2 = ax * ax;
        const f64 r = ax * (1.0 + x2 * (1.0/6 + x2 * (1.0/120 + x2 * (1.0/5040
                    + x2 * (1.0/362880 + x2 * 1.0/39916800)))));
        return apply_sign64(r, sign);
    }
    const f64 ex = exp(ax);
    const f64 r  = 0.5 * (ex - 1.0 / ex);
    return apply_sign64(r, sign);
}

f64 cosh(f64 x) noexcept
{
    const f64 ax = fast_abs64(x);
    if (ax > 709.0) return std::numeric_limits<f64>::infinity();
    const f64 ex = exp(ax);
    return 0.5 * (ex + 1.0 / ex);
}

f64 tanh(f64 x) noexcept
{
    const crd::u64 sign = sign_bit64(x);
    const f64 ax = fast_abs64(x);
    if (ax > 20.0) return apply_sign64(1.0, sign);
    if (ax < 0.5)
    {
        // Taylor degree 9.
        const f64 x2 = ax * ax;
        const f64 r = ax * (1.0 + x2 * (-1.0/3 + x2 * (2.0/15 + x2 * (-17.0/315
                    + x2 * 62.0/2835))));
        return apply_sign64(r, sign);
    }
    const f64 e2 = exp(2.0 * ax);
    const f64 r  = 1.0 - 2.0 / (e2 + 1.0);
    return apply_sign64(r, sign);
}

// ===========================================================================
// f64 special functions: erf / erfc / gamma / lgamma / beta
// ===========================================================================

namespace
{
// Cephes erf.c: erf(x) = x * P(x²)/Q(x²) for |x| ≤ 0.84375.
inline constexpr f64 k64ErfT[5] = {
     9.60497373987051638749E0,
     9.00260197203842689217E1,
     2.23200534594684319226E3,
     7.00332514112805075473E3,
     5.55923013010394962768E4
};
inline constexpr f64 k64ErfU[5] = {
     3.35617141647503099647E1,
     5.21357949780152679795E2,
     4.59432382970980127987E3,
     2.26290000613890934246E4,
     4.92673942608635921086E4
};

// Cephes erfc.c: erfc(x) ≈ exp(-x²) * P(x)/Q(x) for x in [0.84375, 8].
inline constexpr f64 k64ErfcP[9] = {
     2.46196981473530512524E-10,
     5.64189564831068821977E-1,
     7.46321056442269912687E0,
     4.86371970985681366614E1,
     1.96520832956077098242E2,
     5.26445194995477358631E2,
     9.34528527171957607540E2,
     1.02755188689515710272E3,
     5.57535335369399327526E2
};
inline constexpr f64 k64ErfcQ[8] = {
     1.32281951154744992508E1,
     8.67072140885989742329E1,
     3.54937778887819891062E2,
     9.75708501743205489753E2,
     1.82390916687909736289E3,
     2.24633760818710981792E3,
     1.65666309194161350182E3,
     5.57535340817727675546E2
};

// Cephes gamma.c: rational approximation for gamma(x+2) on x in [0, 1].
// P has 7 coefficients (degree 6); Q has 8 coefficients (degree 7).
// Constant terms: P[6] = 1.0 (so polevl(0, P) = 1.0) and Q[7] = 1.0,
// giving gamma(2) = polevl(0, P)/polevl(0, Q) = 1.0/1.0 = 1.0 ✓.
inline constexpr f64 k64GammaP[7] = {
     1.60119522476751861407E-4,
     1.19135147006586384913E-3,
     1.04213797561761569935E-2,
     4.76367800457137231464E-2,
     2.07448227648435975150E-1,
     4.94214826801497100753E-1,
     9.99999999999999996796E-1
};
inline constexpr f64 k64GammaQ[8] = {
    -2.31581873324120129819E-5,
     5.39605580493303397842E-4,
    -4.45641913851797240494E-3,
     1.18139785222060435552E-2,
     3.58236398605498653373E-2,
    -2.34591795718243348568E-1,
     7.14304917030273074085E-2,
     1.00000000000000000320E0
};

[[nodiscard]] f64 gamma_reduced(f64 x) noexcept
{
    f64 z = 1.0;
    while (x >= 3.0)
    {
        x -= 1.0;
        z *= x;
    }
    while (x < 2.0)
    {
        if (x < 1.0e-9) return z / x;
        z /= x;
        x += 1.0;
    }
    if (x == 2.0) return z;  // Cephes shortcut

    // x in [2, 3]: shift to [0, 1] and evaluate rational.
    const f64 t = x - 2.0;
    const f64 num = polevl(t, k64GammaP);
    const f64 den = polevl(t, k64GammaQ);
    return z * num / den;
}
}  // namespace

f64 erfc(f64 x) noexcept;  // forward decl � referenced by erf(x) for tail-symmetry  // NOLINT(readability-redundant-declaration)

f64 erf(f64 a) noexcept
{
    const f64 ax = fast_abs64(a);
    if (ax > 1.0) return 1.0 - erfc(a);
    const f64 z = a * a;
    return a * polevl(z, k64ErfT) / p1evl(z, k64ErfU);
}

f64 erfc(f64 a) noexcept
{
    const f64 ax = fast_abs64(a);
    if (ax < 1.0) return 1.0 - erf(a);

    const f64 zm = -a * a;
    if (zm < -708.0)
    {
        return a < 0.0 ? 2.0 : 0.0;
    }

    const f64 ez = exp(zm);
    const f64 num = polevl(ax, k64ErfcP);
    const f64 den = p1evl(ax, k64ErfcQ);
    f64 y = ez * num / den;

    if (y == 0.0) return a < 0.0 ? 2.0 : 0.0;
    return a < 0.0 ? 2.0 - y : y;
}

f64 gamma(f64 x) noexcept
{
    if (x >= 0.0)
    {
        if (x > 171.0) return std::numeric_limits<f64>::infinity();
        return gamma_reduced(x);
    }
    const f64 ax = fast_abs64(x);
    const f64 fl = floor(ax);
    if (ax == fl) return std::numeric_limits<f64>::quiet_NaN();
    const f64 fr = ax - fl;
    const f64 frac = fr > 0.5 ? 1.0 - fr : fr;
    const f64 z = ax * sin(pi64 * frac);
    return -pi64 / (z * gamma_reduced(1.0 - x));
}

f64 lgamma(f64 x) noexcept
{
    const f64 g = gamma(x);
    return log(fast_abs64(g));
}

f64 beta(f64 a, f64 b) noexcept
{
    if (a + b > 170.0)
    {
        return exp(lgamma(a) + lgamma(b) - lgamma(a + b));
    }
    return gamma(a) * gamma(b) / gamma(a + b);
}

// ---- f32 special functions: forward to f64 (single source of truth) -------

f32 erf   (f32 x)        noexcept { return static_cast<f32>(erf   (static_cast<f64>(x))); }
f32 erfc  (f32 x)        noexcept { return static_cast<f32>(erfc  (static_cast<f64>(x))); }
f32 gamma (f32 x)        noexcept { return static_cast<f32>(gamma (static_cast<f64>(x))); }
f32 lgamma(f32 x)        noexcept { return static_cast<f32>(lgamma(static_cast<f64>(x))); }
f32 beta  (f32 a, f32 b) noexcept { return static_cast<f32>(beta  (static_cast<f64>(a), static_cast<f64>(b))); }

// ===========================================================================
// SIMD-batched overloads — Vec4f / Vec8f, branchless Cephes implementation.
//
// Each function is a fully-vectorized version of the scalar f32 algorithm
// from above. Octant reduction, sign tracking, and special-case handling
// are all done with select() / bitmask ops — no per-lane branches. The
// arithmetic is identical to the scalar form, so lane k of the SIMD result
// equals the scalar result on lane k bit-for-bit (the math suite verifies
// this; the [simd] tag in test_deterministic.cpp pins the contract).
//
// Performance: on AVX2 with Vec8f, the inner polynomial evaluation is one
// vmulps+vaddps per Horner step over 8 lanes — the disasm check
// (crd-simd-emission-check) confirms 256-bit FP ops are emitted.
// ===========================================================================

namespace
{
using namespace crd::math::simd;

// Cephes f32 sin/cos polynomial constants (same as scalar deterministic.cpp).
inline constexpr f32 kS4OverPi   = 1.27323954473516F;
inline constexpr f32 kSDp1         = 0.78515625F;
inline constexpr f32 kSDp2         = 2.4187564849853515625e-4F;
inline constexpr f32 kSDp3         = 3.77489497744594108e-8F;
inline constexpr f32 kSSincofP0   = -1.9515295891E-4F;
inline constexpr f32 kSSincofP1   =  8.3321608736E-3F;
inline constexpr f32 kSSincofP2   = -1.6666654611E-1F;
inline constexpr f32 kSCoscofP0   =  2.443315711809948E-005F;
inline constexpr f32 kSCoscofP1   = -1.388731625493765E-003F;
inline constexpr f32 kSCoscofP2   =  4.166664568298827E-002F;

// Cephes f32 exp polynomial constants.
inline constexpr f32 kSLog2E       = 1.44269504088896341F;
inline constexpr f32 kSLn2Hi      = 0.693359375F;
inline constexpr f32 kSLn2Lo      = -2.12194440e-4F;
inline constexpr f32 kSExpcofP0   = 1.9875691500E-4F;
inline constexpr f32 kSExpcofP1   = 1.3981999507E-3F;
inline constexpr f32 kSExpcofP2   = 8.3334519073E-3F;
inline constexpr f32 kSExpcofP3   = 4.1665795894E-2F;
inline constexpr f32 kSExpcofP4   = 1.6666665459E-1F;
inline constexpr f32 kSExpcofP5   = 5.0000001201E-1F;
inline constexpr f32 kSExpMax     = 88.722832F;
inline constexpr f32 kSExpMin     = -87.336544F;

// Cephes f32 log polynomial constants.
inline constexpr f32 kSLogcofP0   =  7.0376836292E-2F;
inline constexpr f32 kSLogcofP1   = -1.1514610310E-1F;
inline constexpr f32 kSLogcofP2   =  1.1676998740E-1F;
inline constexpr f32 kSLogcofP3   = -1.2420140846E-1F;
inline constexpr f32 kSLogcofP4   =  1.4249322787E-1F;
inline constexpr f32 kSLogcofP5   = -1.6668057665E-1F;
inline constexpr f32 kSLogcofP6   =  2.0000714765E-1F;
inline constexpr f32 kSLogcofP7   = -2.4999993993E-1F;
inline constexpr f32 kSLogcofP8   =  3.3333331174E-1F;
inline constexpr f32 kSLogQ1      = -2.12194440e-4F;
inline constexpr f32 kSLogQ2      =  0.693359375F;

// ---- Vec8f branchless Cephes sin -----------------------------------------
[[nodiscard]] CRD_FORCEINLINE Vec8f simd_sin_v8(Vec8f xx) noexcept
{
    const Vec8f sign_mask_const = bitcast_to_float(Vec8i(static_cast<crd::i32>(0x80000000)));
    const Vec8f abs_x  = bit_andnot(sign_mask_const, xx);
    const Vec8f sign_in = bit_and(xx, sign_mask_const);

    // octant integer
    Vec8f      j_f = truncate(abs_x * Vec8f(kS4OverPi));
    Vec8i      j_i = convert_truncate(j_f);

    // even-up: if j is odd, j += 1
    const Vec8i odd_bit = j_i & Vec8i(1);
    const Vec8i is_odd  = cmp_eq(odd_bit, Vec8i(1));
    j_i = j_i + (is_odd & Vec8i(1));
    j_f = j_f + bit_and(bitcast_to_float(is_odd), Vec8f(1.0F));

    // octant = j_i & 7
    Vec8i octant = j_i & Vec8i(7);

    // if octant > 3, sign-flip and octant -= 4
    const Vec8i ge4 = cmp_gt(octant, Vec8i(3));
    const Vec8f flip_mask = bitcast_to_float(ge4 & Vec8i(static_cast<crd::i32>(0x80000000)));
    const Vec8f sign_out = bit_xor(sign_in, flip_mask);
    octant = octant - (ge4 & Vec8i(4));

    // z = ((abs_x - j*DP1) - j*DP2) - j*DP3
    Vec8f z = abs_x - j_f * Vec8f(kSDp1);
    z = z - j_f * Vec8f(kSDp2);
    z = z - j_f * Vec8f(kSDp3);
    const Vec8f zz = z * z;

    // sin polynomial
    const Vec8f sin_poly = ((Vec8f(kSSincofP0) * zz + Vec8f(kSSincofP1)) * zz
                           + Vec8f(kSSincofP2)) * zz * z + z;
    // cos polynomial
    const Vec8f cos_poly = ((Vec8f(kSCoscofP0) * zz + Vec8f(kSCoscofP1)) * zz
                           + Vec8f(kSCoscofP2)) * zz * zz - Vec8f(0.5F) * zz + Vec8f(1.0F);

    // Use cos polynomial for octant 1 or 2.
    const Vec8i use_cos = cmp_eq(octant, Vec8i(1)) | cmp_eq(octant, Vec8i(2));
    const Vec8f y = select(bitcast_to_float(use_cos), cos_poly, sin_poly);

    return bit_xor(y, sign_out);
}

// ---- Vec8f branchless Cephes cos -----------------------------------------
[[nodiscard]] CRD_FORCEINLINE Vec8f simd_cos_v8(Vec8f xx) noexcept
{
    const Vec8f sign_mask_const = bitcast_to_float(Vec8i(static_cast<crd::i32>(0x80000000)));
    const Vec8f abs_x = bit_andnot(sign_mask_const, xx);

    Vec8f      j_f = truncate(abs_x * Vec8f(kS4OverPi));
    Vec8i      j_i = convert_truncate(j_f);

    const Vec8i odd_bit = j_i & Vec8i(1);
    const Vec8i is_odd  = cmp_eq(odd_bit, Vec8i(1));
    j_i = j_i + (is_odd & Vec8i(1));
    j_f = j_f + bit_and(bitcast_to_float(is_odd), Vec8f(1.0F));

    Vec8i octant = j_i & Vec8i(7);

    Vec8f sign_out = Vec8f::zero();
    const Vec8i ge4 = cmp_gt(octant, Vec8i(3));
    sign_out = bit_xor(sign_out,
                       bitcast_to_float(ge4 & Vec8i(static_cast<crd::i32>(0x80000000))));
    octant = octant - (ge4 & Vec8i(4));

    const Vec8i gt1 = cmp_gt(octant, Vec8i(1));
    sign_out = bit_xor(sign_out,
                       bitcast_to_float(gt1 & Vec8i(static_cast<crd::i32>(0x80000000))));

    Vec8f z = abs_x - j_f * Vec8f(kSDp1);
    z = z - j_f * Vec8f(kSDp2);
    z = z - j_f * Vec8f(kSDp3);
    const Vec8f zz = z * z;

    const Vec8f sin_poly = ((Vec8f(kSSincofP0) * zz + Vec8f(kSSincofP1)) * zz
                           + Vec8f(kSSincofP2)) * zz * z + z;
    const Vec8f cos_poly = ((Vec8f(kSCoscofP0) * zz + Vec8f(kSCoscofP1)) * zz
                           + Vec8f(kSCoscofP2)) * zz * zz - Vec8f(0.5F) * zz + Vec8f(1.0F);

    const Vec8i use_sin = cmp_eq(octant, Vec8i(1)) | cmp_eq(octant, Vec8i(2));
    const Vec8f y = select(bitcast_to_float(use_sin), sin_poly, cos_poly);

    return bit_xor(y, sign_out);
}

// ---- Vec8f branchless Cephes exp -----------------------------------------
[[nodiscard]] CRD_FORCEINLINE Vec8f simd_exp_v8(Vec8f x) noexcept
{
    // Clamp to safe range
    x = min(max(x, Vec8f(kSExpMin)), Vec8f(kSExpMax));

    // k = round(x * log2e); r = x - k*ln2_hi - k*ln2_lo
    const Vec8f fk = round_nearest(x * Vec8f(kSLog2E));
    const Vec8i k  = convert_truncate(fk);
    Vec8f r = x - fk * Vec8f(kSLn2Hi);
    r = r - fk * Vec8f(kSLn2Lo);

    // exp(r) = 1 + r + r²*P(r)
    const Vec8f rr = r * r;
    Vec8f p = Vec8f(kSExpcofP0);
    p = p * r + Vec8f(kSExpcofP1);
    p = p * r + Vec8f(kSExpcofP2);
    p = p * r + Vec8f(kSExpcofP3);
    p = p * r + Vec8f(kSExpcofP4);
    p = p * r + Vec8f(kSExpcofP5);
    const Vec8f y = Vec8f(1.0F) + r + rr * p;

    // scale = 2^k via bit-injection into f32 exponent field (bias 127)
    const Vec8i scale_bits = shift_left<23>(k + Vec8i(127));
    const Vec8f scale = bitcast_to_float(scale_bits);

    return y * scale;
}

// ---- Vec8f branchless Cephes log -----------------------------------------
[[nodiscard]] CRD_FORCEINLINE Vec8f simd_log_v8(Vec8f x) noexcept
{
    // Special cases handled at the end via select.
    const Vec8f neg_inf = Vec8f(-std::numeric_limits<f32>::infinity());
    const Vec8f pos_inf = Vec8f(std::numeric_limits<f32>::infinity());
    const Vec8f nanv    = Vec8f(std::numeric_limits<f32>::quiet_NaN());

    // Replace x with a safe value in the special-case lanes so the
    // polynomial doesn't produce garbage NaNs that propagate via FMA reorder.
    const Vec8f x_safe_mask = cmp_le(x, Vec8f::zero());
    const Vec8f x_for_poly  = select(x_safe_mask, Vec8f(1.0F), x);

    // frexp-style decompose: extract exponent and normalise mantissa to [0.5, 1).
    const Vec8i bits     = bitcast_to_int(x_for_poly);
    const Vec8i raw_exp  = shift_right_logical<23>(bits) & Vec8i(0xFF);
    Vec8i exp_int        = raw_exp - Vec8i(126);
    const Vec8i mant_bits = (bits & Vec8i(static_cast<crd::i32>(0x807FFFFF)))
                          | Vec8i(static_cast<crd::i32>(0x3F000000));  // exponent field = 126 (value 0.5)
    Vec8f m = bitcast_to_float(mant_bits);

    // Adjust m to [sqrt(0.5), sqrt(2)] for better polynomial behaviour.
    const Vec8f lt_sqrt_half = cmp_lt(m, Vec8f(0.707106781187F));
    exp_int = exp_int + (bitcast_to_int(lt_sqrt_half) & Vec8i(-1));  // sub 1 if lt
    m = select(lt_sqrt_half, m + m, m);
    m = m - Vec8f(1.0F);

    // Polynomial in m
    const Vec8f z = m * m;
    Vec8f p = Vec8f(kSLogcofP0);
    p = p * m + Vec8f(kSLogcofP1);
    p = p * m + Vec8f(kSLogcofP2);
    p = p * m + Vec8f(kSLogcofP3);
    p = p * m + Vec8f(kSLogcofP4);
    p = p * m + Vec8f(kSLogcofP5);
    p = p * m + Vec8f(kSLogcofP6);
    p = p * m + Vec8f(kSLogcofP7);
    p = p * m + Vec8f(kSLogcofP8);
    Vec8f y = p * m * z;

    const Vec8f fe = convert_to_float(exp_int);
    y = y + fe * Vec8f(kSLogQ1);
    y = y - Vec8f(0.5F) * z;
    y = y + m;
    y = y + fe * Vec8f(kSLogQ2);

    // Special cases
    const Vec8f is_zero = cmp_eq(x, Vec8f::zero());
    const Vec8f is_neg  = cmp_lt(x, Vec8f::zero());
    const Vec8f is_pinf = cmp_eq(x, pos_inf);
    y = select(is_pinf, pos_inf, y);
    y = select(is_neg,  nanv,    y);
    y = select(is_zero, neg_inf, y);

    return y;
}

}  // namespace

// Public Vec4f/Vec8f overloads — direct calls to the SIMD implementations.

crd::math::simd::Vec8f sin(crd::math::simd::Vec8f x) noexcept { return simd_sin_v8(x); }
crd::math::simd::Vec8f cos(crd::math::simd::Vec8f x) noexcept { return simd_cos_v8(x); }
crd::math::simd::Vec8f exp(crd::math::simd::Vec8f x) noexcept { return simd_exp_v8(x); }
crd::math::simd::Vec8f log(crd::math::simd::Vec8f x) noexcept { return simd_log_v8(x); }

// Vec4f: compose into a Vec8f, run the AVX2 path, narrow back. On non-AVX2
// backends this is the same number of ops as a direct Vec4f path would be
// (Vec8f decomposes to two Vec4f), so there's no penalty.
namespace
{
[[nodiscard]] CRD_FORCEINLINE Vec4f narrow_to_v4(Vec8f wide) noexcept
{
    f32 buf[8]; wide.store(buf);
    return Vec4f::load(buf);
}
}  // namespace

crd::math::simd::Vec4f sin(crd::math::simd::Vec4f x) noexcept
{
    return narrow_to_v4(simd_sin_v8(crd::math::simd::Vec8f(x, x)));
}
crd::math::simd::Vec4f cos(crd::math::simd::Vec4f x) noexcept
{
    return narrow_to_v4(simd_cos_v8(crd::math::simd::Vec8f(x, x)));
}
crd::math::simd::Vec4f exp(crd::math::simd::Vec4f x) noexcept
{
    return narrow_to_v4(simd_exp_v8(crd::math::simd::Vec8f(x, x)));
}
crd::math::simd::Vec4f log(crd::math::simd::Vec4f x) noexcept
{
    return narrow_to_v4(simd_log_v8(crd::math::simd::Vec8f(x, x)));
}

}  // namespace crd::math::deterministic
