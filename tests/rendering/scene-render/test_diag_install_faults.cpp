// DIAG.9b -- fault injection at the renderer's install stage. `SceneRenderer::reload` re-reads a hot-reloadable
// frame's source through the asset resolver (crd-platform's `read_file_text`, on the I/O fault seam), re-cooks it and
// installs the new graph at a frame boundary, bumping the asset's generation. An install interrupted by a failed or
// short read of that source must install nothing: the live frame and its generation stay as they were, the flight
// capture names the file, the recipe's one-hit plan interrupts the same install on its own, and the next clean
// reload installs the new graph. No device is needed: the frame kind installs without one.
//
// A shipping build cannot arm a plan; there the case asserts the refusal and that the install runs plain.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/core/fault_seam.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scenerender/scene_renderer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>

namespace
{
namespace fs   = crd::platform::fs;
namespace cp   = crd::perf;
namespace cont = crd::containers;

constexpr cp::DiagAuthoritySet kInject = cp::authority_bit(cp::DiagAuthority::Inject);
constexpr const char*          kFrameId = "app://frame/install_gate";

// A minimal valid frame graph (one mesh-amplify pass into @output), and the same shape with another clear colour.
constexpr const char* kFrameA = "schema = 1\n"
                                "name   = \"engine://frame/install_gate\"\n"
                                "\n"
                                "[[pass]]\n"
                                "name        = \"amplify\"\n"
                                "kind        = \"raster.mesh\"\n"
                                "shader      = \"engine://scene/mesh\"\n"
                                "writes      = [\"@output\"]\n"
                                "clear_color = [0.0, 0.0, 0.0, 1.0]\n"
                                "params      = { groups = 2 }\n";

constexpr const char* kFrameB = "schema = 1\n"
                                "name   = \"engine://frame/install_gate\"\n"
                                "\n"
                                "[[pass]]\n"
                                "name        = \"amplify\"\n"
                                "kind        = \"raster.mesh\"\n"
                                "shader      = \"engine://scene/mesh\"\n"
                                "writes      = [\"@output\"]\n"
                                "clear_color = [1.0, 0.0, 0.0, 1.0]\n"
                                "params      = { groups = 2 }\n";

cp::FaultPlan plan_of(const char* text)
{
    cp::FaultPlan plan;
    cont::String  reason;
    const bool    parsed = cp::parse_fault_plan(cont::StringView{text}, plan, reason);
    INFO(reason.c_str());
    REQUIRE(parsed);
    return plan;
}

// A private asset root under the temp directory, removed when the case ends however it ends.
struct AssetRoot
{
    fs::Path root;

    explicit AssetRoot(crd::memory::IAllocator* alloc)
    {
        cont::String leaf("crd_diag9b_install_", alloc);
        leaf.append(crd::resources::ResourceId::mint_random().to_string(alloc));
        root = fs::temp_directory() / cont::StringView{leaf.data(), leaf.size()};
        REQUIRE(fs::create_directories(root / "frame"));
    }
    ~AssetRoot() { (void)fs::remove_all(root); }
    AssetRoot(const AssetRoot&)            = delete;
    AssetRoot& operator=(const AssetRoot&) = delete;
    AssetRoot(AssetRoot&&)                 = delete;
    AssetRoot& operator=(AssetRoot&&)      = delete;

    void write_frame(const char* text) const
    {
        REQUIRE(fs::write_file_text(root / "frame" / "install_gate.frame.toml",
                                    cont::StringView{text, std::strlen(text)}));
    }
};

// Installs the injector on the I/O fault seam for one scope.
struct ScopedIoHook
{
    bool installed = false;

    explicit ScopedIoHook(cp::FaultInjector& injector)
        : installed(crd::install_io_fault_hook(&cp::FaultInjector::io_hook, &injector))
    {
    }
    ~ScopedIoHook()
    {
        if (installed)
        {
            crd::uninstall_io_fault_hook();
        }
    }
    ScopedIoHook(const ScopedIoHook&)            = delete;
    ScopedIoHook& operator=(const ScopedIoHook&) = delete;
    ScopedIoHook(ScopedIoHook&&)                 = delete;
    ScopedIoHook& operator=(ScopedIoHook&&)      = delete;
};

// The message of the one fault a capture holds (the event recorded just before its InjectedFault trigger), and the
// one-hit plan its recipe names.
struct FaultEvidence
{
    cont::String message;
    cont::String minimal;
    explicit FaultEvidence(crd::memory::IAllocator* a) : message(a), minimal(a) {}
};

FaultEvidence take_fault_evidence(cp::FlightRecorder& recorder, crd::memory::IAllocator* alloc)
{
    recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"after the install"}); // closes the post window
    cp::FlightCapture capture;
    REQUIRE(recorder.take_capture(capture));
    CHECK(capture.trigger.kind == cp::FlightTrigger::InjectedFault);
    FaultEvidence          out(alloc);
    const cp::FlightEvent* fault = nullptr;
    crd::u32               after = 0U;
    for (const cp::FlightEvent& e : capture.events)
    {
        if (e.code != cp::kFlightCodeFaultInjected)
        {
            continue;
        }
        if (e.sequence < capture.trigger.sequence)
        {
            fault = &e;
        }
        else
        {
            ++after;
        }
    }
    REQUIRE(fault != nullptr);
    CHECK(fault->sequence + 1U == capture.trigger.sequence);
    CHECK(after == 0U);
    out.message.append(fault->message_view());
    const cont::StringView recipe{capture.replay.data(), capture.replay.size()};
    const crd::usize       at = recipe.find("minimal=");
    REQUIRE(at != cont::StringView::npos);
    const cont::StringView rest = recipe.substr(at + 8U);
    const crd::usize       end  = rest.find(' ');
    out.minimal.append(rest.substr(0U, end == cont::StringView::npos ? rest.size() : end));
    return out;
}
} // namespace

TEST_CASE("fault injection: a frame install interrupted by a failed or short read keeps the live generation",
          "[scene-render][diag9b][fault][install]")
{
    crd::memory::TlsfAllocator alloc(1U << 24U, nullptr, "diag9b-install");
    const AssetRoot            assets(&alloc);
    assets.write_frame(kFrameA);

    crd::scenerender::SceneRenderer renderer(&alloc);
    REQUIRE(renderer.set_app_asset_root(assets.root.generic().data()));
    REQUIRE(renderer.set_frame_graph(kFrameId)); // installs A and registers the frame as hot-reloadable
    REQUIRE(renderer.asset_generation(kFrameId) == 0U);

    assets.write_frame(kFrameB); // the next install has a new graph to install

    cp::FaultInjector injector;
    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan_of("seed=51;failed-read@0+1%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
        const ScopedIoHook hook(injector);
        CHECK_FALSE(hook.installed);
        CHECK(renderer.reload(kFrameId));
        CHECK(renderer.asset_generation(kFrameId) == 1U);
        return;
    }

    cp::FlightRecorder recorder;
    cp::FlightPolicy   policy;
    policy.capacity    = 64U;
    policy.pre_events  = 32U;
    policy.post_events = 1U;
    REQUIRE(recorder.init(policy) == cp::FlightPolicyStatus::Committed);
    injector.attach_flight_recorder(&recorder);

    // The install reads the frame's source once, whole. It fails outright, is cut inside the first line, or is cut
    // one byte short of the end (where the text left would still be most of a frame): none may install.
    char last_byte_plan[96];
    (void)std::snprintf(last_byte_plan, sizeof(last_byte_plan), "seed=53;partial-read@0+1%%1000000=%llu",
                        static_cast<unsigned long long>(std::strlen(kFrameB) - 1U));
    const char* const plans[] = {
        "seed=51;failed-read@0+1%1000000",
        "seed=52;partial-read@0+1%1000000=4",
        last_byte_plan,
    };
    const auto install_faulted = [&](const char* plan)
    {
        REQUIRE(injector.arm(plan_of(plan), kInject) == cp::FaultArmStatus::Armed);
        bool installed = true;
        {
            const ScopedIoHook hook(injector);
            REQUIRE(hook.installed);
            installed = renderer.reload(kFrameId);
        }
        CHECK_FALSE(installed);
        CHECK(renderer.asset_generation(kFrameId) == 0U);
        CHECK(injector.fired(cp::FaultSite::FailedRead) + injector.fired(cp::FaultSite::PartialRead) == 1U);
        FaultEvidence evidence = take_fault_evidence(recorder, &alloc);
        CHECK(cont::StringView{evidence.message.data(), evidence.message.size()}.ends_with(
            cont::StringView{" on install_gate.frame.toml"}));
        return evidence;
    };
    for (const char* plan : plans)
    {
        INFO(plan);
        const FaultEvidence evidence = install_faulted(plan);
        // The recipe's one-hit plan interrupts the same install on its own.
        INFO(evidence.minimal.c_str());
        (void)install_faulted(evidence.minimal.c_str());
    }
    injector.disarm();

    // The live frame is still A: with A back on disk a clean reload is a no-op (its content hash equals the installed
    // one), so the generation does not move. Then B installs as generation 1.
    assets.write_frame(kFrameA);
    CHECK(renderer.reload(kFrameId));
    CHECK(renderer.asset_generation(kFrameId) == 0U);
    assets.write_frame(kFrameB);
    CHECK(renderer.reload(kFrameId));
    CHECK(renderer.asset_generation(kFrameId) == 1U);
    recorder.shutdown();
}
