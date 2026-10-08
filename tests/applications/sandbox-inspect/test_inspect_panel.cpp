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
// fails. Expected lines are scanned from the text, never taken from the parser or the panel.
// ASCII test names.

#include "inspect_panel.hpp"

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
