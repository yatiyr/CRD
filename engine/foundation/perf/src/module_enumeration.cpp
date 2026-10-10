// crd-perf -- live loaded-module enumeration and the module snapshot (DIAG.9b). See symbol_index.hpp.
//
// The identities are the ones a crash dump records for the same modules (DIAG.5d): on Windows the PE TimeDateStamp,
// SizeOfImage and CheckSum and the CodeView RSDS GUID and age, read from the loaded image's own headers; on Linux the
// GNU build-id note. Every read of a loaded image is bounds-checked against the image size, so a module with a
// malformed or missing header yields id_kind None (nothing to match), never a read outside the image.

#include <crd/perf/symbol_index.hpp>

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // GetModuleHandleExW, FreeLibrary, IMAGE_* headers
#include <tlhelp32.h> // CreateToolhelp32Snapshot, Module32FirstW / Module32NextW
#elif defined(__linux__)
#include <elf.h>
#include <link.h> // dl_iterate_phdr, ElfW, struct dl_phdr_info
#include <unistd.h> // readlink
#endif

namespace crd::perf
{
namespace
{
namespace cont = crd::containers;

[[maybe_unused]] constexpr crd::u32   kRsdsSignature   = 0x53445352U; // 'RSDS' little-endian
[[maybe_unused]] constexpr crd::u32   kMaxDebugEntries = 64U;         // debug directory entries read per image
[[maybe_unused]] constexpr crd::usize kModuleNameBytes = 256U;        // a basename's bound in the Linux walk

// The part of `path` after its last separator ('/' or '\\').
[[maybe_unused]] cont::StringView basename_of_path(const char* path, crd::usize n) noexcept
{
    crd::usize start = 0U;
    for (crd::usize i = 0U; i < n; ++i)
    {
        if (path[i] == '/' || path[i] == '\\')
        {
            start = i + 1U;
        }
    }
    return cont::StringView{path + start, n - start};
}

[[nodiscard]] bool same_module(const ModuleIdentity& a, const ModuleIdentity& b) noexcept
{
    const cont::StringView an{a.name.data(), a.name.size()};
    const cont::StringView bn{b.name.data(), b.name.size()};
    return a.base == b.base && a.size == b.size && a.timestamp == b.timestamp && a.checksum == b.checksum &&
           a.age == b.age && a.id_kind == b.id_kind && a.id_len == b.id_len &&
           std::memcmp(a.id, b.id, kMaxSymbolIdBytes) == 0 && an == bn;
}

#if defined(_WIN32)

// Read `n` bytes at `rva` of a loaded image of `bound` bytes, or fail when the range leaves the image.
[[nodiscard]] bool read_image(const crd::u8* base, crd::u64 bound, crd::u64 rva, void* out, crd::u64 n) noexcept
{
    if (rva > bound || n > bound - rva)
    {
        return false;
    }
    std::memcpy(out, base + rva, static_cast<std::size_t>(n));
    return true;
}

// Fill `m` from the loaded image's PE headers. `loaded_size` is the loader's size of the mapping.
void read_pe_identity(const crd::u8* base, crd::u64 loaded_size, ModuleIdentity& m, crd::memory::IAllocator* alloc)
{
    IMAGE_DOS_HEADER dos{};
    if (!read_image(base, loaded_size, 0U, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0)
    {
        return;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!read_image(base, loaded_size, static_cast<crd::u64>(dos.e_lfanew), &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        return;
    }
    m.timestamp = nt.FileHeader.TimeDateStamp;
    m.checksum  = nt.OptionalHeader.CheckSum;
    m.size      = nt.OptionalHeader.SizeOfImage;
    // Every later read stays inside both the loader's mapping and the image the header declares.
    const crd::u64 bound = m.size < loaded_size ? m.size : loaded_size;

    if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
    {
        return;
    }
    const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0U || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
    {
        return;
    }
    crd::u64 entries = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    entries          = entries < kMaxDebugEntries ? entries : kMaxDebugEntries;
    for (crd::u64 e = 0U; e < entries; ++e)
    {
        IMAGE_DEBUG_DIRECTORY dbg{};
        if (!read_image(base, bound, dir.VirtualAddress + e * sizeof(IMAGE_DEBUG_DIRECTORY), &dbg, sizeof(dbg)))
        {
            return;
        }
        if (dbg.Type != IMAGE_DEBUG_TYPE_CODEVIEW || dbg.AddressOfRawData == 0U || dbg.SizeOfData < 24U)
        {
            continue;
        }
        crd::u8 head[24] = {};
        if (!read_image(base, bound, dbg.AddressOfRawData, head, sizeof(head)))
        {
            continue;
        }
        crd::u32 signature = 0U;
        std::memcpy(&signature, head, sizeof(signature));
        if (signature != kRsdsSignature)
        {
            continue;
        }
        m.id_kind = SymbolIdKind::Rsds;
        m.id_len  = 16U;
        std::memcpy(m.id, head + 4U, 16U);
        std::memcpy(&m.age, head + 20U, sizeof(m.age));
        // The PDB path follows, NUL-terminated inside SizeOfData; only its basename is kept.
        char           path[MAX_PATH] = {};
        const crd::u64 room           = dbg.SizeOfData - 24U;
        const crd::u64 want           = room < sizeof(path) - 1U ? room : sizeof(path) - 1U;
        if (read_image(base, bound, static_cast<crd::u64>(dbg.AddressOfRawData) + 24U, path, want))
        {
            const crd::usize len = ::strnlen(path, static_cast<std::size_t>(want));
            m.debug_file         = cont::String(basename_of_path(path, len), alloc);
        }
        return;
    }
}

void enumerate_windows(LiveModules& out, crd::memory::IAllocator* alloc)
{
    HANDLE snap = INVALID_HANDLE_VALUE;
    // A module snapshot can fail with ERROR_BAD_LENGTH while the loader is changing the list; it is retried.
    for (crd::u32 attempt = 0U; attempt < 8U && snap == INVALID_HANDLE_VALUE; ++attempt)
    {
        snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0U);
        if (snap == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_BAD_LENGTH)
        {
            return;
        }
    }
    if (snap == INVALID_HANDLE_VALUE)
    {
        return;
    }
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    for (BOOL more = ::Module32FirstW(snap, &me); more != FALSE; more = ::Module32NextW(snap, &me))
    {
        if (out.modules.size() >= kMaxLiveModules)
        {
            out.overflow = true;
            break;
        }
        // Pin the module while its headers are read: a module unloaded since the snapshot is skipped, never read.
        HMODULE pinned = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(me.modBaseAddr),
                                 &pinned) == FALSE)
        {
            continue;
        }
        if (reinterpret_cast<const crd::u8*>(pinned) == me.modBaseAddr)
        {
            ModuleIdentity m;
            m.base = reinterpret_cast<crd::u64>(me.modBaseAddr);
            m.size = me.modBaseSize;
            // The loader's base name, in the case the module was loaded by (OLEAUT32.dll where a dump, which reads
            // the mapped file, says oleaut32.dll): Windows module names compare without case.
            char      name[MAX_PATH * 3] = {};
            const int bytes = ::WideCharToMultiByte(CP_UTF8, 0U, me.szModule, -1, name, static_cast<int>(sizeof(name)),
                                                    nullptr, nullptr);
            const crd::usize len = bytes > 0 ? static_cast<crd::usize>(bytes) - 1U : 0U;
            m.name               = cont::String(basename_of_path(name, len), alloc);
            m.debug_file         = cont::String(alloc);
            read_pe_identity(me.modBaseAddr, me.modBaseSize, m, alloc);
            out.modules.push_back(static_cast<ModuleIdentity&&>(m));
        }
        (void)::FreeLibrary(pinned);
    }
    (void)::CloseHandle(snap);
    out.available = true;
}

#elif defined(__linux__)

struct RawModule
{
    crd::u64 base   = 0U;
    crd::u64 size   = 0U;
    crd::u8  id_len = 0U;
    crd::u8  id[kMaxSymbolIdBytes] = {};
    char     name[kModuleNameBytes] = {};
};

struct Walk
{
    RawModule* slots    = nullptr;
    crd::u32   capacity = 0U;
    crd::u32   count    = 0U;
    bool       overflow = false;
    bool       first    = true;
    const char* exe_name = "";
};

// The GNU build-id from the PT_NOTE segments of one loaded object, bounds-checked against each segment's size.
crd::u8 note_build_id(const dl_phdr_info* info, crd::u8* out) noexcept
{
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE)
        {
            continue;
        }
        const auto* p   = reinterpret_cast<const crd::u8*>(info->dlpi_addr + ph.p_vaddr);
        crd::u64    rem = ph.p_memsz;
        while (rem >= sizeof(ElfW(Nhdr)))
        {
            ElfW(Nhdr) nh{};
            std::memcpy(&nh, p, sizeof(nh));
            const crd::u64 name_pad = (static_cast<crd::u64>(nh.n_namesz) + 3U) & ~static_cast<crd::u64>(3U);
            const crd::u64 desc_pad = (static_cast<crd::u64>(nh.n_descsz) + 3U) & ~static_cast<crd::u64>(3U);
            const crd::u64 total    = sizeof(ElfW(Nhdr)) + name_pad + desc_pad;
            if (total > rem)
            {
                break;
            }
            if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4U &&
                std::memcmp(p + sizeof(ElfW(Nhdr)), "GNU", 4U) == 0)
            {
                const crd::u64 take = nh.n_descsz < kMaxSymbolIdBytes ? nh.n_descsz : kMaxSymbolIdBytes;
                std::memcpy(out, p + sizeof(ElfW(Nhdr)) + name_pad, static_cast<std::size_t>(take));
                return static_cast<crd::u8>(take);
            }
            p += total;
            rem -= total;
        }
    }
    return 0U;
}

// dl_iterate_phdr callback: copies into the preallocated slots only (it runs under the loader lock).
int walk_module(dl_phdr_info* info, size_t /*size*/, void* data) noexcept
{
    auto*      walk  = static_cast<Walk*>(data);
    const bool first = walk->first;
    walk->first      = false;
    crd::u64   low   = ~static_cast<crd::u64>(0U);
    crd::u64   high  = 0U;
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD)
        {
            continue;
        }
        low  = ph.p_vaddr < low ? ph.p_vaddr : low;
        high = ph.p_vaddr + ph.p_memsz > high ? ph.p_vaddr + ph.p_memsz : high;
    }
    if (high == 0U)
    {
        return 0; // nothing mapped: not a module a symbolizer could use
    }
    if (walk->count >= walk->capacity)
    {
        walk->overflow = true;
        return 1;
    }
    RawModule& r = walk->slots[walk->count];
    r.base       = static_cast<crd::u64>(info->dlpi_addr) + low;
    r.size       = high - low;
    r.id_len     = note_build_id(info, r.id);
    const char* path = info->dlpi_name != nullptr ? info->dlpi_name : "";
    if (first && path[0] == '\0')
    {
        path = walk->exe_name; // the main executable has no name in the walk
    }
    const cont::StringView bn = basename_of_path(path, std::strlen(path));
    const crd::usize       n  = bn.size() < kModuleNameBytes - 1U ? bn.size() : kModuleNameBytes - 1U;
    std::memcpy(r.name, bn.data(), n);
    r.name[n] = '\0';
    ++walk->count;
    return 0;
}

void enumerate_linux(LiveModules& out, crd::memory::IAllocator* alloc)
{
    char          exe[4096] = {};
    const ssize_t n         = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1U);
    exe[n > 0 ? static_cast<crd::usize>(n) : 0U] = '\0';

    cont::Array<RawModule> slots(alloc);
    slots.resize(kMaxLiveModules);
    if (slots.size() != kMaxLiveModules)
    {
        return;
    }
    Walk walk;
    walk.slots    = slots.data();
    walk.capacity = kMaxLiveModules;
    walk.exe_name = exe;
    (void)::dl_iterate_phdr(&walk_module, &walk);

    for (crd::u32 i = 0U; i < walk.count; ++i)
    {
        const RawModule& r = slots[i];
        ModuleIdentity   m;
        m.base   = r.base;
        m.size   = r.size;
        m.id_len = r.id_len;
        if (r.id_len != 0U)
        {
            m.id_kind = SymbolIdKind::GnuBuildId;
            std::memcpy(m.id, r.id, r.id_len);
        }
        m.name       = cont::String(cont::StringView{r.name, std::strlen(r.name)}, alloc);
        m.debug_file = cont::String(alloc);
        out.modules.push_back(static_cast<ModuleIdentity&&>(m));
    }
    out.overflow  = walk.overflow;
    out.available = true;
}

#endif
} // namespace

LiveModules enumerate_loaded_modules(crd::memory::IAllocator* alloc)
{
    LiveModules out;
    out.modules = cont::Array<ModuleIdentity>(alloc);
#if defined(_WIN32)
    enumerate_windows(out, alloc);
#elif defined(__linux__)
    enumerate_linux(out, alloc);
#else
    (void)alloc;
#endif
    return out;
}

crd::u32 merge_module_snapshot(cont::Array<ModuleIdentity>& known, cont::ConstSpan<ModuleIdentity> live,
                               bool& overflow)
{
    crd::u32 changed = 0U;
    for (ModuleIdentity& k : known)
    {
        if (k.unloaded)
        {
            continue;
        }
        bool still_loaded = false;
        for (const ModuleIdentity& l : live)
        {
            if (same_module(k, l))
            {
                still_loaded = true;
                break;
            }
        }
        if (!still_loaded)
        {
            k.unloaded = true;
            ++changed;
        }
    }
    for (const ModuleIdentity& l : live)
    {
        bool present = false;
        for (const ModuleIdentity& k : known)
        {
            if (!k.unloaded && same_module(k, l))
            {
                present = true;
                break;
            }
        }
        if (present)
        {
            continue;
        }
        if (known.size() >= kMaxLiveModules)
        {
            overflow = true;
            continue;
        }
        known.push_back(l);
        known.back().unloaded = false;
        ++changed;
    }
    return changed;
}

} // namespace crd::perf
