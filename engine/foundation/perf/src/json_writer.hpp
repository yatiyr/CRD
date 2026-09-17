#pragma once
// crd-perf -- internal JSON-append helpers, shared by the DiagnosticEvent serializer (diagnostics.cpp) and the
// CPROF -> Perfetto trace-events exporter (capture_export.cpp). Locale-INDEPENDENT by construction: every number is
// formatted by integer arithmetic (no printf/%f), because a comma-decimal locale (this box is tr-TR) would otherwise
// emit invalid JSON. Header-only inline; no owning STL containers.

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <bit>   // std::bit_cast
#include <cmath> // std::isfinite, std::llround

namespace crd::perf::detail
{
namespace cont = crd::containers;

// Append a JSON-escaped string body (NO surrounding quotes) to `out`.
inline void append_json_escaped(cont::String& out, cont::StringView s)
{
    for (const char c : s)
    {
        switch (c)
        {
        case '"':  out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n");  break;
        case '\r': out.append("\\r");  break;
        case '\t': out.append("\\t");  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U)
            {
                static constexpr char kHex[] = "0123456789abcdef";
                out.append("\\u00");
                out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xFU]);
                out.push_back(kHex[static_cast<unsigned char>(c) & 0xFU]);
            }
            else
            {
                out.push_back(c);
            }
            break;
        }
    }
}

// Unsigned decimal.
inline void append_u64(cont::String& out, crd::u64 v)
{
    char buf[20];
    int  n = 0;
    if (v == 0U)
    {
        out.push_back('0');
        return;
    }
    while (v > 0U && n < 20)
    {
        buf[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    }
    while (n-- > 0)
        out.push_back(buf[n]);
}

// Signed decimal.
inline void append_i64(cont::String& out, crd::i64 v)
{
    if (v < 0)
    {
        out.push_back('-');
        // Negate in u64 to avoid INT64_MIN overflow.
        append_u64(out, static_cast<crd::u64>(~static_cast<crd::u64>(v) + 1U));
    }
    else
    {
        append_u64(out, static_cast<crd::u64>(v));
    }
}

// A non-negative nanosecond value as MICROSECONDS with a 3-digit fraction (Perfetto `ts`/`dur` are microseconds).
// Integer math only: whole = ns/1000, frac = ns%1000 (thousandths of a microsecond == the ns remainder).
inline void append_us_from_ns(cont::String& out, crd::i64 ns)
{
    if (ns < 0)
        ns = 0; // MonotonicClock-relative endpoints and durations are non-negative; clamp defensively.
    const crd::u64 whole = static_cast<crd::u64>(ns) / 1000ULL;
    const crd::u64 frac  = static_cast<crd::u64>(ns) % 1000ULL;
    append_u64(out, whole);
    out.push_back('.');
    out.push_back(static_cast<char>('0' + (frac / 100ULL) % 10ULL));
    out.push_back(static_cast<char>('0' + (frac / 10ULL) % 10ULL));
    out.push_back(static_cast<char>('0' + frac % 10ULL));
}

// A finite double as a fixed-point JSON number with 6 fractional digits (integer math; llround is locale-free).
// Non-finite -> 0 so the document stays valid JSON.
inline void append_f64_fixed(cont::String& out, double v)
{
    if (!std::isfinite(v))
    {
        out.push_back('0');
        return;
    }
    const bool neg = v < 0.0;
    if (neg)
        v = -v;
    const crd::u64 scaled = static_cast<crd::u64>(std::llround(v * 1'000'000.0));
    const crd::u64 whole  = scaled / 1'000'000ULL;
    crd::u64       frac   = scaled % 1'000'000ULL;
    if (neg && (whole != 0U || frac != 0U))
        out.push_back('-');
    append_u64(out, whole);
    out.push_back('.');
    char fbuf[6];
    for (int i = 5; i >= 0; --i)
    {
        fbuf[i] = static_cast<char>('0' + (frac % 10ULL));
        frac /= 10ULL;
    }
    for (int i = 0; i < 6; ++i)
        out.push_back(fbuf[i]);
}

} // namespace crd::perf::detail
