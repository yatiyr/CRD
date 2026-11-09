// crd-net -- the capsule protocol (4.2a; docs/design/network-foundation.md#http3).

#include <crd/net/http_capsule.hpp>
#include <crd/net/quic_header.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
[[nodiscard]] usize varint_bytes_of(u8 first) noexcept
{
    return usize{1} << (first >> 6U);
}

constexpr char kFieldName[] = "capsule-protocol";
} // namespace

usize HttpCapsuleReader::next(cont::ConstSpan<u8> bytes, HttpCapsuleChunk& chunk, bool& got) noexcept
{
    got = false;
    usize used = 0U;
    if (!m_in_value)
    {
        // Gathers the two integers one byte at a time until both are whole.
        for (;;)
        {
            usize need = 1U;
            if (m_head_bytes != 0U)
            {
                const usize type_bytes = varint_bytes_of(m_head[0]);
                need = type_bytes;
                if (m_head_bytes >= type_bytes)
                {
                    need = type_bytes + 1U;
                    if (m_head_bytes > type_bytes)
                    {
                        need = type_bytes + varint_bytes_of(m_head[type_bytes]);
                    }
                }
            }
            if (m_head_bytes == need && m_head_bytes > 1U && m_head_bytes > varint_bytes_of(m_head[0]))
            {
                break;
            }
            if (used == bytes.size())
            {
                return used;
            }
            m_head[m_head_bytes++] = bytes[used++];
        }
        cont::WireReader in(cont::ConstSpan<u8>(m_head, m_head_bytes));
        static_cast<void>(take_quic_varint(in, m_type));
        static_cast<void>(take_quic_varint(in, m_length));
        m_head_bytes = 0U;
        m_offset = 0U;
        m_in_value = true;
    }
    const u64 left = m_length - m_offset;
    const u64 available = bytes.size() - used;
    if (left != 0U && available == 0U)
    {
        return used;
    }
    const usize count = static_cast<usize>((left < available) ? left : available);
    chunk = HttpCapsuleChunk{};
    chunk.type = m_type;
    chunk.length = m_length;
    chunk.offset = m_offset;
    chunk.value = bytes.subspan(used, count);
    m_offset += count;
    chunk.complete = m_offset == m_length;
    m_in_value = !chunk.complete;
    got = true;
    return used + count;
}

usize http_capsule_header_bytes(u64 type, u64 length) noexcept
{
    return quic_varint_bytes(type) + quic_varint_bytes(length);
}

void put_http_capsule_header(cont::WireWriter& out, u64 type, u64 length) noexcept
{
    put_quic_varint(out, type);
    put_quic_varint(out, length);
}

bool http_capsule_protocol(cont::ConstSpan<HttpField> fields) noexcept
{
    const HttpField* found = nullptr;
    for (const HttpField& field : fields)
    {
        if (field.name.size() == sizeof(kFieldName) - 1U &&
            std::memcmp(field.name.data(), kFieldName, sizeof(kFieldName) - 1U) == 0)
        {
            if (found != nullptr)
            {
                return false; // a list, no Boolean
            }
            found = &field;
        }
    }
    if (found == nullptr)
    {
        return false;
    }
    const cont::ConstSpan<u8> value = found->value;
    if (value.size() < 2U || value[0] != '?' || value[1] != '1')
    {
        return false;
    }
    return value.size() == 2U || value[2] == ';';
}

} // namespace crd::net
