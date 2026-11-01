// test_zip.cpp — crd-compression's ZIP container (OWN.1, decision 8): writer and reader round trips through both
// methods, the written bytes against a hand-laid archive, deterministic archives, Zip64 read, the reader's views and
// exact-size decodes, every refusal class of the reader and the writer, and allocation refusals on a small pool.

#include <crd/compression/zip.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace
{
namespace zip = crd::compression;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;

[[nodiscard]] ConstSpan<crd::u8> sv(const char* s)
{
    return {reinterpret_cast<const crd::u8*>(s), std::strlen(s)};
}
[[nodiscard]] ConstSpan<crd::u8> view_of(const Array<crd::u8>& b)
{
    return {b.data(), b.size()};
}
[[nodiscard]] bool same(ConstSpan<crd::u8> a, ConstSpan<crd::u8> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

void wr16(Array<crd::u8>& b, crd::u16 v)
{
    b.push_back(static_cast<crd::u8>(v & 0xFFU));
    b.push_back(static_cast<crd::u8>(v >> 8U));
}
void wr32(Array<crd::u8>& b, crd::u32 v)
{
    for (crd::u32 s = 0; s < 32U; s += 8U)
    {
        b.push_back(static_cast<crd::u8>((v >> s) & 0xFFU));
    }
}
void wr64(Array<crd::u8>& b, crd::u64 v)
{
    for (crd::u32 s = 0; s < 64U; s += 8U)
    {
        b.push_back(static_cast<crd::u8>((v >> s) & 0xFFU));
    }
}
void wr_bytes(Array<crd::u8>& b, ConstSpan<crd::u8> bytes)
{
    for (const crd::u8 byte : bytes)
    {
        b.push_back(byte);
    }
}
[[nodiscard]] crd::u32 rd32_at(const Array<crd::u8>& b, crd::usize at)
{
    crd::u32 v = 0;
    for (crd::u32 s = 0; s < 32U; s += 8U)
    {
        v |= static_cast<crd::u32>(b[at + s / 8U]) << s;
    }
    return v;
}
void wr16_at(Array<crd::u8>& b, crd::usize at, crd::u16 v)
{
    b[at] = static_cast<crd::u8>(v & 0xFFU);
    b[at + 1U] = static_cast<crd::u8>(v >> 8U);
}
void wr32_at(Array<crd::u8>& b, crd::usize at, crd::u32 v)
{
    for (crd::u32 s = 0; s < 32U; s += 8U)
    {
        b[at + s / 8U] = static_cast<crd::u8>((v >> s) & 0xFFU);
    }
}

// The bytes of `zip_bytes` with no archive comment: the end record is the tail and names the directory.
[[nodiscard]] crd::usize directory_of(const Array<crd::u8>& zip_bytes)
{
    return rd32_at(zip_bytes, zip_bytes.size() - 22U + 16U);
}

[[nodiscard]] zip::ZipError open_copy(crd::memory::IAllocator* a, const Array<crd::u8>& bytes)
{
    zip::ZipReader r(a);
    return r.open(view_of(bytes));
}

// One archive of a compressible member (DEFLATE), four bytes DEFLATE cannot shrink (stored) and a STORED text part.
[[nodiscard]] Array<crd::u8> sample(crd::memory::IAllocator* a, const Array<crd::u8>& big)
{
    static constexpr crd::u8 kTiny[4] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    zip::ZipWriter w(a);
    REQUIRE(w.add("3D/model.xml", view_of(big), zip::ZipMethod::Deflate) == zip::ZipError::Ok);
    REQUIRE(w.add("tiny.bin", ConstSpan<crd::u8>(kTiny, 4U), zip::ZipMethod::Deflate) == zip::ZipError::Ok);
    REQUIRE(w.add("[Content_Types].xml", sv("<Types/>"), zip::ZipMethod::Stored) == zip::ZipError::Ok);
    Array<crd::u8> out(a);
    REQUIRE(w.finish(out) == zip::ZipError::Ok);
    return out;
}

// Counts the allocations it forwards to another allocator.
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    explicit CountingAllocator(crd::memory::IAllocator& inner) noexcept : m_inner(inner) {}

    void* allocate(crd::usize size, crd::usize alignment) override
    {
        ++m_count;
        return m_inner.allocate(size, alignment);
    }
    void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        ++m_count;
        return m_inner.try_allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { m_inner.deallocate(p); }
    bool owns(const void* p) const noexcept override { return m_inner.owns(p); }

    [[nodiscard]] crd::usize count() const noexcept { return m_count; }

private:
    crd::memory::IAllocator& m_inner;
    crd::usize m_count = 0;
};

[[nodiscard]] Array<crd::u8> compressible(crd::memory::IAllocator* a, crd::usize n)
{
    Array<crd::u8> big(a);
    for (crd::usize i = 0; i < n; ++i)
    {
        big.push_back(static_cast<crd::u8>(i % 7U));
    }
    return big;
}
} // namespace

TEST_CASE("zip: write -> read round trip through both methods, deterministic", "[compression][zip]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const Array<crd::u8> big = compressible(&alloc, 4096U);
    const Array<crd::u8> zip_a = sample(&alloc, big);
    const Array<crd::u8> zip_b = sample(&alloc, big);
    CHECK(same(view_of(zip_a), view_of(zip_b))); // deterministic: a fixed stamp, no incidental state
    CHECK(zip_a.size() < 4096U);                 // the big member deflated

    zip::ZipReader r(&alloc);
    REQUIRE(r.open(view_of(zip_a)) == zip::ZipError::Ok);
    REQUIRE(r.entry_count() == 3U);
    const crd::i64 model = r.find("3D/model.xml");
    const crd::i64 tiny = r.find("tiny.bin");
    const crd::i64 types = r.find("[Content_Types].xml");
    REQUIRE(model == 0);
    REQUIRE(tiny == 1);
    REQUIRE(types == 2);
    CHECK(r.find("missing") == -1);
    CHECK(r.find("3D/model.xm") == -1); // exact names only
    CHECK(r.entry(0).method == zip::ZipMethod::Deflate);
    CHECK(r.entry(1).method == zip::ZipMethod::Stored); // Deflate asked, stored because DEFLATE did not shrink it
    CHECK(r.entry(2).method == zip::ZipMethod::Stored);
    CHECK(r.entry(0).uncompressed_size == 4096U);
    CHECK(r.entry(0).crc32 == crd::containers::crc32(view_of(big)));

    Array<crd::u8> out(&alloc);
    REQUIRE(r.extract(0U, out) == zip::ZipError::Ok);
    CHECK(same(view_of(out), view_of(big)));
    REQUIRE(r.extract(1U, out) == zip::ZipError::Ok);
    REQUIRE(out.size() == 4U);
    CHECK(out[0] == 0xDEU);
    CHECK(out[3] == 0xEFU);

    // a STORED member reads as a view of the archive; a DEFLATE member has none
    ConstSpan<crd::u8> view;
    REQUIRE(r.view(2U, view) == zip::ZipError::Ok);
    CHECK(same(view, sv("<Types/>")));
    CHECK(view.data() >= zip_a.data());
    CHECK(view.data() + view.size() <= zip_a.data() + zip_a.size());
    CHECK(r.view(0U, view) == zip::ZipError::Unsupported);

    // either method decodes into a caller's buffer of exactly the member's size, and only that size
    Array<crd::u8> exact(&alloc);
    exact.resize(4096U);
    REQUIRE(r.extract(0U, Span<crd::u8>(exact.data(), exact.size())) == zip::ZipError::Ok);
    CHECK(same(view_of(exact), view_of(big)));
    CHECK(r.extract(0U, Span<crd::u8>(exact.data(), 4095U)) == zip::ZipError::Unsupported);
    CHECK(r.extract(2U, Span<crd::u8>(exact.data(), 9U)) == zip::ZipError::Unsupported);
    REQUIRE(r.extract(2U, Span<crd::u8>(exact.data(), 8U)) == zip::ZipError::Ok);
    CHECK(std::memcmp(exact.data(), "<Types/>", 8U) == 0);

    // a refused open leaves no entry behind
    CHECK(r.open(sv("not an archive at all, just some text")) == zip::ZipError::NotRecognized);
    CHECK(r.entry_count() == 0U);
}

TEST_CASE("zip: the written bytes are the hand-laid classic archive", "[compression][zip]")
{
    // one STORED member: local header, data, central record, end record; version 2.0, no flags, the DOS stamp
    // 1980-01-01 00:00, no extra fields, attributes or comments
    crd::memory::TlsfAllocator alloc(1U << 20U);
    const ConstSpan<crd::u8> payload = sv("hello, zip");
    const char* name = "a/b.txt";
    const crd::u32 crc = crd::containers::crc32(payload);

    Array<crd::u8> want(&alloc);
    wr32(want, 0x04034B50U);
    wr16(want, 20U);
    wr16(want, 0U);
    wr16(want, 0U);
    wr16(want, 0U);
    wr16(want, 0x21U);
    wr32(want, crc);
    wr32(want, static_cast<crd::u32>(payload.size()));
    wr32(want, static_cast<crd::u32>(payload.size()));
    wr16(want, 7U);
    wr16(want, 0U);
    wr_bytes(want, sv(name));
    wr_bytes(want, payload);
    const crd::u32 directory = static_cast<crd::u32>(want.size());
    wr32(want, 0x02014B50U);
    wr16(want, 20U);
    wr16(want, 20U);
    wr16(want, 0U);
    wr16(want, 0U);
    wr16(want, 0U);
    wr16(want, 0x21U);
    wr32(want, crc);
    wr32(want, static_cast<crd::u32>(payload.size()));
    wr32(want, static_cast<crd::u32>(payload.size()));
    wr16(want, 7U);
    wr16(want, 0U);
    wr16(want, 0U);
    wr16(want, 0U);
    wr16(want, 0U);
    wr32(want, 0U);
    wr32(want, 0U); // the local header's offset
    wr_bytes(want, sv(name));
    const crd::u32 directory_size = static_cast<crd::u32>(want.size()) - directory;
    wr32(want, 0x06054B50U);
    wr16(want, 0U);
    wr16(want, 0U);
    wr16(want, 1U);
    wr16(want, 1U);
    wr32(want, directory_size);
    wr32(want, directory);
    wr16(want, 0U);

    zip::ZipWriter w(&alloc);
    REQUIRE(w.add(name, payload, zip::ZipMethod::Stored) == zip::ZipError::Ok);
    Array<crd::u8> got(&alloc);
    REQUIRE(w.finish(got) == zip::ZipError::Ok);
    CHECK(same(view_of(got), view_of(want)));

    // the writer starts a new archive after finish: an empty one is the bare end record
    Array<crd::u8> empty(&alloc);
    REQUIRE(w.finish(empty) == zip::ZipError::Ok);
    REQUIRE(empty.size() == 22U);
    CHECK(rd32_at(empty, 0U) == 0x06054B50U);
    zip::ZipReader r(&alloc);
    REQUIRE(r.open(view_of(empty)) == zip::ZipError::Ok);
    CHECK(r.entry_count() == 0U);

    // an empty member is a valid STORED member, and its view is empty
    REQUIRE(w.add("empty", ConstSpan<crd::u8>(), zip::ZipMethod::Deflate) == zip::ZipError::Ok);
    Array<crd::u8> one(&alloc);
    REQUIRE(w.finish(one) == zip::ZipError::Ok);
    REQUIRE(r.open(view_of(one)) == zip::ZipError::Ok);
    REQUIRE(r.entry_count() == 1U);
    CHECK(r.entry(0).method == zip::ZipMethod::Stored);
    ConstSpan<crd::u8> view = sv("x");
    REQUIRE(r.view(0U, view) == zip::ZipError::Ok);
    CHECK(view.empty());
    Array<crd::u8> out(&alloc);
    REQUIRE(r.extract(0U, out) == zip::ZipError::Ok);
    CHECK(out.empty());
}

TEST_CASE("zip: the end record is found behind an archive comment, and a member written with a data descriptor reads",
          "[compression][zip]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    zip::ZipWriter w(&alloc);
    REQUIRE(w.add("f.txt", sv("the payload the payload the payload"), zip::ZipMethod::Deflate) == zip::ZipError::Ok);
    Array<crd::u8> base(&alloc);
    REQUIRE(w.finish(base) == zip::ZipError::Ok);
    const crd::usize directory = directory_of(base);

    // a comment after the end record: the back-scan finds the record behind it
    Array<crd::u8> commented(base, &alloc);
    const char* comment = "archive comment";
    wr16_at(commented, commented.size() - 2U, static_cast<crd::u16>(std::strlen(comment)));
    wr_bytes(commented, sv(comment));
    zip::ZipReader r(&alloc);
    REQUIRE(r.open(view_of(commented)) == zip::ZipError::Ok);
    Array<crd::u8> out(&alloc);
    REQUIRE(r.extract(0U, out) == zip::ZipError::Ok);
    CHECK(same(view_of(out), sv("the payload the payload the payload")));

    // general-purpose bit 3: the local header's CRC-32 and sizes are zero and a descriptor follows the data; the
    // central record's values are authoritative (APPNOTE 4.4.4), so the member reads like any other
    Array<crd::u8> described(base, &alloc);
    wr16_at(described, 6U, 0x0008U);
    wr32_at(described, 14U, 0U);
    wr32_at(described, 18U, 0U);
    wr32_at(described, 22U, 0U);
    wr16_at(described, directory + 8U, 0x0008U);
    REQUIRE(r.open(view_of(described)) == zip::ZipError::Ok);
    REQUIRE(r.extract(0U, out) == zip::ZipError::Ok);
    CHECK(same(view_of(out), sv("the payload the payload the payload")));
}

TEST_CASE("zip: Zip64 read -- sentinel fields resolve through the 0x0001 extra and the Zip64 end record",
          "[compression][zip]")
{
    // the reference 3MF toolchain's shape (lib3mf writes it for any size): one STORED member whose central sizes and
    // offset are all sentinels resolved by the 0x0001 extra, a sentinel classic end record, and the Zip64 end record
    // and locator holding the directory
    crd::memory::TlsfAllocator alloc(4U << 20U);
    const ConstSpan<crd::u8> payload = sv("zip64 payload");
    const char* name = "part.txt";
    const crd::usize name_len = std::strlen(name);
    const crd::u32 crc = crd::containers::crc32(payload);

    Array<crd::u8> z(&alloc);
    wr32(z, 0x04034B50U); // local header (real sizes are legal here; the central record carries the sentinels)
    wr16(z, 45U);         // version needed: 4.5, Zip64
    wr16(z, 0U);
    wr16(z, 0U);
    wr16(z, 0U);
    wr16(z, 0U);
    wr32(z, crc);
    wr32(z, static_cast<crd::u32>(payload.size()));
    wr32(z, static_cast<crd::u32>(payload.size()));
    wr16(z, static_cast<crd::u16>(name_len));
    wr16(z, 0U);
    wr_bytes(z, sv(name));
    wr_bytes(z, payload);

    const crd::u64 cdir_off = z.size();
    wr32(z, 0x02014B50U);
    wr16(z, 45U);
    wr16(z, 45U);
    wr16(z, 0U);
    wr16(z, 0U);
    wr16(z, 0U);
    wr16(z, 0U);
    wr32(z, crc);
    wr32(z, 0xFFFFFFFFU); // compressed size sentinel
    wr32(z, 0xFFFFFFFFU); // uncompressed size sentinel
    wr16(z, static_cast<crd::u16>(name_len));
    wr16(z, 28U); // extra: 4 + 8 * 3
    wr16(z, 0U);
    wr16(z, 0U);
    wr16(z, 0U);
    wr32(z, 0U);
    wr32(z, 0xFFFFFFFFU); // local offset sentinel
    wr_bytes(z, sv(name));
    wr16(z, 0x0001U); // uncompressed, compressed, local offset (the spec's order)
    wr16(z, 24U);
    wr64(z, payload.size());
    wr64(z, payload.size());
    wr64(z, 0U);
    const crd::u64 cdir_size = z.size() - cdir_off;

    const crd::u64 e64_off = z.size();
    wr32(z, 0x06064B50U);
    wr64(z, 44U); // the record's size past this field
    wr16(z, 45U);
    wr16(z, 45U);
    wr32(z, 0U); // this disk
    wr32(z, 0U); // the directory's disk
    wr64(z, 1U); // entries on this disk
    wr64(z, 1U); // entries in all
    wr64(z, cdir_size);
    wr64(z, cdir_off);
    const crd::usize locator = z.size();
    wr32(z, 0x07064B50U);
    wr32(z, 0U); // the record's disk
    wr64(z, e64_off);
    wr32(z, 1U); // disks
    wr32(z, 0x06054B50U);
    wr16(z, 0U);
    wr16(z, 0U);
    wr16(z, 0xFFFFU);
    wr16(z, 0xFFFFU);
    wr32(z, 0xFFFFFFFFU);
    wr32(z, 0xFFFFFFFFU);
    wr16(z, 0U);

    zip::ZipReader r(&alloc);
    REQUIRE(r.open(view_of(z)) == zip::ZipError::Ok);
    REQUIRE(r.entry_count() == 1U);
    CHECK(r.entry(0).uncompressed_size == payload.size()); // from the extra, not the sentinel
    CHECK(r.entry(0).compressed_size == payload.size());
    CHECK(r.entry(0).data_offset == 30U + name_len);
    ConstSpan<crd::u8> view;
    REQUIRE(r.view(0U, view) == zip::ZipError::Ok);
    CHECK(same(view, payload));

    const auto patched = [&](crd::usize at, crd::u8 value)
    {
        Array<crd::u8> bad(z, &alloc);
        bad[at] = value;
        return open_copy(&alloc, bad);
    };
    // a sentinel central record without the 0x0001 extra is a violation, not a quiet zero-size member
    CHECK(patched(static_cast<crd::usize>(cdir_off) + 46U + name_len, 0x02U) == zip::ZipError::Malformed);
    // an extra too short for the sentinel fields, and one that runs past its record
    CHECK(patched(static_cast<crd::usize>(cdir_off) + 46U + name_len + 2U, 16U) == zip::ZipError::Malformed);
    CHECK(patched(static_cast<crd::usize>(cdir_off) + 46U + name_len + 2U, 25U) == zip::ZipError::Malformed);
    // the locator and the record: a missing signature, a record past the locator, a spanned archive
    CHECK(patched(locator, 0x00U) == zip::ZipError::Malformed);
    CHECK(patched(static_cast<crd::usize>(e64_off), 0x00U) == zip::ZipError::Malformed);
    CHECK(patched(locator + 8U, 0xF0U) == zip::ZipError::Truncated);
    CHECK(patched(locator + 4U, 1U) == zip::ZipError::Unsupported);
    CHECK(patched(locator + 16U, 2U) == zip::ZipError::Unsupported);
    CHECK(patched(static_cast<crd::usize>(e64_off) + 16U, 1U) == zip::ZipError::Unsupported);
    CHECK(patched(static_cast<crd::usize>(e64_off) + 24U, 2U) == zip::ZipError::Unsupported);
    // a record size field too small for the record, and one that runs into the locator
    CHECK(patched(static_cast<crd::usize>(e64_off) + 4U, 43U) == zip::ZipError::Malformed);
    CHECK(patched(static_cast<crd::usize>(e64_off) + 4U, 45U) == zip::ZipError::Malformed);
    {
        Array<crd::u8> wraps(z, &alloc); // a size whose sum with its 12 leading bytes wraps past 2^64
        for (crd::usize i = 0; i < 8U; ++i)
        {
            wraps[static_cast<crd::usize>(e64_off) + 4U + i] = i == 0U ? 0xF8U : 0xFFU;
        }
        CHECK(open_copy(&alloc, wraps) == zip::ZipError::Malformed);
    }
    // a directory size or entry count the directory cannot hold
    CHECK(patched(static_cast<crd::usize>(e64_off) + 40U, static_cast<crd::u8>(cdir_size + 1U)) ==
          zip::ZipError::Truncated);
    CHECK(patched(static_cast<crd::usize>(e64_off) + 32U, 2U) == zip::ZipError::Unsupported); // on disk != total
}

TEST_CASE("zip: the reader's refusal classes", "[compression][zip]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    zip::ZipWriter w(&alloc);
    REQUIRE(w.add("f.txt", sv("the payload the payload the payload"), zip::ZipMethod::Deflate) == zip::ZipError::Ok);
    REQUIRE(w.add("g.txt", sv("stored"), zip::ZipMethod::Stored) == zip::ZipError::Ok);
    Array<crd::u8> good(&alloc);
    REQUIRE(w.finish(good) == zip::ZipError::Ok);
    const crd::usize cdir = directory_of(good);
    const crd::usize second = cdir + 46U + 5U; // the STORED member's central record
    REQUIRE(good[cdir + 10U] == 8U);
    REQUIRE(good[second + 10U] == 0U);
    const crd::u32 csize = rd32_at(good, cdir + 20U);
    const crd::u32 usize = rd32_at(good, cdir + 24U);

    const auto with32 = [&](crd::usize at, crd::u32 value)
    {
        Array<crd::u8> bad(good, &alloc);
        wr32_at(bad, at, value);
        return bad;
    };
    const auto with16 = [&](crd::usize at, crd::u16 value)
    {
        Array<crd::u8> bad(good, &alloc);
        wr16_at(bad, at, value);
        return bad;
    };

    SECTION("a member that does not decode to its CRC-32 or its size is refused at read, nothing partial kept")
    {
        Array<crd::u8> bad(good, &alloc);
        bad[35U] ^= 0xFFU; // the first member's data: past the 30-byte local header and the 5-byte name
        zip::ZipReader r(&alloc);
        REQUIRE(r.open(view_of(bad)) == zip::ZipError::Ok);
        Array<crd::u8> out(&alloc);
        CHECK(r.extract(0U, out) == zip::ZipError::Corrupt);
        CHECK(out.empty());

        const Array<crd::u8> bad_crc = with32(second + 16U, rd32_at(good, second + 16U) ^ 1U);
        REQUIRE(r.open(view_of(bad_crc)) == zip::ZipError::Ok);
        ConstSpan<crd::u8> view;
        CHECK(r.view(1U, view) == zip::ZipError::Corrupt);
        CHECK(r.extract(1U, out) == zip::ZipError::Corrupt);

        for (const crd::u32 claim : {usize + 1U, usize - 1U})
        {
            const Array<crd::u8> wrong = with32(cdir + 24U, claim);
            REQUIRE(r.open(view_of(wrong)) == zip::ZipError::Ok);
            CHECK(r.extract(0U, out) == zip::ZipError::Corrupt);
            CHECK(out.empty());
        }
    }
    SECTION("a DEFLATE size its compressed bytes cannot hold is refused at open, before anything is sized")
    {
        CHECK(open_copy(&alloc, with32(cdir + 24U, csize * 1032U + 1U)) == zip::ZipError::Corrupt);
        CHECK(open_copy(&alloc, with32(cdir + 24U, 0xFFFFFFF0U)) == zip::ZipError::Corrupt);
        CHECK(open_copy(&alloc, with32(cdir + 24U, csize * 1032U)) == zip::ZipError::Ok); // the bound itself
    }
    SECTION("structures past the bytes are Truncated")
    {
        CHECK(open_copy(&alloc, with32(cdir + 20U, static_cast<crd::u32>(good.size()))) == zip::ZipError::Truncated);
        CHECK(open_copy(&alloc, with32(cdir + 42U, static_cast<crd::u32>(good.size()) - 10U)) ==
              zip::ZipError::Truncated);
        CHECK(open_copy(&alloc, with16(good.size() - 22U + 10U, 3U)) == zip::ZipError::Unsupported); // 2 on disk
        Array<crd::u8> three = with16(good.size() - 22U + 10U, 3U);
        wr16_at(three, three.size() - 22U + 8U, 3U);
        CHECK(open_copy(&alloc, three) == zip::ZipError::Truncated); // a third record past the directory
        CHECK(open_copy(&alloc, with16(cdir + 30U, 0x4000U)) == zip::ZipError::Truncated); // an extra past it
        CHECK(open_copy(&alloc, with32(good.size() - 22U + 12U, rd32_at(good, good.size() - 22U + 12U) + 1U)) ==
              zip::ZipError::Truncated); // the directory runs into the end record
        CHECK(open_copy(&alloc, with32(good.size() - 22U + 16U, static_cast<crd::u32>(good.size()))) ==
              zip::ZipError::Truncated);
        zip::ZipReader r(&alloc);
        CHECK(r.open(ConstSpan<crd::u8>(good.data(), good.size() - 30U)) == zip::ZipError::NotRecognized);
        CHECK(r.open(ConstSpan<crd::u8>(good.data(), 21U)) == zip::ZipError::NotRecognized);
        CHECK(r.open(ConstSpan<crd::u8>()) == zip::ZipError::NotRecognized);
    }
    SECTION("a signature or field the format forbids is Malformed")
    {
        CHECK(open_copy(&alloc, with32(cdir, 0x01020304U)) == zip::ZipError::Malformed);
        CHECK(open_copy(&alloc, with32(0U, 0x01020304U)) == zip::ZipError::Malformed);  // the local header
        CHECK(open_copy(&alloc, with32(second + 20U, 7U)) == zip::ZipError::Malformed); // STORED sizes differ
        // a sentinel classic end record with no Zip64 locator before it
        CHECK(open_copy(&alloc, with16(good.size() - 22U + 10U, 0xFFFFU)) == zip::ZipError::Malformed);
    }
    SECTION("encryption, spanned archives and other methods are Unsupported")
    {
        CHECK(open_copy(&alloc, with16(cdir + 8U, 0x0001U)) == zip::ZipError::Unsupported);
        CHECK(open_copy(&alloc, with16(cdir + 8U, 0x0041U)) == zip::ZipError::Unsupported); // strong encryption
        CHECK(open_copy(&alloc, with16(cdir + 10U, 12U)) == zip::ZipError::Unsupported);    // bzip2
        CHECK(open_copy(&alloc, with16(cdir + 10U, 9U)) == zip::ZipError::Unsupported);     // DEFLATE64
        CHECK(open_copy(&alloc, with16(good.size() - 22U + 4U, 1U)) == zip::ZipError::Unsupported);
        CHECK(open_copy(&alloc, with16(good.size() - 22U + 6U, 1U)) == zip::ZipError::Unsupported);
    }
    SECTION("not an archive")
    {
        zip::ZipReader r(&alloc);
        CHECK(r.open(sv("just some text, definitely not an archive")) == zip::ZipError::NotRecognized);
        CHECK(r.entry_count() == 0U);
    }
}

TEST_CASE("zip: the writer's refusals keep the archive so far", "[compression][zip]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    zip::ZipWriter w(&alloc);
    CHECK(w.add("", sv("x"), zip::ZipMethod::Stored) == zip::ZipError::Malformed);

    Array<char> long_name(&alloc);
    long_name.resize(0x10000U, 'n');
    CHECK(w.add(crd::containers::StringView(long_name.data(), 0x10000U), sv("x"), zip::ZipMethod::Stored) ==
          zip::ZipError::Unsupported);
    REQUIRE(w.add(crd::containers::StringView(long_name.data(), 0xFFFFU), sv("x"), zip::ZipMethod::Stored) ==
            zip::ZipError::Ok); // the longest name the field holds

    // 65534 members fit; a 65535th would write the count's Zip64 sentinel
    for (crd::u32 i = 1U; i < 0xFFFEU; ++i)
    {
        REQUIRE(w.add("m", ConstSpan<crd::u8>(), zip::ZipMethod::Stored) == zip::ZipError::Ok);
    }
    CHECK(w.add("m", ConstSpan<crd::u8>(), zip::ZipMethod::Stored) == zip::ZipError::Unsupported);
    Array<crd::u8> archive(&alloc);
    REQUIRE(w.finish(archive) == zip::ZipError::Ok);
    zip::ZipReader r(&alloc);
    REQUIRE(r.open(view_of(archive)) == zip::ZipError::Ok);
    REQUIRE(r.entry_count() == 0xFFFEU);
    CHECK(r.entry(0).name.size() == 0xFFFFU);
    CHECK(r.find("m") == 1);
}

TEST_CASE("zip: a reserved archive is written in the one allocation", "[compression][zip]")
{
    crd::memory::TlsfAllocator pool(1U << 20U);
    CountingAllocator counting(pool);
    const char* names[3] = {"a.npy", "bb.npy", "ccc.npy"};
    const ConstSpan<crd::u8> data[3] = {sv("first"), ConstSpan<crd::u8>(), sv("the third member")};
    crd::usize name_bytes = 0;
    crd::usize data_bytes = 0;
    for (crd::usize i = 0; i < 3U; ++i)
    {
        name_bytes += std::strlen(names[i]);
        data_bytes += data[i].size();
    }

    zip::ZipWriter w(&counting);
    REQUIRE(w.reserve(3U, name_bytes, data_bytes) == zip::ZipError::Ok);
    const crd::usize reserved = counting.count();
    zip::ZipWriter plain(&pool);
    for (crd::usize i = 0; i < 3U; ++i)
    {
        REQUIRE(w.add(names[i], data[i], zip::ZipMethod::Stored) == zip::ZipError::Ok);
        REQUIRE(plain.add(names[i], data[i], zip::ZipMethod::Stored) == zip::ZipError::Ok);
    }
    Array<crd::u8> out(&counting);
    REQUIRE(w.finish(out) == zip::ZipError::Ok);
    CHECK(counting.count() == reserved); // no allocation past the reservation
    CHECK(out.size() == 3U * (30U + 46U) + 2U * name_bytes + data_bytes + 22U);
    Array<crd::u8> unreserved(&pool);
    REQUIRE(plain.finish(unreserved) == zip::ZipError::Ok);
    CHECK(same(view_of(out), view_of(unreserved))); // the same archive

    // a reservation no allocator holds, or this pool does not, is refused and the writer goes on
    zip::ZipWriter refused(&pool);
    CHECK(refused.reserve(1U, 1U, 2U << 20U) == zip::ZipError::OutOfMemory);
    CHECK(refused.reserve(~crd::usize{0} / 8U, 0U, 0U) == zip::ZipError::OutOfMemory);
    CHECK(refused.reserve(1U, 0U, ~crd::usize{0} - 10U) == zip::ZipError::OutOfMemory);
    REQUIRE(refused.add("x", sv("y"), zip::ZipMethod::Stored) == zip::ZipError::Ok);
    Array<crd::u8> one(&pool);
    REQUIRE(refused.finish(one) == zip::ZipError::Ok);
    zip::ZipReader r(&pool);
    REQUIRE(r.open(view_of(one)) == zip::ZipError::Ok);
    CHECK(r.entry_count() == 1U);
}

TEST_CASE("zip: allocation refusals are statuses, never fatal", "[compression][zip]")
{
    crd::memory::TlsfAllocator big(32U << 20U);
    crd::memory::TlsfAllocator small(1U << 20U);

    SECTION("the writer: a member the pool cannot hold is refused and the archive so far is kept")
    {
        const Array<crd::u8> payload = compressible(&big, 2U << 20U);
        zip::ZipWriter w(&small);
        REQUIRE(w.add("first", sv("kept"), zip::ZipMethod::Stored) == zip::ZipError::Ok);
        CHECK(w.add("too-big", view_of(payload), zip::ZipMethod::Stored) == zip::ZipError::OutOfMemory);
        REQUIRE(w.add("last", sv("kept too"), zip::ZipMethod::Stored) == zip::ZipError::Ok);
        Array<crd::u8> out(&big);
        REQUIRE(w.finish(out) == zip::ZipError::Ok);
        zip::ZipReader r(&big);
        REQUIRE(r.open(view_of(out)) == zip::ZipError::Ok);
        REQUIRE(r.entry_count() == 2U);
        ConstSpan<crd::u8> view;
        REQUIRE(r.view(1U, view) == zip::ZipError::Ok);
        CHECK(same(view, sv("kept too")));
    }
    SECTION("the reader: a directory larger than the pool, and a member larger than it")
    {
        zip::ZipWriter w(&big);
        for (crd::u32 i = 0; i < 30000U; ++i)
        {
            REQUIRE(w.add("e", ConstSpan<crd::u8>(), zip::ZipMethod::Stored) == zip::ZipError::Ok);
        }
        Array<crd::u8> many(&big);
        REQUIRE(w.finish(many) == zip::ZipError::Ok);
        zip::ZipReader r(&small);
        CHECK(r.open(view_of(many)) == zip::ZipError::OutOfMemory);
        CHECK(r.entry_count() == 0U);

        const Array<crd::u8> payload = compressible(&big, 2U << 20U);
        REQUIRE(w.add("big", view_of(payload), zip::ZipMethod::Deflate) == zip::ZipError::Ok);
        Array<crd::u8> one(&big);
        REQUIRE(w.finish(one) == zip::ZipError::Ok);
        REQUIRE(r.open(view_of(one)) == zip::ZipError::Ok);
        Array<crd::u8> out(&small);
        CHECK(r.extract(0U, out) == zip::ZipError::OutOfMemory);
        CHECK(out.empty());
        Array<crd::u8> enough(&big);
        REQUIRE(r.extract(0U, enough) == zip::ZipError::Ok);
        CHECK(same(view_of(enough), view_of(payload)));
    }
}
