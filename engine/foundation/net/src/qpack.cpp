// crd-net -- QPACK, HTTP/3's field compression (4.2a; docs/design/network-foundation.md#qpack).

#include <crd/core/fnv1a.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/qpack.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
constexpr u64 kDrainingFraction = 4U; // the oldest quarter of the table is close to eviction
constexpr usize kPrefixRoom = 2U * kHttpMaxIntegerBytes; // a section prefix's two integers at most

struct StaticField
{
    cont::ConstSpan<u8> name;
    cont::ConstSpan<u8> value;
};

template <usize N> cont::ConstSpan<u8> bytes_of(const char (&text)[N]) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text), N - 1U);
}

const StaticField& static_field(u64 index) noexcept
{
    // RFC 9204 appendix A.
    static const StaticField kTable[kQpackStaticEntries] = {
        {bytes_of(":authority"), bytes_of("")},
        {bytes_of(":path"), bytes_of("/")},
        {bytes_of("age"), bytes_of("0")},
        {bytes_of("content-disposition"), bytes_of("")},
        {bytes_of("content-length"), bytes_of("0")},
        {bytes_of("cookie"), bytes_of("")},
        {bytes_of("date"), bytes_of("")},
        {bytes_of("etag"), bytes_of("")},
        {bytes_of("if-modified-since"), bytes_of("")},
        {bytes_of("if-none-match"), bytes_of("")},
        {bytes_of("last-modified"), bytes_of("")},
        {bytes_of("link"), bytes_of("")},
        {bytes_of("location"), bytes_of("")},
        {bytes_of("referer"), bytes_of("")},
        {bytes_of("set-cookie"), bytes_of("")},
        {bytes_of(":method"), bytes_of("CONNECT")},
        {bytes_of(":method"), bytes_of("DELETE")},
        {bytes_of(":method"), bytes_of("GET")},
        {bytes_of(":method"), bytes_of("HEAD")},
        {bytes_of(":method"), bytes_of("OPTIONS")},
        {bytes_of(":method"), bytes_of("POST")},
        {bytes_of(":method"), bytes_of("PUT")},
        {bytes_of(":scheme"), bytes_of("http")},
        {bytes_of(":scheme"), bytes_of("https")},
        {bytes_of(":status"), bytes_of("103")},
        {bytes_of(":status"), bytes_of("200")},
        {bytes_of(":status"), bytes_of("304")},
        {bytes_of(":status"), bytes_of("404")},
        {bytes_of(":status"), bytes_of("503")},
        {bytes_of("accept"), bytes_of("*/*")},
        {bytes_of("accept"), bytes_of("application/dns-message")},
        {bytes_of("accept-encoding"), bytes_of("gzip, deflate, br")},
        {bytes_of("accept-ranges"), bytes_of("bytes")},
        {bytes_of("access-control-allow-headers"), bytes_of("cache-control")},
        {bytes_of("access-control-allow-headers"), bytes_of("content-type")},
        {bytes_of("access-control-allow-origin"), bytes_of("*")},
        {bytes_of("cache-control"), bytes_of("max-age=0")},
        {bytes_of("cache-control"), bytes_of("max-age=2592000")},
        {bytes_of("cache-control"), bytes_of("max-age=604800")},
        {bytes_of("cache-control"), bytes_of("no-cache")},
        {bytes_of("cache-control"), bytes_of("no-store")},
        {bytes_of("cache-control"), bytes_of("public, max-age=31536000")},
        {bytes_of("content-encoding"), bytes_of("br")},
        {bytes_of("content-encoding"), bytes_of("gzip")},
        {bytes_of("content-type"), bytes_of("application/dns-message")},
        {bytes_of("content-type"), bytes_of("application/javascript")},
        {bytes_of("content-type"), bytes_of("application/json")},
        {bytes_of("content-type"), bytes_of("application/x-www-form-urlencoded")},
        {bytes_of("content-type"), bytes_of("image/gif")},
        {bytes_of("content-type"), bytes_of("image/jpeg")},
        {bytes_of("content-type"), bytes_of("image/png")},
        {bytes_of("content-type"), bytes_of("text/css")},
        {bytes_of("content-type"), bytes_of("text/html; charset=utf-8")},
        {bytes_of("content-type"), bytes_of("text/plain")},
        {bytes_of("content-type"), bytes_of("text/plain;charset=utf-8")},
        {bytes_of("range"), bytes_of("bytes=0-")},
        {bytes_of("strict-transport-security"), bytes_of("max-age=31536000")},
        {bytes_of("strict-transport-security"), bytes_of("max-age=31536000; includesubdomains")},
        {bytes_of("strict-transport-security"), bytes_of("max-age=31536000; includesubdomains; preload")},
        {bytes_of("vary"), bytes_of("accept-encoding")},
        {bytes_of("vary"), bytes_of("origin")},
        {bytes_of("x-content-type-options"), bytes_of("nosniff")},
        {bytes_of("x-xss-protection"), bytes_of("1; mode=block")},
        {bytes_of(":status"), bytes_of("100")},
        {bytes_of(":status"), bytes_of("204")},
        {bytes_of(":status"), bytes_of("206")},
        {bytes_of(":status"), bytes_of("302")},
        {bytes_of(":status"), bytes_of("400")},
        {bytes_of(":status"), bytes_of("403")},
        {bytes_of(":status"), bytes_of("421")},
        {bytes_of(":status"), bytes_of("425")},
        {bytes_of(":status"), bytes_of("500")},
        {bytes_of("accept-language"), bytes_of("")},
        {bytes_of("access-control-allow-credentials"), bytes_of("FALSE")},
        {bytes_of("access-control-allow-credentials"), bytes_of("TRUE")},
        {bytes_of("access-control-allow-headers"), bytes_of("*")},
        {bytes_of("access-control-allow-methods"), bytes_of("get")},
        {bytes_of("access-control-allow-methods"), bytes_of("get, post, options")},
        {bytes_of("access-control-allow-methods"), bytes_of("options")},
        {bytes_of("access-control-expose-headers"), bytes_of("content-length")},
        {bytes_of("access-control-request-headers"), bytes_of("content-type")},
        {bytes_of("access-control-request-method"), bytes_of("get")},
        {bytes_of("access-control-request-method"), bytes_of("post")},
        {bytes_of("alt-svc"), bytes_of("clear")},
        {bytes_of("authorization"), bytes_of("")},
        {bytes_of("content-security-policy"), bytes_of("script-src 'none'; object-src 'none'; base-uri 'none'")},
        {bytes_of("early-data"), bytes_of("1")},
        {bytes_of("expect-ct"), bytes_of("")},
        {bytes_of("forwarded"), bytes_of("")},
        {bytes_of("if-range"), bytes_of("")},
        {bytes_of("origin"), bytes_of("")},
        {bytes_of("purpose"), bytes_of("prefetch")},
        {bytes_of("server"), bytes_of("")},
        {bytes_of("timing-allow-origin"), bytes_of("*")},
        {bytes_of("upgrade-insecure-requests"), bytes_of("1")},
        {bytes_of("user-agent"), bytes_of("")},
        {bytes_of("x-forwarded-for"), bytes_of("")},
        {bytes_of("x-frame-options"), bytes_of("deny")},
        {bytes_of("x-frame-options"), bytes_of("sameorigin")},
    };
    return kTable[index];
}

bool same(cont::ConstSpan<u8> left, cont::ConstSpan<u8> right) noexcept
{
    return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

struct StaticMatch
{
    i32 index = -1; // of the field if `exact`, else of the first entry with its name; -1 for neither
    bool exact = false;
};

StaticMatch find_static(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value) noexcept
{
    StaticMatch match;
    for (u32 index = 0U; index < kQpackStaticEntries; ++index)
    {
        const StaticField& field = static_field(index);
        if (!same(field.name, name))
        {
            continue;
        }
        if (same(field.value, value))
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

// RFC 9204 section 7.1: a field an attacker could guess at through the size of what it shares a table with.
bool sensitive(cont::ConstSpan<u8> name) noexcept
{
    return same(name, bytes_of("authorization")) || same(name, bytes_of("proxy-authorization")) ||
           same(name, bytes_of("cookie")) || same(name, bytes_of("set-cookie"));
}

// Where decode() puts a section. Past its storage it goes on measuring, so the section is still validated to its end.
struct Output
{
    cont::Span<HttpField> fields;
    cont::Span<u8> text;
    usize count = 0U;
    usize used = 0U;
    bool overflow = false;

    [[nodiscard]] cont::Span<u8> reserve(usize bytes) noexcept
    {
        if (overflow || text.size() - used < bytes)
        {
            overflow = true;
            return {};
        }
        const cont::Span<u8> place = text.subspan(used, bytes);
        used += bytes;
        return place;
    }

    // The literal `coded`, in `text`.
    [[nodiscard]] bool literal(cont::ConstSpan<u8> coded, bool huffman, cont::ConstSpan<u8>& out) noexcept
    {
        if (!huffman)
        {
            const cont::Span<u8> place = reserve(coded.size());
            if (!overflow && !coded.empty())
            {
                std::memcpy(place.data(), coded.data(), coded.size());
            }
            out = place;
            return true;
        }
        const cont::Span<u8> rest = overflow ? cont::Span<u8>{} : text.subspan(used);
        usize decoded = 0U;
        if (!http_huffman_decode(coded, rest, decoded))
        {
            return false;
        }
        if (decoded > rest.size())
        {
            overflow = true;
            return true;
        }
        out = reserve(decoded);
        return true;
    }

    void emit(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value, bool never_index) noexcept
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
};

void name_of(const QpackTable& table, Output& out, const QpackTable::Entry& entry, cont::ConstSpan<u8>& name) noexcept
{
    const cont::Span<u8> place = out.reserve(entry.name_bytes);
    if (!out.overflow)
    {
        table.copy_name(entry, place);
    }
    name = place;
}

void value_of(const QpackTable& table, Output& out, const QpackTable::Entry& entry, cont::ConstSpan<u8>& value) noexcept
{
    const cont::Span<u8> place = out.reserve(entry.value_bytes);
    if (!out.overflow)
    {
        table.copy_value(entry, place);
    }
    value = place;
}

// The absolute index of relative `index` below `base`, if `base` has an entry that far back.
bool below_base(u64 base, u64 index, u64& absolute) noexcept
{
    if (index >= base)
    {
        return false;
    }
    absolute = base - index - 1U;
    return true;
}

// Whether a section with Required Insert Count `required` may reference `absolute`.
bool referable(const QpackTable& table, u64 absolute, u64 required) noexcept
{
    return absolute < required && table.live(absolute);
}

u64 entry_size(const QpackTable::Entry& entry) noexcept
{
    return u64{entry.name_bytes} + entry.value_bytes + kHttpFieldOverhead;
}

u32 hash_field(const HttpField& field) noexcept
{
    const u8 separator = 0xFFU;
    u32 hash = fnv1a_32(field.name.data(), field.name.size());
    hash = fnv1a_32(&separator, 1U, hash);
    return fnv1a_32(field.value.data(), field.value.size(), hash) | 1U;
}
} // namespace

// -- the table -------------------------------------------------------------------------------------------------------

bool QpackTable::init(memory::IAllocator& allocator, u32 max_capacity) noexcept
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

void QpackTable::release(memory::IAllocator& allocator) noexcept
{
    memory::destroy_array(allocator, m_bytes, m_max_capacity);
    memory::destroy_array(allocator, m_entries, m_max_entries);
    m_bytes = nullptr;
    m_entries = nullptr;
}

const QpackTable::Entry& QpackTable::entry(u64 absolute) const noexcept
{
    return m_entries[absolute % m_max_entries];
}

void QpackTable::make_room(usize entry_bytes) noexcept
{
    while (m_size + entry_bytes > m_capacity)
    {
        m_size -= static_cast<u32>(entry_size(entry(m_dropped)));
        ++m_dropped;
    }
}

void QpackTable::set_capacity(u32 capacity) noexcept
{
    m_capacity = capacity;
    make_room(0U);
}

void QpackTable::write(cont::ConstSpan<u8> bytes) noexcept
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

void QpackTable::write_within(u32 at, u32 bytes) noexcept
{
    for (u32 index = 0U; index < bytes; ++index)
    {
        m_bytes[m_tail] = m_bytes[at];
        m_tail = (m_tail + 1U) % m_max_capacity;
        at = (at + 1U) % m_max_capacity;
    }
}

void QpackTable::push(u32 at, u32 name_bytes, u32 value_bytes) noexcept
{
    m_entries[m_inserted % m_max_entries] = Entry{at, name_bytes, value_bytes};
    ++m_inserted;
    m_size += name_bytes + value_bytes + static_cast<u32>(kHttpFieldOverhead);
}

void QpackTable::append(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value) noexcept
{
    const u32 at = m_tail;
    write(name);
    write(value);
    push(at, static_cast<u32>(name.size()), static_cast<u32>(value.size()));
}

void QpackTable::append_named(Entry source, cont::ConstSpan<u8> value) noexcept
{
    const u32 at = m_tail;
    write_within(source.at, source.name_bytes);
    write(value);
    push(at, source.name_bytes, static_cast<u32>(value.size()));
}

void QpackTable::append_copy(Entry source) noexcept
{
    const u32 at = m_tail;
    write_within(source.at, source.name_bytes + source.value_bytes);
    push(at, source.name_bytes, source.value_bytes);
}

void QpackTable::read(u32 at, cont::Span<u8> out) const noexcept
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

bool QpackTable::equals(u32 at, cont::ConstSpan<u8> bytes) const noexcept
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

void QpackTable::copy_name(const Entry& entry, cont::Span<u8> out) const noexcept
{
    read(entry.at, out.subspan(0U, entry.name_bytes));
}

void QpackTable::copy_value(const Entry& entry, cont::Span<u8> out) const noexcept
{
    read((entry.at + entry.name_bytes) % m_max_capacity, out.subspan(0U, entry.value_bytes));
}

bool QpackTable::name_is(const Entry& entry, cont::ConstSpan<u8> name) const noexcept
{
    return entry.name_bytes == name.size() && equals(entry.at, name);
}

bool QpackTable::value_is(const Entry& entry, cont::ConstSpan<u8> value) const noexcept
{
    return entry.value_bytes == value.size() && equals((entry.at + entry.name_bytes) % m_max_capacity, value);
}

// -- the decoder -----------------------------------------------------------------------------------------------------

QpackDecoder::~QpackDecoder()
{
    release();
}

void QpackDecoder::release() noexcept
{
    if (m_allocator == nullptr)
    {
        return;
    }
    m_table.release(*m_allocator);
    memory::destroy_array(*m_allocator, m_partial, m_partial_capacity);
    memory::destroy_array(*m_allocator, m_scratch, m_config.table_capacity);
    memory::destroy_array(*m_allocator, m_out, m_config.instruction_bytes);
    memory::destroy_array(*m_allocator, m_blocked, m_config.blocked_streams);
    m_partial = nullptr;
    m_scratch = nullptr;
    m_out = nullptr;
    m_blocked = nullptr;
    m_allocator = nullptr;
}

bool QpackDecoder::init(memory::IAllocator& allocator, const QpackDecoderConfig& config) noexcept
{
    if (m_allocator != nullptr || config.table_capacity > kQpackMaxTableCapacity ||
        config.blocked_streams > kQpackMaxBlockedStreams || config.instruction_bytes < kQpackMinInstructionBytes ||
        config.instruction_bytes > kQpackMaxInstructionBytes)
    {
        return false;
    }
    m_allocator = &allocator;
    m_config = config;
    // A string's Huffman code may run to 30 bits a byte; no instruction a conformant encoder sends is longer.
    m_partial_capacity = (usize{config.table_capacity} * 4U) + kQpackMinInstructionBytes;
    m_partial = memory::try_construct_array<u8>(allocator, m_partial_capacity);
    m_out = memory::try_construct_array<u8>(allocator, config.instruction_bytes);
    bool ready = m_table.init(allocator, config.table_capacity) && m_partial != nullptr && m_out != nullptr;
    if (config.table_capacity != 0U)
    {
        m_scratch = memory::try_construct_array<u8>(allocator, config.table_capacity);
        ready = ready && m_scratch != nullptr;
    }
    if (config.blocked_streams != 0U)
    {
        m_blocked = memory::try_construct_array<Blocked>(allocator, config.blocked_streams);
        ready = ready && m_blocked != nullptr;
    }
    if (!ready)
    {
        release();
    }
    return ready;
}

bool QpackDecoder::fail(u64 code) noexcept
{
    if (m_error == 0U)
    {
        m_error = code;
    }
    return false;
}

usize QpackDecoder::consume(cont::ConstSpan<u8> bytes) noexcept
{
    usize used = 0U;
    while (used < bytes.size())
    {
        cont::WireReader in(bytes.subspan(used));
        if (instruction(in) != Parse::Applied)
        {
            break;
        }
        used += in.consumed();
    }
    return used;
}

bool QpackDecoder::on_encoder_stream(cont::ConstSpan<u8> bytes) noexcept
{
    while (m_error == 0U && !bytes.empty())
    {
        if (m_partial_bytes == 0U)
        {
            bytes = bytes.subspan(consume(bytes));
            if (m_error != 0U || bytes.empty())
            {
                break;
            }
            if (bytes.size() > m_partial_capacity)
            {
                return fail(kQpackEncoderStreamError);
            }
            std::memcpy(m_partial, bytes.data(), bytes.size());
            m_partial_bytes = bytes.size();
            break;
        }
        const usize room = m_partial_capacity - m_partial_bytes;
        const usize moved = (bytes.size() < room) ? bytes.size() : room;
        if (moved == 0U)
        {
            return fail(kQpackEncoderStreamError);
        }
        std::memcpy(m_partial + m_partial_bytes, bytes.data(), moved);
        m_partial_bytes += moved;
        bytes = bytes.subspan(moved);
        const usize used = consume(cont::ConstSpan<u8>(m_partial, m_partial_bytes));
        m_partial_bytes -= used;
        if (used != 0U && m_partial_bytes != 0U)
        {
            std::memmove(m_partial, m_partial + used, m_partial_bytes);
        }
    }
    return m_error == 0U;
}

QpackDecoder::Parse QpackDecoder::instruction(cont::WireReader& in) noexcept
{
    const auto settle = [this](HttpDecode status) noexcept
    {
        if (status == HttpDecode::Invalid)
        {
            fail(kQpackEncoderStreamError);
            return Parse::Failed;
        }
        return (status == HttpDecode::NeedMore) ? Parse::NeedMore : Parse::Applied;
    };
    const auto text = [this, &in, &settle](u8 first, u8 bits, cont::ConstSpan<u8>& coded, bool& huffman) noexcept
    {
        return settle(take_http_string(in, first, bits, m_partial_capacity, coded, huffman));
    };

    u8 first = 0U;
    if (!in.take_u8(first))
    {
        return Parse::NeedMore;
    }
    if ((first & 0xC0U) != 0U)
    {
        const bool named = (first & 0x80U) != 0U;
        u64 index = 0U;
        cont::ConstSpan<u8> name;
        bool name_huffman = false;
        const Parse head = named ? settle(take_http_int(in, first, 6U, index)) : text(first, 5U, name, name_huffman);
        if (head != Parse::Applied)
        {
            return head;
        }
        // A name reference that cannot resolve is refused at once, not when its value has arrived.
        if (named && !name_exists((first & 0x40U) != 0U, index))
        {
            fail(kQpackEncoderStreamError);
            return Parse::Failed;
        }
        u8 second = 0U;
        if (!in.take_u8(second))
        {
            return Parse::NeedMore;
        }
        cont::ConstSpan<u8> value;
        bool value_huffman = false;
        const Parse tail = text(second, 7U, value, value_huffman);
        if (tail != Parse::Applied)
        {
            return tail;
        }
        const bool applied = named ? named_entry((first & 0x40U) != 0U, index, value, value_huffman)
                                   : literal_entry(name, name_huffman, value, value_huffman);
        if (!applied)
        {
            fail(kQpackEncoderStreamError);
            return Parse::Failed;
        }
        return Parse::Applied;
    }

    u64 number = 0U;
    const Parse head = settle(take_http_int(in, first, 5U, number));
    if (head != Parse::Applied)
    {
        return head;
    }
    if ((first & 0x20U) != 0U)
    {
        if (number > m_config.table_capacity)
        {
            fail(kQpackEncoderStreamError);
            return Parse::Failed;
        }
        m_table.set_capacity(static_cast<u32>(number));
        return Parse::Applied;
    }
    u64 absolute = 0U;
    if (!below_base(m_table.inserted(), number, absolute) || !m_table.live(absolute))
    {
        fail(kQpackEncoderStreamError);
        return Parse::Failed;
    }
    const QpackTable::Entry source = m_table.entry(absolute);
    m_table.make_room(static_cast<usize>(entry_size(source)));
    m_table.append_copy(source);
    return Parse::Applied;
}

bool QpackDecoder::literal_entry(cont::ConstSpan<u8> name, bool name_huffman, cont::ConstSpan<u8> value,
                                 bool value_huffman) noexcept
{
    const usize scratch = m_config.table_capacity;
    cont::ConstSpan<u8> plain_name = name;
    usize name_bytes = name.size();
    if (name_huffman)
    {
        if (!http_huffman_decode(name, cont::Span<u8>(m_scratch, scratch), name_bytes) || name_bytes > scratch)
        {
            return false;
        }
        plain_name = cont::ConstSpan<u8>(m_scratch, name_bytes);
    }
    cont::ConstSpan<u8> plain_value = value;
    usize value_bytes = value.size();
    if (value_huffman)
    {
        const usize used = name_huffman ? name_bytes : 0U; // a plain name stays in the instruction
        const cont::Span<u8> rest = cont::Span<u8>(m_scratch, scratch).subspan(used);
        if (!http_huffman_decode(value, rest, value_bytes) || value_bytes > rest.size())
        {
            return false;
        }
        plain_value = rest.subspan(0U, value_bytes);
    }
    const u64 entry_bytes = u64{name_bytes} + value_bytes + kHttpFieldOverhead;
    if (entry_bytes > m_table.capacity())
    {
        return false;
    }
    m_table.make_room(static_cast<usize>(entry_bytes));
    m_table.append(plain_name, plain_value);
    return true;
}

bool QpackDecoder::name_exists(bool from_static, u64 index) const noexcept
{
    u64 absolute = 0U;
    return from_static ? index < kQpackStaticEntries
                       : (below_base(m_table.inserted(), index, absolute) && m_table.live(absolute));
}

bool QpackDecoder::named_entry(bool from_static, u64 index, cont::ConstSpan<u8> value, bool huffman) noexcept
{
    cont::ConstSpan<u8> plain = value;
    usize value_bytes = value.size();
    if (huffman)
    {
        const cont::Span<u8> out(m_scratch, m_config.table_capacity);
        if (!http_huffman_decode(value, out, value_bytes) || value_bytes > out.size())
        {
            return false;
        }
        plain = out.subspan(0U, value_bytes);
    }
    if (from_static)
    {
        const cont::ConstSpan<u8> name = static_field(index).name;
        const u64 entry_bytes = u64{name.size()} + value_bytes + kHttpFieldOverhead;
        if (entry_bytes > m_table.capacity())
        {
            return false;
        }
        m_table.make_room(static_cast<usize>(entry_bytes));
        m_table.append(name, plain);
        return true;
    }
    const QpackTable::Entry source = m_table.entry(m_table.inserted() - index - 1U);
    const u64 entry_bytes = u64{source.name_bytes} + value_bytes + kHttpFieldOverhead;
    if (entry_bytes > m_table.capacity())
    {
        return false;
    }
    m_table.make_room(static_cast<usize>(entry_bytes));
    m_table.append_named(source, plain);
    return true;
}

bool QpackDecoder::read_prefix(cont::WireReader& in, u64& required, u64& base) const noexcept
{
    u8 first = 0U;
    u64 encoded = 0U;
    if (!in.take_u8(first) || take_http_int(in, first, 8U, encoded) != HttpDecode::Ok)
    {
        return false;
    }
    required = 0U;
    if (encoded != 0U)
    {
        const u64 max_entries = m_config.table_capacity / kHttpFieldOverhead;
        const u64 full_range = 2U * max_entries;
        if (encoded > full_range)
        {
            return false;
        }
        const u64 max_value = m_table.inserted() + max_entries;
        required = ((max_value / full_range) * full_range) + encoded - 1U;
        if (required > max_value)
        {
            if (required <= full_range)
            {
                return false;
            }
            required -= full_range;
        }
        if (required == 0U)
        {
            return false;
        }
    }
    u8 second = 0U;
    u64 delta = 0U;
    if (!in.take_u8(second) || take_http_int(in, second, 7U, delta) != HttpDecode::Ok)
    {
        return false;
    }
    if ((second & 0x80U) != 0U)
    {
        if (delta >= required)
        {
            return false;
        }
        base = required - delta - 1U;
    }
    else
    {
        base = required + delta;
    }
    return true;
}

QpackStatus QpackDecoder::block(u64 stream, u64 required) noexcept
{
    for (u32 index = 0U; index < m_blocked_count; ++index)
    {
        if (m_blocked[index].stream == stream)
        {
            m_blocked[index].required = required;
            return QpackStatus::Blocked;
        }
    }
    if (m_blocked_count == m_config.blocked_streams)
    {
        fail(kQpackDecompressionFailed);
        return QpackStatus::Failed;
    }
    m_blocked[m_blocked_count] = Blocked{stream, required};
    ++m_blocked_count;
    return QpackStatus::Blocked;
}

void QpackDecoder::forget_blocked(u64 stream) noexcept
{
    for (u32 index = 0U; index < m_blocked_count; ++index)
    {
        if (m_blocked[index].stream == stream)
        {
            for (u32 next = index + 1U; next < m_blocked_count; ++next)
            {
                m_blocked[next - 1U] = m_blocked[next];
            }
            --m_blocked_count;
            return;
        }
    }
}

void QpackDecoder::queue_instruction(u8 flags, u8 prefix_bits, u64 value) noexcept
{
    if (m_config.instruction_bytes - m_out_bytes < kHttpMaxIntegerBytes)
    {
        fail(kHttp3ExcessiveLoad);
        return;
    }
    cont::WireWriter out(cont::Span<u8>(m_out + m_out_bytes, m_config.instruction_bytes - m_out_bytes));
    put_http_int(out, flags, prefix_bits, value);
    m_out_bytes += out.written();
}

QpackStatus QpackDecoder::decode(u64 stream, cont::ConstSpan<u8> section, cont::Span<HttpField> fields,
                                 cont::Span<u8> text, usize& count) noexcept
{
    count = 0U;
    if (m_error != 0U)
    {
        return QpackStatus::Failed;
    }
    const auto refused = [this]() noexcept
    {
        fail(kQpackDecompressionFailed);
        return QpackStatus::Failed;
    };

    cont::WireReader in(section);
    u64 required = 0U;
    u64 base = 0U;
    if (!read_prefix(in, required, base))
    {
        return refused();
    }
    if (required > m_table.inserted())
    {
        return block(stream, required);
    }
    forget_blocked(stream);

    Output out{fields, text};
    while (!in.done())
    {
        u8 first = 0U;
        if (!in.take_u8(first))
        {
            return refused();
        }
        const bool indexed = (first & 0x80U) != 0U;
        const bool post_base = !indexed && (first & 0xF0U) == 0x10U;
        cont::ConstSpan<u8> name;
        cont::ConstSpan<u8> value;
        if (indexed || post_base)
        {
            u64 index = 0U;
            if (take_http_int(in, first, indexed ? 6U : 4U, index) != HttpDecode::Ok)
            {
                return refused();
            }
            if (indexed && (first & 0x40U) != 0U)
            {
                if (index >= kQpackStaticEntries)
                {
                    return refused();
                }
                out.emit(static_field(index).name, static_field(index).value, false);
                continue;
            }
            u64 absolute = base + index;
            if ((indexed && !below_base(base, index, absolute)) || !referable(m_table, absolute, required))
            {
                return refused();
            }
            const QpackTable::Entry& entry = m_table.entry(absolute);
            name_of(m_table, out, entry, name);
            value_of(m_table, out, entry, value);
            out.emit(name, value, false);
            continue;
        }

        bool never_index = false;
        if ((first & 0xE0U) == 0x20U)
        {
            // A literal name.
            never_index = (first & 0x10U) != 0U;
            cont::ConstSpan<u8> coded;
            bool huffman = false;
            if (take_http_string(in, first, 3U, section.size(), coded, huffman) != HttpDecode::Ok ||
                !out.literal(coded, huffman, name))
            {
                return refused();
            }
        }
        else
        {
            // A name reference: 01NT with a static or pre-base index, or 0000N with a post-base index.
            const bool pre_base = (first & 0xC0U) == 0x40U;
            never_index = (first & (pre_base ? 0x20U : 0x08U)) != 0U;
            const bool from_static = pre_base && (first & 0x10U) != 0U;
            u64 index = 0U;
            if (take_http_int(in, first, pre_base ? 4U : 3U, index) != HttpDecode::Ok)
            {
                return refused();
            }
            if (from_static)
            {
                if (index >= kQpackStaticEntries)
                {
                    return refused();
                }
                name = static_field(index).name;
            }
            else
            {
                u64 absolute = base + index;
                if ((pre_base && !below_base(base, index, absolute)) || !referable(m_table, absolute, required))
                {
                    return refused();
                }
                name_of(m_table, out, m_table.entry(absolute), name);
            }
        }
        u8 second = 0U;
        cont::ConstSpan<u8> coded;
        bool huffman = false;
        if (!in.take_u8(second) || take_http_string(in, second, 7U, section.size(), coded, huffman) != HttpDecode::Ok ||
            !out.literal(coded, huffman, value))
        {
            return refused();
        }
        out.emit(name, value, never_index);
    }

    if (required != 0U)
    {
        queue_instruction(0x80U, 7U, stream);
        if (required > m_known_received)
        {
            m_known_received = required;
        }
    }
    if (m_error != 0U)
    {
        return QpackStatus::Failed;
    }
    if (out.overflow)
    {
        return QpackStatus::TooLarge;
    }
    count = out.count;
    return QpackStatus::Complete;
}

void QpackDecoder::cancel(u64 stream) noexcept
{
    forget_blocked(stream);
    if (m_error == 0U && m_config.table_capacity != 0U)
    {
        queue_instruction(0x40U, 6U, stream);
    }
}

bool QpackDecoder::next_unblocked(u64& stream) noexcept
{
    for (u32 index = 0U; index < m_blocked_count; ++index)
    {
        if (m_blocked[index].required <= m_table.inserted())
        {
            stream = m_blocked[index].stream;
            forget_blocked(stream);
            return true;
        }
    }
    return false;
}

usize QpackDecoder::take_instructions(cont::Span<u8> out) noexcept
{
    const usize moved = (out.size() < m_out_bytes) ? out.size() : m_out_bytes;
    if (moved != 0U)
    {
        std::memcpy(out.data(), m_out, moved);
        m_out_bytes -= moved;
        std::memmove(m_out, m_out + moved, m_out_bytes);
    }
    // Insertions the sections acknowledged above did not cover: once the rest is out, so an increment follows them.
    const u64 owed = m_table.inserted() - m_known_received;
    if (m_out_bytes != 0U || owed == 0U || out.size() - moved < kHttpMaxIntegerBytes)
    {
        return moved;
    }
    cont::WireWriter writer(out.subspan(moved));
    put_http_int(writer, 0x00U, 6U, owed);
    m_known_received = m_table.inserted();
    return moved + writer.written();
}

// -- the encoder -----------------------------------------------------------------------------------------------------

QpackEncoder::~QpackEncoder()
{
    release();
}

void QpackEncoder::release() noexcept
{
    if (m_allocator == nullptr)
    {
        return;
    }
    m_table.release(*m_allocator);
    memory::destroy_array(*m_allocator, m_sections, m_config.sections);
    memory::destroy_array(*m_allocator, m_out, m_config.instruction_bytes);
    m_sections = nullptr;
    m_out = nullptr;
    m_allocator = nullptr;
}

bool QpackEncoder::init(memory::IAllocator& allocator, const QpackEncoderConfig& config) noexcept
{
    if (m_allocator != nullptr || config.table_capacity > kQpackMaxTableCapacity ||
        config.sections > kQpackMaxSections || config.instruction_bytes < kQpackMinInstructionBytes ||
        config.instruction_bytes > kQpackMaxInstructionBytes)
    {
        return false;
    }
    m_allocator = &allocator;
    m_config = config;
    m_out = memory::try_construct_array<u8>(allocator, config.instruction_bytes);
    bool ready = m_table.init(allocator, config.table_capacity) && m_out != nullptr;
    if (config.sections != 0U)
    {
        m_sections = memory::try_construct_array<Section>(allocator, config.sections);
        ready = ready && m_sections != nullptr;
    }
    if (!ready)
    {
        release();
    }
    return ready;
}

void QpackEncoder::peer_settings(u64 max_table_capacity, u64 blocked_streams) noexcept
{
    if (m_settings_known)
    {
        return;
    }
    m_settings_known = true;
    m_peer_max_entries = max_table_capacity / kHttpFieldOverhead;
    m_peer_blocked = blocked_streams;
    const u64 capacity = (max_table_capacity < m_config.table_capacity) ? max_table_capacity : m_config.table_capacity;
    if (capacity == 0U)
    {
        return;
    }
    m_table.set_capacity(static_cast<u32>(capacity));
    cont::WireWriter out(cont::Span<u8>(m_out, m_config.instruction_bytes));
    put_http_int(out, 0x20U, 5U, capacity);
    m_out_bytes = out.written();
}

bool QpackEncoder::fail() noexcept
{
    if (m_error == 0U)
    {
        m_error = kQpackDecoderStreamError;
    }
    return false;
}

bool QpackEncoder::instruction(u8 first, u64 value) noexcept
{
    if ((first & 0xC0U) == 0U)
    {
        if (value == 0U || value > m_table.inserted() - m_known_received)
        {
            return fail();
        }
        m_known_received += value;
        return true;
    }
    const bool cancelled = (first & 0x80U) == 0U;
    u32 kept = 0U;
    bool found = false;
    for (u32 index = 0U; index < m_section_count; ++index)
    {
        const Section section = m_sections[index];
        if (section.stream == value && (cancelled || !found))
        {
            found = true;
            if (!cancelled && section.required > m_known_received)
            {
                m_known_received = section.required;
            }
            continue;
        }
        m_sections[kept] = section;
        ++kept;
    }
    m_section_count = kept;
    return found || cancelled || fail();
}

bool QpackEncoder::on_decoder_stream(cont::ConstSpan<u8> bytes) noexcept
{
    for (const u8 byte : bytes)
    {
        if (m_error != 0U)
        {
            return false;
        }
        m_partial[m_partial_bytes] = byte;
        ++m_partial_bytes;
        cont::WireReader in(cont::ConstSpan<u8>(m_partial, m_partial_bytes));
        u8 first = 0U;
        if (!in.take_u8(first))
        {
            return fail();
        }
        u64 value = 0U;
        const HttpDecode status = take_http_int(in, first, ((first & 0x80U) != 0U) ? 7U : 6U, value);
        if (status == HttpDecode::Invalid)
        {
            return fail();
        }
        if (status == HttpDecode::Ok)
        {
            m_partial_bytes = 0U;
            if (!instruction(first, value))
            {
                return false;
            }
        }
    }
    return m_error == 0U;
}

usize QpackEncoder::blocking_streams(u64 except) const noexcept
{
    usize count = 0U;
    for (u32 index = 0U; index < m_section_count; ++index)
    {
        const Section& section = m_sections[index];
        if (section.required <= m_known_received || section.stream == except)
        {
            continue;
        }
        bool counted = false;
        for (u32 earlier = 0U; earlier < index && !counted; ++earlier)
        {
            counted = m_sections[earlier].stream == section.stream && m_sections[earlier].required > m_known_received;
        }
        count += counted ? 0U : 1U;
    }
    return count;
}

bool QpackEncoder::usable(u64 absolute, const Work& work) const noexcept
{
    return work.may_reference && (absolute < m_known_received || work.may_block);
}

bool QpackEncoder::find_field(const HttpField& field, u64& absolute) const noexcept
{
    for (u64 index = m_table.inserted(); index > m_table.dropped(); --index)
    {
        const QpackTable::Entry& entry = m_table.entry(index - 1U);
        if (m_table.name_is(entry, field.name) && m_table.value_is(entry, field.value))
        {
            absolute = index - 1U;
            return true;
        }
    }
    return false;
}

bool QpackEncoder::find_name(cont::ConstSpan<u8> name, u64& absolute) const noexcept
{
    for (u64 index = m_table.inserted(); index > m_table.dropped(); --index)
    {
        if (m_table.name_is(m_table.entry(index - 1U), name))
        {
            absolute = index - 1U;
            return true;
        }
    }
    return false;
}

bool QpackEncoder::seen_before(const HttpField& field) noexcept
{
    const u32 hash = hash_field(field);
    u32& slot = m_seen[(hash >> 1U) % 64U];
    if (slot == hash)
    {
        return true;
    }
    slot = hash;
    return false;
}

bool QpackEncoder::room_for(usize entry_bytes, const Work& work) const noexcept
{
    if (entry_bytes > m_table.capacity())
    {
        return false;
    }
    const u64 size = m_table.size();
    if (size + entry_bytes <= m_table.capacity())
    {
        return true;
    }
    // An entry is evictable once its insertion is acknowledged and nothing outstanding references it (section 2.1.1).
    u64 limit = (m_known_received < work.lowest) ? m_known_received : work.lowest;
    for (u32 index = 0U; index < m_section_count; ++index)
    {
        limit = (m_sections[index].lowest < limit) ? m_sections[index].lowest : limit;
    }
    const u64 excess = size + entry_bytes - m_table.capacity();
    u64 freed = 0U;
    for (u64 absolute = m_table.dropped(); freed < excess; ++absolute)
    {
        if (absolute >= limit)
        {
            return false;
        }
        freed += entry_size(m_table.entry(absolute));
    }
    return true;
}

bool QpackEncoder::insert(const HttpField& field, i32 static_name, const Work& work) noexcept
{
    const usize entry_bytes = field.name.size() + field.value.size() + kHttpFieldOverhead;
    if (!room_for(entry_bytes, work))
    {
        return false;
    }
    u64 named = 0U;
    const bool dynamic_name = static_name < 0 && find_name(field.name, named);
    const u64 name_index = (static_name >= 0) ? static_cast<u64>(static_name) : (m_table.inserted() - named - 1U);
    const usize head = (static_name >= 0 || dynamic_name) ? http_int_bytes(name_index, 6U)
                                                          : http_string_bytes(field.name, 5U);
    if (head + http_string_bytes(field.value, 7U) > m_config.instruction_bytes - m_out_bytes)
    {
        return false;
    }
    cont::WireWriter out(cont::Span<u8>(m_out + m_out_bytes, m_config.instruction_bytes - m_out_bytes));
    if (static_name >= 0)
    {
        put_http_int(out, 0xC0U, 6U, name_index);
    }
    else if (dynamic_name)
    {
        put_http_int(out, 0x80U, 6U, name_index);
    }
    else
    {
        put_http_string(out, 0x40U, 5U, field.name);
    }
    put_http_string(out, 0x00U, 7U, field.value);
    m_out_bytes += out.written();
    if (dynamic_name)
    {
        const QpackTable::Entry source = m_table.entry(named);
        m_table.make_room(entry_bytes);
        m_table.append_named(source, field.value);
    }
    else
    {
        m_table.make_room(entry_bytes);
        m_table.append(field.name, field.value);
    }
    return true;
}

void QpackEncoder::refresh(u64& absolute, const Work& work) noexcept
{
    // A field still in use that is about to fall out of the table is duplicated (section 2.1.1.1), which costs a byte
    // or two where falling out would cost its literal again.
    const u64 live = m_table.inserted() - m_table.dropped();
    if ((absolute - m_table.dropped()) >= (live / kDrainingFraction))
    {
        return;
    }
    const QpackTable::Entry source = m_table.entry(absolute);
    const usize entry_bytes = source.name_bytes + usize{source.value_bytes} + kHttpFieldOverhead;
    const u64 index = m_table.inserted() - absolute - 1U;
    // A section that may not block keeps referencing the original, so that must survive the copy's eviction.
    Work guarded = work;
    guarded.lowest = (!work.may_block && absolute < work.lowest) ? absolute : work.lowest;
    if (m_table.size() + entry_bytes <= m_table.capacity() || !room_for(entry_bytes, guarded) ||
        http_int_bytes(index, 5U) > m_config.instruction_bytes - m_out_bytes)
    {
        return;
    }
    cont::WireWriter out(cont::Span<u8>(m_out + m_out_bytes, m_config.instruction_bytes - m_out_bytes));
    put_http_int(out, 0x00U, 5U, index);
    m_out_bytes += out.written();
    m_table.make_room(entry_bytes);
    m_table.append_copy(source);
    if (work.may_block)
    {
        absolute = m_table.inserted() - 1U;
    }
}

usize QpackEncoder::write_line(const HttpField& field, Work& work, cont::Span<u8> out) noexcept
{
    const bool never_index = field.never_index || sensitive(field.name);
    const StaticMatch match = find_static(field.name, field.value);
    if (!never_index)
    {
        if (match.exact)
        {
            const usize bytes = http_int_bytes(static_cast<u64>(match.index), 6U);
            if (bytes > out.size())
            {
                return 0U;
            }
            cont::WireWriter line(out);
            put_http_int(line, 0xC0U, 6U, static_cast<u64>(match.index));
            return line.written();
        }
        u64 absolute = 0U;
        bool found = work.may_reference && find_field(field, absolute);
        if (!found && work.may_reference && seen_before(field) && insert(field, match.index, work))
        {
            absolute = m_table.inserted() - 1U;
            found = true;
        }
        if (found && work.may_reference)
        {
            refresh(absolute, work);
        }
        if (found && usable(absolute, work))
        {
            const bool pre_base = absolute < work.base;
            const u64 index = pre_base ? (work.base - absolute - 1U) : (absolute - work.base);
            const u8 prefix = pre_base ? 6U : 4U;
            if (http_int_bytes(index, prefix) > out.size())
            {
                return 0U;
            }
            cont::WireWriter line(out);
            put_http_int(line, pre_base ? 0x80U : 0x10U, prefix, index);
            work.required = (absolute + 1U > work.required) ? absolute + 1U : work.required;
            work.lowest = (absolute < work.lowest) ? absolute : work.lowest;
            return line.written();
        }
    }

    const u8 never_flag = never_index ? 0x20U : 0x00U;
    u64 named = 0U;
    const bool static_name = match.index >= 0;
    const bool dynamic_name = !static_name && !never_index && work.may_reference && find_name(field.name, named) &&
                              usable(named, work);
    const usize value_bytes = http_string_bytes(field.value, 7U);
    if (static_name || dynamic_name)
    {
        const bool post_base = dynamic_name && named >= work.base;
        u64 index = static_cast<u64>(match.index);
        if (dynamic_name)
        {
            index = post_base ? (named - work.base) : (work.base - named - 1U);
        }
        const u8 prefix = post_base ? 3U : 4U;
        if (http_int_bytes(index, prefix) + value_bytes > out.size())
        {
            return 0U;
        }
        cont::WireWriter line(out);
        const u32 flags = post_base ? 0x00U : (0x40U | u32{never_flag} | (static_name ? 0x10U : 0x00U));
        put_http_int(line, static_cast<u8>(flags), prefix, index);
        put_http_string(line, 0x00U, 7U, field.value);
        if (dynamic_name)
        {
            work.required = (named + 1U > work.required) ? named + 1U : work.required;
            work.lowest = (named < work.lowest) ? named : work.lowest;
        }
        return line.written();
    }
    if (http_string_bytes(field.name, 3U) + value_bytes > out.size())
    {
        return 0U;
    }
    cont::WireWriter line(out);
    put_http_string(line, static_cast<u8>(0x20U | (never_index ? 0x10U : 0x00U)), 3U, field.name);
    put_http_string(line, 0x00U, 7U, field.value);
    return line.written();
}

usize QpackEncoder::encode(u64 stream, cont::ConstSpan<HttpField> fields, cont::Span<u8> out) noexcept
{
    if (m_error != 0U || out.size() < kPrefixRoom)
    {
        return 0U;
    }
    Work work;
    work.stream = stream;
    work.base = m_table.inserted();
    work.may_reference = m_table.capacity() != 0U && m_section_count < m_config.sections;
    // A stream that already blocks counts once however many sections it has.
    work.may_block = blocking_streams(stream) < m_peer_blocked;

    usize at = kPrefixRoom;
    for (const HttpField& field : fields)
    {
        const usize line = write_line(field, work, out.subspan(at));
        if (line == 0U)
        {
            return 0U;
        }
        at += line;
    }

    u8 prefix[kPrefixRoom] = {};
    cont::WireWriter head((cont::Span<u8>(prefix)));
    if (work.required == 0U)
    {
        head.put_u8(0U);
        head.put_u8(0U);
    }
    else
    {
        const u64 encoded = (work.required % (2U * m_peer_max_entries)) + 1U;
        put_http_int(head, 0x00U, 8U, encoded);
        if (work.base >= work.required)
        {
            put_http_int(head, 0x00U, 7U, work.base - work.required);
        }
        else
        {
            put_http_int(head, 0x80U, 7U, work.required - work.base - 1U);
        }
        m_sections[m_section_count] = Section{stream, work.required, work.lowest};
        ++m_section_count;
    }
    const usize lines = at - kPrefixRoom;
    std::memcpy(out.data(), prefix, head.written());
    std::memmove(out.data() + head.written(), out.data() + kPrefixRoom, lines);
    return head.written() + lines;
}

usize QpackEncoder::take_instructions(cont::Span<u8> out) noexcept
{
    const usize moved = (out.size() < m_out_bytes) ? out.size() : m_out_bytes;
    if (moved != 0U)
    {
        std::memcpy(out.data(), m_out, moved);
        m_out_bytes -= moved;
        std::memmove(m_out, m_out + moved, m_out_bytes);
    }
    return moved;
}

} // namespace crd::net
