// hesap-net's RaptorQ code (HESAP-NET; crd/hesap/net/raptorq.hpp) against nanorq and the raptorq crate
// (CRD_BUILD_RAPTORQ_VS_PEERS): every parameter and tuple, every repair symbol byte for byte, each decoder on the
// others' symbols, and one verdict on rows that do not suffice.

#include "raptorq_peers.h"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/raptorq.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <span>

namespace
{

using crd::u32;
using crd::u8;
using crd::usize;
using namespace crd::hesap::net;

template <typename T> using Array = crd::containers::Array<T>;
using Bytes = Array<u8>;

Bytes sized(usize size)
{
    Bytes bytes;
    bytes.resize(size);
    if (size != 0U)
    {
        std::memset(bytes.data(), 0, size);
    }
    return bytes;
}

Bytes random_block(SeededWords& words, u32 k, usize t)
{
    Bytes bytes = sized(usize{k} * t);
    for (usize i = 0U; i < bytes.size(); ++i)
    {
        bytes[i] = static_cast<u8>(words.next_u32() >> 24U);
    }
    return bytes;
}

Array<const u8*> symbols_of(const Bytes& bytes, usize t)
{
    Array<const u8*> symbols;
    for (usize i = 0U; i < bytes.size() / t; ++i)
    {
        symbols.push_back(bytes.data() + (i * t));
    }
    return symbols;
}

// Our plan with its memory.
struct Plan
{
    RaptorqSolver solver;
    Bytes structure;
    Bytes factors;

    RaptorqStatus encoder(u32 k)
    {
        structure = sized(RaptorqSolver::structure_bytes(k));
        const RaptorqStatus status = solver.analyze(k, structure);
        return status == RaptorqStatus::Ok ? finish() : status;
    }

    RaptorqStatus decoder(u32 k, std::span<const u32> esis)
    {
        structure = sized(RaptorqSolver::structure_bytes(k, esis));
        const RaptorqStatus status = solver.analyze(k, esis, structure);
        return status == RaptorqStatus::Ok ? finish() : status;
    }

    RaptorqStatus finish()
    {
        factors = sized(solver.factor_bytes());
        return solver.factor(factors);
    }

    Bytes solve(std::span<const u8* const> symbols, usize t) const
    {
        Bytes c = sized(usize{solver.parameters().l} * t);
        Bytes scratch = sized(solver.solve_scratch_bytes(t));
        REQUIRE(solver.solve(symbols, t, c, scratch) == RaptorqStatus::Ok);
        return c;
    }
};

Bytes our_intermediate(const Bytes& block, u32 k, usize t)
{
    Plan plan;
    REQUIRE(plan.encoder(k) == RaptorqStatus::Ok);
    const Array<const u8*> symbols = symbols_of(block, t);
    return plan.solve(symbols, t);
}

// The columns of Tuple[K', X], as §5.3.5.3 walks them.
void columns_of(const RaptorqParameters& p, u32 isi, Array<u32>& out)
{
    const RaptorqTuple tuple = raptorq_tuple(p, isi);
    u32 b = tuple.b;
    out.push_back(b);
    for (u32 j = 1U; j < tuple.d; ++j)
    {
        b = (b + tuple.a) % p.w;
        out.push_back(b);
    }
    u32 b1 = tuple.b1;
    while (b1 >= p.p)
    {
        b1 = (b1 + tuple.a1) % p.p1;
    }
    out.push_back(p.w + b1);
    for (u32 j = 1U; j < tuple.d1; ++j)
    {
        b1 = (b1 + tuple.a1) % p.p1;
        while (b1 >= p.p)
        {
            b1 = (b1 + tuple.a1) % p.p1;
        }
        out.push_back(p.w + b1);
    }
}

// Our repair symbols ESI k + first .. against nanorq's and the crate's, byte for byte.
void check_repairs(const Bytes& block, u32 k, usize t, u32 first, u32 count)
{
    CAPTURE(k, t, first);
    RaptorqParameters p;
    REQUIRE(raptorq_parameters(k, p));
    const Bytes c = our_intermediate(block, k, t);

    Bytes ours = sized(usize{count} * t);
    for (u32 i = 0U; i < count; ++i)
    {
        raptorq_symbol(p, c.data(), t, raptorq_isi(p, k, k + first + i), ours.data() + (usize{i} * t));
    }

    crd_nanorq_plan* const nanorq = crd_nanorq_plan_new(k, static_cast<u32>(t), nullptr, nullptr, 0U);
    REQUIRE(nanorq != nullptr);
    const Array<const u8*> rows = symbols_of(block, t);
    crd_nanorq_run(nanorq, rows.data());
    Bytes theirs = sized(usize{count} * t);
    for (u32 i = 0U; i < count; ++i)
    {
        crd_nanorq_symbol(nanorq, k + first + i, theirs.data() + (usize{i} * t));
    }
    crd_nanorq_plan_free(nanorq);
    CHECK(std::memcmp(ours.data(), theirs.data(), ours.size()) == 0);

    void* const crate_plan = crd_raptorq_crate_plan_new(k);
    crd_raptorq_crate_repair(crate_plan, block.data(), k, static_cast<u32>(t), first, count, theirs.data());
    crd_raptorq_crate_plan_free(crate_plan);
    CHECK(std::memcmp(ours.data(), theirs.data(), ours.size()) == 0);
}

} // namespace

TEST_CASE("raptorq vs nanorq: the parameters of every K' and of the K just above it", "[raptorq-peers]")
{
    for (const RaptorqSystematicRow& row : raptorq_systematic_rows())
    {
        for (const u32 k : {u32{row.k_prime}, u32{row.k_prime} + 1U})
        {
            if (k > kRaptorqMaxSourceSymbols)
            {
                continue;
            }
            RaptorqParameters p;
            REQUIRE(raptorq_parameters(k, p));
            u32 theirs[11];
            crd_nanorq_parameters(k, theirs);
            CAPTURE(k);
            CHECK(theirs[0] == k);
            CHECK(theirs[1] == p.k_prime);
            CHECK(theirs[2] == p.s);
            CHECK(theirs[3] == p.h);
            CHECK(theirs[4] == p.w);
            CHECK(theirs[5] == p.l);
            CHECK(theirs[6] == p.p);
            CHECK(theirs[7] == p.p1);
            CHECK(theirs[8] == p.u);
            CHECK(theirs[9] == p.b);
            CHECK(theirs[10] == p.j);
        }
    }
}

TEST_CASE("raptorq vs nanorq: the tuples of every K' name the same intermediate symbols", "[raptorq-peers]")
{
    for (const RaptorqSystematicRow& row : raptorq_systematic_rows())
    {
        const u32 k = row.k_prime;
        RaptorqParameters p;
        REQUIRE(raptorq_parameters(k, p));
        for (const u32 esi : {0U, 1U, k - 1U, k, k + 1U, 12345U, kRaptorqEsiLimit - 1U})
        {
            u32 theirs[32];
            const u32 count = crd_nanorq_mix(k, esi, theirs);
            Array<u32> ours;
            columns_of(p, raptorq_isi(p, k, esi), ours);
            REQUIRE(ours.size() == count);
            std::sort(theirs, theirs + count);
            std::sort(ours.begin(), ours.end());
            CAPTURE(k, esi);
            CHECK(std::memcmp(ours.data(), theirs, count * sizeof(u32)) == 0);
        }
    }
}

TEST_CASE("raptorq vs nanorq: every K' of Table 2, its repair and source symbols byte for byte", "[raptorq-peers]")
{
    SeededWords words(0x6330U, 20U);
    const usize t = 4U;
    for (const RaptorqSystematicRow& row : raptorq_systematic_rows())
    {
        const u32 k = row.k_prime;
        CAPTURE(k);
        RaptorqParameters p;
        REQUIRE(raptorq_parameters(k, p));
        const Bytes block = random_block(words, k, t);
        const Bytes c = our_intermediate(block, k, t);

        crd_nanorq_plan* const nanorq = crd_nanorq_plan_new(k, static_cast<u32>(t), nullptr, nullptr, 0U);
        REQUIRE(nanorq != nullptr);
        const Array<const u8*> rows = symbols_of(block, t);
        crd_nanorq_run(nanorq, rows.data());
        u8 ours[4];
        u8 theirs[4];
        bool same = true;
        for (u32 esi = 0U; esi < k + 32U; esi = esi < 8U || esi + 40U >= k ? esi + 1U : k - 8U)
        {
            raptorq_symbol(p, c.data(), t, raptorq_isi(p, k, esi), ours);
            crd_nanorq_symbol(nanorq, esi, theirs);
            same = same && std::memcmp(ours, theirs, t) == 0;
        }
        crd_nanorq_plan_free(nanorq);
        CHECK(same);
    }
}

TEST_CASE("raptorq vs both: short blocks, large blocks and the last ESIs, byte for byte", "[raptorq-peers]")
{
    SeededWords words(0x6330U, 21U);
    for (const u32 k : {1U, 2U, 9U, 10U, 11U, 12U, 13U, 48U, 49U, 100U, 101U, 999U, 1000U, 5000U, 20000U, 56403U})
    {
        const usize t = k <= 1000U ? 1280U : 16U;
        const Bytes block = random_block(words, k, t);
        check_repairs(block, k, t, 0U, 64U);
        check_repairs(block, k, t, kRaptorqEsiLimit - k - 64U, 64U);
    }
    for (const usize t : {1U, 2U, 3U, 7U, 31U, 64U, 65U, 1000U, 65535U})
    {
        const Bytes block = random_block(words, 13U, t);
        check_repairs(block, 13U, t, 5U, 8U);
    }
}

TEST_CASE("raptorq vs both: each decoder recovers a block from another's symbols", "[raptorq-peers]")
{
    SeededWords words(0x6330U, 22U);
    for (const u32 k : {10U, 13U, 101U, 1000U, 10000U})
    {
        CAPTURE(k);
        const usize t = k <= 1000U ? 256U : 16U;
        RaptorqParameters p;
        REQUIRE(raptorq_parameters(k, p));
        const Bytes block = random_block(words, k, t);
        const Array<const u8*> sources = symbols_of(block, t);

        // A quarter of the sources lost, each replaced by a repair symbol from the crate.
        const u32 lost = (k + 3U) / 4U;
        void* const crate_plan = crd_raptorq_crate_plan_new(k);
        const u32 spare = 16U;
        Bytes repair = sized(usize{lost + spare} * t);
        crd_raptorq_crate_repair(crate_plan, block.data(), k, static_cast<u32>(t), 0U, lost + spare, repair.data());
        crd_raptorq_crate_plan_free(crate_plan);

        u32 used = lost;
        Plan ours;
        Array<u32> esis;
        Array<const u8*> received;
        RaptorqStatus status = RaptorqStatus::Singular;
        while (status == RaptorqStatus::Singular)
        {
            esis.clear();
            received.clear();
            for (u32 i = lost; i < k; ++i)
            {
                esis.push_back(i);
                received.push_back(sources[i]);
            }
            for (u32 i = 0U; i < used; ++i)
            {
                esis.push_back(k + i);
                received.push_back(repair.data() + (usize{i} * t));
            }
            status = ours.decoder(k, esis);
            used += 1U;
        }
        REQUIRE(status == RaptorqStatus::Ok);

        // Ours on the crate's symbols.
        const Bytes c = ours.solve(received, t);
        Bytes symbol = sized(t);
        bool same = true;
        for (u32 x = 0U; x < lost; ++x)
        {
            raptorq_symbol(p, c.data(), t, x, symbol.data());
            same = same && std::memcmp(symbol.data(), sources[x], t) == 0;
        }
        CHECK(same);

        // The crate's on the same symbols, written by ours.
        Bytes written = sized(esis.size() * t);
        Array<const u8*> pointers;
        for (usize i = 0U; i < esis.size(); ++i)
        {
            u8* const out = written.data() + (i * t);
            raptorq_symbol(p, c.data(), t, raptorq_isi(p, k, esis[i]), out);
            pointers.push_back(out);
        }
        Bytes decoded = sized(usize{k} * t);
        REQUIRE(crd_raptorq_crate_decode(k, static_cast<u32>(t), esis.data(), pointers.data(),
                                         static_cast<u32>(esis.size()), decoded.data()));
        CHECK(std::memcmp(decoded.data(), block.data(), block.size()) == 0);

        // nanorq's: it takes exactly K rows, each lost one replaced in place.
        const u32 replaced = lost < esis.size() - (k - lost) ? lost : static_cast<u32>(esis.size() - (k - lost));
        Array<u32> rows;
        Array<u32> repair_esis;
        Array<const u8*> row_symbols;
        for (u32 x = 0U; x < k; ++x)
        {
            row_symbols.push_back(sources[x]);
        }
        for (u32 x = 0U; x < replaced; ++x)
        {
            rows.push_back(x);
            repair_esis.push_back(k + x);
            row_symbols[x] = repair.data() + (usize{x} * t);
        }
        crd_nanorq_plan* const nanorq =
            crd_nanorq_plan_new(k, static_cast<u32>(t), rows.data(), repair_esis.data(), replaced);
        if (nanorq != nullptr)
        {
            crd_nanorq_run(nanorq, row_symbols.data());
            same = true;
            for (u32 x = 0U; x < replaced; ++x)
            {
                crd_nanorq_symbol(nanorq, x, symbol.data());
                same = same && std::memcmp(symbol.data(), sources[x], t) == 0;
            }
            crd_nanorq_plan_free(nanorq);
            CHECK(same);
        }
    }
}

TEST_CASE("raptorq vs both: one verdict on every set of K rows", "[raptorq-peers]")
{
    // About 1 set in 250 is singular: all three recover exactly from the sets that suffice.
    SeededWords words(0x6330U, 23U);
    const u32 k = 10U;
    const usize t = 4U;
    const Bytes block = random_block(words, k, t);
    const Array<const u8*> sources = symbols_of(block, t);
    RaptorqParameters p;
    REQUIRE(raptorq_parameters(k, p));
    const Bytes c = our_intermediate(block, k, t);

    u32 singular = 0U;
    for (u32 trial = 0U; trial < 3000U; ++trial)
    {
        const u32 lost = 1U + (words.next_u32() % k);
        const u32 base = k + (words.next_u32() % 100000U);
        Array<u32> esis;
        Array<u32> rows;
        Array<u32> repair_esis;
        Bytes repair = sized(usize{lost} * t);
        for (u32 x = lost; x < k; ++x)
        {
            esis.push_back(x);
        }
        for (u32 x = 0U; x < lost; ++x)
        {
            esis.push_back(base + x);
            rows.push_back(x);
            repair_esis.push_back(base + x);
            raptorq_symbol(p, c.data(), t, raptorq_isi(p, k, base + x), repair.data() + (usize{x} * t));
        }

        Plan ours;
        const RaptorqStatus status = ours.decoder(k, esis);
        REQUIRE((status == RaptorqStatus::Ok || status == RaptorqStatus::Singular));
        singular += status == RaptorqStatus::Singular ? 1U : 0U;

        crd_nanorq_plan* const nanorq =
            crd_nanorq_plan_new(k, static_cast<u32>(t), rows.data(), repair_esis.data(), lost);
        CHECK((nanorq != nullptr) == (status == RaptorqStatus::Ok));
        crd_nanorq_plan_free(nanorq);

        Array<const u8*> pointers;
        for (u32 x = lost; x < k; ++x)
        {
            pointers.push_back(sources[x]);
        }
        for (u32 x = 0U; x < lost; ++x)
        {
            pointers.push_back(repair.data() + (usize{x} * t));
        }
        Bytes decoded = sized(usize{k} * t);
        const bool recovered = crd_raptorq_crate_decode(k, static_cast<u32>(t), esis.data(), pointers.data(),
                                                        static_cast<u32>(esis.size()), decoded.data());
        CHECK(recovered == (status == RaptorqStatus::Ok));
        if (recovered)
        {
            CHECK(std::memcmp(decoded.data(), block.data(), block.size()) == 0);
        }
    }
    CHECK(singular > 0U);
}
