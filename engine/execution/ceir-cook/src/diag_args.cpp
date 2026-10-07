#include "diag_args.hpp"

#include <algorithm>

namespace crd::ceir::cook::detail
{
bool parse_u64(containers::StringView text, crd::u64 max, crd::u64& out) noexcept
{
    if (text.empty() || text.size() > 20U)
    {
        return false;
    }
    crd::u64 v = 0U;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        const auto digit = static_cast<crd::u64>(c - '0');
        if (v > (max - digit) / 10U)
        {
            return false;
        }
        v = v * 10U + digit;
    }
    out = v;
    return true;
}

bool parse_i64(containers::StringView text, crd::i64& out) noexcept
{
    const bool                   negative = !text.empty() && text[0] == '-';
    const containers::StringView digits   = negative ? text.substr(1U) : text;
    constexpr crd::u64           max_positive = 9223372036854775807ULL;
    crd::u64                     magnitude    = 0U;
    if (!parse_u64(digits, negative ? max_positive + 1U : max_positive, magnitude))
    {
        return false;
    }
    out = negative ? static_cast<crd::i64>(0U - magnitude) : static_cast<crd::i64>(magnitude);
    return true;
}

bool valid_entry_name(containers::StringView name, crd::u32 max_bytes) noexcept
{
    if (name.empty() || name.size() > max_bytes)
    {
        return false;
    }
    return std::ranges::all_of(name,
                               [](char c)
                               {
                                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                          c == '_' || c == '.';
                               });
}
} // namespace crd::ceir::cook::detail
