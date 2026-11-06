// HESAP-NET's Reed-Solomon code against Intel ISA-L's erasure code (CRD_BUILD_ERASURE_VS_ISAL; cmake/pins.json isa-l,
// its erasure_code/ec_base.c and on x86-64 its assembly, compiled as crd-isal-peer). ISA-L's field
// (gf_mul and gf_inv over its own logarithm tables of 0x11D) and its Gauss-Jordan inversion (gf_invert_matrix) are an
// implementation written apart from hesap's, so every byte below is held to a second party:
//   * every product and inverse of the field;
//   * the generator matrix GM = V(k,k)^-1 V(k,n) of RFC 5510 §8.2 formed with ISA-L's arithmetic and inversion;
//   * every repair symbol against each ISA-L encoder this CPU runs (isal_arms.h) over GM's repair columns;
//   * every recovered source against each of them over ISA-L's inverse of the received columns of GM.

#include <crd/containers/array.hpp>
#include <crd/hesap/comms/erasure.hpp>
#include <crd/hesap/comms/gf256.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include <erasure_code.h>

#include "isal_arms.h"

namespace hc = crd::hesap::comms;
using crd::u32;
using crd::u8;
using crd::usize;
using hc::ErasureStatus;
using hc::ErasureSymbol;
using hc::ReedSolomonCode;
using Bytes = crd::containers::Array<u8>;

namespace
{

struct Shape
{
    u32 k;
    u32 n;
};

constexpr Shape kShapes[] = {{1U, 1U},    {1U, 2U},    {2U, 3U},     {3U, 7U},     {10U, 14U},   {16U, 24U},
                             {32U, 48U},  {64U, 96U},  {100U, 200U}, {128U, 255U}, {200U, 255U}, {254U, 255U},
                             {255U, 255U}};
constexpr usize kLengths[] = {1U, 15U, 64U, 1000U, 1500U};

Bytes random_bytes(crd::hesap::stats::PhiloxRng& rng, usize n)
{
    Bytes out;
    out.resize(n);
    for (usize i = 0U; i < n; ++i)
    {
        out[i] = static_cast<u8>(rng.next_u32());
    }
    return out;
}

// alpha^e with ISA-L's multiply: its 255 powers, formed once.
u8 isal_alpha_pow(u32 e)
{
    struct Powers
    {
        u8 of[255];
    };
    static const Powers powers = []()
    {
        Powers p{};
        p.of[0] = 1U;
        for (u32 i = 1U; i < 255U; ++i)
        {
            p.of[i] = gf_mul(p.of[i - 1U], 2U);
        }
        return p;
    }();
    return powers.of[e % 255U];
}

// The number of ISA-L encoders this CPU runs whose product of `rows` (row-major, rows x count) over `inputs` differs
// from `expected` (row_count outputs of `bytes`); each such arm is named with its first differing output.
usize isal_mismatches(const Bytes& rows, usize row_count, usize count, unsigned char** inputs,
                      const crd::containers::Array<Bytes>& expected, usize bytes)
{
    Bytes tables;
    tables.resize(32U * row_count * count);
    crd::containers::Array<Bytes> theirs;
    crd::containers::Array<unsigned char*> outputs;
    for (usize q = 0U; q < row_count; ++q)
    {
        theirs.push_back(Bytes{});
        theirs.back().resize(bytes);
    }
    for (usize q = 0U; q < row_count; ++q)
    {
        outputs.push_back(theirs[q].data());
    }
    usize bad = 0U;
    for (unsigned a = 0U; a < crd_isal_arm_count; ++a)
    {
        const crd_isal_arm& arm = crd_isal_arms[a];
        if (!crd_isal_arm_runs(arm))
        {
            continue;
        }
        Bytes matrix = rows;
        arm.tables(static_cast<int>(count), static_cast<int>(row_count), matrix.data(), tables.data());
        arm.encode(static_cast<int>(bytes), static_cast<int>(count), static_cast<int>(row_count), tables.data(),
                   inputs, outputs.data());
        for (usize q = 0U; q < row_count; ++q)
        {
            if (std::memcmp(theirs[q].data(), expected[q].data(), bytes) != 0)
            {
                UNSCOPED_INFO("ISA-L arm " << arm.name << " differs at output " << q);
                ++bad;
                break;
            }
        }
    }
    return bad;
}

} // namespace

TEST_CASE("ISA-L oracle: the peer holds its portable arm and, with its assembly, every ISA body", "[erasure][isal]")
{
    CHECK(crd_isal_arm_count == (CRD_ISAL_ASSEMBLY != 0 ? 8U : 1U));
    CHECK(crd_isal_arm_runs(crd_isal_arms[0]));
    CHECK(crd_isal_arm_runs(crd_isal_arms[crd_isal_arm_count - 1U]));
}

TEST_CASE("ISA-L oracle: the field's products and inverses are ISA-L's", "[erasure][isal]")
{
    usize bad = 0U;
    for (u32 a = 0U; a < 256U; ++a)
    {
        for (u32 b = 0U; b < 256U; ++b)
        {
            const u8 x = static_cast<u8>(a);
            const u8 y = static_cast<u8>(b);
            bad += hc::gf256_mul(x, y) != gf_mul(x, y) ? 1U : 0U;
        }
        if (a != 0U)
        {
            bad += hc::gf256_inv(static_cast<u8>(a)) != gf_inv(static_cast<u8>(a)) ? 1U : 0U;
        }
    }
    CHECK(bad == 0U);
}

TEST_CASE("ISA-L oracle: GM is V(k,k)^-1 V(k,n) under ISA-L's arithmetic and inversion", "[erasure][isal]")
{
    for (const Shape shape : kShapes)
    {
        INFO("k " << shape.k << " n " << shape.n);
        const u32 k = shape.k;
        const u32 n = shape.n;
        Bytes vkk;
        vkk.resize(usize{k} * k);
        for (u32 i = 0U; i < k; ++i)
        {
            for (u32 j = 0U; j < k; ++j)
            {
                vkk[(usize{i} * k) + j] = isal_alpha_pow(i * j);
            }
        }
        Bytes inverse;
        inverse.resize(usize{k} * k);
        REQUIRE(gf_invert_matrix(vkk.data(), inverse.data(), static_cast<int>(k)) == 0);
        const ReedSolomonCode code(k, n);
        usize bad = 0U;
        for (u32 i = 0U; i < k; ++i)
        {
            for (u32 j = 0U; j < n; ++j)
            {
                u8 entry = 0U;
                for (u32 m = 0U; m < k; ++m)
                {
                    entry = static_cast<u8>(entry ^ gf_mul(inverse[(usize{i} * k) + m], isal_alpha_pow(m * j)));
                }
                bad += entry != code.coefficient(i, j) ? 1U : 0U;
            }
        }
        CHECK(bad == 0U);
    }
}

TEST_CASE("ISA-L oracle: repair symbols are ec_encode_data_base over GM's repair columns", "[erasure][isal]")
{
    crd::hesap::stats::PhiloxRng rng(2026, 21);
    for (const Shape shape : kShapes)
    {
        const u32 k = shape.k;
        const u32 n = shape.n;
        const u32 r = n - k;
        if (r == 0U)
        {
            continue;
        }
        const ReedSolomonCode code(k, n);
        Bytes rows;
        rows.resize(usize{r} * k);
        for (u32 l = 0U; l < r; ++l)
        {
            for (u32 i = 0U; i < k; ++i)
            {
                rows[(usize{l} * k) + i] = code.coefficient(i, k + l);
            }
        }
        for (const usize bytes : kLengths)
        {
            INFO("k " << k << " n " << n << " bytes " << bytes);
            crd::containers::Array<Bytes> sources;
            crd::containers::Array<Bytes> ours;
            crd::containers::Array<const u8*> source_ptrs;
            crd::containers::Array<unsigned char*> isal_sources;
            crd::containers::Array<u8*> our_ptrs;
            crd::containers::Array<u32> repair;
            for (u32 i = 0U; i < k; ++i)
            {
                sources.push_back(random_bytes(rng, bytes));
            }
            for (u32 l = 0U; l < r; ++l)
            {
                ours.push_back(Bytes{});
                ours.back().resize(bytes);
                repair.push_back(k + l);
            }
            for (u32 i = 0U; i < k; ++i)
            {
                source_ptrs.push_back(sources[i].data());
                isal_sources.push_back(sources[i].data());
            }
            for (u32 l = 0U; l < r; ++l)
            {
                our_ptrs.push_back(ours[l].data());
            }
            REQUIRE(code.encode({source_ptrs.data(), source_ptrs.size()}, {repair.data(), repair.size()},
                                {our_ptrs.data(), our_ptrs.size()}, bytes) == ErasureStatus::Ok);
            CHECK(isal_mismatches(rows, r, k, isal_sources.data(), ours, bytes) == 0U);
        }
    }
}

TEST_CASE("ISA-L oracle: recovered sources are ISA-L's inverse of the received columns of GM", "[erasure][isal]")
{
    crd::hesap::stats::PhiloxRng rng(2026, 22);
    constexpr usize kBytes = 333U;
    for (const Shape shape : kShapes)
    {
        const u32 k = shape.k;
        const u32 n = shape.n;
        const ReedSolomonCode code(k, n);
        // The whole block: the sources, then every repair symbol.
        crd::containers::Array<Bytes> block;
        for (u32 j = 0U; j < n; ++j)
        {
            block.push_back(j < k ? random_bytes(rng, kBytes) : Bytes{});
            block.back().resize(kBytes);
        }
        crd::containers::Array<const u8*> source_ptrs;
        crd::containers::Array<u8*> repair_ptrs;
        crd::containers::Array<u32> repair;
        for (u32 j = 0U; j < n; ++j)
        {
            if (j < k)
            {
                source_ptrs.push_back(block[j].data());
            }
            else
            {
                repair_ptrs.push_back(block[j].data());
                repair.push_back(j);
            }
        }
        REQUIRE(code.encode({source_ptrs.data(), source_ptrs.size()}, {repair.data(), repair.size()},
                            {repair_ptrs.data(), repair_ptrs.size()}, kBytes) == ErasureStatus::Ok);
        for (u32 pattern = 0U; pattern < 8U; ++pattern)
        {
            INFO("k " << k << " n " << n << " pattern " << pattern);
            // k distinct indices of [0, n) by a seeded shuffle; the pattern 0 keeps the repair symbols first.
            crd::containers::Array<u32> order;
            for (u32 j = 0U; j < n; ++j)
            {
                order.push_back(pattern == 0U ? n - 1U - j : j);
            }
            for (u32 j = n - 1U; pattern != 0U && j > 0U; --j)
            {
                const u32 pick = rng.next_u32() % (j + 1U);
                const u32 held = order[j];
                order[j] = order[pick];
                order[pick] = held;
            }
            crd::containers::Array<ErasureSymbol> received;
            crd::containers::Array<unsigned char*> isal_inputs;
            bool have[ReedSolomonCode::kMaxSymbols] = {};
            for (u32 t = 0U; t < k; ++t)
            {
                received.push_back(ErasureSymbol{order[t], block[order[t]].data()});
                isal_inputs.push_back(block[order[t]].data());
                have[order[t]] = true;
            }
            crd::containers::Array<u32> lost;
            for (u32 i = 0U; i < k; ++i)
            {
                if (!have[i])
                {
                    lost.push_back(i);
                }
            }
            if (lost.empty())
            {
                continue;
            }
            // B[t][i] = GM[i][order[t]], so the received symbols are B s; the lost sources are rows of B^-1.
            Bytes b;
            b.resize(usize{k} * k);
            for (u32 t = 0U; t < k; ++t)
            {
                for (u32 i = 0U; i < k; ++i)
                {
                    b[(usize{t} * k) + i] = code.coefficient(i, order[t]);
                }
            }
            Bytes inverse;
            inverse.resize(usize{k} * k);
            REQUIRE(gf_invert_matrix(b.data(), inverse.data(), static_cast<int>(k)) == 0);
            Bytes rows;
            rows.resize(lost.size() * k);
            for (usize q = 0U; q < lost.size(); ++q)
            {
                std::memcpy(rows.data() + (q * k), inverse.data() + (usize{lost[q]} * k), k);
            }
            crd::containers::Array<Bytes> ours;
            crd::containers::Array<u8*> our_ptrs;
            for (usize q = 0U; q < lost.size(); ++q)
            {
                ours.push_back(Bytes{});
                ours.back().resize(kBytes);
            }
            for (usize q = 0U; q < lost.size(); ++q)
            {
                our_ptrs.push_back(ours[q].data());
            }
            REQUIRE(code.decode({received.data(), received.size()}, {lost.data(), lost.size()},
                                {our_ptrs.data(), our_ptrs.size()}, kBytes) == ErasureStatus::Ok);
            usize bad = isal_mismatches(rows, lost.size(), k, isal_inputs.data(), ours, kBytes);
            for (usize q = 0U; q < lost.size(); ++q)
            {
                bad += std::memcmp(ours[q].data(), block[lost[q]].data(), kBytes) != 0 ? 1U : 0U;
            }
            CHECK(bad == 0U);
        }
    }
}
