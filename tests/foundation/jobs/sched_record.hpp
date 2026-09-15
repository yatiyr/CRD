#pragma once

// Test-only recording oracle for the DIAG.4b controlled-interleaving hooks. It observes the order in
// which threads hit the counter-path yield points -- the ground-truth trace that the (d) driver will
// later impose and (e) will minimize. Recording only; it controls nothing. Entirely gated: with the
// scheduler hooks off this header is empty, so it is safe to include from any gated test.

#include "../../../engine/foundation/jobs/src/sched_check.hpp"

#if CRD_JOBS_SCHED_CHECK

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>

#include <mutex>

namespace crd::jobs::test
{

struct SchedEvent
{
    const char* tag    = nullptr; // the yield-point tag (a string literal from the counter path)
    crd::u32    thread = 0U;      // crd::jobs::worker_index() at the point (0 = main / thread 0)
};

// Appends (tag, worker_index) at every yield point. Bounded and allocation-free while recording (the
// capacity is reserved up front; past it, events are dropped and a flag is set). on_point may run on a
// fiber stack (fp.finalizing / wait.resumed) -- the critical section is short and never suspends the
// fiber, so a std::mutex is safe. events()/overflowed() are meant for use AFTER uninstall(), once no
// worker can still be writing.
class SchedRecorder
{
public:
    explicit SchedRecorder(crd::usize cap = 4096U) : m_cap(cap) { m_events.reserve(cap); }

    void install() noexcept
    {
        m_oracle.on_point = &SchedRecorder::trampoline;
        m_oracle.user     = this;
        crd::jobs::detail::set_sched_oracle(&m_oracle);
    }
    static void uninstall() noexcept { crd::jobs::detail::set_sched_oracle(nullptr); }

    [[nodiscard]] const crd::containers::Array<SchedEvent>& events() const noexcept { return m_events; }
    [[nodiscard]] bool overflowed() const noexcept { return m_overflowed; }

private:
    static void trampoline(void* user, const char* tag) noexcept
    {
        static_cast<SchedRecorder*>(user)->record(tag);
    }

    void record(const char* tag) noexcept
    {
        const crd::u32                  tid = crd::jobs::worker_index();
        const std::lock_guard<std::mutex> lock(m_mtx);
        if (m_events.size() >= m_cap)
        {
            m_overflowed = true;
            return;
        }
        m_events.push_back(SchedEvent{tag, tid});
    }

    crd::jobs::detail::SchedOracle     m_oracle{};
    std::mutex                         m_mtx;
    crd::containers::Array<SchedEvent> m_events;
    crd::usize                         m_cap;
    bool                               m_overflowed = false;
};

} // namespace crd::jobs::test

#endif
