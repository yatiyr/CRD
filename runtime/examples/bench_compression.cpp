// OWN.1's before/after board for the compression codecs moved from crd-resources into crd-compression: raw DEFLATE
// and inflate, the zlib wrapper and Adler-32, and the zstd binding the CRDR container compresses and decompresses its
// chunks through. The move changes no code path, so the board is two builds of this program (the engine before the
// move and HEAD) as two commands of scripts/ab-board.py: `time <case> <ns per KiB of input>` gives each build's arm,
// and `bits <case> <XXH3-64 of the case's output>` lets --same hold both builds to the same bytes.

#include <crd/compression/deflate.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/core/seed_generators.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/crdr.hpp>

#include <chrono>
#include <cstdio>

using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
namespace codec = crd::compression;

namespace
{
volatile u64 g_sink = 0U;

constexpr usize kInputBytes = static_cast<usize>(256) * 1024U;
constexpr double kInputKiB = static_cast<double>(kInputBytes) / 1024.0;
constexpr u32 kChunk = crd::resources::make_fourcc('D', 'A', 'T', 'A');

// Text-like input that both codecs compress: words of a small vocabulary chosen by SplitMix64, with a run of raw
// generator bytes every 4 KiB so the literal paths run too.
Array<u8> make_input(crd::memory::IAllocator* alloc)
{
    static constexpr const char* kWords[] = {"cerid ", "engine ", "chunk ",  "frame ", "deflate ", "inflate ",
                                             "zstd ",  "owner ",  "module ", "bytes ", "\n",       "0123 "};
    Array<u8> input(alloc);
    input.reserve(kInputBytes);
    u64 state = 0x0123456789ABCDEFULL;
    while (input.size() < kInputBytes)
    {
        if (input.size() % 4096U < 8U)
        {
            const u64 noise = crd::splitmix64_next(state);
            for (usize b = 0; b < 8U && input.size() < kInputBytes; ++b)
            {
                input.push_back(static_cast<u8>(noise >> (8U * b)));
            }
            continue;
        }
        const char* word = kWords[crd::splitmix64_next(state) % (sizeof(kWords) / sizeof(kWords[0]))];
        for (const char* c = word; *c != '\0' && input.size() < kInputBytes; ++c)
        {
            input.push_back(static_cast<u8>(*c));
        }
    }
    return input;
}

ConstSpan<u8> view(const Array<u8>& bytes)
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

// Nanoseconds per KiB of input of `body()`, the median of five timed runs after a warm one, each long enough (~20 ms)
// that the clock's resolution does not show.
template <typename Body> double ns_per_kib(Body body)
{
    body();
    usize reps = 1U;
    for (;;)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (usize r = 0; r < reps; ++r)
        {
            body();
        }
        const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= 2.0e7)
        {
            break;
        }
        reps *= 2U;
    }
    double runs[5] = {};
    for (double& run : runs)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (usize r = 0; r < reps; ++r)
        {
            body();
        }
        run = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
              (static_cast<double>(reps) * kInputKiB);
    }
    for (usize i = 1; i < 5U; ++i) // insertion sort of five
    {
        for (usize j = i; j > 0U && runs[j - 1U] > runs[j]; --j)
        {
            const double t = runs[j];
            runs[j] = runs[j - 1U];
            runs[j - 1U] = t;
        }
    }
    return runs[2];
}

void report(const char* name, double ns, u64 bits)
{
    std::printf("time %s %.3f\n", name, ns);
    std::printf("bits %s %016llx\n", name, static_cast<unsigned long long>(bits));
}

// The CRDR container of `input` as one zstd-compressed chunk (level 3, the cooker's).
Array<u8> crdr_compressed(crd::memory::IAllocator* alloc, ConstSpan<u8> input)
{
    crd::resources::CrdrWriter writer(alloc, crd::resources::ResourceId{1U, 2U}, kChunk);
    writer.add_chunk_compressed(kChunk, input, 3);
    return writer.finish();
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 26);
    const Array<u8> input = make_input(&alloc);
    const ConstSpan<u8> in = view(input);
    const auto digest = [](const Array<u8>& bytes)
    {
        return crd::containers::xxh3_64(view(bytes));
    };

    const Array<u8> raw = codec::deflate_raw(in, &alloc);
    report("deflate-raw", ns_per_kib([&] { g_sink = g_sink + codec::deflate_raw(in, &alloc).size(); }), digest(raw));

    Array<u8> inflated(&alloc);
    const bool raw_ok = codec::inflate_raw(view(raw), inflated);
    report("inflate-raw",
           ns_per_kib(
               [&]
               {
                   Array<u8> out(&alloc);
                   g_sink = g_sink + (codec::inflate_raw(view(raw), out) ? out.size() : 0U);
               }),
           raw_ok ? digest(inflated) : 0U);

    const Array<u8> zlib = codec::zlib_deflate(in, &alloc);
    report("zlib-deflate", ns_per_kib([&] { g_sink = g_sink + codec::zlib_deflate(in, &alloc).size(); }), digest(zlib));

    Array<u8> unzlibbed(&alloc);
    const bool zlib_ok = codec::zlib_inflate(view(zlib), unzlibbed);
    report("zlib-inflate",
           ns_per_kib(
               [&]
               {
                   Array<u8> out(&alloc);
                   g_sink = g_sink + (codec::zlib_inflate(view(zlib), out) ? out.size() : 0U);
               }),
           zlib_ok ? digest(unzlibbed) : 0U);

    report("adler32", ns_per_kib([&] { g_sink = g_sink + codec::adler32(in); }), codec::adler32(in));

    const Array<u8> container = crdr_compressed(&alloc, in);
    report("zstd-compress", ns_per_kib([&] { g_sink = g_sink + crdr_compressed(&alloc, in).size(); }),
           digest(container));

    crd::resources::CrdrFile file(&alloc);
    const bool read_ok = crd::resources::crdr_read(view(container), file, &alloc) == crd::resources::CrdrError::Ok &&
                         file.chunks.size() == 1U && (file.chunks[0].flags & 0x1U) != 0U;
    report("zstd-decompress",
           ns_per_kib(
               [&]
               {
                   crd::resources::CrdrFile again(&alloc);
                   g_sink = g_sink + static_cast<u64>(crd::resources::crdr_read(view(container), again, &alloc));
               }),
           read_ok ? crd::containers::xxh3_64(file.chunks[0].payload) : 0U);
    return raw_ok && zlib_ok && read_ok ? 0 : 1;
}
