// The DIAG.11a diagnostic-host specimen: a small application that composes the engine's DiagnosticHost from an
// authored profile (argv[1], a diagnostic-host asset) before anything else, records its own work into the flight
// recorder, then fails on purpose. Mode "invariant" fails a CRD_DIAG_INVARIANT (a fatal invariant), mode "assert"
// reports a failed assertion through crd's own assert path while headless (it aborts). Nothing in it wires a crash
// handler, a recorder trigger or a bundle writer by hand: the profile's capture directory gets the flight bundle
// before the process dies. The parent (crd-perf-tests) reads that bundle: the trigger, the events before it, the
// replay recipe, the build and this executable's own identity.
#include "specimen_common.hpp"

#include <crd/containers/string.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_triggers.hpp>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 3)
    {
        std::printf("usage: diag_host_specimen <profile> invariant|assert\n");
        return 2;
    }
    namespace cp   = crd::perf;
    namespace cont = crd::containers; // CRD_DIAG_INVARIANT names cont::StringView

    cp::DiagnosticHostProfile profile;
    cont::String              reason;
    if (!cp::load_diagnostic_host_profile(argv[1], profile, reason))
    {
        std::printf("CRD_HOST_PROFILE_REFUSED %s\n", reason.c_str());
        return 3;
    }
    static cp::DiagnosticHost host; // static: the fatal path runs while main's frame is still live
    const cp::DiagHostStart   start = host.start(profile, reason);
    if (start != cp::DiagHostStart::Started)
    {
        std::printf("CRD_HOST_REFUSED %s %s\n", cp::diag_host_start_name(start).data(), reason.c_str());
        return 4;
    }
    std::printf("CRD_HOST_STARTED crash=%s flight=%s faults=%s\n", cp::diag_mode_state_name(host.modes().crash).data(),
                cp::diag_mode_state_name(host.modes().flight).data(),
                cp::diag_mode_state_name(host.modes().faults).data());
    std::fflush(stdout);
    host.recorder().set_replay_recipe("crd-diag-host-specimen <profile> invariant|assert");

    char text[32];
    for (crd::u32 i = 0U; i < 40U; ++i)
    {
        (void)std::snprintf(text, sizeof(text), "order %u", static_cast<unsigned>(i));
        host.recorder().record(0x0042U, cp::Severity::Info, cont::StringView{text}, i, 0U);
    }

    if (std::strcmp(argv[2], "invariant") == 0)
    {
        CRD_DIAG_INVARIANT(false, 0x0711U, "the order total went negative");
    }
    else if (std::strcmp(argv[2], "assert") == 0)
    {
        (void)crd::detail::report_assert_failure("total >= 0", __FILE__, __LINE__, "the order total went negative");
    }
    // Reaching here means the fatal path returned: the parent reads exit 0 as a failure of the specimen.
    return 0;
}
