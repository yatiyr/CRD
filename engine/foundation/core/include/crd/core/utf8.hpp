#pragma once

// crd-core -- UTF-8 well-formedness (RFC 3629; the Unicode Standard's table of well-formed byte sequences).
//
// The engine's text readers and JSON writers share one rule for what a UTF-8 sequence is (DIAG.10a): the JSON parser
// refuses a string holding anything else, and the JSON writers replace anything else with U+FFFD, so a document they
// write is valid JSON whatever bytes they were handed. Header-only, no allocation.

#include <crd/core/types.hpp>

namespace crd
{

// The length (1 to 4) of the well-formed UTF-8 sequence that starts at `p`, with `n` bytes available there, or 0 when
// the bytes there are not one: a continuation byte where a sequence starts, a sequence cut short, an overlong form, an
// encoded surrogate (U+D800 to U+DFFF) or a code point past U+10FFFF.
[[nodiscard]] constexpr usize utf8_sequence_length(const u8* p, usize n) noexcept
{
    if (n == 0U)
    {
        return 0U;
    }
    const u8 lead = p[0];
    if (lead < 0x80U)
    {
        return 1U;
    }
    usize length = 0U;
    u8    low    = 0x80U; // the range of the byte after the lead; every later byte is 0x80 to 0xBF
    u8    high   = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU)
    {
        length = 2U;
    }
    else if (lead >= 0xE0U && lead <= 0xEFU)
    {
        length = 3U;
        if (lead == 0xE0U)
        {
            low = 0xA0U; // below is an overlong form
        }
        else if (lead == 0xEDU)
        {
            high = 0x9FU; // above is a surrogate
        }
    }
    else if (lead >= 0xF0U && lead <= 0xF4U)
    {
        length = 4U;
        if (lead == 0xF0U)
        {
            low = 0x90U; // below is an overlong form
        }
        else if (lead == 0xF4U)
        {
            high = 0x8FU; // above is past U+10FFFF
        }
    }
    else
    {
        return 0U; // a continuation byte, an overlong lead (0xC0, 0xC1) or a lead past U+10FFFF
    }
    if (n < length || p[1] < low || p[1] > high)
    {
        return 0U;
    }
    for (usize i = 2U; i < length; ++i)
    {
        if (p[i] < 0x80U || p[i] > 0xBFU)
        {
            return 0U;
        }
    }
    return length;
}

} // namespace crd
