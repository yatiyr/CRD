// CPU instruction-set detection and the dispatch-level selection (crd/core/cpu_features.hpp, SIMD-DISPATCH).
//
// The decode is judged on CPUID words built here (each level's features, the OS-saved state gates, AVX10, APX and
// hybrid core types); the selection on this host is judged against the operating system's own view of the CPUs
// (Windows IsProcessorFeaturePresent, Linux /proc/cpuinfo of every processor), and the CRD_CPU_LEVEL override both as
// a pure function and in a child process that reads it at startup.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/diag/specimen_runner.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

namespace
{
namespace cont = crd::containers;
namespace cd = crd::diag;
using crd::CpuFeature;
using crd::CpuLevel;
using crd::CpuOverride;

constexpr crd::u32 bit(crd::u32 b) noexcept
{
    return 1U << b;
}

// A Haswell-class x86-64-v3 CPU under an OS that saves the x87, SSE and AVX state.
crd::CpuidWords v3_words()
{
    crd::CpuidWords w;
    w.max_leaf = 0x20U;
    w.max_extended_leaf = 0x80000008U;
    w.leaf1_ecx = bit(0U) | bit(9U) | bit(12U) | bit(13U) | bit(19U) | bit(20U) | bit(22U) | bit(23U) | bit(26U) |
                  bit(27U) | bit(28U) | bit(29U);
    w.leaf1_edx = bit(25U) | bit(26U);
    w.leaf7_ebx = bit(3U) | bit(5U) | bit(8U);
    w.ext1_ecx = bit(0U) | bit(5U);
    w.ext7_edx = bit(8U);
    w.xcr0 = 0x7U;
    return w;
}

// v3 plus AVX-512 F, DQ, CD, BW and VL; the OS state is left to the case.
crd::CpuidWords v4_words()
{
    crd::CpuidWords w = v3_words();
    w.leaf7_ebx |= bit(16U) | bit(17U) | bit(28U) | bit(30U) | bit(31U);
    return w;
}

CpuLevel level_of(const crd::CpuidWords& w)
{
    return crd::cpu_level_of(crd::CpuArch::X64, crd::decode_cpuid(w));
}

crd::CpuSelection detected_at(CpuLevel level)
{
    crd::CpuSelection s;
    s.arch = crd::CpuArch::X64;
    s.detected = level;
    s.selected = level;
    return s;
}

bool contains(const cd::Outcome& o, std::string_view text)
{
    return std::string_view{o.output.data(), o.output.size()}.find(text) != std::string_view::npos;
}

// Sets an environment variable for the children this process starts, and puts the old value back.
class ScopedEnv
{
public:
    ScopedEnv(const char* name, const char* value) : m_name(name)
    {
        char old[256] = {}; // NOLINT(modernize-avoid-c-arrays)
#if defined(_WIN32)
        const DWORD n = ::GetEnvironmentVariableA(name, old, sizeof(old));
        m_had = n > 0U && n < sizeof(old);
        (void)_putenv_s(name, value);
#else
        const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe): the test is single-threaded here
        m_had = v != nullptr && std::strlen(v) < sizeof(old);
        if (m_had)
        {
            std::memcpy(old, v, std::strlen(v) + 1U);
        }
        (void)::setenv(name, value, 1);
#endif
        if (m_had)
        {
            m_old = cont::String{old};
        }
    }
    ~ScopedEnv()
    {
#if defined(_WIN32)
        (void)_putenv_s(m_name, m_had ? m_old.c_str() : "");
#else
        if (m_had)
        {
            (void)::setenv(m_name, m_old.c_str(), 1);
        }
        else
        {
            (void)::unsetenv(m_name);
        }
#endif
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
    bool m_had = false;
    cont::String m_old;
};

cd::Outcome run_level_child(const char* level)
{
    const ScopedEnv env{"CRD_CPU_LEVEL", level};
    cont::Array<cont::String> args;
    args.push_back(cont::String{"[.cpu-level-child]"});
    cd::Expectation e;
    e.want = cd::Expectation::Want::CleanExit;
    e.timeout_ms = 30000U;
    const cont::String self = cd::self_executable();
    REQUIRE_FALSE(self.empty());
    return cd::run_specimen(self, args, e);
}

#if !defined(_WIN32)
// The flags every processor in /proc/cpuinfo lists, as a mask over the features the kernel names.
struct KernelView
{
    crd::u32 processors = 0U;
    crd::u64 all = 0U;   // features every processor lists
    crd::u64 named = 0U; // features the kernel has a flag name for
};

const char* kernel_flag(CpuFeature f)
{
    switch (f)
    {
        case CpuFeature::Sse2:
            return "sse2";
        case CpuFeature::Sse3:
            return "pni";
        case CpuFeature::Ssse3:
            return "ssse3";
        case CpuFeature::Sse41:
            return "sse4_1";
        case CpuFeature::Sse42:
            return "sse4_2";
        case CpuFeature::Popcnt:
            return "popcnt";
        case CpuFeature::Cx16:
            return "cx16";
        case CpuFeature::LahfSahf:
            return "lahf_lm";
        case CpuFeature::Movbe:
            return "movbe";
        case CpuFeature::Aes:
            return "aes";
        case CpuFeature::Pclmulqdq:
            return "pclmulqdq";
        case CpuFeature::Xsave:
            return "xsave";
        case CpuFeature::Avx:
            return "avx";
        case CpuFeature::Fma:
            return "fma";
        case CpuFeature::F16c:
            return "f16c";
        case CpuFeature::Avx2:
            return "avx2";
        case CpuFeature::Bmi1:
            return "bmi1";
        case CpuFeature::Bmi2:
            return "bmi2";
        case CpuFeature::Lzcnt:
            return "abm";
        case CpuFeature::Sha:
            return "sha_ni";
        case CpuFeature::Gfni:
            return "gfni";
        case CpuFeature::Vaes:
            return "vaes";
        case CpuFeature::Vpclmulqdq:
            return "vpclmulqdq";
        case CpuFeature::AvxVnni:
            return "avx_vnni";
        case CpuFeature::Avx512F:
            return "avx512f";
        case CpuFeature::Avx512Cd:
            return "avx512cd";
        case CpuFeature::Avx512Bw:
            return "avx512bw";
        case CpuFeature::Avx512Dq:
            return "avx512dq";
        case CpuFeature::Avx512Vl:
            return "avx512vl";
        case CpuFeature::Avx512Ifma:
            return "avx512ifma";
        case CpuFeature::Avx512Vbmi:
            return "avx512vbmi";
        case CpuFeature::Avx512Vbmi2:
            return "avx512_vbmi2";
        case CpuFeature::Avx512Vnni:
            return "avx512_vnni";
        case CpuFeature::Avx512Bitalg:
            return "avx512_bitalg";
        case CpuFeature::Avx512Vpopcntdq:
            return "avx512_vpopcntdq";
        case CpuFeature::Avx512Bf16:
            return "avx512_bf16";
        case CpuFeature::Avx512Fp16:
            return "avx512_fp16";
        case CpuFeature::AmxTile:
            return "amx_tile";
        case CpuFeature::AmxBf16:
            return "amx_bf16";
        case CpuFeature::AmxInt8:
            return "amx_int8";
        case CpuFeature::InvariantTsc:
            return "nonstop_tsc"; // set from CPUID 0x80000007 EDX bit 8 on Intel and AMD
        default:
            return nullptr; // OSXSAVE, AVX10, APX and hybrid have no flag name every kernel prints
    }
}

bool has_word(std::string_view line, std::string_view word)
{
    for (crd::usize at = line.find(word); at != std::string_view::npos; at = line.find(word, at + 1U))
    {
        const bool starts = at == 0U || line[at - 1U] == ' ' || line[at - 1U] == '\t';
        const crd::usize end = at + word.size();
        const bool ends = end == line.size() || line[end] == ' ' || line[end] == '\n';
        if (starts && ends)
        {
            return true;
        }
    }
    return false;
}

KernelView read_kernel_view()
{
    KernelView v;
    for (crd::u32 i = 0U; i < static_cast<crd::u32>(CpuFeature::Count); ++i)
    {
        if (kernel_flag(static_cast<CpuFeature>(i)) != nullptr)
        {
            v.named |= crd::u64{1} << i;
        }
    }
    v.all = v.named;
    std::FILE* f = std::fopen("/proc/cpuinfo", "r");
    REQUIRE(f != nullptr);
    char line[16384] = {}; // NOLINT(modernize-avoid-c-arrays): one flags line
    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        const std::string_view l{line};
        if (l.rfind("flags", 0U) != 0U)
        {
            continue;
        }
        ++v.processors;
        for (crd::u32 i = 0U; i < static_cast<crd::u32>(CpuFeature::Count); ++i)
        {
            const char* name = kernel_flag(static_cast<CpuFeature>(i));
            if (name != nullptr && !has_word(l.substr(l.find(':') + 1U), name))
            {
                v.all &= ~(crd::u64{1} << i);
            }
        }
    }
    (void)std::fclose(f);
    return v;
}
#endif
} // namespace

TEST_CASE("cpu features: each level needs its features and the OS-saved state they use", "[core][cpu]")
{
    const crd::CpuReport v3 = crd::decode_cpuid(v3_words());
    CHECK(v3.usable.has(CpuFeature::Avx2));
    CHECK(v3.usable.has(CpuFeature::Fma));
    CHECK(v3.usable.has(CpuFeature::InvariantTsc));
    CHECK_FALSE(v3.reported.has(CpuFeature::Avx512F));
    CHECK(v3.xcr0 == 0x7U);
    CHECK(level_of(v3_words()) == CpuLevel::Avx2);

    // Each x86-64-v3 feature is needed for the level: without it the CPU is v2.
    constexpr crd::u32 v3_leaf1_ecx_bits[] = {12U, 22U, 28U, 29U}; // NOLINT(modernize-avoid-c-arrays): FMA..F16C
    for (const crd::u32 b : v3_leaf1_ecx_bits)
    {
        crd::CpuidWords w = v3_words();
        w.leaf1_ecx &= ~bit(b);
        INFO("leaf 1 ECX bit " << b);
        CHECK(level_of(w) == CpuLevel::Sse42);
    }
    for (const crd::u32 b : {3U, 5U, 8U})
    {
        crd::CpuidWords w = v3_words();
        w.leaf7_ebx &= ~bit(b);
        INFO("leaf 7 EBX bit " << b);
        CHECK(level_of(w) == CpuLevel::Sse42);
    }
    {
        crd::CpuidWords w = v3_words();
        w.ext1_ecx &= ~bit(5U); // LZCNT
        CHECK(level_of(w) == CpuLevel::Sse42);
    }
    // Each x86-64-v2 feature is needed for that level.
    for (const crd::u32 b : {0U, 9U, 13U, 19U, 20U, 23U})
    {
        crd::CpuidWords w = v3_words();
        w.leaf1_ecx &= ~bit(b);
        INFO("leaf 1 ECX bit " << b);
        CHECK(level_of(w) == CpuLevel::Sse2);
    }
    {
        crd::CpuidWords w = v3_words();
        w.ext1_ecx &= ~bit(0U); // LAHF/SAHF
        CHECK(level_of(w) == CpuLevel::Sse2);
    }

    // The OS does not save the YMM state: AVX is reported and not usable.
    {
        crd::CpuidWords w = v3_words();
        w.xcr0 = 0x3U;
        const crd::CpuReport r = crd::decode_cpuid(w);
        CHECK(r.reported.has(CpuFeature::Avx2));
        CHECK_FALSE(r.usable.has(CpuFeature::Avx2));
        CHECK_FALSE(r.usable.has(CpuFeature::Avx));
        CHECK(r.usable.has(CpuFeature::Bmi2)); // a general-register feature needs no saved state
        CHECK(level_of(w) == CpuLevel::Sse42);
    }
    // Without OSXSAVE the XCR0 word is never trusted.
    {
        crd::CpuidWords w = v3_words();
        w.leaf1_ecx &= ~bit(27U);
        const crd::CpuReport r = crd::decode_cpuid(w);
        CHECK(r.xcr0 == 0U);
        CHECK_FALSE(r.usable.has(CpuFeature::Avx));
        CHECK(level_of(w) == CpuLevel::Sse42);
    }
    // A leaf above the reported maximum is not read.
    {
        crd::CpuidWords w = v3_words();
        w.max_leaf = 6U;
        CHECK_FALSE(crd::decode_cpuid(w).reported.has(CpuFeature::Avx2));
        w = v3_words();
        w.max_extended_leaf = 0x80000000U;
        CHECK_FALSE(crd::decode_cpuid(w).reported.has(CpuFeature::Lzcnt));
        CHECK_FALSE(crd::decode_cpuid(w).reported.has(CpuFeature::InvariantTsc));
    }
}

TEST_CASE("cpu features: AVX-512, AVX10 and APX count only with their register state saved", "[core][cpu]")
{
    crd::CpuidWords w = v4_words();
    CHECK(crd::decode_cpuid(w).reported.has(CpuFeature::Avx512Vl));
    CHECK(level_of(w) == CpuLevel::Avx2); // no opmask or ZMM state
    for (const crd::u64 missing : {0x20U, 0x40U, 0x80U})
    {
        w.xcr0 = 0xE7U & ~missing;
        INFO("XCR0 without " << missing);
        CHECK_FALSE(crd::decode_cpuid(w).usable.has(CpuFeature::Avx512F));
        CHECK(level_of(w) == CpuLevel::Avx2);
    }
    w.xcr0 = 0xE7U;
    CHECK(level_of(w) == CpuLevel::Avx512);
    for (const crd::u32 b : {16U, 17U, 28U, 30U, 31U})
    {
        crd::CpuidWords missing = w;
        missing.leaf7_ebx &= ~bit(b);
        INFO("leaf 7 EBX bit " << b);
        CHECK(level_of(missing) == CpuLevel::Avx2);
    }

    // AVX10.2 at 512 bits is the v4 level without the separate AVX-512 bits.
    crd::CpuidWords a = v3_words();
    a.max_leaf = 0x24U;
    a.leaf7_1_edx = bit(19U);
    a.leaf24_ebx = 2U | bit(16U) | bit(17U) | bit(18U);
    a.xcr0 = 0xE7U;
    crd::CpuReport r = crd::decode_cpuid(a);
    CHECK(r.usable.has(CpuFeature::Avx10));
    CHECK(r.avx10_version == 2U);
    CHECK(r.avx10_vector_bits == 512U);
    CHECK(level_of(a) == CpuLevel::Avx512);
    a.leaf24_ebx = 1U | bit(16U) | bit(17U); // AVX10.1 limited to 256 bits
    r = crd::decode_cpuid(a);
    CHECK(r.avx10_vector_bits == 256U);
    CHECK(level_of(a) == CpuLevel::Avx2);
    a.leaf24_ebx = 2U | bit(16U) | bit(17U) | bit(18U);
    a.xcr0 = 0x7U;
    r = crd::decode_cpuid(a);
    CHECK(r.reported.has(CpuFeature::Avx10));
    CHECK_FALSE(r.usable.has(CpuFeature::Avx10));
    CHECK(r.avx10_vector_bits == 0U);
    CHECK(level_of(a) == CpuLevel::Avx2);
    a.max_leaf = 0x23U; // no AVX10 leaf: no version
    CHECK(crd::decode_cpuid(a).avx10_version == 0U);

    // APX needs its own extended-GPR state component.
    crd::CpuidWords p = v4_words();
    p.leaf7_1_edx = bit(21U);
    p.xcr0 = 0xE7U;
    CHECK(crd::decode_cpuid(p).reported.has(CpuFeature::Apx));
    CHECK_FALSE(crd::decode_cpuid(p).usable.has(CpuFeature::Apx));
    p.xcr0 |= 0x80000U;
    CHECK(crd::decode_cpuid(p).usable.has(CpuFeature::Apx));

    // AMX needs the tile configuration and tile data state.
    crd::CpuidWords t = v4_words();
    t.leaf7_edx = bit(24U);
    t.xcr0 = 0xE7U | 0x20000U;
    CHECK_FALSE(crd::decode_cpuid(t).usable.has(CpuFeature::AmxTile));
    t.xcr0 |= 0x40000U;
    CHECK(crd::decode_cpuid(t).usable.has(CpuFeature::AmxTile));
}

TEST_CASE("cpu features: a hybrid CPU reports its core type, and features and names are one table", "[core][cpu]")
{
    crd::CpuidWords w = v3_words();
    w.leaf1a_eax = 0x40000001U;
    CHECK(crd::decode_cpuid(w).core_type == 0U); // not hybrid: leaf 0x1A is not read
    w.leaf7_edx = bit(15U);
    CHECK(crd::decode_cpuid(w).core_type == 0x40U);
    w.max_leaf = 0x19U;
    CHECK(crd::decode_cpuid(w).core_type == 0U);

    for (crd::u32 i = 0U; i < static_cast<crd::u32>(CpuFeature::Count); ++i)
    {
        const char* name = crd::cpu_feature_name(static_cast<CpuFeature>(i));
        INFO("feature " << i);
        CHECK(std::strcmp(name, "?") != 0);
        for (crd::u32 j = 0U; j < i; ++j)
        {
            CHECK(std::strcmp(name, crd::cpu_feature_name(static_cast<CpuFeature>(j))) != 0);
        }
    }
    for (const CpuLevel level :
         {CpuLevel::Portable, CpuLevel::Sse2, CpuLevel::Sse42, CpuLevel::Avx2, CpuLevel::Avx512, CpuLevel::Neon})
    {
        CpuLevel back = CpuLevel::Portable;
        REQUIRE(crd::cpu_level_from_name(crd::cpu_level_name(level), back));
        CHECK(back == level);
    }
}

TEST_CASE("cpu features: the selection keeps only what every visited CPU executes", "[core][cpu]")
{
    // A performance core with AVX-512 and AVX10.2 at 512 bits, an efficiency core with neither.
    crd::CpuidWords p = v4_words();
    p.max_leaf = 0x24U;
    p.leaf7_edx = bit(15U);
    p.leaf7_1_edx = bit(19U);
    p.leaf24_ebx = 2U | bit(16U) | bit(17U) | bit(18U);
    p.leaf1a_eax = 0x40000000U;
    p.xcr0 = 0xE7U;
    crd::CpuidWords e = v3_words();
    e.max_leaf = 0x24U;
    e.leaf7_edx = bit(15U);
    e.leaf1a_eax = 0x20000000U;
    e.xcr0 = 0xE7U;

    crd::CpuSelection s;
    s.arch = crd::CpuArch::X64;
    crd::add_cpu_report(s, crd::decode_cpuid(p));
    CHECK(s.uniform);
    CHECK(crd::cpu_level_of(s.arch, s.features) == CpuLevel::Avx512);
    crd::add_cpu_report(s, crd::decode_cpuid(p));
    CHECK(s.uniform);
    crd::add_cpu_report(s, crd::decode_cpuid(e));
    crd::add_cpu_report(s, crd::decode_cpuid(p));
    CHECK(s.cpus_visited == 4U);
    CHECK_FALSE(s.uniform);
    CHECK(s.core_types_seen == (crd::kCpuCoreEfficiency | crd::kCpuCorePerformance));
    CHECK(s.features.core_type == 0U);
    CHECK_FALSE(s.features.usable.has(CpuFeature::Avx512F));
    CHECK_FALSE(s.features.reported.has(CpuFeature::Avx10));
    CHECK(s.features.avx10_version == 0U);
    CHECK(s.features.avx10_vector_bits == 0U);
    CHECK(s.features.usable.has(CpuFeature::Avx2));
    CHECK(crd::cpu_level_of(s.arch, s.features) == CpuLevel::Avx2);
}

TEST_CASE("cpu features: CRD_CPU_LEVEL lowers the level and refuses one the CPUs cannot run", "[core][cpu]")
{
    const crd::CpuSelection avx2 = detected_at(CpuLevel::Avx2);
    CHECK(crd::apply_cpu_override(avx2, nullptr).override_status == CpuOverride::None);
    CHECK(crd::apply_cpu_override(avx2, "").selected == CpuLevel::Avx2);
    for (const CpuLevel lower : {CpuLevel::Portable, CpuLevel::Sse2, CpuLevel::Sse42, CpuLevel::Avx2})
    {
        const crd::CpuSelection s = crd::apply_cpu_override(avx2, crd::cpu_level_name(lower));
        CHECK(s.override_status == CpuOverride::Applied);
        CHECK(s.selected == lower);
        CHECK(s.detected == CpuLevel::Avx2);
    }
    const crd::CpuSelection above = crd::apply_cpu_override(avx2, "avx512");
    CHECK(above.override_status == CpuOverride::AboveHardware);
    CHECK(above.selected == CpuLevel::Avx2);
    const crd::CpuSelection other = crd::apply_cpu_override(avx2, "neon");
    CHECK(other.override_status == CpuOverride::OtherArch);
    CHECK(other.selected == CpuLevel::Avx2);
    for (const char* unknown : {"AVX2", "avx", "fast", "?"})
    {
        INFO(unknown);
        CHECK(crd::apply_cpu_override(avx2, unknown).override_status == CpuOverride::Unknown);
        CHECK(crd::apply_cpu_override(avx2, unknown).selected == CpuLevel::Avx2);
    }
    // An earlier override never carries into a later one.
    const crd::CpuSelection lowered = crd::apply_cpu_override(avx2, "sse2");
    CHECK(crd::apply_cpu_override(lowered, nullptr).selected == CpuLevel::Avx2);
    CHECK(crd::apply_cpu_override(lowered, nullptr).kernel_features == avx2.features.usable);

    CHECK(crd::cpu_level_includes(CpuLevel::Avx512, CpuLevel::Sse2));
    CHECK(crd::cpu_level_includes(CpuLevel::Neon, CpuLevel::Portable));
    CHECK_FALSE(crd::cpu_level_includes(CpuLevel::Sse42, CpuLevel::Avx2));
    CHECK_FALSE(crd::cpu_level_includes(CpuLevel::Avx512, CpuLevel::Neon));
    CHECK_FALSE(crd::cpu_level_includes(CpuLevel::Neon, CpuLevel::Sse2));
    CHECK_FALSE(crd::cpu_level_includes(CpuLevel::Portable, CpuLevel::Sse2));
}

TEST_CASE("cpu features: kernel variants select on every usable feature, a forced level only on its definition",
          "[core][cpu]")
{
    crd::CpuSelection hw = detected_at(CpuLevel::Avx512);
    for (const CpuFeature f : {CpuFeature::Sse2, CpuFeature::Sse42, CpuFeature::Avx2, CpuFeature::Fma,
                               CpuFeature::Avx512F, CpuFeature::Avx512Vl, CpuFeature::AvxVnni, CpuFeature::Avx512Vnni,
                               CpuFeature::Gfni, CpuFeature::InvariantTsc})
    {
        hw.features.usable.set(f);
    }
    CHECK(crd::apply_cpu_override(hw, nullptr).kernel_features == hw.features.usable);
    CHECK(crd::apply_cpu_override(hw, "avx3").kernel_features == hw.features.usable); // unknown: nothing changes
    CHECK(crd::apply_cpu_override(hw, "neon").kernel_features == hw.features.usable); // other architecture
    const crd::CpuFeatureSet at_avx512 = crd::apply_cpu_override(hw, "avx512").kernel_features;
    CHECK(at_avx512.has_all(crd::CpuFeatureSet{}.with(CpuFeature::Avx512F).with(CpuFeature::Avx512Vl)));
    CHECK_FALSE(at_avx512.has(CpuFeature::Avx512Vnni));
    CHECK_FALSE(at_avx512.has(CpuFeature::Gfni));
    const crd::CpuFeatureSet at_avx2 = crd::apply_cpu_override(hw, "avx2").kernel_features;
    CHECK(at_avx2.has(CpuFeature::Avx2));
    CHECK(at_avx2.has(CpuFeature::Fma));
    CHECK_FALSE(at_avx2.has(CpuFeature::AvxVnni)); // the smallest avx2 CPU has no AVX-VNNI
    CHECK_FALSE(at_avx2.has(CpuFeature::Avx512F));
    CHECK_FALSE(at_avx2.has(CpuFeature::InvariantTsc));
    const crd::CpuFeatureSet at_sse2 = crd::apply_cpu_override(hw, "sse2").kernel_features;
    CHECK(at_sse2 == crd::CpuFeatureSet{}.with(CpuFeature::Sse2));
    CHECK(crd::apply_cpu_override(hw, "portable").kernel_features == crd::CpuFeatureSet{});

    // Each level's definition holds the one below it and adds its own (x86-64 psABI v1 to v4).
    CHECK(crd::cpu_level_features(CpuLevel::Portable) == crd::CpuFeatureSet{});
    CHECK(crd::cpu_level_features(CpuLevel::Neon) == crd::CpuFeatureSet{});
    const CpuLevel order[] = {CpuLevel::Sse2, CpuLevel::Sse42, CpuLevel::Avx2, CpuLevel::Avx512};
    const CpuFeature added[] = {CpuFeature::Sse2, CpuFeature::Popcnt, CpuFeature::Movbe, CpuFeature::Avx512Dq};
    for (crd::usize i = 0; i < 4U; ++i)
    {
        INFO(crd::cpu_level_name(order[i]));
        CHECK(crd::cpu_level_features(order[i]).has(added[i]));
        if (i > 0U)
        {
            CHECK(crd::cpu_level_features(order[i]).has_all(crd::cpu_level_features(order[i - 1U])));
            CHECK_FALSE(crd::cpu_level_features(order[i - 1U]).has(added[i]));
        }
    }

    // The process: no override uses every usable feature, an applied one only the forced level's.
    const crd::CpuSelection& s = crd::cpu_selection();
    if (s.override_status == CpuOverride::Applied)
    {
        CHECK(s.kernel_features.bits == (s.features.usable.bits & crd::cpu_level_features(s.selected).bits));
    }
    else
    {
        CHECK(s.kernel_features == s.features.usable);
    }
}

TEST_CASE("cpu features: the selection is what every CPU of the process executes, as the OS reports it", "[core][cpu]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    INFO("detected " << crd::cpu_level_name(s.detected) << ", selected " << crd::cpu_level_name(s.selected) << ", "
                     << s.cpus_visited << " CPUs visited, " << s.cpus_refused << " refused, note '" << s.walk_note
                     << "'");
    CHECK(s.detected == crd::cpu_level_of(s.arch, s.features));
    if (s.arch != crd::CpuArch::X64)
    {
        CHECK(s.cpus_visited == 0U);
        return;
    }
    CHECK(std::strlen(s.walk_note) == 0U);
    CHECK(s.uniform);
    CHECK(s.features.usable.has(CpuFeature::Sse2));
    if (s.features.reported.has(CpuFeature::Hybrid))
    {
        CHECK(s.core_types_seen != 0U); // every CPU of a hybrid part names its core type
    }

#if defined(_WIN32)
    CHECK(s.cpus_visited + s.cpus_refused == ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    if (::GetActiveProcessorGroupCount() == 1U)
    {
        DWORD_PTR process = 0U;
        DWORD_PTR system = 0U;
        REQUIRE(::GetProcessAffinityMask(::GetCurrentProcess(), &process, &system) != 0);
        crd::u32 allowed = 0U;
        for (DWORD_PTR m = process; m != 0U; m &= m - 1U)
        {
            ++allowed;
        }
        CHECK(s.cpus_visited == allowed);
    }
    struct Os
    {
        CpuFeature feature;
        DWORD pf;
    };
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    constexpr Os os[] = {
        {CpuFeature::Sse2, PF_XMMI64_INSTRUCTIONS_AVAILABLE},     {CpuFeature::Sse3, PF_SSE3_INSTRUCTIONS_AVAILABLE},
        {CpuFeature::Ssse3, PF_SSSE3_INSTRUCTIONS_AVAILABLE},     {CpuFeature::Sse41, PF_SSE4_1_INSTRUCTIONS_AVAILABLE},
        {CpuFeature::Sse42, PF_SSE4_2_INSTRUCTIONS_AVAILABLE},    {CpuFeature::Cx16, PF_COMPARE_EXCHANGE128},
        {CpuFeature::Avx, PF_AVX_INSTRUCTIONS_AVAILABLE},         {CpuFeature::Avx2, PF_AVX2_INSTRUCTIONS_AVAILABLE},
        {CpuFeature::Avx512F, PF_AVX512F_INSTRUCTIONS_AVAILABLE},
    };
    for (const Os& o : os)
    {
        INFO(crd::cpu_feature_name(o.feature));
        CHECK(s.features.usable.has(o.feature) == (::IsProcessorFeaturePresent(o.pf) != 0));
    }
#else
    const long configured = ::sysconf(_SC_NPROCESSORS_CONF);
    CHECK(s.cpus_visited + s.cpus_refused == static_cast<crd::u32>(configured));
    cpu_set_t mine[8] = {}; // NOLINT(modernize-avoid-c-arrays)
    REQUIRE(::sched_getaffinity(0, sizeof(mine), mine) == 0);
    CHECK(s.cpus_visited >= static_cast<crd::u32>(CPU_COUNT_S(sizeof(mine), mine)));

    const KernelView kernel = read_kernel_view();
    REQUIRE(kernel.processors >= s.cpus_visited);
    for (crd::u32 i = 0U; i < static_cast<crd::u32>(CpuFeature::Count); ++i)
    {
        if (((kernel.named >> i) & 1U) == 0U)
        {
            continue;
        }
        INFO(crd::cpu_feature_name(static_cast<CpuFeature>(i)));
        CHECK(s.features.usable.has(static_cast<CpuFeature>(i)) == (((kernel.all >> i) & 1U) != 0U));
    }
#endif
}

TEST_CASE("cpu features: the walk gives the calling thread its own affinity back", "[core][cpu]")
{
    // The process's own walk (cpu_selection) ran on this thread: its affinity is still the process's. Then a thread
    // narrowed to one CPU keeps exactly that CPU through a second walk.
    const crd::CpuSelection& first = crd::cpu_selection();
#if defined(_WIN32)
    const HANDLE thread = ::GetCurrentThread();
    GROUP_AFFINITY entry = {};
    REQUIRE(::GetThreadGroupAffinity(thread, &entry) != 0);
    if (::GetActiveProcessorGroupCount() == 1U)
    {
        DWORD_PTR process = 0U;
        DWORD_PTR system = 0U;
        REQUIRE(::GetProcessAffinityMask(::GetCurrentProcess(), &process, &system) != 0);
        CHECK(entry.Mask == static_cast<KAFFINITY>(process));
    }
    GROUP_AFFINITY narrow = entry;
    narrow.Mask = entry.Mask & (~entry.Mask + 1U); // its lowest CPU
    REQUIRE(::SetThreadGroupAffinity(thread, &narrow, nullptr) != 0);
    const crd::CpuSelection s = crd::detect_cpus();
    GROUP_AFFINITY after = {};
    REQUIRE(::GetThreadGroupAffinity(thread, &after) != 0);
    REQUIRE(::SetThreadGroupAffinity(thread, &entry, nullptr) != 0);
    CHECK(after.Group == narrow.Group);
    CHECK(after.Mask == narrow.Mask);
#else
    cpu_set_t entry[8] = {}; // NOLINT(modernize-avoid-c-arrays)
    REQUIRE(::sched_getaffinity(0, sizeof(entry), entry) == 0);
    cpu_set_t narrow[8] = {}; // NOLINT(modernize-avoid-c-arrays)
    for (crd::usize cpu = 0U; cpu < sizeof(entry) * 8U; ++cpu)
    {
        if (CPU_ISSET_S(cpu, sizeof(entry), entry))
        {
            CPU_SET_S(cpu, sizeof(narrow), narrow);
            break;
        }
    }
    REQUIRE(::sched_setaffinity(0, sizeof(narrow), narrow) == 0);
    const crd::CpuSelection s = crd::detect_cpus();
    cpu_set_t after[8] = {}; // NOLINT(modernize-avoid-c-arrays)
    REQUIRE(::sched_getaffinity(0, sizeof(after), after) == 0);
    REQUIRE(::sched_setaffinity(0, sizeof(entry), entry) == 0);
    CHECK(CPU_EQUAL_S(sizeof(narrow), narrow, after));
#endif
    CHECK(s.cpus_visited == first.cpus_visited); // a narrowed thread still walks every CPU of the process
    CHECK(s.features == first.features);
}

namespace
{
// One deterministic cache subleaf: type (1 data, 2 instruction, 3 unified), level, sharers, ways, line bytes, sets.
crd::CpuidCacheWords cache_words(crd::u32 type, crd::u32 level, crd::u32 sharers, crd::u32 ways, crd::u32 line,
                                 crd::u32 sets)
{
    return {type | (level << 5U) | ((sharers - 1U) << 14U), (line - 1U) | ((ways - 1U) << 22U), sets - 1U};
}

// A performance core's leaf 4: 48 KiB L1 data, an instruction cache, 2 MiB L2 for two threads, 36 MiB L3 for 32.
crd::CpuidWords performance_core_words()
{
    crd::CpuidWords w = v3_words();
    w.cache_leaf = 4U;
    w.cache[0] = cache_words(1U, 1U, 2U, 12U, 64U, 64U);
    w.cache[1] = cache_words(2U, 1U, 2U, 8U, 64U, 64U);
    w.cache[2] = cache_words(3U, 2U, 2U, 16U, 64U, 2048U);
    w.cache[3] = cache_words(3U, 3U, 32U, 12U, 64U, 49152U);
    w.cache[5] = cache_words(3U, 4U, 1U, 16U, 64U, 65536U); // past the null subleaf: never read
    return w;
}
} // namespace

TEST_CASE("cpu features: the cache leaves give each data cache's size and line", "[core][cpu]")
{
    CHECK(crd::decode_cpuid(v3_words()).caches == crd::CpuCaches{}); // no leaf describes them

    crd::CpuidWords w = performance_core_words();
    const crd::CpuCaches p = crd::decode_cpuid(w).caches;
    CHECK(p.source == crd::CpuCacheSource::Deterministic);
    CHECK(p.line_bytes == 64U);
    CHECK(p.l1d_bytes == 48U * 1024U);
    CHECK(p.l2_bytes == 2U * 1024U * 1024U);
    CHECK(p.l3_bytes == 36U * 1024U * 1024U);
    w.cache_leaf = 0x8000001DU; // AMD's leaf has the same layout
    CHECK(crd::decode_cpuid(w).caches == p);

    crd::CpuidWords legacy = v3_words();
    legacy.ext5_ecx = (32U << 24U) | 64U;
    legacy.ext6_ecx = (512U << 16U) | 64U;
    legacy.ext6_edx = 16U << 18U;
    const crd::CpuCaches l = crd::decode_cpuid(legacy).caches;
    CHECK(l.source == crd::CpuCacheSource::Legacy);
    CHECK(l.line_bytes == 64U);
    CHECK(l.l1d_bytes == 32U * 1024U);
    CHECK(l.l2_bytes == 512U * 1024U);
    CHECK(l.l3_bytes == 8U * 1024U * 1024U);
    legacy.max_extended_leaf = 0x80000005U;
    CHECK(crd::decode_cpuid(legacy).caches == crd::CpuCaches{});
}

TEST_CASE("cpu features: the selection keeps each cache's smallest size over the CPUs", "[core][cpu]")
{
    // An efficiency core: 32 KiB L1 data, 4 MiB L2 for a cluster of four, the same L3.
    crd::CpuidWords e = performance_core_words();
    e.cache[0] = cache_words(1U, 1U, 1U, 8U, 64U, 64U);
    e.cache[2] = cache_words(3U, 2U, 4U, 16U, 64U, 4096U);
    crd::CpuSelection s;
    s.arch = crd::CpuArch::X64;
    crd::add_cpu_report(s, crd::decode_cpuid(performance_core_words()));
    crd::add_cpu_report(s, crd::decode_cpuid(e));
    crd::add_cpu_report(s, crd::decode_cpuid(performance_core_words()));
    CHECK(s.uniform); // the features agree
    const crd::CpuCaches& c = s.features.caches;
    CHECK(c.source == crd::CpuCacheSource::Deterministic);
    CHECK(c.l1d_bytes == 32U * 1024U);
    CHECK(c.l2_bytes == 2U * 1024U * 1024U);
    CHECK(c.l3_bytes == 36U * 1024U * 1024U);
    crd::add_cpu_report(s, crd::decode_cpuid(v3_words())); // a CPU that describes none
    CHECK(s.features.caches == crd::CpuCaches{});
}

namespace
{
// The smallest data or unified cache of levels 1 to 3 the OS reports for any CPU (0: none reported), and L1's line.
struct OsCaches
{
    crd::u32 bytes[4] = {}; // NOLINT(modernize-avoid-c-arrays): by level
    crd::u32 line = 0U;
    void add(crd::u32 level, crd::u32 size, crd::u32 line_bytes)
    {
        if (level < 1U || level > 3U)
        {
            return;
        }
        bytes[level] = bytes[level] == 0U || size < bytes[level] ? size : bytes[level];
        if (level == 1U)
        {
            line = line == 0U || line_bytes < line ? line_bytes : line;
        }
    }
};

#if !defined(_WIN32)
// The first line of one sysfs cache attribute of a CPU; false when the file is absent.
bool read_cache_attribute(long cpu, int index, const char* name, char (&out)[32]) // NOLINT(modernize-avoid-c-arrays)
{
    char path[128] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cache/index%d/%s", cpu, index, name);
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr)
    {
        return false;
    }
    const bool ok = std::fgets(out, sizeof(out), f) != nullptr;
    (void)std::fclose(f);
    return ok;
}
#endif

OsCaches read_os_caches()
{
    OsCaches os;
#if defined(_WIN32)
    DWORD len = 0U;
    (void)::GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    cont::Array<unsigned char> buf;
    buf.resize(len);
    auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
    REQUIRE(::GetLogicalProcessorInformationEx(RelationCache, first, &len) != 0);
    for (DWORD off = 0U; off < len;)
    {
        const auto* p = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (p->Relationship == RelationCache && (p->Cache.Type == CacheData || p->Cache.Type == CacheUnified))
        {
            os.add(p->Cache.Level, p->Cache.CacheSize, p->Cache.LineSize);
        }
        off += p->Size;
    }
#else
    const long configured = ::sysconf(_SC_NPROCESSORS_CONF);
    for (long cpu = 0; cpu < configured; ++cpu)
    {
        for (int index = 0; index < 10; ++index)
        {
            char level[32] = {}; // NOLINT(modernize-avoid-c-arrays)
            char type[32] = {};  // NOLINT(modernize-avoid-c-arrays)
            char size[32] = {};  // NOLINT(modernize-avoid-c-arrays)
            char line[32] = {};  // NOLINT(modernize-avoid-c-arrays)
            if (!read_cache_attribute(cpu, index, "level", level) || !read_cache_attribute(cpu, index, "type", type) ||
                !read_cache_attribute(cpu, index, "size", size) ||
                !read_cache_attribute(cpu, index, "coherency_line_size", line))
            {
                break;
            }
            if (std::strncmp(type, "Instruction", 11U) == 0)
            {
                continue;
            }
            char* unit = nullptr;
            unsigned long bytes = std::strtoul(size, &unit, 10);
            bytes *= *unit == 'K' ? 1024UL : (*unit == 'M' ? 1024UL * 1024UL : 1UL);
            os.add(static_cast<crd::u32>(std::strtoul(level, nullptr, 10)), static_cast<crd::u32>(bytes),
                   static_cast<crd::u32>(std::strtoul(line, nullptr, 10)));
        }
    }
#endif
    return os;
}
} // namespace

TEST_CASE("cpu features: the caches are the smallest of each level the OS reports", "[core][cpu]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const crd::CpuCaches& c = s.features.caches;
    if (s.arch != crd::CpuArch::X64)
    {
        CHECK(c == crd::CpuCaches{}); // no CPUID
        return;
    }
    INFO("line " << c.line_bytes << ", L1d " << c.l1d_bytes << ", L2 " << c.l2_bytes << ", L3 " << c.l3_bytes);
    REQUIRE(c.source != crd::CpuCacheSource::None);
    CHECK(c.line_bytes >= 32U);
    CHECK(c.line_bytes <= 256U);
    CHECK((c.line_bytes & (c.line_bytes - 1U)) == 0U);
    CHECK((c.source == crd::CpuCacheSource::Legacy || c.l1d_bytes > 0U)); // Intel reserves leaf 0x80000005's L1d
    CHECK(c.l2_bytes > c.l1d_bytes);
    CHECK((c.l3_bytes == 0U || c.l3_bytes > c.l2_bytes));

    const OsCaches os = read_os_caches();
    const crd::u32 mine[4] = {0U, c.l1d_bytes, c.l2_bytes, c.l3_bytes}; // NOLINT(modernize-avoid-c-arrays)
    for (crd::u32 level = 1U; level <= 3U; ++level)
    {
        INFO("L" << level << ": OS " << os.bytes[level]);
        // 0: the OS hides it (some sandboxes), or the legacy leaves do not describe it.
        const bool unread = c.source == crd::CpuCacheSource::Legacy && mine[level] == 0U;
        CHECK((os.bytes[level] == 0U || unread || os.bytes[level] == mine[level]));
    }
    CHECK((os.line == 0U || os.line == c.line_bytes));
}

TEST_CASE("cpu features: the selection line names the levels, the override, the CRT's level and extra features",
          "[core][cpu]")
{
    crd::CpuSelection s;
    s.arch = crd::CpuArch::X64;
    s.detected = CpuLevel::Avx2;
    s.selected = CpuLevel::Sse2;
    s.override_status = CpuOverride::Applied;
    s.kernel_features = crd::cpu_level_features(CpuLevel::Sse2);
    char line[160] = {}; // NOLINT(modernize-avoid-c-arrays)
    const std::string_view applied = "cpu detected=avx2 selected=sse2 override=applied";
    CHECK(crd::format_cpu_selection(s, crd::CpuRuntimeDispatch{}, line, sizeof(line)) == applied.size());
    CHECK(std::string_view{line} == applied);

    // The CRT's own level where it has one, then the kernel features above the selected level's definition.
    s.selected = CpuLevel::Avx2;
    s.override_status = CpuOverride::None;
    s.kernel_features = crd::cpu_level_features(CpuLevel::Avx2).with(CpuFeature::AvxVnni).with(CpuFeature::Sha);
    const std::string_view full = "cpu detected=avx2 selected=avx2 override=none crt=5 fma3=1 extra=sha,avx-vnni";
    crd::CpuRuntimeDispatch crt;
    crt.crt_level = 5;
    crt.ucrt_fma3 = true;
    CHECK(crd::format_cpu_selection(s, crt, line, sizeof(line)) == full.size());
    CHECK(std::string_view{line} == full);

    // A short buffer keeps a terminated prefix and the result still counts the whole line; none writes nothing.
    char cut[12] = {}; // NOLINT(modernize-avoid-c-arrays)
    CHECK(crd::format_cpu_selection(s, crt, cut, sizeof(cut)) == full.size());
    CHECK(std::string_view{cut} == full.substr(0U, sizeof(cut) - 1U));
    CHECK(crd::format_cpu_selection(s, crt, nullptr, 0U) == full.size());

    // Refused levels keep the detected one and say why.
    s.override_status = CpuOverride::AboveHardware;
    s.kernel_features = crd::cpu_level_features(CpuLevel::Avx2);
    CHECK(crd::format_cpu_selection(s, crd::CpuRuntimeDispatch{}, line, sizeof(line)) > 0U);
    CHECK(std::string_view{line} == "cpu detected=avx2 selected=avx2 override=above-hardware");
}

TEST_CASE("cpu features child: print the selection", "[.cpu-level-child]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    char line[512] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)crd::format_cpu_selection(s, crd::cpu_runtime_dispatch(), line, sizeof(line));
    std::printf("%s\n", line);
    std::printf("cpu walk: %u visited %u refused, core types 0x%x, uniform %d, xcr0 0x%llx, avx10 %u/%u\n",
                s.cpus_visited, s.cpus_refused, static_cast<unsigned>(s.core_types_seen), s.uniform ? 1 : 0,
                static_cast<unsigned long long>(s.features.xcr0), static_cast<unsigned>(s.features.avx10_version),
                static_cast<unsigned>(s.features.avx10_vector_bits));
    const crd::CpuCaches& c = s.features.caches;
    std::printf("cpu caches: line %u, L1d %u, L2 %u, L3 %u, source %u\n", c.line_bytes, c.l1d_bytes, c.l2_bytes,
                c.l3_bytes, static_cast<unsigned>(c.source));
    for (crd::u32 i = 0U; i < static_cast<crd::u32>(CpuFeature::Count); ++i)
    {
        const auto f = static_cast<CpuFeature>(i);
        if (s.features.reported.has(f))
        {
            std::printf(" %s%s", crd::cpu_feature_name(f), s.features.usable.has(f) ? "" : "(not usable)");
        }
    }
    std::printf("\n");
    (void)std::fflush(stdout);
}

TEST_CASE("cpu features: a process started with CRD_CPU_LEVEL selects that level", "[core][cpu]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    if (s.arch != crd::CpuArch::X64)
    {
        return;
    }
    const cd::Outcome lowered = run_level_child("sse2");
    INFO(lowered.output.c_str());
    CHECK(lowered.verdict == cd::Verdict::Clean);
    CHECK(contains(lowered, "selected=sse2 override=applied"));
    // The harness names the selection the child made, not the parent's own.
    const std::string_view tuple{lowered.host_tuple.c_str()};
    CHECK(tuple.find("selected=sse2 override=applied") != std::string_view::npos);

    cont::String refused{"selected="};
    refused.append(crd::cpu_level_name(s.detected));
    refused.append(" override=other-arch");
    const cd::Outcome other = run_level_child("neon");
    INFO(other.output.c_str());
    CHECK(contains(other, refused.c_str()));

    if (s.detected != CpuLevel::Avx512)
    {
        const cd::Outcome above = run_level_child("avx512");
        INFO(above.output.c_str());
        CHECK(contains(above, "override=above-hardware"));
    }
}

// The child's " crt=<level> fma3=<0|1>" fields as those fields again, with the level capped at `cap`.
cont::String runtime_line(const cd::Outcome& o, long cap, bool fma3)
{
    const std::string_view out{o.output.data(), o.output.size()};
    const std::string_view key = " crt=";
    const std::size_t at = out.find(key);
    REQUIRE(at != std::string_view::npos);
    const long crt = std::strtol(out.data() + at + key.size(), nullptr, 10);
    char line[64] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)std::snprintf(line, sizeof(line), " crt=%ld fma3=%d", crt < cap ? crt : cap, fma3 ? 1 : 0);
    return cont::String{line};
}

TEST_CASE("cpu features: an applied CRD_CPU_LEVEL lowers the C runtime's own dispatch", "[core][cpu]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    if (s.arch != crd::CpuArch::X64 || crd::cpu_runtime_dispatch().crt_level < 0)
    {
        return; // only MSVC's CRT keeps a dispatch level of its own
    }
    // The CRT's levels (isa_availability.h): 1 SSE2, 2 SSE4.2, 5 AVX2; the UCRT's FMA3 libm needs avx2.
    const cd::Outcome own = run_level_child("");
    INFO(own.output.c_str());
    CHECK(contains(own, "override=none"));
    CHECK(std::string_view{own.host_tuple.c_str()}.find("override=none") != std::string_view::npos);
    const bool own_fma3 = contains(own, " fma3=1");

    const cd::Outcome sse2 = run_level_child("sse2");
    INFO(sse2.output.c_str());
    CHECK(contains(sse2, " crt=1 fma3=0"));
    const cd::Outcome portable = run_level_child("portable");
    INFO(portable.output.c_str());
    CHECK(contains(portable, " crt=1 fma3=0"));
    if (crd::cpu_level_includes(s.detected, CpuLevel::Sse42))
    {
        const cd::Outcome sse42 = run_level_child("sse4.2");
        INFO(sse42.output.c_str());
        const cont::String want = runtime_line(own, 2L, false);
        CHECK(contains(sse42, want.c_str()));
    }
    if (crd::cpu_level_includes(s.detected, CpuLevel::Avx2))
    {
        const cd::Outcome avx2 = run_level_child("avx2");
        INFO(avx2.output.c_str());
        const cont::String want = runtime_line(own, 5L, own_fma3);
        CHECK(contains(avx2, want.c_str()));
    }
    // A refused level lowers nothing.
    const cd::Outcome other = run_level_child("neon");
    INFO(other.output.c_str());
    const cont::String unchanged = runtime_line(own, 6L, own_fma3);
    CHECK(contains(other, unchanged.c_str()));
}
