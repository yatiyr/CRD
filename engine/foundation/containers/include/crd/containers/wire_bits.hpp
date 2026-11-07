#pragma once

// crd-containers -- bit-packed fields over byte spans: the bounded writer and reader under every generated Cerid wire
// format (NET.3, crd/net/wire.hpp). The first field bit is the most significant bit of the first byte, and a field
// written at a byte boundary with a whole number of bytes reads as wire_bytes.hpp's big-endian integer, so a packet
// diagram reads as an RFC's does.
//
//   BitWriter  packs fields into a span. A message's size is known only as it is written, so running out of room is a
//              reported outcome, not a contract violation: overflowed() goes true and stays true, nothing past the span
//              is written and the writer's position keeps counting, so it also measures.
//   BitReader  takes fields from received bytes, or from a whole-byte part of them (sub_bytes). Every take reports
//              whether the bits held the whole field and consumes nothing when they did not. Taken bytes view the
//              input. Padding bits are checked to be zero, so one message has one encoding.

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <bit>
#include <cstring>
#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h> // _byteswap_uint64
#endif

namespace crd::containers
{
static_assert(std::endian::native == std::endian::little, "the word loads below swap a little-endian host's bytes");

namespace wire_bits_detail
{
[[nodiscard]] CRD_FORCEINLINE u64 load_big_endian_u64(const u8* bytes) noexcept
{
    u64 value = 0U;
    std::memcpy(&value, bytes, sizeof(value));
#if defined(_MSC_VER) && !defined(__clang__)
    return _byteswap_uint64(value);
#else
    return __builtin_bswap64(value);
#endif
}

CRD_FORCEINLINE void store_big_endian_u64(u8* bytes, u64 value) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    value = _byteswap_uint64(value);
#else
    value = __builtin_bswap64(value);
#endif
    std::memcpy(bytes, &value, sizeof(value));
}
} // namespace wire_bits_detail

class BitWriter
{
public:
    explicit BitWriter(Span<u8> out) noexcept : m_out(out) {}

    // The low `bits` bits of `value`, 1 to 64; bits above them must be clear.
    CRD_FORCEINLINE void put(u64 value, u32 bits) noexcept
    {
        CRD_ASSERT(bits >= 1U && bits <= 64U);
        CRD_ASSERT(bits == 64U || (value >> bits) == 0U);
        const u32 free = 64U - m_used;
        if (bits < free)
        {
            m_pending |= value << (free - bits);
            m_used += bits;
            return;
        }
        const u32 rest = bits - free;
        m_pending |= (rest == 0U) ? value : (value >> rest);
        flush_word();
        m_pending = (rest == 0U) ? u64{0U} : (value << (64U - rest));
        m_used    = rest;
    }

    // Zero bits to the next byte boundary.
    void align() noexcept
    {
        const u32 over = static_cast<u32>(bit_position() & 7U);
        if (over != 0U)
        {
            put(0U, 8U - over);
        }
    }

    // Whole bytes at a boundary (after align()).
    void put_bytes(ConstSpan<u8> bytes) noexcept
    {
        CRD_ASSERT((bit_position() & 7U) == 0U);
        flush_bytes();
        const usize count = bytes.size();
        if (count == 0U)
        {
            return;
        }
        if (m_overflowed || m_out.size() - m_at < count)
        {
            m_overflowed = true;
            m_past += count;
            return;
        }
        std::memcpy(m_out.data() + m_at, bytes.data(), count);
        m_at += count;
    }

    // The room left past what has been written, at a boundary (after align()), for a nested writer to fill. Empty once
    // a write has not fitted. claim() then says how many of its bytes now count as written here.
    [[nodiscard]] Span<u8> remaining() noexcept
    {
        CRD_ASSERT((bit_position() & 7U) == 0U);
        flush_bytes();
        return m_overflowed ? Span<u8>{} : m_out.subspan(m_at);
    }

    void claim(usize count) noexcept
    {
        CRD_ASSERT(m_used == 0U);
        if (m_overflowed || m_out.size() - m_at < count)
        {
            m_overflowed = true;
            m_past += count;
            return;
        }
        m_at += count;
    }

    // The bits written so far, whether or not they fitted.
    [[nodiscard]] u64 bit_position() const noexcept
    {
        return (static_cast<u64>(m_at + m_past) * 8U) + m_used;
    }

    [[nodiscard]] bool overflowed() const noexcept
    {
        return m_overflowed;
    }

    // Pads with zero bits to a byte boundary and stores what is pending: the bytes the message takes. Further writes
    // continue from that boundary.
    usize finish() noexcept
    {
        align();
        flush_bytes();
        return m_at + m_past;
    }

private:
    void flush_word() noexcept
    {
        if (!m_overflowed && m_at + sizeof(u64) <= m_out.size())
        {
            wire_bits_detail::store_big_endian_u64(m_out.data() + m_at, m_pending);
            m_at += sizeof(u64);
            return;
        }
        m_overflowed = true;
        m_past += sizeof(u64);
    }

    // At a byte boundary: stores the pending whole bytes.
    void flush_bytes() noexcept
    {
        const usize count = m_used / 8U;
        if (m_overflowed || m_out.size() - m_at < count)
        {
            m_overflowed = true;
            m_past += count;
        }
        else
        {
            for (usize i = 0U; i < count; ++i)
            {
                m_out[m_at + i] = static_cast<u8>(m_pending >> (56U - (8U * i)));
            }
            m_at += count;
        }
        m_pending = 0U;
        m_used    = 0U;
    }

    Span<u8> m_out;
    usize    m_at         = 0U; // bytes stored
    usize    m_past       = 0U; // bytes that did not fit
    u64      m_pending    = 0U; // the next bits, from the top
    u32      m_used       = 0U; // how many of them, below 64
    bool     m_overflowed = false;
};

class BitReader
{
public:
    BitReader() noexcept = default;

    explicit BitReader(ConstSpan<u8> bytes) noexcept : m_data(bytes.data()), m_end(static_cast<u64>(bytes.size()) * 8U)
    {
    }

    [[nodiscard]] u64 bits_left() const noexcept
    {
        return m_end - m_at;
    }

    [[nodiscard]] u64 bit_position() const noexcept
    {
        return m_at;
    }

    // The next `bits` bits, 1 to 64.
    [[nodiscard]] CRD_FORCEINLINE bool take(u32 bits, u64& value) noexcept
    {
        CRD_ASSERT(bits >= 1U && bits <= 64U);
        if (bits_left() < bits)
        {
            return false;
        }
        value = read_at(m_at, bits);
        m_at += bits;
        return true;
    }

    // To the next byte boundary; false, consuming nothing, when a padding bit is set.
    [[nodiscard]] bool align() noexcept
    {
        const u32 over = static_cast<u32>(m_at & 7U);
        if (over == 0U)
        {
            return true;
        }
        const u32 padding = 8U - over; // the end is a byte boundary, so the padding is there
        if (read_at(m_at, padding) != 0U)
        {
            return false;
        }
        m_at += padding;
        return true;
    }

    // `count` bytes at a boundary (after align()), viewing the input.
    [[nodiscard]] bool take_bytes(usize count, ConstSpan<u8>& bytes) noexcept
    {
        CRD_ASSERT((m_at & 7U) == 0U);
        if ((bits_left() / 8U) < count)
        {
            return false;
        }
        bytes = ConstSpan<u8>(m_data + (m_at / 8U), count);
        m_at += static_cast<u64>(count) * 8U;
        return true;
    }

    // The next `count` bytes at a boundary as a reader of their own, consumed from this one.
    [[nodiscard]] bool sub_bytes(usize count, BitReader& inner) noexcept
    {
        ConstSpan<u8> bytes;
        if (!take_bytes(count, bytes))
        {
            return false;
        }
        inner = BitReader(bytes);
        return true;
    }

    // Past `bits` bits, without reading them.
    [[nodiscard]] bool skip(u64 bits) noexcept
    {
        if (bits_left() < bits)
        {
            return false;
        }
        m_at += bits;
        return true;
    }

private:
    [[nodiscard]] CRD_FORCEINLINE u64 read_at(u64 at, u32 bits) const noexcept
    {
        const u64 byte  = at / 8U;
        const u32 shift = static_cast<u32>(at & 7U);
        if (byte + sizeof(u64) <= m_end / 8U)
        {
            const u64 word = wire_bits_detail::load_big_endian_u64(m_data + byte) << shift;
            if (bits + shift <= 64U)
            {
                return word >> (64U - bits);
            }
            const u32 spill = bits + shift - 64U; // at most 7 bits, in the next byte
            const u64 next  = static_cast<u64>(m_data[byte + sizeof(u64)]) >> (8U - spill);
            return (word >> (64U - bits)) | next;
        }
        // Within the last eight bytes of the input: one byte at a time, never loading past the end.
        u64 value = 0U;
        u32 want  = bits;
        u64 from  = at;
        while (want != 0U)
        {
            const u32 inside = 8U - static_cast<u32>(from & 7U);
            const u32 piece  = (want < inside) ? want : inside;
            const u32 low    = inside - piece;
            const u64 part   = (static_cast<u64>(m_data[from / 8U]) >> low) & ((u64{1} << piece) - 1U);
            value            = (value << piece) | part;
            want -= piece;
            from += piece;
        }
        return value;
    }

    const u8* m_data = nullptr;
    u64       m_at   = 0U; // in bits
    u64       m_end  = 0U;
};

} // namespace crd::containers
