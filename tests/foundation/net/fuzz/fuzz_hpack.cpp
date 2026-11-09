// crd-fuzz-net-hpack: HPACK in crd-net (4.2a; docs/design/network-foundation.md#hpack) under the bounded fuzz harness
// (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: a mode byte, then that mode's bytes (0 past the end).
//   0  a hostile peer: any blocks under any limits. Decoded fields view the caller's text or static data, a block's
//      count is zero unless Complete, a table never exceeds its capacity and a refusal stays.
//   1  a table written apart from the implementation (entries in a plain list, eviction by summing sizes, the size
//      update rules restated) predicts every field, refusal, capacity and entry.
//   2  crd-net's encoder and decoder across SETTINGS changes: every block decodes to the fields given, both tables stay
//      the same, and a block refused for its room changes nothing.
#include "text_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/hpack.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::fuzz::BudgetAllocator;
using namespace crd::net::testing;
namespace cont = crd::containers;
using namespace crd::net;

constexpr usize kMaxLines = 12U;
constexpr u32 kModelStorage = 4096U;

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

Text short_text(Input& in)
{
    const usize length = in.byte() % 24U;
    return text_of(in.bytes(length));
}

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

void put_string(Text& out, const Text& text, bool huffman)
{
    if (!huffman)
    {
        put_int(out, 0x00U, 7U, text.size());
        out.append(text);
        return;
    }
    const usize bytes = http_huffman_bytes(view(text));
    put_int(out, 0x80U, 7U, bytes);
    const usize at = out.size();
    out.resize(at + bytes);
    http_huffman_encode(view(text), Span<u8>(reinterpret_cast<u8*>(out.data()) + at, bytes));
}

struct StaticRow
{
    u32 index;
    const char* name;
    const char* value;
};

// RFC 7541 appendix A's rows this mode uses; the rest are read back by the unit test of the whole table.
constexpr StaticRow kRows[] = {
    {1U, ":authority", ""},        {2U, ":method", "GET"},
    {8U, ":status", "200"},        {16U, "accept-encoding", "gzip, deflate"},
    {61U, "www-authenticate", ""},
};

struct Model
{
    cont::Array<TextPair> entries; // oldest first; those before `first` were evicted
    usize first = 0U;
    usize size = 0U;
    usize capacity = kHpackDefaultTableSize;
    usize limit = kHpackDefaultTableSize;
    usize owed_at_most = 0U;
    bool owed = false;

    [[nodiscard]] usize live() const { return entries.size() - first; }
    [[nodiscard]] const TextPair& back(usize number) const { return entries[entries.size() - number]; }

    void evict(usize target)
    {
        while (size > target)
        {
            size -= entries[first].first.size() + entries[first].second.size() + 32U;
            ++first;
        }
    }

    void insert(const Text& name, const Text& value)
    {
        const usize bytes = name.size() + value.size() + 32U;
        if (bytes > capacity)
        {
            evict(0U);
            return;
        }
        evict(capacity - bytes);
        entries.push_back(TextPair(name, value));
        size += bytes;
    }

    void set_limit(usize value)
    {
        limit = value;
        if (value < capacity)
        {
            owed_at_most = (owed && owed_at_most < value) ? owed_at_most : value;
            owed = true;
        }
    }
};

void hostile(Input& in, BudgetAllocator& allocator)
{
    HpackDecoder decoder;
    if (!decoder.init(allocator, (u32{in.byte()} * 32U) + in.byte()))
    {
        return;
    }
    HttpField fields[16];
    u8 text[256];
    volatile u8 sink = 0U; // every byte a field views is read
    while (in.more())
    {
        const u8 operation = in.byte();
        if ((operation % 8U) == 0U)
        {
            decoder.set_limit((u32{in.byte()} * 32U) % (decoder.table().max_capacity() + 1U));
            continue;
        }
        const ConstSpan<u8> block = in.bytes(in.byte());
        usize count = 0U;
        const bool failed_before = decoder.failed();
        const HpackStatus status = decoder.decode(block, Span<HttpField>(fields), Span<u8>(text), count);
        CRD_FUZZ_REQUIRE(status == HpackStatus::Complete || count == 0U);
        CRD_FUZZ_REQUIRE(!failed_before || status == HpackStatus::Failed);
        CRD_FUZZ_REQUIRE((status == HpackStatus::Failed) == decoder.failed());
        CRD_FUZZ_REQUIRE(decoder.table().size() <= decoder.table().capacity());
        for (usize index = 0U; index < count; ++index)
        {
            for (const u8 byte : fields[index].name)
            {
                sink ^= byte;
            }
            for (const u8 byte : fields[index].value)
            {
                sink ^= byte;
            }
        }
    }
}

void modelled(Input& in, BudgetAllocator& allocator)
{
    HpackDecoder decoder;
    if (!decoder.init(allocator, kModelStorage))
    {
        return;
    }
    Model model;
    HttpField fields[kMaxLines];
    u8 text[kMaxLines * 2048U];
    while (in.more())
    {
        if ((in.byte() % 4U) == 0U)
        {
            const usize limit = (u32{in.byte()} * 17U) % (kModelStorage + 1U);
            model.set_limit(limit);
            decoder.set_limit(static_cast<u32>(limit));
        }
        Text block;
        Text expected;
        bool refused = false;
        bool opened = false;
        u32 updates = 0U;
        const usize lines = in.byte() % kMaxLines;
        for (usize line = 0U; line < lines && !refused; ++line)
        {
            const u8 kind = in.byte() % 8U;
            if (kind == 7U)
            {
                const usize size = ((usize{in.byte()} << 8U) | in.byte()) % 5000U;
                put_int(block, 0x20U, 5U, size);
                if (opened || updates == 2U || size > (model.owed ? model.owed_at_most : model.limit))
                {
                    refused = true;
                    break;
                }
                ++updates;
                model.capacity = size;
                model.evict(size);
                model.owed = false;
                continue;
            }
            if (model.owed)
            {
                // A line where an update was owed: whatever line this is, it is refused.
                block.push_back(static_cast<char>(0x82U));
                refused = true;
                break;
            }
            opened = true;
            if (kind <= 1U)
            {
                if (kind == 0U)
                {
                    const StaticRow& row = kRows[in.byte() % 5U];
                    put_int(block, 0x80U, 7U, row.index);
                    expected += Text(row.name) + "=" + Text(row.value) + "\n";
                    continue;
                }
                const usize number = (in.byte() % (model.live() + 1U)) + 1U;
                put_int(block, 0x80U, 7U, 61U + number);
                if (number > model.live())
                {
                    refused = true;
                    break;
                }
                const TextPair& entry = model.back(number);
                expected += entry.first + "=" + entry.second + "\n";
                continue;
            }
            const bool incremental = kind <= 4U;
            const bool never = kind == 6U;
            const u32 literal_flag = never ? 0x10U : 0x00U;
            const u32 flags = incremental ? 0x40U : literal_flag;
            const u32 prefix = incremental ? 6U : 4U;
            Text name;
            if (kind == 2U || kind == 5U)
            {
                put_int(block, flags, prefix, 0U);
                name = short_text(in);
                put_string(block, name, (in.byte() & 1U) != 0U);
            }
            else if (kind == 3U)
            {
                const StaticRow& row = kRows[in.byte() % 5U];
                put_int(block, flags, prefix, row.index);
                name = Text(row.name);
            }
            else
            {
                const usize number = (in.byte() % (model.live() + 1U)) + 1U;
                put_int(block, flags, prefix, 61U + number);
                if (number > model.live())
                {
                    refused = true;
                    break;
                }
                name = model.back(number).first;
            }
            Text value = short_text(in);
            put_string(block, value, (in.byte() & 1U) != 0U);
            if (never)
            {
                expected += "!";
            }
            expected += name + "=" + value + "\n";
            if (incremental)
            {
                model.insert(name, value);
            }
        }
        refused = refused || model.owed;

        usize count = 0U;
        const HpackStatus status = decoder.decode(view(block), Span<HttpField>(fields), Span<u8>(text), count);
        if (refused)
        {
            CRD_FUZZ_REQUIRE(status == HpackStatus::Failed);
            return;
        }
        CRD_FUZZ_REQUIRE(status == HpackStatus::Complete);
        CRD_FUZZ_REQUIRE(joined(ConstSpan<HttpField>(fields, count)) == expected);
        const HttpFieldTable& table = decoder.table();
        CRD_FUZZ_REQUIRE(table.size() == model.size);
        CRD_FUZZ_REQUIRE(table.capacity() == model.capacity);
        CRD_FUZZ_REQUIRE(table.inserted() - table.dropped() == model.live());
    }
}

void round_trip(Input& in, BudgetAllocator& allocator)
{
    const u32 storage = u32{in.byte()} * 64U;
    HpackEncoder encoder;
    HpackDecoder decoder;
    if (!encoder.init(allocator, storage) || !decoder.init(allocator, 16384U))
    {
        return;
    }
    Text names[kMaxLines];
    Text values[kMaxLines];
    HttpField fields[kMaxLines];
    HttpField decoded[kMaxLines];
    u8 block[4096];
    u8 text[8192];
    while (in.more())
    {
        for (u8 changes = in.byte() % 3U; changes != 0U; --changes)
        {
            const u32 allowed = (u32{in.byte()} * 64U) % 16385U;
            decoder.set_limit(allowed);
            encoder.set_peer_limit(allowed);
        }
        const usize lines = in.byte() % kMaxLines;
        Text expected;
        for (usize line = 0U; line < lines; ++line)
        {
            names[line] = Text("x-") + number_text(in.byte() % 16U);
            values[line] = short_text(in);
            const bool never = (in.byte() % 8U) == 0U;
            expected += (never ? "!" : "");
            expected += names[line] + "=" + values[line] + "\n";
            fields[line] = HttpField{view(names[line]), view(values[line]), never};
        }
        const ConstSpan<HttpField> all(fields, lines);
        const u64 inserted = encoder.table().inserted();
        CRD_FUZZ_REQUIRE(encoder.encode(all, Span<u8>(block, HpackEncoder::max_bytes(all) - 1U)) == 0U);
        CRD_FUZZ_REQUIRE(encoder.table().inserted() == inserted);
        const usize written = encoder.encode(all, Span<u8>(block));
        CRD_FUZZ_REQUIRE(written != 0U || lines == 0U);
        usize count = 0U;
        const HpackStatus status =
            decoder.decode(ConstSpan<u8>(block, written), Span<HttpField>(decoded), Span<u8>(text), count);
        CRD_FUZZ_REQUIRE(status == HpackStatus::Complete);
        CRD_FUZZ_REQUIRE(joined(ConstSpan<HttpField>(decoded, count)) == expected);
        CRD_FUZZ_REQUIRE(decoder.table().size() == encoder.table().size());
        CRD_FUZZ_REQUIRE(decoder.table().inserted() - decoder.table().dropped() ==
                         encoder.table().inserted() - encoder.table().dropped());
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const u8* data, size_t size)
{
    Input in{data, size};
    BudgetAllocator allocator;
    switch (in.byte() % 3U)
    {
        case 0U:
            hostile(in, allocator);
            break;
        case 1U:
            modelled(in, allocator);
            break;
        default:
            round_trip(in, allocator);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // RFC 7541 C.2.1 as a hostile block.
    const u8 hostile[] = {0x00U, 0x10U, 0x00U, 0x01U, 0x0FU, 0x40U, 0x01U, 0x61U,
                          0x01U, 0x62U, 0xBEU, 0x3FU, 0x45U, 0x82U, 0x20U, 0x80U};
    sink.add("hostile", hostile, sizeof(hostile));
    // The model: inserts, references, an update, a lowered limit.
    const u8 model[] = {0x01U, 0x01U, 0x05U, 0x02U, 0x02U, 0x61U, 0x62U, 0x00U, 0x02U, 0x63U, 0x64U,
                        0x01U, 0x01U, 0x00U, 0x04U, 0x00U, 0x01U, 0x65U, 0x00U, 0x07U, 0x00U, 0x40U,
                        0x00U, 0x00U, 0x02U, 0x07U, 0x00U, 0x10U, 0x02U, 0x02U, 0x68U, 0x69U, 0x01U};
    sink.add("model", model, sizeof(model));
    // Both sides with repeats so that fields go into the table.
    const u8 both[] = {0x02U, 0x40U, 0x00U, 0x03U, 0x01U, 0x01U, 0x31U, 0x01U, 0x02U, 0x01U,
                       0x31U, 0x09U, 0x01U, 0x10U, 0x03U, 0x05U, 0x00U, 0x01U, 0x31U, 0x01U,
                       0x01U, 0x00U, 0x02U, 0x40U, 0x02U, 0x01U, 0x31U, 0x01U};
    sink.add("both", both, sizeof(both));
}
