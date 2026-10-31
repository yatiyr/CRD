// The checksum family (crd/containers/checksum.hpp, HW-RESILIENCE.1): the published check values and reference
// vectors, and every crc and hash kernel level the host runs (SIMD-DISPATCH, ADR-0134 §5, §7) against a bitwise CRC
// and the portable XXH3 loop at every length class and alignment. crd-containers-kernels-at-<level> runs these
// [kernels] cases under each forced level, and under SDE's skx and icx (VPCLMULQDQ) models at avx512.

#include "kernel_levels.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/containers/checksum_dispatch.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/cpu_features.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace cc = crd::containers;
using cc::CrcKernels;
using cc::HashKernels;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::test::kernel_level_runs_here;

namespace
{

// The input stream of the reference vectors: byte i is the top byte of 0x9E3779B97F4A7C15 * (i + 1) mod 2^64.
cc::Array<u8> stream_bytes(usize n)
{
    cc::Array<u8> out;
    out.resize(n);
    for (usize i = 0; i < n; ++i)
    {
        out[i] = static_cast<u8>((0x9E3779B97F4A7C15ULL * (i + 1U)) >> 56U);
    }
    return out;
}

struct Vector
{
    usize len;
    u64 xxh3_seed0;
    u64 xxh3_seed1;
    u64 xxh3_seed_golden;
    u32 crc32;
    u32 crc32c;
};

// XXH3-64 from libxxhash 0.8.2 (Ubuntu 24.04 libxxhash0, XXH3_64bits_withSeed through ctypes) with seeds 0, 1 and
// 0x9E3779B97F4A7C15; CRC-32 from Python's zlib.crc32; CRC-32C from a bit-serial reference of polynomial 0x82F63B78.
constexpr Vector kVectors[] = {
    {0U, 0x2D06800538D394C2ULL, 0x4DC5B0CC826F6703ULL, 0x602B0E2CD6662C8BULL, 0x00000000U, 0x00000000U},
    {1U, 0xDD02FBE6D2C66464ULL, 0x1CA7A33BDE2D5D7BULL, 0x02C81AC06CA6090EULL, 0xC5B551CEU, 0x6CAD1061U},
    {2U, 0x64DD7B7921809F37ULL, 0x729465327D85D3B4ULL, 0x8BB8E4A678581FB2ULL, 0x8174C9ECU, 0xBC1C6841U},
    {3U, 0xFEEA62717A65F4B9ULL, 0xD18AA08B06A1711CULL, 0x2D039D0F743C960FULL, 0x1D390EDDU, 0x59E01815U},
    {4U, 0x2F4454DBF80A0E2FULL, 0x877530374FA2D9B0ULL, 0x3977B65C070F7A5CULL, 0x74A381E4U, 0x8CCA6CF8U},
    {5U, 0x4CF5F56AFF1CAD40ULL, 0x19A22CD9B6457068ULL, 0x984083272F0A72C3ULL, 0xF6C2EFAAU, 0xEFD20D03U},
    {7U, 0x78A1317C84B5433DULL, 0xECA811E4BF00CA5CULL, 0xCC876FDD95DEB350ULL, 0x4E541504U, 0xDE7E44BCU},
    {8U, 0x9D3E45DB5E113FD7ULL, 0xB8F466AD5BD18A08ULL, 0x9F8A6DEF77928536ULL, 0x1F9BBD0BU, 0xACF07C7AU},
    {9U, 0x554E53130559708BULL, 0x1E427454805EE180ULL, 0x78B9E1209F0EB03FULL, 0x38C83309U, 0x944ED844U},
    {15U, 0xA59517294C5A21D6ULL, 0x85AC6BDD0CAA4388ULL, 0x9959E4008994600DULL, 0x45F6F88EU, 0x73953A23U},
    {16U, 0x726C0D7E2CE27907ULL, 0xBE4270BCC53F971AULL, 0xB36A6DAB405DBDE1ULL, 0xE1440490U, 0x9183E0AFU},
    {17U, 0x48C881938B08BF45ULL, 0xF2F545E149226F5CULL, 0x3FB92C929DF4B57BULL, 0xB8538B7BU, 0xDE29E148U},
    {31U, 0x2DAF0E7EDCE76228ULL, 0x23D84DA03ABB7796ULL, 0x48B73602FD7B0E90ULL, 0xAB11DC95U, 0x7A8032C3U},
    {32U, 0xB90BF00E3FABCB13ULL, 0x60117B3CC3C05534ULL, 0x9A67EC887530BD86ULL, 0x20CBFE1FU, 0x67F6C77FU},
    {33U, 0xA1197B681EAE9F57ULL, 0xE6A0987F490426FBULL, 0xB1F979810C8EE7E1ULL, 0x62F2BC51U, 0x29A64AC1U},
    {63U, 0x8FA8E2C54CAE0FB4ULL, 0xA0E9D5AC2961F3ACULL, 0x63915833DD5D6C75ULL, 0x9BF535ECU, 0x54980393U},
    {64U, 0x7F639448ABC25E6DULL, 0x4931FDF0C9B033A0ULL, 0x8772732E1FD309CBULL, 0xE82C4B76U, 0xEE0FB31AU},
    {65U, 0xB10D06F3F8477F2AULL, 0xD4B730C2B72B6502ULL, 0x16ED67601D213BA5ULL, 0x59547B2CU, 0x44D1F2BBU},
    {96U, 0x7854026E6AD483CBULL, 0xAEAEEE4AFFFA52C7ULL, 0x0F8A74614764A4E5ULL, 0x9A5776E4U, 0x11745AFCU},
    {97U, 0x32AD83070146A29EULL, 0x50ACC5005573C87EULL, 0x55086C765A230D19ULL, 0x514B3D3CU, 0x0C7F1B2FU},
    {127U, 0x6F831C2A2C7598F8ULL, 0xBDCB58304DEB1B99ULL, 0xE2D2F2888AD7CD65ULL, 0x8D4A21A3U, 0xFEB861A4U},
    {128U, 0x3764BCDB112E17FAULL, 0x251B200C17330068ULL, 0x12364220269A2FCEULL, 0x17359E12U, 0xBE85A5DDU},
    {129U, 0x3EB30581B3D9C562ULL, 0xB2B9AA67E630434CULL, 0x8B1985E1E97B3A6FULL, 0x9311A073U, 0xF49FD289U},
    {191U, 0x594FD91B3AACA563ULL, 0x609D834D705CADA3ULL, 0xDF0969DB17EFDB24ULL, 0xA37A94C5U, 0x7AD9A955U},
    {239U, 0xA089379BBD71879EULL, 0x152719B7A9E97B74ULL, 0x2FB9B8B4ECD595A5ULL, 0x58B1AAEEU, 0x535CD158U},
    {240U, 0x3CAC406073FB6376ULL, 0x146AA0B1A74D57C8ULL, 0xF3370B1A08CE519EULL, 0xF9EE04B5U, 0x1F6DC050U},
    {241U, 0x6DEB1AE71A8A8BECULL, 0xDCE9E6EE855C45DFULL, 0xF18730308057AB3DULL, 0x3A43D5BAU, 0x1112FBC0U},
    {255U, 0x23B90DB6D6DE6567ULL, 0x0CA0343FD27E4BCFULL, 0xE82715872B8A92BEULL, 0xC3101C11U, 0xE80FE440U},
    {256U, 0x76A9AC333596068EULL, 0xFBFC1062F84C04E6ULL, 0x3315880380F8EE2FULL, 0x00CC7A6CU, 0xF7C76C53U},
    {511U, 0x4F4C8266F17FB3D6ULL, 0xB34045826DA7522BULL, 0x14B13338CD0A68D0ULL, 0x198713D4U, 0x0F4502F9U},
    {512U, 0x9D113A046367A1F3ULL, 0x5D22AE2B56349436ULL, 0x87819E4C4141EF36ULL, 0xF9AF320CU, 0x14108EAFU},
    {1023U, 0x53F97B096F6E41A0ULL, 0x08713599E789B764ULL, 0x63EF19511BFFE156ULL, 0xFB62A492U, 0x4AA447FAU},
    {1024U, 0x5370F57883C8F088ULL, 0x2FB8DFE16C4E7525ULL, 0xDB24AADB92B8409DULL, 0x349AD128U, 0xA6401D23U},
    {1025U, 0x2E7BC851FC4A1332ULL, 0x8F0B64866C7425BCULL, 0x3A5E2F63B44011CFULL, 0xBE30E0B1U, 0x5DEDF5BBU},
    {2047U, 0x4E6ABBC59C1828F5ULL, 0x67785DAAD7EBB41EULL, 0x5F4EE0F3FD5D0EB8ULL, 0x4D4AB8DEU, 0xAE8BBEFCU},
    {2048U, 0x1A4A21CFF48B1290ULL, 0x61C7C5B459C1D536ULL, 0x22FB6D0E2CB5A35CULL, 0xEF9730E2U, 0xC762A1B8U},
    {4095U, 0x8EC5F347403453ADULL, 0x7C2499C7EF8DA07CULL, 0xBD34EBC988FEF1E8ULL, 0xFD4989B1U, 0xB16273CCU},
    {4096U, 0xB5F432DCAE68FF30ULL, 0xA26F25FF5CF54531ULL, 0x6A54CDE6821FE90EULL, 0xA0F8C181U, 0x7950E8D0U},
    {10000U, 0xDE0BD01BECCBAE41ULL, 0xF70A568F4CED9CEBULL, 0x24A50AA3CFB2181DULL, 0xC4A7C005U, 0x912FA144U},
    {65537U, 0x0457A31B37522578ULL, 0xE6CC871B5B934C65ULL, 0xBDB4EA324D2446FFULL, 0x228FF2E6U, 0x475F3AC8U},
};

constexpr u64 kGoldenSeed = 0x9E3779B97F4A7C15ULL;

// The CRC register of a reflected polynomial, one bit at a time: the definition the kernels are judged against.
u32 bitwise_register(u32 poly, u32 state, const u8* p, usize n)
{
    for (usize i = 0; i < n; ++i)
    {
        state ^= p[i];
        for (int b = 0; b < 8; ++b)
        {
            state = (state >> 1U) ^ ((state & 1U) != 0U ? poly : 0U);
        }
    }
    return state;
}

constexpr u32 kPoly32 = 0xEDB88320U;
constexpr u32 kPoly32c = 0x82F63B78U;

// XXH3's initial accumulators (the specification's XXH3_INIT_ACC).
constexpr u64 kInitAcc[8] = {0xC2B2AE3DULL,         0x9E3779B185EBCA87ULL, 0xC2B2AE3D27D4EB4FULL, 0x165667B19E3779F9ULL,
                             0x85EBCA77C2B2AE63ULL, 0x85EBCA77ULL,         0x27D4EB2F165667C5ULL, 0x9E3779B1ULL};

// The accumulators a hash level leaves after n bytes, between two guard words it must not touch.
void accumulate(const HashKernels& k, const u8* data, usize n, const u8* secret, u64 (&out)[8], bool& guards_kept)
{
    constexpr u64 guard = 0xA5A5A5A55A5A5A5AULL;
    u64 words[10] = {guard};
    std::memcpy(words + 1, kInitAcc, sizeof(kInitAcc));
    words[9] = guard;
    k.xxh3_accumulate(words + 1, data, n, secret);
    std::memcpy(out, words + 1, sizeof(out));
    guards_kept = words[0] == guard && words[9] == guard;
}

} // namespace

TEST_CASE("checksum: the published check values", "[containers][checksum]")
{
    const char* check = "123456789";
    const cc::ConstSpan<u8> bytes(reinterpret_cast<const u8*>(check), 9U);
    CHECK(cc::crc32(bytes) == 0xCBF43926U);
    CHECK(cc::crc32c(bytes) == 0xE3069283U);
    CHECK(cc::crc32({}) == 0U);
    CHECK(cc::crc32c({}) == 0U);
    CHECK(cc::xxh3_64({}) == 0x2D06800538D394C2ULL);
}

TEST_CASE("checksum: the reference vectors of every length class", "[containers][checksum][kernels]")
{
    const cc::Array<u8> data = stream_bytes(65537U);
    for (const Vector& v : kVectors)
    {
        INFO("length " << v.len);
        const cc::ConstSpan<u8> bytes(data.data(), v.len);
        CHECK(cc::crc32(bytes) == v.crc32);
        CHECK(cc::crc32c(bytes) == v.crc32c);
        CHECK(cc::xxh3_64(bytes) == v.xxh3_seed0);
        CHECK(cc::xxh3_64(bytes, 1U) == v.xxh3_seed1);
        CHECK(cc::xxh3_64(bytes, kGoldenSeed) == v.xxh3_seed_golden);
    }
}

TEST_CASE("checksum: the CRCs chain over any split", "[containers][checksum][kernels]")
{
    const cc::Array<u8> data = stream_bytes(1000U);
    const cc::ConstSpan<u8> all(data.data(), data.size());
    const u32 whole32 = cc::crc32(all);
    const u32 whole32c = cc::crc32c(all);
    usize bad = 0;
    for (usize split = 0; split <= data.size(); ++split)
    {
        const cc::ConstSpan<u8> head = all.first(split);
        const cc::ConstSpan<u8> tail = all.subspan(split);
        bad += cc::crc32(tail, cc::crc32(head)) == whole32 ? 0U : 1U;
        bad += cc::crc32c(tail, cc::crc32c(head)) == whole32c ? 0U : 1U;
    }
    CHECK(bad == 0U);
}

TEST_CASE("checksum kernels: every crc level gives the bitwise register", "[containers][checksum][kernels]")
{
    // Every length to 1100 at three alignments meets each body's 256/128/64/32/16-byte steps and byte tails; every
    // alignment 0..63 at the step boundaries; lengths to 1 MiB run the wide loops. The source bytes stay the same, so
    // one reference register per length serves every alignment.
    constexpr usize max_len = 1100U;
    const cc::Array<u8> data = stream_bytes((1U << 20U) + 64U);
    cc::Array<u8> shifted;
    shifted.resize((1U << 20U) + 64U);
    const u32 state = 0x12345678U; // a register mid-stream, not only the initial one
    cc::Array<u32> want32;
    cc::Array<u32> want32c;
    for (usize len = 0; len <= max_len; ++len)
    {
        want32.push_back(bitwise_register(kPoly32, state, data.data(), len));
        want32c.push_back(bitwise_register(kPoly32c, state, data.data(), len));
    }
    constexpr usize long_lens[] = {4095U, 65536U + 17U, 300000U, 1U << 20U};
    u32 long32[4];
    u32 long32c[4];
    for (usize i = 0; i < 4U; ++i)
    {
        long32[i] = bitwise_register(kPoly32, state, data.data(), long_lens[i]);
        long32c[i] = bitwise_register(kPoly32c, state, data.data(), long_lens[i]);
    }
    usize ran = 0;
    for (const KernelLevel<CrcKernels>& level : cc::crc_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level) << " features " << level.features.bits);
        usize bad = 0;
        const auto check = [&](usize len, usize align, u32 w32, u32 w32c)
        {
            std::memcpy(shifted.data() + align, data.data(), len);
            const u8* p = shifted.data() + align;
            bad += level.table->crc32(state, p, len) == w32 ? 0U : 1U;
            bad += level.table->crc32c(state, p, len) == w32c ? 0U : 1U;
        };
        for (usize len = 0; len <= max_len; ++len)
        {
            const bool boundary = len < 24U || (len + 1U) % 16U < 3U;
            for (usize align = 0; align < 64U; ++align)
            {
                if (boundary || align == 0U || align == 1U || align == 13U)
                {
                    check(len, align, want32[len], want32c[len]);
                }
            }
        }
        for (usize i = 0; i < 4U; ++i)
        {
            check(long_lens[i], long_lens[i] % 61U, long32[i], long32c[i]);
        }
        CHECK(bad == 0U);
    }
    CHECK(ran >= 1U);
}

TEST_CASE("checksum kernels: every hash level gives the portable accumulators", "[containers][checksum][kernels]")
{
    // Every long length to 1400 covers a partial block with every stripe count and the overlapping last stripe; 1 MiB
    // covers many scrambles. A derived secret checks a level reads the secret it is given.
    const cc::Array<u8> data = stream_bytes((1U << 20U) + 64U);
    cc::Array<u8> shifted;
    shifted.resize((1U << 20U) + 64U);
    u8 secret[192];
    for (usize i = 0; i < sizeof(secret); ++i)
    {
        secret[i] = static_cast<u8>((i * 167U) ^ 0x5AU);
    }
    const HashKernels& portable = *cc::hash_kernel_levels()[0].table;
    usize ran = 0;
    for (const KernelLevel<HashKernels>& level : cc::hash_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        usize bad = 0;
        const auto check = [&](usize len, usize align)
        {
            std::memcpy(shifted.data() + align, data.data(), len);
            u64 want[8];
            u64 got[8];
            bool want_guards = false;
            bool got_guards = false;
            accumulate(portable, data.data(), len, secret, want, want_guards);
            accumulate(*level.table, shifted.data() + align, len, secret, got, got_guards);
            bad += std::memcmp(want, got, sizeof(want)) == 0 && want_guards && got_guards ? 0U : 1U;
        };
        for (usize len = 241U; len <= 1400U; ++len)
        {
            check(len, len % 3U == 0U ? 0U : len % 64U);
        }
        for (const usize len : {4096U, 65537U, 1U << 20U})
        {
            check(len, 7U);
        }
        CHECK(bad == 0U);
    }
    CHECK(ran >= 1U);
}

TEST_CASE("checksum kernels: the process runs the levels its CPU selection resolves", "[containers][checksum][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const KernelLevel<CrcKernels>& crc = cc::crc_kernels();
    const KernelLevel<HashKernels>& hash = cc::hash_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", crc " << crd::cpu_level_name(crc.level) << ", hash "
                     << crd::cpu_level_name(hash.level));
    CHECK(&crc == &crd::select_kernel_level<CrcKernels>(cc::crc_kernel_levels(), s.selected, s.kernel_features,
                                                        crd::kernel_level_cap()));
    CHECK(&hash == &crd::select_kernel_level<HashKernels>(cc::hash_kernel_levels(), s.selected, s.kernel_features,
                                                          crd::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, crc.level));
    CHECK(crd::cpu_level_includes(s.selected, hash.level));
    // Evidence on every run (CTest's log and JUnit keep it): which crc variant and hash level this host ran, so a
    // hosted lane's record shows the carry-less and CRC32 variants were the ones tested, not only the plain levels.
    cc::String variant;
    for (u32 f = 0; f < static_cast<u32>(crd::CpuFeature::Count); ++f)
    {
        if (crc.features.has(static_cast<crd::CpuFeature>(f)))
        {
            variant.append(" +");
            variant.append(crd::cpu_feature_name(static_cast<crd::CpuFeature>(f)));
        }
    }
    WARN("checksum kernels ran: crc " << crd::cpu_level_name(crc.level) << variant.c_str() << ", hash "
                                      << crd::cpu_level_name(hash.level));
}
