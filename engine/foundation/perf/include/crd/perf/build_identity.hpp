#pragma once

// crd-perf -- the build identity of a diagnostic bundle (DIAG.9b; the BuildIdentity bundle section, tag 7).
//
// A flight capture's replay recipe and its events only mean something on the build that recorded them. The build
// identity names that build: the engine version, platform, compiler and its version, architecture, configuration and
// the capabilities the configuration compiled in or out (assertions, profiling, fault seams, the sanitizer). A reader
// compares it with its own build through same_build_identity(), which names every field that differs, before trusting
// the recipe. The binaries themselves are named by the SymbolIndex section beside it (crd/perf/symbol_index.hpp).
//
// Every field is a compile-time constant of the build, so current_build_identity() never enumerates, locks or reads
// the file system: it is safe on a fatal path.
//
// On-disk payload (little-endian, versioned; the bundle section CRC covers it):
//   u32 magic 'CBI1', u32 version, u32 diagnostic schema, u32 flags (kBuildFlag*, a closed set),
//   then version, platform, compiler, compiler_version, arch, config: each a u16 length and that many bytes,
//   at most kBuildFieldBytes. The decoder refuses unknown flags, an over-long or short field and trailing bytes.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::perf
{

inline constexpr crd::u32 kBuildIdentityMagic   = 0x31494243U; // bytes 'C','B','I','1'
inline constexpr crd::u32 kBuildIdentityVersion = 1U;
inline constexpr crd::u32 kBuildFieldBytes      = 64U; // each text field's bound

// BuildIdentity::flags bits.
inline constexpr crd::u32 kBuildFlagAsserts    = 1U << 0; // CRD_ENABLE_ASSERTS
inline constexpr crd::u32 kBuildFlagProfiling  = 1U << 1; // CRD_ENABLE_PROFILING
inline constexpr crd::u32 kBuildFlagFaultSeams = 1U << 2; // fault seams compiled in (not a shipping build)
inline constexpr crd::u32 kBuildFlagAsan       = 1U << 3; // built with AddressSanitizer
inline constexpr crd::u32 kBuildFlagTsan       = 1U << 4; // built with ThreadSanitizer
inline constexpr crd::u32 kBuildKnownFlags =
    kBuildFlagAsserts | kBuildFlagProfiling | kBuildFlagFaultSeams | kBuildFlagAsan | kBuildFlagTsan;

struct BuildIdentity
{
    BuildIdentity() = default;
    explicit BuildIdentity(crd::memory::IAllocator* alloc)
        : version(alloc), platform(alloc), compiler(alloc), compiler_version(alloc), arch(alloc), config(alloc)
    {
    }

    crd::u32                diagnostic_schema = 0U; // cerid-diagnostics/N of the build that wrote it
    crd::u32                flags             = 0U; // kBuildFlag*
    crd::containers::String version;                // CRD_VERSION_STRING
    crd::containers::String platform;               // crd::platform_name()
    crd::containers::String compiler;               // crd::compiler_name()
    crd::containers::String compiler_version;       // e.g. "msvc 195135217", "clang 20.1.8", "gcc 14.2.0"
    crd::containers::String arch;                   // crd::arch_name()
    crd::containers::String config;                 // "debug" or "release"
};

// This process's build.
[[nodiscard]] BuildIdentity current_build_identity(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// True when every field of `a` and `b` matches. Otherwise appends the differing field names to `differing`, comma
// separated, in declaration order ("diagnostic_schema", "version", "platform", "compiler", "compiler_version", "arch",
// "config", then one name per differing flag: "asserts", "profiling", "fault_seams", "asan", "tsan").
[[nodiscard]] bool same_build_identity(const BuildIdentity& a, const BuildIdentity& b,
                                       crd::containers::String& differing);

// Encode `b` as a BuildIdentity section payload. A field longer than kBuildFieldBytes is clipped to it.
[[nodiscard]] crd::containers::Array<crd::u8>
encode_build_identity(const BuildIdentity& b, crd::memory::IAllocator* alloc = crd::memory::default_allocator());

struct BuildIdentityRead
{
    bool                        ok = false;
    crd::containers::StringView error; // why the payload was refused (static text)
    BuildIdentity               identity;
};

// Decode an untrusted BuildIdentity payload. A wrong magic or version, unknown flag bits, a field over
// kBuildFieldBytes or past the payload, and trailing bytes are refused; nothing is read outside `payload`.
[[nodiscard]] BuildIdentityRead
decode_build_identity(crd::containers::ConstSpan<crd::u8> payload,
                      crd::memory::IAllocator*            alloc = crd::memory::default_allocator());

// A one-line JSON object of the identity's fields.
void build_identity_json(const BuildIdentity& b, crd::containers::String& out);

} // namespace crd::perf
