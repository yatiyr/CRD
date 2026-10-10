// crd-perf -- the flight recorder's host side and its flight.* commands (DIAG.9b). See crd/perf/flight_commands.hpp.

#include <crd/perf/flight_commands.hpp>

#include <crd/perf/bundle.hpp>
#include <crd/perf/diag_assets.hpp>

#include <cstdio>

namespace crd::perf
{

FlightCommandHost::FlightCommandHost(FlightRecorder& recorder) noexcept : m_recorder(recorder) {}

FlightCommandHost::~FlightCommandHost()
{
    if (m_installed.load(std::memory_order_acquire))
    {
        m_recorder.set_sink(nullptr, nullptr);
    }
}

void FlightCommandHost::install_sink() noexcept
{
    m_recorder.set_sink(&FlightCommandHost::deliver, this);
    m_installed.store(m_recorder.is_initialized(), std::memory_order_release);
}

void FlightCommandHost::set_forward(FlightCaptureSink sink, void* user) noexcept
{
    m_forward_user.store(user, std::memory_order_release);
    m_forward.store(sink, std::memory_order_release);
}

void FlightCommandHost::deliver(const FlightCapture& capture, void* host)
{
    auto* const self = static_cast<FlightCommandHost*>(host);
    if (self == nullptr)
    {
        return;
    }
    {
        const std::lock_guard<std::mutex> guard(self->m_mutex);
        self->m_last     = capture;
        self->m_has_last = true;
    }
    self->m_received.fetch_add(1U, std::memory_order_acq_rel);
    const FlightCaptureSink forward = self->m_forward.load(std::memory_order_acquire);
    if (forward != nullptr)
    {
        forward(capture, self->m_forward_user.load(std::memory_order_acquire));
    }
}

bool FlightCommandHost::retained(FlightCapture& out) const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_has_last)
    {
        return false;
    }
    out = m_last;
    return true;
}

// Holds m_modules_mutex and records the holding thread, so try_modules() on that same thread (a forward sink reached
// from an assertion inside the merge) refuses instead of locking a mutex its thread already owns.
class FlightCommandHost::ModulesLock
{
public:
    explicit ModulesLock(const FlightCommandHost& host) : m_host(host)
    {
        m_host.m_modules_mutex.lock();
        m_host.m_modules_owner.store(std::this_thread::get_id(), std::memory_order_release);
    }
    ~ModulesLock()
    {
        m_host.m_modules_owner.store(std::thread::id{}, std::memory_order_release);
        m_host.m_modules_mutex.unlock();
    }

    ModulesLock(const ModulesLock&)            = delete;
    ModulesLock& operator=(const ModulesLock&) = delete;
    ModulesLock(ModulesLock&&)                 = delete;
    ModulesLock& operator=(ModulesLock&&)      = delete;

private:
    const FlightCommandHost& m_host;
};

bool FlightCommandHost::refresh_modules()
{
    const LiveModules live = enumerate_loaded_modules();
    if (!live.available)
    {
        return false;
    }
    const ModulesLock guard(*this);
    bool              overflow = live.overflow;
    (void)merge_module_snapshot(m_modules, cont::ConstSpan<ModuleIdentity>{live.modules.data(), live.modules.size()},
                                overflow);
    m_modules_overflow = m_modules_overflow || overflow;
    return true;
}

cont::Array<ModuleIdentity> FlightCommandHost::modules() const
{
    const ModulesLock guard(*this);
    return m_modules;
}

bool FlightCommandHost::modules_overflow() const
{
    const ModulesLock guard(*this);
    return m_modules_overflow;
}

namespace
{
// Bounded attempts at the snapshot's lock from a forward sink (the recorder's fatal-path ring lock takes the same).
constexpr crd::u32 kModulesLockAttempts = 2000U;
} // namespace

bool FlightCommandHost::try_modules(cont::Array<ModuleIdentity>& out, bool& overflow) const
{
    out.clear();
    overflow = false;
    if (m_modules_owner.load(std::memory_order_acquire) == std::this_thread::get_id())
    {
        return false; // this thread holds the lock: the sink was reached from inside a snapshot update
    }
    bool locked = false;
    for (crd::u32 attempt = 0U; attempt < kModulesLockAttempts && !locked; ++attempt)
    {
        locked = m_modules_mutex.try_lock();
        if (!locked)
        {
            std::this_thread::yield();
        }
    }
    if (!locked)
    {
        return false;
    }
    const std::lock_guard<std::mutex> guard(m_modules_mutex, std::adopt_lock);
    out      = m_modules;
    overflow = m_modules_overflow;
    return true;
}

namespace
{
void append_trigger_names(cont::String& out, crd::u32 mask)
{
    if (mask == kFlightAllTriggers)
    {
        out.append("all");
        return;
    }
    bool first = true;
    for (crd::u32 t = 1U; t < kFlightTriggerCount; ++t)
    {
        const auto kind = static_cast<FlightTrigger>(t);
        if ((mask & flight_trigger_bit(kind)) == 0U)
        {
            continue;
        }
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        out.append(flight_trigger_name(kind));
    }
}

void append_generation_kept(cont::String& reason, const FlightRecorder& recorder)
{
    char text[64];
    (void)std::snprintf(text, sizeof(text), "; the active policy stays at generation %u",
                        static_cast<unsigned>(recorder.policy_generation()));
    reason.append(text);
}

// The recorder's state and active policy, as summary fields.
void write_recorder_fields(FlightRecorder& recorder, DiagFields& fields, cont::String& scratch)
{
    const FlightPolicy policy = recorder.policy();
    scratch.clear();
    append_trigger_names(scratch, policy.trigger_mask);
    fields.boolean("initialized", recorder.is_initialized())
        .u64("generation", recorder.policy_generation())
        .u64("capacity", policy.capacity)
        .u64("pre_events", policy.pre_events)
        .u64("post_events", policy.post_events)
        .u64("post_window_ns", policy.post_window_ns)
        .u64("hitch_threshold_ns", policy.hitch_threshold_ns)
        .str("triggers", cont::StringView{scratch.data(), scratch.size()})
        .u64("recorded", recorder.total_recorded())
        .boolean("pending", recorder.capture_pending())
        .boolean("fatal", recorder.fatal_entered())
        .u64("delivered", recorder.captures_delivered());
}

DiagStatus run_flight_status(void* context, const DiagCall& /*call*/, DiagSnapshot& out)
{
    auto* const  host = static_cast<FlightCommandHost*>(context);
    cont::String scratch(out.allocator());
    write_recorder_fields(host->recorder(), out.summary, scratch);
    out.summary.u64("received", host->received());

    FlightCapture capture;
    if (!host->retained(capture))
    {
        out.summary.boolean("retained", false);
        return DiagStatus::Ok;
    }
    out.summary.boolean("retained", true)
        .str("trigger", flight_trigger_name(capture.trigger.kind))
        .u64("trigger_code", capture.trigger.code)
        .u64("trigger_sequence", capture.trigger.sequence)
        .str("trigger_message", capture.trigger.message_view())
        .str("end", flight_end_name(capture.end))
        .u64("pre_retained", capture.pre_retained)
        .u64("pre_dropped", capture.recorded_before_trigger - capture.pre_retained)
        .u64("post_retained", capture.post_retained)
        .boolean("post_truncated", capture.post_truncated)
        .boolean("ring_unavailable", capture.ring_unavailable)
        .u64("suppressed", capture.suppressed_triggers)
        .boolean("replay_available", capture.replay_available)
        .str("replay", cont::StringView{capture.replay.data(), capture.replay.size()});
    DiagFields item(out.allocator());
    for (const FlightEvent& e : capture.events)
    {
        item.clear();
        item.u64("sequence", e.sequence)
            .u64("code", e.code)
            .str("severity", severity_name(e.severity))
            .u64("a", e.value_a)
            .u64("b", e.value_b)
            .str("message", e.message_view())
            .boolean("message_truncated", e.message_truncated);
        (void)out.add_item(item);
    }
    return DiagStatus::Ok;
}

DiagStatus check_flight_trigger(void* /*context*/, cont::ConstSpan<DiagArg> args, cont::String& reason)
{
    if (args.empty() || (args.size() == 1U && args[0].name == cont::StringView{"message"}))
    {
        return DiagStatus::Ok;
    }
    reason.append("flight.trigger takes at most one argument, message=<text>");
    return DiagStatus::BadArgument;
}

DiagStatus run_flight_trigger(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const       host = static_cast<FlightCommandHost*>(context);
    FlightTriggerInput input;
    input.kind    = FlightTrigger::Manual;
    input.code    = kFlightCodeManual;
    input.message = call.request->args.empty() ? cont::StringView{"flight.trigger"} : call.request->args[0].value;
    switch (host->recorder().trigger(input)) // no default: every result is answered
    {
    case FlightTriggerResult::Started:
        break;
    case FlightTriggerResult::Suppressed:
        out.reason.append("a capture is pending; the trigger was counted as suppressed in it");
        return DiagStatus::Failed;
    case FlightTriggerResult::Masked:
        out.reason.append("the active flight policy does not arm manual triggers");
        return DiagStatus::Failed;
    case FlightTriggerResult::NotInitialized:
        out.reason.append("the flight recorder is not initialised in this process");
        return DiagStatus::Unavailable;
    }
    cont::String scratch(out.allocator());
    write_recorder_fields(host->recorder(), out.summary, scratch);
    out.summary.str("result", cont::StringView{"started"});
    return DiagStatus::Ok;
}

DiagStatus run_flight_policy(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const    host     = static_cast<FlightCommandHost*>(context);
    FlightRecorder& recorder = host->recorder();
    if (!recorder.is_initialized())
    {
        out.reason.append("the flight recorder is not initialised in this process");
        return DiagStatus::Unavailable;
    }
    const cont::String  path(call.file.data(), call.file.size(), out.allocator());
    cont::String        text(out.allocator());
    const DiagAssetRead read = read_diag_asset_file(path.c_str(), text);
    switch (read) // no default: every result is answered
    {
    case DiagAssetRead::Ok:
        break;
    case DiagAssetRead::Missing:
        out.reason.append("the policy asset could not be opened");
        append_generation_kept(out.reason, recorder);
        return DiagStatus::Failed;
    case DiagAssetRead::TooLarge:
        out.reason.append("the policy asset is larger than 16384 bytes");
        append_generation_kept(out.reason, recorder);
        return DiagStatus::Oversized;
    case DiagAssetRead::Failed:
        out.reason.append("the policy asset could not be read");
        append_generation_kept(out.reason, recorder);
        return DiagStatus::Failed;
    }

    FlightPolicy candidate;
    if (!parse_flight_policy_asset(cont::StringView{text.data(), text.size()}, candidate, out.reason))
    {
        append_generation_kept(out.reason, recorder);
        return DiagStatus::BadArgument;
    }
    const crd::u32           previous = recorder.policy_generation();
    const FlightPolicyStatus status   = recorder.reload_policy(candidate);
    if (status != FlightPolicyStatus::Committed)
    {
        out.reason.append("the recorder refused the policy: ");
        out.reason.append(flight_policy_status_name(status));
        if (status == FlightPolicyStatus::RejectedBusy)
        {
            out.reason.append(" (a capture is pending until it is collected)");
        }
        append_generation_kept(out.reason, recorder);
        return status == FlightPolicyStatus::RejectedNotInitialized ? DiagStatus::Unavailable : DiagStatus::Failed;
    }
    cont::String scratch(out.allocator());
    out.summary.str("status", flight_policy_status_name(status)).u64("previous_generation", previous);
    write_recorder_fields(recorder, out.summary, scratch);
    return DiagStatus::Ok;
}

DiagStatus run_flight_write(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const host = static_cast<FlightCommandHost*>(context);
    // A capture whose post window has closed is collected first, so it is the one written.
    (void)host->recorder().poll();
    FlightCapture capture;
    if (!host->retained(capture))
    {
        out.reason.append("no flight capture has been delivered to this host");
        return DiagStatus::Failed;
    }
    const cont::String path(call.file.data(), call.file.size(), out.allocator());
    std::FILE*         existing = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&existing, path.c_str(), "rb") != 0)
    {
        existing = nullptr;
    }
#else
    existing = std::fopen(path.c_str(), "rb");
#endif
    if (existing != nullptr)
    {
        (void)std::fclose(existing);
        out.reason.append("refusing to overwrite an existing file");
        return DiagStatus::Failed;
    }
    // The module snapshot is refreshed first, outside any recorder lock: this is a request, never a fatal path.
    (void)host->refresh_modules();
    const cont::Array<ModuleIdentity> modules = host->modules();
    const cont::Array<crd::u8>        bundle =
        write_flight_bundle(capture, cont::ConstSpan<ModuleIdentity>{modules.data(), modules.size()});
    if (bundle.empty() || !write_bundle_atomic(path.c_str(), cont::ConstSpan<crd::u8>{bundle.data(), bundle.size()}))
    {
        out.reason.append("the flight bundle could not be written");
        return DiagStatus::Failed;
    }
    out.summary.u64("bytes", bundle.size())
        .u64("modules", modules.size())
        .boolean("modules_overflow", host->modules_overflow())
        .str("trigger", flight_trigger_name(capture.trigger.kind))
        .str("end", flight_end_name(capture.end))
        .u64("events", capture.events.size());
    return DiagStatus::Ok;
}
} // namespace

bool register_flight_commands(DiagCommandService& service, FlightCommandHost& host)
{
    DiagCommandSpec status;
    status.name      = cont::StringView{"flight.status"};
    status.owner     = cont::StringView{"perf"};
    status.summary   = cont::StringView{"the flight recorder's state and policy, and the retained capture's events"};
    status.authority = DiagAuthority::Read;

    DiagCommandSpec trigger;
    trigger.name      = cont::StringView{"flight.trigger"};
    trigger.owner     = cont::StringView{"perf"};
    trigger.summary   = cont::StringView{"fire a manual flight trigger (message=<text>)"};
    trigger.authority = DiagAuthority::Record;

    DiagCommandSpec policy;
    policy.name       = cont::StringView{"flight.policy"};
    policy.owner      = cont::StringView{"perf"};
    policy.summary    = cont::StringView{"reload the flight policy from a flight-policy asset under the root"};
    policy.authority  = DiagAuthority::Record;
    policy.takes_path = true;

    DiagCommandSpec write;
    write.name       = cont::StringView{"flight.write"};
    write.owner      = cont::StringView{"perf"};
    write.summary    = cont::StringView{"write the retained flight capture as a bundle under the root"};
    write.authority  = DiagAuthority::Record;
    write.takes_path = true;

    const bool s = service.register_command(status, &run_flight_status, &host);
    const bool t = service.register_command(trigger, &run_flight_trigger, &host, &check_flight_trigger);
    const bool p = service.register_command(policy, &run_flight_policy, &host);
    const bool w = service.register_command(write, &run_flight_write, &host);
    return s && t && p && w;
}

} // namespace crd::perf
