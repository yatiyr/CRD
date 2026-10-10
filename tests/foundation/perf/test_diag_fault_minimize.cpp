// DIAG.9c -- minimizing a fault plan that reproduces a failure (crd/perf/fault_minimize.hpp).
//
// The reproductions here are written in the test. A synthetic program makes a fixed sequence of calls, each consulting
// one site and counting that site's hits from 0 as an armed injector does (a read consults failed-read, then
// partial-read, and the first that fires applies, as FaultInjector::io_hook does); which hits fire comes from
// fault_plan_fires, the injector's own pure decision. Its failure is a predicate over the faults that applied, so the
// smallest plan is known before the minimizer runs, and 1-minimality is checked by running the program without each
// remaining fault, not by asking the minimizer. These cases are pure and run on every lane, shipping included.
//
// One case drives a real FaultInjector through a FaultAllocator: there the plan is armed for every run, so on a
// shipping lane (no fault seams) the arm is refused, nothing fails, and the minimizer answers NotReproduced with the
// plan unchanged. The cook in crd-resources-tests minimizes a seeded plan against the real cooker.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9c); ADR-0133.

#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/fault_minimize.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/minimize.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;
namespace mz   = crd::minimize;
using crd::u32;
using crd::u64;
using crd::usize;

constexpr cp::DiagAuthoritySet kInject = cp::authority_bit(cp::DiagAuthority::Inject);

cp::FaultPlan plan_of(cont::StringView text)
{
    cp::FaultPlan plan;
    cont::String  reason;
    const bool    parsed = cp::parse_fault_plan(text, plan, reason);
    INFO(reason.c_str());
    REQUIRE(parsed);
    return plan;
}

cont::String text_of(const cp::FaultPlan& plan)
{
    cont::String out;
    cp::fault_plan_to_text(plan, out);
    return out;
}

// One fault that applied during a synthetic run.
struct Applied
{
    cp::FaultSite site  = cp::FaultSite::AllocationFailure;
    u64           hit   = 0U;
    u64           value = 0U;
};

enum class Call : crd::u8
{
    Alloc,
    Read,
    Submit,
};

// A synthetic program: its calls in order, and the faults that must all apply for it to fail.
struct Program
{
    cont::Array<Call>    calls;
    cont::Array<Applied> needed;
};

// Run `program` under `plan`: count each site's hits from 0, apply what fires, fill `hits`, and answer whether every
// needed fault applied.
bool run_program(const Program& program, const cp::FaultPlan& plan, cp::FaultRunHits& hits)
{
    hits = cp::FaultRunHits{};
    cont::Array<Applied> applied;
    const auto           decide = [&](cp::FaultSite site, bool& fired, u64& hit, u64& value)
    {
        hit = hits.hits[static_cast<u32>(site)]++;
        u32 rule = 0U;
        fired    = cp::fault_plan_fires(plan, site, hit, &rule);
        value    = fired ? plan.rules[rule].value : 0U;
    };
    for (const Call c : program.calls)
    {
        bool fired = false;
        u64  hit   = 0U;
        u64  value = 0U;
        switch (c)
        {
        case Call::Alloc:
            decide(cp::FaultSite::AllocationFailure, fired, hit, value);
            if (fired)
            {
                applied.push_back(Applied{cp::FaultSite::AllocationFailure, hit, value});
            }
            break;
        case Call::Submit:
            decide(cp::FaultSite::DeviceLoss, fired, hit, value);
            if (fired)
            {
                applied.push_back(Applied{cp::FaultSite::DeviceLoss, hit, value});
            }
            break;
        case Call::Read:
        {
            // Both read sites count the read; the first that fires applies.
            bool f_fired = false;
            u64  f_hit   = 0U;
            u64  f_value = 0U;
            decide(cp::FaultSite::FailedRead, f_fired, f_hit, f_value);
            decide(cp::FaultSite::PartialRead, fired, hit, value);
            if (f_fired)
            {
                applied.push_back(Applied{cp::FaultSite::FailedRead, f_hit, f_value});
            }
            else if (fired)
            {
                applied.push_back(Applied{cp::FaultSite::PartialRead, hit, value});
            }
            break;
        }
        }
    }
    for (const Applied& need : program.needed)
    {
        bool found = false;
        for (const Applied& a : applied)
        {
            if (a.site == need.site && a.hit == need.hit && a.value == need.value)
            {
                found = true;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

bool program_run(void* user, const cp::FaultPlan& plan, cp::FaultRunHits& hits)
{
    return run_program(*static_cast<const Program*>(user), plan, hits);
}

bool fails(const Program& program, const cp::FaultPlan& plan)
{
    cp::FaultRunHits hits;
    return run_program(program, plan, hits);
}

// The plan without rule `drop`.
cp::FaultPlan without(const cp::FaultPlan& plan, u32 drop)
{
    cp::FaultPlan out = plan;
    out.rule_count    = 0U;
    for (u32 i = 0U; i < plan.rule_count; ++i)
    {
        if (i != drop)
        {
            out.rules[out.rule_count++] = plan.rules[i];
        }
    }
    return out;
}

// Independently of the minimizer: `plan` makes `program` fail, and no plan with one rule (one hit) fewer does.
void check_one_minimal(const Program& program, const cp::FaultPlan& plan)
{
    CHECK(fails(program, plan));
    for (u32 i = 0U; i < plan.rule_count; ++i)
    {
        INFO(i);
        CHECK(plan.rules[i].hit_limit == 1U);
        if (plan.rule_count > 1U)
        {
            CHECK_FALSE(fails(program, without(plan, i)));
        }
    }
}

// 60 reads, an allocation every third read and a submission every twelfth.
Program mixed_program()
{
    Program p;
    for (u32 i = 0U; i < 60U; ++i)
    {
        p.calls.push_back(Call::Read);
        if (i % 3U == 0U)
        {
            p.calls.push_back(Call::Alloc);
        }
        if (i % 12U == 0U)
        {
            p.calls.push_back(Call::Submit);
        }
    }
    return p;
}
} // namespace

TEST_CASE("diag 9c fault minimize: a plan failing every allocation shrinks to the one hit the failure needs",
          "[perf][diag9c][minimize]")
{
    Program program;
    for (u32 i = 0U; i < 40U; ++i)
    {
        program.calls.push_back(Call::Alloc);
    }
    program.needed.push_back(Applied{cp::FaultSite::AllocationFailure, 13U, 0U});

    const cp::FaultPlan plan = plan_of("seed=5;allocation-failure@0+0%1000000");
    REQUIRE(fails(program, plan));

    mz::Budget budget;
    budget.runs_per_candidate = 2U;
    cp::FaultMinimizeResult result;
    cp::minimize_fault_plan(plan, &program_run, &program, budget, result);
    CHECK(result.status == mz::Status::Minimal);
    CHECK(result.plan_error == cp::FaultPlanError::None);
    CHECK(result.pinned);
    CHECK(result.hits == 1U);
    CHECK(text_of(result.plan) == cont::String{"seed=0;allocation-failure@13+1%1000000"});
    CHECK(result.stats.runs <= budget.max_runs);
    CHECK(result.stats.flaky == 0U);
    check_one_minimal(program, result.plan);
}

TEST_CASE("diag 9c fault minimize: a seeded four-rule plan shrinks to the two hits the failure needs, seed-free",
          "[perf][diag9c][minimize]")
{
    const cp::FaultPlan plan = plan_of("seed=99;failed-read@0+0%300000;partial-read@0+0%500000=3;"
                                       "allocation-failure@0+20%1000000;device-loss@0+0%1000000");
    // The failure needs a read the seeded failed-read rule fails (the first from read 20) and a later read the seeded
    // partial-read rule cuts short while failed-read leaves it alone (the first from read 31).
    u64  r1 = 0U;
    u64  r2 = 0U;
    bool f1 = false;
    bool f2 = false;
    for (u64 r = 20U; r < 60U && !f1; ++r)
    {
        if (cp::fault_plan_fires(plan, cp::FaultSite::FailedRead, r))
        {
            r1 = r;
            f1 = true;
        }
    }
    for (u64 r = 31U; r < 60U && !f2; ++r)
    {
        if (cp::fault_plan_fires(plan, cp::FaultSite::PartialRead, r) &&
            !cp::fault_plan_fires(plan, cp::FaultSite::FailedRead, r))
        {
            r2 = r;
            f2 = true;
        }
    }
    REQUIRE(f1);
    REQUIRE(f2);
    Program program = mixed_program();
    program.needed.push_back(Applied{cp::FaultSite::FailedRead, r1, 0U});
    program.needed.push_back(Applied{cp::FaultSite::PartialRead, r2, 3U});
    REQUIRE(fails(program, plan));

    cp::FaultMinimizeResult result;
    cp::minimize_fault_plan(plan, &program_run, &program, mz::Budget{}, result);
    CHECK(result.status == mz::Status::Minimal);
    CHECK(result.pinned);
    CHECK(result.hits == 2U);
    char expected[160];
    (void)std::snprintf(expected, sizeof(expected), "seed=0;failed-read@%llu+1%%1000000;partial-read@%llu+1%%1000000=3",
                        static_cast<unsigned long long>(r1), static_cast<unsigned long long>(r2));
    INFO(text_of(result.plan).c_str());
    CHECK(text_of(result.plan) == cont::String{expected});
    check_one_minimal(program, result.plan);

    // The seed no longer matters: the same rules under another seed fail the same way.
    cp::FaultPlan reseeded = result.plan;
    reseeded.seed          = 123456789U;
    CHECK(fails(program, reseeded));
}

TEST_CASE("diag 9c fault minimize: a run budget stops early with a plan that still reproduces",
          "[perf][diag9c][minimize]")
{
    Program program = mixed_program();
    program.needed.push_back(Applied{cp::FaultSite::AllocationFailure, 7U, 0U});
    const cp::FaultPlan plan = plan_of("seed=1;allocation-failure@0+0%1000000;failed-read@0+0%500000");
    REQUIRE(fails(program, plan));

    mz::Budget budget;
    budget.runs_per_candidate = 1U;
    budget.max_runs           = 6U;
    cp::FaultMinimizeResult result;
    cp::minimize_fault_plan(plan, &program_run, &program, budget, result);
    CHECK(result.status == mz::Status::Budget);
    CHECK(result.stats.runs <= budget.max_runs);
    CHECK(fails(program, result.plan));
    CHECK(cp::validate_fault_plan(result.plan) == cp::FaultPlanError::None);
}

TEST_CASE("diag 9c fault minimize: a plan that does not reproduce, an invalid plan and a race are refused",
          "[perf][diag9c][minimize]")
{
    Program program = mixed_program();
    program.needed.push_back(Applied{cp::FaultSite::DeviceLoss, 3U, 0U});

    SECTION("a plan that never reaches the needed fault is returned as given")
    {
        const cp::FaultPlan     plan = plan_of("seed=2;allocation-failure@0+0%1000000");
        cp::FaultMinimizeResult result;
        cp::minimize_fault_plan(plan, &program_run, &program, mz::Budget{}, result);
        CHECK(result.status == mz::Status::NotReproduced);
        CHECK(text_of(result.plan) == text_of(plan));
        CHECK(result.stats.candidates == 1U);
        CHECK(result.stats.runs == 1U);
    }
    SECTION("an invalid plan is refused before anything runs")
    {
        cp::FaultPlan plan = plan_of("seed=2;device-loss@0+0%1000000");
        plan.rules[0].probability_ppm = 0U;
        cp::FaultMinimizeResult result;
        cp::minimize_fault_plan(plan, &program_run, &program, mz::Budget{}, result);
        CHECK(result.status == mz::Status::NotReproduced);
        CHECK(result.plan_error == cp::FaultPlanError::BadProbability);
        CHECK(result.stats.runs == 0U);
    }
    SECTION("a reproduction that fails on alternate runs is a race, not a reproduction")
    {
        struct Racy
        {
            const Program* program = nullptr;
            u64            calls   = 0U;
        };
        Racy       racy{&program, 0U};
        const auto run = [](void* user, const cp::FaultPlan& plan, cp::FaultRunHits& hits)
        {
            auto&     r    = *static_cast<Racy*>(user);
            const u64 call = r.calls++;
            return run_program(*r.program, plan, hits) && call % 2U == 0U;
        };
        const cp::FaultPlan plan = plan_of("seed=2;device-loss@0+0%1000000");
        REQUIRE(fails(program, plan));
        cp::FaultMinimizeResult result;
        cp::minimize_fault_plan(plan, run, &racy, mz::Budget{}, result);
        CHECK(result.status == mz::Status::NotReproduced);
        CHECK(result.stats.flaky == 1U);
        CHECK(text_of(result.plan) == text_of(plan));
    }
}

namespace
{
// A real injector behind a FaultAllocator: each run arms the plan (restarting every count), makes 20 attempts and
// fails when attempts 6 and 11 both came back null.
struct AllocatorRun
{
    cp::FaultInjector injector;
    u32               arms_refused = 0U;
    u64               fired        = 0U; // the last run's fired faults
};

bool allocator_run(void* user, const cp::FaultPlan& plan, cp::FaultRunHits& hits)
{
    auto& r = *static_cast<AllocatorRun*>(user);
    hits    = cp::FaultRunHits{};
    if (r.injector.arm(plan, kInject) != cp::FaultArmStatus::Armed)
    {
        ++r.arms_refused;
        return false;
    }
    crd::memory::LinearAllocator linear(1U << 12U, nullptr, "minimize-linear");
    cp::FaultAllocator           faulty(&linear, &r.injector);
    bool                         null_at[20] = {};
    for (u32 i = 0U; i < 20U; ++i)
    {
        null_at[i] = faulty.try_allocate(16U) == nullptr;
    }
    r.fired = 0U;
    for (u32 s = 0U; s < cp::kFaultSiteCount; ++s)
    {
        hits.hits[s] = r.injector.hits(static_cast<cp::FaultSite>(s));
        r.fired += r.injector.fired(static_cast<cp::FaultSite>(s));
    }
    r.injector.disarm();
    return null_at[6] && null_at[11];
}
} // namespace

TEST_CASE("diag 9c fault minimize: a seeded plan armed on a real injector shrinks to the two attempts that fail",
          "[perf][diag9c][minimize]")
{
    // The first seed whose half-share rule fails both attempts 6 and 11 (the scenario, not the minimizer, picks it).
    cp::FaultPlan plan = plan_of("seed=0;allocation-failure@0+0%500000");
    bool          seeded = false;
    for (u64 seed = 1U; seed < 1000U && !seeded; ++seed)
    {
        plan.seed = seed;
        seeded    = cp::fault_plan_fires(plan, cp::FaultSite::AllocationFailure, 6U) &&
                 cp::fault_plan_fires(plan, cp::FaultSite::AllocationFailure, 11U);
    }
    REQUIRE(seeded);

    AllocatorRun            run;
    cp::FaultMinimizeResult result;
    cp::minimize_fault_plan(plan, &allocator_run, &run, mz::Budget{}, result);
    if (!crd::fault_seams_compiled())
    {
        // Shipping: the arm is refused, the run cannot fail, and the plan is returned as given.
        CHECK(result.status == mz::Status::NotReproduced);
        CHECK(run.arms_refused == 1U);
        CHECK(text_of(result.plan) == text_of(plan));
        return;
    }
    CHECK(run.arms_refused == 0U);
    CHECK(result.status == mz::Status::Minimal);
    CHECK(result.pinned);
    CHECK(text_of(result.plan) ==
          cont::String{"seed=0;allocation-failure@6+1%1000000;allocation-failure@11+1%1000000"});
    // Armed alone, the result fails attempts 6 and 11 and nothing else.
    cp::FaultRunHits hits;
    CHECK(allocator_run(&run, result.plan, hits));
    CHECK(hits.hits[static_cast<u32>(cp::FaultSite::AllocationFailure)] == 20U);
    CHECK(run.fired == 2U);
}
