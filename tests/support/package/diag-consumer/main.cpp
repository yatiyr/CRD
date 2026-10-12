// A thin external application of an installed Cerid package (DIAG.11a). It is built only against the moved install
// prefix (find_package(Cerid), Cerid::perf), never inside the engine's build, and touches no engine source: it composes
// the engine's DiagnosticHost from an authored diagnostic-host profile, does a little work of its own, fails on
// purpose, and is investigated with the package's own route.
//
//   diag-consumer fail <profile.host.toml>              start the host from the profile, record forty orders into the
//                                                       flight recorder, then fail a fatal invariant; the profile's
//                                                       capture directory receives the flight bundle before the
//                                                       process dies (exit 0 means the fatal path returned: a defect)
//   diag-consumer inspect <directory> <bundle name>     read that bundle through bundle.inspect (the bounded,
//                                                       non-executing importer) and print every page's JSON
//
// scripts/test-package-consumer.py builds it against the moved prefix of every profile it packages and judges the
// output: the host's modes, the bundle, the trigger at this file's failing line, the events before it, this build and
// this executable's own module identity.
#include <crd/core/assert.hpp>
#include <crd/core/build_config.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/diagnostics.hpp>

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>
#endif

namespace
{

namespace cp         = crd::perf;
namespace containers = crd::containers; // no `cont` alias: CRD_DIAG_INVARIANT names its own types fully

// The application's own event codes, above the range the engine's foundation owns.
constexpr cp::EventCode kOrderRecorded = 0x00A10001U;
constexpr cp::EventCode kTotalNegative = 0x00A10002U;

// A failing check must end the process with its evidence, never wait on a dialog nobody will click.
void run_headless()
{
    crd::set_assert_headless(true);
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
}

void print_build()
{
    const cp::DiagnosticHostCapabilities caps = cp::diagnostic_host_capabilities();
    std::printf("CONSUMER_BUILD asserts=%d profiling=%d fault_seams=%d crash_backend=%d\n", CRD_ENABLE_ASSERTS,
                CRD_ENABLE_PROFILING, crd::fault_seams_compiled() ? 1 : 0, caps.crash_backend ? 1 : 0);
}

int fail(const char* profile_path)
{
    cp::DiagnosticHostProfile profile;
    containers::String        reason;
    if (!cp::load_diagnostic_host_profile(profile_path, profile, reason))
    {
        std::printf("CONSUMER_PROFILE_REFUSED %s\n", reason.c_str());
        return 3;
    }
    // Static: the fatal path runs while this frame is still live, and the host must outlive it.
    static cp::DiagnosticHost host;
    const cp::DiagHostStart   start = host.start(profile, reason);
    if (start != cp::DiagHostStart::Started)
    {
        std::printf("CONSUMER_HOST_REFUSED %s %s\n", cp::diag_host_start_name(start).data(), reason.c_str());
        return 4;
    }
    const cp::DiagnosticHostModes& modes = host.modes();
    std::printf("CONSUMER_HOST_STARTED crash=%s flight=%s faults=%s\n", cp::diag_mode_state_name(modes.crash).data(),
                cp::diag_mode_state_name(modes.flight).data(), cp::diag_mode_state_name(modes.faults).data());
    (void)std::fflush(stdout);
    (void)cp::register_code_range("diag-consumer", 0x00A10000U, 0x00A1FFFFU);
    host.recorder().set_replay_recipe("diag-consumer fail <profile.host.toml>");

    // What this binary can do, as the host's own service reports it: the profiler's modes (compiled only into a
    // profiling build), the live flight recorder and fault injection (compiled out of a shipping build).
    cp::DiagServiceConfig config;
    host.configure_service(config);
    cp::DiagCommandService service{host.profile().command_grant, config};
    if (!host.serve(service))
    {
        std::printf("CONSUMER_SERVE_FAILED\n");
        return 6;
    }
    cp::DiagRequest capabilities;
    capabilities.command    = "diag.capabilities";
    capabilities.page_items = cp::kDiagMaxPageItems;
    capabilities.page_bytes = cp::kDiagMaxPageBytes;
    const cp::DiagResult caps = service.execute(capabilities);
    std::printf("CONSUMER_CAPABILITIES %s\n", caps.json.c_str());
    (void)std::fflush(stdout);

    // The application's work: forty orders, recorded as they are booked (a refund as a warning, value_b = 1).
    crd::i64 total = 0;
    char     text[48];
    for (crd::u32 i = 0U; i < 40U; ++i)
    {
        const crd::i64 amount = (i % 7U == 6U) ? -900 : 25;
        total += amount;
        (void)std::snprintf(text, sizeof(text), "order %u booked %lld", static_cast<unsigned>(i),
                            static_cast<long long>(amount));
        host.recorder().record(kOrderRecorded, amount < 0 ? cp::Severity::Warning : cp::Severity::Info,
                               containers::StringView{text}, i, amount < 0 ? 1U : 0U);
    }

    // The deliberate failure: refunds outran the bookings.
    CRD_DIAG_INVARIANT(total >= 0, kTotalNegative, "the order total went negative");
    std::printf("CONSUMER_RETURNED_FROM_FATAL\n");
    return 0;
}

int inspect(const char* directory, const char* bundle)
{
    cp::DiagServiceConfig config;
    config.root = containers::StringView{directory};
    cp::DiagCommandService service{cp::authority_bit(cp::DiagAuthority::Read), config};
    cp::DiagRequest        request;
    request.command    = "bundle.inspect";
    request.path       = containers::StringView{bundle};
    request.page_items = cp::kDiagMaxPageItems;
    request.page_bytes = cp::kDiagMaxPageBytes;
    for (crd::u32 page = 0U; page < 1024U; ++page)
    {
        const cp::DiagResult result = service.execute(request);
        std::printf("CONSUMER_INSPECT_PAGE %s\n", result.json.c_str());
        if (result.status != cp::DiagStatus::Ok)
        {
            std::printf("CONSUMER_INSPECT_FAILED %s %s\n", cp::status_name(result.status).data(),
                        result.reason.c_str());
            return 5;
        }
        if (result.complete || result.next_cursor == 0U)
        {
            std::printf("CONSUMER_INSPECT_DONE pages=%u items=%u\n", static_cast<unsigned>(page + 1U),
                        static_cast<unsigned>(result.total));
            return 0;
        }
        request.cursor = result.next_cursor;
    }
    std::printf("CONSUMER_INSPECT_FAILED too-many-pages\n");
    return 5;
}

} // namespace

int main(int argc, char** argv)
{
    run_headless();
    print_build();
    if (argc == 3 && std::strcmp(argv[1], "fail") == 0)
    {
        return fail(argv[2]);
    }
    if (argc == 4 && std::strcmp(argv[1], "inspect") == 0)
    {
        return inspect(argv[2], argv[3]);
    }
    std::printf("usage: diag-consumer fail <profile.host.toml> | inspect <directory> <bundle name>\n");
    return 2;
}
