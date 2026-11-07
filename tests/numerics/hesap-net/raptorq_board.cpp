// crd-hesap-raptorq-board: megabits per second of hesap-net's RaptorQ code against nanorq and the raptorq crate, on one
// block of K symbols of 1280 octets. Each arm's answer is compared with hesap's, so a faster wrong answer fails.
//   encode        a fresh plan, the intermediate symbols, 15% repair symbols
//   planned       the same with the block's plan made once before
//   decode        15% of the sources lost: a plan for the received rows, the intermediate symbols, the lost sources
// The first number of a block's own cost is its plan, which the planned row leaves out; the decode rows always pay it.
//
// Usage: crd-hesap-raptorq-board [--rounds N] [--quick] [--k K] [--planned]
//   rounds   interleaved rounds of every arm; the best of them is reported (default 7)
//   quick    one round over small blocks (the smoke CTest)
//   k        this block size alone
//   planned  the planned arms alone, so a profile of the run holds nothing else

#include "raptorq_peers.h"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/raptorq.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace hn = crd::hesap::net;
using crd::u32;
using crd::u8;
using crd::usize;

namespace
{

using Clock = std::chrono::steady_clock;
using Bytes = crd::containers::Array<u8>;
template <typename T> using Array = crd::containers::Array<T>;

constexpr usize kSymbol = 1280U;

void fail(const char* arm, u32 k)
{
    std::fprintf(stderr, "%s at K = %u: wrong bytes\n", arm, k);
    std::exit(1);
}

Bytes sized(usize size)
{
    Bytes bytes;
    bytes.resize(size);
    std::memset(bytes.data(), 0, size);
    return bytes;
}

// hesap's plan in memory kept from run to run, as a caller's would be.
struct Ours
{
    hn::RaptorqSolver solver;
    Bytes structure;
    Bytes factors;
    Bytes scratch;
    Bytes c;
    crd::u64 plan_bytes = 0U;

    void size_for(usize structure_bytes)
    {
        if (structure.size() < structure_bytes)
        {
            structure = sized(structure_bytes);
        }
    }

    void finish(u32 k)
    {
        if (solver.factor_bytes() > factors.size())
        {
            factors = sized(solver.factor_bytes());
        }
        if (solver.factor(factors) != hn::RaptorqStatus::Ok)
        {
            fail("hesap factor", k);
        }
        plan_bytes = structure.size() + solver.factor_bytes();
        const usize scratch_bytes = solver.solve_scratch_bytes(kSymbol);
        if (scratch_bytes > scratch.size())
        {
            scratch = sized(scratch_bytes);
        }
        const usize c_bytes = usize{solver.parameters().l} * kSymbol;
        if (c_bytes > c.size())
        {
            c = sized(c_bytes);
        }
    }

    void encoder(u32 k)
    {
        size_for(hn::RaptorqSolver::structure_bytes(k));
        if (solver.analyze(k, structure) != hn::RaptorqStatus::Ok)
        {
            fail("hesap analyze", k);
        }
        finish(k);
    }

    void decoder(u32 k, std::span<const u32> esis)
    {
        size_for(hn::RaptorqSolver::structure_bytes(k, esis));
        if (solver.analyze(k, esis, structure) != hn::RaptorqStatus::Ok)
        {
            fail("hesap analyze", k);
        }
        finish(k);
    }

    void solve(std::span<const u8* const> symbols)
    {
        if (solver.solve(symbols, kSymbol, c, scratch) != hn::RaptorqStatus::Ok)
        {
            fail("hesap solve", k());
        }
    }

    [[nodiscard]] u32 k() const { return solver.parameters().k_prime; }
};

struct Row
{
    double hesap = 1e300;
    double nanorq = 1e300;
    double crate = 1e300;
};

template <typename Work> void timed(double& best, const Work& work)
{
    const Clock::time_point start = Clock::now();
    work();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    best = seconds < best ? seconds : best;
}

void print_row(const char* name, u32 k, const Row& row)
{
    const double megabits = static_cast<double>(k) * static_cast<double>(kSymbol) * 8.0 / 1e6;
    std::printf("%-8s K=%-6u hesap %9.0f  nanorq %9.0f  crate %9.0f Mbit/s  hesap/best %5.2fx\n", name, k,
                megabits / row.hesap, megabits / row.nanorq, megabits / row.crate,
                (row.nanorq < row.crate ? row.nanorq : row.crate) / row.hesap);
}

} // namespace

int main(int argc, char** argv)
{
    u32 rounds = 7U;
    bool quick = false;
    bool only_planned = false;
    u32 only_k = 0U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--quick") == 0)
        {
            quick = true;
            rounds = 1U;
        }
        else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (std::strcmp(argv[i], "--k") == 0 && i + 1 < argc)
        {
            only_k = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (std::strcmp(argv[i], "--planned") == 0)
        {
            only_planned = true;
        }
    }

    const u32 full[] = {10U, 101U, 1000U, 10000U, 50000U};
    const u32 small[] = {10U, 101U};
    const std::span<const u32> sizes = only_k != 0U ? std::span<const u32>(&only_k, 1U)
                                       : quick      ? std::span<const u32>(small)
                                                    : std::span<const u32>(full);

    for (const u32 k : sizes)
    {
        SeededWords words(0x6330U, k);
        Bytes block = sized(usize{k} * kSymbol);
        for (usize i = 0U; i < block.size(); ++i)
        {
            block[i] = static_cast<u8>(words.next_u32() >> 24U);
        }
        Array<const u8*> sources;
        for (u32 i = 0U; i < k; ++i)
        {
            sources.push_back(block.data() + (usize{i} * kSymbol));
        }

        hn::RaptorqParameters p;
        if (!hn::raptorq_parameters(k, p))
        {
            fail("parameters", k);
        }

        const u32 repair_count = (k * 15U + 99U) / 100U;
        Bytes expected = sized(usize{repair_count} * kSymbol);
        Bytes out = sized(usize{repair_count} * kSymbol);

        Ours ours;
        ours.encoder(k);
        ours.solve(sources);
        for (u32 i = 0U; i < repair_count; ++i)
        {
            hn::raptorq_symbol(p, ours.c.data(), kSymbol, hn::raptorq_isi(p, k, k + i),
                               expected.data() + (usize{i} * kSymbol));
        }
        const u32 u_encode = ours.solver.inactivated();
        const crd::u64 encoder_plan_bytes = ours.plan_bytes;

        // The rows a decoder gets: the sources after the first `lost`, then repair symbols, until they suffice for all.
        const u32 lost = repair_count;
        Array<u32> esis;
        Array<const u8*> received;
        for (u32 x = lost; x < k; ++x)
        {
            esis.push_back(x);
            received.push_back(sources[x]);
        }
        for (u32 x = 0U; x < lost; ++x)
        {
            esis.push_back(k + x);
            received.push_back(expected.data() + (usize{x} * kSymbol));
        }
        Ours probe;
        probe.size_for(hn::RaptorqSolver::structure_bytes(k, esis));
        if (probe.solver.analyze(k, esis, probe.structure) != hn::RaptorqStatus::Ok)
        {
            fail("hesap analyze", k);
        }
        probe.factors = sized(probe.solver.factor_bytes());
        if (probe.solver.factor(probe.factors) != hn::RaptorqStatus::Ok)
        {
            std::printf("K=%u: this seed's %u rows are singular, row skipped\n", k, k);
            continue;
        }
        const u32 u_decode = probe.solver.inactivated();
        const crd::u64 decoder_plan_bytes = probe.structure.size() + probe.solver.factor_bytes();

        Array<u32> rows;
        Array<u32> repair_esis;
        Array<const u8*> row_symbols;
        for (u32 x = 0U; x < k; ++x)
        {
            row_symbols.push_back(sources[x]);
        }
        for (u32 x = 0U; x < lost; ++x)
        {
            rows.push_back(x);
            repair_esis.push_back(k + x);
            row_symbols[x] = expected.data() + (usize{x} * kSymbol);
        }

        Row encode;
        Row planned;
        Row decode;

        crd_nanorq_plan* const nanorq_plan = crd_nanorq_plan_new(k, kSymbol, nullptr, nullptr, 0U);
        void* const crate_plan = crd_raptorq_crate_plan_new(k);
        Bytes decoded = sized(usize{k} * kSymbol);
        Bytes lost_symbols = sized(usize{lost} * kSymbol);

        const auto produce = [&]()
        {
            for (u32 i = 0U; i < repair_count; ++i)
            {
                hn::raptorq_symbol(p, ours.c.data(), kSymbol, hn::raptorq_isi(p, k, k + i),
                                   out.data() + (usize{i} * kSymbol));
            }
        };
        const auto check = [&](const char* arm)
        {
            if (std::memcmp(out.data(), expected.data(), out.size()) != 0)
            {
                fail(arm, k);
            }
            std::memset(out.data(), 0, out.size());
        };

        const auto planned_arms = [&]()
        {
            timed(planned.hesap,
                  [&]()
                  {
                      ours.solve(sources);
                      produce();
                  });
            check("hesap planned");
            timed(planned.nanorq,
                  [&]()
                  {
                      crd_nanorq_run(nanorq_plan, sources.data());
                      for (u32 i = 0U; i < repair_count; ++i)
                      {
                          crd_nanorq_symbol(nanorq_plan, k + i, out.data() + (usize{i} * kSymbol));
                      }
                  });
            check("nanorq planned");
        };

        for (u32 round = 0U; round < rounds; ++round)
        {
            if (only_planned)
            {
                planned_arms();
                continue;
            }
            timed(encode.hesap,
                  [&]()
                  {
                      ours.encoder(k);
                      ours.solve(sources);
                      produce();
                  });
            check("hesap encode");
            timed(encode.nanorq,
                  [&]()
                  {
                      crd_nanorq_plan* const plan = crd_nanorq_plan_new(k, kSymbol, nullptr, nullptr, 0U);
                      crd_nanorq_run(plan, sources.data());
                      for (u32 i = 0U; i < repair_count; ++i)
                      {
                          crd_nanorq_symbol(plan, k + i, out.data() + (usize{i} * kSymbol));
                      }
                      crd_nanorq_plan_free(plan);
                  });
            check("nanorq encode");
            timed(encode.crate,
                  [&]()
                  {
                      void* const plan = crd_raptorq_crate_plan_new(k);
                      crd_raptorq_crate_repair(plan, block.data(), k, kSymbol, 0U, repair_count, out.data());
                      crd_raptorq_crate_plan_free(plan);
                  });
            check("crate encode");

            planned_arms();
            timed(planned.crate, [&]()
                  { crd_raptorq_crate_repair(crate_plan, block.data(), k, kSymbol, 0U, repair_count, out.data()); });
            check("crate planned");

            const auto check_lost = [&](const char* arm)
            {
                if (std::memcmp(lost_symbols.data(), block.data(), lost_symbols.size()) != 0)
                {
                    fail(arm, k);
                }
                std::memset(lost_symbols.data(), 0, lost_symbols.size());
            };
            timed(decode.hesap,
                  [&]()
                  {
                      ours.decoder(k, esis);
                      ours.solve(received);
                      for (u32 x = 0U; x < lost; ++x)
                      {
                          hn::raptorq_symbol(p, ours.c.data(), kSymbol, x, lost_symbols.data() + (usize{x} * kSymbol));
                      }
                  });
            check_lost("hesap decode");
            timed(decode.nanorq,
                  [&]()
                  {
                      crd_nanorq_plan* const plan =
                          crd_nanorq_plan_new(k, kSymbol, rows.data(), repair_esis.data(), lost);
                      if (plan == nullptr)
                      {
                          fail("nanorq decode", k);
                      }
                      crd_nanorq_run(plan, row_symbols.data());
                      for (u32 x = 0U; x < lost; ++x)
                      {
                          crd_nanorq_symbol(plan, x, lost_symbols.data() + (usize{x} * kSymbol));
                      }
                      crd_nanorq_plan_free(plan);
                  });
            check_lost("nanorq decode");
            timed(decode.crate,
                  [&]()
                  {
                      if (!crd_raptorq_crate_decode(k, kSymbol, esis.data(), received.data(),
                                                    static_cast<u32>(esis.size()), decoded.data()))
                      {
                          fail("crate decode", k);
                      }
                  });
            if (std::memcmp(decoded.data(), block.data(), decoded.size()) != 0)
            {
                fail("crate decode", k);
            }
        }
        crd_nanorq_plan_free(nanorq_plan);
        crd_raptorq_crate_plan_free(crate_plan);

        if (!only_planned)
        {
            print_row("encode", k, encode);
        }
        print_row("planned", k, planned);
        if (!only_planned)
        {
            print_row("decode", k, decode);
        }
        std::printf(
            "         K=%-6u L=%u P=%u: inactivated %u at the encoder, %u at the decoder; plans of %llu and %llu "
            "bytes\n",
            k, p.l, p.p, u_encode, u_decode, static_cast<unsigned long long>(encoder_plan_bytes),
            static_cast<unsigned long long>(decoder_plan_bytes));
    }
    return 0;
}
