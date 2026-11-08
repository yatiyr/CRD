#pragma once

// What the generated known answers (tests/foundation/net/generated/*_vectors.inc) call: a message built field by field
// held to the reference interpreter's bytes, and a decoder held to its verdict on damaged bytes.

#include <crd/net/wire.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace crd::net::wire::testing
{

inline constexpr usize kMaxVectorBytes = 4096U;

inline usize from_hex(const char* text, u8* out)
{
    const auto digit = [](char c) -> u8
    {
        return static_cast<u8>((c <= '9') ? (c - '0') : (c - 'A' + 10));
    };
    const usize length = std::strlen(text);
    REQUIRE(length % 2U == 0U);
    REQUIRE(length / 2U <= kMaxVectorBytes);
    for (usize i = 0U; i < length / 2U; ++i)
    {
        out[i] = static_cast<u8>((digit(text[2U * i]) << 4U) | digit(text[(2U * i) + 1U]));
    }
    return length / 2U;
}

template <typename M> void check_value(const M& message, u16 minor, const char* hex)
{
    static u8 expected[kMaxVectorBytes];
    static u8 written[kMaxVectorBytes];
    const usize size = from_hex(hex, expected);

    usize count = 0U;
    REQUIRE(encode(message, minor, cont::Span<u8>(written), count) == WireError::Ok);
    REQUIRE(count == size);
    CHECK((size == 0U || std::memcmp(written, expected, size) == 0));
    CHECK(encoded_bytes(message, minor) == size);

    M decoded{};
    REQUIRE(decode(cont::ConstSpan<u8>(expected, size), minor, decoded) == WireError::Ok);
    CHECK(same(decoded, message));
    CHECK(same(message, decoded));

    // A decode sets every field: over the value the previous vector of this message left, it reads the same.
    static M reused{};
    REQUIRE(decode(cont::ConstSpan<u8>(expected, size), minor, reused) == WireError::Ok);
    CHECK(same(reused, message));

    // A decoded message forwards as it is, its lists read from the input.
    usize again = 0U;
    REQUIRE(encode(decoded, minor, cont::Span<u8>(written), again) == WireError::Ok);
    REQUIRE(again == size);
    CHECK((size == 0U || std::memcmp(written, expected, size) == 0));
}

template <typename M> void check_verdict(u16 writer_minor, bool accepted, const char* hex)
{
    static u8 bytes[kMaxVectorBytes];
    const usize size = from_hex(hex, bytes);
    M message{};
    const WireError error = decode(cont::ConstSpan<u8>(bytes, size), writer_minor, message);
    CHECK((error == WireError::Ok) == accepted);
}

} // namespace crd::net::wire::testing
