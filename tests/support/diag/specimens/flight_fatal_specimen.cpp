// The DIAG.9b fatal-path flight-recorder specimen. It records 50 events, opens nothing else, then enters a real fatal
// path with the flight triggers installed: mode "invariant" fails a CRD_DIAG_INVARIANT (check_invariant's default
// fatal path), mode "assert" reports a failed assertion through crd's own assert path while headless (it aborts).
// The flight trigger closes the window as Fatal and its sink writes a bundle to argv[2] before the process dies, with
// the build identity and the module snapshot taken at startup. The parent (crd-perf-tests) reads that bundle: the
// trigger, the events before it, the truncated post window, the replay recipe, the build and this executable's own
// identity must all be there, and the process must not have exited cleanly.
#include "specimen_common.hpp"

#include <crd/perf/bundle.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/perf/flight_triggers.hpp>
#include <crd/perf/symbol_index.hpp>

#include <cstdio>
#include <cstring>

namespace
{
namespace cp = crd::perf;

const char* g_out_path = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once in main

// The module snapshot, taken at startup: the fatal path writes it and never enumerates (that takes the loader lock).
void write_capture(const cp::FlightCapture& capture, void* user)
{
    const auto* modules = static_cast<const cp::LiveModules*>(user);
    const crd::containers::Array<crd::u8> bundle = cp::write_flight_bundle(
        capture, crd::containers::ConstSpan<cp::ModuleIdentity>{modules->modules.data(), modules->modules.size()});
    const bool written =
        cp::write_bundle_atomic(g_out_path, crd::containers::ConstSpan<crd::u8>{bundle.data(), bundle.size()});
    std::printf("CRD_FLIGHT_WRITTEN=%d\n", written ? 1 : 0);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 3)
    {
        std::printf("usage: flight_fatal_specimen invariant|assert <bundle path>\n");
        return 2;
    }
    g_out_path = argv[2];

    static cp::FlightRecorder recorder;
    cp::FlightPolicy          policy;
    policy.capacity    = 64U;
    policy.pre_events  = 32U;
    policy.post_events = 16U;
    if (recorder.init(policy) != cp::FlightPolicyStatus::Committed)
    {
        return 3;
    }
    static cp::LiveModules modules = cp::enumerate_loaded_modules();
    recorder.set_sink(&write_capture, &modules);
    recorder.set_replay_recipe("crd-diag-flight-fatal-specimen invariant|assert <bundle path>");
    if (!cp::install_flight_assert_trigger(&recorder) || !cp::install_flight_fatal_trigger(&recorder))
    {
        return 4;
    }

    char text[32];
    for (crd::u32 i = 0U; i < 50U; ++i)
    {
        (void)std::snprintf(text, sizeof(text), "event %u", static_cast<unsigned>(i));
        recorder.record(0x0042U, cp::Severity::Info, crd::containers::StringView{text}, i, 0U);
    }

    if (std::strcmp(argv[1], "invariant") == 0)
    {
        CRD_DIAG_INVARIANT(false, 0x0777U, "flight specimen invariant");
    }
    else if (std::strcmp(argv[1], "assert") == 0)
    {
        (void)crd::detail::report_assert_failure("flight_specimen_condition", __FILE__, __LINE__,
                                                 "flight specimen assertion");
    }
    // Reaching here means the fatal path returned: the parent reads exit 0 as a failure of the specimen.
    return 0;
}
