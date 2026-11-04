// HESAP-NET's packet erasure codes: GF(2^8) of RFC 5510 §8.1 against a carry-less reference multiply, the dispatched
// dot product at every level the host runs against the scalar product (crd-hesap-comms-kernels-at-<level> runs the
// [kernels] cases under each forced level), and the RFC 5510 Reed-Solomon code against the RFC's literal matrices:
// GM = V(k,k)^-1 V(k,n) by Gauss-Jordan inversion, encoding as s × GM, and recovery from every k-subset of small codes.

#include "kernel_levels.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/comms/erasure.hpp>
#include <crd/hesap/comms/erasure_dispatch.hpp>
#include <crd/hesap/comms/gf256.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string_view>
#include <utility>

namespace hc = crd::hesap::comms;
using crd::CpuLevel;
using crd::KernelLevel;
using crd::u32;
using crd::u8;
using crd::usize;
using hc::ErasureKernels;
using hc::ErasureStatus;
using hc::ErasureSymbol;
using hc::ReedSolomonCode;
template <typename T> using Array = crd::containers::Array<T>;
using Bytes = Array<u8>;

namespace
{

// Shift-and-add multiplication modulo x^8 + x^4 + x^3 + x^2 + 1, independent of the tables.
u8 reference_mul(u8 a, u8 b)
{
    u32 product = 0U;
    for (u32 bit = 0U; bit < 8U; ++bit)
    {
        if ((b >> bit) & 1U)
        {
            product ^= static_cast<u32>(a) << bit;
        }
    }
    for (u32 bit = 15U; bit >= 8U; --bit)
    {
        if ((product >> bit) & 1U)
        {
            product ^= 0x11DU << (bit - 8U);
        }
    }
    return static_cast<u8>(product);
}

u8 reference_pow(u8 a, u32 e)
{
    u8 r = 1U;
    for (u32 i = 0U; i < e; ++i)
    {
        r = reference_mul(r, a);
    }
    return r;
}

Bytes filled(usize n, u8 fill)
{
    Bytes v;
    v.resize(n, fill);
    return v;
}

Bytes random_bytes(crd::hesap::stats::PhiloxRng& rng, usize n)
{
    Bytes v = filled(n, 0U);
    for (u8& b : v)
    {
        b = static_cast<u8>(rng.next_u32());
    }
    return v;
}

bool same(const Bytes& a, const Bytes& b)
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// RFC 5510 §8.2's generator matrix, literally: V(k,n) with v_ij = alpha^(i·j), and GM = V(k,k)^-1 V(k,n) with the
// inverse by Gauss-Jordan elimination, in the field the first two cases hold to the reference multiply. Row-major.
Bytes rfc_generator_matrix(u32 k, u32 n)
{
    Bytes v = filled(static_cast<usize>(k) * n, 0U);
    for (u32 i = 0U; i < k; ++i)
    {
        for (u32 j = 0U; j < n; ++j)
        {
            v[(static_cast<usize>(i) * n) + j] = hc::gf256_alpha_pow(static_cast<crd::u64>(i) * j);
        }
    }
    // [V(k,k) | I] -> [I | V(k,k)^-1].
    Bytes a = filled(static_cast<usize>(k) * 2U * k, 0U);
    const usize w = 2U * k;
    for (u32 i = 0U; i < k; ++i)
    {
        for (u32 j = 0U; j < k; ++j)
        {
            a[(i * w) + j] = v[(static_cast<usize>(i) * n) + j];
        }
        a[(i * w) + k + i] = 1U;
    }
    for (u32 col = 0U; col < k; ++col)
    {
        u32 pivot = col;
        while (a[(pivot * w) + col] == 0U)
        {
            ++pivot;
        }
        for (usize c = 0U; c < w; ++c)
        {
            const u8 t = a[(col * w) + c];
            a[(col * w) + c] = a[(pivot * w) + c];
            a[(pivot * w) + c] = t;
        }
        const u8 inv = hc::gf256_inv(a[(col * w) + col]);
        for (usize c = 0U; c < w; ++c)
        {
            a[(col * w) + c] = hc::gf256_mul(a[(col * w) + c], inv);
        }
        for (u32 r = 0U; r < k; ++r)
        {
            const u8 f = a[(r * w) + col];
            if (r != col && f != 0U)
            {
                for (usize c = 0U; c < w; ++c)
                {
                    a[(r * w) + c] = static_cast<u8>(a[(r * w) + c] ^ hc::gf256_mul(f, a[(col * w) + c]));
                }
            }
        }
    }
    Bytes gm = filled(static_cast<usize>(k) * n, 0U);
    for (u32 i = 0U; i < k; ++i)
    {
        for (u32 j = 0U; j < n; ++j)
        {
            u8 acc = 0U;
            for (u32 l = 0U; l < k; ++l)
            {
                acc = static_cast<u8>(acc ^ hc::gf256_mul(a[(i * w) + k + l], v[(static_cast<usize>(l) * n) + j]));
            }
            gm[(static_cast<usize>(i) * n) + j] = acc;
        }
    }
    return gm;
}

// One block: k random sources of `bytes`, and every encoding symbol e = s × GM computed from `gm` byte by byte.
struct Block
{
    Array<Bytes> symbols; // n encoding symbols, the first k the sources
};

Block rfc_block(const Bytes& gm, u32 k, u32 n, usize bytes, crd::hesap::stats::PhiloxRng& rng)
{
    Block b;
    for (u32 i = 0U; i < k; ++i)
    {
        b.symbols.push_back(random_bytes(rng, bytes));
    }
    for (u32 j = k; j < n; ++j)
    {
        Bytes e = filled(bytes, 0U);
        for (usize u = 0U; u < bytes; ++u)
        {
            u8 acc = 0U;
            for (u32 i = 0U; i < k; ++i)
            {
                acc = static_cast<u8>(acc ^ hc::gf256_mul(b.symbols[i][u], gm[(static_cast<usize>(i) * n) + j]));
            }
            e[u] = acc;
        }
        b.symbols.push_back(std::move(e));
    }
    return b;
}

// Decodes `block` from the encoding symbols `received` (k indices) and checks every lost source.
bool recovers(const ReedSolomonCode& code, const Block& block, const Array<u32>& received, usize bytes)
{
    Array<ErasureSymbol> symbols;
    Bytes have = filled(code.symbol_count(), 0U);
    for (const u32 index : received)
    {
        symbols.push_back({index, block.symbols[index].data()});
        have[index] = 1U;
    }
    Array<u32> lost;
    for (u32 i = 0U; i < code.source_count(); ++i)
    {
        if (have[i] == 0U)
        {
            lost.push_back(i);
        }
    }
    Array<Bytes> out;
    Array<u8*> outputs;
    for (usize q = 0U; q < lost.size(); ++q)
    {
        out.push_back(filled(bytes, 0xA5U));
        outputs.push_back(out[q].data());
    }
    if (code.decode(symbols, lost, outputs, bytes) != ErasureStatus::Ok)
    {
        return false;
    }
    for (usize q = 0U; q < lost.size(); ++q)
    {
        if (!same(out[q], block.symbols[lost[q]]))
        {
            return false;
        }
    }
    return true;
}

std::string_view routine_of(const char* routine)
{
    return routine != nullptr ? routine : "";
}

} // namespace

TEST_CASE("gf256: the tables are RFC 5510's field with alpha = 2", "[erasure]")
{
    CHECK(hc::kGf256.exp[0] == 1U);
    CHECK(hc::gf256_alpha_pow(255U) == 1U);
    bool seen[256] = {};
    for (u32 e = 0U; e < 255U; ++e)
    {
        const u8 x = hc::gf256_alpha_pow(e);
        CHECK(x == reference_pow(2U, e));
        CHECK_FALSE(seen[x]); // alpha is primitive: its 255 powers are the 255 nonzero bytes
        seen[x] = true;
        CHECK(hc::kGf256.log[x] == e);
        CHECK(hc::kGf256.exp[e + 255U] == x);
    }
    CHECK_FALSE(seen[0]);
}

TEST_CASE("gf256: multiplication, inverse, division and expansion over every pair", "[erasure]")
{
    usize bad = 0U;
    for (u32 a = 0U; a < 256U; ++a)
    {
        const hc::GfCoefficient e = hc::gf256_expand(static_cast<u8>(a));
        for (u32 b = 0U; b < 256U; ++b)
        {
            const u8 p = hc::gf256_mul(static_cast<u8>(a), static_cast<u8>(b));
            bad += p == reference_mul(static_cast<u8>(a), static_cast<u8>(b)) ? 0U : 1U;
            bad += p == static_cast<u8>(e.lo[b & 15U] ^ e.hi[b >> 4U]) ? 0U : 1U;
            if (b != 0U)
            {
                bad += hc::gf256_mul(hc::gf256_div(static_cast<u8>(a), static_cast<u8>(b)), static_cast<u8>(b)) ==
                               a
                           ? 0U
                           : 1U;
            }
        }
        if (a != 0U)
        {
            bad += hc::gf256_mul(static_cast<u8>(a), hc::gf256_inv(static_cast<u8>(a))) == 1U ? 0U : 1U;
        }
    }
    CHECK(bad == 0U);
    static_assert(hc::gf256_mul(0x80U, 2U) == 0x1DU, "x^8 reduces to x^4 + x^3 + x^2 + 1");
    static_assert(hc::gf256_inv(1U) == 1U && hc::gf256_div(0U, 7U) == 0U);
}

TEST_CASE("erasure kernels: the archive holds portable and each body level the build compiles", "[erasure][kernels]")
{
    const auto levels = hc::erasure_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    const crd::CpuArch arch = crd::cpu_selection().arch;
    bool sse2 = false;
    bool avx2 = false;
    bool avx512 = false;
    bool neon = false;
    for (usize i = 0U; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        sse2 = sse2 || levels[i].level == CpuLevel::Sse2;
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
        avx512 = avx512 || levels[i].level == CpuLevel::Avx512;
        neon = neon || levels[i].level == CpuLevel::Neon;
    }
    const bool x64 = arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
    CHECK(avx512 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx512)));
    CHECK(neon == (arch == crd::CpuArch::Arm64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Neon)));
}

TEST_CASE("erasure kernels: every level the host runs gives the scalar dot product", "[erasure][kernels]")
{
    crd::hesap::stats::PhiloxRng rng(2026, 11);
    // Up to nine outputs (two groups of four and one, and every shorter group) of up to seven sources, every length
    // from 0 to 300 (every vector count and remainder of each level), and 2100 and 2111 bytes (the SSE2 body's two
    // whole 1 KiB tiles and a partial one), from odd offsets, accumulating and not.
    constexpr usize kMaxOut = 9U;
    constexpr usize kMaxSrc = 7U;
    constexpr usize kMaxBytes = 2111U;
    Array<Bytes> src;
    for (usize j = 0U; j < kMaxSrc; ++j)
    {
        src.push_back(random_bytes(rng, kMaxBytes + 3U));
    }
    Bytes coef = random_bytes(rng, kMaxOut * kMaxSrc);
    coef[0] = 0U;
    coef[1] = 1U;
    coef[2] = 0xFFU;
    Array<hc::GfCoefficient> rows;
    rows.resize(kMaxOut * kMaxSrc);
    Array<usize> lengths;
    for (usize n = 0U; n <= 300U; ++n)
    {
        lengths.push_back(n);
    }
    lengths.push_back(2100U);
    lengths.push_back(2111U);
    usize ran = 0U;
    for (const KernelLevel<ErasureKernels>& level : hc::erasure_kernel_levels())
    {
        if (!crd::test::kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        usize bad = 0U;
        for (usize outs = 1U; outs <= kMaxOut; outs += (outs < 5U ? 1U : 4U))
        {
            for (usize count = 0U; count <= kMaxSrc; count += (count < 3U ? 1U : 4U))
            {
                for (usize g = 0U; g < outs; ++g)
                {
                    for (usize j = 0U; j < count; ++j)
                    {
                        rows[(g * count) + j] = hc::gf256_expand(coef[(g * kMaxSrc) + j]);
                    }
                }
                for (const usize bytes : lengths)
                {
                    const u8* sources[kMaxSrc];
                    for (usize j = 0U; j < count; ++j)
                    {
                        sources[j] = src[j].data() + 1U + (j & 1U);
                    }
                    for (const bool accumulate : {false, true})
                    {
                        Array<Bytes> out;
                        Array<Bytes> want;
                        u8* outputs[kMaxOut];
                        for (usize g = 0U; g < outs; ++g)
                        {
                            out.push_back(filled(bytes + 2U, 0U));
                            want.push_back(filled(bytes, 0U));
                            for (usize i = 0U; i < bytes + 2U; ++i)
                            {
                                out[g][i] = static_cast<u8>((i * 7U) + g);
                            }
                            for (usize i = 0U; i < bytes; ++i)
                            {
                                u8 acc = accumulate ? out[g][i + 1U] : u8{0U};
                                for (usize j = 0U; j < count; ++j)
                                {
                                    acc = static_cast<u8>(acc ^ hc::gf256_mul(coef[(g * kMaxSrc) + j], sources[j][i]));
                                }
                                want[g][i] = acc;
                            }
                            outputs[g] = out[g].data() + 1U;
                        }
                        level.table->dot(rows.data(), sources, count, outputs, outs, bytes, accumulate);
                        for (usize g = 0U; g < outs; ++g)
                        {
                            bad += bytes == 0U || std::memcmp(outputs[g], want[g].data(), bytes) == 0 ? 0U : 1U;
                            bad += out[g][0] == static_cast<u8>(g) ? 0U : 1U; // the guard bytes either side
                            bad += out[g][bytes + 1U] == static_cast<u8>(((bytes + 1U) * 7U) + g) ? 0U : 1U;
                        }
                    }
                }
            }
        }
        CHECK(bad == 0U);
    }
    CHECK(ran >= 1U);
}

TEST_CASE("erasure kernels: the process runs the level its selection allows", "[erasure][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const KernelLevel<ErasureKernels>& chosen = hc::erasure_kernels();
    CHECK(&chosen == &crd::select_kernel_level<ErasureKernels>(hc::erasure_kernel_levels(), s.selected,
                                                               s.kernel_features, crd::kernel_level_cap()));
    const crd::KernelFamily* family = crd::test::kernel_family("hesap.erasure");
    REQUIRE(family != nullptr);
    CHECK(family->result.status == crd::KernelCheckStatus::Passed);
    CHECK(family->result.level == chosen.level);
}

namespace
{

// The portable dot product with the last byte of the last output flipped.
void dot_tail_off(const hc::GfCoefficient* c, const u8* const* sources, usize count, u8* const* outputs, usize nout,
                  usize bytes, bool accumulate) noexcept
{
    hc::kErasureKernelsPortable.dot(c, sources, count, outputs, nout, bytes, accumulate);
    if (!accumulate && nout != 0U && bytes != 0U)
    {
        outputs[nout - 1U][bytes - 1U] ^= 1U;
    }
}

} // namespace

TEST_CASE("erasure self-check: every table the host runs passes, and a wrong one fails", "[erasure][kernels]")
{
    const crd::test::SelfCheckRun run =
        crd::test::self_check_levels(hc::erasure_kernel_levels(), &hc::erasure_self_check);
    INFO("first failure " << crd::cpu_level_name(run.level) << " " << routine_of(run.routine));
    CHECK(run.failed == 0U);
    CHECK(run.ran >= 1U);
    ErasureKernels wrong = hc::kErasureKernelsPortable;
    wrong.dot = &dot_tail_off;
    CHECK(routine_of(hc::erasure_self_check(wrong)) == "dot");
}

TEST_CASE("reed-solomon: GM is RFC 5510's V(k,k)^-1 V(k,n), systematic", "[erasure]")
{
    const u32 shapes[][2] = {{1U, 1U},   {1U, 9U},    {2U, 3U},     {3U, 7U},
                             {10U, 30U}, {64U, 255U}, {200U, 255U}, {255U, 255U}};
    for (const auto& shape : shapes)
    {
        const u32 k = shape[0];
        const u32 n = shape[1];
        INFO("k " << k << " n " << n);
        const ReedSolomonCode code(k, n);
        CHECK(code.source_count() == k);
        CHECK(code.symbol_count() == n);
        const Bytes gm = rfc_generator_matrix(k, n);
        usize bad = 0U;
        for (u32 i = 0U; i < k; ++i)
        {
            for (u32 j = 0U; j < n; ++j)
            {
                const u8 want = gm[(static_cast<usize>(i) * n) + j];
                bad += code.coefficient(i, j) == want ? 0U : 1U;
                if (j < k)
                {
                    bad += want == (i == j ? 1U : 0U) ? 0U : 1U;
                }
            }
        }
        CHECK(bad == 0U);
    }
    CHECK(ReedSolomonCode::valid(1U, 1U));
    CHECK(ReedSolomonCode::valid(255U, 255U));
    CHECK_FALSE(ReedSolomonCode::valid(0U, 5U));
    CHECK_FALSE(ReedSolomonCode::valid(6U, 5U));
    CHECK_FALSE(ReedSolomonCode::valid(5U, 256U));
}

TEST_CASE("reed-solomon: repair symbols are s x GM, and any k of n recover the block", "[erasure]")
{
    crd::hesap::stats::PhiloxRng rng(2026, 12);
    // Every k-subset of the small codes, both of whose sizes make a lost-source count of every size.
    const u32 small[][2] = {{1U, 4U}, {3U, 7U}, {4U, 8U}, {5U, 10U}};
    for (const auto& shape : small)
    {
        const u32 k = shape[0];
        const u32 n = shape[1];
        INFO("k " << k << " n " << n);
        const ReedSolomonCode code(k, n);
        const usize bytes = 67U;
        const Block block = rfc_block(rfc_generator_matrix(k, n), k, n, bytes, rng);
        Array<u32> repair;
        Array<Bytes> encoded;
        Array<u8*> outputs;
        Array<const u8*> sources;
        for (u32 i = 0U; i < k; ++i)
        {
            sources.push_back(block.symbols[i].data());
        }
        for (u32 j = k; j < n; ++j)
        {
            repair.push_back(j);
            encoded.push_back(filled(bytes, 0U));
            outputs.push_back(encoded[j - k].data());
        }
        REQUIRE(code.encode(sources, repair, outputs, bytes) == ErasureStatus::Ok);
        for (u32 j = k; j < n; ++j)
        {
            CHECK(same(encoded[j - k], block.symbols[j]));
        }
        usize subsets = 0U;
        usize failed = 0U;
        for (u32 mask = 0U; mask < (1U << n); ++mask)
        {
            Array<u32> received;
            for (u32 j = 0U; j < n; ++j)
            {
                if ((mask >> j) & 1U)
                {
                    received.push_back(j);
                }
            }
            if (received.size() == k)
            {
                ++subsets;
                failed += recovers(code, block, received, bytes) ? 0U : 1U;
            }
        }
        CHECK(subsets > 0U);
        CHECK(failed == 0U);
    }
}

TEST_CASE("reed-solomon: wide blocks encode in any order and decode random erasure patterns", "[erasure]")
{
    crd::hesap::stats::PhiloxRng rng(2026, 13);
    const u32 shapes[][2] = {{1U, 255U}, {33U, 70U}, {100U, 255U}, {200U, 255U}, {255U, 255U}};
    for (const auto& shape : shapes)
    {
        const u32 k = shape[0];
        const u32 n = shape[1];
        INFO("k " << k << " n " << n);
        const ReedSolomonCode code(k, n);
        const usize bytes = 401U;
        const Block block = rfc_block(rfc_generator_matrix(k, n), k, n, bytes, rng);
        Array<const u8*> sources;
        for (u32 i = 0U; i < k; ++i)
        {
            sources.push_back(block.symbols[i].data());
        }
        // Repair symbols one at a time, in reverse order.
        usize bad = 0U;
        for (u32 j = n; j > k; --j)
        {
            Bytes out = filled(bytes, 0U);
            u8* const outputs[1] = {out.data()};
            const u32 index[1] = {j - 1U};
            bad += code.encode(sources, index, outputs, bytes) == ErasureStatus::Ok ? 0U : 1U;
            bad += same(out, block.symbols[j - 1U]) ? 0U : 1U;
        }
        CHECK(bad == 0U);
        // Random patterns: a shuffled order of all n symbols, the first k received, in shuffled order.
        for (u32 trial = 0U; trial < 6U; ++trial)
        {
            Array<u32> order;
            for (u32 j = 0U; j < n; ++j)
            {
                order.push_back(j);
            }
            for (u32 j = n - 1U; j > 0U; --j)
            {
                const u32 r = rng.next_u32() % (j + 1U);
                const u32 t = order[j];
                order[j] = order[r];
                order[r] = t;
            }
            order.resize(k);
            CHECK(recovers(code, block, order, bytes));
        }
        if (n - k >= k)
        {
            // Every source lost: the block from repair symbols alone.
            Array<u32> repair_only;
            for (u32 j = n - k; j < n; ++j)
            {
                repair_only.push_back(j);
            }
            CHECK(recovers(code, block, repair_only, bytes));
        }
    }
}

TEST_CASE("reed-solomon: k = 1 repeats the source, and zero bytes or nothing lost write nothing", "[erasure]")
{
    const ReedSolomonCode code(1U, 6U);
    const u8 source[5] = {1U, 2U, 3U, 0xFEU, 0x80U};
    const u8* const sources[1] = {source};
    u8 out[4][5] = {};
    u8* const outputs[4] = {out[0], out[1], out[2], out[3]};
    const u32 repair[4] = {5U, 1U, 4U, 2U};
    REQUIRE(code.encode(sources, repair, outputs, 5U) == ErasureStatus::Ok);
    for (const auto& o : out)
    {
        CHECK(std::memcmp(o, source, 5U) == 0);
    }
    u8 untouched[3] = {7U, 7U, 7U};
    u8* const one[1] = {untouched};
    const u32 five[1] = {5U};
    CHECK(code.encode(sources, five, one, 0U) == ErasureStatus::Ok);
    CHECK(untouched[0] == 7U);
    const ErasureSymbol all[1] = {{0U, source}};
    CHECK(code.decode(all, {}, {}, 5U) == ErasureStatus::Ok);
}

TEST_CASE("reed-solomon: malformed requests are refused and write nothing", "[erasure]")
{
    const ReedSolomonCode code(3U, 6U);
    const u8 data[6][4] = {{1U}, {2U}, {3U}, {4U}, {5U}, {6U}};
    const u8* const sources[3] = {data[0], data[1], data[2]};
    u8 out[2][4] = {{9U, 9U, 9U, 9U}, {9U, 9U, 9U, 9U}};
    u8* const outputs[2] = {out[0], out[1]};
    const auto untouched = [&] { return out[0][0] == 9U && out[1][3] == 9U; };

    const u32 r34[2] = {3U, 4U};
    CHECK(code.encode(std::span<const u8* const>(sources, 2U), r34, outputs, 4U) == ErasureStatus::WrongCount);
    CHECK(code.encode(sources, std::span<const u32>(r34, 1U), outputs, 4U) == ErasureStatus::WrongCount);
    const u32 r2[2] = {2U, 4U};
    CHECK(code.encode(sources, r2, outputs, 4U) == ErasureStatus::IndexOutOfRange);
    const u32 r6[2] = {3U, 6U};
    CHECK(code.encode(sources, r6, outputs, 4U) == ErasureStatus::IndexOutOfRange);
    const u32 r44[2] = {4U, 4U};
    CHECK(code.encode(sources, r44, outputs, 4U) == ErasureStatus::DuplicateIndex);
    CHECK(untouched());

    const ErasureSymbol good[3] = {{3U, data[3]}, {4U, data[4]}, {2U, data[2]}};
    const u32 lost01[2] = {0U, 1U};
    CHECK(code.decode(std::span<const ErasureSymbol>(good, 2U), lost01, outputs, 4U) == ErasureStatus::WrongCount);
    CHECK(code.decode(good, std::span<const u32>(lost01, 1U), outputs, 4U) == ErasureStatus::WrongCount);
    const ErasureSymbol far[3] = {{3U, data[3]}, {6U, data[4]}, {2U, data[2]}};
    CHECK(code.decode(far, lost01, outputs, 4U) == ErasureStatus::IndexOutOfRange);
    const ErasureSymbol twice[3] = {{3U, data[3]}, {3U, data[4]}, {2U, data[2]}};
    CHECK(code.decode(twice, lost01, outputs, 4U) == ErasureStatus::DuplicateIndex);
    const u32 lost03[2] = {0U, 3U};
    CHECK(code.decode(good, lost03, outputs, 4U) == ErasureStatus::IndexOutOfRange);
    const u32 lost00[2] = {0U, 0U};
    CHECK(code.decode(good, lost00, outputs, 4U) == ErasureStatus::DuplicateIndex);
    const u32 lost02[2] = {0U, 2U};
    CHECK(code.decode(good, lost02, outputs, 4U) == ErasureStatus::NotLost);
    CHECK(untouched());
}
