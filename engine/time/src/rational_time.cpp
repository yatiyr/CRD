#include <crd/time/rational_time.hpp>

#include <cmath>   // std::isfinite / std::floor — classification + exact-integer split (not transcendental)
#include <cstdio>  // snprintf — timecode text

namespace crd::time
{

namespace
{
    // the SMPTE /1001 families — snapped FIRST so the canonical editorial rates always import as themselves
    constexpr RationalRate kSmpteFamily[] = {
        kRateNtsc24, kRateNtsc30, kRateNtsc48, kRateNtsc60, kRateNtsc120,
    };

    constexpr crd::i64 kMaxI32       = 0x7FFFFFFF;
    constexpr crd::i64 kCfDenLimit  = 1LL << 20U; // continued-fraction denominator cap (every real rate is far below)
    constexpr int      kMaxBinScale = 40;         // binary-fraction fallback: r·2^k integral within i32
} // namespace

RationalRate rate_from_f64(crd::f64 rate) noexcept
{
    if (!std::isfinite(rate) || rate <= 0.0) { return {}; }

    // integral fast path (24, 25, 30, 48, 50, 60, 96, 120 …)
    if (rate == std::floor(rate) && rate <= static_cast<crd::f64>(kMaxI32))
    {
        return make_rate(static_cast<crd::i64>(rate), 1);
    }

    // SMPTE snap: the f64 OTIO writes for 24000/1001 is EXACTLY (f64)24000/1001 — match it bit-for-bit
    for (const RationalRate& r : kSmpteFamily)
    {
        if (rate == static_cast<crd::f64>(r.num) / static_cast<crd::f64>(r.den)) { return r; }
    }

    // best rational by continued fractions, accepted ONLY when it round-trips to the exact input f64
    {
        crd::f64 x  = rate;
        crd::i64 h0 = 0;
        crd::i64 h1 = 1; // convergent numerators
        crd::i64 q0 = 1;
        crd::i64 q1 = 0; // convergent denominators
        for (int i = 0; i < 40; ++i)
        {
            const crd::f64 fa = std::floor(x);
            if (fa > static_cast<crd::f64>(kMaxI32)) { break; }
            const crd::i64 a  = static_cast<crd::i64>(fa);
            const crd::i64 h2 = a * h1 + h0;
            const crd::i64 q2 = a * q1 + q0;
            if (h2 > kMaxI32 || q2 > kCfDenLimit) { break; }
            h0 = h1;
            h1 = h2;
            q0 = q1;
            q1 = q2;
            if (q1 > 0 && static_cast<crd::f64>(h1) / static_cast<crd::f64>(q1) == rate)
            {
                return make_rate(h1, q1);
            }
            const crd::f64 frac = x - fa;
            if (frac <= 0.0) { break; }
            x = 1.0 / frac;
        }
    }

    // exact binary fraction: every finite double is m/2^k — lossless as long as it fits the i32 rate fields
    {
        crd::f64 num = rate;
        crd::i64 den = 1;
        for (int i = 0; i < kMaxBinScale && num != std::floor(num); ++i)
        {
            num *= 2.0;
            den *= 2;
        }
        if (num == std::floor(num) && num <= static_cast<crd::f64>(kMaxI32) && den <= kMaxI32)
        {
            return make_rate(static_cast<crd::i64>(num), den);
        }
    }
    return {};
}

RationalTime time_from_f64(crd::f64 value, crd::f64 rate) noexcept
{
    const RationalRate r = rate_from_f64(rate);
    if (!r.valid() || !std::isfinite(value)) { return {}; }

    if (value == std::floor(value) && value >= -9.0e15 && value <= 9.0e15) // integral frames — the OTIO norm
    {
        return {static_cast<crd::i64>(value), r};
    }

    // subframe value m/2^k → value m at rate (num·2^k)/den — EXACT while the scaled rate fits
    crd::f64 scaled = value;
    crd::i64 pow2   = 1;
    for (int i = 0; i < kMaxBinScale && scaled != std::floor(scaled); ++i)
    {
        scaled *= 2.0;
        pow2 *= 2;
        const crd::i64 scaled_num = static_cast<crd::i64>(r.num) * pow2;
        if (scaled_num > kMaxI32) { break; }
        if (scaled == std::floor(scaled) && scaled >= -9.0e15 && scaled <= 9.0e15)
        {
            return {static_cast<crd::i64>(scaled), make_rate(scaled_num, r.den)};
        }
    }
    // beyond exact representation (sub-2^-40 subframe dust): the containing tick, floor — documented rounding
    const crd::f64 floored = std::floor(value);
    if (floored < -9.0e15 || floored > 9.0e15) { return {}; }
    return {static_cast<crd::i64>(floored), r};
}

crd::f64 rate_to_f64(RationalRate rate) noexcept
{
    if (!rate.valid()) { return 0.0; }
    return static_cast<crd::f64>(rate.num) / static_cast<crd::f64>(rate.den);
}

crd::f64 value_to_f64(const RationalTime& t) noexcept
{
    return static_cast<crd::f64>(t.value);
}

// ── SMPTE timecode ──────────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
    // nominal integer fps for timecode counting: integer rates directly; the /1001 family counts at num/1000
    [[nodiscard]] crd::i64 nominal_fps(RationalRate rate) noexcept
    {
        if (!rate.valid()) { return 0; }
        if (rate.den == 1) { return rate.num; }
        if (rate.den == 1001 && rate.num % 1000 == 0) { return rate.num / 1000; }
        return 0;
    }

    // drop-frame is DEFINED for 29.97 (drop 2/min) and 59.94 (drop 4/min) only — SMPTE 12M
    [[nodiscard]] crd::i64 drop_count(RationalRate rate) noexcept
    {
        if (rate == kRateNtsc30) { return 2; }
        if (rate == kRateNtsc60) { return 4; }
        return 0;
    }
} // namespace

bool to_timecode(const RationalTime& t, bool drop_frame, Timecode& out) noexcept
{
    out.text[0] = '\0';
    const crd::i64 fps = nominal_fps(t.rate);
    if (fps <= 0 || fps > 99 || t.value < 0) { return false; }

    crd::i64 display = t.value;
    if (drop_frame)
    {
        const crd::i64 d = drop_count(t.rate);
        if (d == 0) { return false; }
        const crd::i64 per_min_nominal = 60 * fps;         // the block's first (undropped) minute
        const crd::i64 per_min_actual  = 60 * fps - d;     // each of the 9 dropped minutes
        const crd::i64 per_10min       = per_min_nominal + 9 * per_min_actual;
        const crd::i64 blocks          = t.value / per_10min;
        const crd::i64 rem             = t.value % per_10min;
        crd::i64       minutes_in_block = 0;
        crd::i64       pos_in_min       = rem;
        if (rem >= per_min_nominal)
        {
            const crd::i64 rem2 = rem - per_min_nominal;
            minutes_in_block    = 1 + rem2 / per_min_actual;
            pos_in_min          = rem2 % per_min_actual + d; // display numbering skips the dropped d
        }
        const crd::i64 total_minutes = blocks * 10 + minutes_in_block;
        display                      = total_minutes * per_min_nominal + pos_in_min;
    }

    const crd::i64 ff    = display % fps;
    const crd::i64 total = display / fps;
    const crd::i64 ss    = total % 60;
    const crd::i64 mm    = (total / 60) % 60;
    const crd::i64 hh    = total / 3600;
    if (hh > 99) { return false; }
    std::snprintf(out.text, sizeof(out.text), "%02lld:%02lld:%02lld%c%02lld", static_cast<long long>(hh),
                  static_cast<long long>(mm), static_cast<long long>(ss), drop_frame ? ';' : ':',
                  static_cast<long long>(ff));
    return true;
}

bool from_timecode(const char* text, RationalRate rate, RationalTime& out) noexcept
{
    out = {};
    const crd::i64 fps = nominal_fps(rate);
    if (text == nullptr || fps <= 0 || fps > 99) { return false; }

    const auto two_digits = [](const char* p, crd::i64& v) noexcept -> bool {
        if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') { return false; }
        v = static_cast<crd::i64>(p[0] - '0') * 10 + (p[1] - '0');
        return true;
    };
    crd::i64 hh = 0;
    crd::i64 mm = 0;
    crd::i64 ss = 0;
    crd::i64 ff = 0;
    if (!two_digits(text, hh) || text[2] != ':' || !two_digits(text + 3, mm) || text[5] != ':' ||
        !two_digits(text + 6, ss))
    {
        return false;
    }
    const char sep = text[8];
    if ((sep != ':' && sep != ';') || !two_digits(text + 9, ff) || text[11] != '\0') { return false; }
    if (mm > 59 || ss > 59 || ff >= fps) { return false; }

    const bool drop_frame = sep == ';';
    if (drop_frame)
    {
        const crd::i64 d = drop_count(rate);
        if (d == 0) { return false; }
        const crd::i64 total_minutes = hh * 60 + mm;
        if (total_minutes % 10 != 0 && ss == 0 && ff < d) { return false; } // a DROPPED number is not a time
        const crd::i64 display = ((total_minutes * 60) + ss) * fps + ff;
        out = {display - d * (total_minutes - total_minutes / 10), rate};
        return true;
    }
    out = {((hh * 60 + mm) * 60 + ss) * fps + ff, rate};
    return true;
}

} // namespace crd::time
