// crd-perf -- DIAG.5d(c): exact symbol identities. See symbol_index.hpp.
// The minidump parser is byte-based and platform-neutral (its structs are defined here, not from
// <DbgHelp.h>), so it compiles on every lane and treats the dump as untrusted input.

#include <crd/perf/symbol_index.hpp>

#include <cstddef>
#include <cstring>
#include <utility>

#if defined(_WIN32)
// Types only (no DbgHelp call -> no dbghelp.lib link), to pin our crd-native layout to the real SDK
// struct on the one lane that has it. Same pattern as tests/foundation/core/minidump_probe.hpp.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <DbgHelp.h> // must follow windows.h
// clang-format on
#endif

namespace crd::perf
{
namespace cont = crd::containers;

namespace
{
// ---- byte helpers -------------------------------------------------------
void append_bytes(cont::Array<crd::u8>& a, const void* src, crd::usize n) noexcept
{
    if (n == 0)
    {
        return;
    }
    const crd::usize old = a.size();
    a.resize_uninitialized(old + n);
    std::memcpy(a.data() + old, src, n);
}

const char* const kHexUpper = "0123456789ABCDEF";
const char* const kHexLower = "0123456789abcdef";

void append_hex_byte(cont::String& s, crd::u8 b, const char* hex) noexcept
{
    s.push_back(hex[b >> 4]);
    s.push_back(hex[b & 0x0FU]);
}

void append_hex_u32_min(cont::String& s, crd::u32 v) noexcept // uppercase, no leading zeros, >=1 digit
{
    if (v == 0)
    {
        s.push_back('0');
        return;
    }
    char tmp[8];
    int  n = 0;
    while (v != 0)
    {
        tmp[n++] = kHexUpper[v & 0x0FU];
        v >>= 4;
    }
    for (int i = n - 1; i >= 0; --i)
    {
        s.push_back(tmp[i]);
    }
}

// UTF-16LE (char_count code units at p) -> UTF-8 into a crd::String. Handles surrogate pairs.
cont::String utf16le_to_utf8(const crd::u8* p, crd::usize char_count, crd::memory::IAllocator* a) noexcept
{
    cont::String s(a);
    crd::usize   i = 0;
    while (i < char_count)
    {
        crd::u32 cp = static_cast<crd::u32>(p[2 * i]) | (static_cast<crd::u32>(p[2 * i + 1]) << 8);
        ++i;
        if (cp >= 0xD800U && cp <= 0xDBFFU && i < char_count)
        {
            const crd::u32 lo = static_cast<crd::u32>(p[2 * i]) | (static_cast<crd::u32>(p[2 * i + 1]) << 8);
            if (lo >= 0xDC00U && lo <= 0xDFFFU)
            {
                cp = 0x10000U + ((cp - 0xD800U) << 10) + (lo - 0xDC00U);
                ++i;
            }
        }
        if (cp < 0x80U)
        {
            s.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800U)
        {
            s.push_back(static_cast<char>(0xC0U | (cp >> 6)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else if (cp < 0x10000U)
        {
            s.push_back(static_cast<char>(0xE0U | (cp >> 12)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else
        {
            s.push_back(static_cast<char>(0xF0U | (cp >> 18)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }
    return s;
}

// ---- minidump on-disk structs (pinned; NOT from <DbgHelp.h>) ------------
// The SDK wraps every minidump struct in <pshpack4.h> (4-byte packing). We mirror that with an
// explicit #pragma pack(4) so our crd-native copies have the SAME on-disk sizes/offsets as the real
// types -- crucially MINIDUMP_MODULE == 108 B (a natural-aligned copy would pad to 112 and read every
// module after the first at the wrong stride). Pinned below, and against the real <DbgHelp.h> layout
// on Windows.
constexpr crd::u32 kMinidumpSignature       = 0x504D444DU; // 'MDMP'
constexpr crd::u32 kModuleListStream         = 4U;         // MINIDUMP_STREAM_TYPE::ModuleListStream
constexpr crd::u32 kUnloadedModuleListStream = 14U;        // MINIDUMP_STREAM_TYPE::UnloadedModuleListStream
constexpr crd::u32 kRsdsMagic                = 0x53445352U; // 'RSDS' little-endian

#pragma pack(push, 4)
struct MdLocation
{
    crd::u32 data_size;
    crd::u32 rva;
};

struct MdHeader
{
    crd::u32 signature;
    crd::u32 version;
    crd::u32 number_of_streams;
    crd::u32 stream_directory_rva;
    crd::u32 checksum;
    crd::u32 time_date_stamp;
    crd::u64 flags;
};

struct MdDirectory
{
    crd::u32   stream_type;
    MdLocation location;
};

struct MdModule
{
    crd::u64   base_of_image;    // 0
    crd::u32   size_of_image;    // 8
    crd::u32   checksum;         // 12
    crd::u32   time_date_stamp;  // 16
    crd::u32   module_name_rva;  // 20
    crd::u32   version_info[13]; // 24 (VS_FIXEDFILEINFO, 52 B) -> 76
    MdLocation cv_record;        // 76 -> 84
    MdLocation misc_record;      // 84 -> 92
    crd::u64   reserved0;        // 92 (pack4: no pad) -> 100
    crd::u64   reserved1;        // 100 -> 108
};

// The UnloadedModuleListStream is SELF-DESCRIBING: the header carries size_of_header + size_of_entry, so the entry
// stride is read from the file (never sizeof) -- the inverse of the (c1) 108-vs-112 trap, here the format hands us the
// answer and a future larger entry still parses.
struct MdUnloadedModuleList
{
    crd::u32 size_of_header;    // 0  == sizeof(MdUnloadedModuleList) (12) for v1
    crd::u32 size_of_entry;     // 4  stride of each MdUnloadedModule (>= 24)
    crd::u32 number_of_entries; // 8
};

struct MdUnloadedModule
{
    crd::u64 base_of_image;   // 0
    crd::u32 size_of_image;   // 8
    crd::u32 checksum;        // 12
    crd::u32 time_date_stamp; // 16
    crd::u32 module_name_rva; // 20
};
#pragma pack(pop)

static_assert(sizeof(MdLocation) == 8, "MINIDUMP_LOCATION_DESCRIPTOR pin");
static_assert(sizeof(MdHeader) == 32, "MINIDUMP_HEADER pin");
static_assert(sizeof(MdDirectory) == 12, "MINIDUMP_DIRECTORY pin");
static_assert(sizeof(MdModule) == kMinidumpModuleRecordBytes, "MINIDUMP_MODULE is 108 B under pack(4)");
static_assert(offsetof(MdModule, cv_record) == 76, "CvRecord at 76");
static_assert(offsetof(MdModule, module_name_rva) == 20, "ModuleNameRva at 20");
static_assert(sizeof(MdUnloadedModuleList) == 12, "MINIDUMP_UNLOADED_MODULE_LIST pin");
static_assert(sizeof(MdUnloadedModule) == 24, "MINIDUMP_UNLOADED_MODULE pin");
static_assert(offsetof(MdUnloadedModule, module_name_rva) == 20, "unloaded ModuleNameRva at 20");

#if defined(_WIN32)
// Permanent guard: our crd-native layout must equal the real SDK type on the lane that ships it.
static_assert(sizeof(::MINIDUMP_MODULE) == kMinidumpModuleRecordBytes, "MdModule vs SDK MINIDUMP_MODULE size");
static_assert(offsetof(::MINIDUMP_MODULE, CvRecord) == 76, "MdModule vs SDK CvRecord offset");
static_assert(offsetof(::MINIDUMP_MODULE, ModuleNameRva) == 20, "MdModule vs SDK ModuleNameRva offset");
static_assert(sizeof(::MINIDUMP_HEADER) == sizeof(MdHeader), "MdHeader vs SDK MINIDUMP_HEADER size");
static_assert(sizeof(::MINIDUMP_DIRECTORY) == sizeof(MdDirectory), "MdDirectory vs SDK size");
static_assert(sizeof(::MINIDUMP_UNLOADED_MODULE_LIST) == sizeof(MdUnloadedModuleList), "MdUnloadedModuleList vs SDK");
static_assert(sizeof(::MINIDUMP_UNLOADED_MODULE) == sizeof(MdUnloadedModule), "MdUnloadedModule vs SDK");
static_assert(offsetof(::MINIDUMP_UNLOADED_MODULE, ModuleNameRva) == 20, "unloaded ModuleNameRva vs SDK");
#endif

// A MINIDUMP_STRING at file offset `rva`: u32 byte-length + UTF-16LE. Bounds-checked; clamps to buf.
cont::String read_md_string(cont::ConstSpan<crd::u8> dump, crd::u64 rva, crd::memory::IAllocator* a) noexcept
{
    cont::String s(a);
    if (rva == 0 || rva + sizeof(crd::u32) > dump.size())
    {
        return s;
    }
    crd::u32 nbytes = 0;
    std::memcpy(&nbytes, dump.data() + rva, sizeof(nbytes));
    const crd::u64 soff  = rva + sizeof(crd::u32);
    crd::u64       avail = dump.size() - soff;
    crd::u64       want  = nbytes;
    if (want > avail)
    {
        want = avail & ~static_cast<crd::u64>(1); // even byte count only
    }
    return utf16le_to_utf8(dump.data() + soff, static_cast<crd::usize>(want / 2), a);
}

// A NUL-terminated UTF-8 C string in [off, max). Bounds-checked.
cont::String read_cstr(cont::ConstSpan<crd::u8> dump, crd::u64 off, crd::u64 max,
                       crd::memory::IAllocator* a) noexcept
{
    cont::String s(a);
    for (crd::u64 i = off; i < max && dump[static_cast<crd::usize>(i)] != 0; ++i)
    {
        s.push_back(static_cast<char>(dump[static_cast<crd::usize>(i)]));
    }
    return s;
}

cont::StringView basename_of(cont::StringView sv) noexcept
{
    const crd::usize pos = sv.find_last_of("/\\");
    return (pos == cont::StringView::npos) ? sv : sv.substr(pos + 1);
}

cont::StringView basename_view(const cont::String& path) noexcept
{
    return basename_of(cont::StringView{path.c_str(), path.size()});
}

bool sv_has_nul(cont::StringView s) noexcept
{
    return s.find('\0') != cont::StringView::npos;
}

// A basename is safe to use as a lookup path segment iff it is non-empty, not "." or "..", and free of an embedded
// NUL (which would silently truncate the built c_str path). Path separators are already gone (it is a basename).
bool basename_safe(cont::StringView bn) noexcept
{
    return !bn.empty() && bn != cont::StringView{"."} && bn != cont::StringView{".."} && !sv_has_nul(bn);
}

cont::String blob_slice(cont::ConstSpan<crd::u8> payload, crd::u64 blob_off, crd::u64 blob_len, crd::u32 off,
                        crd::u32 len, crd::memory::IAllocator* a) noexcept
{
    cont::String s(a);
    if (static_cast<crd::u64>(off) + len > blob_len) // out of the declared blob -> empty, never a read past end
    {
        return s;
    }
    if (len != 0)
    {
        s.append(reinterpret_cast<const char*>(payload.data() + blob_off + off), static_cast<crd::usize>(len));
    }
    return s;
}
} // namespace

// ---- minidump parser ----------------------------------------------------

cont::Array<ModuleIdentity> parse_minidump_modules(cont::ConstSpan<crd::u8> dump,
                                                   crd::memory::IAllocator*  alloc) noexcept
{
    cont::Array<ModuleIdentity> out(alloc);
    if (dump.size() < sizeof(MdHeader))
    {
        return out;
    }

    MdHeader hdr{};
    std::memcpy(&hdr, dump.data(), sizeof(hdr));
    if (hdr.signature != kMinidumpSignature)
    {
        return out;
    }

    for (crd::u32 i = 0; i < hdr.number_of_streams; ++i)
    {
        const crd::u64 doff = static_cast<crd::u64>(hdr.stream_directory_rva) + static_cast<crd::u64>(i) * sizeof(MdDirectory);
        if (doff + sizeof(MdDirectory) > dump.size())
        {
            break;
        }
        MdDirectory dir{};
        std::memcpy(&dir, dump.data() + doff, sizeof(dir));
        if (dir.stream_type != kModuleListStream)
        {
            continue;
        }

        const crd::u64 lrva = dir.location.rva;
        if (lrva + sizeof(crd::u32) > dump.size())
        {
            break;
        }
        crd::u32 count = 0;
        std::memcpy(&count, dump.data() + lrva, sizeof(count));

        for (crd::u32 m = 0; m < count; ++m)
        {
            const crd::u64 moff = lrva + sizeof(crd::u32) + static_cast<crd::u64>(m) * sizeof(MdModule);
            if (moff + sizeof(MdModule) > dump.size())
            {
                break; // truncated module list -> partial index (stop, don't guess)
            }
            MdModule mod{};
            std::memcpy(&mod, dump.data() + moff, sizeof(mod));

            ModuleIdentity id;
            id.base      = mod.base_of_image;
            id.size      = mod.size_of_image;
            id.checksum  = mod.checksum;
            id.timestamp = mod.time_date_stamp;
            id.name      = read_md_string(dump, mod.module_name_rva, alloc);

            if (mod.cv_record.data_size >= 24U && mod.cv_record.rva != 0U)
            {
                const crd::u64 crva = mod.cv_record.rva;
                if (crva + 24U <= dump.size())
                {
                    crd::u32 cv_magic = 0;
                    std::memcpy(&cv_magic, dump.data() + crva, sizeof(cv_magic));
                    if (cv_magic == kRsdsMagic)
                    {
                        id.id_kind = SymbolIdKind::Rsds;
                        id.id_len  = 16U;
                        std::memcpy(id.id, dump.data() + crva + 4U, 16U);
                        std::memcpy(&id.age, dump.data() + crva + 20U, sizeof(id.age));
                        crd::u64 pmax = crva + mod.cv_record.data_size;
                        if (pmax > dump.size())
                        {
                            pmax = dump.size();
                        }
                        id.debug_file = read_cstr(dump, crva + 24U, pmax, alloc);
                    }
                }
            }
            out.push_back(std::move(id));
        }
        break; // one ModuleListStream
    }
    return out;
}

cont::Array<ModuleIdentity> parse_minidump_unloaded_modules(cont::ConstSpan<crd::u8> dump,
                                                           crd::memory::IAllocator*  alloc) noexcept
{
    cont::Array<ModuleIdentity> out(alloc);
    if (dump.size() < sizeof(MdHeader))
    {
        return out;
    }

    MdHeader hdr{};
    std::memcpy(&hdr, dump.data(), sizeof(hdr));
    if (hdr.signature != kMinidumpSignature)
    {
        return out;
    }

    for (crd::u32 i = 0; i < hdr.number_of_streams; ++i)
    {
        const crd::u64 doff =
            static_cast<crd::u64>(hdr.stream_directory_rva) + static_cast<crd::u64>(i) * sizeof(MdDirectory);
        if (doff + sizeof(MdDirectory) > dump.size())
        {
            break;
        }
        MdDirectory dir{};
        std::memcpy(&dir, dump.data() + doff, sizeof(dir));
        if (dir.stream_type != kUnloadedModuleListStream)
        {
            continue;
        }

        const crd::u64 lrva = dir.location.rva;
        if (lrva + sizeof(MdUnloadedModuleList) > dump.size())
        {
            break;
        }
        MdUnloadedModuleList ul{};
        std::memcpy(&ul, dump.data() + lrva, sizeof(ul));
        // Self-describing header: reject a header/entry smaller than v1 (refuse, don't guess), but accept a LARGER
        // size_of_entry (forward-compat) by striding with it while reading only the fields we know.
        if (ul.size_of_header < sizeof(MdUnloadedModuleList) || ul.size_of_entry < sizeof(MdUnloadedModule))
        {
            break;
        }

        crd::u64 entry_max = static_cast<crd::u64>(lrva) + dir.location.data_size; // bound by the declared stream...
        if (entry_max > dump.size())
        {
            entry_max = dump.size();                                              // ...and by the buffer
        }

        for (crd::u32 m = 0; m < ul.number_of_entries; ++m)
        {
            const crd::u64 eoff = lrva + ul.size_of_header + static_cast<crd::u64>(m) * ul.size_of_entry;
            if (eoff + sizeof(MdUnloadedModule) > entry_max)
            {
                break; // truncated / past the stream -> partial (bounded, no overrun)
            }
            MdUnloadedModule em{};
            std::memcpy(&em, dump.data() + eoff, sizeof(em));

            ModuleIdentity id;
            id.base      = em.base_of_image;
            id.size      = em.size_of_image;
            id.checksum  = em.checksum;
            id.timestamp = em.time_date_stamp;
            id.id_kind   = SymbolIdKind::PeImage; // unloaded entries carry no CV -> PE image identity (timestamp + size)
            id.id_len    = 0;
            id.unloaded  = true;
            id.name      = read_md_string(dump, em.module_name_rva, alloc);
            out.push_back(std::move(id));
        }
        break; // one UnloadedModuleListStream
    }
    return out;
}

// ---- serialise / read ---------------------------------------------------

cont::Array<crd::u8> serialize_symbol_index(cont::ConstSpan<ModuleIdentity> modules,
                                            crd::memory::IAllocator*         alloc) noexcept
{
    cont::Array<crd::u8>     out(alloc);
    cont::Array<SymbolRecord> records(alloc);
    cont::Array<crd::u8>     blob(alloc);

    for (const ModuleIdentity& m : modules)
    {
        SymbolRecord r{};
        r.base      = m.base;
        r.size      = m.size;
        r.checksum  = m.checksum;
        r.timestamp = m.timestamp;
        r.age       = m.age;
        r.id_kind   = static_cast<crd::u8>(m.id_kind);
        r.id_len    = m.id_len;
        r.flags     = m.unloaded ? kSymbolRecordFlagUnloaded : static_cast<crd::u16>(0);
        std::memcpy(r.id, m.id, kMaxSymbolIdBytes);
        r.name_off = static_cast<crd::u32>(blob.size());
        r.name_len = static_cast<crd::u32>(m.name.size());
        append_bytes(blob, m.name.c_str(), m.name.size());
        r.debug_off = static_cast<crd::u32>(blob.size());
        r.debug_len = static_cast<crd::u32>(m.debug_file.size());
        append_bytes(blob, m.debug_file.c_str(), m.debug_file.size());
        records.push_back(r);
    }

    SymbolIndexHeader hdr{};
    hdr.version      = kSymbolIndexVersion;
    hdr.module_count = static_cast<crd::u32>(records.size());
    hdr.blob_bytes   = blob.size();

    append_bytes(out, &hdr, sizeof(hdr));
    if (!records.empty())
    {
        append_bytes(out, records.data(), records.size() * sizeof(SymbolRecord));
    }
    if (!blob.empty())
    {
        append_bytes(out, blob.data(), blob.size());
    }
    return out;
}

cont::Array<ModuleIdentity> read_symbol_index(cont::ConstSpan<crd::u8> payload,
                                              crd::memory::IAllocator*  alloc) noexcept
{
    cont::Array<ModuleIdentity> out(alloc);
    if (payload.size() < sizeof(SymbolIndexHeader))
    {
        return out;
    }
    SymbolIndexHeader hdr{};
    std::memcpy(&hdr, payload.data(), sizeof(hdr));
    if (hdr.version != kSymbolIndexVersion)
    {
        return out;
    }

    const crd::u64 recs_off = sizeof(SymbolIndexHeader);
    const crd::u64 blob_off = recs_off + static_cast<crd::u64>(hdr.module_count) * sizeof(SymbolRecord);
    if (blob_off > payload.size())
    {
        return out; // record table truncated
    }
    crd::u64 blob_len = hdr.blob_bytes;
    if (blob_len > payload.size() - blob_off)
    {
        blob_len = payload.size() - blob_off; // clamp a lying blob_bytes
    }

    for (crd::u32 i = 0; i < hdr.module_count; ++i)
    {
        const crd::u64 roff = recs_off + static_cast<crd::u64>(i) * sizeof(SymbolRecord);
        SymbolRecord   r{};
        std::memcpy(&r, payload.data() + roff, sizeof(r));

        ModuleIdentity m;
        m.base      = r.base;
        m.size      = r.size;
        m.checksum  = r.checksum;
        m.timestamp = r.timestamp;
        m.age       = r.age;
        // Clamp an out-of-range id_kind to None so a corrupt payload cannot forge a matchable kind.
        m.id_kind = (r.id_kind <= kMaxSymbolIdKind) ? static_cast<SymbolIdKind>(r.id_kind) : SymbolIdKind::None;
        m.id_len   = (r.id_len <= kMaxSymbolIdBytes) ? r.id_len : kMaxSymbolIdBytes;
        m.unloaded = (r.flags & kSymbolRecordFlagUnloaded) != 0U;
        std::memcpy(m.id, r.id, kMaxSymbolIdBytes);
        m.name       = blob_slice(payload, blob_off, blob_len, r.name_off, r.name_len, alloc);
        m.debug_file = blob_slice(payload, blob_off, blob_len, r.debug_off, r.debug_len, alloc);
        out.push_back(std::move(m));
    }
    return out;
}

cont::Array<crd::u8> build_symbol_index_from_minidump(cont::ConstSpan<crd::u8> dump,
                                                      crd::memory::IAllocator*  alloc) noexcept
{
    cont::Array<ModuleIdentity> mods     = parse_minidump_modules(dump, alloc);
    cont::Array<ModuleIdentity> unloaded = parse_minidump_unloaded_modules(dump, alloc);
    for (ModuleIdentity& u : unloaded) // loaded generations first, then the unloaded ones
    {
        mods.push_back(std::move(u));
    }
    return serialize_symbol_index(cont::ConstSpan<ModuleIdentity>{mods.data(), mods.size()}, alloc);
}

// ---- matching + lookup --------------------------------------------------

bool identity_matches(const ModuleIdentity& have, const ModuleIdentity& want) noexcept
{
    if (have.id_kind == SymbolIdKind::None || want.id_kind == SymbolIdKind::None)
    {
        return false; // never guess against an unknown identity
    }
    if (have.id_kind != want.id_kind)
    {
        return false;
    }

    if (have.id_kind == SymbolIdKind::PeImage)
    {
        // PE image identity: TimeDateStamp + SizeOfImage (no id bytes). A zero stamp is reproducible/stripped and not
        // a real identity -> refuse rather than match many binaries.
        return have.timestamp != 0U && have.timestamp == want.timestamp && have.size == want.size;
    }

    // Rsds / GnuBuildId: exact id bytes (plus age for Rsds).
    if (have.id_len != want.id_len || have.id_len == 0U)
    {
        return false; // no identity bytes -> never a match (a zero-length record is not an identity)
    }
    if (have.id_kind == SymbolIdKind::Rsds && have.id_len != 16U)
    {
        return false; // a malformed RSDS record cannot certify a PDB
    }
    if (std::memcmp(have.id, want.id, have.id_len) != 0)
    {
        return false;
    }
    if (have.id_kind == SymbolIdKind::Rsds && have.age != want.age)
    {
        return false;
    }
    return true;
}

cont::String debug_file_path(const ModuleIdentity& m, cont::StringView symstore_root,
                             crd::memory::IAllocator* alloc) noexcept
{
    cont::String s(alloc);
    if (m.id_kind == SymbolIdKind::None)
    {
        return s; // nothing to look up -- no plausible guess
    }

    if (m.id_kind == SymbolIdKind::Rsds)
    {
        const cont::StringView pdb = basename_view(m.debug_file);
        if (!basename_safe(pdb))
        {
            return s; // traversal guard: empty / "." / ".." / embedded-NUL name -> no path (refuse, don't build one)
        }
        // <root>/<pdb>/<GUID32-UPPER><AGE-UPPER>/<pdb>
        s.append(symstore_root);
        s.push_back('/');
        s.append(pdb);
        s.push_back('/');
        // GUID: Data1(LE) Data2(LE) Data3(LE) Data4(BE), 32 uppercase hex, no dashes.
        append_hex_byte(s, m.id[3], kHexUpper);
        append_hex_byte(s, m.id[2], kHexUpper);
        append_hex_byte(s, m.id[1], kHexUpper);
        append_hex_byte(s, m.id[0], kHexUpper);
        append_hex_byte(s, m.id[5], kHexUpper);
        append_hex_byte(s, m.id[4], kHexUpper);
        append_hex_byte(s, m.id[7], kHexUpper);
        append_hex_byte(s, m.id[6], kHexUpper);
        for (int i = 8; i < 16; ++i)
        {
            append_hex_byte(s, m.id[i], kHexUpper);
        }
        append_hex_u32_min(s, m.age);
        s.push_back('/');
        s.append(pdb);
        return s;
    }

    if (m.id_kind == SymbolIdKind::PeImage)
    {
        const cont::StringView name = basename_view(m.name);
        if (!basename_safe(name))
        {
            return s; // traversal guard (see Rsds branch)
        }
        // <root>/<name>/<TimeDateStamp:08X><SizeOfImage:X>/<name> -- the symsrv key for a PE binary (how a DLL's own
        // debug info is fetched by image identity, so an UNLOADED module's symbols are still findable after reload).
        s.append(symstore_root);
        s.push_back('/');
        s.append(name);
        s.push_back('/');
        for (int shift = 28; shift >= 0; shift -= 4) // TimeDateStamp, 8 uppercase hex, zero-padded
        {
            s.push_back(kHexUpper[(m.timestamp >> shift) & 0xFU]);
        }
        append_hex_u32_min(s, static_cast<crd::u32>(m.size)); // SizeOfImage, uppercase hex, no leading zeros
        s.push_back('/');
        s.append(name);
        return s;
    }

    // GnuBuildId: <root>/.build-id/<nn>/<rest>.debug  (lowercase hex, debuginfod layout)
    s.append(symstore_root);
    s.append("/.build-id/");
    if (m.id_len >= 1U)
    {
        append_hex_byte(s, m.id[0], kHexLower);
        s.push_back('/');
        for (crd::u8 i = 1U; i < m.id_len; ++i)
        {
            append_hex_byte(s, m.id[i], kHexLower);
        }
    }
    s.append(".debug");
    return s;
}

bool is_safe_lookup_name(cont::StringView name) noexcept
{
    return basename_safe(basename_of(name));
}

} // namespace crd::perf
