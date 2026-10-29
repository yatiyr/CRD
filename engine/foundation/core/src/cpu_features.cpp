// CPU instruction-set detection and the dispatch-level selection (crd/core/cpu_features.hpp, ADR-0134).

#include <crd/core/cpu_features.hpp>
#include <crd/core/environment.hpp>
#include <crd/core/platform.hpp>

#include <cstring>
#include <initializer_list>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if CRD_ARCH_X64
#include <intrin.h>
#endif
#if defined(_MSC_VER) && CRD_ARCH_X64
#include <cmath>
#include <isa_availability.h>
// The CRT's view of the CPU, one copy per image (vcruntime's cpu_disp.obj, set before any C++ initializer):
// __isa_available is the level the compiler's vectorized paths and the STL test, __isa_enabled its bit set,
// __isa_inverted the __IA_SUPPORT_* bits __check_isa_support() reads (a set bit: not supported).
extern "C"
{
    extern int __isa_available;               // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)
    extern int __isa_enabled;                 // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)
    extern unsigned long long __isa_inverted; // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)
    extern unsigned __avx10_version;          // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)
}
#endif
#elif CRD_OS_LINUX
#include <cstdio>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#if CRD_ARCH_X64
#include <cpuid.h>
#endif
#endif

namespace crd
{
namespace
{

// The CPUID word a feature bit lives in.
enum class Word : crd::u8
{
    Leaf1Ecx,
    Leaf1Edx,
    Leaf7Ebx,
    Leaf7Ecx,
    Leaf7Edx,
    Leaf71Eax,
    Leaf71Edx,
    Ext1Ecx,
    Ext7Edx,
};

// XCR0 state components a feature's registers need: SSE (bit 1) and AVX (bit 2) for YMM; opmask (5), ZMM_Hi256 (6)
// and Hi16_ZMM (7) for AVX-512 and AVX10; XTILECFG (17) and XTILEDATA (18) for AMX; the APX extended GPRs (19).
constexpr crd::u64 kStateNone = 0U;
constexpr crd::u64 kStateYmm = 0x6U;
constexpr crd::u64 kStateZmm = 0xE6U;
constexpr crd::u64 kStateTile = 0x60000U;
constexpr crd::u64 kStateApx = 0x80000U;

struct FeatureBit
{
    CpuFeature feature;
    Word word;
    crd::u8 bit;
    crd::u64 state;
    const char* name;
};

// Intel SDM vol. 2A CPUID and AMD APM vol. 3 appendix E; GFNI keeps its legacy SSE encoding, so it needs no XCR0 state.
// NOLINTNEXTLINE(modernize-avoid-c-arrays): a constant table indexed by CpuFeature
constexpr FeatureBit kFeatures[] = {
    {CpuFeature::Sse2, Word::Leaf1Edx, 26U, kStateNone, "sse2"},
    {CpuFeature::Sse3, Word::Leaf1Ecx, 0U, kStateNone, "sse3"},
    {CpuFeature::Ssse3, Word::Leaf1Ecx, 9U, kStateNone, "ssse3"},
    {CpuFeature::Sse41, Word::Leaf1Ecx, 19U, kStateNone, "sse4.1"},
    {CpuFeature::Sse42, Word::Leaf1Ecx, 20U, kStateNone, "sse4.2"},
    {CpuFeature::Popcnt, Word::Leaf1Ecx, 23U, kStateNone, "popcnt"},
    {CpuFeature::Cx16, Word::Leaf1Ecx, 13U, kStateNone, "cx16"},
    {CpuFeature::LahfSahf, Word::Ext1Ecx, 0U, kStateNone, "lahf-sahf"},
    {CpuFeature::Movbe, Word::Leaf1Ecx, 22U, kStateNone, "movbe"},
    {CpuFeature::Aes, Word::Leaf1Ecx, 25U, kStateNone, "aes"},
    {CpuFeature::Pclmulqdq, Word::Leaf1Ecx, 1U, kStateNone, "pclmulqdq"},
    {CpuFeature::Xsave, Word::Leaf1Ecx, 26U, kStateNone, "xsave"},
    {CpuFeature::Osxsave, Word::Leaf1Ecx, 27U, kStateNone, "osxsave"},
    {CpuFeature::Avx, Word::Leaf1Ecx, 28U, kStateYmm, "avx"},
    {CpuFeature::Fma, Word::Leaf1Ecx, 12U, kStateYmm, "fma"},
    {CpuFeature::F16c, Word::Leaf1Ecx, 29U, kStateYmm, "f16c"},
    {CpuFeature::Avx2, Word::Leaf7Ebx, 5U, kStateYmm, "avx2"},
    {CpuFeature::Bmi1, Word::Leaf7Ebx, 3U, kStateNone, "bmi1"},
    {CpuFeature::Bmi2, Word::Leaf7Ebx, 8U, kStateNone, "bmi2"},
    {CpuFeature::Lzcnt, Word::Ext1Ecx, 5U, kStateNone, "lzcnt"},
    {CpuFeature::Sha, Word::Leaf7Ebx, 29U, kStateNone, "sha"},
    {CpuFeature::Gfni, Word::Leaf7Ecx, 8U, kStateNone, "gfni"},
    {CpuFeature::Vaes, Word::Leaf7Ecx, 9U, kStateYmm, "vaes"},
    {CpuFeature::Vpclmulqdq, Word::Leaf7Ecx, 10U, kStateYmm, "vpclmulqdq"},
    {CpuFeature::AvxVnni, Word::Leaf71Eax, 4U, kStateYmm, "avx-vnni"},
    {CpuFeature::Avx512F, Word::Leaf7Ebx, 16U, kStateZmm, "avx512f"},
    {CpuFeature::Avx512Cd, Word::Leaf7Ebx, 28U, kStateZmm, "avx512cd"},
    {CpuFeature::Avx512Bw, Word::Leaf7Ebx, 30U, kStateZmm, "avx512bw"},
    {CpuFeature::Avx512Dq, Word::Leaf7Ebx, 17U, kStateZmm, "avx512dq"},
    {CpuFeature::Avx512Vl, Word::Leaf7Ebx, 31U, kStateZmm, "avx512vl"},
    {CpuFeature::Avx512Ifma, Word::Leaf7Ebx, 21U, kStateZmm, "avx512ifma"},
    {CpuFeature::Avx512Vbmi, Word::Leaf7Ecx, 1U, kStateZmm, "avx512vbmi"},
    {CpuFeature::Avx512Vbmi2, Word::Leaf7Ecx, 6U, kStateZmm, "avx512vbmi2"},
    {CpuFeature::Avx512Vnni, Word::Leaf7Ecx, 11U, kStateZmm, "avx512vnni"},
    {CpuFeature::Avx512Bitalg, Word::Leaf7Ecx, 12U, kStateZmm, "avx512bitalg"},
    {CpuFeature::Avx512Vpopcntdq, Word::Leaf7Ecx, 14U, kStateZmm, "avx512vpopcntdq"},
    {CpuFeature::Avx512Bf16, Word::Leaf71Eax, 5U, kStateZmm, "avx512bf16"},
    {CpuFeature::Avx512Fp16, Word::Leaf7Edx, 23U, kStateZmm, "avx512fp16"},
    {CpuFeature::Avx10, Word::Leaf71Edx, 19U, kStateZmm, "avx10"},
    {CpuFeature::AmxTile, Word::Leaf7Edx, 24U, kStateTile, "amx-tile"},
    {CpuFeature::AmxBf16, Word::Leaf7Edx, 22U, kStateTile, "amx-bf16"},
    {CpuFeature::AmxInt8, Word::Leaf7Edx, 25U, kStateTile, "amx-int8"},
    {CpuFeature::Apx, Word::Leaf71Edx, 21U, kStateApx, "apx"},
    {CpuFeature::Hybrid, Word::Leaf7Edx, 15U, kStateNone, "hybrid"},
    {CpuFeature::InvariantTsc, Word::Ext7Edx, 8U, kStateNone, "invariant-tsc"},
};
static_assert(sizeof(kFeatures) / sizeof(kFeatures[0]) == static_cast<crd::usize>(CpuFeature::Count),
              "one table row per CpuFeature");
static_assert(static_cast<crd::usize>(CpuFeature::Count) <= 64U, "CpuFeatureSet holds 64 features");

constexpr bool table_in_enum_order() noexcept
{
    for (crd::usize i = 0U; i < sizeof(kFeatures) / sizeof(kFeatures[0]); ++i)
    {
        if (static_cast<crd::usize>(kFeatures[i].feature) != i)
        {
            return false;
        }
    }
    return true;
}
static_assert(table_in_enum_order(), "kFeatures is indexed by CpuFeature");

crd::u32 word_of(const CpuidWords& w, Word which) noexcept
{
    const bool leaf1 = w.max_leaf >= 1U;
    const bool leaf7 = w.max_leaf >= 7U;
    const bool ext1 = w.max_extended_leaf >= 0x80000001U;
    const bool ext7 = w.max_extended_leaf >= 0x80000007U;
    crd::u32 result = 0U;
    switch (which)
    {
        case Word::Leaf1Ecx:
            result = leaf1 ? w.leaf1_ecx : 0U;
            break;
        case Word::Leaf1Edx:
            result = leaf1 ? w.leaf1_edx : 0U;
            break;
        case Word::Leaf7Ebx:
            result = leaf7 ? w.leaf7_ebx : 0U;
            break;
        case Word::Leaf7Ecx:
            result = leaf7 ? w.leaf7_ecx : 0U;
            break;
        case Word::Leaf7Edx:
            result = leaf7 ? w.leaf7_edx : 0U;
            break;
        case Word::Leaf71Eax:
            result = leaf7 ? w.leaf7_1_eax : 0U;
            break;
        case Word::Leaf71Edx:
            result = leaf7 ? w.leaf7_1_edx : 0U;
            break;
        case Word::Ext1Ecx:
            result = ext1 ? w.ext1_ecx : 0U;
            break;
        case Word::Ext7Edx:
            result = ext7 ? w.ext7_edx : 0U;
            break;
    }
    return result;
}

constexpr CpuArch kArch = CRD_ARCH_X64 ? CpuArch::X64 : (CRD_ARCH_ARM64 ? CpuArch::Arm64 : CpuArch::Other);

CpuArch arch_of(CpuLevel level) noexcept
{
    switch (level)
    {
        case CpuLevel::Sse2:
        case CpuLevel::Sse42:
        case CpuLevel::Avx2:
        case CpuLevel::Avx512:
            return CpuArch::X64;
        case CpuLevel::Neon:
            return CpuArch::Arm64;
        case CpuLevel::Portable:
            break;
    }
    return CpuArch::Other;
}

constexpr CpuFeatureSet features_of(std::initializer_list<CpuFeature> list) noexcept
{
    CpuFeatureSet s;
    for (const CpuFeature f : list)
    {
        s.set(f);
    }
    return s;
}

// The x86-64 psABI levels, each with its own additions to the one below.
using F = CpuFeature;
constexpr CpuFeatureSet kV1 = features_of({F::Sse2});
constexpr CpuFeatureSet kV2 = features_of({F::Sse3, F::Ssse3, F::Sse41, F::Sse42, F::Popcnt, F::Cx16, F::LahfSahf});
constexpr CpuFeatureSet kV3 =
    features_of({F::Avx, F::Avx2, F::Fma, F::F16c, F::Bmi1, F::Bmi2, F::Lzcnt, F::Movbe, F::Osxsave});
constexpr CpuFeatureSet kV4 = features_of({F::Avx512F, F::Avx512Cd, F::Avx512Bw, F::Avx512Dq, F::Avx512Vl});

#if CRD_ARCH_X64
void cpuid(crd::u32 leaf, crd::u32 subleaf, crd::u32 (&out)[4]) noexcept // NOLINT(modernize-avoid-c-arrays)
{
#if CRD_OS_WINDOWS
    int regs[4] = {}; // NOLINT(modernize-avoid-c-arrays): __cpuidex's out-parameter
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    for (int i = 0; i < 4; ++i)
    {
        out[i] = static_cast<crd::u32>(regs[i]);
    }
#else
    __cpuid_count(leaf, subleaf, out[0], out[1], out[2], out[3]);
#endif
}

// XGETBV(0). GCC and Clang gate the _xgetbv intrinsic behind -mxsave, which this baseline unit does not take. The
// caller runs it only when CPUID reports OSXSAVE; kept out of line, it is the one function of general code the ISA
// isolation guard's reviewed XGETBV exception names (scripts/isa-isolation.json).
CRD_NOINLINE crd::u64 read_xcr0() noexcept
{
#if CRD_COMPILER_MSVC
    return _xgetbv(0U);
#else
    crd::u32 low = 0U;
    crd::u32 high = 0U;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0U));
    return (static_cast<crd::u64>(high) << 32U) | low;
#endif
}
#endif

void visit_here(CpuSelection& s) noexcept
{
    add_cpu_report(s, decode_cpuid(read_cpuid_here()));
}

// Pin the calling thread to each logical CPU in turn, read it there, and give the thread its own affinity back.
// Returns false when the affinity could not be read or set at all.
bool walk_every_cpu(CpuSelection& s) noexcept
{
#if CRD_OS_WINDOWS
    const HANDLE thread = ::GetCurrentThread();
    GROUP_AFFINITY original = {};
    if (::GetThreadGroupAffinity(thread, &original) == 0)
    {
        return false;
    }
    const WORD groups = ::GetActiveProcessorGroupCount();
    for (WORD g = 0U; g < groups; ++g)
    {
        const DWORD count = ::GetActiveProcessorCount(g);
        for (DWORD i = 0U; i < count && i < 64U; ++i)
        {
            GROUP_AFFINITY one = {};
            one.Group = g;
            one.Mask = KAFFINITY{1} << i;
            if (::SetThreadGroupAffinity(thread, &one, nullptr) == 0)
            {
                ++s.cpus_refused;
                continue;
            }
            PROCESSOR_NUMBER at = {};
            ::GetCurrentProcessorNumberEx(&at);
            if (at.Group != g || at.Number != i)
            {
                ::SwitchToThread();
                ::GetCurrentProcessorNumberEx(&at);
            }
            if (at.Group != g || at.Number != i)
            {
                ++s.cpus_refused;
                continue;
            }
            visit_here(s);
        }
    }
    ::SetThreadGroupAffinity(thread, &original, nullptr);
    return true;
#elif CRD_OS_LINUX
    cpu_set_t original[8]; // NOLINT(modernize-avoid-c-arrays): 8,192 CPUs without the heap of CPU_ALLOC
    cpu_set_t one[8];      // NOLINT(modernize-avoid-c-arrays)
    constexpr crd::usize bytes = sizeof(original);
    if (::sched_getaffinity(0, bytes, original) != 0)
    {
        return false;
    }
    const long configured = ::sysconf(_SC_NPROCESSORS_CONF);
    const int count = configured > 0 ? static_cast<int>(configured < 8192 ? configured : 8192) : 0;
    for (int cpu = 0; cpu < count; ++cpu)
    {
        CPU_ZERO_S(bytes, one);
        CPU_SET_S(static_cast<crd::usize>(cpu), bytes, one);
        if (::sched_setaffinity(0, bytes, one) != 0)
        {
            ++s.cpus_refused;
            continue;
        }
        if (::sched_getcpu() != cpu)
        {
            ::sched_yield();
        }
        if (::sched_getcpu() != cpu)
        {
            ++s.cpus_refused;
            continue;
        }
        visit_here(s);
    }
    ::sched_setaffinity(0, bytes, original);
    return true;
#else
    (void)s;
    return false;
#endif
}

#if CRD_OS_WINDOWS || CRD_OS_LINUX
// One L2 instance folded into the selection's OS fields, each the smallest so far (0: none yet).
void take_l2_instance(CpuSelection& s, crd::u64 bytes, crd::u32 sharers) noexcept
{
    if (bytes == 0U || sharers == 0U)
    {
        return;
    }
    const auto take = [](crd::u32& into, crd::u64 value) noexcept
    {
        const crd::u32 clamped = value > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<crd::u32>(value);
        into = into == 0U || clamped < into ? clamped : into;
    };
    take(s.os_l2_bytes, bytes);
    take(s.l2_share_bytes, bytes / sharers);
}
#endif

#if CRD_OS_LINUX
// Opens one sysfs attribute of `cpu`'s cache `index`; -1 when absent.
int open_cache_attribute(int cpu, int index, const char* name) noexcept
{
    char path[96] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/%s", cpu, index, name);
    return ::open(path, O_RDONLY | O_CLOEXEC);
}

// The attribute's first bytes, NUL-terminated; false when it is absent or empty.
// NOLINTNEXTLINE(modernize-avoid-c-arrays)
bool read_cache_attribute(int cpu, int index, const char* name, char (&out)[32]) noexcept
{
    const int fd = open_cache_attribute(cpu, index, name);
    if (fd < 0)
    {
        return false;
    }
    const ssize_t got = ::read(fd, out, sizeof(out) - 1U);
    (void)::close(fd);
    out[got > 0 ? got : 0] = '\0';
    return got > 0;
}

// The CPUs a shared_cpu_map names (comma-separated hex words), counted as it streams, so no CPU count bounds it.
crd::u32 count_cache_sharers(int cpu, int index) noexcept
{
    const int fd = open_cache_attribute(cpu, index, "shared_cpu_map");
    if (fd < 0)
    {
        return 0U;
    }
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): the set bits of each hex digit
    constexpr crd::u8 kNibbleBits[16] = {0U, 1U, 1U, 2U, 1U, 2U, 2U, 3U, 1U, 2U, 2U, 3U, 2U, 3U, 3U, 4U};
    crd::u32 sharers = 0U;
    char chunk[64] = {}; // NOLINT(modernize-avoid-c-arrays)
    for (ssize_t got = ::read(fd, chunk, sizeof(chunk)); got > 0; got = ::read(fd, chunk, sizeof(chunk)))
    {
        for (ssize_t i = 0; i < got; ++i)
        {
            const char c = chunk[i];
            if (c >= '0' && c <= '9')
            {
                sharers += kNibbleBits[c - '0'];
            }
            else if (c >= 'a' && c <= 'f')
            {
                sharers += kNibbleBits[c - 'a' + 10];
            }
            else if (c >= 'A' && c <= 'F')
            {
                sharers += kNibbleBits[c - 'A' + 10];
            }
        }
    }
    (void)::close(fd);
    return sharers;
}
#elif CRD_OS_WINDOWS
crd::u32 count_mask_bits(KAFFINITY mask) noexcept // a plain loop: std::popcount is POPCNT behind a CRT branch
{
    crd::u32 n = 0U;
    for (; mask != 0U; mask &= mask - 1U)
    {
        ++n;
    }
    return n;
}
#endif

// CpuSelection::os_l2_bytes and l2_share_bytes: every L2 instance the OS describes and the logical CPUs sharing it.
void read_os_l2(CpuSelection& s) noexcept
{
#if CRD_OS_WINDOWS
    DWORD len = 0U;
    (void)::GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    if (len == 0U)
    {
        return;
    }
    void* const buffer = ::VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); // pages, no heap
    if (buffer == nullptr)
    {
        return;
    }
    const auto* const bytes = static_cast<const unsigned char*>(buffer);
    if (::GetLogicalProcessorInformationEx(RelationCache, static_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer),
                                           &len) != 0)
    {
        for (DWORD off = 0U; off < len;)
        {
            const auto* p = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(bytes + off);
            const CACHE_RELATIONSHIP& c = p->Cache;
            if (p->Relationship == RelationCache && c.Level == 2U && (c.Type == CacheData || c.Type == CacheUnified))
            {
                const WORD groups = c.GroupCount == 0U ? WORD{1} : c.GroupCount; // 0: one GroupMask (older OS)
                const GROUP_AFFINITY* masks = c.GroupMasks;
                crd::u32 sharers = 0U;
                for (WORD g = 0U; g < groups; ++g)
                {
                    sharers += count_mask_bits(masks[g].Mask);
                }
                take_l2_instance(s, c.CacheSize, sharers);
            }
            off += p->Size;
        }
    }
    (void)::VirtualFree(buffer, 0U, MEM_RELEASE);
#elif CRD_OS_LINUX
    const long configured = ::sysconf(_SC_NPROCESSORS_CONF);
    const int count = configured > 0 ? static_cast<int>(configured < 8192 ? configured : 8192) : 0;
    for (int cpu = 0; cpu < count; ++cpu)
    {
        for (int index = 0;; ++index) // offline CPUs and sandboxes that hide the cache directories end at once
        {
            char level[32] = {}; // NOLINT(modernize-avoid-c-arrays)
            char type[32] = {};  // NOLINT(modernize-avoid-c-arrays)
            char size[32] = {};  // NOLINT(modernize-avoid-c-arrays)
            if (!read_cache_attribute(cpu, index, "level", level))
            {
                break;
            }
            if (level[0] != '2' || (level[1] >= '0' && level[1] <= '9') ||
                !read_cache_attribute(cpu, index, "type", type) || std::strncmp(type, "Instruction", 11U) == 0 ||
                !read_cache_attribute(cpu, index, "size", size))
            {
                continue;
            }
            crd::u64 cache_bytes = 0U;
            const char* digit = size;
            for (; *digit >= '0' && *digit <= '9'; ++digit)
            {
                cache_bytes = cache_bytes * 10U + static_cast<crd::u64>(*digit - '0');
            }
            cache_bytes <<= *digit == 'K' ? 10U : (*digit == 'M' ? 20U : (*digit == 'G' ? 30U : 0U));
            take_l2_instance(s, cache_bytes, count_cache_sharers(cpu, index));
        }
    }
#else
    (void)s;
#endif
}

// CRD_CPU_LEVEL's value copied into `buffer`; nullptr when unset, "?" (an unknown name) when too long for one.
const char* read_override(char (&buffer)[32]) noexcept // NOLINT(modernize-avoid-c-arrays)
{
    switch (read_environment("CRD_CPU_LEVEL", buffer, sizeof(buffer)))
    {
        case EnvironmentRead::Set:
            return buffer;
        case EnvironmentRead::TooLong:
            return "?";
        case EnvironmentRead::Unset:
            break;
    }
    return nullptr;
}

#if defined(_MSC_VER) && CRD_ARCH_X64
// The CRT level of `level`: no CRT level sits between SSE2 and SSE4.2, and portable runs on the SSE2 floor.
int crt_level_of(CpuLevel level) noexcept
{
    switch (level)
    {
        case CpuLevel::Sse42:
            return __ISA_AVAILABLE_SSE42;
        case CpuLevel::Avx2:
            return __ISA_AVAILABLE_AVX2;
        case CpuLevel::Avx512:
            return __ISA_AVAILABLE_AVX512;
        case CpuLevel::Portable:
        case CpuLevel::Sse2:
        case CpuLevel::Neon:
            break;
    }
    return __ISA_AVAILABLE_SSE2;
}

// Lowers the CRT's own dispatch to `level`, never raising it, so the compiler's and the STL's runtime paths and the
// UCRT's libm run what a CPU of that level runs. No level includes AVX10, APX or FP16.
void lower_crt_dispatch(CpuLevel level) noexcept
{
    const int cap = crt_level_of(level);
    unsigned long long supported = __IA_SUPPORT_VECTOR128;
    if (cap >= __ISA_AVAILABLE_SSE42)
    {
        supported |= __IA_SUPPORT_SSE42;
    }
    if (cap >= __ISA_AVAILABLE_AVX2)
    {
        supported |= __IA_SUPPORT_VECTOR256;
    }
    if (cap >= __ISA_AVAILABLE_AVX512)
    {
        supported |= __IA_SUPPORT_VECTOR512 | __IA_SUPPORT_SV128X;
    }
    constexpr unsigned long long known = __IA_SUPPORT_VECTOR128 | __IA_SUPPORT_VECTOR256 | __IA_SUPPORT_VECTOR512 |
                                         __IA_SUPPORT_AVX10 | __IA_SUPPORT_SSE42 | __IA_SUPPORT_SV128X |
                                         __IA_SUPPORT_AVX10_2 | __IA_SUPPORT_APX | __IA_SUPPORT_FP16;
    if (__isa_available > cap)
    {
        __isa_available = cap;
    }
    __isa_enabled &= (1 << (cap + 1)) - 1;
    __isa_inverted |= known & ~supported;
    __avx10_version = 0U;
    if (cap < __ISA_AVAILABLE_AVX2)
    {
        (void)_set_FMA3_enable(0);
    }
}
#endif

// The data caches the words describe: the deterministic leaf's data and unified caches, else the legacy leaves.
CpuCaches decode_caches(const CpuidWords& words) noexcept
{
    CpuCaches c;
    const auto clamp = [](crd::u64 bytes) noexcept
    {
        return static_cast<crd::u32>(bytes < 0xFFFFFFFFULL ? bytes : 0xFFFFFFFFULL);
    };
    if (words.cache_leaf != 0U)
    {
        for (const CpuidCacheWords& d : words.cache)
        {
            const crd::u32 type = d.eax & 0x1FU; // 1 data, 2 instruction, 3 unified
            if (type == 0U)
            {
                break;
            }
            if (type == 2U)
            {
                continue;
            }
            const crd::u64 line = (d.ebx & 0xFFFU) + 1U;
            const crd::u64 bytes = (((d.ebx >> 22U) & 0x3FFU) + 1U) * (((d.ebx >> 12U) & 0x3FFU) + 1U) * line *
                                   (static_cast<crd::u64>(d.ecx) + 1U);
            switch ((d.eax >> 5U) & 0x7U)
            {
                case 1U:
                    c.l1d_bytes = clamp(bytes);
                    c.line_bytes = static_cast<crd::u32>(line);
                    break;
                case 2U:
                    c.l2_bytes = clamp(bytes);
                    break;
                case 3U:
                    c.l3_bytes = clamp(bytes);
                    break;
                default:
                    break;
            }
        }
        c.source = CpuCacheSource::Deterministic;
    }
    else if (words.max_extended_leaf >= 0x80000006U && words.ext6_ecx != 0U)
    {
        c.l1d_bytes = (words.ext5_ecx >> 24U) * 1024U;
        c.line_bytes = words.ext5_ecx != 0U ? (words.ext5_ecx & 0xFFU) : (words.ext6_ecx & 0xFFU);
        c.l2_bytes = (words.ext6_ecx >> 16U) * 1024U;
        c.l3_bytes = clamp(static_cast<crd::u64>(words.ext6_edx >> 18U) * 512U * 1024U);
        c.source = CpuCacheSource::Legacy;
    }
    return c;
}

} // namespace

CpuReport decode_cpuid(const CpuidWords& words) noexcept
{
    CpuReport r;
    const bool osxsave = ((word_of(words, Word::Leaf1Ecx) >> 27U) & 1U) != 0U;
    r.xcr0 = osxsave ? words.xcr0 : 0U;
    for (const FeatureBit& f : kFeatures)
    {
        if (((word_of(words, f.word) >> f.bit) & 1U) == 0U)
        {
            continue;
        }
        r.reported.set(f.feature);
        if (f.state == kStateNone || (r.xcr0 & f.state) == f.state)
        {
            r.usable.set(f.feature);
        }
    }
    if (r.reported.has(CpuFeature::Avx10) && words.max_leaf >= 0x24U)
    {
        r.avx10_version = static_cast<crd::u8>(words.leaf24_ebx & 0xFFU);
        crd::u16 widest = 0U;
        for (crd::u32 b = 0U; b < 3U; ++b)
        {
            if (((words.leaf24_ebx >> (16U + b)) & 1U) != 0U)
            {
                widest = static_cast<crd::u16>(128U << b);
            }
        }
        r.avx10_vector_bits = r.usable.has(CpuFeature::Avx10) ? widest : 0U;
    }
    if (r.reported.has(CpuFeature::Hybrid) && words.max_leaf >= 0x1AU)
    {
        r.core_type = static_cast<crd::u8>(words.leaf1a_eax >> 24U);
    }
    r.caches = decode_caches(words);
    return r;
}

CpuidWords read_cpuid_here() noexcept
{
    CpuidWords w;
#if CRD_ARCH_X64
    crd::u32 r[4] = {}; // NOLINT(modernize-avoid-c-arrays)
    cpuid(0U, 0U, r);
    w.max_leaf = r[0];
    cpuid(0x80000000U, 0U, r);
    w.max_extended_leaf = r[0];
    if (w.max_leaf >= 1U)
    {
        cpuid(1U, 0U, r);
        w.leaf1_ecx = r[2];
        w.leaf1_edx = r[3];
    }
    if (w.max_leaf >= 7U)
    {
        cpuid(7U, 0U, r);
        const crd::u32 subleaves = r[0];
        w.leaf7_ebx = r[1];
        w.leaf7_ecx = r[2];
        w.leaf7_edx = r[3];
        if (subleaves >= 1U)
        {
            cpuid(7U, 1U, r);
            w.leaf7_1_eax = r[0];
            w.leaf7_1_edx = r[3];
        }
    }
    if (w.max_leaf >= 0x1AU)
    {
        cpuid(0x1AU, 0U, r);
        w.leaf1a_eax = r[0];
    }
    if (w.max_leaf >= 0x24U)
    {
        cpuid(0x24U, 0U, r);
        w.leaf24_ebx = r[1];
    }
    if (w.max_extended_leaf >= 0x80000001U)
    {
        cpuid(0x80000001U, 0U, r);
        w.ext1_ecx = r[2];
    }
    if (w.max_extended_leaf >= 0x80000007U)
    {
        cpuid(0x80000007U, 0U, r);
        w.ext7_edx = r[3];
    }
    // Leaf 4 (Intel; AMD returns a null type there), else 0x8000001D under AMD's topology extensions.
    if (w.max_leaf >= 4U)
    {
        cpuid(4U, 0U, r);
        w.cache_leaf = (r[0] & 0x1FU) != 0U ? 4U : 0U;
    }
    if (w.cache_leaf == 0U && w.max_extended_leaf >= 0x8000001DU && ((w.ext1_ecx >> 22U) & 1U) != 0U)
    {
        w.cache_leaf = 0x8000001DU;
    }
    for (crd::u32 i = 0U; w.cache_leaf != 0U && i < kCpuidCacheSubleaves; ++i)
    {
        cpuid(w.cache_leaf, i, r);
        if ((r[0] & 0x1FU) == 0U)
        {
            break;
        }
        w.cache[i] = CpuidCacheWords{r[0], r[1], r[2]};
    }
    if (w.max_extended_leaf >= 0x80000005U)
    {
        cpuid(0x80000005U, 0U, r);
        w.ext5_ecx = r[2];
    }
    if (w.max_extended_leaf >= 0x80000006U)
    {
        cpuid(0x80000006U, 0U, r);
        w.ext6_ecx = r[2];
        w.ext6_edx = r[3];
    }
    if (((w.leaf1_ecx >> 27U) & 1U) != 0U)
    {
        w.xcr0 = read_xcr0();
    }
#endif
    return w;
}

CpuLevel cpu_level_of(CpuArch arch, const CpuReport& report) noexcept
{
    if (arch == CpuArch::Arm64)
    {
        return CpuLevel::Neon;
    }
    if (arch != CpuArch::X64)
    {
        return CpuLevel::Portable;
    }
    if (!report.usable.has_all(kV2))
    {
        return CpuLevel::Sse2;
    }
    if (!report.usable.has_all(kV3))
    {
        return CpuLevel::Sse42;
    }
    const bool avx10_512 = report.avx10_version >= 1U && report.avx10_vector_bits >= 512U;
    if (!avx10_512 && !report.usable.has_all(kV4))
    {
        return CpuLevel::Avx2;
    }
    return CpuLevel::Avx512;
}

CpuFeatureSet cpu_level_features(CpuLevel level) noexcept
{
    switch (level)
    {
        case CpuLevel::Sse2:
            return kV1;
        case CpuLevel::Sse42:
            return CpuFeatureSet{kV1.bits | kV2.bits};
        case CpuLevel::Avx2:
            return CpuFeatureSet{kV1.bits | kV2.bits | kV3.bits};
        case CpuLevel::Avx512:
            return CpuFeatureSet{kV1.bits | kV2.bits | kV3.bits | kV4.bits};
        case CpuLevel::Portable:
        case CpuLevel::Neon:
            break;
    }
    return {};
}

void add_cpu_report(CpuSelection& s, const CpuReport& r) noexcept
{
    if (r.core_type == 0x20U)
    {
        s.core_types_seen |= kCpuCoreEfficiency;
    }
    else if (r.core_type == 0x40U)
    {
        s.core_types_seen |= kCpuCorePerformance;
    }
    else if (r.core_type != 0U)
    {
        s.core_types_seen |= kCpuCoreOther;
    }
    CpuReport& acc = s.features;
    if (s.cpus_visited == 0U)
    {
        acc = r;
        acc.core_type = 0U;
        s.cpus_visited = 1U;
        return;
    }
    if (r.usable != acc.usable || r.avx10_version != acc.avx10_version || r.avx10_vector_bits != acc.avx10_vector_bits)
    {
        s.uniform = false;
    }
    acc.reported.bits &= r.reported.bits;
    acc.usable.bits &= r.usable.bits;
    acc.xcr0 &= r.xcr0;
    acc.avx10_version = r.avx10_version < acc.avx10_version ? r.avx10_version : acc.avx10_version;
    acc.avx10_vector_bits = r.avx10_vector_bits < acc.avx10_vector_bits ? r.avx10_vector_bits : acc.avx10_vector_bits;
    const auto smaller = [](crd::u32 a, crd::u32 b) noexcept
    {
        return a < b ? a : b;
    };
    CpuCaches& c = acc.caches;
    c.line_bytes = smaller(c.line_bytes, r.caches.line_bytes);
    c.l1d_bytes = smaller(c.l1d_bytes, r.caches.l1d_bytes);
    c.l2_bytes = smaller(c.l2_bytes, r.caches.l2_bytes);
    c.l3_bytes = smaller(c.l3_bytes, r.caches.l3_bytes);
    c.source = r.caches.source == c.source ? c.source : CpuCacheSource::None;
    if (c.source == CpuCacheSource::None)
    {
        c = CpuCaches{};
    }
    ++s.cpus_visited;
}

CpuSelection detect_cpus() noexcept
{
    CpuSelection s;
    s.arch = kArch;
    if (kArch == CpuArch::X64)
    {
        if (!walk_every_cpu(s) || s.cpus_visited == 0U)
        {
            s.walk_note = "the thread's affinity could not be set; only the current CPU was read";
            s.cpus_visited = 0U;
            visit_here(s);
        }
    }
    else
    {
        s.walk_note = "no CPUID on this architecture; its level is architectural";
    }
    read_os_l2(s);
    s.detected = cpu_level_of(s.arch, s.features);
    s.selected = s.detected;
    s.kernel_features = s.features.usable;
    return s;
}

CpuSelection apply_cpu_override(const CpuSelection& detected, const char* override_text) noexcept
{
    CpuSelection s = detected;
    s.selected = detected.detected;
    s.override_status = CpuOverride::None;
    s.kernel_features = detected.features.usable;
    if (override_text == nullptr || override_text[0] == '\0')
    {
        return s;
    }
    CpuLevel want = CpuLevel::Portable;
    if (!cpu_level_from_name(override_text, want))
    {
        s.override_status = CpuOverride::Unknown;
    }
    else if (want != CpuLevel::Portable && arch_of(want) != detected.arch) // every architecture runs portable
    {
        s.override_status = CpuOverride::OtherArch;
    }
    else if (!cpu_level_includes(detected.detected, want))
    {
        s.override_status = CpuOverride::AboveHardware;
    }
    else
    {
        s.override_status = CpuOverride::Applied;
        s.selected = want;
        s.kernel_features.bits &= cpu_level_features(want).bits;
    }
    return s;
}

const CpuSelection& cpu_selection() noexcept
{
    static const CpuSelection kSelection = []() noexcept
    {
        char buffer[32] = {}; // NOLINT(modernize-avoid-c-arrays)
        const CpuSelection s = apply_cpu_override(detect_cpus(), read_override(buffer));
#if defined(_MSC_VER) && CRD_ARCH_X64
        if (s.override_status == CpuOverride::Applied)
        {
            lower_crt_dispatch(s.selected);
        }
#endif
        return s;
    }();
    return kSelection;
}

CpuRuntimeDispatch cpu_runtime_dispatch() noexcept
{
    CpuRuntimeDispatch d;
#if defined(_MSC_VER) && CRD_ARCH_X64
    d.crt_level = __isa_available;
    d.ucrt_fma3 = _get_FMA3_enable() != 0;
#endif
    return d;
}

#if defined(_MSC_VER) && CRD_ARCH_X64
bool detail::ucrt_fma3_active = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): see the header

namespace
{

// Selects at startup when CRD_CPU_LEVEL is set, so an applied level reaches the CRT's dispatch with the library
// initializers, before any user initializer runs; without it the selection stays lazy. Then records whether the
// UCRT's fma runs FMA3, which nothing changes later.
void __cdecl select_at_start() noexcept
{
    char buffer[32] = {}; // NOLINT(modernize-avoid-c-arrays)
    if (read_override(buffer) != nullptr)
    {
        (void)cpu_selection();
    }
    detail::ucrt_fma3_active = _get_FMA3_enable() != 0;
}

} // namespace

// A pointer in the CRT's library initializer section: .CRT$XCL sorts after the C initializers (__isa_available is
// set) and before the user ones (.CRT$XCU). crd-core's interface links /INCLUDE:kCrdCpuSelectAtStart, so every image
// keeps it, also one that never asks for the selection; extern "C" keeps the x64 name literal.
#pragma section(".CRT$XCL", read)
extern "C" __declspec(allocate(".CRT$XCL")) void(__cdecl* const kCrdCpuSelectAtStart)() = &select_at_start;
#endif

namespace
{

// A bounded line that counts its full length (format_cpu_selection).
struct LineWriter
{
    char* out;
    crd::usize capacity;
    crd::usize length = 0U;

    void put(char c) noexcept
    {
        if (length + 1U < capacity)
        {
            out[length] = c;
        }
        ++length;
    }

    void put(const char* text) noexcept
    {
        for (; *text != '\0'; ++text)
        {
            put(*text);
        }
    }

    void put(crd::u32 value) noexcept
    {
        char digits[10] = {}; // NOLINT(modernize-avoid-c-arrays)
        crd::usize n = 0U;
        do
        {
            digits[n] = static_cast<char>('0' + (value % 10U));
            ++n;
            value /= 10U;
        } while (value != 0U);
        while (n > 0U)
        {
            --n;
            put(digits[n]);
        }
    }
};

} // namespace

crd::usize format_cpu_selection(const CpuSelection& s, const CpuRuntimeDispatch& crt, char* out,
                                crd::usize capacity) noexcept
{
    LineWriter w{out, out == nullptr ? 0U : capacity};
    w.put("cpu detected=");
    w.put(cpu_level_name(s.detected));
    w.put(" selected=");
    w.put(cpu_level_name(s.selected));
    w.put(" override=");
    w.put(cpu_override_name(s.override_status));
    if (crt.crt_level >= 0)
    {
        w.put(" crt=");
        w.put(static_cast<crd::u32>(crt.crt_level));
        w.put(crt.ucrt_fma3 ? " fma3=1" : " fma3=0");
    }
    const CpuFeatureSet extra{s.kernel_features.bits & ~cpu_level_features(s.selected).bits};
    const char* separator = " extra=";
    for (crd::u32 i = 0U; i < static_cast<crd::u32>(CpuFeature::Count); ++i)
    {
        const auto f = static_cast<CpuFeature>(i);
        if (extra.has(f))
        {
            w.put(separator);
            w.put(cpu_feature_name(f));
            separator = ",";
        }
    }
    if (w.capacity > 0U)
    {
        out[w.length < w.capacity ? w.length : w.capacity - 1U] = '\0';
    }
    return w.length;
}

bool cpu_level_includes(CpuLevel have, CpuLevel want) noexcept
{
    if (want == CpuLevel::Portable)
    {
        return true;
    }
    return arch_of(have) == arch_of(want) && static_cast<crd::u8>(have) >= static_cast<crd::u8>(want);
}

const char* cpu_level_name(CpuLevel level) noexcept
{
    switch (level)
    {
        case CpuLevel::Portable:
            return "portable";
        case CpuLevel::Sse2:
            return "sse2";
        case CpuLevel::Sse42:
            return "sse4.2";
        case CpuLevel::Avx2:
            return "avx2";
        case CpuLevel::Avx512:
            return "avx512";
        case CpuLevel::Neon:
            return "neon";
    }
    return "?";
}

bool cpu_level_from_name(const char* name, CpuLevel& out) noexcept
{
    if (name == nullptr)
    {
        return false;
    }
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    constexpr CpuLevel all[] = {CpuLevel::Portable, CpuLevel::Sse2,   CpuLevel::Sse42,
                                CpuLevel::Avx2,     CpuLevel::Avx512, CpuLevel::Neon};
    for (const CpuLevel level : all)
    {
        if (std::strcmp(name, cpu_level_name(level)) == 0)
        {
            out = level;
            return true;
        }
    }
    return false;
}

const char* cpu_feature_name(CpuFeature feature) noexcept
{
    const auto i = static_cast<crd::usize>(feature);
    return i < sizeof(kFeatures) / sizeof(kFeatures[0]) ? kFeatures[i].name : "?";
}

const char* cpu_override_name(CpuOverride status) noexcept
{
    switch (status)
    {
        case CpuOverride::None:
            return "none";
        case CpuOverride::Applied:
            return "applied";
        case CpuOverride::Unknown:
            return "unknown";
        case CpuOverride::OtherArch:
            return "other-arch";
        case CpuOverride::AboveHardware:
            return "above-hardware";
    }
    return "?";
}

} // namespace crd
