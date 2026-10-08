// DIAG.8b: the SANDBOX consumer of runtime inspection (sandbox/src/inspect_panel.cpp, compiled into this test the way
// the showcase tests compile the sandbox's own sources). The panel loads the committed authored program
// assets/ceir/inspect_demo.ceir through the renderer's app-first program seam (`SceneRenderer::resolve_program_text`,
// the `ceir` folder registered in render-asset-core) and runs it on crd-ceir-cook's InspectHost while this test thread
// plays the sandbox's frame loop: it calls `tick` once per "frame" and issues the panel's commands. No device: the
// renderer is constructed only for its asset mounts. Covered: an application file shadows the engine default (and a
// reload that swaps the winning mount keeps the asset and its breakpoints); frames keep ticking, each quickly, while
// the program sits at a breakpoint and while it runs; a breakpoint stop reports its authored line, depth and typed
// values, including the unavailable ones; step into, step out and continue; a pause request lands in the running
// loop and a cancel ends it; a scripted run; a load while paused is Busy; commands for a replaced generation are
// refused; destroying the panel while paused returns. DIAG.9a: a run the frame loop holds, steps and lets fault is
// recorded on the panel's host and replays without a session to the same trace and fault; a run started again without
// recording forgets the record. The panel's seeded host inputs reach every run from the first draw (the held run
// reads them, "Run again" reads the same draws, and the record replays from its draws alone); without inputs a draw
// fails. The panel's clock reaches every run: the held run reads the step it was given and the record replays from its
// reads alone; a frame-clock run reads the frame loop's time, step and frame index as they were at its start, even
// when the loop moves on while the run is held. The panel's events reach every run: a window input event packs as
// input.event reads it (quantized and saturated, against a layout spelled out here); listed events reach every run
// from the first; the application layer stages the window's events without handling them, a run takes those staged
// before its start (a held run's later events feed the next run, and a Busy start takes none), its record holds them
// and replays from them alone, nothing staged is an open, empty queue, and past the bound the newest are kept and
// the dropped counted. Expected lines are scanned from the text, never taken from the parser or the panel.
// ASCII test names.

#include "inspect_panel.hpp"

#include <crd/app/events/input_events.hpp>
#include <crd/app/events/window_events.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/input_ops.hpp>
#include <crd/ceir/input.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderasset/identity.hpp>
#include <crd/renderasset/renderasset.hpp>
#include <crd/scenerender/scene_renderer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <thread>

using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::sandbox::InspectPanel;
using crd::sandbox::PanelAction;
using crd::sandbox::PanelState;
using crd::sandbox::TickEvent;
using crd::scenerender::ProgramSource;
namespace insp = crd::ceir::inspect;
namespace fs   = crd::platform::fs;

namespace
{
constexpr const char* kEngineAssets = CRD_SANDBOX_INSPECT_ENGINE_ASSETS;
constexpr const char* kRel          = "ceir/inspect_demo";
constexpr u32         kWaitMs       = 20000U; // generous: a sanitizer lane is slow, and a pass never waits this long
// A frame's tick must return promptly: it polls and never waits on the program. Generous for sanitizer lanes, and far
// below the controller wait a blocking tick would sit in.
constexpr double kFrameBoundMs = 1000.0;

StringView sv(const String& s)
{
    return StringView(s.data(), s.size());
}

String read_text(const fs::Path& p, crd::memory::IAllocator* alloc)
{
    String out(alloc);
    REQUIRE(fs::read_file_text(p, out));
    return out;
}

// The 1-based line of the first line containing `needle`.
u32 line_of(StringView text, StringView needle)
{
    u32   line  = 1U;
    usize start = 0U;
    for (usize i = 0; i <= text.size(); ++i)
    {
        if (i == text.size() || text[i] == '\n')
        {
            if (text.substr(start, i - start).find(needle) != StringView::npos)
            {
                return line;
            }
            ++line;
            start = i + 1U;
        }
    }
    return 0U;
}

struct Lines
{
    u32 h, a, len, call, loop, x;
    explicit Lines(StringView t)
        : h(line_of(t, "arith.muli")), a(line_of(t, "%7 = arith.addi")), len(line_of(t, "!qty")),
          call(line_of(t, "func.call")), loop(line_of(t, "core.for")), x(line_of(t, "%11 = arith.addi"))
    {
    }
};

// The committed program with its constant 2 replaced by 5: main returns ((1 + 5) * 2)^2 = 144.
String shadow_text(StringView text, crd::memory::IAllocator* alloc)
{
    const StringView from = "{value = 2}";
    const usize      at   = text.find(from);
    REQUIRE(at != StringView::npos);
    String out(alloc);
    out.append(text.data(), at);
    out.append("{value = 5}");
    out.append(text.data() + at + from.size(), text.size() - at - from.size());
    return out;
}

// A fresh application asset root under the temp directory, removed on scope exit.
struct AppRoot
{
    fs::Path dir;
    explicit AppRoot(const char* tag) : dir(fs::temp_directory() / StringView(tag))
    {
        (void)fs::remove_all(dir);
        REQUIRE(fs::create_directories(dir / StringView("ceir")));
    }
    void write(StringView text) const
    {
        REQUIRE(fs::write_file_text(dir / StringView("ceir/inspect_demo.ceir"), text));
    }
    ~AppRoot() { (void)fs::remove_all(dir); }
    AppRoot(const AppRoot&)            = delete;
    AppRoot& operator=(const AppRoot&) = delete;
    AppRoot(AppRoot&&)                 = delete;
    AppRoot& operator=(AppRoot&&)      = delete;
};

// The frame loop: tick until `want` (true) or `kWaitMs` elapsed (false). Records the slowest tick.
struct Frames
{
    double worst_ms = 0.0;
    bool   until(InspectPanel& panel, TickEvent want)
    {
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            const auto      t0 = std::chrono::steady_clock::now();
            const TickEvent e  = panel.tick();
            const auto      t1 = std::chrono::steady_clock::now();
            const double    ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            worst_ms           = (ms > worst_ms) ? ms : worst_ms;
            if (e == want)
            {
                return true;
            }
            if (std::chrono::duration<double, std::milli>(t1 - start).count() > kWaitMs)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // `n` frames in which nothing may happen: each returns None.
    bool idle(InspectPanel& panel, u32 n)
    {
        for (u32 i = 0; i < n; ++i)
        {
            const auto      t0 = std::chrono::steady_clock::now();
            const TickEvent e  = panel.tick();
            const auto      t1 = std::chrono::steady_clock::now();
            const double    ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            worst_ms = (ms > worst_ms) ? ms : worst_ms;
            if (e != TickEvent::None)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
};

const crd::sandbox::PanelValue& value_at(const InspectPanel& panel, u32 line)
{
    for (usize i = 0; i < panel.values().size(); ++i)
    {
        if (panel.values()[i].line == line)
        {
            return panel.values()[i];
        }
    }
    FAIL("no watched value at that line");
    return panel.values()[0];
}

// DIAG.9a: the dialects a replay registers into its fresh Context (the panel's own program dialects).
void register_replay_dialects(crd::ceir::Context& ctx, void* /*user*/)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
    (void)crd::ceir::input::register_input_ops(ctx);
}

// Draw `n` of stream 0 for `seed`, reduced as random_demo.ceir's switch reads it.
i64 random_switch_draw(u64 seed, u64 n)
{
    return crd::ceir::input::reduce_draw(crd::ceir::input::SeededInputs::draw(seed, 0U, n), 4U);
}

bool contains(const String& s, StringView needle)
{
    return sv(s).find(needle) != StringView::npos;
}
} // namespace

TEST_CASE("diag 8b: the sandbox panel loads its program app-first through the renderer's program seam",
          "[sandbox][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;

    // The folder is registered: the canonical id names the committed file.
    crd::renderasset::DiagnosticList diags(&alloc);
    const crd::renderasset::AssetRef ref =
        crd::renderasset::AssetRef::parse("engine://ceir/inspect_demo", diags, &alloc);
    REQUIRE(ref.valid());
    CHECK(ref.type() == crd::renderasset::AssetType::Program);
    String rel(&alloc);
    REQUIRE(crd::renderasset::on_disk_relative(ref, rel));
    CHECK(sv(rel) == StringView("ceir/inspect_demo.ceir"));

    const String engine_text =
        read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/inspect_demo.ceir"), &alloc);
    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));

    // No application file: the shipped default.
    String text(&alloc);
    CHECK(renderer.resolve_program_text(kRel, text) == ProgramSource::Engine);
    CHECK(sv(text) == sv(engine_text));
    CHECK(renderer.resolve_program_text("ceir/no_such_program", text) == ProgramSource::NotFound);
    CHECK(text.empty());
    CHECK(renderer.resolve_program_text("nofolder/inspect_demo", text) == ProgramSource::NotFound);

    InspectPanel panel(&alloc, renderer);
    const crd::sandbox::PanelLoad l1 = panel.load(StringView(kRel), StringView("main"));
    REQUIRE(l1.ok());
    CHECK(l1.source == ProgramSource::Engine);
    CHECK(panel.source() == ProgramSource::Engine);
    CHECK(panel.file() == StringView(kRel));
    const Lines ln(sv(engine_text));
    REQUIRE(panel.add_breakpoint(ln.call) == insp::Refusal::None);
    const i64 n = 3;
    Frames    frames;
    REQUIRE(panel.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop_line() == ln.call);
    REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    REQUIRE(panel.state() == PanelState::Finished);
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == 36);

    // An application file under the app root now shadows the default. Reloading the SAME panel swaps the winning
    // mount: the asset and the file name stay the canonical id, so the reload is a new generation of the same program
    // and the breakpoint rebinds to the same authored line.
    const AppRoot app("crd-diag8b-sandbox-shadow");
    const String  shadow = shadow_text(sv(engine_text), &alloc);
    app.write(sv(shadow));
    REQUIRE(renderer.set_app_asset_root(String(app.dir.generic(), &alloc).c_str()));
    CHECK(renderer.resolve_program_text(kRel, text) == ProgramSource::App);
    CHECK(sv(text) == sv(shadow));

    const u64                     gen1 = panel.generation();
    const crd::sandbox::PanelLoad l2   = panel.load(StringView(kRel), StringView("main"));
    REQUIRE(l2.ok());
    CHECK(l2.source == ProgramSource::App);
    CHECK(panel.source() == ProgramSource::App);
    CHECK(panel.generation() > gen1);
    CHECK(panel.file() == StringView(kRel));
    REQUIRE(panel.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(panel.binds().size() == 1U);
    CHECK(panel.binds()[0].status == insp::BindStatus::Bound);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop_line() == ln.call);
    REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == 144); // the application's program ran, not the default

    // A second consumer starting with the app root mounted resolves the application's file first.
    InspectPanel                  fresh(&alloc, renderer);
    const crd::sandbox::PanelLoad l3 = fresh.load(StringView(kRel), StringView("main"));
    REQUIRE(l3.ok());
    CHECK(l3.source == ProgramSource::App);
    // A name nothing resolves loads nothing.
    InspectPanel missing(&alloc, renderer);
    const crd::sandbox::PanelLoad l4 = missing.load(StringView("ceir/no_such_program"), StringView("main"));
    CHECK_FALSE(l4.ok());
    CHECK(l4.source == ProgramSource::NotFound);
    CHECK(missing.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::NotBound);
}

TEST_CASE("diag 8b: the sandbox frame loop keeps ticking while its program is paused, and steps it",
          "[sandbox][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String text = read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/inspect_demo.ceir"), &alloc);
    const Lines  ln(sv(text));
    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));

    InspectPanel panel(&alloc, renderer);
    REQUIRE(panel.load(StringView(kRel), StringView("main")).ok());
    REQUIRE(panel.add_breakpoint(ln.call) == insp::Refusal::None);
    panel.watch(ln.a);
    panel.watch(ln.len);
    panel.watch(ln.call);
    panel.watch(ln.x);
    const i64 n = 3;
    Frames    frames;
    REQUIRE(panel.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.state() == PanelState::Paused);
    CHECK(panel.stop().reason == insp::StopReason::Breakpoint);
    CHECK(panel.stop().depth == 0U);
    CHECK(panel.stop_line() == ln.call);
    CHECK(panel.stops() == 1U);

    // The values were taken once, at the stop, and typed.
    CHECK(value_at(panel, ln.a).refusal == insp::Refusal::None);
    CHECK(value_at(panel, ln.a).value.status == insp::ValueStatus::Available);
    CHECK(value_at(panel, ln.a).value.bits == 3);
    CHECK(contains(value_at(panel, ln.a).value.type_text, "i32"));
    CHECK(value_at(panel, ln.len).value.status == insp::ValueStatus::Available);
    CHECK(value_at(panel, ln.len).value.bits == 7);
    CHECK(value_at(panel, ln.len).value.has_unit);
    CHECK(value_at(panel, ln.call).value.status == insp::ValueStatus::NotYetComputed); // the op at the stop
    CHECK(value_at(panel, ln.x).value.status == insp::ValueStatus::OutOfScope);        // the loop body's value

    // The frame loop keeps running while the program is held: frames tick, nothing changes, nothing waits.
    const u64 ticks = panel.ticks();
    REQUIRE(frames.idle(panel, 50U));
    CHECK(panel.ticks() == ticks + 50U);
    CHECK(panel.state() == PanelState::Paused);
    CHECK(panel.stops() == 1U);

    REQUIRE(panel.command(panel.generation(), PanelAction::StepInto) == insp::Refusal::None);
    CHECK(panel.state() == PanelState::Running);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop().reason == insp::StopReason::Step);
    CHECK(panel.stop().depth == 1U);
    CHECK(panel.stop_line() == ln.h);
    CHECK(value_at(panel, ln.a).value.status == insp::ValueStatus::OutOfScope); // the caller's frame

    REQUIRE(panel.command(panel.generation(), PanelAction::StepOut) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop().depth == 0U);
    CHECK(panel.stop_line() == ln.loop);
    CHECK(value_at(panel, ln.call).value.status == insp::ValueStatus::Available); // refreshed at the new stop
    CHECK(value_at(panel, ln.call).value.bits == 36);
    CHECK(panel.stops() == 3U);

    REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    CHECK(panel.state() == PanelState::Finished);
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == 36);
    REQUIRE(frames.idle(panel, 5U)); // frames continue after the run
    CHECK(frames.worst_ms < kFrameBoundMs);

    // The sandbox's command-line script: the same steps applied by the panel itself at each stop.
    const PanelAction script[2] = {PanelAction::StepInto, PanelAction::StepOut};
    panel.set_script(ConstSpan<PanelAction>(script, 2U));
    REQUIRE(panel.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    CHECK(panel.stops() == 6U); // the breakpoint, the step into the callee, the step out
    CHECK(panel.stop_line() == ln.loop);
    CHECK(panel.last_scripted() == PanelAction::Continue); // the script was used up at the third stop
    CHECK(panel.state() == PanelState::Finished);
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == 36);
}

TEST_CASE("diag 8b: the sandbox frame loop keeps ticking while its program runs, then pauses and cancels it",
          "[sandbox][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String text = read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/inspect_demo.ceir"), &alloc);
    const Lines  ln(sv(text));
    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));

    InspectPanel panel(&alloc, renderer);
    REQUIRE(panel.load(StringView(kRel), StringView("main")).ok());
    panel.watch(ln.call);
    const i64 n = 2147483647; // a loop no frame budget can wait out
    Frames    frames;
    REQUIRE(panel.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(frames.idle(panel, 20U));
    CHECK(panel.state() == PanelState::Running);
    CHECK(frames.worst_ms < kFrameBoundMs);

    REQUIRE(panel.request_pause(panel.generation()) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop().reason == insp::StopReason::PauseRequest);
    CHECK(panel.stop().depth == 0U);
    CHECK(panel.stop_line() >= ln.loop); // inside the loop, the only place left to run
    CHECK(panel.stop_line() <= ln.x + 1U);
    CHECK(value_at(panel, ln.call).value.status == insp::ValueStatus::Available);
    CHECK(value_at(panel, ln.call).value.bits == 36);

    REQUIRE(panel.command(panel.generation(), PanelAction::Cancel) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    CHECK(panel.state() == PanelState::Cancelled);
    CHECK(panel.error() == crd::ceir::plan::RunError::Cancelled);
    CHECK(panel.results().empty());
}

TEST_CASE("diag 8b: the sandbox panel refuses a replaced generation and a load while paused",
          "[sandbox][inspect][reload][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String engine_text =
        read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/inspect_demo.ceir"), &alloc);
    const Lines   ln(sv(engine_text));
    const AppRoot app("crd-diag8b-sandbox-reload");
    app.write(sv(engine_text));
    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));
    REQUIRE(renderer.set_app_asset_root(String(app.dir.generic(), &alloc).c_str()));

    InspectPanel panel(&alloc, renderer);
    REQUIRE(panel.load(StringView(kRel), StringView("main")).ok());
    CHECK(panel.source() == ProgramSource::App);
    const u64 gen1 = panel.generation();
    REQUIRE(panel.add_breakpoint(ln.call) == insp::Refusal::None);
    panel.watch(ln.a);
    const i64 n = 3;
    Frames    frames;
    REQUIRE(panel.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    // An edit saved while the program is held cannot install: its plan and Context are in use.
    app.write(sv(shadow_text(sv(engine_text), &alloc)));
    const crd::sandbox::PanelLoad busy = panel.load(StringView(kRel), StringView("main"));
    CHECK(busy.host.status == crd::ceir::cook::HostLoad::Busy);
    CHECK(panel.generation() == gen1);
    CHECK(panel.state() == PanelState::Paused);
    REQUIRE(panel.command(gen1, PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == 36);

    // After the run the edit installs as a new generation; commands for the replaced one are refused before any work.
    const crd::sandbox::PanelLoad l2 = panel.load(StringView(kRel), StringView("main"));
    REQUIRE(l2.ok());
    const u64 gen2 = panel.generation();
    CHECK(gen2 > gen1);
    REQUIRE(panel.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop_line() == ln.call);
    CHECK(value_at(panel, ln.a).value.bits == 6); // the edited constant, read from the new generation's frame
    CHECK(panel.command(gen1, PanelAction::Continue) == insp::Refusal::StaleGeneration);
    CHECK(panel.request_pause(gen1) == insp::Refusal::StaleGeneration);
    CHECK(panel.command(gen1, PanelAction::Cancel) == insp::Refusal::StaleGeneration);
    CHECK(panel.state() == PanelState::Paused);
    REQUIRE(frames.idle(panel, 5U));
    REQUIRE(panel.command(gen2, PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == 144);
}

TEST_CASE("diag 8b: destroying the sandbox panel while its program is paused returns", "[sandbox][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String text = read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/inspect_demo.ceir"), &alloc);
    const Lines  ln(sv(text));
    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));
    {
        InspectPanel panel(&alloc, renderer);
        REQUIRE(panel.load(StringView(kRel), StringView("main")).ok());
        REQUIRE(panel.add_breakpoint(ln.call) == insp::Refusal::None);
        const i64 n = 3;
        Frames    frames;
        REQUIRE(panel.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
        REQUIRE(frames.until(panel, TickEvent::Stopped));
        CHECK(panel.state() == PanelState::Paused);
    } // the sandbox closes its window mid-stop: the host cancels and joins the executing thread
    SUCCEED("the panel was destroyed while paused");
}

TEST_CASE("diag 9a: the sandbox frame loop records the inspected run, and the record reproduces without a session",
          "[sandbox][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String text = read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/replay_demo.ceir"), &alloc);
    const u32    call = line_of(sv(text), "func.call");
    const u32    sw   = line_of(sv(text), "core.switch");
    REQUIRE(call != 0U);
    REQUIRE(sw != 0U);
    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));

    InspectPanel panel(&alloc, renderer);
    REQUIRE(panel.load(StringView("ceir/replay_demo"), StringView("main")).ok());
    REQUIRE(panel.add_breakpoint(call) == insp::Refusal::None);
    const i64 seed = 1; // main(1) selects a switch region that does not exist
    Frames    frames;
    REQUIRE(panel.start(ConstSpan<i64>(&seed, 1U), crd::ceir::cook::HostRecording{true, 0U}) == insp::Refusal::None);

    // The frame loop holds, steps into the callee and out, then lets it run to its fault.
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    crd::ceir::cook::ReplayRecord rec(&alloc);
    CHECK(panel.host().record(rec) == crd::ceir::cook::HostRecord::Running);
    REQUIRE(frames.idle(panel, 20U));
    REQUIRE(panel.command(panel.generation(), PanelAction::StepInto) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop().depth == 1U);
    REQUIRE(panel.command(panel.generation(), PanelAction::StepOut) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    // Continue through the later call hits to the fault, one frame at a time.
    u32       later = 0U;
    TickEvent e     = TickEvent::Stopped;
    while (e == TickEvent::Stopped && later < 8U)
    {
        REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
        const auto start = std::chrono::steady_clock::now();
        do
        {
            e = panel.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (e == TickEvent::None && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(kWaitMs));
        later += (e == TickEvent::Stopped) ? 1U : 0U;
    }
    REQUIRE(e == TickEvent::Ended);
    CHECK(later >= 1U); // the loop's later iterations hit the breakpoint too
    CHECK(panel.error() == crd::ceir::plan::RunError::SelectorOutOfRange);
    CHECK(frames.worst_ms < kFrameBoundMs);

    // The record names the panel's program version and replays, in a fresh Context and with no session, to the same
    // trace and the same fault at the switch's authored line.
    REQUIRE(panel.host().record(rec) == crd::ceir::cook::HostRecord::Ok);
    CHECK(rec.generation == panel.generation());
    CHECK(rec.asset != 0U);
    CHECK(sv(rec.program_path) == panel.file());
    crd::ceir::Context                ctx(&alloc);
    crd::ceir::cook::ReplayProgram    program(&alloc);
    crd::ceir::cook::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, "main",
                                         &register_replay_dialects, nullptr, program);
    REQUIRE(program.ok());
    crd::ceir::cook::ReplayTrace trace(&alloc);
    crd::ceir::cook::run_traced(program, {rec.args.data(), rec.args.size()}, rec.max_events, nullptr, trace);
    CHECK(crd::ceir::cook::first_divergence(rec, trace).kind == crd::ceir::cook::DivergenceKind::None);
    CHECK(trace.events_total == rec.events_total);
    CHECK(crd::ceir::cook::replay_site_of_op(ctx, program, rec.fault_op).line == sw);

    // "Run again" starts without recording: the record is forgotten.
    REQUIRE(panel.start(ConstSpan<i64>(&seed, 1U)) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    REQUIRE(panel.command(panel.generation(), PanelAction::Cancel) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    CHECK(panel.host().record(rec) == crd::ceir::cook::HostRecord::NotRecorded);
}

TEST_CASE("diag 9a: the sandbox panel's runs read its seeded host inputs, each run from the first draw",
          "[sandbox][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const String text = read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/random_demo.ceir"), &alloc);
    const u32    draw = line_of(sv(text), "input.random() {stream = 0");
    const u32    sw   = line_of(sv(text), "core.switch");
    REQUIRE(draw != 0U);
    REQUIRE(sw != 0U);

    // A seed whose four draws all take a case (from SeededInputs::draw, not from the panel), and what main(4) returns.
    u64 seed = 0U;
    for (u64 s = 1U; s < 10000U && seed == 0U; ++s)
    {
        bool passes = true;
        for (u64 i = 0U; i < 4U; ++i)
        {
            passes = passes && random_switch_draw(s, i) != 3;
        }
        seed = passes ? s : 0U;
    }
    REQUIRE(seed != 0U);
    const i64 expected = crd::ceir::input::reduce_draw(crd::ceir::input::SeededInputs::draw(seed, 1U, 0U), 100U);

    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));
    InspectPanel panel(&alloc, renderer);
    REQUIRE(panel.load(StringView("ceir/random_demo"), StringView("main")).ok());
    const i64 args[1] = {4};

    // No inputs chosen: the host has no random source, and the first draw fails.
    CHECK_FALSE(panel.inputs().seeded);
    Frames frames;
    REQUIRE(panel.start(ConstSpan<i64>(args, 1U)) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    CHECK(panel.state() == PanelState::Failed);
    CHECK(panel.error() == crd::ceir::plan::RunError::InputUnavailable);

    // Seeded and recorded: held at each switch, the watched draw is the seed's draw; the run returns its stream-1
    // draw, and the record holds all five draws and replays from them alone.
    panel.set_inputs(crd::sandbox::PanelInputs{true, seed});
    REQUIRE(panel.add_breakpoint(sw) == insp::Refusal::None);
    panel.watch(draw);
    REQUIRE(panel.start(ConstSpan<i64>(args, 1U), crd::ceir::cook::HostRecording{true, 0U}) == insp::Refusal::None);
    for (u32 n = 0U; n < 4U; ++n)
    {
        REQUIRE(frames.until(panel, TickEvent::Stopped));
        const crd::sandbox::PanelValue& v = value_at(panel, draw);
        REQUIRE(v.value.status == insp::ValueStatus::Available);
        CHECK(v.value.bits == random_switch_draw(seed, n));
        REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    }
    REQUIRE(frames.until(panel, TickEvent::Ended));
    REQUIRE(panel.state() == PanelState::Finished);
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == expected);

    crd::ceir::cook::ReplayRecord rec(&alloc);
    REQUIRE(panel.host().record(rec) == crd::ceir::cook::HostRecord::Ok);
    CHECK(rec.input_reads_total == 5U);
    String missing(&alloc);
    CHECK(crd::ceir::cook::record_missing_inputs(rec, missing));
    crd::ceir::Context             ctx(&alloc);
    crd::ceir::cook::ReplayProgram program(&alloc);
    crd::ceir::cook::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, "main",
                                         &register_replay_dialects, nullptr, program);
    REQUIRE(program.ok());
    crd::ceir::cook::ReplayTrace trace(&alloc);
    crd::ceir::cook::InputFeed   feed({rec.input_reads.data(), rec.input_reads.size()}, trace);
    crd::ceir::cook::run_traced(program, {rec.args.data(), rec.args.size()}, rec.max_events, nullptr, trace,
                                feed.source());
    CHECK(crd::ceir::cook::first_divergence(rec, trace).kind == crd::ceir::cook::DivergenceKind::None);

    // "Run again" reads the same seed from its first draw: the same result, not the streams' continuation.
    for (u32 run = 0U; run < 2U; ++run)
    {
        REQUIRE(panel.start(ConstSpan<i64>(args, 1U)) == insp::Refusal::None);
        for (u32 n = 0U; n < 4U; ++n)
        {
            REQUIRE(frames.until(panel, TickEvent::Stopped));
            CHECK(value_at(panel, draw).value.bits == random_switch_draw(seed, n));
            REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
        }
        REQUIRE(frames.until(panel, TickEvent::Ended));
        CHECK(panel.state() == PanelState::Finished);
        REQUIRE(panel.results().size() == 1U);
        CHECK(panel.results()[0] == expected);
    }
}

TEST_CASE("diag 9a: the sandbox panel's runs read its clock, and a frame-clock run reads the frame loop's at its start",
          "[sandbox][inspect][diag]")
{
    constexpr i64 hitch_ns    = 50000000; // over clock_demo's 33,333,333 ns budget: its switch has no case for it
    constexpr i64 time_ns     = 2500000000;
    constexpr i64 step_ns     = 16666667;
    constexpr i64 frame_index = 150;
    using crd::ceir::cook::ReplayInputRead;
    using crd::ceir::input::InputKind;

    crd::memory::GrowableTlsfAllocator alloc;
    const String text = read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/clock_demo.ceir"), &alloc);
    const u32    step = line_of(sv(text), "input.time_step() {domain = \"sim\"}");
    const u32    sw   = line_of(sv(text), "core.switch");
    REQUIRE(step != 0U);
    REQUIRE(sw != 0U);
    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));

    // The panel's clock: a sim step over budget. The held switch reads it, the run fails there, and the record holds
    // that one read and replays from it alone.
    InspectPanel panel(&alloc, renderer);
    REQUIRE(panel.load(StringView("ceir/clock_demo"), StringView("main")).ok());
    REQUIRE(panel.add_breakpoint(sw) == insp::Refusal::None);
    panel.watch(step);
    crd::sandbox::PanelInputs inputs;
    inputs.clock.has_sim_step = true;
    inputs.clock.sim_step     = hitch_ns;
    panel.set_inputs(inputs);
    const i64 args[1] = {7};
    Frames    frames;
    REQUIRE(panel.start(ConstSpan<i64>(args, 1U), crd::ceir::cook::HostRecording{true, 0U}) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop_line() == sw);
    REQUIRE(value_at(panel, step).value.status == insp::ValueStatus::Available);
    CHECK(value_at(panel, step).value.bits == hitch_ns);
    REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    CHECK(panel.error() == crd::ceir::plan::RunError::SelectorOutOfRange);
    crd::ceir::cook::ReplayRecord rec(&alloc);
    REQUIRE(panel.host().record(rec) == crd::ceir::cook::HostRecord::Ok);
    REQUIRE(rec.input_reads.size() == 1U);
    CHECK(rec.input_reads[0] == ReplayInputRead{InputKind::TimeStep, crd::ceir::cook::kSimDomain, true, hitch_ns});
    String missing(&alloc);
    CHECK(crd::ceir::cook::record_missing_inputs(rec, missing));
    {
        crd::ceir::Context             ctx(&alloc);
        crd::ceir::cook::ReplayProgram program(&alloc);
        crd::ceir::cook::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, "main",
                                             &register_replay_dialects, nullptr, program);
        REQUIRE(program.ok());
        crd::ceir::cook::ReplayTrace trace(&alloc);
        crd::ceir::cook::InputFeed   feed({rec.input_reads.data(), rec.input_reads.size()}, trace);
        crd::ceir::cook::run_traced(program, {rec.args.data(), rec.args.size()}, rec.max_events, nullptr, trace,
                                    feed.source());
        CHECK(crd::ceir::cook::first_divergence(rec, trace).kind == crd::ceir::cook::DivergenceKind::None);
    }

    // An application program that reads the frame and sim domains (and their steps), loaded app-first.
    const AppRoot     app("crd-diag9a-sandbox-frame-clock");
    const StringView  frame_program = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0(%0 : !i64):
        %1 = input.clock() {domain = "frame"} : !i64
        %2 = input.time_step() {domain = "frame"} : !i64
        %3 = input.clock() {domain = "sim"} : !i64
        %4 = input.time_step() {domain = "sim"} : !i64
        %5 = arith.addi(%1, %2) : !i64
        %6 = arith.addi(%3, %4) : !i64
        %7 = arith.addi(%5, %6) : !i64
        func.return(%7)
    }
}
)";
    REQUIRE(fs::write_file_text(app.dir / StringView("ceir/frame_clock.ceir"), frame_program));
    REQUIRE(renderer.set_app_asset_root(String(app.dir.generic(), &alloc).c_str()));
    const u32 sim_read = line_of(frame_program, "%3 = input.clock");
    REQUIRE(sim_read != 0U);

    // The frame loop's clock as the run starts; the loop then moves on while the run is held before its sim read.
    InspectPanel frame_panel(&alloc, renderer);
    REQUIRE(frame_panel.load(StringView("ceir/frame_clock"), StringView("main")).ok());
    REQUIRE(frame_panel.add_breakpoint(sim_read) == insp::Refusal::None);
    crd::sandbox::PanelInputs frame_inputs;
    frame_inputs.frame_clock = true;
    frame_panel.set_inputs(frame_inputs);
    frame_panel.set_frame_clock(time_ns, step_ns, frame_index);
    REQUIRE(frame_panel.start(ConstSpan<i64>(args, 1U), crd::ceir::cook::HostRecording{true, 0U}) ==
            insp::Refusal::None);
    REQUIRE(frames.until(frame_panel, TickEvent::Stopped));
    CHECK(frame_panel.stop_line() == sim_read);
    frame_panel.set_frame_clock(time_ns + step_ns, step_ns + 1, frame_index + 1); // the next frame, while held
    REQUIRE(frame_panel.command(frame_panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(frame_panel, TickEvent::Ended));
    REQUIRE(frame_panel.state() == PanelState::Finished);
    REQUIRE(frame_panel.results().size() == 1U);
    CHECK(frame_panel.results()[0] == frame_index + 1 + time_ns + step_ns);
    REQUIRE(frame_panel.host().record(rec) == crd::ceir::cook::HostRecord::Ok);
    REQUIRE(rec.input_reads.size() == 4U);
    CHECK(rec.input_reads[0] == ReplayInputRead{InputKind::Clock, crd::ceir::cook::kFrameDomain, true, frame_index});
    CHECK(rec.input_reads[1] == ReplayInputRead{InputKind::TimeStep, crd::ceir::cook::kFrameDomain, true, 1});
    CHECK(rec.input_reads[2] == ReplayInputRead{InputKind::Clock, crd::ceir::cook::kSimDomain, true, time_ns});
    CHECK(rec.input_reads[3] == ReplayInputRead{InputKind::TimeStep, crd::ceir::cook::kSimDomain, true, step_ns});

    // "Run again" reads the frame loop's clock as it is at this start.
    REQUIRE(frame_panel.start(ConstSpan<i64>(args, 1U)) == insp::Refusal::None);
    REQUIRE(frames.until(frame_panel, TickEvent::Stopped));
    REQUIRE(frame_panel.command(frame_panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(frame_panel, TickEvent::Ended));
    REQUIRE(frame_panel.results().size() == 1U);
    CHECK(frame_panel.results()[0] == (frame_index + 1) + 1 + (time_ns + step_ns) + (step_ns + 1));
    CHECK(frames.worst_ms < kFrameBoundMs);
}

namespace
{
// The packed layout, spelled out here independently of ceir::input::pack_event: type, code << 8, mods << 24, x << 32
// and y << 48, each of x and y as 16 two's-complement bits.
i64 packed(u64 type, u64 code, u64 mods, i64 x, i64 y)
{
    const u64 ux = static_cast<u64>(x) & 0xFFFFU;
    const u64 uy = static_cast<u64>(y) & 0xFFFFU;
    return static_cast<i64>(type | (code << 8U) | (mods << 24U) | (ux << 32U) | (uy << 48U));
}

crd::platform::InputEvent key_event(crd::platform::InputEvent::Type type, crd::platform::Key k,
                                    crd::platform::KeyMods mods)
{
    crd::platform::InputEvent e;
    e.type            = type;
    e.mods            = mods;
    e.payload.key.key = k;
    return e;
}

crd::platform::InputEvent button_event(crd::platform::InputEvent::Type type, crd::platform::MouseButton b,
                                       crd::platform::KeyMods mods)
{
    crd::platform::InputEvent e;
    e.type                        = type;
    e.mods                        = mods;
    e.payload.mouse_button.button = b;
    return e;
}

crd::platform::InputEvent move_event(crd::platform::InputEvent::Type type, float x, float y)
{
    crd::platform::InputEvent e;
    e.type = type;
    if (type == crd::platform::InputEvent::Type::Scroll)
    {
        e.payload.scroll.dx = x;
        e.payload.scroll.dy = y;
    }
    else
    {
        e.payload.mouse_move.x = x;
        e.payload.mouse_move.y = y;
    }
    return e;
}

crd::platform::InputEvent resize_event(crd::i32 w, crd::i32 h)
{
    crd::platform::InputEvent e;
    e.type                  = crd::platform::InputEvent::Type::Resize;
    e.payload.resize.width  = w;
    e.payload.resize.height = h;
    return e;
}

// The record of the panel's ended run.
crd::ceir::cook::ReplayRecord ended_record(InspectPanel& panel, crd::memory::IAllocator* alloc)
{
    crd::ceir::cook::ReplayRecord rec(alloc);
    REQUIRE(panel.host().record(rec) == crd::ceir::cook::HostRecord::Ok);
    return rec;
}

// The record replayed from its own program and reads alone, in a fresh Context: no divergence.
void replays_from_its_reads(const crd::ceir::cook::ReplayRecord& rec, crd::memory::IAllocator* alloc)
{
    crd::ceir::Context             ctx(alloc);
    crd::ceir::cook::ReplayProgram program(alloc);
    crd::ceir::cook::load_replay_program(ctx, {rec.program.data(), rec.program.size()}, "main",
                                         &register_replay_dialects, nullptr, program);
    REQUIRE(program.ok());
    crd::ceir::cook::ReplayTrace trace(alloc);
    crd::ceir::cook::InputFeed   feed({rec.input_reads.data(), rec.input_reads.size()}, trace);
    crd::ceir::cook::run_traced(program, {rec.args.data(), rec.args.size()}, rec.max_events, nullptr, trace,
                                feed.source());
    CHECK(crd::ceir::cook::first_divergence(rec, trace).kind == crd::ceir::cook::DivergenceKind::None);
}

constexpr u32 kHostStateInput = 5U; // the host-state input's slot in a record
} // namespace

TEST_CASE("diag 9a event: a window input event packs as input.event reads it, quantized and saturated",
          "[sandbox][inspect][diag][event]")
{
    using Type = crd::platform::InputEvent::Type;
    using crd::platform::Key;
    using crd::platform::KeyMods;
    using crd::platform::MouseButton;
    using crd::sandbox::window_event;
    const KeyMods none{};
    const KeyMods shift_alt{true, false, true, false};
    const KeyMods ctrl_super{false, true, false, true};
    const KeyMods all{true, true, true, true};

    CHECK(window_event(crd::platform::InputEvent{}) == 0); // a None event is an empty queue's answer
    CHECK(window_event(key_event(Type::KeyDown, Key::A, shift_alt)) ==
          packed(1U, static_cast<u64>(Key::A), 1U | 4U, 0, 0));
    CHECK(window_event(key_event(Type::KeyUp, Key::Escape, ctrl_super)) ==
          packed(2U, static_cast<u64>(Key::Escape), 2U | 8U, 0, 0));
    CHECK(window_event(key_event(Type::KeyRepeat, Key::RightAlt, all)) ==
          packed(3U, static_cast<u64>(Key::RightAlt), 15U, 0, 0));
    CHECK(window_event(button_event(Type::MouseDown, MouseButton::Right, none)) ==
          packed(4U, static_cast<u64>(MouseButton::Right), 0U, 0, 0));
    CHECK(window_event(button_event(Type::MouseUp, MouseButton::X2, ctrl_super)) ==
          packed(5U, static_cast<u64>(MouseButton::X2), 10U, 0, 0));
    // The pointer in whole pixels, rounded half away from zero; saturated at 16 signed bits; a NaN is 0.
    CHECK(window_event(move_event(Type::MouseMove, 12.4F, -7.5F)) == packed(6U, 0U, 0U, 12, -8));
    CHECK(window_event(move_event(Type::MouseMove, 40000.0F, -40000.0F)) == packed(6U, 0U, 0U, 32767, -32768));
    CHECK(window_event(move_event(Type::MouseMove, std::numeric_limits<float>::quiet_NaN(), 3.5F)) ==
          packed(6U, 0U, 0U, 0, 4));
    // The scroll offset in hundredths of a step.
    CHECK(window_event(move_event(Type::Scroll, 0.5F, -1.25F)) == packed(7U, 0U, 0U, 50, -125));
    CHECK(window_event(move_event(Type::Scroll, 1000.0F, -1000.0F)) == packed(7U, 0U, 0U, 32767, -32768));
    // The new size in pixels.
    CHECK(window_event(resize_event(1920, 1080)) == packed(8U, 0U, 0U, 1920, 1080));
    CHECK(window_event(resize_event(70000, -5)) == packed(8U, 0U, 0U, 32767, -5));
}

TEST_CASE("diag 9a event: the sandbox panel's runs take its listed events, or the window events staged before start",
          "[sandbox][inspect][diag][event]")
{
    using crd::ceir::cook::ReplayInputRead;
    using crd::ceir::input::InputKind;
    crd::memory::GrowableTlsfAllocator alloc;
    const String text = read_text(fs::Path(StringView(kEngineAssets)) / StringView("ceir/event_demo.ceir"), &alloc);
    const u32    second = line_of(sv(text), "%6, %7, %8, %9, %10 = input.event()");
    const u32    sw     = line_of(sv(text), "core.switch");
    REQUIRE(second != 0U);
    REQUIRE(sw != 0U);
    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));
    const i64 args[1] = {7};
    Frames    frames;

    // Listed events: every run takes them from the first. The panel copies them, so the caller's list may change.
    {
        InspectPanel panel(&alloc, renderer);
        REQUIRE(panel.load(StringView("ceir/event_demo"), StringView("main")).ok());
        i64                       list[2] = {packed(1U, 65U, 3U, 0, 0), packed(6U, 0U, 0U, -5, 9)};
        crd::sandbox::PanelInputs inputs;
        inputs.events = crd::ceir::cook::HostEventsSpec{true, ConstSpan<i64>(list, 2U)};
        panel.set_inputs(inputs);
        list[0] = packed(8U, 0U, 0U, 1, 1);
        panel.push_window_event(resize_event(640, 480)); // not a window-events panel: nothing is staged
        CHECK(panel.window_events_staged() == 0U);
        for (u32 run = 0U; run < 2U; ++run)
        {
            REQUIRE(panel.start(ConstSpan<i64>(args, 1U), crd::ceir::cook::HostRecording{true, 0U}) ==
                    insp::Refusal::None);
            REQUIRE(frames.until(panel, TickEvent::Ended));
            REQUIRE(panel.state() == PanelState::Finished);
            REQUIRE(panel.results().size() == 1U);
            CHECK(panel.results()[0] == 65 - 5 + 9 + 7);
            const crd::ceir::cook::ReplayRecord rec = ended_record(panel, &alloc);
            REQUIRE(rec.input_reads.size() == 2U);
            CHECK(rec.input_reads[0] == ReplayInputRead{InputKind::Event, 0U, true, packed(1U, 65U, 3U, 0, 0)});
            CHECK(rec.input_reads[1] == ReplayInputRead{InputKind::Event, 0U, true, packed(6U, 0U, 0U, -5, 9)});
            CHECK(rec.inputs[kHostStateInput].state == crd::ceir::cook::ReplayInputState::Recorded);
        }
    }

    // A start the host refuses (nothing loaded) takes none of the staged window events.
    {
        InspectPanel              unloaded(&alloc, renderer);
        crd::sandbox::PanelInputs window;
        window.window_events = true;
        unloaded.set_inputs(window);
        unloaded.push_window_event(resize_event(640, 480));
        unloaded.push_window_event(crd::platform::InputEvent{}); // a None event is never staged
        CHECK(unloaded.window_events_staged() == 1U);
        CHECK(unloaded.start(ConstSpan<i64>(args, 1U)) == insp::Refusal::NotBound);
        CHECK(unloaded.window_events_staged() == 1U);
        CHECK(unloaded.run_window_events() == 0U);
    }

    // Window events: the frame loop's layer stages them; a run takes those that arrived before its start, oldest
    // first, and events that arrive while it is held feed the next run only.
    InspectPanel panel(&alloc, renderer);
    REQUIRE(panel.load(StringView("ceir/event_demo"), StringView("main")).ok());
    REQUIRE(panel.add_breakpoint(sw) == insp::Refusal::None);
    panel.watch(second);
    crd::sandbox::PanelInputs inputs;
    inputs.window_events = true;
    panel.set_inputs(inputs);
    crd::sandbox::InspectEventLayer layer(panel);
    crd::app::KeyPressedEvent   key(crd::platform::Key::A, crd::platform::KeyMods{true, true, false, false}, false);
    crd::app::WindowResizeEvent resize(1280, 720);
    crd::app::WindowCloseEvent  close; // no input event: not staged
    layer.on_event(key);
    layer.on_event(close);
    layer.on_event(resize);
    CHECK_FALSE(key.handled); // every other consumer still sees them
    CHECK_FALSE(resize.handled);
    CHECK(panel.window_events_staged() == 2U);
    REQUIRE(panel.start(ConstSpan<i64>(args, 1U), crd::ceir::cook::HostRecording{true, 0U}) == insp::Refusal::None);
    CHECK(panel.run_window_events() == 2U);
    CHECK(panel.run_window_dropped() == 0U);
    CHECK(panel.window_events_staged() == 0U);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(panel.stop_line() == sw);
    REQUIRE(value_at(panel, second).value.status == insp::ValueStatus::Available);
    CHECK(value_at(panel, second).value.bits == 8); // the second event's type: a resize, which the switch lacks

    // While held: two more window events are staged, and a start is Busy and takes none of them.
    layer.on_event(key);
    crd::app::MouseMovedEvent moved(-5.0F, 9.0F);
    layer.on_event(moved);
    CHECK(panel.window_events_staged() == 2U);
    CHECK(panel.start(ConstSpan<i64>(args, 1U)) == insp::Refusal::Busy);
    CHECK(panel.window_events_staged() == 2U);
    REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    CHECK(panel.error() == crd::ceir::plan::RunError::SelectorOutOfRange);
    {
        const crd::ceir::cook::ReplayRecord rec = ended_record(panel, &alloc);
        REQUIRE(rec.input_reads.size() == 2U);
        CHECK(rec.input_reads[0] == ReplayInputRead{InputKind::Event, 0U, true,
                                                    packed(1U, static_cast<u64>(crd::platform::Key::A), 3U, 0, 0)});
        CHECK(rec.input_reads[1] == ReplayInputRead{InputKind::Event, 0U, true, packed(8U, 0U, 0U, 1280, 720)});
        CHECK(rec.inputs[kHostStateInput].state == crd::ceir::cook::ReplayInputState::Recorded);
        replays_from_its_reads(rec, &alloc);
    }

    // "Run again" takes the two events staged while the first run was held: it passes its switch and finishes.
    REQUIRE(panel.start(ConstSpan<i64>(args, 1U)) == insp::Refusal::None);
    CHECK(panel.run_window_events() == 2U);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    REQUIRE(value_at(panel, second).value.status == insp::ValueStatus::Available);
    CHECK(value_at(panel, second).value.bits == 6); // a mouse move
    REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    REQUIRE(panel.state() == PanelState::Finished);
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == static_cast<i64>(crd::platform::Key::A) - 5 + 9 + 7);

    // Nothing staged: the run's queue is open and empty, so both reads are none events.
    REQUIRE(panel.start(ConstSpan<i64>(args, 1U)) == insp::Refusal::None);
    CHECK(panel.run_window_events() == 0U);
    REQUIRE(frames.until(panel, TickEvent::Stopped));
    CHECK(value_at(panel, second).value.bits == 0);
    REQUIRE(panel.command(panel.generation(), PanelAction::Continue) == insp::Refusal::None);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    REQUIRE(panel.results().size() == 1U);
    CHECK(panel.results()[0] == 7);
    CHECK(frames.worst_ms < kFrameBoundMs);
}

TEST_CASE("diag 9a event: the sandbox panel's layer stages every window input event, keeping the newest past its bound",
          "[sandbox][inspect][diag][event]")
{
    using crd::ceir::cook::ReplayInputRead;
    using crd::ceir::input::InputKind;
    constexpr u32 extra = 3U;
    constexpr u32 total = crd::sandbox::kMaxWindowEvents + extra;
    static_assert(crd::sandbox::kMaxWindowEvents == 4096U);

    crd::memory::GrowableTlsfAllocator alloc;
    crd::scenerender::SceneRenderer    renderer(&alloc);
    REQUIRE(renderer.set_asset_root(kEngineAssets));
    // An application program that takes `n` events and returns 0.
    const AppRoot    app("crd-diag9a-sandbox-window-events");
    const StringView reads = R"(module {
  ^bb0:
    func.func() {sym_name = "main"} {
      ^bb0(%0 : !i64):
        %1 = arith.const() {value = 0} : !i64
        %2 = arith.const() {value = 1} : !i64
        core.for(%1, %0, %2) {
          ^bb0(%3 : !i64):
            %4, %5, %6, %7, %8 = input.event() {queue = 0} : !i64
            core.yield()
        }
        func.return(%1)
    }
}
)";
    REQUIRE(fs::write_file_text(app.dir / StringView("ceir/event_reads.ceir"), reads));
    REQUIRE(renderer.set_app_asset_root(String(app.dir.generic(), &alloc).c_str()));

    InspectPanel panel(&alloc, renderer);
    REQUIRE(panel.load(StringView("ceir/event_reads"), StringView("main")).ok());
    crd::sandbox::PanelInputs inputs;
    inputs.window_events = true;
    panel.set_inputs(inputs);
    Frames frames;

    // Every input event the application dispatches reaches the run through the layer, in order, rebuilt as the
    // platform event it came from; an event of another kind does not.
    {
        using crd::platform::Key;
        using crd::platform::MouseButton;
        const crd::platform::KeyMods       ctrl{false, true, false, false};
        crd::sandbox::InspectEventLayer    layer(panel);
        crd::app::KeyPressedEvent          down(Key::B, ctrl, false);
        crd::app::KeyPressedEvent          repeat(Key::B, ctrl, true);
        crd::app::KeyReleasedEvent         up(Key::B, ctrl);
        crd::app::MouseButtonPressedEvent  press(MouseButton::Middle, ctrl);
        crd::app::MouseButtonReleasedEvent release(MouseButton::Middle, crd::platform::KeyMods{});
        crd::app::MouseMovedEvent          moved(100.5F, -3.25F);
        crd::app::MouseScrolledEvent       scrolled(0.0F, -2.0F);
        crd::app::WindowResizeEvent        resized(800, 600);
        crd::app::WindowCloseEvent         close;
        layer.on_event(down);
        layer.on_event(repeat);
        layer.on_event(up);
        layer.on_event(close);
        layer.on_event(press);
        layer.on_event(release);
        layer.on_event(moved);
        layer.on_event(scrolled);
        layer.on_event(resized);
        const i64 nine[1] = {9};
        REQUIRE(panel.start(ConstSpan<i64>(nine, 1U), crd::ceir::cook::HostRecording{true, 0U}) == insp::Refusal::None);
        CHECK(panel.run_window_events() == 8U);
        REQUIRE(frames.until(panel, TickEvent::Ended));
        const crd::ceir::cook::ReplayRecord rec = ended_record(panel, &alloc);
        const u64 b           = static_cast<u64>(Key::B);
        const u64 mid         = static_cast<u64>(MouseButton::Middle);
        const i64 expected[9] = {packed(1U, b, 2U, 0, 0),         packed(3U, b, 2U, 0, 0),
                                 packed(2U, b, 2U, 0, 0),         packed(4U, mid, 2U, 0, 0),
                                 packed(5U, mid, 0U, 0, 0),       packed(6U, 0U, 0U, 101, -3),
                                 packed(7U, 0U, 0U, 0, -200),     packed(8U, 0U, 0U, 800, 600),
                                 0};
        REQUIRE(rec.input_reads.size() == 9U);
        for (u32 k = 0U; k < 9U; ++k)
        {
            INFO("read " << k);
            CHECK(rec.input_reads[k] == ReplayInputRead{InputKind::Event, 0U, true, expected[k]});
        }
    }

    for (u32 i = 0U; i < total; ++i)
    {
        const u32 x = i % 1000U;
        const u32 y = i / 1000U; // whole: the event's y is the thousands of its index
        panel.push_window_event(
            move_event(crd::platform::InputEvent::Type::MouseMove, static_cast<float>(x), static_cast<float>(y)));
    }
    CHECK(panel.window_events_staged() == crd::sandbox::kMaxWindowEvents);
    CHECK(panel.window_events_dropped() == extra);

    // The run reads every staged event, oldest first (the first `extra` were dropped), then a none event.
    const i64 args[1] = {static_cast<i64>(crd::sandbox::kMaxWindowEvents) + 1};
    REQUIRE(panel.start(ConstSpan<i64>(args, 1U), crd::ceir::cook::HostRecording{true, 0U}) == insp::Refusal::None);
    CHECK(panel.run_window_events() == crd::sandbox::kMaxWindowEvents);
    CHECK(panel.run_window_dropped() == extra);
    CHECK(panel.window_events_staged() == 0U);
    CHECK(panel.window_events_dropped() == 0U);
    REQUIRE(frames.until(panel, TickEvent::Ended));
    REQUIRE(panel.state() == PanelState::Finished);
    const crd::ceir::cook::ReplayRecord rec = ended_record(panel, &alloc);
    REQUIRE(rec.input_reads.size() == crd::sandbox::kMaxWindowEvents + 1U);
    u32 wrong = 0U;
    for (u32 k = 0U; k < crd::sandbox::kMaxWindowEvents; ++k)
    {
        const u32             i = k + extra;
        const ReplayInputRead staged{InputKind::Event, 0U, true, packed(6U, 0U, 0U, i % 1000U, i / 1000U)};
        if (!(rec.input_reads[k] == staged))
        {
            ++wrong;
        }
    }
    CHECK(wrong == 0U);
    CHECK(rec.input_reads[crd::sandbox::kMaxWindowEvents] == ReplayInputRead{InputKind::Event, 0U, true, 0});
    CHECK(rec.inputs[kHostStateInput].state == crd::ceir::cook::ReplayInputState::Recorded);
}
