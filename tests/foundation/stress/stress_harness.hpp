#pragma once

// ---------------------------------------------------------------------------
// Cerid stress-test harness (detour D-002, slice v0)
//
// A reusable driver for pounding a data structure / allocator with many
// concurrent agents and verifying invariants between rounds.
//
// Two run modes:
//   RunMode::Fibers  — N worker jobs on the crd-jobs fiber scheduler. The
//                      *normative* lane for things whose concurrency contract
//                      is fiber-shaped (scene storages). TSan cannot see the
//                      hand-rolled asm context switch, so the fiber lane proves
//                      correctness via ASan + invariant oracles + seeded
//                      reproduction, not TSan instrumentation.
//   RunMode::Threads — the *same* worker closure run on N std::threads. The
//                      normative lane for pure concurrent primitives
//                      (ConcurrentQueue, AtomicArray) because TSan-on-Linux can
//                      instrument it.
//
// A run is RR rounds of (II iterations per worker). The invariant oracle runs
// on the main thread between rounds and once more at the end — i.e. at points
// where no worker is touching the structure, so the oracle observes a quiescent
// snapshot. Per-worker RNG is seeded deterministically from (base_seed,
// worker_index, round) so any failure reproduces from the printed seed.
//
// Fiber mode requires crd::jobs to be initialised by the caller; the stress
// test binary's main does this once for the whole process (see main_stress.cpp).
// ---------------------------------------------------------------------------

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <span>
#include <thread>

namespace crd::stress
{

using crd::u32;
using crd::u64;

// ---------------------------------------------------------------------------
// FailSink — thread-safe failure recorder.
//
// Catch2 assertion macros are NOT thread-safe, so worker closures must never
// call REQUIRE/CHECK directly. They report into a FailSink; the oracle (which
// runs on the main thread, quiescent) inspects it and turns it into a Catch2
// assertion via CRD_STRESS_ORACLE_OK below. The first failer wins the message.
// ---------------------------------------------------------------------------
class FailSink
{
public:
    void fail(const char* what, u64 iter = 0U, u32 worker = 0U) noexcept
    {
        bool expected = false;
        if (m_failed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            std::snprintf(m_msg, sizeof(m_msg), "worker %u, iter %llu: %s", static_cast<unsigned>(worker),
                          static_cast<unsigned long long>(iter), what != nullptr ? what : "(null)");
        }
    }

    [[nodiscard]] bool ok() const noexcept { return !m_failed.load(std::memory_order_acquire); }
    [[nodiscard]] const char* message() const noexcept { return m_msg; }

private:
    std::atomic<bool> m_failed{false};
    char m_msg[192]{};
};

// Use inside a worker closure: records first failure, then `return`s out of the
// current iteration's lambda body (worker keeps running so other workers aren't
// starved of contention, but does no further bookkeeping this iteration).
#define CRD_STRESS_FAIL_IF(sink, worker, iter, cond, what)                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            (sink).fail((what), (iter), (worker));                                                                     \
        }                                                                                                              \
    } while (false)

// Use after a harness run on the main thread: turns a FailSink into a Catch2 assertion.
#define CRD_STRESS_ORACLE_OK(sink)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        INFO((sink).message());                                                                                        \
        REQUIRE((sink).ok());                                                                                          \
    } while (false)

// --- splitmix64 — tiny, fast, well-distributed; deterministic per worker -----
class Rng
{
public:
    explicit Rng(u64 seed) noexcept : m_state(seed) {}

    u64 next_u64() noexcept
    {
        u64 z = (m_state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform-ish in [0, bound). bound must be > 0. (Multiply-shift; bias negligible here.)
    u32 next_u32(u32 bound) noexcept
    {
        CRD_ASSERT(bound > 0U);
        return static_cast<u32>(((next_u64() >> 32) * static_cast<u64>(bound)) >> 32);
    }

    bool flip() noexcept { return (next_u64() & 1ULL) != 0ULL; }

    float next_f01() noexcept
    {
        return static_cast<float>(next_u64() >> 40) * (1.0f / 16777216.0f); // 24-bit mantissa
    }

private:
    u64 m_state;
};

// Deterministic seed for (base, worker, round); mixed so adjacent streams differ.
inline u64 worker_seed(u64 base, u32 worker, u32 round) noexcept
{
    u64 z = base ^ (static_cast<u64>(worker) << 1) ^ (static_cast<u64>(round) << 33);
    z = (z ^ (z >> 33)) * 0xFF51AFD7ED558CCDULL;
    z = (z ^ (z >> 33)) * 0xC4CEB9FE1A85EC53ULL;
    return z ^ (z >> 33);
}

enum class RunMode
{
    Fibers,
    Threads,
};

struct Config
{
    RunMode mode = RunMode::Fibers;
    u32 num_workers = 0U;               // 0 → jobs::num_workers() (Fibers) / hardware_concurrency() (Threads)
    u32 rounds = 1U;                    // oracle runs after each round + once at the end
    u64 iterations_per_round = 50'000U; // per worker, per round
    u64 base_seed = 0xCE21D5EEDULL;
};

// "Bounded" — what runs inside the 14-config full-sweep: small, fixed, fast.
inline Config bounded(RunMode mode = RunMode::Fibers) noexcept
{
    Config c;
    c.mode = mode;
    c.rounds = 4U;
    c.iterations_per_round = 4'000U;
    return c;
}

// "Soak" — the nightly lane: long. Drive these from tests behind the [.soak] tag.
inline Config soak(RunMode mode = RunMode::Fibers) noexcept
{
    Config c;
    c.mode = mode;
    c.rounds = 20U;
    c.iterations_per_round = 2'000'000U;
    return c;
}

namespace detail
{
inline u32 resolve_workers(const Config& cfg) noexcept
{
    if (cfg.num_workers != 0U)
    {
        return cfg.num_workers;
    }
    if (cfg.mode == RunMode::Fibers)
    {
        const u32 n = crd::jobs::num_workers();
        return n > 0U ? n : 4U;
    }
    const unsigned hc = std::thread::hardware_concurrency();
    return hc > 0U ? static_cast<u32>(hc) : 4U;
}
} // namespace detail

// ---------------------------------------------------------------------------
// run — drive `work` with N workers for `rounds` rounds, calling `oracle`
//       between rounds and at the end.
//
//   WorkerFn : void(u32 worker_index, u64 iterations, Rng& rng)
//   OracleFn : void(u32 round)   // round == cfg.rounds on the final call
//
// The oracle is invoked with no worker touching the structure. WorkerFn is
// shared by const-ref across all workers — it must be safe to call
// concurrently (that is the point of the test).
// ---------------------------------------------------------------------------
template <typename WorkerFn, typename OracleFn>
void run(const Config& cfg, const WorkerFn& work, const OracleFn& oracle)
{
    const u32 workers = detail::resolve_workers(cfg);
    CRD_ASSERT(workers > 0U);

    crd::memory::IAllocator* const alloc = crd::memory::default_allocator();

    for (u32 round = 0U; round < cfg.rounds; ++round)
    {
        if (cfg.mode == RunMode::Fibers)
        {
            // One job per worker. SBO payload = {const WorkerFn*, u32 w, u64 iter, u64 seed} —
            // trivially copyable, well under the 41-byte make_job limit.
            auto* const decls = static_cast<crd::jobs::JobDecl*>(
                alloc->allocate(workers * sizeof(crd::jobs::JobDecl), alignof(crd::jobs::JobDecl)));

            for (u32 w = 0U; w < workers; ++w)
            {
                const u64 seed = worker_seed(cfg.base_seed, w, round);
                const u64 iter = cfg.iterations_per_round;
                const WorkerFn* wp = &work;
                decls[w] = crd::jobs::make_job(
                    [wp, w, iter, seed]()
                    {
                        Rng rng(seed);
                        (*wp)(w, iter, rng);
                    });
            }

            crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(decls, workers));
            alloc->deallocate(decls);
        }
        else
        {
            auto* const threads =
                static_cast<std::thread*>(alloc->allocate(workers * sizeof(std::thread), alignof(std::thread)));

            for (u32 w = 0U; w < workers; ++w)
            {
                const u64 seed = worker_seed(cfg.base_seed, w, round);
                const u64 iter = cfg.iterations_per_round;
                ::new (threads + w) std::thread(
                    [&work, w, iter, seed]()
                    {
                        Rng rng(seed);
                        work(w, iter, rng);
                    });
            }
            for (u32 w = 0U; w < workers; ++w)
            {
                threads[w].join();
                threads[w].~thread();
            }
            alloc->deallocate(threads);
        }

        oracle(round);
    }
    oracle(cfg.rounds); // final quiescent check
}

} // namespace crd::stress
