// sandbox — DIAG.8b INSPECT PANEL (see inspect_panel.hpp).

#include "inspect_panel.hpp"

#include <crd/app/event_dispatcher.hpp>
#include <crd/app/events/input_events.hpp>
#include <crd/app/events/window_events.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/input_ops.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/renderasset/identity.hpp>

#include <cmath>

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
    (void)ceir::input::register_input_ops(ctx); // DIAG.9a: the run reads the panel's host inputs
}

// The packed type is the platform's type value: the two orders are one list (append-only on both sides).
using PlatformType = platform::InputEvent::Type;
using EventType    = ceir::input::EventType;
static_assert(static_cast<crd::u8>(PlatformType::None) == static_cast<crd::u8>(EventType::None));
static_assert(static_cast<crd::u8>(PlatformType::KeyDown) == static_cast<crd::u8>(EventType::KeyDown));
static_assert(static_cast<crd::u8>(PlatformType::KeyUp) == static_cast<crd::u8>(EventType::KeyUp));
static_assert(static_cast<crd::u8>(PlatformType::KeyRepeat) == static_cast<crd::u8>(EventType::KeyRepeat));
static_assert(static_cast<crd::u8>(PlatformType::MouseDown) == static_cast<crd::u8>(EventType::MouseDown));
static_assert(static_cast<crd::u8>(PlatformType::MouseUp) == static_cast<crd::u8>(EventType::MouseUp));
static_assert(static_cast<crd::u8>(PlatformType::MouseMove) == static_cast<crd::u8>(EventType::MouseMove));
static_assert(static_cast<crd::u8>(PlatformType::Scroll) == static_cast<crd::u8>(EventType::Scroll));
static_assert(static_cast<crd::u8>(PlatformType::Resize) == static_cast<crd::u8>(EventType::Resize));
static_assert(EventType::Resize == ceir::input::kLastEventType);

// `v` rounded half away from zero and saturated to 16 signed bits (a NaN is 0).
[[nodiscard]] crd::i16 saturate16(crd::f64 v) noexcept
{
    if (std::isnan(v))
    {
        return 0;
    }
    if (v >= 32767.0)
    {
        return 32767;
    }
    if (v <= -32768.0)
    {
        return -32768;
    }
    return static_cast<crd::i16>(std::lround(v));
}

[[nodiscard]] crd::u8 mods_of(const platform::KeyMods& m) noexcept
{
    crd::u8 bits = 0U;
    if (m.shift)
    {
        bits = static_cast<crd::u8>(bits | ceir::input::kModShift);
    }
    if (m.ctrl)
    {
        bits = static_cast<crd::u8>(bits | ceir::input::kModCtrl);
    }
    if (m.alt)
    {
        bits = static_cast<crd::u8>(bits | ceir::input::kModAlt);
    }
    if (m.super)
    {
        bits = static_cast<crd::u8>(bits | ceir::input::kModSuper);
    }
    return bits;
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

crd::i64 window_event(const platform::InputEvent& e) noexcept
{
    ceir::input::Event out;
    out.type = static_cast<crd::u8>(e.type);
    out.mods = mods_of(e.mods);
    switch (e.type) // no default (-Werror=switch)
    {
    case PlatformType::None:
        return 0;
    case PlatformType::KeyDown:
    case PlatformType::KeyUp:
    case PlatformType::KeyRepeat:
        out.code = static_cast<crd::u16>(e.payload.key.key);
        break;
    case PlatformType::MouseDown:
    case PlatformType::MouseUp:
        out.code = static_cast<crd::u16>(e.payload.mouse_button.button);
        break;
    case PlatformType::MouseMove:
        out.x = saturate16(static_cast<crd::f64>(e.payload.mouse_move.x));
        out.y = saturate16(static_cast<crd::f64>(e.payload.mouse_move.y));
        break;
    case PlatformType::Scroll:
        out.x = saturate16(static_cast<crd::f64>(e.payload.scroll.dx) * 100.0);
        out.y = saturate16(static_cast<crd::f64>(e.payload.scroll.dy) * 100.0);
        break;
    case PlatformType::Resize:
        out.x = saturate16(static_cast<crd::f64>(e.payload.resize.width));
        out.y = saturate16(static_cast<crd::f64>(e.payload.resize.height));
        break;
    }
    return ceir::input::pack_event(out);
}

InspectPanel::InspectPanel(memory::IAllocator* alloc, scenerender::SceneRenderer& programs)
    : m_alloc(alloc), m_programs(&programs), m_fixed_events(alloc), m_window(alloc),
      m_run_inputs("sandbox-inspect-inputs"), m_host(alloc, &register_program_dialects, nullptr), m_rel(alloc),
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
    if (m_host.running())
    {
        m_last_refusal = insp::Refusal::Busy; // the running execution still reads the host inputs
        return m_last_refusal;
    }
    // Every run reads its streams from their first draw, the clock as it is now and its events from the first.
    m_run_inputs.set(m_inputs.seeded, m_inputs.seed, m_inputs.clock,
                     m_inputs.window_events ? ceir::cook::HostEventsSpec{} : m_inputs.events);
    if (m_inputs.frame_clock)
    {
        ceir::input::HostClock& clock = m_run_inputs.clock();
        clock.set_reading(ceir::cook::kSimDomain, m_frame_time_ns);
        clock.set_step(ceir::cook::kSimDomain, m_frame_step_ns);
        clock.set_reading(ceir::cook::kFrameDomain, m_frame_index);
        clock.set_step(ceir::cook::kFrameDomain, 1);
    }
    if (m_inputs.window_events)
    {
        // The staged window events, oldest first, into an open queue (empty when none arrived). The run's thread does
        // not exist yet, and the frame loop only stages into m_window from now on.
        ceir::input::HostEvents& queue = m_run_inputs.events();
        (void)queue.open(ceir::cook::kEventQueue);
        const auto n = static_cast<crd::u32>(m_window.size());
        for (crd::u32 i = 0U; i < n; ++i)
        {
            // At most kMaxWindowEvents, the queue's own bound, so every push is kept.
            (void)queue.push(ceir::cook::kEventQueue, m_window[(m_window_head + i) % n]);
        }
    }
    const insp::Refusal r = m_host.start(args, recording, m_run_inputs.source());
    m_last_refusal        = r;
    if (r == insp::Refusal::None && m_inputs.window_events)
    {
        // Taken by this run: the next run reads only what arrives from now on.
        m_run_window_events  = static_cast<crd::u32>(m_window.size());
        m_run_window_dropped = m_window_dropped;
        m_window.clear();
        m_window_head    = 0U;
        m_window_dropped = 0U;
    }
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

void InspectPanel::set_inputs(const PanelInputs& inputs)
{
    m_inputs = inputs;
    m_fixed_events.clear();
    for (const crd::i64 e : inputs.events.events)
    {
        m_fixed_events.push_back(e);
    }
    m_inputs.events.events = containers::as_const_span(m_fixed_events);
}

void InspectPanel::push_window_event(const platform::InputEvent& e)
{
    if (!m_inputs.window_events || e.type == PlatformType::None)
    {
        return;
    }
    const crd::i64 packed = window_event(e);
    if (m_window.size() < kMaxWindowEvents)
    {
        m_window.push_back(packed);
        return;
    }
    // Full: the newest replaces the oldest, which moves the ring's start on.
    m_window[m_window_head] = packed;
    m_window_head           = (m_window_head + 1U) % kMaxWindowEvents;
    ++m_window_dropped;
}

void InspectEventLayer::on_event(app::Event& event)
{
    app::EventDispatcher d(event);
    platform::InputEvent e;
    const auto           key = [&e](PlatformType type, platform::Key k, const platform::KeyMods& mods)
    {
        e.type            = type;
        e.mods            = mods;
        e.payload.key.key = k;
        return false; // never handled: every other consumer still sees it
    };
    const auto button = [&e](PlatformType type, platform::MouseButton b, const platform::KeyMods& mods)
    {
        e.type                        = type;
        e.mods                        = mods;
        e.payload.mouse_button.button = b;
        return false;
    };
    (void)d.dispatch<app::KeyPressedEvent>(
        [&key](app::KeyPressedEvent& k)
        { return key(k.repeated() ? PlatformType::KeyRepeat : PlatformType::KeyDown, k.key(), k.mods()); });
    (void)d.dispatch<app::KeyReleasedEvent>([&key](app::KeyReleasedEvent& k)
                                            { return key(PlatformType::KeyUp, k.key(), k.mods()); });
    (void)d.dispatch<app::MouseButtonPressedEvent>([&button](app::MouseButtonPressedEvent& b)
                                                   { return button(PlatformType::MouseDown, b.button(), b.mods()); });
    (void)d.dispatch<app::MouseButtonReleasedEvent>([&button](app::MouseButtonReleasedEvent& b)
                                                    { return button(PlatformType::MouseUp, b.button(), b.mods()); });
    (void)d.dispatch<app::MouseMovedEvent>(
        [&e](app::MouseMovedEvent& m)
        {
            e.type                 = PlatformType::MouseMove;
            e.payload.mouse_move.x = m.x();
            e.payload.mouse_move.y = m.y();
            return false;
        });
    (void)d.dispatch<app::MouseScrolledEvent>(
        [&e](app::MouseScrolledEvent& m)
        {
            e.type              = PlatformType::Scroll;
            e.payload.scroll.dx = m.dx();
            e.payload.scroll.dy = m.dy();
            return false;
        });
    (void)d.dispatch<app::WindowResizeEvent>(
        [&e](app::WindowResizeEvent& w)
        {
            e.type                  = PlatformType::Resize;
            e.payload.resize.width  = w.width();
            e.payload.resize.height = w.height();
            return false;
        });
    m_panel->push_window_event(e); // any other event leaves e a None event, which is not staged
}
} // namespace crd::sandbox
