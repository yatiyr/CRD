// HESAP-NET's Reed-Solomon code against OpenFEC, the codec of RFC 5510's authors (CRD_BUILD_ERASURE_VS_OPENFEC;
// cmake/pins.json openfec, compiled as crd-openfec-peer).
//
// OpenFEC does not build RFC 5510 §8.2's matrix. Its first row is (1, 0, ..., 0), so its code evaluates at the points
// {0, alpha^0, alpha^1, ...} where §8.2's v_ij = alpha^(i*j) evaluates at {alpha^0, alpha^1, ...}: for k > 1 the two
// differ in every block, and hesap keeps the RFC's. Its fast Vandermonde inversion needs that zero point (P(x) starts
// from x - 0), so §8.2's matrix goes through its Gauss-Jordan inversion below. What the two share is held byte for
// byte:
//   * every product and inverse of OpenFEC's tables;
//   * §8.2's GM formed from the RFC's Vandermonde matrix by OpenFEC's Gauss-Jordan inversion and multiplication;
//   * hesap's recovered sources against OpenFEC's matrix inversion of the received columns of GM.
// The difference is held too: the real codec's repair symbols are the Lagrange form of its own point set, computed here
// with hesap's field, and the real decoder recovers from them, so the difference is the points and not a misuse.

#include <crd/containers/array.hpp>
#include <crd/hesap/comms/erasure.hpp>
#include <crd/hesap/comms/gf256.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "openfec_peer.h"

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

constexpr Shape kShapes[] = {{2U, 3U},   {3U, 7U},     {10U, 14U},   {16U, 24U},   {32U, 48U},
                             {64U, 96U}, {100U, 200U}, {128U, 255U}, {200U, 255U}, {254U, 255U}};

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

// OpenFEC's point of symbol j: 0, then alpha^(j - 1).
u8 openfec_point(u32 j)
{
    return j == 0U ? u8{0U} : hc::gf256_alpha_pow(j - 1U);
}

// The Lagrange basis polynomial of source i over OpenFEC's first k points, at symbol j's point: its generator entry.
u8 openfec_coefficient(u32 k, u32 i, u32 j)
{
    u8 numerator = 1U;
    u8 denominator = 1U;
    for (u32 m = 0U; m < k; ++m)
    {
        if (m != i)
        {
            numerator = hc::gf256_mul(numerator, openfec_point(j) ^ openfec_point(m));
            denominator = hc::gf256_mul(denominator, openfec_point(i) ^ openfec_point(m));
        }
    }
    return hc::gf256_div(numerator, denominator);
}

} // namespace

TEST_CASE("OpenFEC oracle: the field's products and inverses are OpenFEC's", "[erasure][openfec]")
{
    usize differing = 0U;
    for (u32 a = 0U; a < 256U; ++a)
    {
        for (u32 b = 0U; b < 256U; ++b)
        {
            differing += hc::gf256_mul(static_cast<u8>(a), static_cast<u8>(b))
                                 == crd_openfec_mul(static_cast<u8>(a), static_cast<u8>(b))
                             ? 0U
                             : 1U;
        }
        if (a != 0U)
        {
            differing += hc::gf256_inv(static_cast<u8>(a)) == crd_openfec_inv(static_cast<u8>(a)) ? 0U : 1U;
        }
        differing += hc::gf256_alpha_pow(a) == crd_openfec_alpha_pow(a) ? 0U : 1U;
    }
    CHECK(differing == 0U);
}

TEST_CASE("OpenFEC oracle: RFC 5510's GM under OpenFEC's inversion and multiplication", "[erasure][openfec]")
{
    for (const Shape shape : kShapes)
    {
        const u32 k = shape.k;
        const u32 r = shape.n - k;
        INFO("k " << k << " n " << shape.n);
        // Rows are points and columns degrees, the transpose of §8.2's V, as OpenFEC's code multiplies.
        Bytes inverse;
        Bytes lower;
        Bytes product;
        inverse.resize(usize{k} * k);
        lower.resize(usize{r} * k);
        product.resize(usize{r} * k);
        for (u32 j = 0U; j < shape.n; ++j)
        {
            for (u32 i = 0U; i < k; ++i)
            {
                const u8 entry = crd_openfec_alpha_pow(i * j);
                if (j < k)
                {
                    inverse[(usize{j} * k) + i] = entry;
                }
                else
                {
                    lower[(usize{j - k} * k) + i] = entry;
                }
            }
        }
        REQUIRE(crd_openfec_invert(inverse.data(), k) == 0);
        crd_openfec_matmul(lower.data(), inverse.data(), product.data(), r, k, k);
        const ReedSolomonCode code(k, shape.n);
        usize differing = 0U;
        for (u32 j = k; j < shape.n; ++j)
        {
            for (u32 i = 0U; i < k; ++i)
            {
                differing += code.coefficient(i, j) == product[(usize{j - k} * k) + i] ? 0U : 1U;
            }
        }
        CHECK(differing == 0U);
    }
}

TEST_CASE("OpenFEC oracle: recovered sources are OpenFEC's inverse of the received columns of GM", "[erasure][openfec]")
{
    crd::hesap::stats::PhiloxRng rng(2026, 31);
    constexpr usize kBytes = 333U;
    for (const Shape shape : kShapes)
    {
        const u32 k = shape.k;
        const u32 n = shape.n;
        const ReedSolomonCode code(k, n);
        crd::containers::Array<Bytes> block;
        crd::containers::Array<const u8*> source_ptrs;
        crd::containers::Array<u8*> repair_ptrs;
        crd::containers::Array<u32> repair;
        for (u32 j = 0U; j < n; ++j)
        {
            block.push_back(j < k ? random_bytes(rng, kBytes) : Bytes{});
            block.back().resize(kBytes);
        }
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
        for (u32 pattern = 0U; pattern < 6U; ++pattern)
        {
            INFO("k " << k << " n " << n << " pattern " << pattern);
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
            Bytes matrix;
            Bytes bytes;
            Bytes theirs;
            bool have[ReedSolomonCode::kMaxSymbols] = {};
            matrix.resize(usize{k} * k);
            bytes.resize(usize{k} * kBytes);
            theirs.resize(usize{k} * kBytes);
            for (u32 t = 0U; t < k; ++t)
            {
                const u32 j = order[t];
                received.push_back(ErasureSymbol{j, block[j].data()});
                have[j] = true;
                std::memcpy(bytes.data() + (usize{t} * kBytes), block[j].data(), kBytes);
                for (u32 i = 0U; i < k; ++i)
                {
                    matrix[(usize{t} * k) + i] = code.coefficient(i, j);
                }
            }
            REQUIRE(crd_openfec_invert(matrix.data(), k) == 0);
            crd_openfec_matmul(matrix.data(), bytes.data(), theirs.data(), k, k, kBytes);
            crd::containers::Array<u32> lost;
            crd::containers::Array<Bytes> ours;
            crd::containers::Array<u8*> our_ptrs;
            for (u32 i = 0U; i < k; ++i)
            {
                if (!have[i])
                {
                    lost.push_back(i);
                    ours.push_back(Bytes{});
                    ours.back().resize(kBytes);
                }
            }
            for (usize q = 0U; q < ours.size(); ++q)
            {
                our_ptrs.push_back(ours[q].data());
            }
            REQUIRE(code.decode({received.data(), received.size()}, {lost.data(), lost.size()},
                                {our_ptrs.data(), our_ptrs.size()}, kBytes) == ErasureStatus::Ok);
            usize differing = 0U;
            for (usize q = 0U; q < lost.size(); ++q)
            {
                const u8* expected = theirs.data() + (usize{lost[q]} * kBytes);
                differing += std::memcmp(ours[q].data(), expected, kBytes) == 0 ? 0U : 1U;
            }
            CHECK(differing == 0U);
        }
    }
}

TEST_CASE("OpenFEC oracle: the real codec evaluates at {0, alpha^i}, not at RFC 5510's points", "[erasure][openfec]")
{
    crd::hesap::stats::PhiloxRng rng(2026, 32);
    constexpr usize kBytes = 200U;
    for (const Shape shape : kShapes)
    {
        const u32 k = shape.k;
        const u32 n = shape.n;
        INFO("k " << k << " n " << n);
        const ReedSolomonCode code(k, n);
        crd::containers::Array<Bytes> sources;
        crd::containers::Array<u8*> source_ptrs;
        crd::containers::Array<Bytes> repair;
        for (u32 i = 0U; i < k; ++i)
        {
            sources.push_back(random_bytes(rng, kBytes));
        }
        for (u32 i = 0U; i < k; ++i)
        {
            source_ptrs.push_back(sources[i].data());
        }
        for (u32 j = k; j < n; ++j)
        {
            repair.push_back(Bytes{});
            repair.back().resize(kBytes);
        }
        usize rfc_differs = 0U;
        for (u32 j = k; j < n; ++j)
        {
            Bytes& out = repair[j - k];
            REQUIRE(crd_openfec_encode(k, n, kBytes, source_ptrs.data(), j, out.data()) == 0);
            Bytes row;
            Bytes rfc_row;
            row.resize(k);
            rfc_row.resize(k);
            for (u32 i = 0U; i < k; ++i)
            {
                row[i] = openfec_coefficient(k, i, j);
                rfc_row[i] = code.coefficient(i, j);
            }
            usize differing = 0U;
            for (usize b = 0U; b < kBytes; ++b)
            {
                u8 lagrange = 0U;
                u8 rfc = 0U;
                for (u32 i = 0U; i < k; ++i)
                {
                    lagrange ^= hc::gf256_mul(row[i], sources[i][b]);
                    rfc ^= hc::gf256_mul(rfc_row[i], sources[i][b]);
                }
                differing += lagrange == out[b] ? 0U : 1U;
                rfc_differs += rfc == out[b] ? 0U : 1U;
            }
            CHECK(differing == 0U);
        }
        CHECK(rfc_differs > 0U);

        // The real decoder: every repair symbol it can use replaces a source, and the lost sources come back.
        const u32 used = n - k < k ? n - k : k;
        crd::containers::Array<Bytes> decoded;
        crd::containers::Array<u8*> outputs;
        crd::containers::Array<u8*> symbols;
        for (u32 i = 0U; i < k; ++i)
        {
            decoded.push_back(Bytes{});
            decoded.back().resize(kBytes);
        }
        for (u32 i = 0U; i < k; ++i)
        {
            outputs.push_back(decoded[i].data());
        }
        for (u32 j = 0U; j < n; ++j)
        {
            u8* symbol = nullptr;
            if (j < k && j >= used)
            {
                symbol = sources[j].data();
            }
            else if (j >= k && j - k < used)
            {
                symbol = repair[j - k].data();
            }
            symbols.push_back(symbol);
        }
        REQUIRE(crd_openfec_decode(k, n, kBytes, symbols.data(), outputs.data()) == 0);
        usize wrong = 0U;
        for (u32 i = 0U; i < used; ++i)
        {
            wrong += std::memcmp(decoded[i].data(), sources[i].data(), kBytes) == 0 ? 0U : 1U;
        }
        CHECK(wrong == 0U);
    }
}

TEST_CASE("OpenFEC oracle: the smallest block shows the two matrices side by side", "[erasure][openfec]")
{
    // k = 2, n = 3: OpenFEC's repair row is (3, 2), RFC 5510 §8.2's (2, 3).
    const ReedSolomonCode code(2U, 3U);
    CHECK(code.coefficient(0U, 2U) == 2U);
    CHECK(code.coefficient(1U, 2U) == 3U);
    CHECK(openfec_coefficient(2U, 0U, 2U) == 3U);
    CHECK(openfec_coefficient(2U, 1U, 2U) == 2U);
}
