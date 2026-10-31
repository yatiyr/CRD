#pragma once

// crd-comply (private) -- what the comply decoders share: the identifier shapes, the bounded and typed TOML reading
// that records where decoding failed first, and the monotonic adoption of a decoded generation into a store.

#include <crd/comply/generation.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/toml/toml.hpp>

#include <cstdint>

namespace crd::comply::detail
{
using toml::node;

inline constexpr usize kNotFound = ~usize{0U};

[[nodiscard]] inline bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] inline bool is_upper(char c) noexcept
{
    return c >= 'A' && c <= 'Z';
}

[[nodiscard]] inline bool is_lower_alnum(char c) noexcept
{
    return is_digit(c) || (c >= 'a' && c <= 'z');
}

[[nodiscard]] inline bool is_alnum(char c) noexcept
{
    return is_lower_alnum(c) || is_upper(c);
}

// Runs of `word` characters joined by single '.' or '-': the register's identifiers (lower case) and row names.
template <typename Word> [[nodiscard]] bool is_dotted(cont::StringView text, Word word) noexcept
{
    if (text.empty() || !word(text.front()) || !word(text.back()))
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        const bool joiner = text[i] == '.' || text[i] == '-';
        if (!joiner && !word(text[i]))
        {
            return false;
        }
        if (joiner && (text[i + 1U] == '.' || text[i + 1U] == '-'))
        {
            return false;
        }
    }
    return true;
}

// An ISO 3166-1 alpha-2 code with an optional ISO 3166-2 subdivision of one to three letters or digits (TR, US-CA,
// UA-43).
[[nodiscard]] inline bool is_region(cont::StringView text) noexcept
{
    if (text.size() < 2U || !is_upper(text[0]) || !is_upper(text[1]))
    {
        return false;
    }
    if (text.size() == 2U)
    {
        return true;
    }
    if (text[2] != '-' || text.size() < 4U || text.size() > 6U)
    {
        return false;
    }
    for (usize i = 3U; i < text.size(); ++i)
    {
        if (!is_upper(text[i]) && !is_digit(text[i]))
        {
            return false;
        }
    }
    return true;
}

// A region, INT or PLATFORM.
[[nodiscard]] inline bool is_jurisdiction(cont::StringView text) noexcept
{
    return text == "INT" || text == "PLATFORM" || is_region(text);
}

[[nodiscard]] inline bool is_https_url(cont::StringView text) noexcept
{
    constexpr cont::StringView scheme = "https://";
    if (!text.starts_with(scheme) || text.size() == scheme.size())
    {
        return false;
    }
    for (const char c : text)
    {
        if (static_cast<unsigned char>(c) <= 0x20U || c == 0x7F)
        {
            return false;
        }
    }
    const char first = text[scheme.size()];
    return first != '/' && first != ':' && first != '?' && first != '#';
}

[[nodiscard]] inline u64 digest_of(cont::StringView text) noexcept
{
    const auto* bytes = reinterpret_cast<const u8*>(text.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    return cont::xxh3_64(cont::ConstSpan<u8>(bytes, text.size()));
}

// Bounded, typed reading of one TOML document; the first refusal is the one reported.
class TomlDecoder
{
protected:
    DecodeFailure fail(ComplyError error, const node* at, cont::StringView key) noexcept
    {
        if (m_failure.error == ComplyError::None)
        {
            m_failure = DecodeFailure{error, at != nullptr ? at->source().begin.line : 0U, key};
        }
        return m_failure;
    }

    bool refuse(ComplyError error, const node* at, cont::StringView key) noexcept
    {
        (void)fail(error, at, key);
        return false;
    }

    // The parsed document, or nullptr (refused) when the text is above `bound` or not TOML.
    const node* parse(toml::parse_result& parsed, cont::StringView text, usize bound, const char* name)
    {
        if (text.size() > bound)
        {
            (void)refuse(ComplyError::TooLarge, nullptr, {});
            return nullptr;
        }
        parsed = toml::parse(text, name);
        if (!parsed)
        {
            m_failure = DecodeFailure{ComplyError::Syntax, parsed.error().line(), {}};
            return nullptr;
        }
        return &parsed.table();
    }

    template <usize N> bool known_keys(const node& table, const cont::StringView (&keys)[N]) noexcept
    {
        for (usize i = 0; i < table.size(); ++i)
        {
            bool known = false;
            for (const cont::StringView key : keys)
            {
                known = known || table.key_at(i) == key;
            }
            if (!known)
            {
                return refuse(ComplyError::UnknownKey, &table.child_at(i), {});
            }
        }
        return true;
    }

    // A required string of at most kMaxTextBytes; `may_be_empty` admits "" (an absent day or counsel day).
    bool string_at(const node& table, cont::StringView key, cont::StringView& out, bool may_be_empty = false) noexcept
    {
        const node* value = table.get(key);
        if (value == nullptr)
        {
            return refuse(ComplyError::MissingKey, &table, key);
        }
        const auto string = value->value<std::string_view>();
        if (!string.has_value())
        {
            return refuse(ComplyError::WrongType, value, key);
        }
        if (string->size() > kMaxTextBytes)
        {
            return refuse(ComplyError::TooLarge, value, key);
        }
        if (string->empty() && !may_be_empty)
        {
            return refuse(ComplyError::BadValue, value, key);
        }
        out = *string;
        return true;
    }

    bool owned(const node& table, cont::StringView key, cont::String& out)
    {
        cont::StringView view;
        if (!string_at(table, key, view))
        {
            return false;
        }
        out = cont::String(view);
        return true;
    }

    // The required string `format` equal to `expected`.
    bool format(const node& root, cont::StringView expected) noexcept
    {
        cont::StringView value;
        if (!string_at(root, "format", value))
        {
            return false;
        }
        return value == expected || refuse(ComplyError::UnsupportedFormat, root.get("format"), "format");
    }

    template <typename Enum, usize N>
    bool enumeration(const node& table, cont::StringView key, const cont::StringView (&names)[N], Enum& out) noexcept
    {
        cont::StringView view;
        if (!string_at(table, key, view))
        {
            return false;
        }
        for (usize i = 0; i < N; ++i)
        {
            if (names[i] == view)
            {
                out = static_cast<Enum>(i);
                return true;
            }
        }
        return refuse(ComplyError::BadValue, table.get(key), key);
    }

    // A day string; the empty string is "none", which only an optional day may be.
    bool day(const node& table, cont::StringView key, Day& out, bool required) noexcept
    {
        cont::StringView view;
        if (!string_at(table, key, view, true))
        {
            return false;
        }
        out = kNoDay;
        if ((view.empty() && required) || (!view.empty() && !parse_day(view, out)))
        {
            return refuse(ComplyError::BadDay, table.get(key), key);
        }
        return true;
    }

    // A required integer in [low, high]; below `low` is BadGeneration for "generation" and BadValue otherwise.
    bool integer(const node& table, cont::StringView key, i64 low, i64 high, i64& out) noexcept
    {
        const node* value = table.get(key);
        if (value == nullptr)
        {
            return refuse(ComplyError::MissingKey, &table, key);
        }
        const auto number = value->value<i64>();
        if (!number.has_value())
        {
            return refuse(ComplyError::WrongType, value, key);
        }
        if (*number < low || *number > high)
        {
            return refuse(key == "generation" ? ComplyError::BadGeneration : ComplyError::BadValue, value, key);
        }
        out = *number;
        return true;
    }

    // The positive generation number every generation carries.
    bool generation(const node& root, u64& out) noexcept
    {
        i64 number = 0;
        if (!integer(root, "generation", 1, INT64_MAX, number))
        {
            return false;
        }
        out = static_cast<u64>(number);
        return true;
    }

    bool boolean(const node& table, cont::StringView key, bool& out) noexcept
    {
        const node* value = table.get(key);
        if (value == nullptr)
        {
            return refuse(ComplyError::MissingKey, &table, key);
        }
        if (!value->is_boolean())
        {
            return refuse(ComplyError::WrongType, value, key);
        }
        out = value->value_or(false);
        return true;
    }

    // The array of tables at `key` of at most `bound` tables; absent is MissingKey unless `optional` (then nullptr
    // with no refusal: callers tell the two apart by m_failure).
    const node* tables(const node& root, cont::StringView key, u32 bound, bool optional = false) noexcept
    {
        const node* list = root.get(key);
        if (list == nullptr)
        {
            if (!optional)
            {
                (void)refuse(ComplyError::MissingKey, &root, key);
            }
            return nullptr;
        }
        if (!list->is_array_of_tables())
        {
            (void)refuse(ComplyError::WrongType, list, key);
            return nullptr;
        }
        if (list->size() > bound)
        {
            (void)refuse(ComplyError::TooLarge, list, key);
            return nullptr;
        }
        return list;
    }

    // A list of at most `bound` strings, each passing `valid`; empty only when `may_be_empty`.
    template <typename Valid>
    bool list(const node& table, cont::StringView key, Valid valid, cont::Array<cont::StringView>& out, u32 bound,
              bool may_be_empty = false) noexcept
    {
        const node* value = table.get(key);
        if (value == nullptr)
        {
            return refuse(ComplyError::MissingKey, &table, key);
        }
        if (!value->is_array())
        {
            return refuse(ComplyError::WrongType, value, key);
        }
        if (value->size() > bound)
        {
            return refuse(ComplyError::TooLarge, value, key);
        }
        if (value->size() == 0U && !may_be_empty)
        {
            return refuse(ComplyError::BadValue, value, key);
        }
        for (const node& item : *value)
        {
            const auto string = item.value<std::string_view>();
            if (!string.has_value())
            {
                return refuse(ComplyError::WrongType, &item, key);
            }
            if (string->size() > kMaxTextBytes)
            {
                return refuse(ComplyError::TooLarge, &item, key);
            }
            if (!valid(cont::StringView(*string)))
            {
                return refuse(ComplyError::BadValue, &item, key);
            }
            out.push_back(*string);
        }
        return true;
    }

    DecodeFailure m_failure;
};

// The generation held for `key`, or nullptr.
template <typename Generation, typename KeyOf>
[[nodiscard]] const Generation* find_held(const cont::Array<Generation>& held, cont::StringView key,
                                          KeyOf key_of) noexcept
{
    for (const Generation& generation : held)
    {
        if (key_of(generation) == key)
        {
            return &generation;
        }
    }
    return nullptr;
}

// Adopt `next` as its key's generation in `held`: a lower number is a rollback, the same number with other bytes a
// conflict and the same bytes already current (each refused with `held` unchanged); a key beyond `bound` is refused.
template <typename Generation, typename KeyOf>
[[nodiscard]] DecodeFailure adopt(cont::Array<Generation>& held, Generation next, KeyOf key_of, usize bound,
                                  cont::StringView key_name)
{
    for (Generation& current : held)
    {
        if (key_of(current) != key_of(next))
        {
            continue;
        }
        if (next.number < current.number)
        {
            return DecodeFailure{ComplyError::Rollback, 0U, "generation"};
        }
        if (next.number == current.number)
        {
            const bool same = next.digest == current.digest;
            return DecodeFailure{same ? ComplyError::AlreadyCurrent : ComplyError::Conflict, 0U, "generation"};
        }
        current = static_cast<Generation&&>(next);
        return DecodeFailure{};
    }
    if (held.size() >= bound)
    {
        return DecodeFailure{ComplyError::StoreFull, 0U, key_name};
    }
    held.push_back(static_cast<Generation&&>(next));
    return DecodeFailure{};
}
} // namespace crd::comply::detail
