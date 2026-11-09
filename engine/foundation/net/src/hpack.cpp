// crd-net -- HPACK, HTTP/2's field compression (4.2a; docs/design/network-foundation.md#hpack).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/hpack.hpp>

namespace crd::net
{
namespace
{
constexpr u8 kIndexed = 0x80U;
constexpr u8 kIncremental = 0x40U;
constexpr u8 kSizeUpdate = 0x20U;
constexpr u8 kNeverIndexed = 0x10U;
constexpr u8 kIndexedPrefix = 7U;
constexpr u8 kIncrementalPrefix = 6U;
constexpr u8 kSizeUpdatePrefix = 5U;
constexpr u8 kLiteralPrefix = 4U;
constexpr u8 kStringPrefix = 7U;
constexpr u32 kMaxUpdates = 2U; // section 4.2: the smallest size since the last block, then the final one

cont::ConstSpan<HttpStaticField> static_table() noexcept
{
    // RFC 7541 appendix A; entry i of this array is index i + 1 on the wire.
    static const HttpStaticField kTable[kHpackStaticEntries] = {
        {http_text(":authority"), http_text("")},
        {http_text(":method"), http_text("GET")},
        {http_text(":method"), http_text("POST")},
        {http_text(":path"), http_text("/")},
        {http_text(":path"), http_text("/index.html")},
        {http_text(":scheme"), http_text("http")},
        {http_text(":scheme"), http_text("https")},
        {http_text(":status"), http_text("200")},
        {http_text(":status"), http_text("204")},
        {http_text(":status"), http_text("206")},
        {http_text(":status"), http_text("304")},
        {http_text(":status"), http_text("400")},
        {http_text(":status"), http_text("404")},
        {http_text(":status"), http_text("500")},
        {http_text("accept-charset"), http_text("")},
        {http_text("accept-encoding"), http_text("gzip, deflate")},
        {http_text("accept-language"), http_text("")},
        {http_text("accept-ranges"), http_text("")},
        {http_text("accept"), http_text("")},
        {http_text("access-control-allow-origin"), http_text("")},
        {http_text("age"), http_text("")},
        {http_text("allow"), http_text("")},
        {http_text("authorization"), http_text("")},
        {http_text("cache-control"), http_text("")},
        {http_text("content-disposition"), http_text("")},
        {http_text("content-encoding"), http_text("")},
        {http_text("content-language"), http_text("")},
        {http_text("content-length"), http_text("")},
        {http_text("content-location"), http_text("")},
        {http_text("content-range"), http_text("")},
        {http_text("content-type"), http_text("")},
        {http_text("cookie"), http_text("")},
        {http_text("date"), http_text("")},
        {http_text("etag"), http_text("")},
        {http_text("expect"), http_text("")},
        {http_text("expires"), http_text("")},
        {http_text("from"), http_text("")},
        {http_text("host"), http_text("")},
        {http_text("if-match"), http_text("")},
        {http_text("if-modified-since"), http_text("")},
        {http_text("if-none-match"), http_text("")},
        {http_text("if-range"), http_text("")},
        {http_text("if-unmodified-since"), http_text("")},
        {http_text("last-modified"), http_text("")},
        {http_text("link"), http_text("")},
        {http_text("location"), http_text("")},
        {http_text("max-forwards"), http_text("")},
        {http_text("proxy-authenticate"), http_text("")},
        {http_text("proxy-authorization"), http_text("")},
        {http_text("range"), http_text("")},
        {http_text("referer"), http_text("")},
        {http_text("refresh"), http_text("")},
        {http_text("retry-after"), http_text("")},
        {http_text("server"), http_text("")},
        {http_text("set-cookie"), http_text("")},
        {http_text("strict-transport-security"), http_text("")},
        {http_text("transfer-encoding"), http_text("")},
        {http_text("user-agent"), http_text("")},
        {http_text("vary"), http_text("")},
        {http_text("via"), http_text("")},
        {http_text("www-authenticate"), http_text("")},
    };
    return cont::ConstSpan<HttpStaticField>(kTable, kHpackStaticEntries);
}

// A field whose value is rarely seen twice, by its static index: indexing it only pushes useful entries out.
bool one_off(i32 static_index) noexcept
{
    switch (static_index + 1)
    {
        case 4:  // :path
        case 21: // age
        case 28: // content-length
        case 34: // etag
        case 40: // if-modified-since
        case 41: // if-none-match
        case 46: // location
            return true;
        default:
            return false;
    }
}

u64 max_of(u64 a, u64 b) noexcept
{
    return (a > b) ? a : b;
}
} // namespace

// -- the decoder -----------------------------------------------------------------------------------------------------

HpackDecoder::~HpackDecoder()
{
    release();
}

void HpackDecoder::release() noexcept
{
    if (m_allocator != nullptr)
    {
        m_table.release(*m_allocator);
        memory::destroy_array(*m_allocator, m_scratch, m_scratch_bytes);
        m_scratch = nullptr;
        m_allocator = nullptr;
    }
}

bool HpackDecoder::init(memory::IAllocator& allocator, u32 max_table_size) noexcept
{
    if (m_allocator != nullptr || max_table_size > kHpackMaxTableSize)
    {
        return false;
    }
    m_allocator = &allocator;
    m_scratch_bytes = static_cast<u32>(max_of(max_table_size, kHpackDefaultTableSize));
    m_scratch = memory::try_construct_array<u8>(allocator, m_scratch_bytes);
    if (m_scratch == nullptr || !m_table.init(allocator, m_scratch_bytes))
    {
        release();
        return false;
    }
    m_table.set_capacity(kHpackDefaultTableSize);
    return true;
}

void HpackDecoder::set_limit(u32 limit) noexcept
{
    m_limit = limit;
    if (limit < m_table.capacity())
    {
        m_owed_at_most = (m_update_owed && m_owed_at_most < limit) ? m_owed_at_most : limit;
        m_update_owed = true;
    }
}

bool HpackDecoder::size_update(cont::WireReader& in, u8 first, u32& updates) noexcept
{
    u64 size = 0U;
    if (take_http_int(in, first, kSizeUpdatePrefix, size) != HttpDecode::Ok || updates == kMaxUpdates)
    {
        return false;
    }
    if (size > (m_update_owed ? m_owed_at_most : m_limit))
    {
        return false;
    }
    m_table.set_capacity(static_cast<u32>(size));
    m_update_owed = false;
    ++updates;
    return true;
}

bool HpackDecoder::resolve(u64 index, NameRef& name, cont::ConstSpan<u8>* fixed_value) const noexcept
{
    if (index == 0U)
    {
        return false;
    }
    if (index <= kHpackStaticEntries)
    {
        const HttpStaticField& entry = static_table()[static_cast<usize>(index - 1U)];
        name.fixed = entry.name;
        if (fixed_value != nullptr)
        {
            *fixed_value = entry.value;
        }
        return true;
    }
    const u64 back = index - kHpackStaticEntries;
    if (back > m_table.inserted() || !m_table.live(m_table.inserted() - back))
    {
        return false;
    }
    name.absolute = m_table.inserted() - back;
    name.dynamic = true;
    return true;
}

void HpackDecoder::insert(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value, bool oversize) noexcept
{
    const u64 entry_bytes = u64{name.size()} + value.size() + kHttpFieldOverhead;
    if (oversize || entry_bytes > m_table.capacity())
    {
        m_table.make_room(m_table.capacity()); // section 4.4: the table empties and nothing is added
        return;
    }
    m_table.make_room(static_cast<usize>(entry_bytes));
    m_table.append(name, value);
}

HpackStatus HpackDecoder::decode(cont::ConstSpan<u8> block, cont::Span<HttpField> fields, cont::Span<u8> text,
                                 usize& count) noexcept
{
    count = 0U;
    if (m_failed)
    {
        return HpackStatus::Failed;
    }
    const auto refused = [this]() noexcept
    {
        m_failed = true;
        return HpackStatus::Failed;
    };

    cont::WireReader in(block);
    HttpFieldOutput out{fields, text, cont::Span<u8>(m_scratch, m_scratch_bytes)};
    u32 updates = 0U;
    bool opened = false; // a field line was read: an update can no longer come
    while (!in.done())
    {
        u8 first = 0U;
        if (!in.take_u8(first))
        {
            return refused();
        }
        if ((first & 0xE0U) == kSizeUpdate)
        {
            if (opened || !size_update(in, first, updates))
            {
                return refused();
            }
            continue;
        }
        opened = true;
        out.begin_line();

        NameRef ref;
        cont::ConstSpan<u8> name;
        cont::ConstSpan<u8> value;
        if ((first & kIndexed) != 0U)
        {
            u64 index = 0U;
            if (take_http_int(in, first, kIndexedPrefix, index) != HttpDecode::Ok || !resolve(index, ref, &value))
            {
                return refused();
            }
            name = ref.fixed;
            if (ref.dynamic)
            {
                const HttpFieldTable::Entry& entry = m_table.entry(ref.absolute);
                out.table_name(m_table, entry, name);
                out.table_value(m_table, entry, value);
            }
            out.emit(name, value, false);
            continue;
        }

        const bool incremental = (first & kIncremental) != 0U;
        const bool never_index = !incremental && (first & kNeverIndexed) != 0U;
        u64 index = 0U;
        if (take_http_int(in, first, incremental ? kIncrementalPrefix : kLiteralPrefix, index) != HttpDecode::Ok)
        {
            return refused();
        }
        cont::ConstSpan<u8> coded;
        bool huffman = false;
        u8 lead = 0U;
        if (index != 0U)
        {
            if (!resolve(index, ref, nullptr))
            {
                return refused();
            }
            name = ref.fixed;
            if (ref.dynamic)
            {
                out.table_name(m_table, m_table.entry(ref.absolute), name);
            }
        }
        else if (!in.take_u8(lead) ||
                 take_http_string(in, lead, kStringPrefix, block.size(), coded, huffman) != HttpDecode::Ok ||
                 !out.literal(coded, huffman, name))
        {
            return refused();
        }
        if (!in.take_u8(lead) ||
            take_http_string(in, lead, kStringPrefix, block.size(), coded, huffman) != HttpDecode::Ok ||
            !out.literal(coded, huffman, value))
        {
            return refused();
        }
        out.emit(name, value, never_index);
        if (incremental)
        {
            insert(name, value, out.oversize);
        }
    }
    if (m_update_owed)
    {
        return refused();
    }
    if (out.overflow)
    {
        return HpackStatus::TooLarge;
    }
    count = out.count;
    return HpackStatus::Complete;
}

// -- the encoder -----------------------------------------------------------------------------------------------------

HpackEncoder::~HpackEncoder()
{
    release();
}

void HpackEncoder::release() noexcept
{
    if (m_allocator != nullptr)
    {
        m_table.release(*m_allocator);
        m_allocator = nullptr;
    }
}

bool HpackEncoder::init(memory::IAllocator& allocator, u32 table_capacity) noexcept
{
    if (m_allocator != nullptr || table_capacity > kHpackMaxTableSize)
    {
        return false;
    }
    m_allocator = &allocator;
    if (!m_table.init(allocator, table_capacity))
    {
        release();
        return false;
    }
    m_max_capacity = table_capacity;
    // The peer's decoder starts at 4096; a smaller table of ours is signalled before its first use.
    m_target = (table_capacity < kHpackDefaultTableSize) ? table_capacity : kHpackDefaultTableSize;
    m_lowest = m_target;
    m_table.set_capacity(m_target);
    return true;
}

void HpackEncoder::set_peer_limit(u32 size) noexcept
{
    const u32 usable = (size < m_max_capacity) ? size : m_max_capacity;
    m_lowest = (usable < m_lowest) ? usable : m_lowest;
    m_target = usable;
}

usize HpackEncoder::max_bytes(cont::ConstSpan<HttpField> fields) noexcept
{
    usize bytes = 2U * kHttpMaxIntegerBytes; // the size updates
    for (usize index = 0U; index < fields.size(); ++index)
    {
        const HttpField& field = fields[index];
        bytes += (kHttpMaxIntegerBytes + http_int_bytes(field.name.size(), kStringPrefix) + field.name.size() +
                  http_int_bytes(field.value.size(), kStringPrefix) + field.value.size());
    }
    return bytes;
}

void HpackEncoder::write_updates(cont::WireWriter& out) noexcept
{
    if (m_lowest < m_signalled)
    {
        put_http_int(out, kSizeUpdate, kSizeUpdatePrefix, m_lowest);
        m_signalled = m_lowest;
        m_table.set_capacity(m_lowest);
    }
    if (m_target != m_signalled)
    {
        put_http_int(out, kSizeUpdate, kSizeUpdatePrefix, m_target);
        m_signalled = m_target;
    }
    m_table.set_capacity(m_target);
    m_lowest = m_target;
}

void HpackEncoder::write_line(const HttpField& field, cont::WireWriter& out) noexcept
{
    const bool never_index = field.never_index || http_field_sensitive(field.name);
    const HttpStaticMatch match = http_find_static(static_table(), field.name, field.value);
    if (!never_index)
    {
        if (match.exact)
        {
            put_http_int(out, kIndexed, kIndexedPrefix, static_cast<u64>(match.index) + 1U);
            return;
        }
        u64 absolute = 0U;
        if (m_table.find_field(field, absolute))
        {
            put_http_int(out, kIndexed, kIndexedPrefix, kHpackStaticEntries + m_table.inserted() - absolute);
            return;
        }
    }

    u64 name_index = 0U;
    u64 named = 0U;
    if (match.index >= 0)
    {
        name_index = static_cast<u64>(match.index) + 1U;
    }
    else if (m_table.find_name(field.name, named))
    {
        name_index = kHpackStaticEntries + m_table.inserted() - named;
    }

    const u64 entry_bytes = u64{field.name.size()} + field.value.size() + kHttpFieldOverhead;
    const bool insert = !never_index && !one_off(match.index) && entry_bytes <= (u64{m_table.capacity()} * 3U) / 4U;
    if (insert)
    {
        put_http_int(out, kIncremental, kIncrementalPrefix, name_index);
    }
    else
    {
        put_http_int(out, never_index ? kNeverIndexed : u8{0U}, kLiteralPrefix, name_index);
    }
    if (name_index == 0U)
    {
        put_http_string(out, 0x00U, kStringPrefix, field.name);
    }
    put_http_string(out, 0x00U, kStringPrefix, field.value);
    if (insert)
    {
        m_table.make_room(static_cast<usize>(entry_bytes));
        m_table.append(field.name, field.value);
    }
}

usize HpackEncoder::encode(cont::ConstSpan<HttpField> fields, cont::Span<u8> out) noexcept
{
    if (out.size() < max_bytes(fields))
    {
        return 0U;
    }
    cont::WireWriter writer(out);
    write_updates(writer);
    for (usize index = 0U; index < fields.size(); ++index)
    {
        write_line(fields[index], writer);
    }
    return writer.written();
}

} // namespace crd::net
