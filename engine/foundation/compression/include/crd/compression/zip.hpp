#pragma once

// zip.hpp — the ZIP container (PKZIP APPNOTE 6.3.10), the engine's one: 3MF and the OPC packages the cooker reads
// and writes, and NumPy's npz archives (OWN.1, capability compression.zip-archives, decision 8 of
// docs/design/capability-ownership.md). Its members are STORED or DEFLATE, decoded through this module's inflate.
//
// Read: the classic format and Zip64 (the 3MF reference toolchain writes Zip64 structures for any size, and NumPy
// does past 4 GiB). `open` finds the end-of-central-directory record by a bounded back-scan (a comment is at most
// 64 KiB), follows the Zip64 locator when the classic fields hold their sentinels, walks the central directory
// (sentinel sizes and offsets resolved through the 0x0001 extra field) and resolves every member's data through its
// local header, so a structure fault is refused before any member is read. The central directory's sizes and CRC-32
// are authoritative (a member written with a data descriptor reads like any other). A member reads as a view of the
// archive (STORED) or decodes into exactly its size (either method); its CRC-32 is checked on every read, a mismatch
// a refusal. Encryption, spanned archives and methods other than STORED and DEFLATE are refused as Unsupported.
//
// Write: the classic format (no Zip64 structures; an archive past its 16- and 32-bit fields is refused), members in
// `add` order, deterministic bytes: the DOS stamp is 1980-01-01 00:00 (the reproducible-build epoch, the first valid
// DOS date), no extra fields and no comment.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::compression
{

enum class ZipError : crd::u8
{
    Ok = 0,
    NotRecognized, // no end-of-central-directory record in range: not a ZIP archive
    Truncated,     // a structure or member reaches past the bytes
    Malformed,     // a signature or field the format forbids
    Corrupt,       // a member that does not decode to its size and CRC-32, or claims more than its bytes can hold
    Unsupported,   // encryption, a spanned archive, another method; a write past the classic fields; a read the
                   // member cannot serve (a compressed member's view, a buffer not its size)
    OutOfMemory,   // the allocator refused
};

enum class ZipMethod : crd::u8 // the format's 16-bit method field holds one of these
{
    Stored = 0,
    Deflate = 8,
};

// One member, as the central directory describes it.
struct ZipEntry
{
    crd::containers::StringView name; // views the archive bytes ('/'-separated, the OPC convention)
    crd::u64 compressed_size = 0;
    crd::u64 uncompressed_size = 0;
    crd::u64 data_offset = 0; // the member's bytes in the archive, past its local header
    crd::u32 crc32 = 0;
    ZipMethod method = ZipMethod::Stored;
};

class ZipReader
{
public:
    explicit ZipReader(crd::memory::IAllocator* a) noexcept : m_entries(a) {}

    // Parse the directory and resolve every member's data. `bytes` must outlive the reader: names and views refer to
    // it. No entry survives a refusal.
    [[nodiscard]] ZipError open(crd::containers::ConstSpan<crd::u8> bytes) noexcept;

    [[nodiscard]] crd::usize entry_count() const noexcept { return m_entries.size(); }
    [[nodiscard]] const ZipEntry& entry(crd::usize i) const noexcept;
    // The index of the member named exactly `name`; -1 when absent.
    [[nodiscard]] crd::i64 find(crd::containers::StringView name) const noexcept;

    // A STORED member's bytes as a view of the archive, CRC-32 checked; Unsupported for a DEFLATE member.
    [[nodiscard]] ZipError view(crd::usize i, crd::containers::ConstSpan<crd::u8>& out) const noexcept;
    // Member `i` decoded into `out`, whose size must be the member's `uncompressed_size`; CRC-32 checked. Nothing is
    // allocated; `out` holds unspecified bytes after a refusal.
    [[nodiscard]] ZipError extract(crd::usize i, crd::containers::Span<crd::u8> out) const noexcept;
    // Member `i` decoded into `out`, sized to it; `out` is left empty after a refusal.
    [[nodiscard]] ZipError extract(crd::usize i, crd::containers::Array<crd::u8>& out) const noexcept;

private:
    crd::containers::Array<ZipEntry> m_entries;
    crd::containers::ConstSpan<crd::u8> m_bytes;
};

class ZipWriter
{
public:
    explicit ZipWriter(crd::memory::IAllocator* a) noexcept : m_out(a), m_central(a), m_alloc(a) {}

    // Append member `name`. Deflate stores the member when DEFLATE does not shrink it (the method a reader sees is
    // the one written). Malformed for an empty name; Unsupported for a name past 64 KiB, a 65535th member or an
    // archive whose offsets or directory reach 4 GiB (each count, size and offset stays below its Zip64 sentinel);
    // OutOfMemory when the allocator refuses. The archive so far is unchanged by a refusal.
    [[nodiscard]] ZipError add(crd::containers::StringView name, crd::containers::ConstSpan<crd::u8> bytes,
                               ZipMethod method) noexcept;

    // Room for `members` more members whose names and written data total `name_bytes` and `data_bytes`, so adding
    // them STORED and finishing allocates nothing more (a caller that knows its members writes in one allocation);
    // OutOfMemory when the allocator refuses, the archive so far unchanged.
    [[nodiscard]] ZipError reserve(crd::usize members, crd::usize name_bytes, crd::usize data_bytes) noexcept;

    // Write the central directory and the end record and move the archive into `out`; the writer starts a new
    // archive afterwards. OutOfMemory leaves `out` unchanged and keeps the archive so far.
    [[nodiscard]] ZipError finish(crd::containers::Array<crd::u8>& out) noexcept;

private:
    crd::containers::Array<crd::u8> m_out;     // local headers and member data
    crd::containers::Array<crd::u8> m_central; // central-directory records
    crd::u32 m_count = 0;
    crd::memory::IAllocator* m_alloc = nullptr;
};

} // namespace crd::compression
