// sandbox — DIAG.8b INSPECT PANEL (see inspect_panel.hpp).

#include "inspect_panel.hpp"

#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/renderasset/identity.hpp>

namespace crd::sandbox
{
namespace
{
namespace insp = crd::ceir::inspect;

// A snapshot is answered by the PAUSED executing thread within microseconds; the bound only keeps a frame from
// hanging if something else (a native debugger) holds that thread.
constexpr crd::u32 kSnapshotMs = 2000U;

// The dialects the compiled plan runs (its scalar host subset), installed into every generation's Context.
void register_program_dialects(ceir::Context& ctx, void* /*user*/)
{
    (void)ceir::arith::register_arith_ops(ctx);
    (void)ceir::core::register_core_ops(ctx);
    (void)ceir::func::register_dialect(ctx);
}

[[nodiscard]] insp::Resume resume_of(PanelAction a) noexcept
{
    switch (a) // ⛔ no default (-Werror=switch)
    {
    case PanelAction::StepInto:
        return insp::Resume::StepInto;
    case PanelAction::StepOver:
        return insp::Resume::StepOver;
    case PanelAction::StepOut:
        return insp::Resume::StepOut;
    case PanelAction::Continue:
    case PanelAction::Cancel:
        return insp::Resume::Continue;
    }
    return insp::Resume::Continue;
}
} // namespace

containers::StringView panel_state_name(PanelState s) noexcept
{
    switch (s) // ⛔ no default (-Werror=switch)
    {
    case PanelState::Idle:
        return "idle";
    case PanelState::Running:
        return "running";
    case PanelState::Paused:
        return "paused";
    case PanelState::Finished:
        return "finished";
    case PanelState::Cancelled:
        return "cancelled";
    case PanelState::Failed:
        return "failed";
    }
    return "?";
}

containers::StringView panel_action_name(PanelAction a) noexcept
{
    switch (a) // ⛔ no default (-Werror=switch)
    {
    case PanelAction::Continue:
        return "continue";
    case PanelAction::StepInto:
        return "into";
    case PanelAction::StepOver:
        return "over";
    case PanelAction::StepOut:
        return "out";
    case PanelAction::Cancel:
        return "cancel";
    }
    return "?";
}

bool parse_panel_action(containers::StringView s, PanelAction& out) noexcept
{
    static constexpr PanelAction kAll[] = {PanelAction::Continue, PanelAction::StepInto, PanelAction::StepOver,
                                           PanelAction::StepOut, PanelAction::Cancel};
    for (const PanelAction a : kAll)
    {
        if (s == panel_action_name(a))
        {
            out = a;
            return true;
        }
    }
    return false;
}

InspectPanel::InspectPanel(memory::IAllocator* alloc, scenerender::SceneRenderer& programs)
    : m_alloc(alloc), m_programs(&programs), m_host(alloc, &register_program_dialects, nullptr), m_rel(alloc),
      m_watches(alloc), m_values(alloc), m_script(alloc)
{
}

containers::StringView InspectPanel::file() const noexcept
{
    return containers::StringView(m_rel.c_str(), m_rel.size());
}

containers::ConstSpan<crd::i64> InspectPanel::results() const noexcept
{
    if (m_state != PanelState::Finished)
    {
        return {};
    }
    const containers::Array<crd::i64>& v = m_host.result().values;
    return containers::ConstSpan<crd::i64>(v.data(), v.size());
}

PanelLoad InspectPanel::load(containers::StringView rel, containers::StringView entry)
{
    PanelLoad out;
    out.host.generation = m_host.generation();
    if (m_host.running())
    {
        out.host.status = ceir::cook::HostLoad::Busy; // checked first: nothing is read while the program is held
        return out;
    }
    containers::String name(m_alloc); // NUL-terminated for the resolver
    name.append(rel.data(), rel.size());
    containers::String text(m_alloc);
    out.source = m_programs->resolve_program_text(name.c_str(), text);
    if (out.source == scenerender::ProgramSource::NotFound)
    {
        return out;
    }
    // The asset id and the file name are the canonical folder/name, never the winning mount (see IDENTITY).
    const ceir::cook::AssetId id = renderasset::asset_id_of(rel);
    out.host = m_host.load(id, containers::StringView(text.c_str(), text.size()), rel, entry);
    if (out.host.ok())
    {
        if (m_rel.empty())
        {
            m_rel.append(rel.data(), rel.size());
        }
        m_source = out.source;
        m_state  = PanelState::Idle;
    }
    return out;
}

ceir::inspect::Refusal InspectPanel::add_breakpoint(crd::u32 line)
{
    crd::u32 index = 0U;
    return m_host.add_line_breakpoint(file(), line, index);
}

void InspectPanel::watch(crd::u32 line)
{
    m_watches.push_back(line);
    PanelValue v(m_alloc);
    v.line = line;
    m_values.push_back(static_cast<PanelValue&&>(v));
}

void InspectPanel::set_script(containers::ConstSpan<PanelAction> actions)
{
    m_script.clear();
    for (crd::usize i = 0; i < actions.size(); ++i)
    {
        m_script.push_back(actions[i]);
    }
    m_next_action = 0U;
}

ceir::inspect::Refusal InspectPanel::start(containers::ConstSpan<crd::i64> args, ceir::cook::HostRecording recording)
{
    const insp::Refusal r = m_host.start(args, recording);
    m_last_refusal        = r;
    if (r == insp::Refusal::None)
    {
        m_state       = PanelState::Running;
        m_stop        = insp::StopRecord{};
        m_stop_line   = 0U;
        m_stop_col    = 0U;
        m_next_action = 0U;
    }
    return r;
}

TickEvent InspectPanel::tick()
{
    ++m_ticks;
    if (m_state != PanelState::Running)
    {
        return TickEvent::None; // paused: the stop's values are cached; idle or ended: nothing to poll
    }
    insp::StopRecord    rec;
    const insp::Refusal r = m_host.session().wait_for_stop(m_host.generation(), 0U, rec); // a poll, never a wait
    if (r == insp::Refusal::None)
    {
        if (rec.sequence == m_stop.sequence)
        {
            // The stop already taken: a cancel was accepted at it, and the executing thread has not yet left it (a
            // cancel, unlike a resume, does not move the session off the stop). Not a new stop: keep polling.
            return TickEvent::None;
        }
        m_state = PanelState::Paused;
        ++m_stops;
        capture(rec);
        if (!m_script.empty())
        {
            const PanelAction a = (m_next_action < m_script.size()) ? m_script[m_next_action++] : PanelAction::Continue;
            m_last_scripted     = a;
            (void)command(m_host.generation(), a);
        }
        return TickEvent::Stopped;
    }
    if (r == insp::Refusal::Finished && m_host.wait_finished(0U)) // joins only a thread that has already ended
    {
        ended();
        return TickEvent::Ended;
    }
    return TickEvent::None;
}

ceir::inspect::Refusal InspectPanel::command(crd::u64 generation, PanelAction action)
{
    insp::Refusal r = insp::Refusal::None;
    if (action == PanelAction::Cancel)
    {
        r = m_host.session().cancel(generation);
    }
    else
    {
        r = m_host.session().resume(generation, resume_of(action));
    }
    m_last_refusal = r;
    if (r == insp::Refusal::None && m_state == PanelState::Paused)
    {
        m_state = PanelState::Running; // the next tick polls for the next stop or the end
    }
    return r;
}

ceir::inspect::Refusal InspectPanel::request_pause(crd::u64 generation)
{
    m_last_refusal = m_host.session().request_pause(generation);
    return m_last_refusal;
}

void InspectPanel::capture(const ceir::inspect::StopRecord& rec)
{
    m_stop                        = rec;
    const ceir::Origin* const org = m_host.stop_origin(rec);
    m_stop_line                   = (org != nullptr) ? org->loc.line : 0U;
    m_stop_col                    = (org != nullptr) ? org->loc.col : 0U;
    for (crd::usize i = 0; i < m_values.size(); ++i)
    {
        PanelValue&          v   = m_values[i];
        const insp::ValueRef ref{m_host.op_at_line(file(), v.line), 0U};
        v.refusal = m_host.session().snapshot(rec.generation, ref, v.value, kSnapshotMs);
    }
}

void InspectPanel::ended()
{
    const ceir::plan::RunResult& res = m_host.result();
    if (res.ok())
    {
        m_state = PanelState::Finished;
    }
    else if (res.error == ceir::plan::RunError::Cancelled)
    {
        m_state = PanelState::Cancelled;
    }
    else
    {
        m_state = PanelState::Failed;
    }
}
} // namespace crd::sandbox
