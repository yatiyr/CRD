// sandbox — DIAG.9b: the sandbox as a flight-recorder and fault-injection host. See flight_host.hpp.

#include "flight_host.hpp"

#include <crd/core/fault_seam.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstdio>
#include <thread>

namespace crd::sandbox
{
namespace
{
namespace cont = crd::containers;
namespace cp   = crd::perf;

// Bounded attempts at the writer's lock: a capture delivered on a fatal path while another thread writes waits this
// long at most, then counts a failure instead of blocking the fatal path.
constexpr u32 kWriterLockAttempts = 2000U;
// Names tried for one bundle before the writer gives up (the directory keeps kDefaultMaxBundles of them).
constexpr u32 kMaxNameAttempts = 4096U;

// The thread inside write_capture (default: none). A capture delivered again on that thread (an assertion inside the
// writer) is not written: the writer's lock is held below it.
std::atomic<std::thread::id> g_writer_owner{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace

std::string_view flight_host_start_name(FlightHostStart s) noexcept
{
    switch (s) // no default: every state is named
    {
    case FlightHostStart::Started:
        return "started";
    case FlightHostStart::AlreadyStarted:
        return "already-started";
    case FlightHostStart::PolicyRefused:
        return "policy-refused";
    case FlightHostStart::RecorderRefused:
        return "recorder-refused";
    case FlightHostStart::DirectoryMissing:
        return "directory-missing";
    case FlightHostStart::TriggersRefused:
        return "triggers-refused";
    }
    return "unknown";
}

FlightHost::FlightHost() noexcept = default;

FlightHost::~FlightHost()
{
    stop();
}

FlightHostStart FlightHost::start(const FlightHostOptions& options, cont::String& reason)
{
    if (m_started)
    {
        reason.append("the flight host is already started");
        return FlightHostStart::AlreadyStarted;
    }
    cp::FlightPolicy policy;
    if (options.policy_asset != nullptr)
    {
        cont::String            text;
        const cp::DiagAssetRead read = cp::read_diag_asset_file(options.policy_asset, text);
        if (read != cp::DiagAssetRead::Ok)
        {
            reason.append(read == cp::DiagAssetRead::TooLarge ? "the flight policy asset is larger than 16384 bytes"
                                                               : "the flight policy asset could not be read");
            return FlightHostStart::PolicyRefused;
        }
        if (!cp::parse_flight_policy_asset(cont::StringView{text.data(), text.size()}, policy, reason))
        {
            return FlightHostStart::PolicyRefused;
        }
    }
    if (options.capture_dir != nullptr)
    {
        const crd::platform::fs::Path dir{cont::StringView{options.capture_dir}};
        if (!crd::platform::fs::is_directory(dir))
        {
            reason.append("the flight capture directory does not exist");
            return FlightHostStart::DirectoryMissing;
        }
    }
    const cp::FlightPolicyStatus status = m_recorder.init(policy);
    if (status != cp::FlightPolicyStatus::Committed)
    {
        reason.append("the flight recorder refused the policy: ");
        reason.append(cp::flight_policy_status_name(status));
        return FlightHostStart::RecorderRefused;
    }
    m_assert_trigger = cp::install_flight_assert_trigger(&m_recorder);
    m_fatal_trigger  = m_assert_trigger && cp::install_flight_fatal_trigger(&m_recorder);
    if (!m_fatal_trigger)
    {
        if (m_assert_trigger)
        {
            cp::uninstall_flight_assert_trigger();
            m_assert_trigger = false;
        }
        m_recorder.shutdown();
        reason.append("another flight recorder owns the assertion or fatal-invariant trigger");
        return FlightHostStart::TriggersRefused;
    }
    {
        const std::lock_guard<std::mutex> guard(m_bundle_mutex);
        m_capture_dir.clear();
        if (options.capture_dir != nullptr)
        {
            m_capture_dir.append(options.capture_dir);
        }
    }
    (void)m_command_host.refresh_modules();
    m_command_host.set_forward(options.capture_dir != nullptr ? &FlightHost::write_capture : nullptr, this);
    m_command_host.install_sink();
    m_injector.attach_flight_recorder(&m_recorder);
    crd::jobs::set_starvation_handler(&cp::flight_on_starvation, &m_recorder);
    crd::jobs::set_hang_handler(&cp::flight_on_hang, &m_recorder);
    m_jobs_handlers = true;
    m_started       = true;
    return FlightHostStart::Started;
}

void FlightHost::configure_jobs(crd::jobs::Config& config) const noexcept
{
    if (config.hang_watchdog_period_ms == 0U)
    {
        config.hang_watchdog_period_ms = kWatchdogPeriodMs;
    }
}

void FlightHost::configure_service(cp::DiagServiceConfig& config) const noexcept
{
    config.flight_recorder = &m_recorder;
    config.fault_injector  = &m_injector;
}

bool FlightHost::serve(cp::DiagCommandService& service)
{
    const bool flight = cp::register_flight_commands(service, m_command_host);
    const bool faults = cp::register_fault_commands(service, m_injector);
    if (m_started && !m_io_hook && !m_gpu_hook)
    {
        m_io_hook  = crd::install_io_fault_hook(&cp::FaultInjector::io_hook, &m_injector);
        m_gpu_hook = crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &m_injector);
    }
    return flight && faults;
}

crd::memory::IAllocator* FlightHost::watch_allocator(crd::memory::IAllocator* backing)
{
    if (m_alloc_faults != nullptr || backing == nullptr)
    {
        return nullptr; // one per host
    }
    m_alloc_watch  = std::make_unique<cp::FlightAllocatorWatch>(backing, &m_recorder, "sandbox resources (flight)");
    m_alloc_faults = std::make_unique<cp::FaultAllocator>(m_alloc_watch.get(), &m_injector, "sandbox resources");
    return m_alloc_faults.get();
}

bool FlightHost::refresh_modules()
{
    return m_command_host.refresh_modules();
}

bool FlightHost::frame(u64 begin_ns, u64 end_ns)
{
    if (!m_started)
    {
        return false;
    }
    (void)m_frame_watch.note_frame(begin_ns, end_ns);
    const u64 before = m_command_host.received();
    (void)m_recorder.poll();
    return m_command_host.received() != before;
}

void FlightHost::stop() noexcept
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
        cp::uninstall_flight_fatal_trigger();
        m_fatal_trigger = false;
    }
    if (m_assert_trigger)
    {
        cp::uninstall_flight_assert_trigger();
        m_assert_trigger = false;
    }
    if (m_started)
    {
        m_recorder.shutdown(); // a pending capture is delivered (and written) first
        m_started = false;
    }
}

cont::String FlightHost::last_bundle() const
{
    const std::lock_guard<std::mutex> guard(m_bundle_mutex);
    return m_last_bundle;
}

void FlightHost::write_capture(const cp::FlightCapture& capture, void* host)
{
    auto* const self = static_cast<FlightHost*>(host);
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
    for (u32 attempt = 0U; attempt < kWriterLockAttempts && !locked; ++attempt)
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
    g_writer_owner.store(std::this_thread::get_id(), std::memory_order_release);

    // The snapshot is read without waiting; when its lock is held the bundle declares the module list absent.
    cont::Array<cp::ModuleIdentity> modules;
    bool                            overflow = false;
    (void)self->m_command_host.try_modules(modules, overflow);
    const cont::Array<u8> bundle =
        cp::write_flight_bundle(capture, cont::ConstSpan<cp::ModuleIdentity>{modules.data(), modules.size()});

    bool         written = false;
    cont::String path;
    if (!bundle.empty() && !self->m_capture_dir.empty())
    {
        for (u32 attempt = 0U; attempt < kMaxNameAttempts && !written; ++attempt)
        {
            char name[48];
            (void)std::snprintf(name, sizeof(name), "flight-%llu.cdb",
                                static_cast<unsigned long long>(self->m_next_name));
            ++self->m_next_name;
            path.clear();
            path.append(cont::StringView{self->m_capture_dir.data(), self->m_capture_dir.size()});
            path.push_back('/');
            path.append(name);
            if (crd::platform::fs::exists(crd::platform::fs::Path{cont::StringView{path.data(), path.size()}}))
            {
                continue; // never over an existing file
            }
            written = cp::write_bundle_atomic(path.c_str(), cont::ConstSpan<u8>{bundle.data(), bundle.size()});
            break;
        }
    }
    if (written)
    {
        self->m_last_bundle = path;
        (void)cp::enforce_bundle_retention(self->m_capture_dir.c_str(), "flight-", ".cdb", cp::kDefaultMaxBundles);
        self->m_bundles_written.fetch_add(1U, std::memory_order_acq_rel);
    }
    else
    {
        self->m_bundle_failures.fetch_add(1U, std::memory_order_acq_rel);
    }
    g_writer_owner.store(std::thread::id{}, std::memory_order_release);
}

} // namespace crd::sandbox
