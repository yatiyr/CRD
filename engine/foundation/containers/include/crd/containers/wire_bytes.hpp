#pragma once

// crd-containers -- big-endian integers over byte spans: the bounded reader and writer of every Cerid wire and signed
// artefact layout (crd-net's negotiation, crd-crypto's policy generations). Network byte order regardless of the host.
//
//   WireWriter  writes into a span the caller sized from the layout; writing past it is a contract violation that
//               stops the process in every build (CRD_FATAL), never a silent truncation or a write out of bounds.
//   WireReader  takes fields from received bytes; every take reports whether the bytes held the whole field and
//               consumes nothing when they did not, so a decoder refuses a short message instead of reading past it.
//               A taken byte range views the input.

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

namespace crd::containers
{
class WireWriter
{
public:
    explicit WireWriter(Span<u8> out) noexcept : m_out(out) {}

    void put_u8(u8 value) noexcept
    {
        if (m_at >= m_out.size())
        {
            CRD_FATAL("WireWriter: a write past the span its layout sized");
        }
        m_out[m_at++] = value;
    }

    void put_u16(u16 value) noexcept
    {
        put_u8(static_cast<u8>(value >> 8U));
        put_u8(static_cast<u8>(value & 0xFFU));
    }

    void put_u32(u32 value) noexcept
    {
        put_u16(static_cast<u16>(value >> 16U));
        put_u16(static_cast<u16>(value & 0xFFFFU));
    }

    void put_u64(u64 value) noexcept
    {
        put_u32(static_cast<u32>(value >> 32U));
        put_u32(static_cast<u32>(value & 0xFFFFFFFFU));
    }

    void put_bytes(ConstSpan<u8> data) noexcept
    {
        for (const u8 byte : data)
        {
            put_u8(byte);
        }
    }

    [[nodiscard]] usize written() const noexcept { return m_at; }

private:
    Span<u8> m_out;
    usize    m_at = 0U;
};

class WireReader
{
public:
    explicit WireReader(ConstSpan<u8> in) noexcept : m_in(in) {}

    [[nodiscard]] bool take_u8(u8& out) noexcept { return take(out); }
    [[nodiscard]] bool take_u16(u16& out) noexcept { return take(out); }
    [[nodiscard]] bool take_u32(u32& out) noexcept { return take(out); }
    [[nodiscard]] bool take_u64(u64& out) noexcept { return take(out); }

    // The next `count` bytes as a view of the input.
    [[nodiscard]] bool take_bytes(usize count, ConstSpan<u8>& out) noexcept
    {
        if (m_in.size() - m_at < count)
        {
            return false;
        }
        out = m_in.subspan(m_at, count);
        m_at += count;
        return true;
    }

    [[nodiscard]] usize consumed() const noexcept { return m_at; }
    [[nodiscard]] bool  done() const noexcept { return m_at == m_in.size(); }

private:
    // One big-endian integer of sizeof(T) bytes; a short input consumes nothing.
    template <typename T> [[nodiscard]] bool take(T& out) noexcept
    {
        if (m_in.size() - m_at < sizeof(T))
        {
            return false;
        }
        u64 value = 0U;
        for (usize i = 0U; i < sizeof(T); ++i)
        {
            value = (value << 8U) | m_in[m_at + i];
        }
        m_at += sizeof(T);
        out = static_cast<T>(value);
        return true;
    }

    ConstSpan<u8> m_in;
    usize         m_at = 0U;
};
} // namespace crd::containers
