#pragma once

// CPU instruction-set detection and the process's one dispatch level (SIMD-DISPATCH, ADR-0134).
//
// General engine code builds at the architecture's baseline (x64: SSE2); hot kernels build once per level and are
// chosen at startup from `cpu_selection()`. A feature counts only when the CPU reports it and the OS saves the
// register state it needs (XCR0 through XGETBV, after OSXSAVE): a CPU with AVX under an OS that does not save the YMM
// state cannot execute AVX code. On x64 the selection walks every logical CPU the process may run on (the calling
// thread is pinned to each in turn and given back its own affinity) and keeps what all of them can execute, so a
// hybrid part or a mixed virtual machine never selects code one of its cores faults on.
//
// `CRD_CPU_LEVEL` (sse2, sse4.2, avx2, avx512, neon, portable) lowers the level for tests and qualification; a level
// above what every CPU executes, or of another architecture, is refused and the detected level stays. It is read once,
// with the detection, at the first `cpu_selection()` call. No heap memory is allocated.

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
    [[nodiscard]] constexpr bool operator==(const CpuFeatureSet&) const noexcept = default;
};

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
};

// Feature decode: what the words report, and what the OS state lets code execute.
[[nodiscard]] CpuReport decode_cpuid(const CpuidWords& words) noexcept;

// CPUID and XCR0 of the CPU the calling thread runs on (zeros off x64).
[[nodiscard]] CpuidWords read_cpuid_here() noexcept;

// The highest level `report` can execute on `arch`.
[[nodiscard]] CpuLevel cpu_level_of(CpuArch arch, const CpuReport& report) noexcept;

// Adds one visited CPU's report to `into`: features, XCR0 and AVX10 shape are intersected, its core type is recorded,
// and `uniform` falls when it differs from the CPUs before it.
void add_cpu_report(CpuSelection& into, const CpuReport& cpu) noexcept;

// Every logical CPU visited and intersected, with the detected level; selected == detected, no override applied.
[[nodiscard]] CpuSelection detect_cpus() noexcept;

// `detected` with `override_text` (CRD_CPU_LEVEL's value, nullptr when unset) applied to its selected level.
[[nodiscard]] CpuSelection apply_cpu_override(const CpuSelection& detected, const char* override_text) noexcept;

// The process's selection: detect_cpus() and CRD_CPU_LEVEL, read once at the first call.
[[nodiscard]] const CpuSelection& cpu_selection() noexcept;

// `have` runs code built for `want`: the same architecture's level, at or above it.
[[nodiscard]] bool cpu_level_includes(CpuLevel have, CpuLevel want) noexcept;

[[nodiscard]] const char* cpu_level_name(CpuLevel level) noexcept; // the CRD_CPU_LEVEL spelling
[[nodiscard]] bool cpu_level_from_name(const char* name, CpuLevel& out) noexcept;
[[nodiscard]] const char* cpu_feature_name(CpuFeature feature) noexcept;
[[nodiscard]] const char* cpu_override_name(CpuOverride status) noexcept;

} // namespace crd
