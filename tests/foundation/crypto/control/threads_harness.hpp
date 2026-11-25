// The four-thread signing harness of the host check's provider legs (NET.2;
// docs/design/test-instruments.md#host-separation-diag10a), shared by crd-mldsa-threads-control (AWS-LC alone) and
// crd-mldsa-threads-cerid (crd-crypto's SigningKey). It names neither library: a program gives each thread a session
// whose sign(reason, size) and verify() make one signing and one verification of kMessage, and the harness runs the
// shape of crd-crypto-tests' "Provider: one verifier and one key serve several threads": four threads, up to eight
// signings each, a thread stopping at its first fault, a refused signing timed and signed again at once, an invalid
// verification verified again. The harness's own arguments, which both programs take: "gate" holds the threads until
// all four exist, so their first signings overlap; "touch" has each thread first touch kTouchBytes of its stack below
// its entry, a page at a time, so the first signing finds its stack pages (and, under AddressSanitizer, their shadow)
// already committed; "touch-stack" makes the same touch from a function AddressSanitizer does not instrument, so only
// the stack pages are committed and their shadow is not; "shift=N" runs each thread's work N bytes (1 to 65536) lower
// on its stack, below an untouched allocation, so the signing's frames meet the stack's pages at another offset.
// Output: one line per thread, then "result: clean" (0) or "result: FAULT" (1); setup failures are the programs' own
// "result: SETUP ..." (2).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

// NOLINTBEGIN(cppcoreguidelines-macro-usage): function attributes and a stack allocation, which only a macro can spell
// per compiler. The programs link no Cerid code, so the engine's CRD_MEM_ASAN_UNCHECKED is not theirs to use.
#if defined(__SANITIZE_ADDRESS__)
#define MLDSA_THREADS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MLDSA_THREADS_ASAN 1
#endif
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define MLDSA_THREADS_NOINLINE __declspec(noinline)
#define MLDSA_THREADS_ALLOCA _alloca
#if defined(MLDSA_THREADS_ASAN)
#define MLDSA_THREADS_UNCHECKED __declspec(no_sanitize_address) __declspec(noinline)
#endif
#else
#define MLDSA_THREADS_NOINLINE __attribute__((noinline))
#define MLDSA_THREADS_ALLOCA __builtin_alloca
#if defined(MLDSA_THREADS_ASAN)
#define MLDSA_THREADS_UNCHECKED __attribute__((no_sanitize_address, noinline))
#endif
#endif
#if !defined(MLDSA_THREADS_UNCHECKED)
#define MLDSA_THREADS_UNCHECKED MLDSA_THREADS_NOINLINE
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

namespace mldsa_threads
{
constexpr unsigned kThreads = 4U;
constexpr unsigned kSignings = 8U;
constexpr std::uint8_t kMessage[] = {'t', 'h', 'r', 'e', 'a', 'd', 's'};
constexpr std::size_t kTouchBytes = 512U * 1024U; // half of a default 1 MiB thread stack
constexpr std::size_t kTouchFrame = 16U * 1024U;
constexpr std::size_t kPage = 4096U;
constexpr std::size_t kShiftLimit = 64U * 1024U;

// How each thread starts: the harness's own arguments (the header comment).
enum class Touch
{
    none,
    checked,   // "touch": instrumented, so under AddressSanitizer the stack's shadow is committed too
    unchecked, // "touch-stack": not instrumented, the stack pages alone
};

struct Start
{
    bool gate = false;
    Touch touch = Touch::none;
    std::size_t shift = 0U;
};

// Touches `bytes` of the calling thread's stack below this frame, one kTouchFrame frame per call; the write after the
// inner call, which reads the frame back, keeps each frame alive across it (no tail call reuses it).
inline void touch_stack(std::size_t bytes)
{
    volatile std::uint8_t frame[kTouchFrame];
    for (std::size_t offset = 0U; offset < kTouchFrame; offset += kPage)
    {
        frame[offset] = 0U;
    }
    if (bytes > kTouchFrame)
    {
        touch_stack(bytes - kTouchFrame);
    }
    frame[0] = static_cast<std::uint8_t>(frame[0] + 1U);
}

// touch_stack's touch from a function AddressSanitizer does not instrument: its frame gets no redzones, so its shadow
// is never written, and its stores are not checked, so the shadow is never read.
MLDSA_THREADS_UNCHECKED inline void touch_stack_unchecked(std::size_t bytes)
{
    volatile std::uint8_t frame[kTouchFrame];
    for (std::size_t offset = 0U; offset < kTouchFrame; offset += kPage)
    {
        frame[offset] = 0U;
    }
    if (bytes > kTouchFrame)
    {
        touch_stack_unchecked(bytes - kTouchFrame);
    }
    frame[0] = static_cast<std::uint8_t>(frame[0] + 1U);
}

// Runs `work` below `bytes` of stack this frame allocates and leaves untouched (AddressSanitizer writes only its
// shadow, poisoning the redzones); the allocation's address escapes through a volatile store, so it is kept.
template <typename Work> MLDSA_THREADS_NOINLINE void shifted(std::size_t bytes, const Work& work)
{
    void* volatile below = MLDSA_THREADS_ALLOCA(bytes);
    static_cast<void>(below);
    work();
}

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
template <typename Make> int run(const Start& start, Make make)
{
    Outcome outcomes[kThreads];
    std::thread threads[kThreads];
    std::atomic<unsigned> arrived{0U};
    for (unsigned t = 0U; t < kThreads; ++t)
    {
        threads[t] = std::thread(
            [&, t]
            {
                const auto work = [&]
                {
                    if (start.touch == Touch::checked)
                    {
                        touch_stack(kTouchBytes);
                    }
                    else if (start.touch == Touch::unchecked)
                    {
                        touch_stack_unchecked(kTouchBytes);
                    }
                    auto session = make();
                    arrived.fetch_add(1U);
                    while (start.gate && arrived.load() < kThreads)
                    {
                        std::this_thread::yield();
                    }
                    run_thread(session, outcomes[t]);
                };
                if (start.shift != 0U)
                {
                    shifted(start.shift, work);
                }
                else
                {
                    work();
                }
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    return report(outcomes);
}

// One of the harness's own arguments into `start`: false when `argument` is none of them or a malformed shift.
inline bool parse_start(const char* argument, Start& start)
{
    constexpr char kShift[] = "shift=";
    if (std::strcmp(argument, "gate") == 0)
    {
        start.gate = true;
    }
    else if (std::strcmp(argument, "touch") == 0)
    {
        start.touch = Touch::checked;
    }
    else if (std::strcmp(argument, "touch-stack") == 0)
    {
        start.touch = Touch::unchecked;
    }
    else if (std::strncmp(argument, kShift, sizeof(kShift) - 1U) == 0)
    {
        const char* digits = argument + sizeof(kShift) - 1U;
        char* end = nullptr;
        const unsigned long bytes = std::strtoul(digits, &end, 10);
        if (*digits < '0' || *digits > '9' || *end != '\0' || bytes == 0U || bytes > kShiftLimit)
        {
            return false;
        }
        start.shift = bytes;
    }
    else
    {
        return false;
    }
    return true;
}

// The program's arguments: the harness's own into `start` (parse_start), every other one a flag that must be one of
// `names`; returns false (after printing the SETUP line) on any other.
inline bool parse_flags(int argc, char** argv, const char* const* names, bool* values, unsigned count, Start& start)
{
    for (int a = 1; a < argc; ++a)
    {
        if (parse_start(argv[a], start))
        {
            continue;
        }
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
            std::printf("result: SETUP unknown or malformed argument %s\n", argv[a]);
            return false;
        }
        values[found] = true;
    }
    return true;
}
} // namespace mldsa_threads
