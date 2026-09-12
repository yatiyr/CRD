#include <crd/ceir/host/plan_perf.hpp>

#include <crd/perf/counters.hpp> // CRD_PERF_COUNTER_SET_I64
#include <crd/perf/scope.hpp>    // CRD_PERF_SCOPE
#include <crd/time/clocks.hpp>   // MonotonicClock (always available — the profile fills even on a perf-OFF build)

#include <utility> // std::move

namespace crd::ceir::host
{
// ⛔ the per-Op arrays are indexed by u8(op); if plan::Op ever grows past kMaxOps this becomes a SILENT drop. MapReduce
// is the last member today — widening the enum must audit THIS consumer (the widen-enum-audit-every-consumer rule).
static_assert(static_cast<crd::u8>(plan::Op::MapReduce) < PlanProfile::kMaxOps, "PlanProfile::kMaxOps < the plan::Op count");
namespace
{
constexpr crd::u32 kStack = 256U; // the parent-pause op stack; deeper nesting is witnessed (depth_overflow), never silent.

// the seam-hook state: the profile + a parent-pause op stack + the last-event timestamp.
struct ProfState
{
    PlanProfile        prof;
    crd::u8            stack[kStack] = {};
    crd::u32           depth         = 0U;
    crd::time::Instant mark          = {};
};

// pre: the time since the last event belongs to the op we are INSIDE (the stack top = the parent); then push + count.
void adapter_pre(crd::u8 op, void* user)
{
    auto* const              s   = static_cast<ProfState*>(user);
    const crd::time::Instant now = crd::time::MonotonicClock::now();
    if (s->depth > 0U && s->depth <= kStack) { s->prof.self_s[s->stack[s->depth - 1U]] += (now - s->mark).value; }
    if (s->depth < kStack) { s->stack[s->depth] = op; }
    else { ++s->prof.depth_overflow; } // ⛔ witness — the parent-pause loses this level's self-time attribution
    ++s->depth;
    if (s->depth > s->prof.max_depth) { s->prof.max_depth = s->depth; }
    s->mark = now;
    if (op < PlanProfile::kMaxOps) { ++s->prof.dispatch[op]; }
    ++s->prof.total_dispatch;
}
// post: the time since the last event belongs to THIS op (its own body after its last child); banked via the op ARG
// (overflow-robust — no stack read). ⛔ `post` fires only on a SUCCESSFUL dispatch, so on an error the stack stays >0 —
// harmless (the state is reset per run).
void adapter_post(crd::u8 op, void* user)
{
    auto* const              s   = static_cast<ProfState*>(user);
    const crd::time::Instant now = crd::time::MonotonicClock::now();
    if (op < PlanProfile::kMaxOps) { s->prof.self_s[op] += (now - s->mark).value; }
    if (s->depth > 0U) { --s->depth; }
    s->mark = now;
}
} // namespace

plan::RunResult profiled_run(const plan::CompiledPlan& plan, containers::ConstSpan<crd::i64> args,
                             memory::IAllocator* alloc, PlanProfile& out)
{
    CRD_PERF_SCOPE("ceir.plan.run");
    ProfState st;
    st.mark           = crd::time::MonotonicClock::now();
    plan::RunResult r = plan::run(plan, args, alloc, plan::RunHooks{adapter_pre, adapter_post, &st});
    CRD_PERF_COUNTER_SET_I64("ceir.plan.dispatch_total", static_cast<crd::i64>(st.prof.total_dispatch));
    out = st.prof;
    return r;
}

plan::CompileResult profiled_compile(Context& ctx, const Module& module, containers::StringView entry,
                                     memory::IAllocator* alloc)
{
    CRD_PERF_SCOPE("ceir.plan.compile");
    plan::CompileResult cr = plan::compile(ctx, module, entry, alloc);
    CRD_PERF_COUNTER_SET_I64("ceir.plan.compile.instrs", static_cast<crd::i64>(cr.stats.num_instrs));
    CRD_PERF_COUNTER_SET_I64("ceir.plan.compile.seqs", static_cast<crd::i64>(cr.stats.num_seqs));
    CRD_PERF_COUNTER_SET_I64("ceir.plan.compile.funcs", static_cast<crd::i64>(cr.stats.num_funcs));
    CRD_PERF_COUNTER_SET_I64("ceir.plan.compile.cells", static_cast<crd::i64>(cr.stats.num_cells));
    CRD_PERF_COUNTER_SET_I64("ceir.plan.compile.maps", static_cast<crd::i64>(cr.stats.num_maps));
    return cr;
}
} // namespace crd::ceir::host
