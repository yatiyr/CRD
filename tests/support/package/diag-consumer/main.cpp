// A thin external application of an installed Cerid package (DIAG.11a). It is built only against the moved install
// prefix (find_package(Cerid)), never inside the engine's build, and touches no engine source: it composes the
// engine's DiagnosticHost from an authored diagnostic-host profile, does a little work of its own, fails on purpose,
// and is investigated with the package's own route. Its examples show the rest of what an application does with the
// diagnostic family.
//
//   diag-consumer fail <profile.host.toml>              start the host from the profile, record forty orders into the
//                                                       flight recorder, then fail a fatal invariant; the profile's
//                                                       capture directory receives the flight bundle before the
//                                                       process dies (exit 0 means the fatal path returned: a defect)
//   diag-consumer inspect <directory> <bundle name>     read that bundle through bundle.inspect (the bounded,
//                                                       non-executing importer) and print every page's JSON
//   diag-consumer allocator <profile> <tlsf|diagnostic> allocator selection: the owner's allocator, watched by the
//                                                       host, exhausted on purpose (example_allocator.cpp)
//   diag-consumer jobs <profile>                        the jobs pool configured by the host, and a stalled
//                                                       main-thread job reported as a hang (example_jobs.cpp)
//   diag-consumer reload <profile> <limits.toml> <dir>  live changes to the diagnostic configuration, and the
//                                                       application's own data reloaded through the engine's I/O
//                                                       fault seam (example_reload.cpp)
//   diag-consumer capture <profile>                     captures requested through the host's service while the
//                                                       application keeps running (example_capture.cpp)
//
// scripts/test-package-consumer.py builds it against the moved prefix of every profile it packages and judges the
// output: the host's modes, the bundles, the trigger at this file's failing line, the events before it, this build,
// this executable's own module identity, and what each example must show.
#include "consumer_support.hpp"
#include "examples.hpp"

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/diagnostics.hpp>

#include <cstdio>
#include <cstring>

namespace diag_consumer
{

namespace containers = crd::containers; // no `cont` alias: CRD_DIAG_INVARIANT names its own types fully

namespace
{

int fail(const char* profile_path)
{
    // Static: the fatal path runs while this frame is still live, and the host must outlive it.
    static cp::DiagnosticHost host;
    if (!start_host(host, profile_path, "diag-consumer fail <profile.host.toml>"))
    {
        return 4;
    }

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
} // namespace diag_consumer

int main(int argc, char** argv)
{
    namespace dc = diag_consumer;
    dc::run_headless();
    dc::print_build();
    const char* const route = argc >= 2 ? argv[1] : "";
    if (argc == 3 && std::strcmp(route, "fail") == 0)
    {
        return dc::fail(argv[2]);
    }
    if (argc == 4 && std::strcmp(route, "inspect") == 0)
    {
        return dc::inspect(argv[2], argv[3]);
    }
    if (argc == 4 && std::strcmp(route, "allocator") == 0)
    {
        return dc::example_allocator(argv[2], argv[3]);
    }
    if (argc == 3 && std::strcmp(route, "jobs") == 0)
    {
        return dc::example_jobs(argv[2]);
    }
    if (argc == 5 && std::strcmp(route, "reload") == 0)
    {
        return dc::example_reload(argv[2], argv[3], argv[4]);
    }
    if (argc == 3 && std::strcmp(route, "capture") == 0)
    {
        return dc::example_capture(argv[2]);
    }
    std::printf("usage: diag-consumer fail <profile.host.toml> | inspect <directory> <bundle name> | allocator "
                "<profile> <tlsf|diagnostic> | jobs <profile> | reload <profile> <limits.toml> <directory> | "
                "capture <profile>\n");
    return 2;
}
