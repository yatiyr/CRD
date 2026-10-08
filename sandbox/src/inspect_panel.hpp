#pragma once

// sandbox — DIAG.8b INSPECT PANEL: the sandbox's consumer of runtime inspection. It runs an AUTHORED CEIR program
// (an asset, loaded app-first through the renderer's program seam) on crd-ceir-cook's `InspectHost` and lets the
// frame loop control it: breakpoints on authored lines, watched lines, step into/over/out, continue, pause and cancel.
//
// ⛔ THE FRAME LOOP IS THE CONTROLLER. The panel is constructed, ticked and commanded on the frame-loop thread (the
// host declares it the session's controller); the program runs on the host's own executing thread. `tick` NEVER waits
// on the running program: it polls for a stop with a zero timeout and returns, so frames keep presenting while the
// program runs or sits at a breakpoint. The only waits are the snapshots taken ONCE per new stop, which the paused
// executing thread answers (each bounded); a frame then draws from that cache.
//
// ⛔ IDENTITY. The program is named by its canonical folder/name (`ceir/inspect_demo`), not by the mount that won: the
// asset id and the file name its breakpoints use stay the same when an application file starts or stops shadowing
// the engine default, so a reload that swaps the winning mount keeps the asset and rebinds every `file:line`.
// `source()` reports which mount the installed text came from.
//
// ⛔ GENERATIONS. Commands carry the generation the caller saw (`generation()`); a command for a replaced generation is
// refused `StaleGeneration` by the session before any work. A load while the program runs or is paused is `Busy`.
//
// ⛔ WINDOW EVENTS (DIAG.9a). The frame loop hands the panel the window's input events (`InspectEventLayer`, pushed
// on the application, rebuilds each `platform::InputEvent` the application dispatched); the panel stages them on the
// frame-loop thread and only `start` copies them into the run's event queue, before the run's thread exists. A run
// never reads a queue the frame loop is still writing: events that arrive while it runs feed the NEXT run.

#include <crd/app/event.hpp>
#include <crd/app/layer.hpp>
#include <crd/ceir/cook/inspect_host.hpp>
#include <crd/ceir/cook/replay_diag.hpp> // RunInputs, HostClockSpec
#include <crd/ceir/inspect.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/input.hpp>
#include <crd/scenerender/scene_renderer.hpp>

namespace crd::sandbox
{
// NOLINTNEXTLINE(performance-enum-size)
enum class PanelState : crd::u8
{
    Idle = 0,  // nothing started since the last load
    Running,   // the program runs on the executing thread
    Paused,    // the program is held at `stop()`
    Finished,  // the run returned `results()`
    Cancelled, // the run ended at a cancel
    Failed,    // the run ended with an error (`error()`)
};
[[nodiscard]] containers::StringView panel_state_name(PanelState s) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class PanelAction : crd::u8
{
    Continue = 0,
    StepInto,
    StepOver,
    StepOut,
    Cancel,
};
[[nodiscard]] containers::StringView panel_action_name(PanelAction a) noexcept;
[[nodiscard]] bool                   parse_panel_action(containers::StringView s, PanelAction& out) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class TickEvent : crd::u8
{
    None = 0, // still running, still paused, or nothing started
    Stopped,  // a new stop was taken this tick (its values are cached)
    Ended,    // the run ended this tick (its thread is joined)
};

// One watched line's value at the current stop.
struct PanelValue
{
    crd::u32                      line    = 0U;
    ceir::inspect::Refusal        refusal = ceir::inspect::Refusal::None; // the snapshot request itself
    ceir::inspect::ValueSnapshot  value;
    explicit PanelValue(memory::IAllocator* a) : value(a) {}
};

// DIAG.9a: the host inputs every run of the panel reads, set again by every `start`. Seeded: input.random reads the
// `SeededInputs` of `seed` from draw 0 (so "Run again" reads the same draws); otherwise the host has no random source
// and a draw fails input-unavailable. `clock`: input.clock and input.time_step read the domains it sets (the live
// wall, the sim reading and step), as replay.record's clock arguments do; a domain it leaves unset has no value.
// `frame_clock`: the sim and frame domains read the frame loop's clock (`set_frame_clock`) as it was at the start:
// sim reads the frame loop's time and its last frame step in nanoseconds, frame reads the frame index with a step of
// one frame. It replaces `clock`'s sim domain. The clock never changes while a run reads it, so a run held across
// frames keeps reading its start's frame clock. `events`: input.event {queue = 0} takes these events in order, as
// replay.record's `events` does (`set_inputs` copies them; an open spec with none is an empty queue); without them
// the host has no event queue and a read fails input-unavailable. `window_events`: queue 0 holds, in order, the window
// events the frame loop pushed (`push_window_event`) since the previous successful start, at most the newest
// `kMaxWindowEvents`; it replaces `events`.
struct PanelInputs
{
    bool                       seeded = false;
    crd::u64                   seed   = 0U;
    ceir::cook::HostClockSpec  clock{};
    bool                       frame_clock = false;
    ceir::cook::HostEventsSpec events{};
    bool                       window_events = false;
};

// DIAG.9a: the most window events one run reads (the event queue's own bound); older ones are dropped and counted.
inline constexpr crd::u32 kMaxWindowEvents = ceir::input::HostEvents::kMaxEvents;

// DIAG.9a: a window input event as input.event reads it, packed (ceir::input::pack_event): the type in the platform's
// order, the key or mouse button as its platform enum value, the modifiers as shift 1, ctrl 2, alt 4 and super 8, and
// x and y saturated to 16 signed bits: the pointer position rounded to whole pixels, the scroll offset in hundredths of
// a step (rounded), the new size in pixels; 0 where a type has none. A None event packs to 0.
[[nodiscard]] crd::i64 window_event(const platform::InputEvent& e) noexcept;

struct PanelLoad
{
    scenerender::ProgramSource source = scenerender::ProgramSource::NotFound;
    ceir::cook::HostLoadResult host{};
    [[nodiscard]] bool         ok() const noexcept
    {
        return source != scenerender::ProgramSource::NotFound && host.ok();
    }
};

class InspectPanel
{
public:
    // `programs` resolves the program text (app-first); it must outlive the panel. Construct on the frame-loop thread.
    InspectPanel(memory::IAllocator* alloc, scenerender::SceneRenderer& programs);
    InspectPanel(const InspectPanel&)            = delete;
    InspectPanel& operator=(const InspectPanel&) = delete;
    InspectPanel(InspectPanel&&)                 = delete;
    InspectPanel& operator=(InspectPanel&&)      = delete;
    ~InspectPanel()                              = default; // the host cancels and joins a running program

    // Resolve `rel` (folder/name, no scheme, no extension) app-first and load or hot-reload it, compiling `entry`. The
    // first successful load fixes the program's `rel`; a later load of another name is a LoadFailed.
    [[nodiscard]] PanelLoad load(containers::StringView rel, containers::StringView entry);

    // Configuration before `start` (Busy while the program runs). Lines are 1-based authored lines of the program.
    [[nodiscard]] ceir::inspect::Refusal add_breakpoint(crd::u32 line);
    void                                 watch(crd::u32 line);
    // Actions applied, in order, at each new stop; once they are used up every later stop continues. Without a
    // script the panel waits for a command at each stop.
    void set_script(containers::ConstSpan<PanelAction> actions);
    // DIAG.9a: the host inputs of every later `start` (see PanelInputs). The events are copied.
    void                      set_inputs(const PanelInputs& inputs);
    [[nodiscard]] PanelInputs inputs() const noexcept { return m_inputs; }
    // DIAG.9a: one event the window delivered, in arrival order (frame-loop thread). Staged only for a
    // `window_events` panel, and never a None event; past kMaxWindowEvents the oldest staged event is dropped.
    void push_window_event(const platform::InputEvent& e);
    // The window events staged for the next start, and how many older ones were dropped since the last start.
    [[nodiscard]] crd::u32 window_events_staged() const noexcept { return static_cast<crd::u32>(m_window.size()); }
    [[nodiscard]] crd::u64 window_events_dropped() const noexcept { return m_window_dropped; }
    // What the latest successful start gave its run: the window events it queued, and how many it lost to the bound.
    [[nodiscard]] crd::u32 run_window_events() const noexcept { return m_run_window_events; }
    [[nodiscard]] crd::u64 run_window_dropped() const noexcept { return m_run_window_dropped; }
    // DIAG.9a: the frame loop's clock, once per frame: its time and last frame step in nanoseconds and its frame
    // index. A `frame_clock` run reads the values given before its `start`.
    void set_frame_clock(crd::i64 time_ns, crd::i64 step_ns, crd::i64 frame) noexcept
    {
        m_frame_time_ns = time_ns;
        m_frame_step_ns = step_ns;
        m_frame_index   = frame;
    }

    // Start the installed generation with `args` (NotBound before a successful load, Busy while running). DIAG.9a:
    // `recording` records the run on the host; `host().record` gives the run record once it has ended. The run reads
    // the host inputs `set_inputs` chose, from their first draw, the frame clock as it is now and, for a
    // `window_events` panel, the window events staged so far (taken only when the start succeeds).
    [[nodiscard]] ceir::inspect::Refusal start(containers::ConstSpan<crd::i64> args,
                                               ceir::cook::HostRecording recording = {});

    // Once per frame. Never waits on the running program (see the header).
    TickEvent tick();

    // Commands for the generation the caller saw; refused before any work otherwise.
    [[nodiscard]] ceir::inspect::Refusal command(crd::u64 generation, PanelAction action);
    [[nodiscard]] ceir::inspect::Refusal request_pause(crd::u64 generation);

    [[nodiscard]] PanelState                          state() const noexcept { return m_state; }
    [[nodiscard]] crd::u64                            generation() const noexcept { return m_host.generation(); }
    [[nodiscard]] scenerender::ProgramSource          source() const noexcept { return m_source; }
    [[nodiscard]] containers::StringView              file() const noexcept;
    [[nodiscard]] crd::u64                            ticks() const noexcept { return m_ticks; }
    [[nodiscard]] crd::u32                            stops() const noexcept { return m_stops; }
    [[nodiscard]] const ceir::inspect::StopRecord&    stop() const noexcept { return m_stop; }
    [[nodiscard]] crd::u32                            stop_line() const noexcept { return m_stop_line; }
    [[nodiscard]] crd::u32                            stop_col() const noexcept { return m_stop_col; }
    [[nodiscard]] const containers::Array<PanelValue>& values() const noexcept { return m_values; }
    [[nodiscard]] const containers::Array<ceir::inspect::BindReport>& binds() const noexcept { return m_host.binds(); }
    [[nodiscard]] containers::ConstSpan<crd::i64>     results() const noexcept;
    [[nodiscard]] ceir::plan::RunError                error() const noexcept { return m_host.result().error; }
    [[nodiscard]] ceir::inspect::Refusal              last_refusal() const noexcept { return m_last_refusal; }
    // The script action applied at the latest stop (Continue when the script is exhausted or absent).
    [[nodiscard]] bool        scripted() const noexcept { return !m_script.empty(); }
    [[nodiscard]] PanelAction last_scripted() const noexcept { return m_last_scripted; }
    [[nodiscard]] ceir::cook::InspectHost&            host() noexcept { return m_host; }

private:
    void capture(const ceir::inspect::StopRecord& rec); // the frame's cache of one new stop
    void ended();                                       // classify the joined run

    memory::IAllocator*                m_alloc;
    scenerender::SceneRenderer*        m_programs;
    PanelInputs                        m_inputs{};
    containers::Array<crd::i64>        m_fixed_events;        // PanelInputs::events, copied
    containers::Array<crd::i64>        m_window;              // staged window events, a ring once full
    crd::u32                           m_window_head    = 0U; // the oldest staged event once the ring is full
    crd::u64                           m_window_dropped = 0U;
    crd::u32                           m_run_window_events  = 0U;
    crd::u64                           m_run_window_dropped = 0U;
    crd::i64                           m_frame_time_ns = 0;
    crd::i64                           m_frame_step_ns = 0;
    crd::i64                           m_frame_index   = 0;
    // The run's host inputs, before m_host: the host's executing thread reads (and grows) them until joined, while
    // the frame loop allocates from m_alloc.
    ceir::cook::RunInputs              m_run_inputs;
    ceir::cook::InspectHost            m_host;
    containers::String                 m_rel;
    scenerender::ProgramSource         m_source = scenerender::ProgramSource::NotFound;
    containers::Array<crd::u32>        m_watches;
    containers::Array<PanelValue>      m_values;
    containers::Array<PanelAction>     m_script;
    crd::usize                         m_next_action  = 0U;
    PanelAction                        m_last_scripted = PanelAction::Continue;
    PanelState                         m_state        = PanelState::Idle;
    ceir::inspect::StopRecord          m_stop{};
    crd::u32                           m_stop_line    = 0U;
    crd::u32                           m_stop_col     = 0U;
    crd::u32                           m_stops        = 0U;
    crd::u64                           m_ticks        = 0U;
    ceir::inspect::Refusal             m_last_refusal = ceir::inspect::Refusal::None;
};

// DIAG.9a: the application layer that hands the panel every window input event the application dispatches, rebuilt
// as the `platform::InputEvent` it came from (key, mouse button, pointer, scroll and resize events). It never marks an
// event handled, so every other consumer still sees it. Push it on the application (frame-loop thread); it must not
// outlive the panel.
class InspectEventLayer final : public app::Layer
{
public:
    explicit InspectEventLayer(InspectPanel& panel) : app::Layer("ceir-inspect-events"), m_panel(&panel) {}
    void on_event(app::Event& event) override;

private:
    InspectPanel* m_panel;
};
} // namespace crd::sandbox
