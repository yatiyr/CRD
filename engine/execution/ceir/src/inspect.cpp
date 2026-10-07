#include <crd/ceir/inspect.hpp>

#include <crd/ceir/print.hpp>      // print_type (a snapshot's type text)
#include <crd/ceir/provenance.hpp> // resolve_provenance (line breakpoints, optimized-away values)

#include <chrono>
#include <utility> // std::move

namespace crd::ceir::inspect
{
containers::StringView pause_scope_name(PauseScope s) noexcept
{
    switch (s) // ⛔ no default (-Werror=switch)
    {
    case PauseScope::Task: return containers::StringView("task");
    case PauseScope::WholeHost: return containers::StringView("whole-host");
    case PauseScope::NonPausable: return containers::StringView("non-pausable");
    }
    return containers::StringView("?");
}

containers::StringView resume_name(Resume r) noexcept
{
    switch (r) // ⛔ no default (-Werror=switch)
    {
    case Resume::Continue: return containers::StringView("continue");
    case Resume::StepInto: return containers::StringView("step-into");
    case Resume::StepOver: return containers::StringView("step-over");
    case Resume::StepOut: return containers::StringView("step-out");
    }
    return containers::StringView("?");
}

containers::StringView stop_reason_name(StopReason r) noexcept
{
    switch (r) // ⛔ no default (-Werror=switch)
    {
    case StopReason::None: return containers::StringView("none");
    case StopReason::Breakpoint: return containers::StringView("breakpoint");
    case StopReason::Step: return containers::StringView("step");
    case StopReason::PauseRequest: return containers::StringView("pause-request");
    }
    return containers::StringView("?");
}

containers::StringView refusal_name(Refusal r) noexcept
{
    switch (r) // ⛔ no default (-Werror=switch)
    {
    case Refusal::None: return containers::StringView("none");
    case Refusal::NotBound: return containers::StringView("not-bound");
    case Refusal::StaleGeneration: return containers::StringView("stale-generation");
    case Refusal::Busy: return containers::StringView("busy");
    case Refusal::NotRunning: return containers::StringView("not-running");
    case Refusal::NotPaused: return containers::StringView("not-paused");
    case Refusal::NonPausable: return containers::StringView("non-pausable");
    case Refusal::NoHostPause: return containers::StringView("no-host-pause");
    case Refusal::SameThread: return containers::StringView("same-thread");
    case Refusal::DetachedBody: return containers::StringView("detached-body");
    case Refusal::Timeout: return containers::StringView("timeout");
    case Refusal::Finished: return containers::StringView("finished");
    }
    return containers::StringView("?");
}

containers::StringView bind_status_name(BindStatus s) noexcept
{
    switch (s) // ⛔ no default (-Werror=switch)
    {
    case BindStatus::Bound: return containers::StringView("bound");
    case BindStatus::NoCodeAtLine: return containers::StringView("no-code-at-line");
    case BindStatus::UnknownFile: return containers::StringView("unknown-file");
    }
    return containers::StringView("?");
}

containers::StringView value_status_name(ValueStatus s) noexcept
{
    switch (s) // ⛔ no default (-Werror=switch)
    {
    case ValueStatus::Available: return containers::StringView("available");
    case ValueStatus::NotYetComputed: return containers::StringView("not-yet-computed");
    case ValueStatus::OutOfScope: return containers::StringView("out-of-scope");
    case ValueStatus::OptimizedAway: return containers::StringView("optimized-away");
    case ValueStatus::NoSuchValue: return containers::StringView("no-such-value");
    case ValueStatus::Redacted: return containers::StringView("redacted");
    }
    return containers::StringView("?");
}

namespace
{
// How often a pause linked to a host re-reads the host's cancel flag (the host raises it without the session's lock).
constexpr std::chrono::milliseconds kHostCancelPoll{2};

// The file id `ctx` registered for `path` (0 when it has none). Never registers.
u32 find_file(const Context& ctx, containers::StringView path) noexcept
{
    for (u32 id = 1U;; ++id)
    {
        const containers::StringView p = ctx.file_path(id);
        if (p.empty())
        {
            return 0U;
        }
        if (p == path)
        {
            return id;
        }
    }
}

// Every op of `r`, in pre-order, appended to `out`.
void collect_ops(const Region* r, containers::Array<const Operation*>& out) // NOLINT(misc-no-recursion)
{
    if (r == nullptr)
    {
        return;
    }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            out.push_back(op);
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                collect_ops(op->region(i), out);
            }
        }
    }
}

bool carries_line(containers::ConstSpan<Origin> origins, u32 file_id, u32 line) noexcept
{
    for (usize i = 0; i < origins.size(); ++i)
    {
        if (origins[i].loc.file_id == file_id && origins[i].loc.line == line)
        {
            return true;
        }
    }
    return false;
}

ValueStatus from_plan(plan::ValueState s) noexcept
{
    switch (s) // ⛔ no default (-Werror=switch)
    {
    case plan::ValueState::Available: return ValueStatus::Available;
    case plan::ValueState::NotYetComputed: return ValueStatus::NotYetComputed;
    case plan::ValueState::OutOfScope: return ValueStatus::OutOfScope;
    case plan::ValueState::OptimizedAway: return ValueStatus::OptimizedAway;
    case plan::ValueState::NoSuchValue: return ValueStatus::NoSuchValue;
    }
    return ValueStatus::NoSuchValue;
}

// Is `def` (an op on the paused op's ancestor chain's block) before `at` in their shared block?
bool precedes(const Operation* def, const Operation* at) noexcept
{
    for (const Operation* op = def->next_in_block(); op != nullptr; op = op->next_in_block())
    {
        if (op == at)
        {
            return true;
        }
    }
    return false;
}

// The structural availability of `def`'s results at the paused op `at` in the reference interpreter: walk out from
// `at` through its enclosing ops; the first enclosing block that holds `def` decides. A def outside that chain lives
// in another region, function or frame.
ValueStatus scope_at(const Operation* def, const Operation* at) noexcept
{
    for (const Operation* a = at; a != nullptr;)
    {
        const Block* const b = a->parent_block();
        if (b == nullptr)
        {
            break;
        }
        if (b == def->parent_block())
        {
            return precedes(def, a) ? ValueStatus::Available : ValueStatus::NotYetComputed;
        }
        const Region* const r = b->parent_region();
        a                     = (r != nullptr) ? r->parent_op() : nullptr;
    }
    return ValueStatus::OutOfScope;
}
} // namespace

StableId op_at_line(const plan::CompiledPlan& plan, const Context& ctx, containers::StringView file, u32 line) noexcept
{
    const u32 fid = find_file(ctx, file);
    if (fid == 0U)
    {
        return StableId{};
    }
    for (usize s = 0; s < plan.sites.size(); ++s)
    {
        const plan::InstrSite&              site = plan.sites[s];
        const containers::ConstSpan<Origin> origins(plan.site_origins.data() + site.origins_off, site.origins_cnt);
        if (carries_line(origins, fid, line))
        {
            return site.op;
        }
    }
    return StableId{};
}

Session::Session(memory::IAllocator* alloc, PauseScope scope, HostPause host)
    : m_alloc(alloc), m_scope(scope), m_host(host), m_breakpoints(alloc), m_bp_sites(alloc), m_bp_ops(alloc),
      m_reply(alloc)
{
}

void Session::connect_controller()
{
    const std::lock_guard<std::mutex> lk(m_mu);
    m_controller     = std::this_thread::get_id();
    m_has_controller = true;
}

Refusal Session::add_line_breakpoint(containers::StringView file, u32 line, u32& out_index)
{
    const std::lock_guard<std::mutex> lk(m_mu);
    if (m_state == State::Running || m_state == State::Paused)
    {
        return Refusal::Busy;
    }
    LineBreakpoint bp(m_alloc);
    bp.file.append(file);
    bp.line   = line;
    out_index = static_cast<u32>(m_breakpoints.size());
    m_breakpoints.push_back(std::move(bp));
    return Refusal::None;
}

void Session::set_redaction(RedactFn fn, void* user)
{
    const std::lock_guard<std::mutex> lk(m_mu);
    m_redact      = fn;
    m_redact_user = user;
}

Refusal Session::bind(const plan::CompiledPlan& plan, const Context& ctx, u64 generation,
                      containers::Array<BindReport>& out)
{
    const std::lock_guard<std::mutex> lk(m_mu);
    if (m_state == State::Running || m_state == State::Paused)
    {
        return Refusal::Busy;
    }
    out.clear();
    m_bp_sites.clear();
    m_bp_ops.clear();
    m_bp_sites.resize(plan.sites.size()); // zero-filled: no breakpoint
    for (u32 b = 0; b < static_cast<u32>(m_breakpoints.size()); ++b)
    {
        const LineBreakpoint& bp = m_breakpoints[b];
        BindReport            rep;
        rep.breakpoint = b;
        const u32 fid  = find_file(ctx, containers::StringView(bp.file.data(), bp.file.size()));
        if (fid == 0U)
        {
            rep.status = BindStatus::UnknownFile;
            out.push_back(rep);
            continue;
        }
        for (u32 s = 0; s < static_cast<u32>(plan.sites.size()); ++s)
        {
            const plan::InstrSite& site = plan.sites[s];
            const containers::ConstSpan<Origin> origins(plan.site_origins.data() + site.origins_off, site.origins_cnt);
            if (!carries_line(origins, fid, bp.line))
            {
                continue;
            }
            if (m_bp_sites[s] == 0U)
            {
                m_bp_sites[s] = b + 1U;
            }
            if (rep.sites == 0U)
            {
                rep.first_op = site.op;
            }
            ++rep.sites;
        }
        rep.status = (rep.sites != 0U) ? BindStatus::Bound : BindStatus::NoCodeAtLine;
        out.push_back(rep);
    }
    m_ctx        = &ctx;
    m_plan       = &plan;
    m_module     = nullptr;
    m_generation = generation;
    m_bound      = true;
    m_state      = State::Idle;
    return Refusal::None;
}

Refusal Session::bind(const Module& m, const Context& ctx, u64 generation, containers::Array<BindReport>& out)
{
    const std::lock_guard<std::mutex> lk(m_mu);
    if (m_state == State::Running || m_state == State::Paused)
    {
        return Refusal::Busy;
    }
    out.clear();
    m_bp_sites.clear();
    m_bp_ops.clear();
    containers::Array<const Operation*> ops(m_alloc);
    collect_ops(m.body(), ops);
    containers::Array<Origin> storage(m_alloc);
    for (u32 b = 0; b < static_cast<u32>(m_breakpoints.size()); ++b)
    {
        const LineBreakpoint& bp = m_breakpoints[b];
        BindReport            rep;
        rep.breakpoint = b;
        const u32 fid  = find_file(ctx, containers::StringView(bp.file.data(), bp.file.size()));
        if (fid == 0U)
        {
            rep.status = BindStatus::UnknownFile;
            out.push_back(rep);
            continue;
        }
        for (usize i = 0; i < ops.size(); ++i)
        {
            const Provenance p = resolve_provenance(ctx, ops[i], storage);
            if (!carries_line(p.origins, fid, bp.line))
            {
                continue;
            }
            if (m_bp_ops.find(ops[i]) == nullptr)
            {
                m_bp_ops.insert(ops[i], b);
            }
            if (rep.sites == 0U)
            {
                rep.first_op = ops[i]->stable_id();
            }
            ++rep.sites;
        }
        rep.status = (rep.sites != 0U) ? BindStatus::Bound : BindStatus::NoCodeAtLine;
        out.push_back(rep);
    }
    m_ctx        = &ctx;
    m_plan       = nullptr;
    m_module     = &m;
    m_generation = generation;
    m_bound      = true;
    m_state      = State::Idle;
    return Refusal::None;
}

void Session::begin_run(Executor e)
{
    const std::lock_guard<std::mutex> lk(m_mu);
    attach_locked(e);
}

void Session::attach_locked(Executor e)
{
    m_state = State::Running;
    m_cancel.store(false, std::memory_order_relaxed);
    m_pause_req.store(false, std::memory_order_relaxed);
    m_step = Resume::Continue;
    m_exec = e;
}

void Session::end_run()
{
    {
        const std::lock_guard<std::mutex> lk(m_mu);
        m_state = State::Finished;
        m_exec  = Executor::None;
    }
    m_cv.notify_all();
}

plan::RunResult Session::run(const plan::CompiledPlan& plan, containers::ConstSpan<i64> args, memory::IAllocator* alloc)
{
    begin_run(Executor::Plan);
    const plan::RunControl control{&Session::on_plan, this, &m_cancel};
    plan::RunResult        r = plan::run(plan, args, alloc, plan::RunHooks{}, &control);
    end_run();
    return r;
}

exec::ExecResult Session::invoke(exec::Interpreter& in, const Module& m, containers::StringView entry,
                                 containers::ConstSpan<i64> args)
{
    return invoke(in, m, entry, args, HostLink{});
}

exec::ExecResult Session::invoke(exec::Interpreter& in, const Module& m, containers::StringView entry,
                                 containers::ConstSpan<i64> args, const HostLink& host)
{
    begin_run(Executor::Interpreter);
    {
        const std::lock_guard<std::mutex> lk(m_mu);
        m_link = host;
    }
    m_cur_in = &in;
    in.set_step_hooks(&Session::on_step, nullptr, this);
    in.set_cancel_flag(host.cancel != nullptr ? host.cancel : &m_cancel);
    exec::ExecResult r = in.invoke(m, entry, args);
    in.set_step_hooks(nullptr, nullptr, nullptr);
    in.set_cancel_flag(nullptr);
    m_cur_in = nullptr;
    {
        const std::lock_guard<std::mutex> lk(m_mu);
        m_link = HostLink{};
    }
    end_run();
    return r;
}

void Session::attach_detached(exec::Interpreter& sub)
{
    sub.set_step_hooks(&Session::on_detached, nullptr, this);
}

plan::SafePointAction Session::on_plan(const plan::CompiledPlan& plan, const plan::SafePoint& at, void* user)
{
    Session&  s    = *static_cast<Session*>(user);
    const u32 site = plan.seqs[at.at.seq].sites[at.at.instr];
    u32       bp   = kNoBreakpoint;
    if (&plan == s.m_plan && site < static_cast<u32>(s.m_bp_sites.size()) && s.m_bp_sites[site] != 0U)
    {
        bp = s.m_bp_sites[site] - 1U;
    }
    const StopReason why = s.decide(bp, at.depth);
    if (why == StopReason::None)
    {
        return plan::SafePointAction::Continue;
    }
    StopRecord rec;
    rec.reason     = why;
    rec.breakpoint = (why == StopReason::Breakpoint) ? bp : kNoBreakpoint;
    rec.depth      = at.depth;
    rec.at         = at.at;
    rec.op         = plan.sites[site].op;
    s.m_cur_plan   = &plan;
    s.m_cur_sp     = &at;
    const bool cancel = s.hold(rec);
    s.m_cur_sp        = nullptr;
    s.m_cur_plan      = nullptr;
    return cancel ? plan::SafePointAction::Cancel : plan::SafePointAction::Continue;
}

void Session::on_step(const Operation& op, void* user)
{
    Session&  s  = *static_cast<Session*>(user);
    u32       bp = kNoBreakpoint;
    if (const u32* const found = s.m_bp_ops.find(&op); found != nullptr)
    {
        bp = *found;
    }
    const u32        depth = (s.m_cur_in != nullptr) ? s.m_cur_in->call_depth() : 0U;
    const StopReason why   = s.decide(bp, depth);
    if (why == StopReason::None)
    {
        return;
    }
    StopRecord rec;
    rec.reason     = why;
    rec.breakpoint = (why == StopReason::Breakpoint) ? bp : kNoBreakpoint;
    rec.depth      = depth;
    rec.op         = op.stable_id();
    s.m_cur_op     = &op;
    (void)s.hold(rec); // a cancel is observed by the interpreter through the flag, before `op` dispatches
    s.m_cur_op = nullptr;
}

// A detached body never pauses and never touches the stepping or pause-request state (it may run on many pool
// workers at once): a bound breakpoint is counted as a refused pause. `m_bp_ops` is read-only while an execution is
// attached (bind is refused Busy), so the concurrent lookups are reads of a table the executing thread does not write.
void Session::on_detached(const Operation& op, void* user)
{
    Session& s = *static_cast<Session*>(user);
    if (s.m_bp_ops.find(&op) == nullptr)
    {
        return;
    }
    s.m_detached.fetch_add(1U, std::memory_order_relaxed);
    s.m_refused.fetch_add(1U, std::memory_order_relaxed);
    s.m_last_refusal.store(static_cast<u8>(Refusal::DetachedBody), std::memory_order_relaxed);
}

Refusal Session::begin_device(u64 generation)
{
    const std::lock_guard<std::mutex> lk(m_mu);
    if (const Refusal r = check(generation); r != Refusal::None)
    {
        return r;
    }
    if (m_state == State::Running || m_state == State::Paused)
    {
        return Refusal::Busy;
    }
    attach_locked(Executor::Device); // under the same lock as the checks, so two recordings cannot both attach
    return Refusal::None;
}

// A device point never pauses and never touches the stepping or pause-request state: a recorded dispatch runs later on
// the device, so the session's scope does not apply. `m_bp_ops` is read-only while the recording is attached (bind is
// refused Busy). A recording has no host link, so the session's own flag is the only cancel.
bool Session::device_point(const Operation& op)
{
    if (m_bp_ops.find(&op) != nullptr)
    {
        m_device.fetch_add(1U, std::memory_order_relaxed);
        m_refused.fetch_add(1U, std::memory_order_relaxed);
        m_last_refusal.store(static_cast<u8>(Refusal::NonPausable), std::memory_order_relaxed);
    }
    return m_cancel.load(std::memory_order_relaxed);
}

void Session::end_device()
{
    end_run();
}

StopReason Session::decide(u32 breakpoint, u32 depth)
{
    StopReason why = StopReason::None;
    if (breakpoint != kNoBreakpoint)
    {
        why = StopReason::Breakpoint;
    }
    else if ((m_step == Resume::StepInto) || (m_step == Resume::StepOver && depth <= m_step_depth) ||
             (m_step == Resume::StepOut && depth < m_step_depth))
    {
        why = StopReason::Step;
    }
    else if (m_pause_req.load(std::memory_order_acquire))
    {
        why = StopReason::PauseRequest;
    }
    if (why == StopReason::None)
    {
        return why;
    }
    Refusal refused = Refusal::None;
    if (m_scope == PauseScope::NonPausable)
    {
        refused = Refusal::NonPausable;
    }
    else if (m_scope == PauseScope::WholeHost && (m_host.freeze == nullptr || m_host.thaw == nullptr))
    {
        refused = Refusal::NoHostPause;
    }
    else if (m_has_controller && std::this_thread::get_id() == m_controller)
    {
        refused = Refusal::SameThread;
    }
    if (refused != Refusal::None)
    {
        m_refused.fetch_add(1U, std::memory_order_relaxed);
        m_last_refusal.store(static_cast<u8>(refused), std::memory_order_relaxed);
        m_pause_req.store(false, std::memory_order_relaxed);
        m_step = Resume::Continue;
        return StopReason::None;
    }
    return why;
}

bool Session::cancel_raised() const noexcept
{
    return m_cancel.load(std::memory_order_relaxed) ||
           (m_link.cancel != nullptr && m_link.cancel->load(std::memory_order_relaxed));
}

bool Session::hold(StopRecord rec)
{
    std::unique_lock<std::mutex> lk(m_mu);
    if (m_link.pending != nullptr)
    {
        rec.pending_jobs = m_link.pending(m_link.user); // the host's own state, read on its executing thread
    }
    rec.generation = m_generation;
    rec.sequence   = ++m_stops;
    m_stop         = rec;
    m_state        = State::Paused;
    m_cmd_pending  = false;
    m_pause_req.store(false, std::memory_order_relaxed);
    if (m_scope == PauseScope::WholeHost)
    {
        m_host.freeze(m_host.user);
    }
    m_cv.notify_all();
    bool cancel = false;
    const auto woken = [this] { return m_cmd_pending || m_req_pending || cancel_raised(); };
    for (;;)
    {
        if (m_link.cancel == nullptr)
        {
            m_cv.wait(lk, woken);
        }
        else
        {
            bool ready = false;
            while (!ready) // the host raises its flag without the session's lock, so it is re-read on each poll
            {
                ready = m_cv.wait_for(lk, kHostCancelPoll, woken);
            }
        }
        if (m_req_pending)
        {
            serve_read();
            m_req_pending = false;
            m_req_done    = true;
            m_cv.notify_all();
            continue;
        }
        if (cancel_raised())
        {
            cancel = true;
            break;
        }
        m_step        = m_cmd;
        m_step_depth  = rec.depth;
        m_cmd_pending = false;
        break;
    }
    m_state = State::Running;
    if (m_scope == PauseScope::WholeHost)
    {
        m_host.thaw(m_host.user);
    }
    return cancel;
}

void Session::serve_read()
{
    ValueSnapshot& out = m_reply;
    out.status         = ValueStatus::NoSuchValue;
    out.op             = m_req.op;
    out.result         = m_req.result;
    out.bits           = 0;
    out.type           = TypeId{};
    out.has_unit       = false;
    out.unit           = QuantityDim{};
    out.redaction      = RedactionClass::Public;
    out.survivor       = StableId{};
    out.type_text.clear();
    if (m_exec == Executor::Plan && m_cur_plan != nullptr && m_cur_sp != nullptr)
    {
        const plan::ValueRead r = plan::read_value(*m_cur_plan, *m_cur_sp, m_req.op, m_req.result);
        out.status              = from_plan(r.state);
        out.bits                = r.bits;
        out.type                = r.type;
        out.survivor            = r.survivor;
    }
    else if (m_exec == Executor::Interpreter && m_cur_in != nullptr && m_cur_op != nullptr && m_module != nullptr &&
             m_req.op.valid())
    {
        containers::Array<const Operation*> ops(m_alloc);
        collect_ops(m_module->body(), ops);
        const Operation* def = nullptr;
        for (usize i = 0; i < ops.size() && def == nullptr; ++i)
        {
            if (ops[i]->stable_id() == m_req.op)
            {
                def = ops[i];
            }
        }
        if (def == nullptr)
        {
            containers::Array<Origin> storage(m_alloc);
            for (usize i = 0; i < ops.size(); ++i)
            {
                const Provenance p = resolve_provenance(*m_ctx, ops[i], storage);
                for (usize o = 0; o < p.origins.size(); ++o)
                {
                    if (p.origins[o].space == OriginSpace::CeirOp && p.origins[o].node == m_req.op)
                    {
                        out.status   = ValueStatus::OptimizedAway;
                        out.survivor = ops[i]->stable_id();
                    }
                }
                if (out.status == ValueStatus::OptimizedAway)
                {
                    break;
                }
            }
        }
        else if (m_req.result < def->num_results())
        {
            out.type   = def->result(m_req.result)->type();
            out.status = scope_at(def, m_cur_op);
            if (out.status == ValueStatus::Available && !m_cur_in->value_of(def->result(m_req.result), out.bits))
            {
                out.status = ValueStatus::NotYetComputed;
            }
        }
    }
    if (out.type.valid() && m_ctx != nullptr)
    {
        print_type(*m_ctx, out.type, out.type_text);
        const Type t = m_ctx->type_of(out.type);
        if (t.kind == TypeKind::Quantity)
        {
            out.has_unit = true;
            out.unit     = unpack_dim(t.count, t.cols);
        }
        if (m_redact != nullptr)
        {
            out.redaction = m_redact(out.op, out.type, m_redact_user);
        }
    }
    if (out.status == ValueStatus::Available && out.redaction == RedactionClass::Restricted)
    {
        out.status = ValueStatus::Redacted;
        out.bits   = 0;
    }
}

Refusal Session::check(u64 generation) const noexcept
{
    if (!m_bound)
    {
        return Refusal::NotBound;
    }
    if (generation != m_generation)
    {
        return Refusal::StaleGeneration;
    }
    return Refusal::None;
}

Refusal Session::request_pause(u64 generation)
{
    const std::lock_guard<std::mutex> lk(m_mu);
    if (const Refusal r = check(generation); r != Refusal::None)
    {
        return r;
    }
    if (m_scope == PauseScope::NonPausable)
    {
        return Refusal::NonPausable;
    }
    if (m_scope == PauseScope::WholeHost && (m_host.freeze == nullptr || m_host.thaw == nullptr))
    {
        return Refusal::NoHostPause;
    }
    if (m_state != State::Running && m_state != State::Paused)
    {
        return Refusal::NotRunning;
    }
    if (m_exec == Executor::Device) // device work never pauses, whatever the session's scope
    {
        return Refusal::NonPausable;
    }
    m_pause_req.store(true, std::memory_order_release);
    return Refusal::None;
}

Refusal Session::wait_for_stop(u64 generation, u32 timeout_ms, StopRecord& out)
{
    std::unique_lock<std::mutex> lk(m_mu);
    if (const Refusal r = check(generation); r != Refusal::None)
    {
        return r;
    }
    const bool settled = m_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                       [this] { return m_state == State::Paused || m_state == State::Finished; });
    if (!settled)
    {
        return Refusal::Timeout;
    }
    if (m_state == State::Finished)
    {
        return Refusal::Finished;
    }
    out = m_stop;
    return Refusal::None;
}

Refusal Session::snapshot(u64 generation, ValueRef ref, ValueSnapshot& out, u32 timeout_ms)
{
    std::unique_lock<std::mutex> lk(m_mu);
    if (const Refusal r = check(generation); r != Refusal::None)
    {
        return r;
    }
    if (m_state != State::Paused)
    {
        return Refusal::NotPaused;
    }
    m_req         = ref;
    m_req_pending = true;
    m_req_done    = false;
    m_cv.notify_all();
    const bool done = m_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return m_req_done; });
    if (!done)
    {
        m_req_pending = false;
        return Refusal::Timeout;
    }
    m_req_done    = false;
    out.status    = m_reply.status;
    out.op        = m_reply.op;
    out.result    = m_reply.result;
    out.bits      = m_reply.bits;
    out.type      = m_reply.type;
    out.has_unit  = m_reply.has_unit;
    out.unit      = m_reply.unit;
    out.redaction = m_reply.redaction;
    out.survivor  = m_reply.survivor;
    out.type_text.clear();
    out.type_text.append(containers::StringView(m_reply.type_text.data(), m_reply.type_text.size()));
    return Refusal::None;
}

Refusal Session::resume(u64 generation, Resume how)
{
    {
        const std::lock_guard<std::mutex> lk(m_mu);
        if (const Refusal r = check(generation); r != Refusal::None)
        {
            return r;
        }
        if (m_state != State::Paused)
        {
            return Refusal::NotPaused;
        }
        m_cmd         = how;
        m_cmd_pending = true;
        m_state       = State::Running; // accepted: a following wait_for_stop waits for the NEXT stop
    }
    m_cv.notify_all();
    return Refusal::None;
}

Refusal Session::cancel(u64 generation)
{
    {
        const std::lock_guard<std::mutex> lk(m_mu);
        if (const Refusal r = check(generation); r != Refusal::None)
        {
            return r;
        }
        if (m_state != State::Running && m_state != State::Paused)
        {
            return Refusal::NotRunning;
        }
        m_cancel.store(true, std::memory_order_relaxed);
        if (m_link.cancel != nullptr) // a host execution: its detached bodies observe only the host's flag
        {
            m_link.cancel->store(true, std::memory_order_relaxed);
        }
    }
    m_cv.notify_all();
    return Refusal::None;
}
} // namespace crd::ceir::inspect
