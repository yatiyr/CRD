// DIAG.10c -- the floating-point mode at the crd-jobs boundary. Declared contract (docs/design/runtime-diagnostics.md,
// DIAG.10c): every job starts in the declared mode (crd::declared_fp_environment()), whatever mode the thread that
// submitted or runs it is in; a mode a job sets is its own, kept across its waits and thread migrations and gone when
// it ends; and no job changes the mode of the thread that ran it. Before DIAG.10c the Linux fiber switch saved neither
// MXCSR nor the x87 control word, so a job's flush-to-zero or rounding mode reached the scheduler stack and the next
// job, and a waiting job resumed in another job's mode: wrong float results without any crash. A single-thread pool
// makes the interleaving deterministic (thread 0 runs every job while it waits); the last case uses four threads so
// waits resume on other workers. Every case restores the test thread's mode on exit, also when an assertion fails.

#include <crd/containers/static_array.hpp>
#include <crd/core/fp_environment.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>

namespace
{
using crd::FpEnvironment;
using crd::FpRounding;
using crd::u16;
using crd::u32;

// A mode far from the declared one in every field that can be set without unmasking an exception.
FpEnvironment odd_mode(FpRounding r, bool ftz, bool daz)
{
    FpEnvironment e = crd::declared_fp_environment();
    e.mxcsr         = (e.mxcsr & ~crd::kMxcsrRoundingMask) | (static_cast<u32>(r) << crd::kMxcsrRoundingShift);
    if (ftz)
    {
        e.mxcsr |= crd::kMxcsrFlushToZero;
    }
    if (daz)
    {
        e.mxcsr |= crd::kMxcsrDenormalsAreZero;
    }
    if (e.x87_read)
    {
        // precision 24-bit, x87 rounding as `r`
        e.x87_control = static_cast<u16>((e.x87_control & ~(crd::kX87PrecisionMask | crd::kX87RoundingMask)) |
                                         (static_cast<u32>(r) << crd::kX87RoundingShift));
    }
    return e;
}

struct Probe
{
    FpEnvironment seen{};
};

// A job that records the mode it starts in.
void record_mode(void* data) noexcept
{
    static_cast<Probe*>(data)->seen = crd::read_fp_environment();
}

// A job that records the mode it starts in, then sets another one and ends without putting the old one back.
struct Leaver
{
    FpEnvironment seen{};
    FpEnvironment set{};
};
void record_then_leave_mode(void* data) noexcept
{
    auto* const l = static_cast<Leaver*>(data);
    l->seen       = crd::read_fp_environment();
    crd::write_fp_environment(l->set);
}

// A job that sets its own mode, waits on a child that leaves another mode behind, and records its mode after the wait.
struct Parent
{
    FpEnvironment own{};
    Leaver        child{};
    FpEnvironment after_wait{};
};
void set_mode_wait_on_child(void* data) noexcept
{
    auto* const p = static_cast<Parent*>(data);
    crd::write_fp_environment(p->own);
    crd::jobs::JobDecl c{};
    c.fn   = &record_then_leave_mode;
    c.data = &p->child;
    crd::jobs::wait(crd::jobs::run(c));
    p->after_wait = crd::read_fp_environment();
}

void run_one(void (*fn)(void*), void* data)
{
    crd::jobs::JobDecl j{};
    j.fn   = fn;
    j.data = data;
    crd::jobs::wait(crd::jobs::run(j));
}
} // namespace

TEST_CASE("diag10c: a job starts in the declared floating-point mode, not its submitting thread's",
          "[jobs][fp][diag10c]")
{
    const crd::ScopedFpEnvironment keep(crd::read_fp_environment());
    crd::jobs::Config              cfg;
    cfg.num_threads = 1U;
    crd::jobs::init(cfg);

    const FpEnvironment host = odd_mode(FpRounding::TowardZero, true, true);
    Probe               p;
    {
        const crd::ScopedFpEnvironment host_mode(host);
        run_one(&record_mode, &p);
        CHECK(crd::read_fp_environment() == host); // the job did not change the submitting thread's mode
    }
    crd::jobs::shutdown();

    INFO("job mxcsr=" << p.seen.mxcsr << " x87=" << p.seen.x87_control);
    CHECK(crd::fp_differences(p.seen, crd::declared_fp_environment()) == 0U);
}

TEST_CASE("diag10c: a mode a job leaves set ends with the job", "[jobs][fp][diag10c]")
{
    const crd::ScopedFpEnvironment keep(crd::read_fp_environment());
    REQUIRE(crd::read_fp_environment() == crd::declared_fp_environment());
    crd::jobs::Config cfg;
    cfg.num_threads = 1U;
    crd::jobs::init(cfg);

    Leaver first;
    first.set = odd_mode(FpRounding::Up, true, false);
    run_one(&record_then_leave_mode, &first);
    const FpEnvironment thread_after = crd::read_fp_environment(); // thread 0 ran the job on its scheduler stack

    Probe next;
    run_one(&record_mode, &next);
    crd::jobs::shutdown();

    INFO("thread mxcsr=" << thread_after.mxcsr << " next job mxcsr=" << next.seen.mxcsr);
    CHECK(crd::fp_differences(first.seen, crd::declared_fp_environment()) == 0U);
    CHECK(crd::fp_differences(thread_after, crd::declared_fp_environment()) == 0U);
    CHECK(crd::fp_differences(next.seen, crd::declared_fp_environment()) == 0U);
}

TEST_CASE("diag10c: a job keeps its mode across its own wait and the job run meanwhile starts clean",
          "[jobs][fp][diag10c]")
{
    const crd::ScopedFpEnvironment keep(crd::read_fp_environment());
    REQUIRE(crd::read_fp_environment() == crd::declared_fp_environment());
    crd::jobs::Config cfg;
    cfg.num_threads = 1U;
    crd::jobs::init(cfg);

    Parent p;
    p.own       = odd_mode(FpRounding::TowardZero, true, true);
    p.child.set = odd_mode(FpRounding::Down, false, true);
    run_one(&set_mode_wait_on_child, &p);
    const FpEnvironment thread_after = crd::read_fp_environment();
    crd::jobs::shutdown();

    INFO("child start mxcsr=" << p.child.seen.mxcsr << " parent after wait mxcsr=" << p.after_wait.mxcsr);
    CHECK(crd::fp_differences(p.child.seen, crd::declared_fp_environment()) == 0U); // not the parent's mode
    CHECK(crd::fp_differences(p.after_wait, p.own) == 0U);                          // not the child's leftover
    CHECK(crd::fp_differences(thread_after, crd::declared_fp_environment()) == 0U);
}

TEST_CASE("diag10c: on four threads each job's mode follows the job across waits and workers", "[jobs][fp][diag10c]")
{
    const crd::ScopedFpEnvironment keep(crd::read_fp_environment());
    REQUIRE(crd::read_fp_environment() == crd::declared_fp_environment());
    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    constexpr u32                                                  parent_count = 8U;
    constexpr u32                                                  round_count  = 16U;
    crd::containers::StaticArray<Parent, parent_count>             parents{};
    crd::containers::StaticArray<crd::jobs::JobDecl, parent_count> jobs{};
    u32                                                            child_dirty  = 0U;
    u32                                                            parent_wrong = 0U;
    for (u32 round = 0U; round < round_count; ++round)
    {
        for (u32 i = 0U; i < parent_count; ++i)
        {
            const auto r         = static_cast<FpRounding>((i + round) % 4U);
            parents[i]           = Parent{};
            parents[i].own       = odd_mode(r, (i & 1U) != 0U, (i & 2U) != 0U);
            parents[i].child.set = odd_mode(static_cast<FpRounding>((i + round + 1U) % 4U), (i & 1U) == 0U, true);
            jobs[i]              = crd::jobs::JobDecl{};
            jobs[i].fn           = &set_mode_wait_on_child;
            jobs[i].data         = &parents[i];
        }
        crd::jobs::wait(crd::jobs::run(std::span<const crd::jobs::JobDecl>{jobs.data(), jobs.size()}));
        for (u32 i = 0U; i < parent_count; ++i)
        {
            if (crd::fp_differences(parents[i].child.seen, crd::declared_fp_environment()) != 0U)
            {
                ++child_dirty;
            }
            if (crd::fp_differences(parents[i].after_wait, parents[i].own) != 0U)
            {
                ++parent_wrong;
            }
        }
    }
    const FpEnvironment thread_after = crd::read_fp_environment();
    crd::jobs::shutdown();

    CHECK(child_dirty == 0U);
    CHECK(parent_wrong == 0U);
    CHECK(crd::fp_differences(thread_after, crd::declared_fp_environment()) == 0U);
}
