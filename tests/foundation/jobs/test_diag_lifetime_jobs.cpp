// DIAG.10c -- the job system's own lifetimes in the lifetime ledger, and the ledger under concurrent recording.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): every fiber stack is one MappedRegion of its
// reserved size (guard page included) from jobs::init to jobs::shutdown, so a shutdown gives back exactly what the
// init took; an installed job observer is one CallbackRegistration until it is cleared (replacing one keeps it); and
// recording is wait-free, so concurrent acquisitions and releases from many jobs balance exactly.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/jobs/observer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>

namespace
{
using crd::LifetimeClass;
using crd::LifetimeVerdict;

crd::LifetimeReport since(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot());
}

void begin_noop(crd::jobs::FiberHandle /*fiber*/, crd::u8 /*thread*/, crd::u8 /*priority*/,
                crd::u8 /*tier*/) noexcept
{
}

struct Hammer
{
    crd::LifetimeLedger* ledger = nullptr;
    crd::u32             rounds = 0U;
};

void hammer(void* data) noexcept
{
    const auto* h = static_cast<const Hammer*>(data);
    for (crd::u32 i = 0U; i < h->rounds; ++i)
    {
        const auto c = static_cast<LifetimeClass>(i % crd::kLifetimeClassCount);
        h->ledger->acquired(c, 64U);
        (void)h->ledger->released(c, 64U);
    }
}
} // namespace

TEST_CASE("diag10c: jobs init holds each fiber stack as a mapped region and shutdown gives every one back",
          "[jobs][lifetime][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads        = 2U;
    cfg.small_fiber_count  = 4U;
    cfg.medium_fiber_count = 2U;
    cfg.large_fiber_count  = 1U;

    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    crd::jobs::init(cfg);
    const crd::LifetimeReport running = since(start);
    crd::jobs::shutdown();
    const crd::LifetimeReport after = since(start);

    // Seven stacks of 64 KiB, 512 KiB and 2 MiB usable bytes, each with its guard page.
    constexpr crd::i64 usable_bytes = (4 * 64 + 2 * 512 + 1 * 2048) * 1024;
    CHECK(running.verdict == LifetimeVerdict::Outstanding);
    CHECK(running.outstanding == crd::lifetime_class_bit(LifetimeClass::MappedRegion));
    CHECK(running.live_change(LifetimeClass::MappedRegion) == 7);
    CHECK(running.byte_change(LifetimeClass::MappedRegion) > usable_bytes);
    CHECK(running.byte_change(LifetimeClass::MappedRegion) <= usable_bytes + 7 * 65536);

    char line[160];
    (void)crd::format_lifetime_report(after, line, sizeof(line));
    INFO("after shutdown: " << line);
    CHECK(after.verdict == LifetimeVerdict::Balanced);
}

TEST_CASE("diag10c: an installed job observer is one callback registration until it is cleared",
          "[jobs][lifetime][diag10c]")
{
    crd::jobs::JobObserver first;
    first.on_job_begin = &begin_noop;
    crd::jobs::JobObserver second;
    second.on_job_begin = &begin_noop;

    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    crd::jobs::set_observer(&first);
    CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 1);
    crd::jobs::set_observer(&second); // a replacement, not a second registration
    CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 1);
    crd::jobs::set_observer(nullptr);
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
    crd::jobs::set_observer(nullptr); // clearing nothing releases nothing
    const crd::LifetimeReport r = since(start);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(r.underflowed == 0U);
}

TEST_CASE("diag10c: concurrent recording from many jobs balances exactly", "[jobs][lifetime][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    crd::LifetimeLedger ledger;
    constexpr crd::u32  job_count = 32U;
    constexpr crd::u32  rounds    = 2000U;
    Hammer              h{&ledger, rounds};
    crd::jobs::JobDecl  decls[job_count]{};
    for (crd::jobs::JobDecl& d : decls)
    {
        d.fn   = &hammer;
        d.data = &h;
    }
    crd::jobs::wait(crd::jobs::run(std::span<const crd::jobs::JobDecl>(decls, job_count)));
    crd::jobs::shutdown();

    const crd::LifetimeSnapshot s = ledger.snapshot();
    crd::u64                    acquired = 0U;
    for (const crd::LifetimeCounts& c : s.classes)
    {
        CHECK(c.live == 0U);
        CHECK(c.live_bytes == 0U);
        CHECK(c.unmatched_releases == 0U);
        CHECK(c.acquired == c.released);
        CHECK(c.high_water >= 1U);
        CHECK(c.high_water <= job_count);
        acquired += c.acquired;
    }
    CHECK(acquired == static_cast<crd::u64>(job_count) * rounds);
}
