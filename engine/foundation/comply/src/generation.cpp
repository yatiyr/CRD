#include "decoder.hpp"

#include <crd/comply/generation.hpp>

#include <chrono>

namespace crd::comply
{
namespace
{
[[nodiscard]] bool read_digits(cont::StringView text, usize from, usize count, u32& out) noexcept
{
    out = 0U;
    for (usize i = from; i < from + count; ++i)
    {
        if (!detail::is_digit(text[i]))
        {
            return false;
        }
        out = (out * 10U) + static_cast<u32>(text[i] - '0');
    }
    return true;
}
} // namespace

bool parse_day(cont::StringView text, Day& out) noexcept
{
    const usize size = text.size();
    if (size != 4U && size != 7U && size != 10U)
    {
        return false;
    }
    u32 year = 0U;
    u32 month = 1U;
    u32 day = 1U;
    if (!read_digits(text, 0U, 4U, year) || year < 1900U)
    {
        return false;
    }
    if (size >= 7U && (text[4] != '-' || !read_digits(text, 5U, 2U, month)))
    {
        return false;
    }
    if (size == 10U && (text[7] != '-' || !read_digits(text, 8U, 2U, day)))
    {
        return false;
    }
    const std::chrono::year_month_day civil{std::chrono::year{static_cast<int>(year)}, std::chrono::month{month},
                                            std::chrono::day{day}};
    if (!civil.ok())
    {
        return false;
    }
    out = (year * 10000U) + (month * 100U) + day;
    return true;
}

cont::StringView comply_error_name(ComplyError error) noexcept
{
    switch (error)
    {
        case ComplyError::None:
            return "none";
        case ComplyError::TooLarge:
            return "too-large";
        case ComplyError::Syntax:
            return "syntax";
        case ComplyError::UnsupportedFormat:
            return "unsupported-format";
        case ComplyError::MissingKey:
            return "missing-key";
        case ComplyError::UnknownKey:
            return "unknown-key";
        case ComplyError::WrongType:
            return "wrong-type";
        case ComplyError::BadValue:
            return "bad-value";
        case ComplyError::BadDay:
            return "bad-day";
        case ComplyError::BadGeneration:
            return "bad-generation";
        case ComplyError::DuplicateScope:
            return "duplicate-scope";
        case ComplyError::UnknownScope:
            return "unknown-scope";
        case ComplyError::DuplicateRule:
            return "duplicate-rule";
        case ComplyError::DuplicateParty:
            return "duplicate-party";
        case ComplyError::DuplicateDestination:
            return "duplicate-destination";
        case ComplyError::Rollback:
            return "rollback";
        case ComplyError::Conflict:
            return "conflict";
        case ComplyError::AlreadyCurrent:
            return "already-current";
        case ComplyError::StoreFull:
            return "store-full";
    }
    return "unknown";
}
} // namespace crd::comply
