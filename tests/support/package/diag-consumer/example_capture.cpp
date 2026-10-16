// Example: capture on request from a running application (DIAG.11a).
//
//   diag-consumer capture <profile.host.toml>
//
// Not every investigation starts with a failure. The application runs a frame loop and hands every frame to the host
// (host.frame(begin, end)): the host records each frame boundary, watches for frame hitches and delivers a capture
// whose post window has closed. Three times during the run someone asks for a capture through the host's own
// diagnostic service (flight.trigger, which needs the record authority the investigation profile grants), as a
// human at a console or an agent over MCP would. Each capture keeps the frames before the request and the frames
// after it, and the host writes it to the profile's capture directory while the application keeps running; the
// directory keeps only the newest max_bundles of them. flight.status then reports the last capture, and the
// application's inspect route (main.cpp) reads any bundle back.
#include "consumer_support.hpp"
#include "examples.hpp"

#include <crd/perf/flight_recorder.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace diag_consumer
{

namespace
{

constexpr crd::u32 kFrames            = 140U;
constexpr crd::u32 kFirstRequestFrame = 20U;
constexpr crd::u32 kRequestInterval   = 40U;
constexpr crd::u32 kRequests          = 3U;

} // namespace

int example_capture(const char* profile_path)
{
    cp::DiagnosticHost host;
    if (!start_host(host, profile_path, "diag-consumer capture <profile.host.toml>"))
    {
        return 4;
    }
    cp::DiagServiceConfig config;
    host.configure_service(config);
    cp::DiagCommandService service{host.profile().command_grant, config};
    if (!host.serve(service))
    {
        std::printf("CONSUMER_SERVE_FAILED\n");
        return 6;
    }

    crd::u32 requested = 0U;
    char     text[64];
    for (crd::u32 frame = 0U; frame < kFrames; ++frame)
    {
        const crd::u64 begin = cp::diagnostic_now_ns();
        CRD_FLIGHT_RECORD(&host.recorder(), kFrameWork, cp::Severity::Info,
                          site_text(text, sizeof(text), "frame %u simulated", static_cast<unsigned>(frame)), frame, 8U);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (requested < kRequests && frame == kFirstRequestFrame + (requested * kRequestInterval))
        {
            char request[64];
            (void)std::snprintf(request, sizeof(request), "capture requested at frame %u",
                                static_cast<unsigned>(frame));
            (void)run_command(service, "flight.trigger", "message", request);
            ++requested;
        }
        (void)host.frame(begin, cp::diagnostic_now_ns());
    }
    const bool written = wait_for_bundles(host, kRequests, 10000U);
    (void)run_command(service, "flight.status");
    std::printf("CONSUMER_CAPTURE requested=%u written=%llu all_written=%d frames=%llu hitches=%llu max_bundles=%u\n",
                static_cast<unsigned>(requested), static_cast<unsigned long long>(host.bundles_written()),
                written ? 1 : 0, static_cast<unsigned long long>(host.frames()),
                static_cast<unsigned long long>(host.hitches()), static_cast<unsigned>(host.profile().max_bundles));
    if (written)
    {
        print_last_bundle(host, "manual");
    }
    return written ? 0 : 8; // the service goes first, then the host (its destructor stops it)
}

} // namespace diag_consumer
