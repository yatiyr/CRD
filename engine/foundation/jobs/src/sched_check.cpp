#include "sched_check.hpp"

// Definitions exist only under the test-only gate; gate off this is an empty translation unit.
#if CRD_JOBS_SCHED_CHECK

#include <atomic>

namespace crd::jobs::detail
{
namespace
{
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) -- single process-wide oracle,
// mirrors the JobObserver registration in observer.cpp.
std::atomic<const SchedOracle*> g_sched_oracle{nullptr};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) -- process-wide violation tally
// for the gate-only publication/reclamation detector; a test resets, provokes, then reads it back.
std::atomic<crd::u64> g_sched_violations{0U};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) -- process-wide break switch for
// the deliberately-broken handshake variant; a test flips it around a forced interleaving.
std::atomic<bool> g_break_handshake{false};
} // namespace

void set_sched_oracle(const SchedOracle* oracle) noexcept
{
    g_sched_oracle.store(oracle, std::memory_order_release);
}

const SchedOracle* current_sched_oracle() noexcept
{
    return g_sched_oracle.load(std::memory_order_acquire);
}

void sched_point(const char* tag) noexcept
{
    const SchedOracle* const o = g_sched_oracle.load(std::memory_order_acquire);
    if (o != nullptr && o->on_point != nullptr)
    {
        o->on_point(o->user, tag);
    }
}

void sched_check_note_violation(const char* /*tag*/) noexcept
{
    g_sched_violations.fetch_add(1U, std::memory_order_acq_rel);
}

crd::u64 sched_check_violations() noexcept
{
    return g_sched_violations.load(std::memory_order_acquire);
}

void sched_check_reset_violations() noexcept
{
    g_sched_violations.store(0U, std::memory_order_release);
}

void sched_check_break_handshake(bool broken) noexcept
{
    g_break_handshake.store(broken, std::memory_order_release);
}

bool sched_check_handshake_broken() noexcept
{
    return g_break_handshake.load(std::memory_order_acquire);
}

} // namespace crd::jobs::detail

#endif
