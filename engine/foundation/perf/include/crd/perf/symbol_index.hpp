#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- DIAG.5d(c): exact symbol identities (the tag-5 SymbolIndex bundle section).
//
// A SymbolIndex records, per loaded module, its binary IDENTITY plus its loaded address range and
// the debug-file name, so an offline symbolizer can find the MATCHING debug file and REJECT a wrong
// one ("plausible lines" guard) instead of symbolizing against a mismatch. Identity is:
//   - Windows: the PDB RSDS GUID (16 bytes) + age, from the crash minidump's ModuleListStream
//     (EVERY loaded module, not only the faulting one).
//   - Linux: the ELF GNU build-id (up to 20 bytes), captured at install() (DIAG.5d(c2)) and read
//     back from the crash record's module lines.
//
// The Windows identities are read back by a SELF-CONTAINED, PLATFORM-NEUTRAL byte parser: the
// minidump structs are defined here (not pulled from <DbgHelp.h>), so the parser compiles and is
// fuzzable on every lane and treats the dump as UNTRUSTED input -- every RVA/length is bounds-checked
// against the buffer, and a truncated/corrupt dump yields the modules parsed so far (a partial
// index), never a read past the end.
//
// On-disk section payload (little-endian, pinned POD, blob-packed strings -- the CPROF NameBlob
// discipline):
//   [SymbolIndexHeader]                     -- version + module_count + blob_bytes
//   [SymbolRecord] x module_count           -- fixed record; name/debug via blob offset+len
//   [name/debug blob]                        -- packed UTF-8 bytes
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.5d: "reject wrong symbols instead of giving
// plausible lines", "ELF build ID and matching debug files"); ADR-0133 ID-1.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::perf
{

// Version of the SymbolIndex payload layout (independent of the bundle container format version).
inline constexpr crd::u32 kSymbolIndexVersion = 1U;

// The largest identity we store: a 16-byte RSDS GUID or a build-id (GNU build-ids are typically 20
// bytes / SHA-1; longer ones are truncated to this with id_len recording the stored length).
inline constexpr crd::u8 kMaxSymbolIdBytes = 20U;

// How a module's identity is expressed.
enum class SymbolIdKind : crd::u8
{
    None       = 0, // no CV / build-id record -> can NEVER match a debug file (refuse, don't guess)
    Rsds       = 1, // Windows PDB: 16-byte GUID + u32 age
    GnuBuildId = 2, // Linux ELF: up to kMaxSymbolIdBytes raw build-id bytes
    PeImage    = 3, // Windows PE image identity: TimeDateStamp + SizeOfImage (no CV; used by unloaded modules)
};

// Highest valid SymbolIdKind value; read_symbol_index clamps an out-of-range kind to None so a corrupt payload
// cannot forge a matchable kind. Bump this when a kind is added.
inline constexpr crd::u8 kMaxSymbolIdKind = static_cast<crd::u8>(SymbolIdKind::PeImage);

// One module's decoded identity + loaded range (the in-memory form; the on-disk form is SymbolRecord).
struct ModuleIdentity
{
    crd::u64                base      = 0;                  // loaded base address
    crd::u64                size      = 0;                  // image size in bytes
    crd::u32                checksum  = 0;                  // PE CheckSum (Windows) else 0
    crd::u32                timestamp = 0;                  // PE TimeDateStamp (Windows) else 0
    crd::u32                age       = 0;                  // RSDS age (Windows) else 0
    SymbolIdKind            id_kind   = SymbolIdKind::None; //
    crd::u8                 id_len    = 0;                  // bytes of `id` in use (16 for RSDS, <=20 build-id, 0 PeImage)
    bool                    unloaded  = false;              // true: an unloaded-generation module (DIAG.5d(d))
    crd::u8                 id[kMaxSymbolIdBytes] = {};     // GUID (16) or build-id (<=20), raw bytes (empty for PeImage)
    crd::containers::String name;                           // module file name as recorded in the dump
    crd::containers::String debug_file;                     // PDB name (Windows) else "" (build-id/PeImage key the path)
};

// ---- On-disk POD layouts (little-endian, explicit padding, pinned sizes) ----

struct SymbolIndexHeader
{
    crd::u32 version;      // 4  kSymbolIndexVersion
    crd::u32 module_count; // 4
    crd::u64 blob_bytes;   // 8  packed name/debug byte count
};

static_assert(sizeof(SymbolIndexHeader) == 16, "SymbolIndexHeader is 16 B; on-disk pin");
static_assert(alignof(SymbolIndexHeader) == 8, "SymbolIndexHeader is 8-aligned");
static_assert(CRD_LITTLE_ENDIAN, "the symbol index is copied as host bytes");

struct SymbolRecord
{
    crd::u64 base;                    // 0
    crd::u64 size;                    // 8
    crd::u32 checksum;                // 16
    crd::u32 timestamp;               // 20
    crd::u32 age;                     // 24
    crd::u32 name_off;                // 28  byte offset into the blob
    crd::u32 name_len;                // 32
    crd::u32 debug_off;               // 36
    crd::u32 debug_len;               // 40
    crd::u8  id_kind;                 // 44  SymbolIdKind
    crd::u8  id_len;                  // 45
    crd::u8  id[kMaxSymbolIdBytes];   // 46 .. 66
    crd::u16 flags;                   // 66  kSymbolRecordFlag*
    crd::u32 _pad_b;                  // 68  NOLINT(readability-identifier-naming): pinned on-disk spare slot
};

// SymbolRecord::flags bits.
inline constexpr crd::u16 kSymbolRecordFlagUnloaded = 1U << 0; // an unloaded-generation module (DIAG.5d(d))

static_assert(sizeof(SymbolRecord) == 72, "SymbolRecord is 72 B; on-disk pin");
static_assert(alignof(SymbolRecord) == 8, "SymbolRecord is 8-aligned");

// ---- Serialise / read the SymbolIndex section payload ----

// Pack `modules` into a SymbolIndex section payload (header + records + blob). Each name and debug file is packed as
// its file name: no directory of the running host leaves in a section (DIAG.12).
[[nodiscard]] crd::containers::Array<crd::u8>
serialize_symbol_index(crd::containers::ConstSpan<ModuleIdentity> modules,
                       crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

// Decode a SymbolIndex section payload. Untrusted input: every offset/length is bounds-checked; a
// record whose name/debug blob range is out of bounds yields an empty string for that field rather
// than a read past the end. Returns empty for a short/wrong-version payload.
[[nodiscard]] crd::containers::Array<ModuleIdentity>
read_symbol_index(crd::containers::ConstSpan<crd::u8> payload,
                  crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

// ---- Windows minidump byte parser (platform-neutral; compiles on every lane) ----

// On-disk size of one MINIDUMP_MODULE record. The SDK packs its minidump structs to 4 bytes
// (<pshpack4.h>), so MINIDUMP_MODULE is 108 B on x64 -- NOT the 112 B a natural-aligned copy would
// be (the trailing ULONG64 Reserved fields get no 4-byte pad). This is the module-list stride; the
// .cpp pins it against the real <DbgHelp.h> layout on Windows.
inline constexpr crd::u32 kMinidumpModuleRecordBytes = 108U;

// The bytes of the first stream of `stream_type` (a system stream or a user stream such as crd::crash's
// kMachineStreamType) in a Windows minidump held entirely in `dump`, on any platform. Empty for a non-minidump, a
// missing stream, or one whose declared bytes run past `dump` (untrusted input: nothing is read outside it).
[[nodiscard]] crd::containers::ConstSpan<crd::u8> find_minidump_stream(crd::containers::ConstSpan<crd::u8> dump,
                                                                       crd::u32 stream_type) noexcept;

// Parse the ModuleListStream of a Windows minidump held entirely in `dump`. Returns EVERY module's
// identity (RSDS GUID/age where the module carries a CV record, else id_kind None). Untrusted input:
// every RVA/length is bounds-checked; a truncated or corrupt dump yields the modules parsed so far.
// Returns empty for a non-minidump (bad signature) or a missing/short ModuleListStream.
[[nodiscard]] crd::containers::Array<ModuleIdentity>
parse_minidump_modules(crd::containers::ConstSpan<crd::u8> dump,
                       crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

// Parse the UnloadedModuleListStream (stream 14) of a Windows minidump: modules unloaded before the fault (a bounded
// OS trace -- ntdll keeps the last ~64 by basename). Each identity is id_kind PeImage (TimeDateStamp + SizeOfImage --
// unloaded entries carry no CV record) with `unloaded = true`. The stream is SELF-DESCRIBING (header gives the entry
// stride), so a future larger entry still parses; untrusted-bounded like parse_minidump_modules.
[[nodiscard]] crd::containers::Array<ModuleIdentity>
parse_minidump_unloaded_modules(crd::containers::ConstSpan<crd::u8> dump,
                                crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

// Convenience: parse a minidump and serialise straight to a SymbolIndex section payload (loaded modules, then the
// unloaded generations).
[[nodiscard]] crd::containers::Array<crd::u8>
build_symbol_index_from_minidump(crd::containers::ConstSpan<crd::u8> dump,
                                 crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

// ---- Linux crash record parser (platform-neutral; compiles on every lane) ----

// Parse the module lines of a Linux crash record (crd::crash's `crash_*.log`): each
// `module <base> <size> <build-id or -> <basename>` becomes an identity with the base and PT_LOAD span
// enumerate_loaded_modules reports (id_kind GnuBuildId, or None for `-`). The main executable's
// `(exe)` is named by the basename of the record's `exe` line, so no directory leaves in an index.
// Untrusted input: only newline-terminated lines count, and a malformed module line ends the parse,
// so a truncated or corrupt record yields the modules parsed so far. Returns empty for a non-record.
[[nodiscard]] crd::containers::Array<ModuleIdentity>
parse_crash_record_modules(crd::containers::ConstSpan<crd::u8> record,
                           crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

// ---- Live module enumeration (DIAG.9b: the SymbolIndex of a flight bundle) ----

// The most modules one enumeration or snapshot keeps (72 B each on disk plus names: well under a section cap).
inline constexpr crd::u32 kMaxLiveModules = 1024U;

struct LiveModules
{
    crd::containers::Array<ModuleIdentity> modules;
    bool                                   available = false; // false: this platform or the walk itself failed
    bool                                   overflow  = false; // more than kMaxLiveModules were loaded; the rest dropped
};

// Enumerate the modules loaded in this process now, with the same identities a crash dump records:
//   Windows: every module of a Toolhelp snapshot, each pinned while its in-memory PE headers are read (bounds-checked
//            against SizeOfImage): base, SizeOfImage, CheckSum, TimeDateStamp and, from the CodeView debug entry, the
//            RSDS GUID and age (id_kind Rsds; None without one). Only basenames are kept: `name` is the module file
//            name and `debug_file` the PDB name, so no directory of the machine leaves in a bundle.
//   Linux:   dl_iterate_phdr: base = load bias + the lowest PT_LOAD address, size = the PT_LOAD span, and the
//            NT_GNU_BUILD_ID note (id_kind GnuBuildId; None without one). `name` is the basename (the main
//            executable's from /proc/self/exe).
// This takes the loader lock and allocates: never call it on a fatal path or from a loader callback. Hosts take a
// snapshot when they start and after loading modules, and a fatal path writes that snapshot.
[[nodiscard]] LiveModules enumerate_loaded_modules(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// Merge a live enumeration into a snapshot kept since the host started: a module of `known` that `live` no longer
// holds (same base, name and identity) is kept and marked unloaded, so a bundle still names the binary a capture's
// events came from; a module of `live` that `known` lacks is appended. Returns the number of modules newly marked
// unloaded plus newly appended. The snapshot never grows past kMaxLiveModules; `overflow` is set when one was dropped.
crd::u32 merge_module_snapshot(crd::containers::Array<ModuleIdentity>&      known,
                               crd::containers::ConstSpan<ModuleIdentity> live, bool& overflow);

// ---- Matching debug files + wrong-symbol rejection ----

// The debug-file path an offline symbolizer must look up for `m`, under `symstore_root`:
//   Rsds:       <root>/<pdb>/<GUID32-UPPER><AGE-UPPER-hex>/<pdb>   (symsrv layout)
//   GnuBuildId: <root>/.build-id/<nn>/<rest>.debug                 (nn = first build-id byte)
// Returns an empty String for id_kind None (nothing to look up -- there is no plausible guess).
[[nodiscard]] crd::containers::String
debug_file_path(const ModuleIdentity& m, crd::containers::StringView symstore_root,
                crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

// True iff `have` is the SAME binary identity as `want`: RSDS requires identical GUID bytes AND age;
// GnuBuildId requires identical id bytes (same length); None NEVER matches, and differing kinds never
// match. This is the "reject wrong symbols instead of giving plausible lines" guard -- a symbolizer
// must call it before trusting a debug file it found by path.
[[nodiscard]] bool identity_matches(const ModuleIdentity& have, const ModuleIdentity& want) noexcept;

// True iff `name`'s basename is safe to use as a debug-lookup path segment: non-empty, not "." or "..", and free of
// path separators (it is reduced to a basename) and embedded NUL. debug_file_path returns "" for an unsafe name (no
// traversal); the importer counts unsafe names it decodes. This is the acceptance's "reject traversal ... inputs".
[[nodiscard]] bool is_safe_lookup_name(crd::containers::StringView name) noexcept;

} // namespace crd::perf
