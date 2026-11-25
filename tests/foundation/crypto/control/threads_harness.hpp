// The four-thread signing harness of the host check's provider legs (NET.2;
// docs/design/test-instruments.md#host-separation-diag10a), shared by crd-mldsa-threads-control (AWS-LC alone) and
// crd-mldsa-threads-cerid (crd-crypto's SigningKey). It names neither library: a program gives each thread a session
// whose sign(reason, size) and verify() make one signing and one verification of kMessage, and the harness runs the
// shape of crd-crypto-tests' "Provider: one verifier and one key serve several threads": four threads, up to eight
// signings each, a thread stopping at its first fault, a refused signing timed and signed again at once, an invalid
// verification verified again. With the gate on, the threads wait until all four exist, so their first signings
// overlap. Output: one line per thread, then "result: clean" (0) or "result: FAULT" (1); setup failures are the
// programs' own "result: SETUP ..." (2).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace mldsa_threads
{
constexpr unsigned kThreads = 4U;
constexpr unsigned kSignings = 8U;
constexpr std::uint8_t kMessage[] = {'t', 'h', 'r', 'e', 'a', 'd', 's'};

// One thread's outcome, printed by the main thread after the joins.
struct Outcome
{
    unsigned iteration = kSignings; // the faulting iteration, kSignings when clean
    char refused[192] = {};         // what refused the signing, when one was refused
    long long refused_ms = 0;       // how long the refused signing took
    bool resigned = false;          // a second signing at once succeeded
    bool invalid = false;           // a signature did not verify
    bool invalid_again = false;     // and the second verification at once did not either
    long long first_ms = 0;         // how long the thread's first signing took
};

inline long long elapsed_ms(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

template <typename Session> void run_thread(Session& session, Outcome& outcome)
{
    for (unsigned i = 0U; i < kSignings && outcome.iteration == kSignings; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        const bool done = session.sign(outcome.refused, sizeof(outcome.refused));
        if (i == 0U)
        {
            outcome.first_ms = elapsed_ms(start);
        }
        if (!done)
        {
            outcome.iteration = i;
            outcome.refused_ms = elapsed_ms(start);
            char again[sizeof(outcome.refused)];
            outcome.resigned = session.sign(again, sizeof(again));
        }
        else if (!session.verify())
        {
            outcome.iteration = i;
            outcome.invalid = true;
            outcome.invalid_again = !session.verify();
        }
    }
}

inline int report(const Outcome (&outcomes)[kThreads])
{
    bool faulted = false;
    for (unsigned t = 0U; t < kThreads; ++t)
    {
        const Outcome& outcome = outcomes[t];
        if (outcome.iteration == kSignings)
        {
            std::printf("thread %u: clean, first signing %lld ms\n", t, outcome.first_ms);
            continue;
        }
        faulted = true;
        if (!outcome.invalid)
        {
            std::printf("thread %u: FAULT at iteration %u: %s, refused after %lld ms, signing again at once %s\n", t,
                        outcome.iteration, outcome.refused, outcome.refused_ms,
                        outcome.resigned ? "succeeded" : "refused");
        }
        else
        {
            std::printf("thread %u: FAULT at iteration %u: the signature did not verify, verifying again at once %s\n",
                        t, outcome.iteration, outcome.invalid_again ? "invalid" : "valid");
        }
    }
    std::printf("result: %s\n", faulted ? "FAULT" : "clean");
    return faulted ? 1 : 0;
}

// Runs the four threads, each with the session make() returns, and reports them.
template <typename Make> int run(bool gate, Make make)
{
    Outcome outcomes[kThreads];
    std::thread threads[kThreads];
    std::atomic<unsigned> arrived{0U};
    for (unsigned t = 0U; t < kThreads; ++t)
    {
        threads[t] = std::thread(
            [&, t]
            {
                auto session = make();
                arrived.fetch_add(1U);
                while (gate && arrived.load() < kThreads)
                {
                    std::this_thread::yield();
                }
                run_thread(session, outcomes[t]);
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    return report(outcomes);
}

// The program's arguments as flags: each must be one of `names`; returns false (after printing the SETUP line) on
// any other.
inline bool parse_flags(int argc, char** argv, const char* const* names, bool* values, unsigned count)
{
    for (int a = 1; a < argc; ++a)
    {
        unsigned found = count;
        for (unsigned n = 0U; n < count; ++n)
        {
            if (std::strcmp(argv[a], names[n]) == 0)
            {
                found = n;
            }
        }
        if (found == count)
        {
            std::printf("result: SETUP unknown argument %s\n", argv[a]);
            return false;
        }
        values[found] = true;
    }
    return true;
}
} // namespace mldsa_threads
