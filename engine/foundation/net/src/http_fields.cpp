// crd-net -- the field coding HPACK and QPACK share (4.2a; docs/design/network-foundation.md#qpack).

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/http_fields.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
constexpr u32 kSymbols = 257U;
constexpr u16 kEos = 256U;
constexpr u8 kMaxCodeBits = 30U;
constexpr u8 kFastBits = 9U;

// RFC 7541 appendix B's code is canonical: codes of a length follow in symbol order, so its lengths define it.
constexpr u8 kCodeBits[kSymbols] = {
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 30, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 6,  10, 10, 12, 13, 6,  8,  11, 10, 10, 8,  11, 8,  6,  6,  6,  5,  5,  5,  6,
    6,  6,  6,  6,  6,  6,  7,  8,  15, 6,  12, 10, 13, 6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
    7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,  7,  8,  13, 19, 13, 14, 6,  15, 5,  6,  5,  6,  5,  6,  6,
    6,  5,  7,  7,  6,  6,  6,  5,  6,  7,  6,  5,  5,  6,  7,  7,  7,  7,  7,  15, 11, 14, 13, 28, 20, 22,
    20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23, 24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23,
    22, 23, 23, 24, 22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23, 21, 21, 22, 21, 23, 22,
    23, 23, 20, 22, 22, 22, 23, 22, 22, 23, 26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
    19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27, 20, 24, 20, 21, 22, 21, 21, 23, 22, 22,
    25, 25, 24, 24, 26, 23, 26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26, 30,
};

struct HuffmanTables
{
    u32 codes[kSymbols] = {};
    u16 order[kSymbols] = {};                // the symbols by code
    u16 first[kMaxCodeBits + 1U] = {};       // the place in `order` of a length's first code
    u32 first_code[kMaxCodeBits + 1U] = {};  // that code
    u64 limit[kMaxCodeBits + 1U] = {};       // one past a length's last code, as 32 bits aligned to the top
    u16 fast[usize{1} << kFastBits] = {};    // by the next bits: symbol | bits << 9, for a code of kFastBits or fewer
};

constexpr HuffmanTables build_tables() noexcept
{
    HuffmanTables tables;
    u32 code = 0U;
    u16 place = 0U;
    for (u8 bits = 1U; bits <= kMaxCodeBits; ++bits)
    {
        code <<= 1U;
        tables.first[bits] = place;
        tables.first_code[bits] = code;
        for (u16 symbol = 0U; symbol < kSymbols; ++symbol)
        {
            if (kCodeBits[symbol] != bits)
            {
                continue;
            }
            tables.codes[symbol] = code;
            tables.order[place] = symbol;
            ++place;
            if (bits <= kFastBits)
            {
                const u32 start = code << (kFastBits - bits);
                const u32 count = u32{1} << (kFastBits - bits);
                for (u32 slot = start; slot < start + count; ++slot)
                {
                    tables.fast[slot] = static_cast<u16>(symbol | (u32{bits} << kFastBits));
                }
            }
            ++code;
        }
        tables.limit[bits] = u64{code} << (32U - bits);
    }
    return tables;
}

constexpr HuffmanTables kTables = build_tables();

void write_code(cont::WireWriter& out, cont::ConstSpan<u8> text) noexcept
{
    u64 held = 0U;
    u32 held_bits = 0U;
    for (const u8 byte : text)
    {
        held = (held << kCodeBits[byte]) | kTables.codes[byte];
        held_bits += kCodeBits[byte];
        while (held_bits >= 8U)
        {
            held_bits -= 8U;
            out.put_u8(static_cast<u8>(held >> held_bits));
        }
    }
    if (held_bits != 0U)
    {
        const u32 padding = 8U - held_bits;
        out.put_u8(static_cast<u8>((held << padding) | ((u64{1} << padding) - 1U)));
    }
}
} // namespace

usize http_int_bytes(u64 value, u8 prefix_bits) noexcept
{
    const u64 limit = (u64{1} << prefix_bits) - 1U;
    if (value < limit)
    {
        return 1U;
    }
    usize bytes = 2U;
    for (u64 rest = value - limit; rest >= 128U; rest >>= 7U)
    {
        ++bytes;
    }
    return bytes;
}

void put_http_int(cont::WireWriter& out, u8 flags, u8 prefix_bits, u64 value) noexcept
{
    const u64 limit = (u64{1} << prefix_bits) - 1U;
    if (value < limit)
    {
        out.put_u8(static_cast<u8>(flags | value));
        return;
    }
    out.put_u8(static_cast<u8>(flags | limit));
    u64 rest = value - limit;
    while (rest >= 128U)
    {
        out.put_u8(static_cast<u8>((rest & 0x7FU) | 0x80U));
        rest >>= 7U;
    }
    out.put_u8(static_cast<u8>(rest));
}

HttpDecode take_http_int(cont::WireReader& in, u8 first, u8 prefix_bits, u64& value) noexcept
{
    const u64 limit = (u64{1} << prefix_bits) - 1U;
    value = first & limit;
    if (value < limit)
    {
        return HttpDecode::Ok;
    }
    for (u32 count = 0U; count + 1U < kHttpMaxIntegerBytes; ++count)
    {
        u8 byte = 0U;
        if (!in.take_u8(byte))
        {
            return HttpDecode::NeedMore;
        }
        const u32 shift = 7U * count;
        const u64 digit = byte & 0x7FU;
        if (digit > ((kHttpMaxInteger - value) >> shift))
        {
            return HttpDecode::Invalid;
        }
        value += digit << shift;
        if ((byte & 0x80U) == 0U)
        {
            return HttpDecode::Ok;
        }
    }
    return HttpDecode::Invalid;
}

usize http_huffman_bytes(cont::ConstSpan<u8> text) noexcept
{
    u64 bits = 0U;
    for (const u8 byte : text)
    {
        bits += kCodeBits[byte];
    }
    return static_cast<usize>((bits + 7U) / 8U);
}

void http_huffman_encode(cont::ConstSpan<u8> text, cont::Span<u8> out) noexcept
{
    cont::WireWriter writer(out);
    write_code(writer, text);
}

bool http_huffman_decode(cont::ConstSpan<u8> coded, cont::Span<u8> out, usize& decoded) noexcept
{
    decoded = 0U;
    u64 held = 0U; // aligned to the top
    u32 held_bits = 0U;
    usize at = 0U;
    while (true)
    {
        while (held_bits <= 56U && at < coded.size())
        {
            held |= u64{coded[at]} << (56U - held_bits);
            held_bits += 8U;
            ++at;
        }
        if (held_bits == 0U)
        {
            return true;
        }
        u32 symbol = kEos;
        u32 bits = kMaxCodeBits;
        const u16 fast = kTables.fast[held >> (64U - kFastBits)];
        if (fast != 0U)
        {
            symbol = fast & ((u32{1} << kFastBits) - 1U);
            bits = u32{fast} >> kFastBits;
        }
        else
        {
            const u64 top = held >> 32U;
            bits = kFastBits + 1U;
            while (bits < kMaxCodeBits && top >= kTables.limit[bits])
            {
                ++bits;
            }
            symbol = kTables.order[kTables.first[bits] + (static_cast<u32>(top >> (32U - bits)) -
                                                          kTables.first_code[bits])];
        }
        if (bits > held_bits)
        {
            // The input ended inside a code: what is left must be padding, a prefix of EOS.
            const u64 ones = ~u64{0} << (64U - held_bits);
            return held_bits < 8U && (held & ones) == ones;
        }
        if (symbol == kEos)
        {
            return false;
        }
        if (decoded < out.size())
        {
            out[decoded] = static_cast<u8>(symbol);
        }
        ++decoded;
        held <<= bits;
        held_bits -= bits;
    }
}

usize http_string_bytes(cont::ConstSpan<u8> text, u8 prefix_bits) noexcept
{
    const usize huffman = http_huffman_bytes(text);
    const usize bytes = (huffman < text.size()) ? huffman : text.size();
    return http_int_bytes(bytes, prefix_bits) + bytes;
}

void put_http_string(cont::WireWriter& out, u8 flags, u8 prefix_bits, cont::ConstSpan<u8> text) noexcept
{
    const usize huffman = http_huffman_bytes(text);
    if (huffman >= text.size())
    {
        put_http_int(out, flags, prefix_bits, text.size());
        out.put_bytes(text);
        return;
    }
    put_http_int(out, static_cast<u8>(flags | (u32{1} << prefix_bits)), prefix_bits, huffman);
    write_code(out, text);
}

HttpDecode take_http_string(cont::WireReader& in, u8 first, u8 prefix_bits, usize max_coded,
                            cont::ConstSpan<u8>& coded, bool& huffman) noexcept
{
    huffman = ((u32{first} >> prefix_bits) & 1U) != 0U;
    u64 bytes = 0U;
    const HttpDecode length = take_http_int(in, first, prefix_bits, bytes);
    if (length != HttpDecode::Ok)
    {
        return length;
    }
    if (bytes > max_coded)
    {
        return HttpDecode::Invalid;
    }
    return in.take_bytes(static_cast<usize>(bytes), coded) ? HttpDecode::Ok : HttpDecode::NeedMore;
}

bool http_equal(cont::ConstSpan<u8> left, cont::ConstSpan<u8> right) noexcept
{
    return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

bool http_field_sensitive(cont::ConstSpan<u8> name) noexcept
{
    return http_equal(name, http_text("authorization")) || http_equal(name, http_text("proxy-authorization")) ||
           http_equal(name, http_text("cookie")) || http_equal(name, http_text("set-cookie"));
}

u64 http_entry_size(const HttpFieldTable::Entry& entry) noexcept
{
    return u64{entry.name_bytes} + entry.value_bytes + kHttpFieldOverhead;
}

HttpStaticMatch http_find_static(cont::ConstSpan<HttpStaticField> table, cont::ConstSpan<u8> name,
                                 cont::ConstSpan<u8> value) noexcept
{
    HttpStaticMatch match;
    for (usize index = 0U; index < table.size(); ++index)
    {
        if (!http_equal(table[index].name, name))
        {
            continue;
        }
        if (http_equal(table[index].value, value))
        {
            match.index = static_cast<i32>(index);
            match.exact = true;
            return match;
        }
        if (match.index < 0)
        {
            match.index = static_cast<i32>(index);
        }
    }
    return match;
}

// -- the dynamic table -----------------------------------------------------------------------------------------------

bool HttpFieldTable::init(memory::IAllocator& allocator, u32 max_capacity) noexcept
{
    m_max_capacity = max_capacity;
    m_max_entries = static_cast<u32>(max_capacity / kHttpFieldOverhead);
    if (m_max_entries == 0U)
    {
        return true; // too small for even an empty entry: legal, and it holds nothing
    }
    m_bytes = memory::try_construct_array<u8>(allocator, max_capacity);
    m_entries = memory::try_construct_array<Entry>(allocator, m_max_entries);
    return m_bytes != nullptr && m_entries != nullptr;
}

void HttpFieldTable::release(memory::IAllocator& allocator) noexcept
{
    memory::destroy_array(allocator, m_bytes, m_max_capacity);
    memory::destroy_array(allocator, m_entries, m_max_entries);
    m_bytes = nullptr;
    m_entries = nullptr;
}

const HttpFieldTable::Entry& HttpFieldTable::entry(u64 absolute) const noexcept
{
    return m_entries[absolute % m_max_entries];
}

void HttpFieldTable::make_room(usize entry_bytes) noexcept
{
    while (m_size + entry_bytes > m_capacity)
    {
        m_size -= static_cast<u32>(http_entry_size(entry(m_dropped)));
        ++m_dropped;
    }
}

void HttpFieldTable::set_capacity(u32 capacity) noexcept
{
    m_capacity = capacity;
    make_room(0U);
}

void HttpFieldTable::write(cont::ConstSpan<u8> bytes) noexcept
{
    if (bytes.empty())
    {
        return;
    }
    const usize first = (bytes.size() < m_max_capacity - m_tail) ? bytes.size() : m_max_capacity - m_tail;
    std::memcpy(m_bytes + m_tail, bytes.data(), first);
    if (first < bytes.size())
    {
        std::memcpy(m_bytes, bytes.data() + first, bytes.size() - first);
    }
    m_tail = static_cast<u32>((m_tail + bytes.size()) % m_max_capacity);
}

void HttpFieldTable::write_within(u32 at, u32 bytes) noexcept
{
    for (u32 index = 0U; index < bytes; ++index)
    {
        m_bytes[m_tail] = m_bytes[at];
        m_tail = (m_tail + 1U) % m_max_capacity;
        at = (at + 1U) % m_max_capacity;
    }
}

void HttpFieldTable::push(u32 at, u32 name_bytes, u32 value_bytes) noexcept
{
    m_entries[m_inserted % m_max_entries] = Entry{at, name_bytes, value_bytes};
    ++m_inserted;
    m_size += name_bytes + value_bytes + static_cast<u32>(kHttpFieldOverhead);
}

void HttpFieldTable::append(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value) noexcept
{
    const u32 at = m_tail;
    write(name);
    write(value);
    push(at, static_cast<u32>(name.size()), static_cast<u32>(value.size()));
}

void HttpFieldTable::append_named(Entry source, cont::ConstSpan<u8> value) noexcept
{
    const u32 at = m_tail;
    write_within(source.at, source.name_bytes);
    write(value);
    push(at, source.name_bytes, static_cast<u32>(value.size()));
}

void HttpFieldTable::append_copy(Entry source) noexcept
{
    const u32 at = m_tail;
    write_within(source.at, source.name_bytes + source.value_bytes);
    push(at, source.name_bytes, source.value_bytes);
}

void HttpFieldTable::read(u32 at, cont::Span<u8> out) const noexcept
{
    if (out.empty())
    {
        return;
    }
    const usize first = (out.size() < m_max_capacity - at) ? out.size() : m_max_capacity - at;
    std::memcpy(out.data(), m_bytes + at, first);
    if (first < out.size())
    {
        std::memcpy(out.data() + first, m_bytes, out.size() - first);
    }
}

bool HttpFieldTable::equals(u32 at, cont::ConstSpan<u8> bytes) const noexcept
{
    if (bytes.empty())
    {
        return true;
    }
    const usize first = (bytes.size() < m_max_capacity - at) ? bytes.size() : m_max_capacity - at;
    if (std::memcmp(bytes.data(), m_bytes + at, first) != 0)
    {
        return false;
    }
    return first == bytes.size() || std::memcmp(bytes.data() + first, m_bytes, bytes.size() - first) == 0;
}

void HttpFieldTable::copy_name(const Entry& entry, cont::Span<u8> out) const noexcept
{
    read(entry.at, out.subspan(0U, entry.name_bytes));
}

void HttpFieldTable::copy_value(const Entry& entry, cont::Span<u8> out) const noexcept
{
    read((entry.at + entry.name_bytes) % m_max_capacity, out.subspan(0U, entry.value_bytes));
}

bool HttpFieldTable::name_is(const Entry& entry, cont::ConstSpan<u8> name) const noexcept
{
    return entry.name_bytes == name.size() && equals(entry.at, name);
}

bool HttpFieldTable::value_is(const Entry& entry, cont::ConstSpan<u8> value) const noexcept
{
    return entry.value_bytes == value.size() && equals((entry.at + entry.name_bytes) % m_max_capacity, value);
}

bool HttpFieldTable::find_field(const HttpField& field, u64& absolute) const noexcept
{
    for (u64 index = m_inserted; index > m_dropped; --index)
    {
        const Entry& candidate = entry(index - 1U);
        if (name_is(candidate, field.name) && value_is(candidate, field.value))
        {
            absolute = index - 1U;
            return true;
        }
    }
    return false;
}

bool HttpFieldTable::find_name(cont::ConstSpan<u8> name, u64& absolute) const noexcept
{
    for (u64 index = m_inserted; index > m_dropped; --index)
    {
        if (name_is(entry(index - 1U), name))
        {
            absolute = index - 1U;
            return true;
        }
    }
    return false;
}

// -- a decoder's output ----------------------------------------------------------------------------------------------

cont::Span<u8> HttpFieldOutput::reserve(usize bytes) noexcept
{
    if (!overflow && text.size() - used >= bytes)
    {
        const cont::Span<u8> place = text.subspan(used, bytes);
        used += bytes;
        return place;
    }
    overflow = true;
    if (spill.size() - spilled < bytes)
    {
        oversize = true;
        return {};
    }
    const cont::Span<u8> place = spill.subspan(spilled, bytes);
    spilled += bytes;
    return place;
}

bool HttpFieldOutput::literal(cont::ConstSpan<u8> coded, bool huffman, cont::ConstSpan<u8>& out) noexcept
{
    if (!huffman)
    {
        const cont::Span<u8> place = reserve(coded.size());
        if (place.size() == coded.size() && !coded.empty())
        {
            std::memcpy(place.data(), coded.data(), coded.size());
        }
        out = place;
        return true;
    }
    const cont::Span<u8> room = overflow ? spill.subspan(spilled) : text.subspan(used);
    usize decoded = 0U;
    if (!http_huffman_decode(coded, room, decoded))
    {
        return false;
    }
    if (decoded <= room.size())
    {
        out = reserve(decoded);
        return true;
    }
    const cont::Span<u8> place = reserve(decoded); // the room ran out: on to the spill, if it holds this
    if (place.size() == decoded && decoded != 0U)
    {
        (void)http_huffman_decode(coded, place, decoded);
    }
    out = place;
    return true;
}

void HttpFieldOutput::table_name(const HttpFieldTable& table, const HttpFieldTable::Entry& entry,
                                 cont::ConstSpan<u8>& name) noexcept
{
    const cont::Span<u8> place = reserve(entry.name_bytes);
    if (place.size() == entry.name_bytes)
    {
        table.copy_name(entry, place);
    }
    name = place;
}

void HttpFieldOutput::table_value(const HttpFieldTable& table, const HttpFieldTable::Entry& entry,
                                  cont::ConstSpan<u8>& value) noexcept
{
    const cont::Span<u8> place = reserve(entry.value_bytes);
    if (place.size() == entry.value_bytes)
    {
        table.copy_value(entry, place);
    }
    value = place;
}

void HttpFieldOutput::emit(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value, bool never_index) noexcept
{
    if (count == fields.size())
    {
        overflow = true;
    }
    if (!overflow)
    {
        fields[count] = HttpField{name, value, never_index};
        ++count;
    }
}

} // namespace crd::net
