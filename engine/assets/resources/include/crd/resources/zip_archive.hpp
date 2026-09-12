#pragma once

// zip_archive.hpp — GEO-5 pt 2 (D-007): the owned ZIP (PKZIP appnote) container — the OPC substrate 3MF/USDZ-class
// formats ride on, built ENTIRELY from the codec stack we already own (`inflate_raw`/`deflate_raw` + the standard
// CRC-32 the PNG codec exposes). A codec-stack citizen of crd-resources, deliberately NOT asset-io: document parsers
// take pre-extracted bytes (the glTF "caller does I/O" posture) and the COOKER bridges the two layers.
//
// Read scope: the classic 32-bit format AND Zip64 (the 3MF reference toolchain — lib3mf, hence every slicer riding
// it — writes Zip64 structures unconditionally, even for KB-sized archives): EOCD located by a bounded back-scan
// (comments ≤ 64 KiB) with the Zip64 EOCD locator honored when classic fields carry sentinels, central-directory
// walk with 0x0001 extra-field resolution for sentinel sizes/offsets, per-entry extraction (STORED + DEFLATE) with
// CRC-32 AND size verification (a mismatch is a refusal, never a warning). Encryption, spanned archives, and
// data-descriptor-only entries are `Unsupported` BY NAME.
// Write scope: the classic 32-bit format (universally readable — Zip64 emission is unnecessary below 4 GiB),
// DEFLATE entries (STORED when deflate does not shrink), correct central directory + EOCD — archives that reference
// implementations (and the reader here) accept byte-for-byte deterministically.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::resources
{

enum class ZipError : crd::u8
{
    Ok = 0,
    NotRecognized, // no EOCD signature in range — not a ZIP
    Truncated,     // structures point past the input
    Malformed,     // signature/field violations
    Corrupt,       // CRC-32 or size mismatch on extraction
    Unsupported,   // Zip64 / encryption / an unsupported compression method
};

struct ZipEntry
{
    crd::containers::String name; // as stored ('/'-separated paths — the OPC convention)
    crd::u64                compressed_size   = 0; // u64: Zip64 entries carry real sizes in the 0x0001 extra field
    crd::u64                uncompressed_size = 0;
    crd::u32                crc32             = 0;
    crd::u16                method            = 0; // 0 = stored, 8 = deflate
    crd::u64                local_offset      = 0;

    explicit ZipEntry(crd::memory::IAllocator* a) : name(a) {}
    ZipEntry(const ZipEntry&)            = delete;
    ZipEntry& operator=(const ZipEntry&) = delete;
    ZipEntry(ZipEntry&&)                 = default;
    ZipEntry& operator=(ZipEntry&&)      = default;
};

class ZipReader
{
public:
    explicit ZipReader(crd::memory::IAllocator* a) : m_entries(a), m_alloc(a) {}

    // Parse the archive's directory. `bytes` must OUTLIVE the reader (extraction reads from it). No partial
    // directory survives failure.
    [[nodiscard]] ZipError open(crd::containers::ConstSpan<crd::u8> bytes);

    [[nodiscard]] crd::usize      entry_count() const noexcept { return m_entries.size(); }
    [[nodiscard]] const ZipEntry& entry(crd::usize i) const noexcept { return m_entries[i]; }
    // index by exact name; -1 when absent
    [[nodiscard]] crd::i64 find(const char* name) const noexcept;

    // Extract entry `i`: decompress + VERIFY crc32 and size. `out` is replaced; empty + error on any mismatch.
    [[nodiscard]] ZipError extract(crd::usize i, crd::containers::Array<crd::u8>& out) const;

private:
    crd::containers::Array<ZipEntry>    m_entries;
    crd::memory::IAllocator*            m_alloc = nullptr;
    crd::containers::ConstSpan<crd::u8> m_bytes;
};

class ZipWriter
{
public:
    explicit ZipWriter(crd::memory::IAllocator* a) : m_out(a), m_central(a), m_alloc(a) {}

    // Append one file entry. DEFLATE is used when it shrinks the payload, STORED otherwise. Returns false on an
    // invalid name (empty, or > 64 KiB — the u16 field's honest bound).
    [[nodiscard]] bool add(const char* name, crd::containers::ConstSpan<crd::u8> bytes);

    // Write the central directory + EOCD and return the finished archive (the writer is spent afterwards).
    [[nodiscard]] crd::containers::Array<crd::u8> finish();

private:
    crd::containers::Array<crd::u8> m_out;     // local headers + data, accumulated
    crd::containers::Array<crd::u8> m_central; // central-directory records, accumulated
    crd::u16                        m_count = 0;
    crd::memory::IAllocator*        m_alloc = nullptr;
};

} // namespace crd::resources
