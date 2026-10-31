#pragma once

// FNV-1a (Fowler, Noll, Vo), 32- and 64-bit: the engine's one implementation (capability hash.fnv1a,
// cmake/capability-owners.json). It lives in crd-core so every module can reach it, crd-memory included.
//
// Every form continues from a running state `h`, so a value built from several fields needs no scratch buffer:
// fnv1a_64(b, nb, fnv1a_64(a, na)) == fnv1a_64(a ++ b). The text form is usable in constant expressions (a `void*` cannot
// be read there). The integer folds feed a value's bytes least significant first by shifts, so an identity that is
// persisted (cooked assets, stable ids) is the same on every host. `fnv1a_64_word` is one FNV-1a step over a whole
// 64-bit word: a cheap combiner for runtime keys (hash-consing, memo and cache keys, change signatures), not FNV-1a over
// the word's bytes, so it never names a persisted identity.

#include <crd/core/types.hpp>

#include <type_traits>

namespace crd
{

inline constexpr u64 kFnv1a64Offset = 0xCBF29CE484222325ULL;
inline constexpr u64 kFnv1a64Prime = 0x00000100000001B3ULL;
inline constexpr u32 kFnv1a32Offset = 0x811C9DC5U;
inline constexpr u32 kFnv1a32Prime = 0x01000193U;

[[nodiscard]] constexpr u64 fnv1a_64(const char* text, usize n, u64 h = kFnv1a64Offset) noexcept
{
    for (usize i = 0; i < n; ++i)
    {
        h = (h ^ static_cast<u64>(static_cast<u8>(text[i]))) * kFnv1a64Prime;
    }
    return h;
}

[[nodiscard]] inline u64 fnv1a_64(const void* data, usize n, u64 h = kFnv1a64Offset) noexcept
{
    const u8* bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < n; ++i)
    {
        h = (h ^ static_cast<u64>(bytes[i])) * kFnv1a64Prime;
    }
    return h;
}

[[nodiscard]] constexpr u32 fnv1a_32(const char* text, usize n, u32 h = kFnv1a32Offset) noexcept
{
    for (usize i = 0; i < n; ++i)
    {
        h = (h ^ static_cast<u32>(static_cast<u8>(text[i]))) * kFnv1a32Prime;
    }
    return h;
}

[[nodiscard]] inline u32 fnv1a_32(const void* data, usize n, u32 h = kFnv1a32Offset) noexcept
{
    const u8* bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < n; ++i)
    {
        h = (h ^ static_cast<u32>(bytes[i])) * kFnv1a32Prime;
    }
    return h;
}

// The sizeof(T) bytes of integer `v`, least significant first.
template <typename T>
[[nodiscard]] constexpr u64 fnv1a_64_le(T v, u64 h) noexcept
{
    static_assert(std::is_integral_v<T>, "fnv1a_64_le folds an integer's bytes");
    const u64 bits = static_cast<u64>(v);
    for (usize i = 0; i < sizeof(T); ++i)
    {
        h = (h ^ ((bits >> (8U * i)) & 0xFFU)) * kFnv1a64Prime;
    }
    return h;
}

template <typename T>
[[nodiscard]] constexpr u32 fnv1a_32_le(T v, u32 h) noexcept
{
    static_assert(std::is_integral_v<T>, "fnv1a_32_le folds an integer's bytes");
    const u64 bits = static_cast<u64>(v);
    for (usize i = 0; i < sizeof(T); ++i)
    {
        h = (h ^ static_cast<u32>((bits >> (8U * i)) & 0xFFU)) * kFnv1a32Prime;
    }
    return h;
}

[[nodiscard]] constexpr u64 fnv1a_64_word(u64 v, u64 h) noexcept
{
    return (h ^ v) * kFnv1a64Prime;
}

} // namespace crd
