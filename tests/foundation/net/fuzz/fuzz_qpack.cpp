// crd-fuzz-net-qpack: QPACK in crd-net (4.2a; docs/design/network-foundation.md#qpack) under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: a mode byte, then that mode's bytes (0 past the end).
//   0  a hostile peer: any bytes on the encoder stream, as sections, cancels and polls, under any limits. Decoded
//      fields view the caller's text or static data, a failure stays and its code is one of the RFC's.
//   1  a table written apart from the implementation (entries in a plain list, eviction by summing sizes) predicts every
//      accepted or refused instruction, every field, every blocked stream, and the decoder stream's bytes.
//   2  crd-net's encoder and decoder over a delayed, split encoder stream: every section decodes to the fields given, no
//      blocked stream beyond the limit, and neither side ever fails.
//   3  a hostile peer on the encoder's decoder stream: what it says never breaks the encoder's own bookkeeping.
//   4  Huffman: any text round trips; any code that decodes re-encodes to a code of the same text.
#include "text_fixture.hpp"

#include <crd/fuzz/harness.hpp>
#include <crd/net/qpack.hpp>

#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::fuzz::BudgetAllocator;
using crd::net::testing::erase_front;
using crd::net::testing::number_text;
using crd::net::testing::slice;
using crd::net::testing::Text;
using crd::net::testing::TextPair;
using namespace crd::net::testing;
namespace cont = crd::containers;
using namespace crd::net;

struct Input
{
    const u8* data;
    usize size;
    usize at = 0U;

    [[nodiscard]] bool more() const { return at < size; }
    u8 byte() { return (at < size) ? data[at++] : u8{0}; }
    ConstSpan<u8> bytes(usize count)
    {
        const usize rest = (count < size - at) ? count : size - at;
        const ConstSpan<u8> out(data + at, rest);
        at += rest;
        return out;
    }
};

// The prefix integer of RFC 7541 section 5.1, written apart from the implementation.
void put_int(Text& out, u32 flags, u32 prefix, u64 value)
{
    const u64 limit = (u64{1} << prefix) - 1U;
    if (value < limit)
    {
        out.push_back(static_cast<char>(flags | value));
        return;
    }
    out.push_back(static_cast<char>(flags | limit));
    value -= limit;
    while (value >= 128U)
    {
        out.push_back(static_cast<char>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<char>(value));
}

Text short_text(Input& in)
{
    const usize length = in.byte() % 24U;
    return text_of(in.bytes(length));
}

// RFC 9204 appendix A's names and values that this mode uses; the rest are never expected to be read back.
const char* static_name(u32 index)
{
    switch (index)
    {
    case 0U:
        return ":authority";
    case 1U:
        return ":path";
    case 15U:
        return ":method";
    case 25U:
        return ":status";
    case 98U:
        return "x-frame-options";
    default:
        return nullptr;
    }
}

const char* static_value(u32 index)
{
    switch (index)
    {
    case 1U:
        return "/";
    case 15U:
        return "CONNECT";
    case 25U:
        return "200";
    case 98U:
        return "sameorigin";
    default:
        return "";
    }
}

void hostile_decoder(Input& in, BudgetAllocator& allocator)
{
    QpackDecoderConfig config;
    config.table_capacity = (u32{in.byte()} * 8U) + (u32{in.byte()} % 8U);
    config.blocked_streams = in.byte() % 8U;
    config.instruction_bytes = 64U + (u32{in.byte()} * 4U);
    QpackDecoder decoder;
    if (!decoder.init(allocator, config))
    {
        return;
    }
    HttpField fields[16];
    u8 text[256];
    u8 out[64];
    volatile u8 sink = 0U; // every byte a field views is read
    while (in.more())
    {
        const u8 operation = in.byte();
        const ConstSpan<u8> payload = in.bytes(in.byte() % 48U);
        const u64 stream = in.byte();
        switch (operation % 5U)
        {
        case 0U:
            (void)decoder.on_encoder_stream(payload);
            break;
        case 1U:
        {
            usize count = 0U;
            const QpackStatus status =
                decoder.decode(stream, payload, Span<HttpField>(fields), Span<u8>(text), count);
            CRD_FUZZ_REQUIRE(status == QpackStatus::Complete || count == 0U);
            usize total = 0U;
            for (usize index = 0U; index < count; ++index)
            {
                total += fields[index].name.size() + fields[index].value.size();
                for (const u8 byte : fields[index].name)
                {
                    sink ^= byte;
                }
                for (const u8 byte : fields[index].value)
                {
                    sink ^= byte;
                }
            }
            CRD_FUZZ_REQUIRE(total <= sizeof(text) + (count * 96U));
            break;
        }
        case 2U:
            decoder.cancel(stream);
            break;
        case 3U:
        {
            u64 unblocked = 0U;
            while (decoder.next_unblocked(unblocked))
            {
            }
            break;
        }
        default:
            (void)decoder.take_instructions(Span<u8>(out));
            break;
        }
        if (decoder.error() != 0U)
        {
            CRD_FUZZ_REQUIRE(decoder.error() == kQpackDecompressionFailed ||
                             decoder.error() == kQpackEncoderStreamError || decoder.error() == kHttp3ExcessiveLoad);
            usize count = 0U;
            CRD_FUZZ_REQUIRE(decoder.decode(0U, ConstSpan<u8>(), Span<HttpField>(fields), Span<u8>(text), count) ==
                             QpackStatus::Failed);
            CRD_FUZZ_REQUIRE(!decoder.on_encoder_stream(ConstSpan<u8>()));
            return;
        }
        CRD_FUZZ_REQUIRE(decoder.blocked() <= config.blocked_streams);
        CRD_FUZZ_REQUIRE(decoder.table().size() <= decoder.table().capacity());
    }
}

// The dynamic table as the RFC words it.
struct ModelTable
{
    struct Entry
    {
        Text name;
        Text value;
    };

    u32 capacity = 0U;
    u64 inserted = 0U;
    cont::Array<Entry> entries; // oldest first; dropped() of them are gone

    [[nodiscard]] u64 size() const
    {
        u64 total = 0U;
        for (const Entry& entry : entries)
        {
            total += entry.name.size() + entry.value.size() + 32U;
        }
        return total;
    }

    [[nodiscard]] u64 dropped() const { return inserted - entries.size(); }

    void evict(u64 need)
    {
        while (size() + need > capacity)
        {
            entries.erase(0U);
        }
    }

    void add(std::string_view name, std::string_view value)
    {
        evict(name.size() + value.size() + 32U);
        entries.push_back(Entry{Text(name), Text(value)});
        ++inserted;
    }

    [[nodiscard]] bool live(u64 absolute) const { return absolute >= dropped() && absolute < inserted; }
    [[nodiscard]] const Entry& at(u64 absolute) const { return entries[static_cast<usize>(absolute - dropped())]; }
};

struct ModelRun
{
    explicit ModelRun(Input& input) : in(input) {}

    Input& in;
    QpackDecoder decoder;
    QpackDecoderConfig config;
    ModelTable table;
    cont::Array<std::pair<u64, u64>> blocked; // stream, required
    Text queue;                        // decoder stream bytes the model expects
    u64 known = 0U;
    HttpField fields[32];
    u8 text[2048];

    void drain()
    {
        u8 out[4096];
        Text got;
        for (usize bytes = decoder.take_instructions(Span<u8>(out)); bytes != 0U;
             bytes = decoder.take_instructions(Span<u8>(out)))
        {
            got.append(reinterpret_cast<const char*>(out), bytes);
        }
        Text expected = queue;
        queue.clear();
        if (table.inserted > known)
        {
            put_int(expected, 0x00U, 6U, table.inserted - known);
            known = table.inserted;
        }
        CRD_FUZZ_REQUIRE(got == expected);
    }

    // Feeds an encoder-stream instruction whose fate the model decided.
    bool feed(std::string_view bytes, bool accepted)
    {
        const bool taken = decoder.on_encoder_stream(view(bytes));
        CRD_FUZZ_REQUIRE(taken == accepted);
        if (!accepted)
        {
            CRD_FUZZ_REQUIRE(decoder.error() == kQpackEncoderStreamError);
        }
        return accepted;
    }

    void poll()
    {
        u64 stream = 0U;
        while (decoder.next_unblocked(stream))
        {
            bool expected = false;
            for (usize index = 0U; index < blocked.size(); ++index)
            {
                if (blocked[index].first == stream && blocked[index].second <= table.inserted)
                {
                    blocked.erase(index);
                    expected = true;
                    break;
                }
            }
            CRD_FUZZ_REQUIRE(expected);
        }
    }

    bool instruction()
    {
        const u8 kind = in.byte() % 5U;
        Text bytes;
        if (kind == 0U)
        {
            const u32 capacity = (u32{in.byte()} * 16U) & 0xFFFU;
            put_int(bytes, 0x20U, 5U, capacity);
            if (capacity > config.table_capacity)
            {
                return feed(bytes, false);
            }
            table.capacity = capacity;
            table.evict(0U);
            return feed(bytes, true);
        }
        if (kind == 1U)
        {
            const Text name = short_text(in);
            const Text value = short_text(in);
            put_int(bytes, 0x40U, 5U, name.size());
            bytes += name;
            put_int(bytes, 0x00U, 7U, value.size());
            bytes += value;
            if (name.size() + value.size() + 32U > table.capacity)
            {
                return feed(bytes, false);
            }
            table.add(name, value);
            return feed(bytes, true);
        }
        if (kind == 2U)
        {
            const u32 index = in.byte() % 104U;
            const Text value = short_text(in);
            put_int(bytes, 0xC0U, 6U, index);
            put_int(bytes, 0x00U, 7U, value.size());
            bytes += value;
            const char* const name = (index < 99U) ? static_name(index) : "?";
            if (name == nullptr)
            {
                return true; // the model holds a few static names only
            }
            if (index >= 99U || std::strlen(name) + value.size() + 32U > table.capacity)
            {
                return feed(bytes, false);
            }
            table.add(name, value);
            return feed(bytes, true);
        }
        const u64 index = in.byte() % 12U;
        const Text value = short_text(in);
        const bool exists = index < table.inserted && table.live(table.inserted - index - 1U);
        if (kind == 3U)
        {
            put_int(bytes, 0x80U, 6U, index);
            put_int(bytes, 0x00U, 7U, value.size());
            bytes += value;
            if (!exists)
            {
                return feed(bytes, false);
            }
            const Text name(table.at(table.inserted - index - 1U).name);
            if (name.size() + value.size() + 32U > table.capacity)
            {
                return feed(bytes, false);
            }
            table.add(name, value);
            return feed(bytes, true);
        }
        put_int(bytes, 0x00U, 5U, index);
        if (!exists)
        {
            return feed(bytes, false);
        }
        const Text copied_name(table.at(table.inserted - index - 1U).name);
        const Text copied_value(table.at(table.inserted - index - 1U).value);
        table.add(copied_name, copied_value);
        return feed(bytes, true);
    }

    bool section()
    {
        const u64 stream = in.byte() % 8U;
        const u64 max_entries = config.table_capacity / 32U;
        u64 required = 0U;
        const u8 pick = in.byte();
        if (max_entries != 0U && (pick & 1U) != 0U)
        {
            const u64 back = pick >> 1U;
            const u64 lowest = (table.inserted + 1U > max_entries) ? table.inserted - max_entries + 2U : 1U;
            required = lowest + (back % (max_entries + 1U));
            required = (required > table.inserted + max_entries) ? table.inserted + max_entries : required;
        }
        Text bytes;
        put_int(bytes, 0x00U, 8U, (required == 0U) ? 0U : ((required % (2U * max_entries)) + 1U));
        const bool negative = required != 0U && (in.byte() & 1U) != 0U;
        const u64 delta = negative ? (in.byte() % required) : (in.byte() % 8U);
        put_int(bytes, negative ? 0x80U : 0x00U, 7U, delta);
        const u64 base = negative ? (required - delta - 1U) : (required + delta);

        Text expected;
        bool valid = true;
        const u32 lines = in.byte() % 6U;
        for (u32 index = 0U; index < lines; ++index)
        {
            const u8 kind = in.byte() % 4U;
            const u32 number = in.byte() % 12U;
            if (kind == 0U)
            {
                const u32 table_index = (in.byte() & 1U) != 0U ? number % 2U : 98U;
                put_int(bytes, 0xC0U, 6U, table_index);
                expected += std::string_view(Text(static_name(table_index)) + "=" + static_value(table_index) + "\n");
                continue;
            }
            const bool post = kind == 2U;
            const u64 absolute = post ? (base + number) : (base - number - 1U);
            const bool in_range = (post || number < base) && absolute < required && table.live(absolute);
            if (kind == 3U)
            {
                put_int(bytes, 0x40U, 4U, number);
                const Text value = short_text(in);
                put_int(bytes, 0x00U, 7U, value.size());
                bytes += value;
                valid = valid && in_range;
                if (in_range)
                {
                    expected += table.at(absolute).name + "=" + value + "\n";
                }
                continue;
            }
            put_int(bytes, post ? 0x10U : 0x80U, post ? 4U : 6U, number);
            valid = valid && in_range;
            if (in_range)
            {
                const ModelTable::Entry& entry = table.at(absolute);
                expected += entry.name + "=" + entry.value + "\n";
            }
        }

        usize count = 0U;
        const QpackStatus status = decoder.decode(stream, view(bytes), Span<HttpField>(fields), Span<u8>(text), count);
        if (required > table.inserted)
        {
            bool known_stream = false;
            for (auto& one : blocked)
            {
                if (one.first == stream)
                {
                    one.second = required;
                    known_stream = true;
                }
            }
            if (!known_stream && blocked.size() == config.blocked_streams)
            {
                CRD_FUZZ_REQUIRE(status == QpackStatus::Failed);
                CRD_FUZZ_REQUIRE(decoder.error() == kQpackDecompressionFailed);
                return false;
            }
            if (!known_stream)
            {
                blocked.push_back(std::pair<u64, u64>(stream, required));
            }
            CRD_FUZZ_REQUIRE(status == QpackStatus::Blocked);
            return true;
        }
        for (usize index = 0U; index < blocked.size(); ++index)
        {
            if (blocked[index].first == stream)
            {
                blocked.erase(index);
                break;
            }
        }
        if (!valid)
        {
            CRD_FUZZ_REQUIRE(status == QpackStatus::Failed);
            CRD_FUZZ_REQUIRE(decoder.error() == kQpackDecompressionFailed);
            return false;
        }
        CRD_FUZZ_REQUIRE(status == QpackStatus::Complete);
        Text got;
        for (usize index = 0U; index < count; ++index)
        {
            got += text_of(fields[index].name) + "=" + text_of(fields[index].value) + "\n";
        }
        CRD_FUZZ_REQUIRE(got == expected);
        if (required != 0U)
        {
            put_int(queue, 0x80U, 7U, stream);
            known = (required > known) ? required : known;
        }
        return true;
    }

    void go(BudgetAllocator& allocator)
    {
        config.table_capacity = (u32{in.byte()} % 16U) * 64U;
        config.blocked_streams = in.byte() % 4U;
        config.instruction_bytes = 4096U;
        if (!decoder.init(allocator, config))
        {
            return;
        }
        bool alive = true;
        while (alive && in.more())
        {
            alive = ((in.byte() & 3U) != 0U) ? instruction() : section();
            if (alive && table.size() > table.capacity)
            {
                CRD_FUZZ_REQUIRE(false);
            }
            if (alive && decoder.table().size() != table.size())
            {
                CRD_FUZZ_REQUIRE(false);
            }
            if (alive && (in.byte() & 1U) != 0U)
            {
                drain();
            }
            if (alive && (in.byte() & 1U) != 0U)
            {
                poll();
            }
            if (queue.size() > 2048U)
            {
                drain();
            }
        }
    }
};

void encoder_decoder(Input& in, BudgetAllocator& allocator)
{
    const u32 capacity = (u32{in.byte()} % 16U) * 64U;
    const u32 blocked = in.byte() % 6U;
    QpackEncoder encoder;
    QpackEncoderConfig encoder_config;
    encoder_config.table_capacity = capacity;
    encoder_config.sections = 16U;
    QpackDecoder decoder;
    QpackDecoderConfig decoder_config;
    decoder_config.table_capacity = capacity;
    decoder_config.blocked_streams = blocked;
    decoder_config.instruction_bytes = 1U << 16U;
    if (!encoder.init(allocator, encoder_config) || !decoder.init(allocator, decoder_config))
    {
        return;
    }
    encoder.peer_settings(capacity, blocked);

    struct Waiting
    {
        u64 stream = 0U;
        Text section;
        Text expected;
    };
    cont::Array<Waiting> waiting;
    Text to_decoder;
    u8 room[4096];
    HttpField fields[32];
    u8 text[4096];
    u64 next = 0U;

    const auto settle = [&](usize at)
    {
        usize count = 0U;
        const QpackStatus status =
            decoder.decode(waiting[at].stream, view(waiting[at].section), Span<HttpField>(fields), Span<u8>(text), count);
        if (status == QpackStatus::Blocked)
        {
            return;
        }
        CRD_FUZZ_REQUIRE(status == QpackStatus::Complete);
        Text got;
        for (usize index = 0U; index < count; ++index)
        {
            got += text_of(fields[index].name) + "=" + text_of(fields[index].value) + "\n";
        }
        CRD_FUZZ_REQUIRE(got == waiting[at].expected);
        waiting.erase(at);
    };

    const char* const names[] = {"x-a", "x-bb", ":path", "accept", "content-type"};
    while (in.more())
    {
        cont::Array<TextPair> list;
        cont::Array<HttpField> input;
        const u32 count = 1U + (in.byte() % 6U);
        for (u32 index = 0U; index < count; ++index)
        {
            const u8 pick = in.byte();
            Text name(names[pick % 5U]);
            Text value(1U, static_cast<char>('a' + (pick % 3U)));
            value += number_text(in.byte() % 7U);
            list.push_back(TextPair(name, value));
        }
        Text expected;
        for (const auto& field : list)
        {
            input.push_back(HttpField{view(field.first), view(field.second), false});
            expected += field.first + "=" + field.second + "\n";
        }
        const usize written = encoder.encode(next, ConstSpan<HttpField>(input.data(), input.size()), Span<u8>(room));
        CRD_FUZZ_REQUIRE(written != 0U);
        waiting.push_back(Waiting{next, Text(reinterpret_cast<const char*>(room), written), expected});
        next += 4U;

        u8 buffer[8192];
        to_decoder.append(reinterpret_cast<const char*>(buffer), encoder.take_instructions(Span<u8>(buffer)));
        if ((in.byte() & 3U) != 0U)
        {
            const usize cut = to_decoder.empty() ? 0U : in.byte() % (to_decoder.size() + 1U);
            CRD_FUZZ_REQUIRE(decoder.on_encoder_stream(view(slice(to_decoder, 0U, cut))));
            erase_front(to_decoder, cut);
        }
        u64 unblocked = 0U;
        while (decoder.next_unblocked(unblocked))
        {
            for (usize at = 0U; at < waiting.size(); ++at)
            {
                if (waiting[at].stream == unblocked)
                {
                    settle(at);
                    break;
                }
            }
        }
        for (usize at = waiting.size(); at-- > 0U;)
        {
            settle(at);
        }
        const usize answer = decoder.take_instructions(Span<u8>(buffer));
        CRD_FUZZ_REQUIRE(encoder.on_decoder_stream(ConstSpan<u8>(buffer, answer)));
        CRD_FUZZ_REQUIRE(encoder.error() == 0U && decoder.error() == 0U);
    }
}

void hostile_encoder_peer(Input& in, BudgetAllocator& allocator)
{
    QpackEncoder encoder;
    QpackEncoderConfig config;
    config.table_capacity = (u32{in.byte()} % 16U) * 64U;
    config.sections = 1U + (in.byte() % 16U);
    if (!encoder.init(allocator, config))
    {
        return;
    }
    encoder.peer_settings(config.table_capacity, in.byte() % 6U);
    u8 room[4096];
    while (in.more())
    {
        const u8 operation = in.byte();
        if ((operation & 1U) != 0U)
        {
            const Text value(1U, static_cast<char>('a' + (operation % 5U)));
            const Text name("x-name");
            const HttpField field{view(name), view(value), false};
            (void)encoder.encode(in.byte() % 8U, ConstSpan<HttpField>(&field, 1U), Span<u8>(room));
        }
        else if ((operation & 2U) != 0U)
        {
            (void)encoder.on_decoder_stream(in.bytes(in.byte() % 12U));
        }
        else
        {
            (void)encoder.take_instructions(Span<u8>(room));
        }
        CRD_FUZZ_REQUIRE(encoder.known_received() <= encoder.table().inserted());
        CRD_FUZZ_REQUIRE(encoder.outstanding() <= config.sections);
        CRD_FUZZ_REQUIRE(encoder.table().size() <= encoder.table().capacity());
        if (encoder.error() != 0U)
        {
            CRD_FUZZ_REQUIRE(encoder.error() == kQpackDecoderStreamError);
            CRD_FUZZ_REQUIRE(!encoder.on_decoder_stream(ConstSpan<u8>(room, 1U)));
            return;
        }
    }
}

void huffman_round_trip(Input& in)
{
    u8 coded[1024];
    u8 text[2048];
    const ConstSpan<u8> source = in.bytes(in.byte());
    const usize encoded = http_huffman_bytes(source);
    if (encoded <= sizeof(coded))
    {
        http_huffman_encode(source, Span<u8>(coded, encoded));
        usize decoded = 0U;
        CRD_FUZZ_REQUIRE(http_huffman_decode(ConstSpan<u8>(coded, encoded), Span<u8>(text), decoded));
        CRD_FUZZ_REQUIRE(decoded == source.size());
        CRD_FUZZ_REQUIRE(source.empty() || std::memcmp(text, source.data(), source.size()) == 0);
    }
    const ConstSpan<u8> hostile = in.bytes(in.byte());
    usize decoded = 0U;
    if (http_huffman_decode(hostile, Span<u8>(text), decoded))
    {
        CRD_FUZZ_REQUIRE(decoded * 5U <= hostile.size() * 8U);
        if (decoded <= sizeof(text))
        {
            const usize again = http_huffman_bytes(ConstSpan<u8>(text, decoded));
            CRD_FUZZ_REQUIRE(again <= sizeof(coded));
            http_huffman_encode(ConstSpan<u8>(text, decoded), Span<u8>(coded, again));
            u8 round[2048];
            usize second = 0U;
            CRD_FUZZ_REQUIRE(http_huffman_decode(ConstSpan<u8>(coded, again), Span<u8>(round), second));
            CRD_FUZZ_REQUIRE(second == decoded);
            CRD_FUZZ_REQUIRE(decoded == 0U || std::memcmp(text, round, decoded) == 0);
        }
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes || size == 0U)
    {
        return 0;
    }
    Input in{data, size};
    BudgetAllocator allocator;
    switch (in.byte() % 5U)
    {
    case 0U:
        hostile_decoder(in, allocator);
        break;
    case 1U:
    {
        ModelRun run(in);
        run.go(allocator);
        break;
    }
    case 2U:
        encoder_decoder(in, allocator);
        break;
    case 3U:
        hostile_encoder_peer(in, allocator);
        break;
    default:
        huffman_round_trip(in);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // RFC 9204 appendix B's streams as a hostile input: capacity 220, then its encoder stream and a section.
    const u8 hostile[] = {0x00U, 0x1BU, 0x04U, 0x08U, 0x30U, 0x00U, 0x06U, 0x3FU, 0xBDU, 0x01U, 0xC0U,
                          0x00U, 0x00U, 0x01U, 0x04U, 0x03U, 0x81U, 0x10U, 0x11U, 0x00U, 0x04U, 0x00U};
    sink.add("hostile", hostile, sizeof(hostile));
    // The model: a table, inserts, duplicates, a name reference, sections referencing them, blocked and not.
    const u8 model[] = {0x01U, 0x05U, 0x02U, 0x01U, 0x00U, 0x10U, 0x00U, 0x01U, 0x01U, 0x03U, 0x61U, 0x62U, 0x63U, 0x02U,
                        0x31U, 0x32U, 0x00U, 0x00U, 0x01U, 0x03U, 0x00U, 0x00U, 0x03U, 0x02U, 0x00U, 0x04U, 0x00U, 0x00U,
                        0x01U, 0x00U, 0x02U, 0x00U, 0x01U, 0x09U, 0x00U, 0x01U, 0x03U, 0x02U, 0x00U, 0x00U, 0x01U, 0x01U};
    sink.add("model", model, sizeof(model));
    // Both sides over a delayed stream, with repeats so that fields go into the table.
    const u8 both[] = {0x02U, 0x05U, 0x02U, 0x03U, 0x00U, 0x01U, 0x06U, 0x01U, 0x07U, 0x01U, 0x02U, 0x00U, 0x05U, 0x01U,
                       0x01U, 0x00U, 0x06U, 0x01U, 0x07U, 0x01U, 0x02U, 0x03U, 0x00U, 0x01U, 0x01U, 0x06U, 0x01U, 0x07U,
                       0x03U, 0x05U, 0x02U, 0x00U, 0x06U, 0x01U, 0x07U, 0x01U, 0x03U, 0x09U};
    sink.add("both", both, sizeof(both));
    const u8 stream[] = {0x03U, 0x04U, 0x04U, 0x02U, 0x03U, 0x00U, 0x05U, 0x03U, 0x02U, 0x02U, 0x80U, 0x41U, 0x01U, 0x00U};
    sink.add("stream", stream, sizeof(stream));
    const u8 huffman[] = {0x04U, 0x06U, 0x77U, 0x77U, 0x77U, 0x2EU, 0x61U, 0x62U, 0x04U, 0xF1U, 0xE3U, 0xC2U, 0xFFU};
    sink.add("huffman", huffman, sizeof(huffman));
    const u8 bad_huffman[] = {0x04U, 0x00U, 0x02U, 0xFFU, 0xFFU};
    sink.add("bad-huffman", bad_huffman, sizeof(bad_huffman));
}
