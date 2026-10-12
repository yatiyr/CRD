// Example: reload under the diagnostic host (DIAG.11a).
//
//   diag-consumer reload <profile.host.toml> <limits.toml> <second capture directory>
//
// Two things reload in a running application, and the host covers both.
//
// The authored diagnostic configuration. The application keeps the profile it started from and hands the host a
// changed one with apply(); the host says what happened. An unchanged profile is Unchanged. The flight policy is
// re-read from its asset, so editing the policy file and applying the same profile is a live change (Applied, a new
// policy generation). A field chosen at start (here the watchdog window) answers RestartRequired, names the field and
// changes nothing. A new capture directory applies live: the next bundle lands there.
//
// The application's own authored data, an order-limits table (TOML, read with the packaged crd-toml). Its reader asks
// the engine's I/O fault seam before it touches the file (crd::consult_io_fault), as the engine's own readers do, so
// the host's fault injection reaches the application's reload too. The example arms a seeded plan through the host's
// own command service (fault.arm, which the investigation profile grants), edits the table, and reloads: the injected
// read failure is reported, the last good table is kept, and the injected-fault capture is written to the new capture
// directory. The next reload, with the plan disarmed, loads the edited table. A shipping build compiles the seam out,
// so there fault.* is not served and the edit simply loads.
#include "consumer_support.hpp"
#include "examples.hpp"

#include <crd/core/fault_seam.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/toml/toml.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <thread>

namespace diag_consumer
{

namespace containers = crd::containers;

namespace
{

constexpr crd::usize kMaxTableBytes   = 4096U;
constexpr crd::u32   kWatchdogChangeMs = 50U;

// The application's limits table and its generation (how many times a table was loaded).
struct Limits
{
    crd::u64 generation = 0U;
    crd::i64 max_order  = 0;
};

std::FILE* open_file(const char* path, const char* mode)
{
#if defined(_WIN32)
    std::FILE* file = nullptr;
    return fopen_s(&file, path, mode) == 0 ? file : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

bool write_text(const char* path, const char* text)
{
    std::FILE* const file = open_file(path, "wb");
    if (file == nullptr)
    {
        return false;
    }
    const crd::usize length = std::strlen(text);
    const bool       ok     = std::fwrite(text, 1U, length, file) == length;
    return (std::fclose(file) == 0) && ok;
}

// Read the table through the engine's I/O fault seam, then parse it. On any failure `limits` keeps the last good
// table and `why` says what failed.
bool load_limits(const char* path, Limits& limits, const char*& why)
{
    const crd::IoFault fault = crd::consult_io_fault(crd::IoFaultOp::ReadWhole, path, std::strlen(path), 0U, 0U);
    if (fault.action == crd::IoFaultAction::Fail)
    {
        why = "read-failed";
        return false;
    }
    if (fault.action == crd::IoFaultAction::Delay)
    {
        std::this_thread::sleep_for(std::chrono::nanoseconds(fault.value));
    }
    std::FILE* const file = open_file(path, "rb");
    if (file == nullptr)
    {
        why = "missing";
        return false;
    }
    char             text[kMaxTableBytes];
    const crd::usize read = std::fread(text, 1U, sizeof(text), file);
    const bool       all  = std::feof(file) != 0;
    (void)std::fclose(file);
    if (!all)
    {
        why = "too-large";
        return false;
    }
    if (fault.action == crd::IoFaultAction::Short && fault.value < read)
    {
        why = "short-read"; // the seam delivered fewer bytes than the file holds: never parse a torn table
        return false;
    }
    const crd::toml::parse_result table = crd::toml::parse(std::string_view{text, read}, path);
    if (!table)
    {
        why = "parse";
        return false;
    }
    const std::optional<std::string_view> kind      = table["kind"].value<std::string_view>();
    const std::optional<crd::i64>         max_order = table["max_order"].value<crd::i64>();
    if (!kind.has_value() || *kind != "order-limits" || !max_order.has_value() || *max_order <= 0)
    {
        why = "invalid";
        return false;
    }
    ++limits.generation;
    limits.max_order = *max_order;
    return true;
}

void reload_limits(cp::DiagnosticHost& host, const char* path, Limits& limits, crd::u32 attempt)
{
    const char* why    = "";
    const bool  loaded = load_limits(path, limits, why);
    char        text[64];
    (void)std::snprintf(text, sizeof(text), "limits %s at generation %llu", loaded ? "loaded" : "kept",
                        static_cast<unsigned long long>(limits.generation));
    host.recorder().record(loaded ? kLimitsLoaded : kLimitsKept, loaded ? cp::Severity::Info : cp::Severity::Warning,
                           containers::StringView{text}, limits.generation, static_cast<crd::u64>(limits.max_order));
    std::printf("CONSUMER_RELOAD_DATA attempt=%u result=%s why=%s generation=%llu max_order=%lld\n",
                static_cast<unsigned>(attempt), loaded ? "loaded" : "kept", loaded ? "none" : why,
                static_cast<unsigned long long>(limits.generation), static_cast<long long>(limits.max_order));
    (void)std::fflush(stdout);
}

void print_apply(const char* step, cp::DiagHostApply answer, const containers::String& reason, cp::DiagnosticHost& host)
{
    std::printf("CONSUMER_RELOAD_APPLY step=%s answer=%s policy_generation=%u watchdog_ms=%u reason=\"%s\"\n", step,
                cp::diag_host_apply_name(answer).data(),
                static_cast<unsigned>(host.recorder().policy_generation()),
                static_cast<unsigned>(host.profile().watchdog_period_ms), reason.c_str());
    (void)std::fflush(stdout);
}

// The edit an operator makes to the policy file: the frame-hitch threshold doubled, written back as an asset.
bool edit_policy(const containers::String& path)
{
    containers::String text;
    if (cp::read_diag_asset_file(path.c_str(), text) != cp::DiagAssetRead::Ok)
    {
        return false;
    }
    cp::FlightPolicy   policy;
    containers::String reason;
    if (!cp::parse_flight_policy_asset(containers::StringView{text.data(), text.size()}, policy, reason))
    {
        return false;
    }
    policy.hitch_threshold_ns *= 2U;
    containers::String edited;
    cp::flight_policy_to_asset(policy, edited);
    return write_text(path.c_str(), edited.c_str());
}

} // namespace

int example_reload(const char* profile_path, const char* limits_path, const char* second_capture_dir)
{
    cp::DiagnosticHost host;
    if (!start_host(host, profile_path, "diag-consumer reload <profile.host.toml> <limits.toml> <directory>"))
    {
        return 4;
    }
    const cp::DiagnosticHostProfile active = host.profile();
    if (active.flight_policy.empty())
    {
        std::printf("CONSUMER_USAGE the reload example needs a profile that names a flight-policy asset\n");
        return 2;
    }
    cp::DiagServiceConfig config;
    host.configure_service(config);
    cp::DiagCommandService service{active.command_grant, config};
    if (!host.serve(service))
    {
        std::printf("CONSUMER_SERVE_FAILED\n");
        return 6;
    }
    Limits limits;
    reload_limits(host, limits_path, limits, 1U);

    // The diagnostic configuration.
    containers::String reason;
    print_apply("unchanged", host.apply(active, reason), reason, host);
    const bool edited = edit_policy(active.flight_policy);
    reason.clear();
    print_apply(edited ? "policy-edited" : "policy-edit-failed", host.apply(active, reason), reason, host);
    std::printf("CONSUMER_RELOAD_POLICY hitch_threshold_ns=%llu\n",
                static_cast<unsigned long long>(host.recorder().policy().hitch_threshold_ns));
    cp::DiagnosticHostProfile restart = active;
    restart.watchdog_period_ms        = active.watchdog_period_ms + kWatchdogChangeMs;
    reason.clear();
    print_apply("watchdog-changed", host.apply(restart, reason), reason, host);
    cp::DiagnosticHostProfile moved = active;
    moved.capture_dir               = containers::String{second_capture_dir};
    reason.clear();
    print_apply("capture-dir-changed", host.apply(moved, reason), reason, host);

    // The application's own data, read through the engine's I/O fault seam.
    (void)run_command(service, "fault.arm", "plan", "seed=1;failed-read@0+1%1000000");
    if (!write_text(limits_path, "kind = \"order-limits\"\nmax_order = 750\n"))
    {
        std::printf("CONSUMER_WRITE_FAILED %s\n", limits_path);
        return 9;
    }
    reload_limits(host, limits_path, limits, 2U);
    if (host.modes().faults == cp::DiagModeState::Active)
    {
        if (!wait_for_bundles(host, 1U, 10000U))
        {
            std::printf("CONSUMER_NO_BUNDLE written=%llu\n", static_cast<unsigned long long>(host.bundles_written()));
            return 8;
        }
        print_last_bundle(host, "injected-fault");
        (void)run_command(service, "fault.disarm");
    }
    reload_limits(host, limits_path, limits, 3U);
    return 0; // the service goes first, then the host (its destructor stops it)
}

} // namespace diag_consumer
