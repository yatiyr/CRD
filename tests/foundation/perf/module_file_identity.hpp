#pragma once

// Test oracle for DIAG.9b's live module enumeration: a module's identity read from its FILE on disk, by a parser that
// shares nothing with the engine's in-memory walk (crd/perf/symbol_index.hpp enumerate_loaded_modules). A PE file is
// read through its section table (a debug directory's RVA mapped to a file offset); an ELF file through its program
// headers' file offsets. Only what both sides can know is filled: PE TimeDateStamp, CheckSum, SizeOfImage and the
// CodeView RSDS GUID, age and PDB basename; ELF the PT_LOAD span and the GNU build-id.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/symbol_index.hpp>

#include <cstdio>
#include <cstring>

namespace crd::test::module_file
{
namespace cont = crd::containers;

inline cont::Array<crd::u8> read_whole_file(const char* path)
{
    cont::Array<crd::u8> out;
    std::FILE*           f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0)
    {
        f = nullptr;
    }
#else
    f = std::fopen(path, "rb");
#endif
    if (f == nullptr)
    {
        return out;
    }
    // Read to the end in chunks: a /proc file reports size 0.
    crd::u8    chunk[65536];
    crd::usize got = 0U;
    while ((got = std::fread(chunk, 1U, sizeof(chunk), f)) > 0U)
    {
        for (crd::usize i = 0U; i < got; ++i)
        {
            out.push_back(chunk[i]);
        }
    }
    (void)std::fclose(f);
    return out;
}

template <typename T> bool get(const cont::Array<crd::u8>& b, crd::u64 at, T& v)
{
    if (at > b.size() || sizeof(T) > b.size() - at)
    {
        return false;
    }
    std::memcpy(&v, b.data() + at, sizeof(T));
    return true;
}

// PE32+ image file -> identity. Returns false when the file is not one.
inline bool pe_file_identity(const cont::Array<crd::u8>& f, crd::perf::ModuleIdentity& m)
{
    crd::u32 lfanew = 0U;
    crd::u32 sig    = 0U;
    if (f.size() < 64U || f[0] != 'M' || f[1] != 'Z' || !get(f, 0x3CU, lfanew) || !get(f, lfanew, sig) ||
        sig != 0x00004550U)
    {
        return false;
    }
    const crd::u64 file_header = lfanew + 4U;
    const crd::u64 opt         = file_header + 20U;
    crd::u16       sections = 0U;
    crd::u16       opt_size = 0U;
    crd::u16       magic    = 0U;
    crd::u32       rva_count = 0U;
    if (!get(f, file_header + 2U, sections) || !get(f, file_header + 4U, m.timestamp) ||
        !get(f, file_header + 16U, opt_size) || !get(f, opt, magic) || magic != 0x20BU)
    {
        return false;
    }
    crd::u32 size_of_image = 0U;
    if (!get(f, opt + 56U, size_of_image) || !get(f, opt + 64U, m.checksum) || !get(f, opt + 108U, rva_count))
    {
        return false;
    }
    m.size = size_of_image;
    crd::u32 debug_rva  = 0U;
    crd::u32 debug_size = 0U;
    if (rva_count <= 6U || !get(f, opt + 112U + 6U * 8U, debug_rva) || !get(f, opt + 116U + 6U * 8U, debug_size))
    {
        return true; // no debug directory: identity None
    }
    // RVA -> file offset through the section table.
    const crd::u64 table      = opt + opt_size;
    const auto     rva_to_off = [&](crd::u64 rva, crd::u64& off)
    {
        for (crd::u16 s = 0U; s < sections; ++s)
        {
            crd::u32 vsize = 0U;
            crd::u32 vaddr = 0U;
            crd::u32 rsize = 0U;
            crd::u32 raw   = 0U;
            const crd::u64 h = table + static_cast<crd::u64>(s) * 40U;
            if (!get(f, h + 8U, vsize) || !get(f, h + 12U, vaddr) || !get(f, h + 16U, rsize) || !get(f, h + 20U, raw))
            {
                return false;
            }
            const crd::u64 span = vsize > rsize ? vsize : rsize;
            if (rva >= vaddr && rva < static_cast<crd::u64>(vaddr) + span)
            {
                off = raw + (rva - vaddr);
                return true;
            }
        }
        return false;
    };
    crd::u64 dir = 0U;
    if (debug_rva == 0U || !rva_to_off(debug_rva, dir))
    {
        return true;
    }
    for (crd::u32 e = 0U; e < debug_size / 28U; ++e)
    {
        crd::u32 type      = 0U;
        crd::u32 data_size = 0U;
        crd::u32 data_ptr  = 0U;
        const crd::u64 at  = dir + static_cast<crd::u64>(e) * 28U;
        if (!get(f, at + 12U, type) || !get(f, at + 16U, data_size) || !get(f, at + 24U, data_ptr))
        {
            return true;
        }
        crd::u32 cv = 0U;
        if (type != 2U || data_size < 24U || !get(f, data_ptr, cv) || cv != 0x53445352U)
        {
            continue;
        }
        m.id_kind = crd::perf::SymbolIdKind::Rsds;
        m.id_len  = 16U;
        std::memcpy(m.id, f.data() + data_ptr + 4U, 16U);
        (void)get(f, data_ptr + 20U, m.age);
        const char* path  = reinterpret_cast<const char*>(f.data() + data_ptr + 24U);
        crd::usize  len   = 0U;
        crd::usize  start = 0U;
        while (len < data_size - 24U && path[len] != '\0')
        {
            if (path[len] == '\\' || path[len] == '/')
            {
                start = len + 1U;
            }
            ++len;
        }
        m.debug_file = cont::String(path + start, len - start);
        return true;
    }
    return true;
}

// ELF64 little-endian file -> identity. Returns false when the file is not one.
inline bool elf_file_identity(const cont::Array<crd::u8>& f, crd::perf::ModuleIdentity& m)
{
    if (f.size() < 64U || f[0] != 0x7FU || f[1] != 'E' || f[2] != 'L' || f[3] != 'F' || f[4] != 2U || f[5] != 1U)
    {
        return false;
    }
    crd::u64 phoff = 0U;
    crd::u16 phentsize = 0U;
    crd::u16 phnum     = 0U;
    if (!get(f, 0x20U, phoff) || !get(f, 0x36U, phentsize) || !get(f, 0x38U, phnum))
    {
        return false;
    }
    crd::u64 low  = ~static_cast<crd::u64>(0U);
    crd::u64 high = 0U;
    for (crd::u16 i = 0U; i < phnum; ++i)
    {
        const crd::u64 ph   = phoff + static_cast<crd::u64>(i) * phentsize;
        crd::u32       type = 0U;
        crd::u64       off  = 0U;
        crd::u64       vaddr  = 0U;
        crd::u64       filesz = 0U;
        crd::u64       memsz  = 0U;
        if (!get(f, ph, type) || !get(f, ph + 8U, off) || !get(f, ph + 16U, vaddr) || !get(f, ph + 32U, filesz) ||
            !get(f, ph + 40U, memsz))
        {
            return false;
        }
        if (type == 1U) // PT_LOAD
        {
            low  = vaddr < low ? vaddr : low;
            high = vaddr + memsz > high ? vaddr + memsz : high;
        }
        if (type != 4U || m.id_kind != crd::perf::SymbolIdKind::None) // PT_NOTE, first build-id only
        {
            continue;
        }
        crd::u64 at  = off;
        crd::u64 end = off + filesz;
        while (at + 12U <= end)
        {
            crd::u32 namesz = 0U;
            crd::u32 descsz = 0U;
            crd::u32 ntype  = 0U;
            if (!get(f, at, namesz) || !get(f, at + 4U, descsz) || !get(f, at + 8U, ntype))
            {
                break;
            }
            const crd::u64 name_pad = (static_cast<crd::u64>(namesz) + 3U) & ~static_cast<crd::u64>(3U);
            const crd::u64 desc_pad = (static_cast<crd::u64>(descsz) + 3U) & ~static_cast<crd::u64>(3U);
            if (ntype == 3U && namesz == 4U && at + 16U <= f.size() &&
                std::memcmp(f.data() + at + 12U, "GNU", 4U) == 0 && at + 12U + name_pad + descsz <= f.size())
            {
                const crd::u32 take = descsz < crd::perf::kMaxSymbolIdBytes ? descsz : crd::perf::kMaxSymbolIdBytes;
                m.id_kind           = crd::perf::SymbolIdKind::GnuBuildId;
                m.id_len            = static_cast<crd::u8>(take);
                std::memcpy(m.id, f.data() + at + 12U + name_pad, take);
                break;
            }
            at += 12U + name_pad + desc_pad;
        }
    }
    m.size = high > low ? high - low : 0U;
    return true;
}

// The identity of the module file at `path` (PE or ELF), or false.
inline bool file_identity(const char* path, crd::perf::ModuleIdentity& m)
{
    const cont::Array<crd::u8> bytes = read_whole_file(path);
    return pe_file_identity(bytes, m) || elf_file_identity(bytes, m);
}

// The same binary identity: kind, id bytes and age (RSDS), plus the image size and, for PE, its stamp and checksum.
inline bool same_identity(const crd::perf::ModuleIdentity& a, const crd::perf::ModuleIdentity& b)
{
    return a.id_kind == b.id_kind && a.id_len == b.id_len && std::memcmp(a.id, b.id, a.id_len) == 0 &&
           a.age == b.age && a.size == b.size && a.timestamp == b.timestamp && a.checksum == b.checksum;
}

} // namespace crd::test::module_file
