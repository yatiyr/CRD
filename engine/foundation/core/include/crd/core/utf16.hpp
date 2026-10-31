#pragma once

// crd-core -- UTF-16 and its transcoding to and from UTF-8 (the Unicode Standard, section 3.9).
//
// Windows keeps its text in UTF-16: file and module names, the command line, the environment, adapter descriptions.
// The engine's text is UTF-8, so every crossing between the two goes through these functions: one rule for an unpaired
// surrogate (U+FFFD) and one for a byte that starts no well-formed UTF-8 sequence (U+FFFD for that byte, as the JSON
// writers replace it), the same on every platform, so the Linux lanes test what Windows runs. Header-only, constexpr,
// no allocation: safe before main and in a crash handler. A code unit is any two-byte type (char16_t, Windows'
// wchar_t, u16), so a Win32 buffer is read and written in place, without a cast between unrelated types.

#include <crd/core/types.hpp>
#include <crd/core/utf8.hpp>

namespace crd
{

inline constexpr u32 kReplacementCharacter = 0xFFFDU;

// Decodes the code point at `p` (with `n` > 0 units available there) into `code_point` and returns the units it took:
// 2 for a surrogate pair, 1 otherwise. An unpaired surrogate decodes as U+FFFD and takes 1.
template <typename Unit> [[nodiscard]] constexpr usize utf16_decode(const Unit* p, usize n, u32& code_point) noexcept
{
    static_assert(sizeof(Unit) == 2U, "a UTF-16 code unit is two bytes");
    const u32 lead = static_cast<u32>(static_cast<u16>(p[0]));
    if (lead < 0xD800U || lead > 0xDFFFU)
    {
        code_point = lead;
        return 1U;
    }
    if (lead <= 0xDBFFU && n >= 2U)
    {
        const u32 trail = static_cast<u32>(static_cast<u16>(p[1]));
        if (trail >= 0xDC00U && trail <= 0xDFFFU)
        {
            code_point = 0x10000U + ((lead - 0xD800U) << 10U) + (trail - 0xDC00U);
            return 2U;
        }
    }
    code_point = kReplacementCharacter;
    return 1U;
}

// Writes `code_point` as UTF-8 into out[0, 4) and returns its length (1 to 4). A surrogate or a value past U+10FFFF,
// which no UTF-8 sequence encodes, is written as U+FFFD.
[[nodiscard]] constexpr usize utf8_encode(u32 code_point, u8* out) noexcept
{
    if ((code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU)
    {
        code_point = kReplacementCharacter;
    }
    if (code_point < 0x80U)
    {
        out[0] = static_cast<u8>(code_point);
        return 1U;
    }
    if (code_point < 0x800U)
    {
        out[0] = static_cast<u8>(0xC0U | (code_point >> 6U));
        out[1] = static_cast<u8>(0x80U | (code_point & 0x3FU));
        return 2U;
    }
    if (code_point < 0x10000U)
    {
        out[0] = static_cast<u8>(0xE0U | (code_point >> 12U));
        out[1] = static_cast<u8>(0x80U | ((code_point >> 6U) & 0x3FU));
        out[2] = static_cast<u8>(0x80U | (code_point & 0x3FU));
        return 3U;
    }
    out[0] = static_cast<u8>(0xF0U | (code_point >> 18U));
    out[1] = static_cast<u8>(0x80U | ((code_point >> 12U) & 0x3FU));
    out[2] = static_cast<u8>(0x80U | ((code_point >> 6U) & 0x3FU));
    out[3] = static_cast<u8>(0x80U | (code_point & 0x3FU));
    return 4U;
}

// Writes `code_point` as UTF-16 into out[0, 2) and returns its length (1 or 2), U+FFFD for a surrogate or a value past
// U+10FFFF.
template <typename Unit> [[nodiscard]] constexpr usize utf16_encode(u32 code_point, Unit* out) noexcept
{
    static_assert(sizeof(Unit) == 2U, "a UTF-16 code unit is two bytes");
    if ((code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU)
    {
        code_point = kReplacementCharacter;
    }
    if (code_point < 0x10000U)
    {
        out[0] = static_cast<Unit>(code_point);
        return 1U;
    }
    code_point -= 0x10000U;
    out[0] = static_cast<Unit>(0xD800U + (code_point >> 10U));
    out[1] = static_cast<Unit>(0xDC00U + (code_point & 0x3FFU));
    return 2U;
}

// The UTF-8 form of the n units at `in`, written into out[0, capacity) while it fits whole: returns the bytes the
// whole form takes. It fits when that is at most `capacity`; otherwise `out` holds the code points that fitted before
// the first that did not. No NUL is written. out may be null when capacity is 0, to measure.
template <typename Unit>
[[nodiscard]] constexpr usize utf16_to_utf8(const Unit* in, usize n, u8* out, usize capacity) noexcept
{
    usize length = 0U;
    usize i      = 0U;
    while (i < n)
    {
        u32 code_point = 0U;
        i += utf16_decode(in + i, n - i, code_point);
        u8          bytes[4] = {}; // NOLINT(modernize-avoid-c-arrays): one sequence
        const usize size     = utf8_encode(code_point, bytes);
        if (length + size <= capacity) // once one does not fit, no later one does: length counts it
        {
            for (usize b = 0U; b < size; ++b)
            {
                out[length + b] = bytes[b];
            }
        }
        length += size;
    }
    return length;
}

// The UTF-16 form of the n bytes at `in`, written into out[0, capacity) while it fits whole: returns the units the
// whole form takes, as utf16_to_utf8 does. Each byte that starts no well-formed sequence (utf8_sequence_length)
// becomes one U+FFFD.
template <typename Unit>
[[nodiscard]] constexpr usize utf8_to_utf16(const u8* in, usize n, Unit* out, usize capacity) noexcept
{
    usize length = 0U;
    usize i      = 0U;
    while (i < n)
    {
        u32         code_point = kReplacementCharacter;
        const usize taken      = utf8_decode(in + i, n - i, code_point);
        i += taken == 0U ? 1U : taken;
        Unit        units[2] = {}; // NOLINT(modernize-avoid-c-arrays): one code point
        const usize size     = utf16_encode(code_point, units);
        if (length + size <= capacity) // once one does not fit, no later one does: length counts it
        {
            for (usize u = 0U; u < size; ++u)
            {
                out[length + u] = units[u];
            }
        }
        length += size;
    }
    return length;
}

} // namespace crd
