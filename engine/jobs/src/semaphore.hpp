#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace crd::jobs::detail
{

// ---------------------------------------------------------------------------
// Semaphore — the worker-sleep primitive, built directly on futex (Linux) /
// WaitOnAddress (Windows) with the canonical expected==decision-value protocol.
//
// WHY THIS EXISTS (2026-07-02, the CI moat-test hang): std::counting_semaphore
// on libstdc++ (GCC 13.3, the Ubuntu 24.04 CI runner) loses wakes under
// oversubscription — proven by a live core dump of a hung determinism-moat
// test: the worker slept in futex_wait(&counter, expected=1) while the counter
// word itself read 1 (a token available, no wake ever coming), and
// WorkerPool::shutdown()'s join() blocked forever. Two library defects combine:
// __atomic_semaphore::_M_release skips the futex wake entirely when the counter
// was already > 0 (the header carries a FIXME admitting wake trouble), and the
// wait side (_S_do_spin) loads the futex `expected` value BEFORE running the
// acquire predicate, so a waiter can enter the kernel expecting a stale
// positive value that matches the current word — defeating the futex's own
// compare-and-sleep protection (GCC PR104928 class). The scheduler protocol
// above the semaphore is correct; the primitive underneath lost the wake.
//
// THE PROTOCOL HERE (immune to that class by construction):
//   - acquire() only ever sleeps with expected == 0, and only after the CAS
//     drain loop OBSERVED the count at 0 (a failed CAS reloads and retries; it
//     never falls through to sleep on a positive observation). The kernel
//     re-checks word == 0 atomically against concurrent releases: a release
//     landing in the pre-sleep window makes the wait return EAGAIN immediately.
//   - release() ALWAYS wakes — never "skip because the count was already
//     positive"; that optimization is exactly what loses wakes.
//   - Spurious wakeups are absorbed by the retry loop.
//
// Memory ordering: release() publishes with fetch_add(release); a successful
// acquire CAS reads acquire. RMWs extend the release sequence, so an acquire
// that consumes a token synchronizes-with the release that posted it — the
// same guarantee callers relied on from std::counting_semaphore (worker_loop's
// m_stopping load after a shutdown wake depends on it).
//
// Token count semantics match the scheduler's usage: tokens are wake hints
// (one per push), consumed only when an idle worker's acquire returns; a u32
// wrap would need 2^32 net-unconsumed releases without a single worker-idle
// consume in between, which the worker_loop structure (try_pop before every
// wait) makes unreachable in practice.
//
// Platform scope matches crd-jobs itself (the fiber context switch is win64 +
// lin64 asm): Windows + Linux only, enforced at compile time in semaphore.cpp.
// ---------------------------------------------------------------------------
class Semaphore
{
public:
    explicit Semaphore(crd::u32 initial = 0U) noexcept : m_count(initial) {}

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;
    ~Semaphore() = default;

    // Add n tokens and ALWAYS issue a wake (single for n == 1, broadcast
    // otherwise). Safe from any thread.
    void release(crd::u32 n = 1U) noexcept
    {
        if (n == 0U)
        {
            return;
        }
        m_count.fetch_add(n, std::memory_order_release);
        if (n == 1U)
        {
            wake_one();
        }
        else
        {
            wake_all();
        }
    }

    // Block until a token is consumed.
    void acquire() noexcept
    {
        for (;;)
        {
            if (try_drain_one())
            {
                return;
            }
            wait_on_zero(); // sleeps only if the word is still 0 (kernel-checked)
        }
    }

    // Try to consume a token, sleeping at most ~ms milliseconds once.
    // Returns true iff a token was consumed. Callers that must wait longer
    // loop (the scheduler's targeted-wake backstop pattern).
    [[nodiscard]] bool try_acquire_for_ms(crd::u32 ms) noexcept
    {
        if (try_drain_one())
        {
            return true;
        }
        timed_wait_on_zero(ms);
        return try_drain_one();
    }

private:
    // CAS-drain: consume one token if the count is positive. A failed CAS
    // reloads the current value and retries; this only returns false after
    // OBSERVING the count at 0 — the precondition for sleeping with expected 0.
    [[nodiscard]] bool try_drain_one() noexcept
    {
        crd::u32 cur = m_count.load(std::memory_order_relaxed);
        while (cur != 0U)
        {
            if (m_count.compare_exchange_weak(cur, cur - 1U, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return true;
            }
        }
        return false;
    }

    // Platform sleep/wake on the 32-bit count word (semaphore.cpp).
    void wait_on_zero() noexcept;
    void timed_wait_on_zero(crd::u32 ms) noexcept;
    void wake_one() noexcept;
    void wake_all() noexcept;

    std::atomic<crd::u32> m_count;
    static_assert(std::atomic<crd::u32>::is_always_lock_free,
                  "Semaphore: the count word must be a plain 32-bit futex-compatible atomic");
};

} // namespace crd::jobs::detail
