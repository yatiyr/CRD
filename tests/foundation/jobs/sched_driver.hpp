#pragma once

// Controlled-interleaving driver for the DIAG.4b hooks: it imposes a scripted ORDER of yield-point tags.
// At each point, if the tag is the next unconsumed script tag it is released and the cursor advances; if
// the tag appears later in the remaining script the thread blocks until it becomes next; a tag not in the
// remaining script passes through. Matching is by TAG -- pool worker indices are not stable run-to-run, so
// the SchedEvent.thread field is informational for now. Replay = feed a recorded trace as the script.
//
// Safety: counter_finish_park (fp.*) runs on a worker/scheduler thread, so blocking it holds that worker;
// controlled tests must run with num_threads >= 3 so a peer stays runnable. A per-wait deadline is the
// net -- a wedge sets `deadlocked` and releases everyone rather than hanging the test binary. Uses a
// condition_variable with a predicate (robust against the lost-wake behaviour that retired
// std::counting_semaphore in this tree), never a raw semaphore.

#include "../../../engine/foundation/jobs/src/sched_check.hpp"

#if CRD_JOBS_SCHED_CHECK

#include "sched_record.hpp" // SchedEvent

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace crd::jobs::test
{

class SchedDriver
{
public:
    explicit SchedDriver(std::vector<const char*> script) : m_script(std::move(script)) {}

    void install() noexcept
    {
        m_oracle.on_point = &SchedDriver::trampoline;
        m_oracle.user     = this;
        crd::jobs::detail::set_sched_oracle(&m_oracle);
    }
    static void uninstall() noexcept { crd::jobs::detail::set_sched_oracle(nullptr); }

    [[nodiscard]] bool                            deadlocked() const noexcept { return m_deadlocked; }
    [[nodiscard]] const std::vector<const char*>& observed() const noexcept { return m_observed; }

    // Test-thread participation. mark() drives a SYNTHETIC tag through the same cursor logic, so the test
    // thread can occupy a scripted slot (e.g. release a worker held at a later point only after the test
    // has done its own step). await_arrived() blocks until some thread has ARRIVED at `tag` (recorded
    // before it may block there), letting the test sequence against a worker reaching a point without
    // racing the cursor. Returns false on timeout.
    void               mark(const char* tag) noexcept { on_point(tag); }
    [[nodiscard]] bool await_arrived(const char* tag, std::chrono::milliseconds deadline) noexcept
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        return m_cv.wait_for(lock, deadline, [&] { return m_deadlocked || arrived_locked(tag); });
    }

private:
    static void trampoline(void* user, const char* tag) noexcept
    {
        static_cast<SchedDriver*>(user)->on_point(tag);
    }

    bool is_next(const char* tag) const noexcept
    {
        return m_next < m_script.size() && std::strcmp(tag, m_script[m_next]) == 0;
    }
    bool in_remaining(const char* tag) const noexcept
    {
        for (std::size_t i = m_next; i < m_script.size(); ++i)
        {
            if (std::strcmp(tag, m_script[i]) == 0)
                return true;
        }
        return false;
    }
    bool arrived_locked(const char* tag) const noexcept
    {
        for (const char* t : m_arrived)
        {
            if (std::strcmp(t, tag) == 0)
                return true;
        }
        return false;
    }

    void on_point(const char* tag) noexcept
    {
        std::unique_lock<std::mutex> lock(m_mtx);
        // Record arrival BEFORE any block, so await_arrived() can observe a thread reaching this point
        // even while it is held here waiting for its scripted turn.
        m_arrived.push_back(tag);
        m_cv.notify_all();
        // Block only while this tag is a scripted one that is not yet its turn.
        while (!m_deadlocked && !is_next(tag) && in_remaining(tag))
        {
            if (!m_cv.wait_for(lock, m_deadline,
                               [&] { return m_deadlocked || is_next(tag) || !in_remaining(tag); }))
            {
                m_deadlocked = true; // wedge: stop imposing, wake everyone, let the test fail (not hang)
                m_cv.notify_all();
            }
        }
        if (!m_deadlocked && is_next(tag))
        {
            ++m_next;
            m_cv.notify_all();
        }
        m_observed.push_back(tag);
    }

    crd::jobs::detail::SchedOracle m_oracle{};
    std::vector<const char*>       m_script;
    std::size_t                    m_next = 0U;
    std::mutex                     m_mtx;
    std::condition_variable        m_cv;
    std::vector<const char*>       m_observed;
    std::vector<const char*>       m_arrived;
    std::chrono::milliseconds      m_deadline{5000};
    bool                           m_deadlocked = false;
};

} // namespace crd::jobs::test

#endif
