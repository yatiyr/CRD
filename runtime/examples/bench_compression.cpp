// OWN.1's before/after boards for the compression codecs crd-compression owns: raw DEFLATE and inflate, the zlib
// wrapper and Adler-32, and the zstd binding the CRDR container compresses and decompresses its chunks through. Each
// board is two builds of this program (the engine before the change and HEAD) as two commands of scripts/ab-board.py:
// `time <case> <ns per KiB of input>` gives each build's arm, and `bits <case> <XXH3-64 of the case's output>` lets
// --same hold both builds to the same bytes. The inflate cases decode as their consumers do: inflate-raw and
// zlib-inflate into an output allocated per call (a zip member, a PNG or EXR stream), inflate-exact into a buffer
// sized once (an npz member in its reader's backing store). The ZIP container's cases run its consumers' shapes: a
// 3MF-like package of eight DEFLATE members written and read back member by member (zip-write, zip-read; the same
// members deflated alone, deflate-members, are the codec's share of zip-write, the control that leaves the
// container's own cost), and NumPy
// archives of eight f32 members, written STORED as np.savez does (npz-write) and read through hesap-tensor's npz
// reader, STORED and DEFLATE (npz-read-stored, npz-read-deflate). The diagnostic bundle's cases write the input as a
// flight bundle's log tail beside a crash record and read every section back (bundle-write, bundle-read; `bytes
// bundle` is the file's size): crd-perf's sections compress through crd-compression since 2026-11-01.

#include <crd/compression/deflate.hpp>
#include <crd/compression/zip.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/core/fnv1a.hpp>
#include <crd/core/seed_generators.hpp>
#include <crd/hesap/tensor/io.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/resources/crdr.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>

using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
namespace codec = crd::compression;
namespace tensor = crd::hesap::tensor;

namespace
{
volatile u64 g_sink = 0U;

constexpr usize kInputBytes = static_cast<usize>(256) * 1024U;
constexpr double kInputKiB = static_cast<double>(kInputBytes) / 1024.0;
constexpr u32 kChunk = crd::resources::make_fourcc('D', 'A', 'T', 'A');
constexpr usize kMembers = 8U;
constexpr usize kMemberBytes = kInputBytes / kMembers;

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

// Member `m` of the input's eight.
ConstSpan<u8> member(ConstSpan<u8> input, usize m)
{
    return input.subspan(m * kMemberBytes, kMemberBytes);
}

// The input as a 3MF-like package: eight members "part<m>.xml", each DEFLATE (STORED where it does not shrink).
Array<u8> zip_package(crd::memory::IAllocator* alloc, ConstSpan<u8> input)
{
    codec::ZipWriter writer(alloc);
    char name[] = "part0.xml";
    bool ok = true;
    for (usize m = 0; m < kMembers; ++m)
    {
        name[4] = static_cast<char>('0' + m);
        ok = ok && writer.add(name, member(input, m), codec::ZipMethod::Deflate) == codec::ZipError::Ok;
    }
    Array<u8> out(alloc);
    ok = ok && writer.finish(out) == codec::ZipError::Ok;
    if (!ok)
    {
        out.clear();
    }
    return out;
}

// The codec's work in zip_package: each member deflated alone; the members' digests folded in order.
u64 deflate_members(crd::memory::IAllocator* alloc, ConstSpan<u8> input, bool digest)
{
    u64 bits = kMembers;
    for (usize m = 0; m < kMembers; ++m)
    {
        const Array<u8> deflated = codec::deflate_raw(member(input, m), alloc);
        bits = digest ? crd::hash_combine(bits, crd::containers::xxh3_64(view(deflated))) : bits + deflated.size();
    }
    return bits;
}

// Every member of `package` extracted as the cooker does (an array each); the members' digests folded in order, or
// 0 when a read fails.
u64 zip_read(crd::memory::IAllocator* alloc, ConstSpan<u8> package, bool digest)
{
    codec::ZipReader reader(alloc);
    if (reader.open(package) != codec::ZipError::Ok)
    {
        return 0U;
    }
    u64 bits = reader.entry_count();
    for (usize i = 0; i < reader.entry_count(); ++i)
    {
        Array<u8> out(alloc);
        if (reader.extract(i, out) != codec::ZipError::Ok)
        {
            return 0U;
        }
        bits = digest ? crd::hash_combine(bits, crd::containers::xxh3_64(view(out))) : bits + out.size();
    }
    return bits;
}

// The input as an npz of eight f32 members "m<m>", STORED as np.savez writes them.
Array<u8> npz_stored(crd::memory::IAllocator* alloc, ConstSpan<u8> input)
{
    tensor::NpzWriter writer(alloc);
    const u64 shape[1] = {kMemberBytes / sizeof(float)};
    char key[] = "m0";
    bool ok = true;
    for (usize m = 0; m < kMembers; ++m)
    {
        key[1] = static_cast<char>('0' + m);
        ok = ok && writer.add(key, tensor::IoDtype::F32, {shape, 1U}, member(input, m)) == tensor::TensorStatus::Ok;
    }
    Array<u8> out(alloc);
    ok = ok && writer.finish(out) == tensor::TensorStatus::Ok;
    if (!ok)
    {
        out.clear();
    }
    return out;
}

// The same members as np.savez_compressed writes them: each .npy member DEFLATE.
Array<u8> npz_deflated(crd::memory::IAllocator* alloc, ConstSpan<u8> input)
{
    codec::ZipWriter writer(alloc);
    const u64 shape[1] = {kMemberBytes / sizeof(float)};
    char name[] = "m0.npy";
    bool ok = true;
    for (usize m = 0; m < kMembers; ++m)
    {
        name[1] = static_cast<char>('0' + m);
        Array<u8> npy(alloc);
        ok = ok && tensor::npy_encode(alloc, tensor::IoDtype::F32, {shape, 1U}, member(input, m), npy) ==
                       tensor::TensorStatus::Ok;
        ok = ok && writer.add(name, view(npy), codec::ZipMethod::Deflate) == codec::ZipError::Ok;
    }
    Array<u8> out(alloc);
    ok = ok && writer.finish(out) == codec::ZipError::Ok;
    if (!ok)
    {
        out.clear();
    }
    return out;
}

// Every member of an npz parsed through hesap-tensor's reader; the payloads' digests folded in order, or 0 when a
// read fails.
u64 npz_read(crd::memory::IAllocator* alloc, ConstSpan<u8> archive, bool digest)
{
    tensor::NpzReader reader(alloc);
    if (reader.parse(archive) != tensor::TensorStatus::Ok)
    {
        return 0U;
    }
    u64 bits = reader.count();
    for (usize i = 0; i < reader.count(); ++i)
    {
        tensor::NpyView v;
        if (reader.npy(i, v) != tensor::TensorStatus::Ok)
        {
            return 0U;
        }
        bits = digest ? crd::hash_combine(bits, crd::containers::xxh3_64(v.payload)) : bits + v.payload.size();
    }
    return bits;
}

// The CRDR container of `input` as one zstd-compressed chunk (level 3, the cooker's).
Array<u8> crdr_compressed(crd::memory::IAllocator* alloc, ConstSpan<u8> input)
{
    crd::resources::CrdrWriter writer(alloc, crd::resources::ResourceId{1U, 2U}, kChunk);
    writer.add_chunk_compressed(kChunk, input, 3);
    return writer.finish();
}

// A diagnostic bundle as the flight recorder writes one: the input as its log tail, its first member as the crash
// record and an explicit absent symbol index, stamped at time 0 so the bytes are reproducible.
Array<u8> bundle_of(ConstSpan<u8> input)
{
    crd::perf::BundleWriter writer;
    (void)writer.add_section(crd::perf::BundleSectionTag::CrashRecord, member(input, 0U));
    (void)writer.add_section(crd::perf::BundleSectionTag::LogTail, input);
    (void)writer.add_absent(crd::perf::BundleSectionTag::SymbolIndex);
    return writer.finish(0U);
}

// Every section of a bundle as read back; the payloads' digests folded in order, or 0 when the read fails.
u64 bundle_read(ConstSpan<u8> bundle, bool digest)
{
    const crd::perf::BundleReadResult read = crd::perf::read_bundle(bundle);
    if (read.status != crd::perf::BundleReadStatus::Ok)
    {
        return 0U;
    }
    u64 bits = read.sections.size();
    for (const crd::perf::BundleSectionView& v : read.sections)
    {
        bits = digest ? crd::hash_combine(bits, crd::containers::xxh3_64(v.payload)) : bits + v.payload.size();
    }
    return bits;
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

    const auto inflate_into = [&](auto inflate, ConstSpan<u8> comp, Array<u8>& out)
    {
        out.resize_uninitialized(kInputBytes);
        return inflate(comp, {out.data(), out.size()});
    };
    Array<u8> inflated(&alloc);
    const bool raw_ok = inflate_into(codec::inflate_raw, view(raw), inflated);
    report("inflate-raw",
           ns_per_kib(
               [&]
               {
                   Array<u8> out(&alloc);
                   g_sink = g_sink + (inflate_into(codec::inflate_raw, view(raw), out) ? out.size() : 0U);
               }),
           raw_ok ? digest(inflated) : 0U);
    Array<u8> backing(&alloc);
    backing.resize_uninitialized(kInputBytes);
    const auto inflate_exact = [&]
    {
        return codec::inflate_raw(view(raw), {backing.data(), backing.size()});
    };
    const bool exact_ok = inflate_exact();
    const u64 exact_bits = exact_ok ? digest(backing) : 0U;
    report("inflate-exact", ns_per_kib([&] { g_sink = g_sink + (inflate_exact() ? 1U : 0U); }), exact_bits);

    const Array<u8> zlib = codec::zlib_deflate(in, &alloc);
    report("zlib-deflate", ns_per_kib([&] { g_sink = g_sink + codec::zlib_deflate(in, &alloc).size(); }), digest(zlib));

    Array<u8> unzlibbed(&alloc);
    const bool zlib_ok = inflate_into(codec::zlib_inflate, view(zlib), unzlibbed);
    report("zlib-inflate",
           ns_per_kib(
               [&]
               {
                   Array<u8> out(&alloc);
                   g_sink = g_sink + (inflate_into(codec::zlib_inflate, view(zlib), out) ? out.size() : 0U);
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

    report("deflate-members", ns_per_kib([&] { g_sink = g_sink + deflate_members(&alloc, in, false); }),
           deflate_members(&alloc, in, true));
    const Array<u8> package = zip_package(&alloc, in);
    report("zip-write", ns_per_kib([&] { g_sink = g_sink + zip_package(&alloc, in).size(); }), digest(package));
    const u64 package_bits = zip_read(&alloc, view(package), true);
    report("zip-read", ns_per_kib([&] { g_sink = g_sink + zip_read(&alloc, view(package), false); }), package_bits);

    const Array<u8> stored = npz_stored(&alloc, in);
    report("npz-write", ns_per_kib([&] { g_sink = g_sink + npz_stored(&alloc, in).size(); }), digest(stored));
    const u64 stored_bits = npz_read(&alloc, view(stored), true);
    report("npz-read-stored", ns_per_kib([&] { g_sink = g_sink + npz_read(&alloc, view(stored), false); }),
           stored_bits);
    const Array<u8> deflated = npz_deflated(&alloc, in);
    const u64 deflated_bits = npz_read(&alloc, view(deflated), true);
    report("npz-read-deflate", ns_per_kib([&] { g_sink = g_sink + npz_read(&alloc, view(deflated), false); }),
           deflated_bits);

    const Array<u8> bundle = bundle_of(in);
    report("bundle-write", ns_per_kib([&] { g_sink = g_sink + bundle_of(in).size(); }), digest(bundle));
    std::printf("bytes bundle %zu\n", static_cast<std::size_t>(bundle.size()));
    const u64 bundle_bits = bundle_read(view(bundle), true);
    report("bundle-read", ns_per_kib([&] { g_sink = g_sink + bundle_read(view(bundle), false); }), bundle_bits);

    const bool zip_ok = !package.empty() && package_bits != 0U && !stored.empty() && stored_bits != 0U &&
                        !deflated.empty() && deflated_bits != 0U;
    return raw_ok && exact_ok && zlib_ok && read_ok && zip_ok && bundle_bits != 0U ? 0 : 1;
}
