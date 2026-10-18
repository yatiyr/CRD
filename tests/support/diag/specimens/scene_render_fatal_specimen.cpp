// Scene renderer contract fatals (crd-scene-render-fatal-* CTests, tests/rendering/scene-render). A frame-graph reload
// stages its re-cooked desc in a block of the renderer's allocator; one that refuses it (a sub-budget allocator
// returns nullptr) leaves the renderer out of memory, fatal in every build, never a null desc cooked into (DIAG.3a; the
// desc was cooked into the refused pointer before). One mode per run (argv[1]); see expected_fatal.hpp for the exit
// protocol:
//   frame-reload-staging-refused  SceneRenderer::reload of a registered frame graph: the staged desc is refused
// The reload needs no device (the RAF-11 live gate's path): the frame source is written next to the specimen (its
// build tree, so lanes never share it, and the next run replaces what a fatal left), registered with set_frame_graph,
// and reloaded with the allocator refusing exactly a desc's request shape (refusing_allocator.hpp), so the staged desc
// is the run's first refused request.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/framecook/frame_asset.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/scenerender/scene_renderer.hpp>

#include <cstring>

namespace
{
namespace fs = crd::platform::fs;

// A minimal valid frame graph (the RAF-11 live gate's): one mesh-amplify pass into @output.
constexpr const char* kFrame = "schema = 1\n"
                               "name   = \"engine://frame/reload_fatal\"\n"
                               "\n"
                               "[[pass]]\n"
                               "name        = \"amplify\"\n"
                               "kind        = \"raster.mesh\"\n"
                               "shader      = \"engine://scene/mesh\"\n"
                               "writes      = [\"@output\"]\n"
                               "clear_color = [0.0, 0.0, 0.0, 1.0]\n"
                               "params      = { groups = 2 }\n";

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    if (std::strcmp(mode, "frame-reload-staging-refused") != 0)
    {
        return false;
    }
    crd_diag_fatal::arm(mode, crd::scenerender::detail::kFrameReloadOutOfMemory);
    const fs::Path root = fs::executable_dir() / crd::containers::StringView{"scene-render-fatal"};
    (void)fs::remove_all(root);
    if (!fs::create_directories(root / crd::containers::StringView{"frame"}) ||
        !fs::write_file_text(root / crd::containers::StringView{"frame/reload_fatal.frame.toml"},
                             crd::containers::StringView{kFrame}))
    {
        CRD_FATAL("scene-render-fatal: the frame source could not be written");
    }
    crd_diag_fatal::RefusingAllocator alloc("scene-render-fatal-refusing");
    {
        crd::scenerender::SceneRenderer renderer(&alloc);
        const crd::containers::String dir = root.native();
        if (!renderer.set_app_asset_root(dir.c_str()) || !renderer.set_frame_graph("app://frame/reload_fatal"))
        {
            (void)fs::remove_all(root);
            CRD_FATAL("scene-render-fatal: the frame graph did not register");
        }
        // The fatal ends the process here; the source stays next to the specimen until the next run.
        alloc.refuse_shape(sizeof(crd::framecook::FrameGraphDesc), alignof(crd::framecook::FrameGraphDesc));
        (void)renderer.reload("app://frame/reload_fatal");
    }
    (void)fs::remove_all(root);
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
