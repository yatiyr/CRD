// crd-qpack-replay: crd-net's QPACK against the offline interop corpus (4.2a;
// docs/design/network-foundation.md#qpack). Two modes, run by qpack_interop.py over the pinned qifs repository:
//
//   decode <qif> <encoded> <capacity> <blocked>       decodes another implementation's output (the block format of
//                                                     QUIC WG's "QPACK Offline Interop": stream ID, length, bytes;
//                                                     stream 0 is the encoder stream) and compares every section with
//                                                     the QIF text it was encoded from.
//   encode <qif> <capacity> <blocked> <ack>           runs crd-net's encoder over the QIF (ack 1: the decoder's
//                                                     instructions return at once, ack 0: never), proves the result
//                                                     through crd-net's decoder and prints the bytes it wrote.
//   refuse <encoded> <capacity> <blocked>             exits 0 only if the decoder refuses the input.
//
// Exit 0 on success, 1 on a difference or refusal, 2 for bad arguments or an unreadable file.

#include "../text_fixture.hpp"

#include <crd/memory/allocator.hpp>
#include <crd/net/qpack.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace
{
using namespace crd;
using namespace crd::net;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::testing::slice;
using crd::net::testing::Text;
using crd::net::testing::TextPair;
namespace cont = crd::containers;

using Fields = cont::Array<TextPair>;

struct Held
{
    u64 stream = 0U;
    Text section;
};

constexpr usize kMaxFields = 4096U;
constexpr usize kMaxText = 1U << 20U;

ConstSpan<u8> view(std::string_view bytes)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
}

bool read_file(const char* path, Text& out)
{
    std::FILE* const file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    char chunk[65536];
    for (usize got = std::fread(chunk, 1U, sizeof(chunk), file); got != 0U; got = std::fread(chunk, 1U, sizeof(chunk), file))
    {
        out.append(chunk, got);
    }
    std::fclose(file);
    return true;
}

// Each blank-line-terminated section of a QIF file: "name<TAB>value" lines, '#' comments.
cont::Array<Fields> parse_qif(std::string_view text)
{
    cont::Array<Fields> sections;
    Fields current;
    while (!text.empty())
    {
        usize end = text.find('\n');
        end = (end == std::string_view::npos) ? text.size() : end;
        std::string_view line = text.substr(0U, end);
        text.remove_prefix((end < text.size()) ? end + 1U : end);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1U);
        }
        if (line.empty())
        {
            if (!current.empty())
            {
                sections.push_back(current);
                current.clear();
            }
            continue;
        }
        if (line[0] == '#')
        {
            continue;
        }
        const usize tab = line.find('\t');
        const Text name(line.substr(0U, tab));
        const Text value((tab == std::string_view::npos) ? std::string_view() : line.substr(tab + 1U));
        current.push_back(TextPair(name, value));
    }
    if (!current.empty())
    {
        sections.push_back(current);
    }
    return sections;
}

struct Decode
{
    Decode(u32 capacity, u32 blocked)
    {
        QpackDecoderConfig config;
        config.table_capacity = capacity;
        config.blocked_streams = blocked;
        config.instruction_bytes = 1U << 20U;
        ok = decoder.init(allocator, config);
        fields.resize(kMaxFields);
        text.resize(kMaxText);
        if (capacity != 0U)
        {
            // "For historical reasons, the initial dynamic table capacity is the maximum."
            u8 room[kHttpMaxIntegerBytes] = {};
            cont::WireWriter out((Span<u8>(room)));
            put_http_int(out, 0x20U, 5U, capacity);
            ok = ok && decoder.on_encoder_stream(ConstSpan<u8>(room, out.written()));
        }
    }

    // One section: `done` is whether it decoded (false: blocked). False on a failure or a difference.
    bool section(u64 stream, std::string_view bytes, const cont::Array<Fields>& expected, bool& done)
    {
        usize count = 0U;
        const QpackStatus status =
            decoder.decode(stream, view(bytes), Span<HttpField>(fields.data(), fields.size()),
                           Span<u8>(text.data(), text.size()), count);
        done = status != QpackStatus::Blocked;
        if (status == QpackStatus::Blocked)
        {
            return true;
        }
        if (status != QpackStatus::Complete)
        {
            std::fprintf(stderr, "stream %llu: decode status %d, error 0x%llx\n", static_cast<unsigned long long>(stream),
                         static_cast<int>(status), static_cast<unsigned long long>(decoder.error()));
            return false;
        }
        if (expected.empty())
        {
            return true; // refuse mode: only whether the input is valid
        }
        if (stream == 0U || stream > expected.size())
        {
            std::fprintf(stderr, "stream %llu has no section in the QIF\n", static_cast<unsigned long long>(stream));
            return false;
        }
        const Fields& want = expected[stream - 1U];
        if (want.size() != count)
        {
            std::fprintf(stderr, "stream %llu: %zu fields, QIF has %zu\n", static_cast<unsigned long long>(stream), count,
                         want.size());
            return false;
        }
        for (usize index = 0U; index < count; ++index)
        {
            const std::string_view name(reinterpret_cast<const char*>(fields[index].name.data()), fields[index].name.size());
            const std::string_view value(reinterpret_cast<const char*>(fields[index].value.data()),
                                         fields[index].value.size());
            if (name != std::string_view(want[index].first) || value != std::string_view(want[index].second))
            {
                std::fprintf(stderr, "stream %llu field %zu differs\n", static_cast<unsigned long long>(stream), index);
                return false;
            }
        }
        return true;
    }

    memory::IAllocator& allocator = *memory::default_allocator();
    QpackDecoder decoder;
    cont::Array<HttpField> fields;
    cont::Array<u8> text;
    bool ok = false;
};

int run_decode(const cont::Array<Fields>* expected, std::string_view encoded, u32 capacity, u32 blocked)
{
    Decode decode(capacity, blocked);
    if (!decode.ok)
    {
        return 2;
    }
    const cont::Array<Fields> none;
    const cont::Array<Fields>& sections = (expected != nullptr) ? *expected : none;
    cont::Array<Held> held;
    usize decoded = 0U;
    usize at = 0U;
    while (at < encoded.size())
    {
        if (encoded.size() - at < 12U)
        {
            std::fprintf(stderr, "a block cut in its header\n");
            return 1;
        }
        const auto byte_at = [&encoded](usize index) { return static_cast<u64>(static_cast<u8>(encoded[index])); };
        u64 stream = 0U;
        for (usize index = 0U; index < 8U; ++index)
        {
            stream = (stream << 8U) | byte_at(at + index);
        }
        u64 length = 0U;
        for (usize index = 0U; index < 4U; ++index)
        {
            length = (length << 8U) | byte_at(at + 8U + index);
        }
        at += 12U;
        if (length > encoded.size() - at)
        {
            std::fprintf(stderr, "a block past the end\n");
            return 1;
        }
        const Text bytes = slice(encoded, at, static_cast<usize>(length));
        at += static_cast<usize>(length);
        if (stream == 0U)
        {
            if (!decode.decoder.on_encoder_stream(view(bytes)))
            {
                std::fprintf(stderr, "encoder stream refused, error 0x%llx\n",
                             static_cast<unsigned long long>(decode.decoder.error()));
                return 1;
            }
            u64 unblocked = 0U;
            while (decode.decoder.next_unblocked(unblocked))
            {
                usize place = 0U;
                while (place < held.size() && held[place].stream != unblocked)
                {
                    ++place;
                }
                bool done = false;
                if (place == held.size() || !decode.section(unblocked, held[place].section, sections, done) || !done)
                {
                    return 1;
                }
                held.erase(place);
                ++decoded;
            }
            continue;
        }
        bool done = false;
        if (!decode.section(stream, bytes, sections, done))
        {
            return 1;
        }
        if (done)
        {
            ++decoded;
        }
        else
        {
            held.push_back(Held{stream, bytes});
        }
    }
    if (expected == nullptr)
    {
        return 0;
    }
    if (decoded != sections.size() || !held.empty())
    {
        std::fprintf(stderr, "%zu of %zu sections decoded, %zu blocked at the end\n", decoded, sections.size(),
                     held.size());
        return 1;
    }
    return 0;
}

int run_encode(const cont::Array<Fields>& sections, u32 capacity, u32 blocked, bool acknowledge)
{
    memory::IAllocator& allocator = *memory::default_allocator();
    QpackEncoder encoder;
    QpackEncoderConfig config;
    config.table_capacity = capacity;
    config.sections = kQpackMaxSections;
    config.instruction_bytes = kQpackMaxInstructionBytes;
    if (!encoder.init(allocator, config))
    {
        return 2;
    }
    encoder.peer_settings(capacity, blocked);
    Decode decode(capacity, blocked);
    if (!decode.ok)
    {
        return 2;
    }
    if (capacity != 0U)
    {
        // run_decode's Set Dynamic Table Capacity was also this one: take the instruction out.
        u8 room[32];
        (void)encoder.take_instructions(Span<u8>(room));
    }
    Text written(usize{1} << 20U, '\0');
    cont::Array<u8> instructions;
    instructions.resize(usize{1} << 20U);
    usize section_bytes = 0U;
    usize stream_bytes = 0U;
    for (usize index = 0U; index < sections.size(); ++index)
    {
        cont::Array<HttpField> list;
        for (const auto& field : sections[index])
        {
            list.push_back(HttpField{view(field.first), view(field.second), false});
        }
        const u64 stream = index + 1U;
        const usize bytes =
            encoder.encode(stream, ConstSpan<HttpField>(list.data(), list.size()),
                           Span<u8>(reinterpret_cast<u8*>(written.data()), written.size()));
        if (bytes == 0U)
        {
            std::fprintf(stderr, "section %zu not written\n", index);
            return 1;
        }
        section_bytes += bytes;
        const Text section = slice(written, 0U, bytes);
        const usize insert = encoder.take_instructions(Span<u8>(instructions.data(), instructions.size()));
        stream_bytes += insert;
        bool done = false;
        if (!decode.section(stream, section, sections, done))
        {
            return 1;
        }
        if (insert != 0U && !decode.decoder.on_encoder_stream(ConstSpan<u8>(instructions.data(), insert)))
        {
            std::fprintf(stderr, "own encoder stream refused\n");
            return 1;
        }
        if (!done)
        {
            bool again = false;
            if (!decode.section(stream, section, sections, again) || !again)
            {
                std::fprintf(stderr, "stream %llu stays blocked\n", static_cast<unsigned long long>(stream));
                return 1;
            }
        }
        u64 unblocked = 0U;
        while (decode.decoder.next_unblocked(unblocked))
        {
        }
        const usize ack = decode.decoder.take_instructions(Span<u8>(instructions.data(), instructions.size()));
        if (acknowledge && !encoder.on_decoder_stream(ConstSpan<u8>(instructions.data(), ack)))
        {
            std::fprintf(stderr, "own decoder stream refused\n");
            return 1;
        }
    }
    std::printf("%zu %zu\n", section_bytes, stream_bytes);
    return 0;
}

u32 number(const char* text)
{
    return static_cast<u32>(std::strtoul(text, nullptr, 10));
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 5 && std::strcmp(argv[1], "refuse") == 0)
    {
        Text encoded;
        if (!read_file(argv[2], encoded))
        {
            return 2;
        }
        return (run_decode(nullptr, encoded, number(argv[3]), number(argv[4])) == 1) ? 0 : 1;
    }
    if (argc == 6 && std::strcmp(argv[1], "decode") == 0)
    {
        Text qif;
        Text encoded;
        if (!read_file(argv[2], qif) || !read_file(argv[3], encoded))
        {
            return 2;
        }
        const cont::Array<Fields> sections = parse_qif(qif);
        return run_decode(&sections, encoded, number(argv[4]), number(argv[5]));
    }
    if (argc == 6 && std::strcmp(argv[1], "encode") == 0)
    {
        Text qif;
        if (!read_file(argv[2], qif))
        {
            return 2;
        }
        return run_encode(parse_qif(qif), number(argv[3]), number(argv[4]), number(argv[5]) != 0U);
    }
    std::fprintf(stderr, "usage: see the head of the source\n");
    return 2;
}
