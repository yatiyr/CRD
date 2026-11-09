#pragma once

// crd-net tests: the owned text the HTTP field tests build and compare (the engine's String and Array, with the few
// joins and cuts those tests want).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

#include <cstdio>
#include <string_view>

namespace crd::net::testing
{
namespace cont = crd::containers;

using Text = cont::String;

// A field line: name and value.
struct TextPair
{
    TextPair() = default;
    TextPair(const char* name, const char* value) : first(name), second(value) {}
    TextPair(Text name, Text value) : first(static_cast<Text&&>(name)), second(static_cast<Text&&>(value)) {}

    Text first;
    Text second;
};

inline Text operator+(std::string_view left, std::string_view right)
{
    Text joined(left);
    joined.append(right);
    return joined;
}

// The `count` bytes of `text` from `at`, as far as they go.
inline Text slice(std::string_view text, usize at, usize count = ~usize{0})
{
    return Text(text.substr(at, count));
}

inline void erase_front(Text& text, usize count)
{
    text = slice(text, count);
}

inline Text number_text(u64 value)
{
    char digits[24] = {};
    const int length = std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(value));
    return Text(digits, static_cast<usize>(length));
}

inline cont::ConstSpan<u8> text_view(std::string_view text)
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

} // namespace crd::net::testing
