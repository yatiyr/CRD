// Queue-full positive control (DIAG.10c): a job pushed into a full injection queue, or a pinned job pushed into a
// full pinned lane, must fail as a distinct, always-on, counted report, never a silent drop. Before DIAG.10c both
// were guarded by CRD_ASSERT_MSG alone: with asserts compiled out the job was dropped (the pinned slot overwritten),
// its counter never reached zero and its waiter hung. A frame allocation past the arena was the same: an assertion,
// and in a build without asserts a pointer past the arena.
//
// Mode `injection`: one thread (no worker drains anything), an injection capacity of 2 and three run() calls; the
// third push finds the Normal queue full. Mode `pinned`: one thread, a pinned lane of 2 and three jobs pinned to
// thread 0; the third finds the lane full. Mode `frame`: a 256-byte frame arena filled exactly, then one more byte
// asked of frame_alloc(). The installed assert platform handler, on the expected message, reads
// crd::jobs::pressure_snapshot(), judges the window since init with the crd-core pressure probe, prints the report
// line as CRD_DIAG_PRESSURE=... and exits 42 only if the window is Exhausted with exactly one refusal and the
// resource held at its bound (60 otherwise, 97 for a different assert or mode). If no fatal fires, control falls
// through to a clean exit 0, which the test rejects.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is a scheduler-budget control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/core/budget_pressure.hpp>
#include <crd/jobs/jobs.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// The budget each mode exhausts: its argument, the fatal's message and the report's label.
enum class Mode : crd::u8
{
    Injection,
    Pinned,
    Frame,
};

struct ModeInfo
{
    const char* argument;
    const char* message;
    const char* label;
};

constexpr ModeInfo kModes[3] = {{"injection", "injection queue full", "injection.normal"},
                                {"pinned", "pinned lane full", "pinned"},
                                {"frame", "frame arena exhausted", "frame"}};

// The mode, and the reading taken right after init().
Mode g_mode = Mode::Injection;
crd::BudgetReading g_before{};

const crd::BudgetReading& pick(const crd::jobs::JobsPressure& p) noexcept
{
    switch (g_mode)
    {
        case Mode::Pinned:
            return p.pinned;
        case Mode::Frame:
            return p.frame;
        case Mode::Injection:
            break;
    }
    return p.injection[1]; // the Normal lane
}

int queue_full_assert_handler(const char* formatted_message)
{
    const ModeInfo& mode = kModes[static_cast<crd::u32>(g_mode)];
    if (formatted_message != nullptr && std::strstr(formatted_message, mode.message) != nullptr)
    {
        const crd::jobs::JobsPressure p     = crd::jobs::pressure_snapshot();
        const crd::BudgetReading&     after = pick(p);
        const crd::PressureReport     rep   = crd::pressure_report(g_before, after);
        char                          line[160] = {};
        (void)crd::format_pressure_report(mode.label, rep, after, line, sizeof(line));
        std::printf("CRD_DIAG_PRESSURE=%s\n", line);
        std::fflush(stdout);
        const bool counted = rep.verdict == crd::PressureVerdict::Exhausted && rep.refused == 1U
                             && after.in_use == after.capacity;
        std::_Exit(counted ? 42 : 60);
    }
    std::_Exit(97);
}

void noop_job(void* /*data*/) noexcept {}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd::u32 mode = 0U;
    while (argc >= 2 && mode < 3U && std::strcmp(argv[1], kModes[mode].argument) != 0)
    {
        ++mode;
    }
    if (argc < 2 || mode == 3U)
    {
        std::printf("usage: crd-diag-queue-full-specimen injection|pinned|frame\n");
        return 97;
    }
    g_mode = static_cast<Mode>(mode);
    crd::set_assert_platform_handler(&queue_full_assert_handler);
    crd_diag_announce();

    crd::jobs::Config cfg;
    cfg.num_threads              = 1U; // thread 0 only, which never pumps: nothing drains a queue
    cfg.injection_queue_capacity = 2U;
    cfg.pinned_queue_capacity    = 2U;
    cfg.frame_alloc_bytes        = 256U;
    crd::jobs::init(cfg);
    g_before = pick(crd::jobs::pressure_snapshot());

    if (g_mode == Mode::Frame)
    {
        (void)crd::jobs::frame_alloc(256U, 1U); // the whole arena
        (void)crd::jobs::frame_alloc(1U, 1U);   // the violation
        return 0;                               // failure path only: no fatal fired
    }
    crd::jobs::JobDecl j{};
    j.fn         = &noop_job;
    j.pin_thread = g_mode == Mode::Pinned ? 0 : -1;
    for (int i = 0; i < 3; ++i) // the third push finds the queue full
    {
        (void)crd::jobs::run(j);
    }

    // Failure path only: the guard did not fire (the job was dropped or overwritten). Exit clean so the test, which
    // demands 42, fails; the counters stay held, so no shutdown is attempted.
    return 0;
}
