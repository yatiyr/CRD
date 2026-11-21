#include <crd/core/parse_integer.hpp>

namespace crd
{

bool parse_u64(const char* first, const char* last, u64 max, u64& out) noexcept
{
    if (first == nullptr || last <= first || last - first > 20)
    {
        return false;
    }
    u64 value = 0U;
    for (const char* at = first; at != last; ++at)
    {
        if (*at < '0' || *at > '9')
        {
            return false;
        }
        const auto digit = static_cast<u64>(*at - '0');
        if (digit > max || value > (max - digit) / 10U)
        {
            return false;
        }
        value = value * 10U + digit;
    }
    out = value;
    return true;
}

bool parse_i64(const char* first, const char* last, i64& out) noexcept
{
    constexpr u64 max_positive = 9223372036854775807ULL;
    const bool negative = first != nullptr && last > first && *first == '-';
    u64 magnitude = 0U;
    if (!parse_u64(negative ? first + 1 : first, last, negative ? max_positive + 1U : max_positive, magnitude))
    {
        return false;
    }
    out = negative ? static_cast<i64>(0U - magnitude) : static_cast<i64>(magnitude);
    return true;
}

} // namespace crd
