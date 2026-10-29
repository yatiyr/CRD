#pragma once

// CPU instruction-set detection, the process's one dispatch level and its data cache sizes (SIMD-DISPATCH, ADR-0134).
//
// General engine code builds at the architecture's baseline (x64: SSE2); hot kernels build once per level and are
// chosen at startup from `cpu_selection()`. A feature counts only when the CPU reports it and the OS saves the
// register state it needs (XCR0 through XGETBV, after OSXSAVE): a CPU with AVX under an OS that does not save the YMM
// state cannot execute AVX code. On x64 the selection walks every logical CPU the process may run on (the calling
// thread is pinned to each in turn and given back its own affinity) and keeps what all of them can execute, so a
// hybrid part or a mixed virtual machine never selects code one of its cores faults on.
//
// `CRD_CPU_LEVEL` (sse2, sse4.2, avx2, avx512, neon, portable) lowers the level for tests and qualification (portable,
// the kernels' scalar reference, on every architecture) and drops the features above that level's definition from
// the ones kernel variants select on; a level above what every CPU executes, or of another architecture, is refused
// and the detected level stays. It is read once, with the detection, at the first
// `cpu_selection()` call; an applied level also lowers the C runtime's own dispatch (`cpu_runtime_dispatch()`). No
// heap memory is allocated.

#include <crd/core/types.hpp>

namespace crd
{

enum class CpuArch : crd::u8
{
    X64,
    Arm64,
    Other,
};

// The dispatch levels. The x64 levels are the x86-64 psABI micro-architecture levels; each includes the one below.
enum class CpuLevel : crd::u8
{
    Portable, // no vector ISA is assumed: an architecture without dispatch levels
    Sse2,     // x86-64-v1, the x64 baseline
    Sse42,    // x86-64-v2: + SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT, CMPXCHG16B, LAHF/SAHF
    Avx2,     // x86-64-v3: + AVX, AVX2, FMA, F16C, BMI1, BMI2, LZCNT, MOVBE, the YMM state saved by the OS
    Avx512,   // x86-64-v4: + AVX-512 F, CD, BW, DQ, VL (or AVX10 at 512 bits), the opmask and ZMM state saved
    Neon,     // ARM64 Advanced SIMD, architectural
};

// x64 features a kernel may select on. The order is the bit index in CpuFeatureSet.
enum class CpuFeature : crd::u8
{
    Sse2,
    Sse3,
    Ssse3,
    Sse41,
    Sse42,
    Popcnt,
    Cx16,
    LahfSahf,
    Movbe,
    Aes,
    Pclmulqdq,
    Xsave,
    Osxsave,
    Avx,
    Fma,
    F16c,
    Avx2,
    Bmi1,
    Bmi2,
    Lzcnt,
    Sha,
    Gfni,
    Vaes,
    Vpclmulqdq,
    AvxVnni,
    Avx512F,
    Avx512Cd,
    Avx512Bw,
    Avx512Dq,
    Avx512Vl,
    Avx512Ifma,
    Avx512Vbmi,
    Avx512Vbmi2,
    Avx512Vnni,
    Avx512Bitalg,
    Avx512Vpopcntdq,
    Avx512Bf16,
    Avx512Fp16,
    Avx10,
    AmxTile,
    AmxBf16,
    AmxInt8,
    Apx,
    Hybrid,
    InvariantTsc,
    Count,
};

struct CpuFeatureSet
{
    crd::u64 bits = 0U;

    [[nodiscard]] constexpr bool has(CpuFeature f) const noexcept
    {
        return ((bits >> static_cast<crd::u32>(f)) & 1U) != 0U;
    }
    constexpr void set(CpuFeature f) noexcept { bits |= crd::u64{1} << static_cast<crd::u32>(f); }
    [[nodiscard]] constexpr CpuFeatureSet with(CpuFeature f) const noexcept
    {
        CpuFeatureSet s = *this;
        s.set(f);
        return s;
    }
    [[nodiscard]] constexpr bool has_all(CpuFeatureSet other) const noexcept { return (other.bits & ~bits) == 0U; }
    [[nodiscard]] constexpr bool operator==(const CpuFeatureSet&) const noexcept = default;
};

// One subleaf of the deterministic cache leaf (4, or 0x8000001D on AMD): type and level; ways, partitions and line;
// sets.
struct CpuidCacheWords
{
    crd::u32 eax = 0U;
    crd::u32 ebx = 0U;
    crd::u32 ecx = 0U;
};

inline constexpr crd::u32 kCpuidCacheSubleaves = 8U; // read until a null type or this many

// The CPUID words and XCR0 the decode reads, as one CPU returned them. Leaves above the reported maximum read zero.
struct CpuidWords
{
    crd::u32 max_leaf = 0U;          // leaf 0 EAX
    crd::u32 max_extended_leaf = 0U; // leaf 0x80000000 EAX
    crd::u32 leaf1_ecx = 0U;
    crd::u32 leaf1_edx = 0U;
    crd::u32 leaf7_ebx = 0U;
    crd::u32 leaf7_ecx = 0U;
    crd::u32 leaf7_edx = 0U;
    crd::u32 leaf7_1_eax = 0U;
    crd::u32 leaf7_1_edx = 0U;
    crd::u32 leaf1a_eax = 0U; // hybrid core type in bits 31:24
    crd::u32 leaf24_ebx = 0U; // AVX10 version in bits 7:0, vector lengths in bits 18:16
    crd::u32 ext1_ecx = 0U;   // leaf 0x80000001
    crd::u32 ext7_edx = 0U;   // leaf 0x80000007
    crd::u64 xcr0 = 0U;       // XGETBV(0); 0 unless OSXSAVE
    crd::u32 cache_leaf = 0U; // 4 or 0x8000001D when that leaf describes the caches, else 0
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): that leaf's subleaves in order
    CpuidCacheWords cache[kCpuidCacheSubleaves] = {};
    crd::u32 ext5_ecx = 0U; // leaf 0x80000005: L1 data cache
    crd::u32 ext6_ecx = 0U; // leaf 0x80000006: L2 cache
    crd::u32 ext6_edx = 0U; // and L3 cache
};

enum class CpuCacheSource : crd::u8
{
    None,          // no leaf describes the caches: every size is 0
    Deterministic, // leaf 4 or 0x8000001D
    Legacy,        // leaves 0x80000005 and 0x80000006
};

// The data caches one CPU sees, or the smallest of each over CPUs, in bytes; 0 where unknown. CPUID's sharing word
// bounds the addressable IDs, not the CPUs sharing a cache, so no per-CPU share is derived from it.
struct CpuCaches
{
    crd::u32 line_bytes = 0U;
    crd::u32 l1d_bytes = 0U;
    crd::u32 l2_bytes = 0U;
    crd::u32 l3_bytes = 0U;
    CpuCacheSource source = CpuCacheSource::None;

    [[nodiscard]] constexpr bool operator==(const CpuCaches&) const noexcept = default;
};

// What one CPU, or the intersection over CPUs, can do.
struct CpuReport
{
    CpuFeatureSet reported; // the CPU implements it (CPUID)
    CpuFeatureSet usable;   // and the OS saves the state it needs: what code may execute
    crd::u64 xcr0 = 0U;
    crd::u8 avx10_version = 0U;      // 0: no AVX10
    crd::u16 avx10_vector_bits = 0U; // the widest usable AVX10 vector, 0 when none
    crd::u8 core_type = 0U;          // CPUID 0x1A: 0x20 efficiency, 0x40 performance; 0 when not hybrid and
                                     // in an intersection (CpuSelection::core_types_seen)
    CpuCaches caches;                // in an intersection the smallest of each over the CPUs (a hybrid part's
                                     // efficiency cores), None when any CPU described none

    [[nodiscard]] constexpr bool operator==(const CpuReport&) const noexcept = default;
};

enum class CpuOverride : crd::u8
{
    None,          // CRD_CPU_LEVEL is unset or empty
    Applied,       // the asked level is at or below the detected one, and is the selected level
    Unknown,       // not a level name; the detected level stays
    OtherArch,     // a level of another architecture; the detected level stays
    AboveHardware, // above what every CPU executes; refused, the detected level stays
};

// Bits of CpuSelection::core_types_seen.
inline constexpr crd::u8 kCpuCoreEfficiency = 1U;
inline constexpr crd::u8 kCpuCorePerformance = 2U;
inline constexpr crd::u8 kCpuCoreOther = 4U;

struct CpuSelection
{
    CpuArch arch = CpuArch::Other;
    CpuReport features;           // the intersection over every CPU visited
    crd::u32 cpus_visited = 0U;   // CPUs the walk ran CPUID on
    crd::u32 cpus_refused = 0U;   // CPUs the process may not run on (affinity, cpuset) or that are offline
    bool uniform = true;          // every visited CPU reported the same usable features and AVX10 shape
    crd::u8 core_types_seen = 0U; // kCpuCore* bits of the visited CPUs
    const char* walk_note = "";   // a static string, set when only the current CPU could be read
    CpuLevel detected = CpuLevel::Portable;
    CpuLevel selected = CpuLevel::Portable;
    CpuOverride override_status = CpuOverride::None;
    // The features a kernel variant may select on (ADR-0134 §1): features.usable, or under an applied CRD_CPU_LEVEL
    // only the features the selected level defines, so a forced arm runs as the smallest CPU of its level.
    CpuFeatureSet kernel_features;
};

// Feature decode: what the words report, and what the OS state lets code execute.
[[nodiscard]] CpuReport decode_cpuid(const CpuidWords& words) noexcept;

// CPUID and XCR0 of the CPU the calling thread runs on (zeros off x64).
[[nodiscard]] CpuidWords read_cpuid_here() noexcept;

// The highest level `report` can execute on `arch`.
[[nodiscard]] CpuLevel cpu_level_of(CpuArch arch, const CpuReport& report) noexcept;

// The features `level` is defined by, its lower levels' included (empty for portable and neon).
[[nodiscard]] CpuFeatureSet cpu_level_features(CpuLevel level) noexcept;

// Adds one visited CPU's report to `into`: features, XCR0 and AVX10 shape are intersected, each cache size takes its
// minimum, its core type is recorded, and `uniform` falls when its features differ from the CPUs before it.
void add_cpu_report(CpuSelection& into, const CpuReport& cpu) noexcept;

// Every logical CPU visited and intersected, with the detected level; selected == detected, no override applied.
[[nodiscard]] CpuSelection detect_cpus() noexcept;

// `detected` with `override_text` (CRD_CPU_LEVEL's value, nullptr when unset) applied to its selected level.
[[nodiscard]] CpuSelection apply_cpu_override(const CpuSelection& detected, const char* override_text) noexcept;

// The process's selection: detect_cpus() and CRD_CPU_LEVEL, read once at the first call (MSVC x64: at startup when
// CRD_CPU_LEVEL is set, see CpuRuntimeDispatch).
[[nodiscard]] const CpuSelection& cpu_selection() noexcept;

// The C runtime's own ISA dispatch in this image (MSVC and clang-cl on x64, ADR-0134 §8). crt_level is the CRT's
// __isa_available (1 SSE2, 2 SSE4.2, 3 AVX, 5 AVX2, 6 AVX-512), which the compiler's vectorized paths and the STL
// test; ucrt_fma3 says whether the UCRT's libm runs its FMA3 bodies. An applied CRD_CPU_LEVEL lowers both, and the
// CRT's other ISA variables, to its level before the first user initializer; nothing raises them. Elsewhere
// crt_level is -1 and ucrt_fma3 false.
struct CpuRuntimeDispatch
{
    crd::i32 crt_level = -1;
    bool ucrt_fma3 = false;
};
[[nodiscard]] CpuRuntimeDispatch cpu_runtime_dispatch() noexcept;

#if defined(_MSC_VER) && defined(_M_X64) && !defined(_M_ARM64EC)
namespace detail
{
// cpu_runtime_dispatch().ucrt_fma3 as a variable, for crd::math's fma (crd/math/libm.hpp), which calls the UCRT's fma
// only while it runs the FMA3 instruction. Written before the first user initializer, after an applied CRD_CPU_LEVEL
// lowered the CRT's dispatch; false until then, which selects the exact integer body.
extern bool ucrt_fma3_active; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): the CRT's own state
} // namespace detail
#endif

// The selection as the one line the doctor, the specimen harness's host tuple and every board record carry
// (ADR-0134 §8): "cpu detected=<level> selected=<level> override=<status>", then " crt=<level> fma3=<0|1>" when `crt`
// reports a C runtime dispatch of its own (crt_level >= 0), then " extra=<feature>,..." naming the kernel features
// above the selected level's definition (none under an applied CRD_CPU_LEVEL). Writes at most capacity - 1
// characters and a terminating NUL (capacity > 0) and returns the line's full length: a result >= capacity was cut.
[[nodiscard]] crd::usize format_cpu_selection(const CpuSelection& selection, const CpuRuntimeDispatch& crt, char* out,
                                              crd::usize capacity) noexcept;

// `have` runs code built for `want`: the same architecture's level, at or above it.
[[nodiscard]] bool cpu_level_includes(CpuLevel have, CpuLevel want) noexcept;

[[nodiscard]] const char* cpu_level_name(CpuLevel level) noexcept; // the CRD_CPU_LEVEL spelling
[[nodiscard]] bool cpu_level_from_name(const char* name, CpuLevel& out) noexcept;
[[nodiscard]] const char* cpu_feature_name(CpuFeature feature) noexcept;
[[nodiscard]] const char* cpu_override_name(CpuOverride status) noexcept;

} // namespace crd
