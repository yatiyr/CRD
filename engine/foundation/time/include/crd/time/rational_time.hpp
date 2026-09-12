#pragma once

// ---------------------------------------------------------------------------
// crd-time — RationalTime: EXACT editorial/media time (GEO-9, D-007 row 74).
//
// A media time is `value` ticks at a rational rate `num/den` ticks-per-second
// (24 = 24/1 · NTSC 23.976 = 24000/1001 · 29.97 = 30000/1001). Seconds are
// value*den/num EXACTLY — float seconds accumulate drift over a feature-length
// timeline (23.976 fps × 2 h ≈ 172,672 frames; one f32 ulp per add is a frame
// slip), which is WHY OTIO's model is rational and why this type exists.
// Floats appear ONLY at explicitly-named edges (`to_seconds_f64`,
// `to_duration`, `from_f64` — the OTIO/JSON boundary).
//
// Exactness contract:
//   - same-rate arithmetic is pure i64 tick math (the timeline fast path);
//   - cross-rate compare/add go through 128-bit intermediates — never a
//     rounded rescale (`detail::` two-limb helpers; MSVC x64 + GCC/Clang
//     intrinsic fast paths, portable 32-bit-limb fallback);
//   - `rescaled_to` states its rounding (`rescales_exactly` to ask first);
//   - every finite f64 IS a rational (mantissa/2^k): `from_f64_rate` snaps
//     the SMPTE families (24000/1001 …) and falls back to the exact binary
//     fraction, so OTIO import is lossless even for exotic rates.
//
// SMPTE timecode (including 29.97/59.94 DROP-FRAME — the 2-per-minute skip,
// tenth minutes exempt) lives in rational_time.cpp: `to_timecode` /
// `from_timecode`, round-trip-exact across a full 24 h day.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/time/duration.hpp>
#include <crd/units/quantity.hpp>

namespace crd::time
{

// ── the rate: ticks per second as a reduced positive fraction ──────────────────────────────────────────────────────────

struct RationalRate
{
    crd::i32 num = 0; // ticks-per-second numerator   (0/0 = the invalid rate)
    crd::i32 den = 0; // ticks-per-second denominator

    [[nodiscard]] constexpr bool valid() const noexcept { return num > 0 && den > 0; }
    [[nodiscard]] constexpr bool operator==(const RationalRate& o) const noexcept
    {
        return num == o.num && den == o.den;
    }
    [[nodiscard]] constexpr bool operator!=(const RationalRate& o) const noexcept { return !(*this == o); }
};

namespace detail
{
    [[nodiscard]] constexpr crd::i64 gcd_i64(crd::i64 a, crd::i64 b) noexcept
    {
        while (b != 0)
        {
            const crd::i64 t = a % b;
            a = b;
            b = t;
        }
        return a < 0 ? -a : a;
    }

    // unsigned 64×64 → 128 (hi, lo) — portable 32-bit-limb schoolbook product
    struct U128
    {
        crd::u64 hi = 0;
        crd::u64 lo = 0;
    };

    [[nodiscard]] constexpr U128 umul_64_64(crd::u64 a, crd::u64 b) noexcept
    {
        const crd::u64 a_lo = a & 0xFFFFFFFFULL;
        const crd::u64 a_hi = a >> 32U;
        const crd::u64 b_lo = b & 0xFFFFFFFFULL;
        const crd::u64 b_hi = b >> 32U;
        const crd::u64 p_ll = a_lo * b_lo;
        const crd::u64 p_lh = a_lo * b_hi;
        const crd::u64 p_hl = a_hi * b_lo;
        const crd::u64 p_hh = a_hi * b_hi;
        const crd::u64 mid  = (p_ll >> 32U) + (p_lh & 0xFFFFFFFFULL) + (p_hl & 0xFFFFFFFFULL);
        U128           r;
        r.lo = (mid << 32U) | (p_ll & 0xFFFFFFFFULL);
        r.hi = p_hh + (p_lh >> 32U) + (p_hl >> 32U) + (mid >> 32U);
        return r;
    }

    [[nodiscard]] constexpr int u128_cmp(const U128& a, const U128& b) noexcept
    {
        if (a.hi != b.hi) { return a.hi < b.hi ? -1 : 1; }
        if (a.lo != b.lo) { return a.lo < b.lo ? -1 : 1; }
        return 0;
    }

    // u128 / u64 → (quotient u128, remainder u64) — shift-subtract long division (exactness paths only, never hot)
    struct U128DivResult
    {
        U128     quot;
        crd::u64 rem = 0;
    };

    [[nodiscard]] constexpr U128DivResult udiv_128_64(const U128& n, crd::u64 d) noexcept
    {
        U128DivResult r;
        if (d == 0) { return r; } // guarded by callers; a zero divisor yields 0 (never UB)
        crd::u64 rem = 0;
        for (int bit = 127; bit >= 0; --bit)
        {
            rem <<= 1U;
            const crd::u64 limb = bit >= 64 ? n.hi : n.lo;
            const auto     idx  = static_cast<crd::u32>(bit >= 64 ? bit - 64 : bit);
            rem |= (limb >> idx) & 1ULL;
            if (rem >= d)
            {
                rem -= d;
                if (bit >= 64) { r.quot.hi |= 1ULL << idx; }
                else { r.quot.lo |= 1ULL << idx; }
            }
        }
        r.rem = rem;
        return r;
    }

    // |a| as u64 with the sign split off (INT64_MIN-safe)
    [[nodiscard]] constexpr crd::u64 abs_u64(crd::i64 v) noexcept
    {
        return v < 0 ? ~static_cast<crd::u64>(v) + 1ULL : static_cast<crd::u64>(v);
    }
} // namespace detail

// Reduced, sign-normalized rate. Zero/negative inputs yield the INVALID rate (never a bogus positive one).
[[nodiscard]] constexpr RationalRate make_rate(crd::i64 num, crd::i64 den) noexcept
{
    if (num <= 0 || den <= 0) { return {}; }
    const crd::i64 g  = detail::gcd_i64(num, den);
    const crd::i64 rn = num / g;
    const crd::i64 rd = den / g;
    if (rn > 0x7FFFFFFF || rd > 0x7FFFFFFF) { return {}; } // beyond any editorial rate — refuse, never truncate
    return {static_cast<crd::i32>(rn), static_cast<crd::i32>(rd)};
}

inline constexpr RationalRate kRate24      = {24, 1};
inline constexpr RationalRate kRate25      = {25, 1};
inline constexpr RationalRate kRate30      = {30, 1};
inline constexpr RationalRate kRate48      = {48, 1};
inline constexpr RationalRate kRate50      = {50, 1};
inline constexpr RationalRate kRate60      = {60, 1};
inline constexpr RationalRate kRateNtsc24  = {24000, 1001};
inline constexpr RationalRate kRateNtsc30   = {30000, 1001};
inline constexpr RationalRate kRateNtsc48  = {48000, 1001};
inline constexpr RationalRate kRateNtsc60   = {60000, 1001};
inline constexpr RationalRate kRateNtsc120  = {120000, 1001};

// ── the time: `value` ticks at `rate` ──────────────────────────────────────────────────────────────────────────────────

struct RationalTime
{
    crd::i64     value = 0;
    RationalRate rate  = {};

    [[nodiscard]] constexpr bool valid() const noexcept { return rate.valid(); }
};

[[nodiscard]] constexpr RationalTime make_time(crd::i64 value, RationalRate rate) noexcept { return {value, rate}; }

// EXACT three-way compare across rates: value_a*den_a*num_b <=> value_b*den_b*num_a through 128-bit magnitudes.
[[nodiscard]] constexpr int compare(const RationalTime& a, const RationalTime& b) noexcept
{
    if (a.rate == b.rate) // the timeline fast path — pure i64
    {
        if (a.value == b.value) { return 0; }
        return a.value < b.value ? -1 : 1;
    }
    const bool neg_a = a.value < 0;
    const bool neg_b = b.value < 0;
    if (neg_a != neg_b) { return neg_a ? -1 : 1; }
    // |v|*den ≤ 2^63·2^31 = 2^94; ×num ≤ 2^125 — two u64 multiplies, the second on a (≤2^30, u64) pair
    const detail::U128 ad = detail::umul_64_64(detail::abs_u64(a.value), static_cast<crd::u64>(a.rate.den));
    const detail::U128 bd = detail::umul_64_64(detail::abs_u64(b.value), static_cast<crd::u64>(b.rate.den));
    // (hi,lo) × c with c ≤ 2^31: hi' = hi*c + carry(lo*c)
    const auto mul_small = [](const detail::U128& x, crd::u64 c) constexpr noexcept -> detail::U128 {
        const detail::U128 lo_prod = detail::umul_64_64(x.lo, c);
        detail::U128       r;
        r.lo = lo_prod.lo;
        r.hi = x.hi * c + lo_prod.hi;
        return r;
    };
    const detail::U128 lhs = mul_small(ad, static_cast<crd::u64>(b.rate.num));
    const detail::U128 rhs = mul_small(bd, static_cast<crd::u64>(a.rate.num));
    const int          mag = detail::u128_cmp(lhs, rhs);
    return neg_a ? -mag : mag;
}

[[nodiscard]] constexpr bool operator==(const RationalTime& a, const RationalTime& b) noexcept
{
    return a.rate.valid() && b.rate.valid() && compare(a, b) == 0;
}
[[nodiscard]] constexpr bool operator<(const RationalTime& a, const RationalTime& b) noexcept
{
    return compare(a, b) < 0;
}
[[nodiscard]] constexpr bool operator<=(const RationalTime& a, const RationalTime& b) noexcept
{
    return compare(a, b) <= 0;
}
[[nodiscard]] constexpr bool operator>(const RationalTime& a, const RationalTime& b) noexcept
{
    return compare(a, b) > 0;
}
[[nodiscard]] constexpr bool operator>=(const RationalTime& a, const RationalTime& b) noexcept
{
    return compare(a, b) >= 0;
}

// Does `t` land EXACTLY on a tick of `rate`? (ask before `rescaled_to` when rounding would be a bug)
[[nodiscard]] constexpr bool rescales_exactly(const RationalTime& t, RationalRate rate) noexcept
{
    if (!t.rate.valid() || !rate.valid()) { return false; }
    if (t.rate == rate) { return true; }
    // v' = v * (num_r * den_t) / (num_t * den_r) — exact iff the denominator divides the 128-bit numerator
    const crd::u64     a = static_cast<crd::u64>(rate.num) * static_cast<crd::u64>(t.rate.den);  // ≤ 2^62
    const crd::u64     b = static_cast<crd::u64>(t.rate.num) * static_cast<crd::u64>(rate.den);  // ≤ 2^62
    const detail::U128 n = detail::umul_64_64(detail::abs_u64(t.value), a);
    return detail::udiv_128_64(n, b).rem == 0;
}

enum class RescaleRounding : crd::u8
{
    Floor = 0, // toward negative infinity (frame CONTAINING the instant — the eval query direction)
    Round,     // nearest, ties away from zero (display/UI direction)
};

// v' = v * (num_r·den_t)/(num_t·den_r) at 128-bit exactness, rounded as asked when inexact.
[[nodiscard]] constexpr RationalTime rescaled_to(const RationalTime& t, RationalRate rate,
                                                 RescaleRounding rounding = RescaleRounding::Floor) noexcept
{
    if (!t.rate.valid() || !rate.valid()) { return {}; }
    if (t.rate == rate) { return t; }
    const crd::u64     a   = static_cast<crd::u64>(rate.num) * static_cast<crd::u64>(t.rate.den);
    const crd::u64     b   = static_cast<crd::u64>(t.rate.num) * static_cast<crd::u64>(rate.den);
    const bool         neg = t.value < 0;
    const detail::U128 n   = detail::umul_64_64(detail::abs_u64(t.value), a);
    const auto         d   = detail::udiv_128_64(n, b);
    crd::u64           q   = d.quot.lo; // editorial magnitudes never fill quot.hi (v·a/b ≤ v·2^31)
    if (rounding == RescaleRounding::Round)
    {
        if (d.rem * 2 >= b) { q += 1; }
    }
    else if (neg && d.rem != 0)
    {
        q += 1; // floor of a negative = magnitude rounded UP
    }
    const crd::i64 v = neg ? -static_cast<crd::i64>(q) : static_cast<crd::i64>(q);
    return {v, rate};
}

// a + b / a − b, EXACT: same-rate is i64; cross-rate lands on the merged grid (tick = gcd of the two tick durations,
// the coarsest grid holding BOTH exactly). Editorial rate products stay far inside i64 (24000×30000 < 2^31).
[[nodiscard]] constexpr RationalTime add(const RationalTime& a, const RationalTime& b) noexcept
{
    if (!a.rate.valid() || !b.rate.valid()) { return {}; }
    if (a.rate == b.rate) { return {a.value + b.value, a.rate}; }
    // merged rate = (num_a·num_b) / gcd(den_a·num_b, den_b·num_a)
    const crd::i64     nn = static_cast<crd::i64>(a.rate.num) * b.rate.num;
    const crd::i64     da = static_cast<crd::i64>(a.rate.den) * b.rate.num;
    const crd::i64     db = static_cast<crd::i64>(b.rate.den) * a.rate.num;
    const crd::i64     g  = detail::gcd_i64(da, db);
    const RationalRate m  = make_rate(nn, g);
    if (!m.valid()) { return {}; }
    const crd::i64 va = a.value * (da / g); // exact by construction of the merged grid
    const crd::i64 vb = b.value * (db / g);
    return {va + vb, m};
}

[[nodiscard]] constexpr RationalTime sub(const RationalTime& a, const RationalTime& b) noexcept
{
    return add(a, {-b.value, b.rate});
}

// ── the float EDGES (explicitly named; never used for editorial structure) ─────────────────────────────────────────────

[[nodiscard]] constexpr crd::f64 to_seconds_f64(const RationalTime& t) noexcept
{
    if (!t.rate.valid()) { return 0.0; }
    return static_cast<crd::f64>(t.value) * static_cast<crd::f64>(t.rate.den) / static_cast<crd::f64>(t.rate.num);
}

[[nodiscard]] inline Duration to_duration(const RationalTime& t) noexcept
{
    return crd::units::Quantity<crd::units::dim::Time, crd::f64>{to_seconds_f64(t)};
}

// OTIO's f64 value at an f64 rate → EXACT rational time (rational_time.cpp: SMPTE-family snap, integral fast path,
// exact-binary-fraction fallback — lossless for EVERY finite double). Returns invalid on NaN/Inf/rate ≤ 0.
[[nodiscard]] RationalRate rate_from_f64(crd::f64 rate) noexcept;
[[nodiscard]] RationalTime time_from_f64(crd::f64 value, crd::f64 rate) noexcept;
// The export edge: the f64 pair OTIO serializes. Exact when the components fit f64 (editorial values always do).
[[nodiscard]] crd::f64 rate_to_f64(RationalRate rate) noexcept;
[[nodiscard]] crd::f64 value_to_f64(const RationalTime& t) noexcept;

// ── SMPTE timecode (rational_time.cpp) ─────────────────────────────────────────────────────────────────────────────────

// "HH:MM:SS:FF" (non-drop) / "HH:MM:SS;FF" (drop-frame — 30000/1001 and 60000/1001 ONLY, per SMPTE 12M: frames
// 00/01 (×2 at 59.94) of each minute skip, tenth minutes exempt). Returns false when the rate has no integer
// nominal fps or the time is negative. Round-trip exact across a 24 h day (gated).
struct Timecode
{
    char text[16] = {}; // NUL-terminated "HH:MM:SS:FF" / "HH:MM:SS;FF"
};

[[nodiscard]] bool to_timecode(const RationalTime& t, bool drop_frame, Timecode& out) noexcept;
[[nodiscard]] bool from_timecode(const char* text, RationalRate rate, RationalTime& out) noexcept;

// ── TimeRange: [start, start+duration) ─────────────────────────────────────────────────────────────────────────────────

struct TimeRange
{
    RationalTime start    = {};
    RationalTime duration = {};

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return start.valid() && duration.valid() && duration.value >= 0;
    }
    [[nodiscard]] constexpr RationalTime end_exclusive() const noexcept { return add(start, duration); }
    [[nodiscard]] constexpr bool         contains(const RationalTime& t) const noexcept
    {
        return valid() && compare(t, start) >= 0 && compare(t, end_exclusive()) < 0;
    }
    [[nodiscard]] constexpr bool overlaps(const TimeRange& o) const noexcept
    {
        return valid() && o.valid() && compare(start, o.end_exclusive()) < 0 &&
               compare(o.start, end_exclusive()) < 0;
    }
};

} // namespace crd::time
