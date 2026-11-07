#pragma once

// decimal.hpp — a request's unsigned decimal number, for every verb that takes one.

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceridc
{
// `text` as an unsigned decimal: digits only, no sign or space, at most 2^64 - 1.
[[nodiscard]] inline bool read_decimal(crd::containers::StringView text, crd::u64& out) noexcept
{
    if (text.empty() || text.size() > 20U)
    {
        return false;
    }
    crd::u64 value = 0U;
    for (const char c : text)
    {
        const crd::u64 digit = static_cast<crd::u64>(c - '0');
        if (c < '0' || c > '9' || value > (~crd::u64{0U} - digit) / 10U)
        {
            return false;
        }
        value = (value * 10U) + digit;
    }
    out = value;
    return true;
}
} // namespace crd::ceridc
