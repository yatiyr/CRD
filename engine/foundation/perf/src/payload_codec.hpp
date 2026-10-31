#pragma once
// crd-perf -- internal little-endian payload helpers shared by the bundle sections' codecs (the build identity, the
// hardware identity and the flight record): appenders, and a bounds-checked cursor over untrusted bytes whose reads
// fail, and stay failed, past the end. Header-only inline; no owning STL containers.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <cstring>

namespace crd::perf::detail
{
namespace cont = crd::containers;

inline void put_uint(cont::Array<crd::u8>& out, crd::u64 v, crd::u32 width)
{
    for (crd::u32 i = 0U; i < width; ++i)
    {
        out.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU));
    }
}

inline void put_u8(cont::Array<crd::u8>& out, crd::u8 v)
{
    out.push_back(v);
}

inline void put_u16(cont::Array<crd::u8>& out, crd::u16 v)
{
    put_uint(out, v, 2U);
}

inline void put_u32(cont::Array<crd::u8>& out, crd::u32 v)
{
    put_uint(out, v, 4U);
}

inline void put_u64(cont::Array<crd::u8>& out, crd::u64 v)
{
    put_uint(out, v, 8U);
}

inline void put_bytes(cont::Array<crd::u8>& out, const char* p, crd::usize n)
{
    for (crd::usize i = 0U; i < n; ++i)
    {
        out.push_back(static_cast<crd::u8>(p[i]));
    }
}

// A u16 length, then that many bytes of `s`, clipped to `bound` (at most 0xFFFF).
inline void put_field(cont::Array<crd::u8>& out, cont::StringView s, crd::usize bound)
{
    const crd::usize n = s.size() < bound ? s.size() : bound;
    put_u16(out, static_cast<crd::u16>(n));
    put_bytes(out, s.data(), n);
}

struct Cursor
{
    cont::ConstSpan<crd::u8> bytes;
    crd::usize at = 0U;
    bool ok = true;

    bool need(crd::usize n) noexcept
    {
        if (!ok || bytes.size() - at < n)
        {
            ok = false;
        }
        return ok;
    }

    crd::u64 uint(crd::u32 width) noexcept
    {
        if (!need(width))
        {
            return 0U;
        }
        crd::u64 v = 0U;
        for (crd::u32 i = 0U; i < width; ++i)
        {
            v |= static_cast<crd::u64>(bytes[at + i]) << (8U * i);
        }
        at += width;
        return v;
    }

    crd::u8 u8() noexcept { return static_cast<crd::u8>(uint(1U)); }
    crd::u16 u16() noexcept { return static_cast<crd::u16>(uint(2U)); }
    crd::u32 u32() noexcept { return static_cast<crd::u32>(uint(4U)); }
    crd::u64 u64() noexcept { return uint(8U); }

    // Copy `n` bytes into a fixed buffer of `cap` bytes (n must leave room for the NUL).
    bool text(char* dst, crd::u32 cap, crd::u16 n) noexcept
    {
        if (n >= cap)
        {
            ok = false;
            return false;
        }
        if (!need(n))
        {
            return false;
        }
        if (n > 0U)
        {
            std::memcpy(dst, bytes.data() + at, n);
        }
        dst[n] = '\0';
        at += n;
        return true;
    }

    // A u16 length then that many bytes into `out`; refused when the length is over `bound` or past the payload.
    bool field(cont::String& out, crd::usize bound) noexcept
    {
        const crd::usize n = u16();
        if (!ok || n > bound || !need(n))
        {
            ok = false;
            return false;
        }
        out.clear();
        out.append(cont::StringView{reinterpret_cast<const char*>(bytes.data() + at), n});
        at += n;
        return true;
    }

    [[nodiscard]] bool done() const noexcept { return ok && at == bytes.size(); }
};

} // namespace crd::perf::detail
