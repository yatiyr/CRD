// crd-hpack-replay: crd-net's HPACK against the hpack-test-case corpus (4.2a; docs/design/network-foundation.md#hpack).
// Modes, run by hpack_interop.py over each story, which it writes out as lines:
//
//   decode <story>   decodes another implementation's blocks and compares every field with the story's.
//   encode <story>   runs crd-net's encoder over the story's fields at the default table size, proves each block
//                    through crd-net's decoder and prints the bytes it wrote.
//
// A story's lines: "S <size>" the table size in force for the next block, "W <hex>" a block, "F <hex> <hex>" a field,
// "E" the end of a case. Exit 0 on success, 1 on a difference or refusal, 2 for bad arguments or an unreadable file.

#include "../text_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/hpack.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

namespace
{
using namespace crd;
using namespace crd::net;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::testing::joined;
using crd::net::testing::Text;
using crd::net::testing::TextPair;
using crd::net::testing::view;
namespace cont = crd::containers;

constexpr usize kMaxFields = 4096U;
constexpr usize kMaxText = 1U << 20U;
constexpr u32 kMaxTable = 1U << 20U;

struct Case
{
    u32 size = 0U; // 0 for none named
    bool sized = false;
    Text wire;
    cont::Array<TextPair> fields;
};

u32 digit(char c)
{
    return (c >= 'a') ? static_cast<u32>(c - 'a') + 10U : static_cast<u32>(c - '0');
}

Text unhex(std::string_view text)
{
    Text out;
    for (usize at = 0U; at + 1U < text.size(); at += 2U)
    {
        out.push_back(static_cast<char>((digit(text[at]) << 4U) | digit(text[at + 1U])));
    }
    return out;
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
    for (usize got = std::fread(chunk, 1U, sizeof(chunk), file); got != 0U;
         got = std::fread(chunk, 1U, sizeof(chunk), file))
    {
        out.append(chunk, got);
    }
    std::fclose(file);
    return true;
}

cont::Array<Case> parse(std::string_view text)
{
    cont::Array<Case> cases;
    Case current;
    while (!text.empty())
    {
        usize end = text.find('\n');
        end = (end == std::string_view::npos) ? text.size() : end;
        const std::string_view line = text.substr(0U, end);
        text.remove_prefix((end < text.size()) ? end + 1U : end);
        if (line.empty())
        {
            continue;
        }
        const std::string_view rest = (line.size() > 2U) ? line.substr(2U) : std::string_view();
        switch (line[0])
        {
            case 'S':
                current.size = static_cast<u32>(std::strtoul(Text(rest).data(), nullptr, 10));
                current.sized = true;
                break;
            case 'W':
                current.wire = unhex(rest);
                break;
            case 'F':
            {
                const usize space = rest.find(' ');
                current.fields.push_back(TextPair(unhex(rest.substr(0U, space)), unhex(rest.substr(space + 1U))));
                break;
            }
            default:
                cases.push_back(current);
                current = Case();
                break;
        }
    }
    return cases;
}

Text expected_of(const Case& entry)
{
    Text out;
    for (const TextPair& field : entry.fields)
    {
        out.append(field.first);
        out.append("=");
        out.append(field.second);
        out.append("\n");
    }
    return out;
}

// Removes the marks joined() puts before never-indexed fields: a story lists only names and values.
Text without_marks(std::string_view text)
{
    Text out;
    bool line_start = true;
    for (const char c : text)
    {
        if (!(line_start && c == '!'))
        {
            out.push_back(c);
        }
        line_start = c == '\n';
    }
    return out;
}

// Whether `block` decodes to the case's fields; a story's `!` in a real name cannot occur, since names are tokens.
bool decode_block(HpackDecoder& decoder, ConstSpan<u8> block, const Case& entry, cont::Array<HttpField>& fields,
                  cont::Array<u8>& text)
{
    usize count = 0U;
    if (decoder.decode(block, Span<HttpField>(fields.data(), fields.size()), Span<u8>(text.data(), text.size()),
                       count) != HpackStatus::Complete)
    {
        std::fprintf(stderr, "block refused\n");
        return false;
    }
    if (without_marks(joined(ConstSpan<HttpField>(fields.data(), count))) != expected_of(entry))
    {
        std::fprintf(stderr, "fields differ\n");
        return false;
    }
    return true;
}

struct Storage
{
    Storage()
    {
        fields.resize(kMaxFields);
        text.resize(kMaxText);
    }

    cont::Array<HttpField> fields;
    cont::Array<u8> text;
};

int run_decode(const cont::Array<Case>& cases)
{
    HpackDecoder decoder;
    if (!decoder.init(*memory::default_allocator(), kMaxTable))
    {
        return 2;
    }
    Storage storage;
    for (const Case& entry : cases)
    {
        if (entry.sized)
        {
            decoder.set_limit(entry.size);
        }
        if (!decode_block(decoder, view(entry.wire), entry, storage.fields, storage.text))
        {
            return 1;
        }
    }
    return 0;
}

int run_encode(const cont::Array<Case>& cases)
{
    HpackEncoder encoder;
    HpackDecoder decoder;
    if (!encoder.init(*memory::default_allocator(), kHpackDefaultTableSize) ||
        !decoder.init(*memory::default_allocator(), kHpackDefaultTableSize))
    {
        return 2;
    }
    Storage storage;
    cont::Array<u8> block;
    block.resize(kMaxText);
    u64 total = 0U;
    for (const Case& entry : cases)
    {
        cont::Array<HttpField> lines;
        for (const TextPair& field : entry.fields)
        {
            lines.push_back(HttpField{view(field.first), view(field.second), false});
        }
        const usize written =
            encoder.encode(ConstSpan<HttpField>(lines.data(), lines.size()), Span<u8>(block.data(), block.size()));
        if (written == 0U ||
            !decode_block(decoder, ConstSpan<u8>(block.data(), written), entry, storage.fields, storage.text))
        {
            return 1;
        }
        total += written;
    }
    std::printf("%llu\n", static_cast<unsigned long long>(total));
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    Text story;
    if (argc != 3 || !read_file(argv[2], story))
    {
        std::fprintf(stderr, "usage: see the head of the source\n");
        return 2;
    }
    const cont::Array<Case> cases = parse(story);
    if (std::strcmp(argv[1], "decode") == 0)
    {
        return run_decode(cases);
    }
    if (std::strcmp(argv[1], "encode") == 0)
    {
        return run_encode(cases);
    }
    return 2;
}
