// RFC 6330's RaptorQ code (HESAP-NET; crd/hesap/net/raptorq.hpp), held to the RFC's literal matrix A of §5.3.3.4.2 for
// its answers and its rank, to the relations of §5.3.3.3, to §5.8's recovery properties, and to §3 and §4's object
// layer by hand. The bit-exact oracles against other implementations are test_raptorq_peers.cpp's.

#include "raptorq_literal.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/gf256.hpp>
#include <crd/hesap/net/raptorq.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <span>
#include <thread>

namespace
{

using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::hesap::net;

template <typename T> using Array = crd::containers::Array<T>;
using Bytes = Array<u8>;

Bytes filled(usize size, u8 value)
{
    Bytes bytes;
    bytes.resize(size);
    if (size != 0U)
    {
        std::memset(bytes.data(), value, size);
    }
    return bytes;
}

Bytes zeros(usize size)
{
    return filled(size, 0U);
}

Bytes random_bytes(SeededWords& words, usize size)
{
    Bytes bytes = zeros(size);
    for (usize i = 0U; i < size; ++i)
    {
        bytes[i] = static_cast<u8>(words.next_u32() >> 24U);
    }
    return bytes;
}

u64 digest_of(const u8* data, usize size)
{
    u64 hash = 0xCBF29CE484222325ULL;
    for (usize i = 0U; i < size; ++i)
    {
        hash = (hash ^ data[i]) * 0x100000001B3ULL;
    }
    return hash;
}

bool is_prime(u32 n)
{
    if (n < 2U)
    {
        return false;
    }
    for (u32 f = 2U; f * f <= n; ++f)
    {
        if (n % f == 0U)
        {
            return false;
        }
    }
    return true;
}

// A solver with its memory.
struct Plan
{
    RaptorqSolver solver;
    Bytes structure;
    Bytes factors;

    RaptorqStatus encoder(u32 k)
    {
        structure = zeros(RaptorqSolver::structure_bytes(k));
        const RaptorqStatus status = solver.analyze(k, structure);
        return status == RaptorqStatus::Ok ? finish() : status;
    }

    RaptorqStatus decoder(u32 k, std::span<const u32> esis)
    {
        structure = zeros(RaptorqSolver::structure_bytes(k, esis));
        const RaptorqStatus status = solver.analyze(k, esis, structure);
        return status == RaptorqStatus::Ok ? finish() : status;
    }

    RaptorqStatus finish()
    {
        factors = zeros(solver.factor_bytes());
        return solver.factor(factors);
    }

    RaptorqStatus solve(std::span<const u8* const> symbols, usize bytes, Bytes& intermediate) const
    {
        // Both start as garbage: solve owes nothing to memory a caller reuses.
        intermediate = filled(usize{solver.parameters().l} * bytes, 0xA5U);
        Bytes scratch = filled(solver.solve_scratch_bytes(bytes), 0x5AU);
        return solver.solve(symbols, bytes, intermediate, scratch);
    }
};

constexpr u8 kGuard = 0xA5U;

// Whether every byte of `bytes` from `first` on still holds the guard.
bool guarded(const Bytes& bytes, usize first)
{
    for (usize i = first; i < bytes.size(); ++i)
    {
        if (bytes[i] != kGuard)
        {
            return false;
        }
    }
    return true;
}

// The K source symbols of a block, and their pointers.
struct Source
{
    Bytes bytes;
    Array<const u8*> symbols;

    Source(SeededWords& words, u32 k, usize t) : bytes(random_bytes(words, usize{k} * t))
    {
        for (u32 i = 0U; i < k; ++i)
        {
            symbols.push_back(bytes.data() + (usize{i} * t));
        }
    }
};

// Whether A·C holds the padded right side: zeros, then `symbols` by row (null is a zero symbol).
bool satisfies(const RaptorqParameters& p, const Bytes& a, std::span<const u8* const> symbols, const Bytes& c, usize t)
{
    const usize constraints = usize{p.s} + p.h;
    const usize rows = a.size() / p.l;
    for (usize row = 0U; row < rows; ++row)
    {
        const u8* const right =
            row >= constraints && row - constraints < symbols.size() ? symbols[row - constraints] : nullptr;
        for (usize i = 0U; i < t; ++i)
        {
            u8 sum = right != nullptr ? right[i] : u8{0U};
            for (u32 column = 0U; column < p.l; ++column)
            {
                sum ^= gf256_mul(a[(row * p.l) + column], c[(usize{column} * t) + i]);
            }
            if (sum != 0U)
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("raptorq: Table 2 is ascending and its derived parameters are section 5.3.3.3's", "[raptorq]")
{
    const auto rows = raptorq_systematic_rows();
    REQUIRE(rows.size() == 477U);
    CHECK(rows[0].k_prime == 10U);
    CHECK(rows[0].j == 254U);
    CHECK(rows[0].s == 7U);
    CHECK(rows[0].h == 10U);
    CHECK(rows[0].w == 17U);
    CHECK(rows[476].k_prime == kRaptorqMaxSourceSymbols);
    CHECK(rows[476].j == 471U);
    CHECK(rows[476].s == 907U);
    CHECK(rows[476].h == 16U);
    CHECK(rows[476].w == 56951U);

    u32 previous = 0U;
    for (const RaptorqSystematicRow& row : rows)
    {
        CHECK(row.k_prime > previous);
        CHECK(is_prime(row.s));
        CHECK(is_prime(row.w));
        CHECK(row.h <= 32U); // the solver's stack array

        RaptorqParameters p;
        REQUIRE(raptorq_parameters(row.k_prime, p));
        CHECK(p.k_prime == row.k_prime);
        CHECK(p.j == row.j);
        CHECK(p.l == p.k_prime + p.s + p.h);
        CHECK(p.l == p.w + p.p);
        CHECK(p.p >= p.h);
        CHECK(p.u == p.p - p.h);
        CHECK(p.b == p.w - p.s);
        CHECK(p.p1 >= p.p);
        CHECK(is_prime(p.p1));
        for (u32 n = p.p; n < p.p1; ++n)
        {
            CHECK_FALSE(is_prime(n));
        }

        // Every K between this K' and the one before it extends to this one.
        RaptorqParameters below;
        REQUIRE(raptorq_parameters(previous + 1U, below));
        CHECK(below.k_prime == row.k_prime);
        previous = row.k_prime;
    }

    RaptorqParameters untouched;
    untouched.k_prime = 77U;
    CHECK_FALSE(raptorq_parameters(0U, untouched));
    CHECK_FALSE(raptorq_parameters(kRaptorqMaxSourceSymbols + 1U, untouched));
    CHECK(untouched.k_prime == 77U);
}

TEST_CASE("raptorq: Rand reads the RFC's arrays through section 5.3.5.1's four indices", "[raptorq]")
{
    // V0[0] ^ V1[0] ^ V2[0] ^ V3[0], by hand from §5.5.
    CHECK(raptorq_rand(0U, 0U, 0xFFFFFFFFU) == (251291136U ^ 807385413U ^ 1629829892U ^ 1191369816U));
    CHECK(raptorq_rand(0x04030201U, 0U, 0xFFFFFFFFU) == 4080392487U);
    CHECK(raptorq_rand(0x04030201U, 5U, 1000U) == 473U);
    CHECK(raptorq_rand(0xFFFFFFFFU, 1U, 1U << 20U) == 145433U); // every index wraps modulo 2^8
    CHECK(raptorq_rand(0U, 0U, 1U) == 0U);
}

TEST_CASE("raptorq: Deg finds Table 1's interval and bounds it by W - 2", "[raptorq]")
{
    CHECK(raptorq_degree(0U, 56951U) == 1U);
    CHECK(raptorq_degree(5242U, 56951U) == 1U);
    CHECK(raptorq_degree(5243U, 56951U) == 2U);
    CHECK(raptorq_degree(529530U, 56951U) == 2U);
    CHECK(raptorq_degree(529531U, 56951U) == 3U);
    CHECK(raptorq_degree(1017661U, 56951U) == 29U);
    CHECK(raptorq_degree(1017662U, 56951U) == 30U);
    CHECK(raptorq_degree((1U << 20U) - 1U, 56951U) == 30U);
    CHECK(raptorq_degree((1U << 20U) - 1U, 17U) == 15U);
    CHECK(raptorq_degree(5243U, 4U) == 2U);
}

TEST_CASE("raptorq: Tuple follows section 5.3.5.4 and its ranges", "[raptorq]")
{
    for (const u32 k : {10U, 49U, 1000U, 56403U})
    {
        RaptorqParameters p;
        REQUIRE(raptorq_parameters(k, p));
        u32 big_a = 53591U + (p.j * 997U);
        if (big_a % 2U == 0U)
        {
            big_a += 1U;
        }
        const u32 big_b = 10267U * (p.j + 1U);
        for (const u32 x : {0U, 1U, 9U, p.k_prime, kRaptorqEsiLimit - 1U, kRaptorqEsiLimit + p.k_prime})
        {
            const RaptorqTuple tuple = raptorq_tuple(p, x);
            const u32 y = big_b + (x * big_a);
            CHECK(tuple.d == raptorq_degree(raptorq_rand(y, 0U, 1U << 20U), p.w));
            CHECK(tuple.a == 1U + raptorq_rand(y, 1U, p.w - 1U));
            CHECK(tuple.b == raptorq_rand(y, 2U, p.w));
            CHECK(tuple.d1 == (tuple.d < 4U ? 2U + raptorq_rand(x, 3U, 2U) : 2U));
            CHECK(tuple.a1 == 1U + raptorq_rand(x, 4U, p.p1 - 1U));
            CHECK(tuple.b1 == raptorq_rand(x, 5U, p.p1));
            CHECK(tuple.a >= 1U);
            CHECK(tuple.a <= p.w - 1U);
            CHECK(tuple.b < p.w);
            CHECK((tuple.d1 == 2U || tuple.d1 == 3U));
            CHECK(tuple.a1 >= 1U);
            CHECK(tuple.b1 < p.p1);
        }
    }
}

TEST_CASE("raptorq: the encoder's symbols solve the literal matrix A", "[raptorq]")
{
    SeededWords words(0x6330U, 1U);
    for (const u32 k : {1U, 9U, 10U, 12U, 13U, 18U, 26U, 49U, 55U, 101U, 213U})
    {
        CAPTURE(k);
        const usize t = 16U;
        RaptorqParameters p;
        REQUIRE(raptorq_parameters(k, p));
        Source source(words, k, t);
        Plan plan;
        REQUIRE(plan.encoder(k) == RaptorqStatus::Ok);
        Bytes c;
        REQUIRE(plan.solve(source.symbols, t, c) == RaptorqStatus::Ok);

        Array<u32> isis;
        for (u32 i = 0U; i < p.k_prime; ++i)
        {
            isis.push_back(i);
        }
        const Bytes a = raptorq_literal::matrix(p, isis);
        CHECK(raptorq_literal::rank(a, a.size() / p.l, p.l) == p.l);
        CHECK(satisfies(p, a, source.symbols, c, t));
    }
}

TEST_CASE("raptorq: Enc reproduces every source and padding symbol of every K' up to 2000", "[raptorq]")
{
    SeededWords words(0x6330U, 2U);
    const usize t = 4U;
    Bytes out = zeros(t);
    for (const RaptorqSystematicRow& row : raptorq_systematic_rows())
    {
        if (row.k_prime > 2000U)
        {
            break;
        }
        const u32 k = row.k_prime;
        CAPTURE(k);
        RaptorqParameters p;
        REQUIRE(raptorq_parameters(k, p));
        Source source(words, k, t);
        Plan plan;
        REQUIRE(plan.encoder(k) == RaptorqStatus::Ok);
        Bytes c;
        REQUIRE(plan.solve(source.symbols, t, c) == RaptorqStatus::Ok);
        bool same = true;
        for (u32 x = 0U; x < k; ++x)
        {
            raptorq_symbol(p, c.data(), t, x, out.data());
            same = same && std::memcmp(out.data(), source.symbols[x], t) == 0;
        }
        CHECK(same);
    }
}

TEST_CASE("raptorq: a short block's padding symbols are zero and its repair ISIs skip them", "[raptorq]")
{
    SeededWords words(0x6330U, 3U);
    const u32 k = 11U;
    const usize t = 8U;
    RaptorqParameters p;
    REQUIRE(raptorq_parameters(k, p));
    REQUIRE(p.k_prime == 12U);
    CHECK(raptorq_isi(p, k, 0U) == 0U);
    CHECK(raptorq_isi(p, k, 10U) == 10U);
    CHECK(raptorq_isi(p, k, 11U) == 12U);
    CHECK(raptorq_isi(p, k, 20U) == 21U);

    Source source(words, k, t);
    Plan plan;
    REQUIRE(plan.encoder(k) == RaptorqStatus::Ok);
    Bytes c;
    REQUIRE(plan.solve(source.symbols, t, c) == RaptorqStatus::Ok);
    Bytes out = zeros(t);
    raptorq_symbol(p, c.data(), t, 11U, out.data());
    const Bytes zero = zeros(t);
    CHECK(std::memcmp(out.data(), zero.data(), t) == 0);
}

TEST_CASE("raptorq: a decoder's intermediate symbols are the encoder's from any sufficient rows", "[raptorq]")
{
    SeededWords words(0x6330U, 4U);
    const usize t = 32U;
    for (const u32 k : {1U, 2U, 10U, 11U, 50U, 100U, 500U, 1000U, 4000U})
    {
        CAPTURE(k);
        RaptorqParameters p;
        REQUIRE(raptorq_parameters(k, p));
        Source source(words, k, t);
        Plan encoder;
        REQUIRE(encoder.encoder(k) == RaptorqStatus::Ok);
        Bytes c;
        REQUIRE(encoder.solve(source.symbols, t, c) == RaptorqStatus::Ok);

        // Lose a share of the sources at random, and replace them with repair symbols, plus a few more.
        for (const u32 loss_percent : {1U, 30U, 100U})
        {
            CAPTURE(loss_percent);
            Array<u32> esis;
            for (u32 i = 0U; i < k; ++i)
            {
                if (words.next_u32() % 100U >= loss_percent)
                {
                    esis.push_back(i);
                }
            }
            u32 next = k + (words.next_u32() % 1000U);
            while (esis.size() < k)
            {
                esis.push_back(next);
                ++next;
            }

            Plan decoder;
            RaptorqStatus status = decoder.decoder(k, esis);
            while (status == RaptorqStatus::Singular)
            {
                esis.push_back(next);
                ++next;
                status = decoder.decoder(k, esis);
            }
            REQUIRE(status == RaptorqStatus::Ok);

            Bytes received = zeros(esis.size() * t);
            Array<const u8*> symbols;
            for (usize i = 0U; i < esis.size(); ++i)
            {
                u8* const symbol = received.data() + (i * t);
                raptorq_symbol(p, c.data(), t, raptorq_isi(p, k, esis[i]), symbol);
                symbols.push_back(symbol);
            }
            Bytes decoded;
            REQUIRE(decoder.solve(symbols, t, decoded) == RaptorqStatus::Ok);
            CHECK(std::memcmp(decoded.data(), c.data(), c.size()) == 0);
        }
    }
}

TEST_CASE("raptorq: factor refuses exactly when the literal matrix has rank below L", "[raptorq]")
{
    // §5.8: a decoder recovers from any mathematically sufficient set. About one set of K' rows in 250 is not, so these
    // trials see both answers, and the rows above K' see the rest.
    SeededWords words(0x6330U, 5U);
    u32 refused = 0U;
    u32 accepted = 0U;
    for (const u32 k : {10U, 12U, 18U, 20U})
    {
        RaptorqParameters p;
        REQUIRE(raptorq_parameters(k, p));
        for (u32 trial = 0U; trial < 2000U; ++trial)
        {
            const u32 extra = (trial % 8U) == 7U ? 1U + (trial % 2U) : 0U;
            Array<u32> esis;
            Array<u32> isis;
            while (esis.size() < k + extra)
            {
                const u32 esi = words.next_u32() % (k + 40U);
                bool repeated = false;
                for (const u32 earlier : esis)
                {
                    repeated = repeated || earlier == esi;
                }
                if (!repeated)
                {
                    esis.push_back(esi);
                    isis.push_back(raptorq_isi(p, k, esi));
                }
            }
            const Bytes a = raptorq_literal::matrix(p, isis);
            const bool full_rank = raptorq_literal::rank(a, a.size() / p.l, p.l) == p.l;

            Plan plan;
            const RaptorqStatus status = plan.decoder(k, esis);
            CHECK(status == (full_rank ? RaptorqStatus::Ok : RaptorqStatus::Singular));
            if (status == RaptorqStatus::Ok)
            {
                ++accepted;
            }
            else
            {
                ++refused;
            }
        }
    }
    CHECK(accepted > 0U);
    CHECK(refused > 0U);
}

TEST_CASE("raptorq: the recovery rates of section 5.8 at K', K' + 1 and K' + 2", "[raptorq]")
{
    SeededWords words(0x6330U, 6U);
    const u32 trials = 3000U;
    u32 failures[3] = {0U, 0U, 0U};
    u32 attempts = 0U;
    for (const u32 k : {10U, 26U, 101U})
    {
        for (u32 trial = 0U; trial < trials; ++trial)
        {
            for (u32 overhead = 0U; overhead < 3U; ++overhead)
            {
                Array<u32> esis;
                for (u32 i = 0U; i < k + overhead; ++i)
                {
                    esis.push_back((words.next_u32() >> 8U) & (kRaptorqEsiLimit - 1U));
                }
                Plan plan;
                const RaptorqStatus status = plan.decoder(k, esis);
                if (status == RaptorqStatus::DuplicateEsi)
                {
                    continue; // a draw of two equal ESIs of 2^24, which a receiver cannot be sent
                }
                REQUIRE((status == RaptorqStatus::Ok || status == RaptorqStatus::Singular));
                failures[overhead] += status == RaptorqStatus::Singular ? 1U : 0U;
            }
        }
        attempts += trials;
    }
    // At most 1 in 100, 1 in 10,000 and 1 in 1,000,000 on average; 9,000 draws can only bound the last by a few.
    CHECK(failures[0] * 100U <= attempts);
    CHECK(failures[1] * 1000U <= attempts);
    CHECK(failures[2] <= 1U);
}

TEST_CASE("raptorq: hostile sets of ESIs decode or refuse, never answer wrongly", "[raptorq]")
{
    SeededWords words(0x6330U, 7U);
    const u32 k = 200U;
    const usize t = 16U;
    RaptorqParameters p;
    REQUIRE(raptorq_parameters(k, p));
    Source source(words, k, t);
    Plan encoder;
    REQUIRE(encoder.encoder(k) == RaptorqStatus::Ok);
    Bytes c;
    REQUIRE(encoder.solve(source.symbols, t, c) == RaptorqStatus::Ok);

    const auto check_esis = [&](const Array<u32>& esis)
    {
        Plan decoder;
        const RaptorqStatus status = decoder.decoder(k, esis);
        REQUIRE((status == RaptorqStatus::Ok || status == RaptorqStatus::Singular));
        if (status == RaptorqStatus::Ok)
        {
            Bytes received = zeros(esis.size() * t);
            Array<const u8*> symbols;
            for (usize i = 0U; i < esis.size(); ++i)
            {
                u8* const symbol = received.data() + (i * t);
                raptorq_symbol(p, c.data(), t, raptorq_isi(p, k, esis[i]), symbol);
                symbols.push_back(symbol);
            }
            Bytes decoded;
            REQUIRE(decoder.solve(symbols, t, decoded) == RaptorqStatus::Ok);
            CHECK(std::memcmp(decoded.data(), c.data(), c.size()) == 0);
        }
    };

    Array<u32> esis;
    SECTION("the last ESIs the field can carry")
    {
        for (u32 i = 0U; i < k + 2U; ++i)
        {
            esis.push_back(kRaptorqEsiLimit - 1U - i);
        }
        check_esis(esis);
    }
    SECTION("every repair symbol, descending")
    {
        for (u32 i = 0U; i < k + 2U; ++i)
        {
            esis.push_back(k + k + 1U - i);
        }
        check_esis(esis);
    }
    SECTION("many more rows than K'")
    {
        for (u32 i = 0U; i < (k * 3U); ++i)
        {
            esis.push_back(i);
        }
        check_esis(esis);
    }
}

TEST_CASE("raptorq: requests are refused in the documented order", "[raptorq]")
{
    Bytes memory = zeros(1U << 20U);
    RaptorqSolver solver;
    Bytes symbol = zeros(16U);
    const u8* const symbols[1] = {symbol.data()};

    CHECK(solver.factor(memory) == RaptorqStatus::BadSequence);
    CHECK(solver.factor_bytes() == 0U);
    CHECK(solver.solve_scratch_bytes(16U) == 0U);
    CHECK(solver.solve(symbols, 16U, memory, memory) == RaptorqStatus::BadSequence);

    CHECK(RaptorqSolver::structure_bytes(0U) == 0U);
    CHECK(RaptorqSolver::structure_bytes(kRaptorqMaxSourceSymbols + 1U) == 0U);
    CHECK(solver.analyze(0U, memory) == RaptorqStatus::BadBlockSize);
    CHECK(solver.analyze(kRaptorqMaxSourceSymbols + 1U, memory) == RaptorqStatus::BadBlockSize);

    Array<u32> esis;
    for (u32 i = 0U; i < 10U; ++i)
    {
        esis.push_back(i);
    }
    CHECK(solver.analyze(0U, esis, memory) == RaptorqStatus::BadBlockSize);

    // An ESI out of range comes before too few symbols.
    const u32 out_of_range[2] = {3U, kRaptorqEsiLimit};
    CHECK(solver.analyze(10U, out_of_range, memory) == RaptorqStatus::EsiOutOfRange);
    CHECK(RaptorqSolver::structure_bytes(10U, out_of_range) == 0U);

    const u32 too_few[9] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    CHECK(solver.analyze(10U, too_few, memory) == RaptorqStatus::TooFewSymbols);
    CHECK(RaptorqSolver::structure_bytes(10U, too_few) == 0U);
    CHECK(solver.analyze(10U, std::span<const u32>(), memory) == RaptorqStatus::TooFewSymbols);

    // Padding counts: 11 of 12 rows are symbols here, and no fewer.
    CHECK(solver.analyze(11U, std::span<const u32>(too_few, 9U), memory) == RaptorqStatus::TooFewSymbols);

    // Too little memory comes before a duplicate, which needs memory to find.
    u32 duplicate[10] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 8U};
    const usize duplicate_bytes = RaptorqSolver::structure_bytes(10U, duplicate);
    REQUIRE(duplicate_bytes != 0U);
    CHECK(solver.analyze(10U, duplicate, std::span<u8>(memory.data(), duplicate_bytes - 1U)) ==
          RaptorqStatus::WorkspaceTooSmall);
    CHECK(solver.analyze(10U, duplicate, memory) == RaptorqStatus::DuplicateEsi);
    duplicate[0] = 8U | (1U << 16U);
    duplicate[9] = 8U | (1U << 16U);
    CHECK(solver.analyze(10U, duplicate, memory) == RaptorqStatus::DuplicateEsi);

    // A failed call leaves the solver unprepared.
    CHECK(solver.factor(memory) == RaptorqStatus::BadSequence);
}

TEST_CASE("raptorq: the sizing calls are exact at every alignment", "[raptorq]")
{
    SeededWords words(0x6330U, 8U);
    const u32 k = 40U;
    const usize t = 8U;
    Source source(words, k, t);
    Plan reference;
    REQUIRE(reference.encoder(k) == RaptorqStatus::Ok);
    Bytes expected;
    REQUIRE(reference.solve(source.symbols, t, expected) == RaptorqStatus::Ok);

    for (usize offset = 0U; offset < 8U; ++offset)
    {
        CAPTURE(offset);
        RaptorqSolver solver;
        // Sentinels after each span: the sizes hold whatever alignment the memory has.
        const usize structure_bytes = RaptorqSolver::structure_bytes(k);
        Bytes structure = zeros(structure_bytes + 8U);
        std::memset(structure.data(), kGuard, structure.size());
        CHECK(solver.analyze(k, std::span<u8>(structure.data() + offset, structure_bytes - 1U)) ==
              RaptorqStatus::WorkspaceTooSmall);
        REQUIRE(solver.analyze(k, std::span<u8>(structure.data() + offset, structure_bytes)) == RaptorqStatus::Ok);
        CHECK(guarded(structure, offset + structure_bytes));

        const usize factor_bytes = solver.factor_bytes();
        REQUIRE(factor_bytes != 0U);
        Bytes factors = zeros(factor_bytes + 8U);
        std::memset(factors.data(), kGuard, factors.size());
        CHECK(solver.factor(std::span<u8>(factors.data() + offset, factor_bytes - 1U)) ==
              RaptorqStatus::WorkspaceTooSmall);
        REQUIRE(solver.factor(std::span<u8>(factors.data() + offset, factor_bytes)) == RaptorqStatus::Ok);
        CHECK(guarded(factors, offset + factor_bytes));

        const usize scratch_bytes = solver.solve_scratch_bytes(t);
        REQUIRE(scratch_bytes != 0U);
        Bytes scratch = zeros(scratch_bytes);
        Bytes c = zeros(usize{solver.parameters().l} * t);
        CHECK(solver.solve(source.symbols, t, c, std::span<u8>(scratch.data(), scratch_bytes - 1U)) ==
              RaptorqStatus::WorkspaceTooSmall);
        CHECK(solver.solve(source.symbols, t, std::span<u8>(c.data(), c.size() - 1U), scratch) ==
              RaptorqStatus::WorkspaceTooSmall);
        CHECK(solver.solve(std::span<const u8* const>(source.symbols.data(), k - 1U), t, c, scratch) ==
              RaptorqStatus::WrongCount);
        REQUIRE(solver.solve(source.symbols, t, c, scratch) == RaptorqStatus::Ok);
        CHECK(std::memcmp(c.data(), expected.data(), c.size()) == 0);
        CHECK(solver.solve(source.symbols, 0U, std::span<u8>(), std::span<u8>()) == RaptorqStatus::Ok);
    }
}

TEST_CASE("raptorq: one plan solves many blocks, and null symbols are zeros", "[raptorq]")
{
    SeededWords words(0x6330U, 9U);
    const u32 k = 64U;
    const usize t = 24U;
    Plan plan;
    REQUIRE(plan.encoder(k) == RaptorqStatus::Ok);
    for (u32 block = 0U; block < 8U; ++block)
    {
        Source source(words, k, t);
        Bytes shared;
        REQUIRE(plan.solve(source.symbols, t, shared) == RaptorqStatus::Ok);
        Plan fresh;
        REQUIRE(fresh.encoder(k) == RaptorqStatus::Ok);
        Bytes own;
        REQUIRE(fresh.solve(source.symbols, t, own) == RaptorqStatus::Ok);
        CHECK(std::memcmp(shared.data(), own.data(), own.size()) == 0);
    }

    Array<const u8*> none;
    for (u32 i = 0U; i < k; ++i)
    {
        none.push_back(nullptr);
    }
    Bytes c;
    REQUIRE(plan.solve(none, t, c) == RaptorqStatus::Ok);
    const Bytes zero = zeros(c.size());
    CHECK(std::memcmp(c.data(), zero.data(), c.size()) == 0);
}

TEST_CASE("raptorq: the largest block encodes, reproduces its sources and decodes", "[raptorq]")
{
    SeededWords words(0x6330U, 10U);
    const u32 k = kRaptorqMaxSourceSymbols;
    const usize t = 8U;
    RaptorqParameters p;
    REQUIRE(raptorq_parameters(k, p));
    Source source(words, k, t);
    Plan encoder;
    REQUIRE(encoder.encoder(k) == RaptorqStatus::Ok);
    Bytes c;
    REQUIRE(encoder.solve(source.symbols, t, c) == RaptorqStatus::Ok);
    // A few hundred more than P at most: the bound the header states for the dense part.
    CHECK(encoder.solver.inactivated() < p.p + 1000U);

    Bytes out = zeros(t);
    bool same = true;
    for (u32 x = 0U; x < k; x += 97U)
    {
        raptorq_symbol(p, c.data(), t, x, out.data());
        same = same && std::memcmp(out.data(), source.symbols[x], t) == 0;
    }
    CHECK(same);

    // Half the sources lost.
    Array<u32> esis;
    for (u32 i = 0U; i < k; i += 2U)
    {
        esis.push_back(i);
    }
    for (u32 i = 0U; esis.size() < usize{k} + 2U; ++i)
    {
        esis.push_back(k + i);
    }
    Bytes received = zeros(esis.size() * t);
    Array<const u8*> symbols;
    for (usize i = 0U; i < esis.size(); ++i)
    {
        u8* const symbol = received.data() + (i * t);
        raptorq_symbol(p, c.data(), t, raptorq_isi(p, k, esis[i]), symbol);
        symbols.push_back(symbol);
    }
    Plan decoder;
    REQUIRE(decoder.decoder(k, esis) == RaptorqStatus::Ok);
    Bytes decoded;
    REQUIRE(decoder.solve(symbols, t, decoded) == RaptorqStatus::Ok);
    CHECK(std::memcmp(decoded.data(), c.data(), c.size()) == 0);
}

TEST_CASE("raptorq: one solver serves several threads", "[raptorq]")
{
    SeededWords words(0x6330U, 11U);
    const u32 k = 300U;
    const usize t = 64U;
    Source source(words, k, t);
    Plan plan;
    REQUIRE(plan.encoder(k) == RaptorqStatus::Ok);
    Bytes expected;
    REQUIRE(plan.solve(source.symbols, t, expected) == RaptorqStatus::Ok);

    constexpr u32 thread_count = 4U;
    bool same[thread_count] = {};
    std::thread threads[thread_count];
    for (u32 n = 0U; n < thread_count; ++n)
    {
        threads[n] = std::thread(
            [&, n]()
            {
                bool all = true;
                for (u32 round = 0U; round < 8U; ++round)
                {
                    Bytes c;
                    all = all && plan.solve(source.symbols, t, c) == RaptorqStatus::Ok &&
                          std::memcmp(c.data(), expected.data(), c.size()) == 0;
                }
                same[n] = all;
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    for (const bool all : same)
    {
        CHECK(all);
    }
}

TEST_CASE("raptorq kernels: every level the process runs gives one encoding and decoding", "[raptorq][kernels]")
{
    SeededWords words(0x6330U, 12U);
    const u32 k = 101U;
    const usize t = 257U; // an odd length, past every vector's width
    RaptorqParameters p;
    REQUIRE(raptorq_parameters(k, p));
    Source source(words, k, t);
    Plan encoder;
    REQUIRE(encoder.encoder(k) == RaptorqStatus::Ok);
    Bytes c;
    REQUIRE(encoder.solve(source.symbols, t, c) == RaptorqStatus::Ok);
    CHECK(digest_of(c.data(), c.size()) == 0x5BF3CC6AA8A0BE50ULL);

    Array<u32> esis;
    for (u32 i = 0U; i < k + 2U; ++i)
    {
        esis.push_back(i + 50U);
    }
    Bytes received = zeros(esis.size() * t);
    Array<const u8*> symbols;
    for (usize i = 0U; i < esis.size(); ++i)
    {
        u8* const symbol = received.data() + (i * t);
        raptorq_symbol(p, c.data(), t, esis[i], symbol);
        symbols.push_back(symbol);
    }
    CHECK(digest_of(received.data(), received.size()) == 0x6E81FE2709B55830ULL);
    Plan decoder;
    REQUIRE(decoder.decoder(k, esis) == RaptorqStatus::Ok);
    Bytes decoded;
    REQUIRE(decoder.solve(symbols, t, decoded) == RaptorqStatus::Ok);
    CHECK(std::memcmp(decoded.data(), c.data(), c.size()) == 0);
}

TEST_CASE("raptorq: the FEC payload ID is section 3.2's four octets", "[raptorq]")
{
    u8 out[4] = {};
    REQUIRE(raptorq_write_payload_id(0xABU, 0x123456U, out));
    CHECK(out[0] == 0xABU);
    CHECK(out[1] == 0x12U);
    CHECK(out[2] == 0x34U);
    CHECK(out[3] == 0x56U);
    u8 sbn = 0U;
    u32 esi = 0U;
    REQUIRE(raptorq_read_payload_id(out, sbn, esi));
    CHECK(sbn == 0xABU);
    CHECK(esi == 0x123456U);

    REQUIRE(raptorq_write_payload_id(0U, kRaptorqEsiLimit - 1U, out));
    REQUIRE(raptorq_read_payload_id(out, sbn, esi));
    CHECK(sbn == 0U);
    CHECK(esi == kRaptorqEsiLimit - 1U);
    CHECK_FALSE(raptorq_write_payload_id(0U, kRaptorqEsiLimit, out));
    CHECK_FALSE(raptorq_write_payload_id(0U, 0U, std::span<u8>(out, 3U)));
    CHECK_FALSE(raptorq_read_payload_id(std::span<const u8>(out, 3U), sbn, esi));
}

TEST_CASE("raptorq: the object transmission information is section 3.3's twelve octets", "[raptorq]")
{
    const RaptorqOti oti{0x0102030405ULL, 0x0604U, 0xFFU, 0x0203U, 2U};
    REQUIRE(raptorq_oti_valid(oti));
    u8 out[12] = {};
    REQUIRE(raptorq_write_oti(oti, out));
    const u8 expected[12] = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x00U, 0x06U, 0x04U, 0xFFU, 0x02U, 0x03U, 0x02U};
    CHECK(std::memcmp(out, expected, 12U) == 0);

    RaptorqOti read;
    REQUIRE(raptorq_read_oti(out, read));
    CHECK(read.transfer_length == oti.transfer_length);
    CHECK(read.symbol_size == oti.symbol_size);
    CHECK(read.source_blocks == oti.source_blocks);
    CHECK(read.sub_blocks == oti.sub_blocks);
    CHECK(read.alignment == oti.alignment);

    out[5] = 0xFFU; // the reserved octet is ignored
    REQUIRE(raptorq_read_oti(out, read));
    CHECK(read.symbol_size == oti.symbol_size);

    CHECK_FALSE(raptorq_write_oti(oti, std::span<u8>(out, 11U)));
    CHECK_FALSE(raptorq_read_oti(std::span<const u8>(out, 11U), read));

    const auto refused = [](RaptorqOti changed)
    {
        u8 bytes[12] = {};
        CHECK_FALSE(raptorq_oti_valid(changed));
        CHECK_FALSE(raptorq_write_oti(changed, bytes));
    };
    const RaptorqOti base{1000U, 64U, 1U, 4U, 4U};
    REQUIRE(raptorq_oti_valid(base));
    RaptorqOti changed = base;
    changed.transfer_length = kRaptorqMaxTransferLength + 1U;
    refused(changed);
    changed = base;
    changed.symbol_size = 0U;
    refused(changed);
    changed = base;
    changed.symbol_size = 65536U;
    changed.alignment = 1U;
    refused(changed);
    changed = base;
    changed.symbol_size = 66U; // not a multiple of 4
    refused(changed);
    changed = base;
    changed.alignment = 0U;
    refused(changed);
    changed = base;
    changed.alignment = 256U;
    refused(changed);
    changed = base;
    changed.source_blocks = 0U;
    refused(changed);
    changed = base;
    changed.source_blocks = 256U;
    refused(changed);
    changed = base;
    changed.sub_blocks = 0U;
    refused(changed);
    changed = base;
    changed.sub_blocks = 17U; // sub-symbols smaller than the alignment
    refused(changed);
    changed = base;
    changed.transfer_length = (u64{kRaptorqMaxSourceSymbols} * 64U) + 1U; // one symbol too many for a block
    refused(changed);

    // Erratum 5548: the largest object is 255 blocks of 56403 symbols of 65535 octets, not the RFC's 256 blocks.
    CHECK(kRaptorqMaxTransferLength == u64{255U} * kRaptorqMaxSourceSymbols * 65535U);
    const RaptorqOti largest{u64{255U} * kRaptorqMaxSourceSymbols * 65535U, 65535U, 255U, 1U, 1U};
    CHECK(raptorq_oti_valid(largest));
    RaptorqOti over = largest;
    over.transfer_length += 1U;
    CHECK_FALSE(raptorq_oti_valid(over));
}

TEST_CASE("raptorq: Partition is section 4.4.1.2's", "[raptorq]")
{
    const RaptorqPartition even = raptorq_partition(12U, 4U);
    CHECK(even.large == 3U);
    CHECK(even.small == 3U);
    CHECK(even.large_count == 0U);
    CHECK(even.small_count == 4U);
    const RaptorqPartition uneven = raptorq_partition(14U, 4U);
    CHECK(uneven.large == 4U);
    CHECK(uneven.small == 3U);
    CHECK(uneven.large_count == 2U);
    CHECK(uneven.small_count == 2U);
    const RaptorqPartition few = raptorq_partition(2U, 5U);
    CHECK(few.large == 1U);
    CHECK(few.small == 0U);
    CHECK(few.large_count == 2U);
    CHECK(few.small_count == 3U);
    for (u64 size = 0U; size < 200U; ++size)
    {
        for (u64 parts = 1U; parts < 30U; ++parts)
        {
            const RaptorqPartition p = raptorq_partition(size, parts);
            CHECK((p.large * p.large_count) + (p.small * p.small_count) == size);
            CHECK(p.large_count + p.small_count == parts);
        }
    }
}

TEST_CASE("raptorq: section 4.3 derives T, Z and N", "[raptorq]")
{
    RaptorqOti oti;
    // F = 1 MiB, WS = 8 MiB, Al = 4, SS = 8, P' = 1024: one block fits, with one sub-block.
    REQUIRE(raptorq_derive_oti(1U << 20U, 8U << 20U, 4U, 8U, 1024U, oti));
    CHECK(oti.symbol_size == 1024U);
    CHECK(oti.source_blocks == 1U);
    CHECK(oti.sub_blocks == 1U);
    CHECK(oti.alignment == 4U);
    CHECK(raptorq_oti_valid(oti));

    // WS = 256 KiB: N is the smallest n whose KL holds the 1024 symbols.
    REQUIRE(raptorq_derive_oti(1U << 20U, 256U << 10U, 4U, 8U, 1024U, oti));
    CHECK(raptorq_oti_valid(oti));
    CHECK(oti.source_blocks == 1U);
    const auto kl = [](u64 ws, u64 al, u64 t, u64 n)
    {
        const u64 limit = ws / (al * ((t + (al * n) - 1U) / (al * n)));
        u32 best = 0U;
        for (const RaptorqSystematicRow& row : raptorq_systematic_rows())
        {
            if (row.k_prime <= limit)
            {
                best = row.k_prime;
            }
        }
        return best;
    };
    CHECK(kl(256U << 10U, 4U, 1024U, oti.sub_blocks) >= 1024U);
    CHECK(kl(256U << 10U, 4U, 1024U, oti.sub_blocks - 1U) < 1024U);

    // A huge object in a small memory takes several blocks.
    REQUIRE(raptorq_derive_oti(u64{4U} << 30U, 1U << 20U, 4U, 8U, 1024U, oti));
    CHECK(oti.source_blocks > 1U);
    CHECK(raptorq_oti_valid(oti));

    // F of exactly two of the largest blocks N_max allows is two blocks; a symbol more is three.
    const u64 largest = kl(1U << 20U, 4U, 1024U, 32U);
    REQUIRE(raptorq_derive_oti(largest * 2U * 1024U, 1U << 20U, 4U, 8U, 1024U, oti));
    CHECK(oti.source_blocks == 2U);
    CHECK(oti.sub_blocks == 32U);
    REQUIRE(raptorq_derive_oti((largest * 2U * 1024U) + 1U, 1U << 20U, 4U, 8U, 1024U, oti));
    CHECK(oti.source_blocks == 3U);

    // Symbols exactly at KL(1) take one sub-block; a symbol more takes two.
    const u64 whole = kl(1U << 20U, 4U, 1024U, 1U);
    REQUIRE(raptorq_derive_oti(whole * 1024U, 1U << 20U, 4U, 8U, 1024U, oti));
    CHECK(oti.source_blocks == 1U);
    CHECK(oti.sub_blocks == 1U);
    REQUIRE(raptorq_derive_oti((whole * 1024U) + 1U, 1U << 20U, 4U, 8U, 1024U, oti));
    CHECK(oti.source_blocks == 1U);
    CHECK(oti.sub_blocks == 2U);

    // 255 blocks are the most Z can name.
    REQUIRE(raptorq_derive_oti(largest * 255U * 1024U, 1U << 20U, 4U, 8U, 1024U, oti));
    CHECK(oti.source_blocks == 255U);
    CHECK_FALSE(raptorq_derive_oti((largest * 255U * 1024U) + 1U, 1U << 20U, 4U, 8U, 1024U, oti));

    // A zero-length object has one empty block.
    REQUIRE(raptorq_derive_oti(0U, 1U << 20U, 4U, 8U, 1024U, oti));
    CHECK(oti.source_blocks == 1U);
    CHECK(oti.sub_blocks == 1U);

    CHECK_FALSE(raptorq_derive_oti(kRaptorqMaxTransferLength + 1U, 1U << 20U, 4U, 8U, 1024U, oti));
    CHECK_FALSE(raptorq_derive_oti(1000U, 1U << 20U, 0U, 8U, 1024U, oti));
    CHECK_FALSE(raptorq_derive_oti(1000U, 1U << 20U, 256U, 1U, 1024U, oti));
    CHECK_FALSE(raptorq_derive_oti(1000U, 1U << 20U, 4U, 0U, 1024U, oti));
    CHECK_FALSE(raptorq_derive_oti(1000U, 1U << 20U, 4U, 8U, 0U, oti));
    CHECK_FALSE(raptorq_derive_oti(1000U, 1U << 20U, 4U, 8U, 65536U, oti));
    CHECK_FALSE(raptorq_derive_oti(1000U, 1U << 20U, 4U, 8U, 1022U, oti));                     // not a multiple of Al
    CHECK_FALSE(raptorq_derive_oti(1000U, 1U << 20U, 4U, 300U, 1024U, oti));                   // N_max = 0
    CHECK_FALSE(raptorq_derive_oti(1000U, 8U, 4U, 8U, 1024U, oti));                            // no K' fits the memory
    CHECK_FALSE(raptorq_derive_oti(kRaptorqMaxTransferLength, 1U << 20U, 4U, 8U, 1024U, oti)); // over 255 blocks
}

TEST_CASE("raptorq: an object is cut into section 4.4.1.2's blocks", "[raptorq]")
{
    // 14 symbols of 8 octets, the last short by 3, in 4 blocks: 4, 4, 3 and 3.
    const RaptorqOti oti{(14U * 8U) - 3U, 8U, 4U, 1U, 8U};
    REQUIRE(raptorq_oti_valid(oti));
    RaptorqBlock block;
    REQUIRE(raptorq_block(oti, 0U, block));
    CHECK(block.k == 4U);
    CHECK(block.offset == 0U);
    CHECK(block.bytes == 32U);
    REQUIRE(raptorq_block(oti, 1U, block));
    CHECK(block.k == 4U);
    CHECK(block.offset == 32U);
    REQUIRE(raptorq_block(oti, 2U, block));
    CHECK(block.k == 3U);
    CHECK(block.offset == 64U);
    CHECK(block.bytes == 24U);
    REQUIRE(raptorq_block(oti, 3U, block));
    CHECK(block.k == 3U);
    CHECK(block.offset == 88U);
    CHECK(block.bytes == 21U);
    CHECK_FALSE(raptorq_block(oti, 4U, block));

    // More blocks than symbols: the last have none, and no octets.
    const RaptorqOti thin{16U, 8U, 4U, 1U, 8U};
    REQUIRE(raptorq_block(thin, 1U, block));
    CHECK(block.k == 1U);
    CHECK(block.offset == 8U);
    REQUIRE(raptorq_block(thin, 2U, block));
    CHECK(block.k == 0U);
    CHECK(block.offset == 16U);
    CHECK(block.bytes == 0U);

    const RaptorqOti empty{0U, 8U, 1U, 1U, 8U};
    REQUIRE(raptorq_block(empty, 0U, block));
    CHECK(block.k == 0U);
    CHECK(block.bytes == 0U);
}

TEST_CASE("raptorq: a symbol of N sub-blocks takes its sub-symbol from each", "[raptorq]")
{
    // T = 12, Al = 2, N = 2... T / Al = 6 is split evenly: two sub-blocks of 3 units; N = 4 gives units 2, 2, 1, 1.
    const u32 k = 3U;
    const RaptorqOti oti{36U, 12U, 1U, 4U, 2U};
    REQUIRE(raptorq_oti_valid(oti));
    u8 object[36];
    for (u8 i = 0U; i < 36U; ++i)
    {
        object[i] = i;
    }
    // Sub-blocks: 3 symbols of 4 octets at 0, of 4 at 12, of 2 at 24, of 2 at 30.
    u8 symbol[12] = {};
    REQUIRE(raptorq_read_symbol(oti, k, object, 1U, symbol));
    const u8 expected[12] = {4U, 5U, 6U, 7U, 16U, 17U, 18U, 19U, 26U, 27U, 32U, 33U};
    CHECK(std::memcmp(symbol, expected, 12U) == 0);

    u8 written[36] = {};
    REQUIRE(raptorq_write_symbol(oti, k, written, 1U, symbol));
    CHECK(written[4] == 4U);
    CHECK(written[7] == 7U);
    CHECK(written[16] == 16U);
    CHECK(written[26] == 26U);
    CHECK(written[33] == 33U);
    CHECK(written[0] == 0U);
    CHECK(written[8] == 0U);

    CHECK_FALSE(raptorq_read_symbol(oti, k, object, 3U, symbol));
    CHECK_FALSE(raptorq_read_symbol(oti, k, object, 0U, std::span<u8>(symbol, 11U)));
    CHECK_FALSE(raptorq_write_symbol(oti, k, written, 3U, symbol));
    CHECK_FALSE(raptorq_write_symbol(oti, k, written, 0U, std::span<const u8>(symbol, 11U)));
    RaptorqOti bad = oti;
    bad.alignment = 0U;
    CHECK_FALSE(raptorq_read_symbol(bad, k, object, 0U, symbol));
    CHECK_FALSE(raptorq_write_symbol(bad, k, written, 0U, symbol));

    // Past the block's end a read is zero, and a write drops its octets.
    const std::span<const u8> partial(object, 33U);
    REQUIRE(raptorq_read_symbol(oti, k, partial, 2U, symbol));
    CHECK(symbol[0] == 8U);
    CHECK(symbol[10] == 0U); // the last sub-block's octet 34 is padding
    CHECK(symbol[11] == 0U);
    u8 small[30] = {};
    REQUIRE(raptorq_write_symbol(oti, k, small, 2U, symbol));
    CHECK(small[8] == 8U);
}

TEST_CASE("raptorq: an object survives the whole path from its bytes back to its bytes", "[raptorq]")
{
    SeededWords words(0x6330U, 13U);
    for (const u32 sub_blocks : {1U, 3U})
    {
        CAPTURE(sub_blocks);
        const usize f = 12345U;
        const RaptorqOti oti{f, 64U, 3U, sub_blocks, 4U};
        REQUIRE(raptorq_oti_valid(oti));
        const Bytes object = random_bytes(words, f);
        Bytes restored = zeros(f);

        for (u32 sbn = 0U; sbn < oti.source_blocks; ++sbn)
        {
            RaptorqBlock block;
            REQUIRE(raptorq_block(oti, sbn, block));
            RaptorqParameters p;
            REQUIRE(raptorq_parameters(block.k, p));
            const usize t = oti.symbol_size;
            const std::span<const u8> in(object.data() + block.offset, static_cast<usize>(block.bytes));

            Bytes sources = zeros(usize{block.k} * t);
            Array<const u8*> pointers;
            for (u32 m = 0U; m < block.k; ++m)
            {
                u8* const symbol = sources.data() + (usize{m} * t);
                REQUIRE(raptorq_read_symbol(oti, block.k, in, m, std::span<u8>(symbol, t)));
                pointers.push_back(symbol);
            }
            Plan encoder;
            REQUIRE(encoder.encoder(block.k) == RaptorqStatus::Ok);
            Bytes c;
            REQUIRE(encoder.solve(pointers, t, c) == RaptorqStatus::Ok);

            // Only repair symbols arrive.
            Array<u32> esis;
            for (u32 i = 0U; i < block.k + 2U; ++i)
            {
                esis.push_back(block.k + i);
            }
            Bytes received = zeros(esis.size() * t);
            Array<const u8*> arrived;
            for (usize i = 0U; i < esis.size(); ++i)
            {
                u8* const symbol = received.data() + (i * t);
                raptorq_symbol(p, c.data(), t, raptorq_isi(p, block.k, esis[i]), symbol);
                arrived.push_back(symbol);
            }
            Plan decoder;
            REQUIRE(decoder.decoder(block.k, esis) == RaptorqStatus::Ok);
            Bytes decoded;
            REQUIRE(decoder.solve(arrived, t, decoded) == RaptorqStatus::Ok);

            const std::span<u8> out(restored.data() + block.offset, static_cast<usize>(block.bytes));
            Bytes symbol = zeros(t);
            for (u32 m = 0U; m < block.k; ++m)
            {
                raptorq_symbol(p, decoded.data(), t, m, symbol.data());
                REQUIRE(raptorq_write_symbol(oti, block.k, out, m, symbol));
            }
        }
        CHECK(std::memcmp(restored.data(), object.data(), f) == 0);
    }
}
