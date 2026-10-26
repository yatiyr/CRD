// crd-perf -- DIAG.11a: the reusable diagnostic host. See crd/perf/diagnostic_host.hpp.

#include <crd/perf/diagnostic_host.hpp>

#include <crd/core/fault_seam.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/time/clocks.hpp>

#include <cstdio>
#include <thread>

namespace crd::perf
{
namespace
{
// Bounded attempts at the writer's lock: a capture delivered on a fatal path while another thread writes waits this
// long at most, then counts a failure instead of blocking the fatal path.
constexpr crd::u32 kWriterLockAttempts = 2000U;
// Names tried for one bundle before the writer gives up.
constexpr crd::u32 kMaxNameAttempts = 4096U;

// The thread inside write_capture (default: none). A capture delivered again on that thread (an assertion inside the
// writer) is not written: the writer's lock is held below it.
std::atomic<std::thread::id> g_writer_owner{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

bool same_text(const cont::String& a, const cont::String& b) noexcept
{
    return cont::StringView{a.data(), a.size()} == cont::StringView{b.data(), b.size()};
}

bool same_policy(const FlightPolicy& a, const FlightPolicy& b) noexcept
{
    return a.schema_version == b.schema_version && a.capacity == b.capacity && a.pre_events == b.pre_events &&
           a.post_events == b.post_events && a.post_window_ns == b.post_window_ns &&
           a.trigger_mask == b.trigger_mask && a.hitch_threshold_ns == b.hitch_threshold_ns && a.detail == b.detail;
}

constexpr crd::u32 field_bit(DiagHostField f) noexcept
{
    return static_cast<crd::u32>(f);
}

// The flight policy a profile names: FlightPolicy{} for an empty path, else the parsed asset.
bool load_policy(const cont::String& path, FlightPolicy& out, cont::String& reason)
{
    if (path.empty())
    {
        out = FlightPolicy{};
        return true;
    }
    cont::String        text;
    const DiagAssetRead read = read_diag_asset_file(path.c_str(), text);
    if (read != DiagAssetRead::Ok)
    {
        reason.append(read == DiagAssetRead::TooLarge ? "the flight policy asset is larger than 16384 bytes"
                                                       : "the flight policy asset could not be read");
        return false;
    }
    return parse_flight_policy_asset(cont::StringView{text.data(), text.size()}, out, reason);
}

// Whether a mode asked for as `asked` runs, given whether this build can provide it.
DiagModeState resolve(DiagMode asked, bool available) noexcept
{
    if (asked == DiagMode::Off)
    {
        return DiagModeState::Off;
    }
    return available ? DiagModeState::Active : DiagModeState::Unavailable;
}

bool install_ok(crd::crash::InstallResult r) noexcept
{
    return r == crd::crash::InstallResult::Ok || r == crd::crash::InstallResult::OkReinstalled ||
           r == crd::crash::InstallResult::OkReplacedForeignFilter;
}

const char* install_result_name(crd::crash::InstallResult r) noexcept
{
    switch (r) // no default: every result is named
    {
    case crd::crash::InstallResult::Ok:
        return "ok";
    case crd::crash::InstallResult::OkReinstalled:
        return "ok-reinstalled";
    case crd::crash::InstallResult::OkReplacedForeignFilter:
        return "ok-replaced-foreign-filter";
    case crd::crash::InstallResult::OutputDirUnusable:
        return "output-dir-unusable";
    case crd::crash::InstallResult::PathTooLong:
        return "path-too-long";
    case crd::crash::InstallResult::FilterInstallFailed:
        return "filter-install-failed";
    case crd::crash::InstallResult::Unsupported:
        return "unsupported";
    }
    return "unknown";
}
} // namespace

cont::StringView diag_mode_name(DiagMode m) noexcept
{
    switch (m) // no default: every mode is named
    {
    case DiagMode::Off:
        return cont::StringView{"off"};
    case DiagMode::On:
        return cont::StringView{"on"};
    case DiagMode::Required:
        return cont::StringView{"required"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView diag_mode_state_name(DiagModeState s) noexcept
{
    switch (s) // no default: every state is named
    {
    case DiagModeState::Off:
        return cont::StringView{"off"};
    case DiagModeState::Active:
        return cont::StringView{"active"};
    case DiagModeState::Unavailable:
        return cont::StringView{"unavailable"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView diag_host_field_name(DiagHostField f) noexcept
{
    switch (f) // no default: every field is named
    {
    case DiagHostField::CrashHandler:
        return cont::StringView{"crash_handler"};
    case DiagHostField::CrashDir:
        return cont::StringView{"crash_dir"};
    case DiagHostField::Flight:
        return cont::StringView{"flight"};
    case DiagHostField::FlightPolicy:
        return cont::StringView{"flight_policy"};
    case DiagHostField::CaptureDir:
        return cont::StringView{"capture_dir"};
    case DiagHostField::MaxBundles:
        return cont::StringView{"max_bundles"};
    case DiagHostField::FaultInjection:
        return cont::StringView{"fault_injection"};
    case DiagHostField::CommandGrant:
        return cont::StringView{"command_grant"};
    case DiagHostField::AllowRemote:
        return cont::StringView{"allow_remote"};
    case DiagHostField::WatchdogPeriodMs:
        return cont::StringView{"watchdog_period_ms"};
    }
    return cont::StringView{"unknown"};
}

void append_diag_host_fields(cont::String& out, crd::u32 fields)
{
    bool first = true;
    for (crd::u32 bit = 0U; bit < kDiagHostFieldCount; ++bit)
    {
        if ((fields & (1U << bit)) == 0U)
        {
            continue;
        }
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        out.append(diag_host_field_name(static_cast<DiagHostField>(1U << bit)));
    }
    if (first)
    {
        out.append("none");
    }
}

cont::StringView diag_host_change_kind_name(DiagHostChangeKind k) noexcept
{
    switch (k) // no default: every kind is named
    {
    case DiagHostChangeKind::Unchanged:
        return cont::StringView{"unchanged"};
    case DiagHostChangeKind::Live:
        return cont::StringView{"live"};
    case DiagHostChangeKind::RestartRequired:
        return cont::StringView{"restart-required"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView diag_host_start_name(DiagHostStart s) noexcept
{
    switch (s) // no default: every result is named
    {
    case DiagHostStart::Started:
        return cont::StringView{"started"};
    case DiagHostStart::AlreadyStarted:
        return cont::StringView{"already-started"};
    case DiagHostStart::LateStart:
        return cont::StringView{"late-start"};
    case DiagHostStart::ProfileRefused:
        return cont::StringView{"profile-refused"};
    case DiagHostStart::PolicyRefused:
        return cont::StringView{"policy-refused"};
    case DiagHostStart::DirectoryMissing:
        return cont::StringView{"directory-missing"};
    case DiagHostStart::ModeUnavailable:
        return cont::StringView{"mode-unavailable"};
    case DiagHostStart::RecorderRefused:
        return cont::StringView{"recorder-refused"};
    case DiagHostStart::TriggersRefused:
        return cont::StringView{"triggers-refused"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView diag_host_apply_name(DiagHostApply a) noexcept
{
    switch (a) // no default: every result is named
    {
    case DiagHostApply::Applied:
        return cont::StringView{"applied"};
    case DiagHostApply::Unchanged:
        return cont::StringView{"unchanged"};
    case DiagHostApply::RestartRequired:
        return cont::StringView{"restart-required"};
    case DiagHostApply::Busy:
        return cont::StringView{"busy"};
    case DiagHostApply::Refused:
        return cont::StringView{"refused"};
    case DiagHostApply::NotStarted:
        return cont::StringView{"not-started"};
    }
    return cont::StringView{"unknown"};
}

DiagHostChange classify_diagnostic_host_change(const DiagnosticHostProfile& active,
                                               const DiagnosticHostProfile& candidate) noexcept
{
    crd::u32 fields = 0U;
    const auto mark = [&fields](bool differs, DiagHostField f) noexcept
    {
        if (differs)
        {
            fields |= field_bit(f);
        }
    };
    mark(active.crash_handler != candidate.crash_handler, DiagHostField::CrashHandler);
    mark(!same_text(active.crash_dir, candidate.crash_dir), DiagHostField::CrashDir);
    mark(active.flight != candidate.flight, DiagHostField::Flight);
    mark(!same_text(active.flight_policy, candidate.flight_policy), DiagHostField::FlightPolicy);
    mark(!same_text(active.capture_dir, candidate.capture_dir), DiagHostField::CaptureDir);
    mark(active.max_bundles != candidate.max_bundles, DiagHostField::MaxBundles);
    mark(active.fault_injection != candidate.fault_injection, DiagHostField::FaultInjection);
    mark(active.command_grant != candidate.command_grant, DiagHostField::CommandGrant);
    mark(active.allow_remote != candidate.allow_remote, DiagHostField::AllowRemote);
    mark(active.watchdog_period_ms != candidate.watchdog_period_ms, DiagHostField::WatchdogPeriodMs);

    DiagHostChange change;
    change.live_fields    = fields & kDiagHostLiveFields;
    change.restart_fields = fields & ~kDiagHostLiveFields;
    if (change.restart_fields != 0U)
    {
        change.kind = DiagHostChangeKind::RestartRequired;
    }
    else if (change.live_fields != 0U)
    {
        change.kind = DiagHostChangeKind::Live;
    }
    return change;
}

DiagnosticHostCapabilities diagnostic_host_capabilities() noexcept
{
    DiagnosticHostCapabilities caps;
#if defined(_WIN32) || defined(__linux__)
    caps.crash_backend = true; // crd::crash has a Windows and a Linux backend
#endif
    caps.fault_seams = crd::fault_seams_compiled();
    return caps;
}

DiagnosticHost::DiagnosticHost() noexcept = default;

DiagnosticHost::~DiagnosticHost()
{
    stop();
}

DiagHostStart DiagnosticHost::start(const DiagnosticHostProfile& profile, cont::String& reason,
                                    const DiagnosticHostCapabilities& caps)
{
    if (m_started)
    {
        reason.append("the diagnostic host is already started");
        return DiagHostStart::AlreadyStarted;
    }
    crd::time::TickClock::warm(); // the event clock is calibrated before the first event, once per process
    if (crd::jobs::num_workers() != 0U)
    {
        reason.append("the jobs pool already runs: start the diagnostic host before the pool, devices and tools");
        return DiagHostStart::LateStart;
    }
    if (!validate_diagnostic_host_profile(profile, reason))
    {
        return DiagHostStart::ProfileRefused;
    }
    FlightPolicy policy;
    if (profile.flight != DiagMode::Off && !load_policy(profile.flight_policy, policy, reason))
    {
        return DiagHostStart::PolicyRefused;
    }
    if (!profile.capture_dir.empty() && !bundle_directory_exists(profile.capture_dir.c_str()))
    {
        reason.append("the capture directory does not exist");
        return DiagHostStart::DirectoryMissing;
    }

    // Every mode is decided before anything is installed, so a refusal leaves nothing behind.
    DiagnosticHostModes modes;
    modes.crash  = resolve(profile.crash_handler, caps.crash_backend);
    modes.flight = resolve(profile.flight, true); // the recorder is in-process and needs nothing from the build
    modes.faults = resolve(profile.fault_injection, caps.fault_seams);
    if (profile.crash_handler == DiagMode::Required && modes.crash != DiagModeState::Active)
    {
        reason.append("crash_handler is required and this build has no crash handler for the platform");
        return DiagHostStart::ModeUnavailable;
    }
    if (profile.fault_injection == DiagMode::Required && modes.faults != DiagModeState::Active)
    {
        reason.append("fault_injection is required and this build compiles the fault seams out");
        return DiagHostStart::ModeUnavailable;
    }

    if (modes.flight == DiagModeState::Active)
    {
        const FlightPolicyStatus status = m_recorder.init(policy);
        if (status != FlightPolicyStatus::Committed)
        {
            reason.append("the flight recorder refused the policy: ");
            reason.append(flight_policy_status_name(status));
            return DiagHostStart::RecorderRefused;
        }
        m_assert_trigger = install_flight_assert_trigger(&m_recorder);
        m_fatal_trigger  = m_assert_trigger && install_flight_fatal_trigger(&m_recorder);
        if (!m_fatal_trigger)
        {
            undo_start();
            reason.append("another flight recorder owns the assertion or fatal-invariant trigger");
            return DiagHostStart::TriggersRefused;
        }
    }

    // The crash handler last among the refusable steps: its install is the only one a required mode can still lose.
    if (modes.crash == DiagModeState::Active)
    {
        modes.crash_install = crd::crash::install(profile.crash_dir.c_str());
        if (install_ok(modes.crash_install))
        {
            m_crash = true;
        }
        else
        {
            modes.crash = DiagModeState::Unavailable;
            if (profile.crash_handler == DiagMode::Required)
            {
                undo_start();
                reason.append("crash_handler is required and the install failed: ");
                reason.append(install_result_name(modes.crash_install));
                return DiagHostStart::ModeUnavailable;
            }
        }
    }

    if (modes.flight == DiagModeState::Active)
    {
        {
            const std::lock_guard<std::mutex> guard(m_bundle_mutex);
            m_capture_dir = profile.capture_dir;
            m_max_bundles = profile.max_bundles;
        }
        (void)m_command_host.refresh_modules();
        m_command_host.set_forward(&DiagnosticHost::write_capture, this);
        m_command_host.install_sink();
        crd::jobs::set_starvation_handler(&flight_on_starvation, &m_recorder);
        crd::jobs::set_hang_handler(&flight_on_hang, &m_recorder);
        m_jobs_handlers = true;
    }
    if (modes.faults == DiagModeState::Active)
    {
        m_injector.attach_flight_recorder(&m_recorder);
        m_io_hook  = crd::install_io_fault_hook(&FaultInjector::io_hook, &m_injector);
        m_gpu_hook = crd::install_gpu_fault_hook(&FaultInjector::gpu_hook, &m_injector);
    }
    m_profile = profile;
    m_modes   = modes;
    m_frames  = 0U;
    m_started = true;
    return DiagHostStart::Started;
}

DiagHostApply DiagnosticHost::apply(const DiagnosticHostProfile& candidate, cont::String& reason)
{
    if (!m_started)
    {
        reason.append("the diagnostic host is not started");
        return DiagHostApply::NotStarted;
    }
    if (!validate_diagnostic_host_profile(candidate, reason))
    {
        return DiagHostApply::Refused;
    }
    const DiagHostChange change = classify_diagnostic_host_change(m_profile, candidate);
    if (change.restart_fields != 0U)
    {
        reason.append("these fields take effect at the next start: ");
        append_diag_host_fields(reason, change.restart_fields);
        return DiagHostApply::RestartRequired;
    }

    // Everything that can refuse is checked before anything changes.
    const bool   flight = m_modes.flight == DiagModeState::Active;
    FlightPolicy policy;
    bool         reload = false;
    if (flight)
    {
        if (!load_policy(candidate.flight_policy, policy, reason))
        {
            return DiagHostApply::Refused;
        }
        reload = !same_policy(policy, m_recorder.policy());
    }
    if (!candidate.capture_dir.empty() && !bundle_directory_exists(candidate.capture_dir.c_str()))
    {
        reason.append("the capture directory does not exist");
        return DiagHostApply::Refused;
    }
    if (!reload && change.live_fields == 0U)
    {
        return DiagHostApply::Unchanged;
    }
    if (reload)
    {
        const FlightPolicyStatus status = m_recorder.reload_policy(policy);
        if (status == FlightPolicyStatus::RejectedBusy)
        {
            reason.append("a capture is pending; the policy changes once it is collected");
            return DiagHostApply::Busy;
        }
        if (status != FlightPolicyStatus::Committed)
        {
            reason.append("the flight recorder refused the policy: ");
            reason.append(flight_policy_status_name(status));
            return DiagHostApply::Refused;
        }
    }
    {
        const std::lock_guard<std::mutex> guard(m_bundle_mutex);
        m_capture_dir = candidate.capture_dir;
        m_max_bundles = candidate.max_bundles;
    }
    m_profile.flight_policy = candidate.flight_policy;
    m_profile.capture_dir   = candidate.capture_dir;
    m_profile.max_bundles   = candidate.max_bundles;
    return DiagHostApply::Applied;
}

void DiagnosticHost::configure_jobs(crd::jobs::Config& config) const noexcept
{
    if (m_started && m_modes.flight == DiagModeState::Active && config.hang_watchdog_period_ms == 0U)
    {
        config.hang_watchdog_period_ms = m_profile.watchdog_period_ms;
    }
}

void DiagnosticHost::configure_service(DiagServiceConfig& config) const noexcept
{
    config.flight_recorder = m_modes.flight == DiagModeState::Active ? &m_recorder : nullptr;
    config.fault_injector  = m_modes.faults == DiagModeState::Active ? &m_injector : nullptr;
}

bool DiagnosticHost::serve(DiagCommandService& service)
{
    bool ok = true;
    if (m_started && m_modes.flight == DiagModeState::Active)
    {
        ok = register_flight_commands(service, m_command_host) && ok;
    }
    if (m_started && m_modes.faults == DiagModeState::Active)
    {
        ok = register_fault_commands(service, m_injector) && ok;
    }
    return ok;
}

crd::memory::IAllocator* DiagnosticHost::watch_allocator(crd::memory::IAllocator* backing, const char* name)
{
    if (!m_started || m_modes.flight != DiagModeState::Active || m_alloc_watched || !m_alloc_watch.watch(backing))
    {
        return nullptr; // one per host
    }
    m_alloc_watched = true;
    if (m_modes.faults != DiagModeState::Active)
    {
        return backing; // observed, not decorated: its successful calls cost nothing more
    }
    m_alloc_faults = std::make_unique<FaultAllocator>(backing, &m_injector, name);
    return m_alloc_faults.get();
}

bool DiagnosticHost::refresh_modules()
{
    return m_started && m_modes.flight == DiagModeState::Active && m_command_host.refresh_modules();
}

bool DiagnosticHost::frame(crd::u64 begin_ns, crd::u64 end_ns)
{
    if (!m_started || m_modes.flight != DiagModeState::Active)
    {
        return false;
    }
    ++m_frames;
    (void)m_frame_watch.note_frame(begin_ns, end_ns);
    return poll();
}

bool DiagnosticHost::poll()
{
    if (!m_started || m_modes.flight != DiagModeState::Active)
    {
        return false;
    }
    const crd::u64 before = m_command_host.received();
    (void)m_recorder.poll();
    return m_command_host.received() != before;
}

void DiagnosticHost::undo_start() noexcept
{
    if (m_io_hook)
    {
        crd::uninstall_io_fault_hook(); // waits for reads in progress
        m_io_hook = false;
    }
    if (m_gpu_hook)
    {
        crd::uninstall_gpu_fault_hook();
        m_gpu_hook = false;
    }
    m_injector.disarm();
    if (m_jobs_handlers)
    {
        crd::jobs::set_starvation_handler(nullptr, nullptr);
        crd::jobs::set_hang_handler(nullptr, nullptr);
        m_jobs_handlers = false;
    }
    if (m_fatal_trigger)
    {
        uninstall_flight_fatal_trigger();
        m_fatal_trigger = false;
    }
    if (m_assert_trigger)
    {
        uninstall_flight_assert_trigger();
        m_assert_trigger = false;
    }
    m_alloc_watch.release(); // the watched allocator may outlive the host
    if (m_recorder.is_initialized())
    {
        m_recorder.shutdown(); // a pending capture is delivered (and written) first
    }
    if (m_crash)
    {
        crd::crash::uninstall();
        m_crash = false;
    }
}

void DiagnosticHost::stop() noexcept
{
    undo_start();
    m_started = false;
}

cont::String DiagnosticHost::last_bundle() const
{
    const std::lock_guard<std::mutex> guard(m_bundle_mutex);
    return m_last_bundle;
}

void DiagnosticHost::write_capture(const FlightCapture& capture, void* host)
{
    auto* const self = static_cast<DiagnosticHost*>(host);
    if (self == nullptr)
    {
        return;
    }
    if (g_writer_owner.load(std::memory_order_acquire) == std::this_thread::get_id())
    {
        self->m_bundle_failures.fetch_add(1U, std::memory_order_acq_rel);
        return;
    }
    bool locked = false;
    for (crd::u32 attempt = 0U; attempt < kWriterLockAttempts && !locked; ++attempt)
    {
        locked = self->m_bundle_mutex.try_lock();
        if (!locked)
        {
            std::this_thread::yield();
        }
    }
    if (!locked)
    {
        self->m_bundle_failures.fetch_add(1U, std::memory_order_acq_rel);
        return;
    }
    const std::lock_guard<std::mutex> guard(self->m_bundle_mutex, std::adopt_lock);
    if (self->m_capture_dir.empty())
    {
        return; // captures are kept in memory only (the command host retains the last one)
    }
    g_writer_owner.store(std::this_thread::get_id(), std::memory_order_release);

    // The snapshot is read without waiting; when its lock is held the bundle declares the module list absent.
    cont::Array<ModuleIdentity> modules;
    bool                        overflow = false;
    (void)self->m_command_host.try_modules(modules, overflow);
    const cont::Array<crd::u8> bundle =
        write_flight_bundle(capture, cont::ConstSpan<ModuleIdentity>{modules.data(), modules.size()});

    bool         written = false;
    cont::String path;
    if (!bundle.empty())
    {
        for (crd::u32 attempt = 0U; attempt < kMaxNameAttempts && !written; ++attempt)
        {
            char name[48];
            (void)std::snprintf(name, sizeof(name), "flight-%llu.cdb",
                                static_cast<unsigned long long>(self->m_next_name));
            ++self->m_next_name;
            path.clear();
            path.append(cont::StringView{self->m_capture_dir.data(), self->m_capture_dir.size()});
            path.push_back('/');
            path.append(name);
            if (bundle_path_exists(path.c_str()))
            {
                continue; // never over an existing file
            }
            written = write_bundle_atomic(path.c_str(), cont::ConstSpan<crd::u8>{bundle.data(), bundle.size()});
            break;
        }
    }
    if (written)
    {
        self->m_last_bundle = path;
        (void)enforce_bundle_retention(self->m_capture_dir.c_str(), "flight-", ".cdb", self->m_max_bundles);
        self->m_bundles_written.fetch_add(1U, std::memory_order_acq_rel);
    }
    else
    {
        self->m_bundle_failures.fetch_add(1U, std::memory_order_acq_rel);
    }
    g_writer_owner.store(std::thread::id{}, std::memory_order_release);
}

} // namespace crd::perf
