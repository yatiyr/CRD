#pragma once

// crd-crypto -- byte-span helpers the units of src/ share: the overlap test every operation with an output runs, byte
// equality and order for public values (keys, identifiers; secrets compare with constant_time_equal), and the scratch
// buffer that lays out a labelled input (HPKE's and MLS's labels before a secret or a message), and the hexadecimal
// text that names a store's keys in a vault or device (the protectors and devices of the key store). Private to
// engine/foundation/crypto/src; no provider header.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>

#include <algorithm>
#include <cstdint>

namespace crd::crypto::detail
{
// Whether two buffers share a byte.
[[nodiscard]] inline bool overlaps(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    if (a.empty() || b.empty())
    {
        return false;
    }
    const auto a_begin = reinterpret_cast<std::uintptr_t>(a.data());
    const auto b_begin = reinterpret_cast<std::uintptr_t>(b.data());
    return a_begin < b_begin + b.size() && b_begin < a_begin + a.size();
}

// Whether two public byte strings are equal, and their lexicographic order.
[[nodiscard]] inline bool same_bytes(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

[[nodiscard]] inline bool bytes_less(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

// Whether two of `values` are the same bytes (sorted in place).
[[nodiscard]] inline bool repeats(cont::Array<cont::ConstSpan<u8>>& values) noexcept
{
    cont::ConstSpan<u8>* begin = values.data();
    cont::ConstSpan<u8>* end   = values.data() + values.size();
    std::sort(begin, end, bytes_less);
    return std::adjacent_find(begin, end, same_bytes) != end;
}

// Zeroes the outputs of a refused operation and returns its error: no partial or unauthenticated byte leaves a call.
inline CryptoError refuse(CryptoError error, cont::Span<u8> first, cont::Span<u8> second = {}) noexcept
{
    secure_zero(first);
    secure_zero(second);
    return error;
}

// `bytes` as lowercase hexadecimal, two characters each, written from `out` without a terminator; returns the end.
template <typename Char> Char* hex_into(cont::ConstSpan<u8> bytes, Char* out) noexcept
{
    constexpr char digits[] = "0123456789abcdef";
    for (const u8 byte : bytes)
    {
        *out++ = static_cast<Char>(digits[byte >> 4U]);
        *out++ = static_cast<Char>(digits[byte & 0x0FU]);
    }
    return out;
}

// A string literal's bytes without its terminator: a KDF or signature label.
template <usize N>
[[nodiscard]] cont::ConstSpan<u8> label_of(const char (&text)[N]) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text), N - 1U);
}

// The bytes of one labelled input, which may hold secrets: inline up to kInlineBytes (every per-message derivation,
// so the message path allocates nothing), from the default allocator beyond (a group context or a signed message on
// the handshake path), and zeroed when released either way.
class LabelBuffer
{
public:
    static constexpr usize kInlineBytes = 256U;

    explicit LabelBuffer(usize bytes)
    {
        if (bytes > kInlineBytes)
        {
            m_heap.resize(bytes);
        }
        m_bytes = bytes;
    }
    ~LabelBuffer() noexcept { secure_zero(span()); }
    LabelBuffer(const LabelBuffer&)            = delete;
    LabelBuffer& operator=(const LabelBuffer&) = delete;
    LabelBuffer(LabelBuffer&&)                 = delete;
    LabelBuffer& operator=(LabelBuffer&&)      = delete;

    [[nodiscard]] cont::Span<u8> span() noexcept
    {
        return cont::Span<u8>(m_bytes > kInlineBytes ? m_heap.data() : m_inline, m_bytes);
    }

private:
    u8              m_inline[kInlineBytes] = {};
    cont::Array<u8> m_heap;
    usize           m_bytes = 0U;
};
} // namespace crd::crypto::detail
