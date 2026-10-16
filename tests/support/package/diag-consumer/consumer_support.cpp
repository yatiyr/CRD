// What every route of the diagnostic application shares (DIAG.11a); see consumer_support.hpp.
#include "consumer_support.hpp"

#include <crd/core/assert.hpp>
#include <crd/core/build_config.hpp>
#include <crd/core/fault_seam.hpp>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>
#endif

namespace diag_consumer
{

namespace containers = crd::containers;

namespace
{

std::atomic<crd::u64> g_site_texts{0U}; // the messages site_text formatted

} // namespace

containers::StringView site_text(char* buffer, crd::usize capacity, const char* format, ...)
{
    g_site_texts.fetch_add(1U, std::memory_order_relaxed);
    if (capacity == 0U)
    {
        return containers::StringView{};
    }
    std::va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, capacity, format, args);
    va_end(args);
    const crd::usize length = written < 0 ? 0U : static_cast<crd::usize>(written);
    return containers::StringView{buffer, length < capacity ? length : capacity - 1U};
}

void print_sites(const cp::FlightRecorder& recorder)
{
    std::printf("CONSUMER_SITES detail=%s formatted=%llu recorded=%llu filtered=%llu\n",
                cp::flight_detail_name(recorder.policy().detail).data(),
                static_cast<unsigned long long>(g_site_texts.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(recorder.total_recorded()),
                static_cast<unsigned long long>(recorder.total_filtered()));
    (void)std::fflush(stdout);
}

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

bool start_host(cp::DiagnosticHost& host, const char* profile_path, const char* recipe)
{
    cp::DiagnosticHostProfile profile;
    containers::String        reason;
    if (!cp::load_diagnostic_host_profile(profile_path, profile, reason))
    {
        std::printf("CONSUMER_PROFILE_REFUSED %s\n", reason.c_str());
        return false;
    }
    const cp::DiagHostStart start = host.start(profile, reason);
    if (start != cp::DiagHostStart::Started)
    {
        std::printf("CONSUMER_HOST_REFUSED %s %s\n", cp::diag_host_start_name(start).data(), reason.c_str());
        return false;
    }
    const cp::DiagnosticHostModes& modes = host.modes();
    std::printf("CONSUMER_HOST_STARTED crash=%s flight=%s faults=%s\n", cp::diag_mode_state_name(modes.crash).data(),
                cp::diag_mode_state_name(modes.flight).data(), cp::diag_mode_state_name(modes.faults).data());
    (void)std::fflush(stdout);
    (void)cp::register_code_range("diag-consumer", kCodeRangeFirst, kCodeRangeLast);
    host.recorder().set_replay_recipe(containers::StringView{recipe});
    return true;
}

cp::DiagStatus run_command(cp::DiagCommandService& service, const char* command, const char* arg_name,
                           const char* arg_value)
{
    cp::DiagArg arg;
    if (arg_name != nullptr)
    {
        arg.name  = containers::StringView{arg_name};
        arg.value = containers::StringView{arg_value != nullptr ? arg_value : ""};
    }
    cp::DiagRequest request;
    request.command    = containers::StringView{command};
    request.page_items = cp::kDiagMaxPageItems;
    request.page_bytes = cp::kDiagMaxPageBytes;
    if (arg_name != nullptr)
    {
        request.args = containers::ConstSpan<cp::DiagArg>{&arg, 1U};
    }
    const cp::DiagResult result = service.execute(request);
    std::printf("CONSUMER_COMMAND command=%s status=%s %s\n", command, cp::status_name(result.status).data(),
                result.json.c_str());
    (void)std::fflush(stdout);
    return result.status;
}

bool wait_for_bundles(cp::DiagnosticHost& host, crd::u64 count, crd::u32 timeout_ms)
{
    // A capture is delivered, and written, by the host's poll once its post window closes (by events or by time).
    for (crd::u32 waited = 0U; waited <= timeout_ms; waited += 5U)
    {
        (void)host.poll();
        if (host.bundles_written() >= count)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return host.bundles_written() >= count;
}

void print_last_bundle(const cp::DiagnosticHost& host, const char* label)
{
    const containers::String path = host.last_bundle();
    const char*              name = path.c_str();
    for (const char* p = path.c_str(); *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            name = p + 1;
        }
    }
    std::printf("CONSUMER_BUNDLE label=%s name=%s written=%llu failures=%llu\n", label, name,
                static_cast<unsigned long long>(host.bundles_written()),
                static_cast<unsigned long long>(host.bundle_failures()));
    (void)std::fflush(stdout);
}

} // namespace diag_consumer
