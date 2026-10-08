#include <crd/ceir/cook/inspect_script.hpp>

#include <crd/ceir/provenance.hpp> // Origin / Provenance (a stop's and a fault's authored position)

#include <chrono>
#include <thread>

namespace crd::ceir::cook
{
namespace
{
namespace insp = crd::ceir::inspect;

// A stop is waited for in slices of this many milliseconds, so the caller's cancel flag is seen while the program runs.
constexpr u32 kSliceMs = 10U;

[[nodiscard]] insp::Resume resume_of(ScriptAction a) noexcept
{
    if (a == ScriptAction::Into)
    {
        return insp::Resume::StepInto;
    }
    if (a == ScriptAction::Over)
    {
        return insp::Resume::StepOver;
    }
    if (a == ScriptAction::Out)
    {
        return insp::Resume::StepOut;
    }
    return insp::Resume::Continue;
}

[[nodiscard]] bool caller_cancelled(const InspectScript& script) noexcept
{
    return script.cancel != nullptr && script.cancel->load(std::memory_order_acquire);
}

// Wait up to the script's bound for the next stop, in slices. Returns Timeout once the bound runs out; `cancelled` is
// set (and nothing is waited for) as soon as the caller's flag is seen.
[[nodiscard]] insp::Refusal wait_stop(InspectHost& host, u64 gen, const InspectScript& script, insp::StopRecord& stop,
                                      bool& cancelled)
{
    u32 waited = 0U;
    for (;;)
    {
        if (caller_cancelled(script))
        {
            cancelled = true;
            return insp::Refusal::None;
        }
        const u32           slice = (script.wait_ms - waited < kSliceMs) ? script.wait_ms - waited : kSliceMs;
        const insp::Refusal r     = host.session().wait_for_stop(gen, slice, stop);
        if (r != insp::Refusal::Timeout)
        {
            return r;
        }
        waited += slice;
        if (waited >= script.wait_ms)
        {
            return insp::Refusal::Timeout;
        }
    }
}

// Cancel the execution. The executing thread attaches to the session only once it runs, so a cancel the session
// refuses NotRunning while the execution is still alive came before that: it is repeated until the session takes it or
// the execution ends, within the script's bound.
void cancel_run(InspectHost& host, u64 gen, const InspectScript& script)
{
    for (u32 waited = 0U;; ++waited)
    {
        if (host.session().cancel(gen) != insp::Refusal::NotRunning || !host.running() || waited >= script.wait_ms)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// The stop's values: each watched line's first compiled result, read by the paused executing thread.
void read_values(InspectHost& host, u64 gen, const InspectScript& script, insp::ValueSnapshot& value,
                 InspectReport& out)
{
    for (crd::usize i = 0; i < script.watches.size(); ++i)
    {
        ScriptValue          v;
        const insp::ValueRef ref{host.op_at_line(script.file, script.watches[i]), 0U};
        v.line = script.watches[i];
        if (host.session().snapshot(gen, ref, value, script.wait_ms) == insp::Refusal::None)
        {
            v.answered = true;
            v.status   = value.status;
            v.type_off = static_cast<u32>(out.text.size());
            v.type_len = static_cast<u32>(value.type_text.size());
            out.text.append(containers::StringView(value.type_text.data(), value.type_text.size()));
            v.has_unit = value.has_unit;
            v.bits     = value.bits;
        }
        out.values.push_back(v);
    }
}
} // namespace

containers::StringView script_action_name(ScriptAction a) noexcept
{
    switch (a) // no default: every action is named
    {
    case ScriptAction::Continue: return containers::StringView{"continue"};
    case ScriptAction::Into: return containers::StringView{"into"};
    case ScriptAction::Over: return containers::StringView{"over"};
    case ScriptAction::Out: return containers::StringView{"out"};
    case ScriptAction::Cancel: return containers::StringView{"cancel"};
    }
    return containers::StringView{"?"};
}

bool parse_script_action(containers::StringView text, ScriptAction& out) noexcept
{
    constexpr ScriptAction all[] = {ScriptAction::Continue, ScriptAction::Into, ScriptAction::Over, ScriptAction::Out,
                                    ScriptAction::Cancel};
    for (const ScriptAction a : all)
    {
        if (text == script_action_name(a))
        {
            out = a;
            return true;
        }
    }
    return false;
}

containers::StringView script_outcome_name(ScriptOutcome o) noexcept
{
    switch (o) // no default: every outcome is named
    {
    case ScriptOutcome::NotStarted: return containers::StringView{"not-started"};
    case ScriptOutcome::Finished: return containers::StringView{"finished"};
    case ScriptOutcome::Cancelled: return containers::StringView{"cancelled"};
    case ScriptOutcome::CallerCancelled: return containers::StringView{"caller-cancelled"};
    case ScriptOutcome::Error: return containers::StringView{"error"};
    case ScriptOutcome::Unfinished: return containers::StringView{"unfinished"};
    }
    return containers::StringView{"?"};
}

void run_inspect_script(InspectHost& host, const InspectScript& script, InspectReport& out)
{
    for (crd::usize i = 0; i < script.breaks.size(); ++i)
    {
        u32 index = 0U;
        (void)host.add_line_breakpoint(script.file, script.breaks[i], index); // not Busy: nothing runs yet
    }
    if (const insp::Refusal r = host.start(script.args, script.record, script.inputs); r != insp::Refusal::None)
    {
        out.outcome = ScriptOutcome::NotStarted;
        out.refusal = r;
        return;
    }
    const u64 gen  = host.generation();
    out.generation = gen;
    for (crd::usize i = 0; i < host.binds().size() && i < script.breaks.size(); ++i)
    {
        out.binds.push_back(ScriptBind{script.breaks[i], host.binds()[i].status, host.binds()[i].sites});
    }

    // This thread is the controller: it waits for each stop, has the paused executing thread read the watched values,
    // then applies the next action. Every wait is bounded.
    crd::usize          next            = 0U;
    bool                script_cancel   = false;
    bool                caller_cancel   = false;
    insp::Refusal       ended           = insp::Refusal::None;
    insp::ValueSnapshot value(out.allocator);
    for (;;)
    {
        insp::StopRecord    stop;
        const insp::Refusal r = wait_stop(host, gen, script, stop, caller_cancel);
        if (caller_cancel)
        {
            cancel_run(host, gen, script);
            break;
        }
        if (r != insp::Refusal::None)
        {
            ended = r;
            break;
        }
        if (out.stops.size() == script.max_stops)
        {
            cancel_run(host, gen, script);
            out.truncated = true;
            break;
        }
        ScriptStop s;
        s.sequence = stop.sequence;
        s.reason   = stop.reason;
        if (const Origin* o = host.stop_origin(stop); o != nullptr)
        {
            s.file_id = o->loc.file_id;
            s.line    = o->loc.line;
            s.col     = o->loc.col;
        }
        s.depth       = stop.depth;
        s.op          = stop.op.value;
        s.first_value = static_cast<u32>(out.values.size());
        read_values(host, gen, script, value, out);
        s.action = (next < script.actions.size()) ? script.actions[next++] : ScriptAction::Continue;
        out.stops.push_back(s);
        if (script.on_stop != nullptr)
        {
            script.on_stop(script.user, s.sequence);
        }
        if (s.action == ScriptAction::Cancel)
        {
            cancel_run(host, gen, script);
            script_cancel = true;
            break;
        }
        if (const insp::Refusal rr = host.session().resume(gen, resume_of(s.action)); rr != insp::Refusal::None)
        {
            cancel_run(host, gen, script);
            ended = rr;
            break;
        }
    }
    const bool joined = host.wait_finished(script.wait_ms); // otherwise the host's destructor cancels and joins

    const plan::RunResult& result = host.result();
    if (!joined || (ended != insp::Refusal::None && ended != insp::Refusal::Finished))
    {
        out.outcome = ScriptOutcome::Unfinished;
        out.refusal = ended != insp::Refusal::None ? ended : insp::Refusal::Timeout;
        return;
    }
    if (result.ok())
    {
        out.outcome = ScriptOutcome::Finished;
        for (crd::usize i = 0; i < result.values.size(); ++i)
        {
            out.results.push_back(result.values[i]);
        }
        return;
    }
    if (result.error == plan::RunError::Cancelled && caller_cancel)
    {
        out.outcome = ScriptOutcome::CallerCancelled;
        return;
    }
    if (result.error == plan::RunError::Cancelled && (script_cancel || out.truncated))
    {
        out.outcome = ScriptOutcome::Cancelled;
        return;
    }
    out.outcome = ScriptOutcome::Error;
    out.error   = result.error;
    if (const Origin* o = plan::instr_provenance(host.compiled_plan(), result.fault).primary(); o != nullptr)
    {
        out.fault_line = o->loc.line;
        out.fault_col  = o->loc.col;
    }
}
} // namespace crd::ceir::cook
