// crd-perf -- minimizing a fault plan that reproduces a failure (DIAG.9c). See crd/perf/fault_minimize.hpp.

#include <crd/perf/fault_minimize.hpp>

#include <crd/containers/array.hpp>

namespace crd::perf
{
namespace
{
namespace mz = crd::minimize;

// One fired hit of a pinned plan.
struct FaultHit
{
    FaultSite site  = FaultSite::AllocationFailure;
    crd::u64  hit   = 0U;
    crd::u64  value = 0U;
};

// The caller's run, keeping the hits of the last run that reproduced.
class Runner
{
public:
    Runner(FaultRunOnce run, void* user) noexcept : m_run(run), m_user(user) {}

    bool operator()(const FaultPlan& plan)
    {
        FaultRunHits hits;
        const bool   failed = m_run(m_user, plan, hits);
        if (failed)
        {
            m_reproduced = hits;
        }
        return failed;
    }

    [[nodiscard]] const FaultRunHits& reproduced() const noexcept { return m_reproduced; }

private:
    FaultRunOnce m_run;
    void*        m_user;
    FaultRunHits m_reproduced;
};

// `hits` as a plan (seed 0, every rule always firing), consecutive hits of one site and value merged in one rule. False
// when it needs more than kFaultMaxRules rules. `hits` is sorted by site, then hit.
bool plan_of_hits(const cont::Array<FaultHit>& hits, bool trigger_flight, FaultPlan& out)
{
    FaultPlan plan;
    plan.seed           = 0U;
    plan.trigger_flight = trigger_flight;
    for (const FaultHit& h : hits)
    {
        if (plan.rule_count > 0U)
        {
            FaultRule& last = plan.rules[plan.rule_count - 1U];
            if (last.site == h.site && last.value == h.value && last.first_hit + last.hit_limit == h.hit)
            {
                ++last.hit_limit;
                continue;
            }
        }
        if (plan.rule_count == kFaultMaxRules)
        {
            return false;
        }
        plan.rules[plan.rule_count] = FaultRule{h.site, h.hit, 1U, kFaultPpmAlways, h.value};
        ++plan.rule_count;
    }
    if (plan.rule_count == 0U)
    {
        return false;
    }
    out = plan;
    return true;
}

// The hits `plan` fires on, each counted once (by the first rule that fires it), sorted by site, then hit. False when a
// rule is unbounded, the ranges hold more than kFaultMaxPinScan hits or more than kFaultMaxPinnedHits fire.
bool fired_hits(const FaultPlan& plan, cont::Array<FaultHit>& out)
{
    out.clear();
    crd::u64 scanned = 0U;
    for (crd::u32 i = 0U; i < plan.rule_count; ++i)
    {
        const FaultRule& r = plan.rules[i];
        if (r.hit_limit == 0U || r.hit_limit > kFaultMaxPinScan - scanned)
        {
            return false;
        }
        scanned += r.hit_limit;
        for (crd::u64 hit = r.first_hit; hit - r.first_hit < r.hit_limit; ++hit)
        {
            crd::u32 rule = 0U;
            if (!fault_plan_fires(plan, r.site, hit, &rule) || rule != i)
            {
                continue;
            }
            if (out.size() == kFaultMaxPinnedHits)
            {
                return false;
            }
            // Insert in (site, hit) order.
            const FaultHit fh{r.site, hit, plan.rules[rule].value};
            crd::usize     at = out.size();
            while (at > 0U && (out[at - 1U].site > fh.site || (out[at - 1U].site == fh.site && out[at - 1U].hit > hit)))
            {
                --at;
            }
            out.push_back(fh);
            for (crd::usize k = out.size() - 1U; k > at; --k)
            {
                out[k] = out[k - 1U];
            }
            out[at] = fh;
        }
    }
    return !out.empty();
}

// Narrow rule `i` of `cur` (step 2). Answers Budget when the budget ran out; `cur` reproduces either way.
mz::Status narrow_rule(FaultPlan& cur, crd::u32 i, Runner& runner, const mz::Budget& budget, mz::Stats& stats)
{
    const FaultRule rule = cur.rules[i];
    crd::u64        end  = 0U;
    if (rule.hit_limit == 0U)
    {
        const crd::u64 seen = runner.reproduced().hits[static_cast<crd::u32>(rule.site)];
        if (rule.first_hit == ~0ULL)
        {
            return mz::Status::Minimal; // nothing past it to bound
        }
        end                         = seen > rule.first_hit ? seen : rule.first_hit + 1U;
        FaultPlan bounded           = cur;
        bounded.rules[i].hit_limit  = end - rule.first_hit;
        const mz::Verdict v         = mz::judge(bounded, runner, budget, stats);
        if (v == mz::Verdict::Budget)
        {
            return mz::Status::Budget;
        }
        if (v == mz::Verdict::No)
        {
            return mz::Status::Minimal; // it needs hits past the reproducing run's count: keep it unbounded
        }
        cur = bounded;
    }
    else
    {
        end = rule.first_hit + rule.hit_limit;
    }

    // Raise the first hit toward `end` (an empty range is never judged).
    crd::u64   first      = rule.first_hit;
    const auto probe_from = [&](crd::u64 f)
    {
        FaultPlan candidate           = cur;
        candidate.rules[i].first_hit  = f;
        candidate.rules[i].hit_limit  = end - f;
        return mz::judge(candidate, runner, budget, stats);
    };
    const mz::Status low   = mz::bisect_toward(first, end, probe_from);
    cur.rules[i].first_hit = first;
    cur.rules[i].hit_limit = end - first;
    if (low == mz::Status::Budget)
    {
        return mz::Status::Budget;
    }

    // Lower the end toward `first` (again never judging an empty range).
    crd::u64   last      = end;
    const auto probe_end = [&](crd::u64 e)
    {
        FaultPlan candidate          = cur;
        candidate.rules[i].hit_limit = e - first;
        return mz::judge(candidate, runner, budget, stats);
    };
    const mz::Status high  = mz::bisect_toward(last, first, probe_end);
    cur.rules[i].hit_limit = last - first;
    return high;
}

} // namespace

void minimize_fault_plan(const FaultPlan& plan, FaultRunOnce run, void* user, const minimize::Budget& budget,
                         FaultMinimizeResult& out)
{
    out        = FaultMinimizeResult{};
    out.plan   = plan;
    out.status = mz::Status::NotReproduced;
    const FaultPlanError e = validate_fault_plan(plan);
    if (e != FaultPlanError::None || run == nullptr)
    {
        out.plan_error = e;
        return;
    }

    Runner runner(run, user);

    // 1. The input itself.
    const mz::Verdict first = mz::judge(plan, runner, budget, out.stats);
    if (first != mz::Verdict::Yes)
    {
        out.status = first == mz::Verdict::Budget ? mz::Status::Budget : mz::Status::NotReproduced;
        return;
    }
    FaultPlan cur = plan;

    // 2. Narrow every rule's range.
    for (crd::u32 i = 0U; i < cur.rule_count; ++i)
    {
        if (narrow_rule(cur, i, runner, budget, out.stats) == mz::Status::Budget)
        {
            out.plan   = cur;
            out.status = mz::Status::Budget;
            return;
        }
    }

    // 3. Pin the plan to the hits it fires on.
    cont::Array<FaultHit> hits;
    FaultPlan             pinned_plan;
    if (fired_hits(cur, hits) && plan_of_hits(hits, cur.trigger_flight, pinned_plan))
    {
        const mz::Verdict v = mz::judge(pinned_plan, runner, budget, out.stats);
        if (v == mz::Verdict::Budget)
        {
            out.plan   = cur;
            out.status = mz::Status::Budget;
            return;
        }
        if (v == mz::Verdict::Yes)
        {
            cur        = pinned_plan;
            out.pinned = true;
        }
    }

    // 4. Remove faults one at a time.
    mz::Status status = mz::Status::Minimal;
    if (out.pinned)
    {
        const bool trigger = cur.trigger_flight;
        const auto as_plan = [trigger](const cont::Array<FaultHit>& candidate, FaultPlan& p)
        {
            return plan_of_hits(candidate, trigger, p);
        };
        const auto run_hits = [&](const cont::Array<FaultHit>& candidate)
        {
            FaultPlan p;
            return as_plan(candidate, p) && runner(p);
        };
        const auto admit = [&](const cont::Array<FaultHit>& candidate)
        {
            FaultPlan p;
            return as_plan(candidate, p);
        };
        // `hits` is the pinned plan's own hit list; it reproduced just now.
        status = mz::minimize_sequence(hits, run_hits, admit, budget, out.stats);
        FaultPlan reduced;
        if (as_plan(hits, reduced))
        {
            cur = reduced;
        }
        out.hits = static_cast<crd::u32>(hits.size());
    }
    else
    {
        cont::Array<FaultRule> rules;
        for (crd::u32 i = 0U; i < cur.rule_count; ++i)
        {
            rules.push_back(cur.rules[i]);
        }
        const FaultPlan base     = cur;
        const auto      as_plan  = [&base](const cont::Array<FaultRule>& candidate)
        {
            FaultPlan p  = base;
            p.rule_count = static_cast<crd::u32>(candidate.size());
            for (crd::u32 i = 0U; i < p.rule_count; ++i)
            {
                p.rules[i] = candidate[i];
            }
            return p;
        };
        const auto run_rules = [&](const cont::Array<FaultRule>& candidate)
        {
            return runner(as_plan(candidate));
        };
        const auto admit = [](const cont::Array<FaultRule>& candidate)
        {
            return !candidate.empty();
        };
        status               = mz::minimize_sequence(rules, run_rules, admit, budget, out.stats);
        cur                  = as_plan(rules);
    }
    if (status == mz::Status::NotReproduced)
    {
        // The removal step judges its starting plan again, and a plan judged to reproduce a moment ago did not: the
        // reproduction is not stable, so nothing smaller is claimed.
        out.plan   = plan;
        out.status = mz::Status::NotReproduced;
        return;
    }
    out.plan   = cur;
    out.status = status;
}

} // namespace crd::perf
