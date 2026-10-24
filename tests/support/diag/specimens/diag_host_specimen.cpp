// The DIAG.11a diagnostic-host specimen: a small application that composes the engine's DiagnosticHost from an
// authored profile (argv[1], a diagnostic-host asset) before anything else, records its own work into the flight
// recorder, then runs one mode (argv[2]) with its defect, or without it when argv[3] is "repaired". Nothing in it wires
// a crash handler, a recorder trigger or a bundle writer by hand: the profile's capture directory gets the bundle. The
// parent (crd-perf-tests) reads it: the trigger, the events before it, the replay recipe, the build and this
// executable's own identity. DIAG.12 runs every mode of the same application faulty, then repaired.
//
//   invariant       a refund larger than the payment fails a CRD_DIAG_INVARIANT (a fatal invariant)
//   assert          the same defect reported through crd's own assert path while headless (it aborts)
//   use-after-free  an order written after its release: the DiagnosticAllocator's quarantine names the write, its
//                   event carries the allocation and free sites, and a fatal invariant ends the run
//   wait            two jobs each wait on the other's counter: the watchdog window the host gives the pool reports the
//                   wait cycle as a hang, the bundle is written while the jobs stay parked, and the run exits 5
//   race            two jobs add to one plain counter: ThreadSanitizer (halt_on_error) aborts the run; no other build
//                   can catch it, so the mode announces sanitizer "none" there and the harness says InstrumentAbsent
// A repaired run prints CRD_HOST_CLEAN with the bundles written and exits 0; an unknown mode exits 2.
#include "specimen_common.hpp"

#include <crd/containers/string.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/diagnostic_allocator.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_triggers.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <thread>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers; // CRD_DIAG_INVARIANT names cont::StringView

constexpr cp::EventCode kOrderEvent      = 0x0042U;
constexpr cp::EventCode kStaleWriteEvent = 0x0043U;

// use-after-free: the violation handler records the write with its sites as program counters, which the bundle's
// module list resolves offline.
struct StaleWrite
{
    crd::memory::DiagnosticAllocator* heap     = nullptr;
    cp::FlightRecorder*               recorder = nullptr;
};

void on_violation(const crd::memory::Violation& v, void* user)
{
    const auto* const watch    = static_cast<const StaleWrite*>(user);
    void*             alloc_pc = nullptr;
    void*             free_pc  = nullptr;
    (void)watch->heap->stack_frames(v.alloc_stack, &alloc_pc, 1U);
    (void)watch->heap->stack_frames(v.free_stack, &free_pc, 1U);
    char text[64];
    (void)std::snprintf(text, sizeof(text), "an order was written after free at byte %zu", v.byte_offset);
    watch->recorder->record(kStaleWriteEvent, cp::Severity::DeveloperAssertion, cont::StringView{text},
                            static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(alloc_pc)),
                            static_cast<crd::u64>(reinterpret_cast<std::uintptr_t>(free_pc)));
}

void use_after_free(cp::DiagnosticHost& host, bool repaired)
{
    crd::memory::GrowableTlsfAllocator orders{crd::usize{64} << 10U, nullptr, "orders"};
    crd::memory::DiagnosticConfig      cfg;
    cfg.quarantine_capacity_bytes = 4096U; // a released order stays quarantined, so a later write is still seen
    crd::memory::DiagnosticAllocator heap{&orders, cfg, nullptr, "orders-diagnostic"};
    StaleWrite                       watch{&heap, &host.recorder()};
    heap.set_violation_handler(&on_violation, &watch);

    auto* const order = static_cast<crd::u32*>(heap.allocate(4U * sizeof(crd::u32), alignof(crd::u32)));
    order[0]          = 7U;
    heap.deallocate(order);
    if (!repaired)
    {
        *static_cast<volatile crd::u32*>(order) = 8U; // the defect: the order is updated after its release
    }
    CRD_DIAG_INVARIANT(heap.check_all_redzones() == 0U, 0x0712U, "a released order was written after free");
}

// wait and race: the jobs see one of these through JobDecl::data.
struct Shared
{
    bool                             repaired = false;
    std::atomic<crd::jobs::Counter*> first{nullptr};
    std::atomic<crd::jobs::Counter*> second{nullptr};
    int                              plain = 0; // race: the defect adds to this with no ordering
    std::atomic<int>                 counted{0};
};

crd::jobs::Counter* published(const std::atomic<crd::jobs::Counter*>& c)
{
    crd::jobs::Counter* p = c.load(std::memory_order_acquire);
    while (p == nullptr)
    {
        std::this_thread::yield(); // keeps the worker executing until main publishes the counter
        p = c.load(std::memory_order_acquire);
    }
    return p;
}

void first_job(void* data) noexcept
{
    crd::jobs::wait(published(static_cast<Shared*>(data)->second));
}

void second_job(void* data) noexcept
{
    auto* const s = static_cast<Shared*>(data);
    if (!s->repaired)
    {
        crd::jobs::wait(published(s->first)); // the defect: each job waits for the other
    }
}

void race_job(void* data) noexcept
{
    auto* const s = static_cast<Shared*>(data);
    for (int i = 0; i < 200000; ++i)
    {
        if (s->repaired)
        {
            s->counted.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            s->plain += 1;
        }
    }
}

void start_jobs(const cp::DiagnosticHost& host)
{
    crd::jobs::Config jobs;
    jobs.num_threads = 3U; // main plus two workers, so the two jobs run at once
    host.configure_jobs(jobs);
    crd::jobs::init(jobs);
}

void wait_cycle(cp::DiagnosticHost& host, Shared& s)
{
    start_jobs(host);
    crd::jobs::JobDecl a{};
    a.fn   = &first_job;
    a.data = &s;
    crd::jobs::JobDecl b{};
    b.fn   = &second_job;
    b.data = &s;
    s.first.store(crd::jobs::run(a), std::memory_order_release);
    s.second.store(crd::jobs::run(b), std::memory_order_release);
    if (s.repaired)
    {
        crd::jobs::wait(s.first.load(std::memory_order_acquire)); // the first job released the second's counter
        crd::jobs::shutdown();
        return;
    }
    // The parked jobs never finish, so the pool cannot shut down: once the hang bundle is written the run ends here.
    for (crd::u32 tick = 0U; tick < 2000U && host.bundles_written() == 0U; ++tick)
    {
        host.recorder().record(kOrderEvent, cp::Severity::Info, cont::StringView{"waiting"}, tick, 0U);
        (void)host.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("CRD_HOST_HANG bundles=%llu\n", static_cast<unsigned long long>(host.bundles_written()));
    std::fflush(stdout);
    std::_Exit(host.bundles_written() > 0U ? 5 : 6);
}

void job_race(const cp::DiagnosticHost& host, Shared& s)
{
    start_jobs(host);
    crd::jobs::JobDecl jobs[2] = {};
    for (crd::jobs::JobDecl& j : jobs)
    {
        j.fn   = &race_job;
        j.data = &s;
    }
    crd::jobs::wait(crd::jobs::run(std::span<const crd::jobs::JobDecl>{jobs}));
    crd::jobs::shutdown();
    std::printf("CRD_HOST_TALLY %d\n", s.repaired ? s.counted.load() : s.plain);
}

// A refund larger than the payment makes the balance negative.
bool balance_holds(bool repaired)
{
    crd::i64 paid = 0;
    for (crd::i64 i = 0; i < 40; ++i)
    {
        paid += i;
    }
    const crd::i64 refunded = repaired ? paid : paid + 1;
    return paid - refunded >= 0;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    const bool race = argc >= 3 && std::strcmp(argv[2], "race") == 0;
    // The race is a ThreadSanitizer route: under any other build, ASan included, it is absent.
    const char* sanitizer = CRD_DIAG_SPECIMEN_SANITIZER;
#if !CRD_DIAG_HAS_TSAN
    if (race)
    {
        sanitizer = "none";
    }
#endif
    std::printf("CRD_DIAG_IDENTITY=%s\nCRD_DIAG_SANITIZER=%s\n", CRD_DIAG_SPECIMEN_ID, sanitizer);
    std::fflush(stdout);
    const char* const modes[] = {"invariant", "assert", "use-after-free", "wait", "race"};
    bool              known   = false;
    for (const char* m : modes)
    {
        known = known || (argc >= 3 && std::strcmp(argv[2], m) == 0);
    }
    if (!known || (argc == 4 && std::strcmp(argv[3], "repaired") != 0) || argc > 4)
    {
        std::printf("usage: diag_host_specimen <profile> invariant|assert|use-after-free|wait|race [repaired]\n");
        return 2;
    }
    const bool        repaired = argc == 4;
    const char* const mode     = argv[2];

    cp::DiagnosticHostProfile profile;
    cont::String              reason;
    if (!cp::load_diagnostic_host_profile(argv[1], profile, reason))
    {
        std::printf("CRD_HOST_PROFILE_REFUSED %s\n", reason.c_str());
        return 3;
    }
    static cp::DiagnosticHost host; // static: the fatal path runs while main's frame is still live
    const cp::DiagHostStart   start = host.start(profile, reason);
    if (start != cp::DiagHostStart::Started)
    {
        std::printf("CRD_HOST_REFUSED %s %s\n", cp::diag_host_start_name(start).data(), reason.c_str());
        return 4;
    }
    std::printf("CRD_HOST_STARTED crash=%s flight=%s faults=%s\n", cp::diag_mode_state_name(host.modes().crash).data(),
                cp::diag_mode_state_name(host.modes().flight).data(),
                cp::diag_mode_state_name(host.modes().faults).data());
    std::fflush(stdout);
    host.recorder().set_replay_recipe("crd-diag-host-specimen <profile> <mode> [repaired]");

    char text[32];
    for (crd::u32 i = 0U; i < 40U; ++i)
    {
        (void)std::snprintf(text, sizeof(text), "order %u", static_cast<unsigned>(i));
        host.recorder().record(kOrderEvent, cp::Severity::Info, cont::StringView{text}, i, 0U);
    }

    static Shared shared; // static: a parked job still reads it after main's frame is gone
    shared.repaired = repaired;
    if (std::strcmp(mode, "invariant") == 0)
    {
        CRD_DIAG_INVARIANT(balance_holds(repaired), 0x0711U, "the order total went negative");
    }
    else if (std::strcmp(mode, "assert") == 0 && !balance_holds(repaired))
    {
        (void)crd::detail::report_assert_failure("total >= 0", __FILE__, __LINE__, "the order total went negative");
    }
    else if (std::strcmp(mode, "use-after-free") == 0)
    {
        use_after_free(host, repaired);
    }
    else if (std::strcmp(mode, "wait") == 0)
    {
        wait_cycle(host, shared);
    }
    else if (race)
    {
        job_race(host, shared);
    }
    if (!repaired)
    {
        // Nothing caught the defect (or the fatal path returned): exit 0 without the clean line, which the parent
        // reads as a missed detection, or as InstrumentAbsent for the race outside ThreadSanitizer.
        return 0;
    }
    (void)host.poll();
    std::printf("CRD_HOST_CLEAN bundles=%llu\n", static_cast<unsigned long long>(host.bundles_written()));
    std::fflush(stdout);
    host.stop();
    return 0;
}
