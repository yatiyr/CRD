#pragma once

#include <crd/core/types.hpp>

#include <cstring>
#include <functional> // std::hash fallback
#include <string_view>
#include <type_traits>

namespace crd::containers
{
// -----------------------------------------------------------------------
// Bit mixers / byte hashers
// -----------------------------------------------------------------------

// splitmix64 finalizer with a non-zero seed mix.
// The vanilla finalizer has 0 as a fixed point (hash(0) == 0), which is
// ugly even though it's not a correctness issue. We XOR a constant first
// so that hash_u64(0) is non-zero and well-distributed.
constexpr u64 hash_u64(u64 x) noexcept
{
    x ^= 0x9E3779B97F4A7C15ULL; // golden-ratio 64-bit constant
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// FNV-1a 64-bit. Not the fastest hash on Earth but stable, simple,
// header-only-friendly, and good enough for short keys (file paths,
// identifiers). We can swap in xxhash/wyhash later when profiling demands.
constexpr u64 fnv1a_64(const void* data, usize n) noexcept
{
    constexpr u64 kFnvOffset = 0xcbf29ce484222325ULL;
    constexpr u64 kFnvPrime = 0x00000100000001B3ULL;
    const u8* bytes = static_cast<const u8*>(data);
    u64 h = kFnvOffset;
    for (usize i = 0; i < n; ++i)
    {
        h ^= static_cast<u64>(bytes[i]);
        h *= kFnvPrime;
    }
    return h;
}

// Convenience wrappers
inline u64 hash_string(const char* s, usize n) noexcept
{
    return fnv1a_64(s, n);
}

inline u64 hash_string(std::string_view sv) noexcept
{
    return fnv1a_64(sv.data(), sv.size());
}

// -----------------------------------------------------------------------
// DefaultHash<T> — generic dispatch
// -----------------------------------------------------------------------
// Specialisations below cover the common engine types; everything else
// falls back to std::hash<T>. HashMap<K,V> uses DefaultHash<K> by default.
// -----------------------------------------------------------------------

template <typename T, typename = void> struct DefaultHash
{
    u64 operator()(const T& v) const noexcept { return static_cast<u64>(std::hash<T>{}(v)); }
};

template <> struct DefaultHash<u8>
{
    u64 operator()(u8 x) const noexcept { return hash_u64(x); }
};
template <> struct DefaultHash<u16>
{
    u64 operator()(u16 x) const noexcept { return hash_u64(x); }
};
template <> struct DefaultHash<u32>
{
    u64 operator()(u32 x) const noexcept { return hash_u64(x); }
};
template <> struct DefaultHash<u64>
{
    u64 operator()(u64 x) const noexcept { return hash_u64(x); }
};
template <> struct DefaultHash<i8>
{
    u64 operator()(i8 x) const noexcept { return hash_u64(static_cast<u64>(x)); }
};
template <> struct DefaultHash<i16>
{
    u64 operator()(i16 x) const noexcept { return hash_u64(static_cast<u64>(x)); }
};
template <> struct DefaultHash<i32>
{
    u64 operator()(i32 x) const noexcept { return hash_u64(static_cast<u64>(x)); }
};
template <> struct DefaultHash<i64>
{
    u64 operator()(i64 x) const noexcept { return hash_u64(static_cast<u64>(x)); }
};

template <> struct DefaultHash<const char*>
{
    u64 operator()(const char* s) const noexcept { return hash_string(s, s ? std::char_traits<char>::length(s) : 0); }
};

template <> struct DefaultHash<std::string_view>
{
    u64 operator()(std::string_view sv) const noexcept { return hash_string(sv); }
};

// Pointer hash: feed the bit pattern through splitmix.
template <typename T> struct DefaultHash<T*>
{
    u64 operator()(T* p) const noexcept { return hash_u64(reinterpret_cast<u64>(p)); }
};

// ---- Heterogeneous hash for String --------------------------------
// Defined here as a forward-declared specialisation; the real impl lives
// alongside String itself (string.hpp) where the type is complete. We
// keep the *declaration* here so DefaultHash<String> resolves without
// every TU needing to include <crd/containers/string.hpp>.
//
// Contract: every overload below MUST produce the same u64 for the same
// bytes. v1c's HashMap heterogeneous lookup relies on this — calling
// `m.find(StringView{...})` on a `HashMap<String, V>` invokes the
// (StringView) overload, which must agree with the (String) overload
// used to place the entry in the first place.
class String;
template <> struct DefaultHash<String>
{
    u64 operator()(const String& s) const noexcept;    // defined in string.hpp
    u64 operator()(std::string_view sv) const noexcept // homogeneous via FNV-1a
    {
        return hash_string(sv);
    }
    u64 operator()(const char* cstr) const noexcept
    {
        return hash_string(cstr ? std::string_view{cstr} : std::string_view{});
    }
};
} // namespace crd::containers
