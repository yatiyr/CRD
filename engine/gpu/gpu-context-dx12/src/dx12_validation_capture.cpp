#include "dx12_device_scope.hpp"
#include "dx12_execution.hpp"

#include <crd/gpu/dx12_validation_capture.hpp>

#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>

#include <d3d12sdklayers.h>

namespace crd::gpu
{
namespace detail
{
struct Dx12CaptureState
{
    Dx12CaptureState* next = nullptr;
    memory::IAllocator* allocator = nullptr;
    Dx12ValidationMessage* records = nullptr;
    u32 capacity = 0;
    u64 first_context = 0;
    Dx12ValidationReport report;
};
} // namespace detail

namespace
{
using Microsoft::WRL::ComPtr;
constexpr u32 kMaxDevices = 32U;
constexpr u32 kMaxMessages = 4096U;

struct DeviceRegistration
{
    ComPtr<ID3D12InfoQueue1> queue;
    ID3D12Device* identity = nullptr;
    DWORD cookie = 0;
    u32 references = 0;
    u32 id = 0;
    bool quarantined = false;
};

struct CaptureRegistry
{
    std::mutex devices_mutex;
    std::mutex records_mutex;
    detail::Dx12CaptureState* captures = nullptr;
    DeviceRegistration devices[kMaxDevices];
    u32 live_contexts = 0;
    u32 next_device = 1;
    u64 next_context = 1;
    bool enabled = false;
    bool gpu_based_validation = false; // DIAG.7a(f-3): process-global GBV state (no leak across contexts)
    detail::Dx12DredProcessState dred{}; // DIAG.7b(b): process-global DRED settings as last applied (no leak)
    bool registration_failed_to_retire = false;
};

CaptureRegistry& registry()
{
    static CaptureRegistry instance;
    return instance;
}

// DIAG.7b(b): apply the requested DRED features to the process-global settings before a device is created. Caller
// holds devices_mutex. A feature is set only when its requested state differs from the last applied one, forced ON
// or OFF explicitly, so a scope that did not ask never inherits an earlier scope's breadcrumbs or page-fault tracking.
void apply_dred_settings(detail::Dx12DredProcessState& applied, bool breadcrumbs, bool page_faults,
                         detail::Dx12DredActivation& out) noexcept
{
    out = detail::Dx12DredActivation{};
    const bool contexts = breadcrumbs; // context strings ride with breadcrumbs when the runtime supports them
    if (applied.breadcrumbs == breadcrumbs && applied.page_faults == page_faults &&
        applied.breadcrumb_contexts == contexts && !breadcrumbs && !page_faults)
    {
        return; // nothing requested and nothing to turn off: zero calls, as before DIAG.7b
    }
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
    {
        out.breadcrumbs = breadcrumbs ? detail::Dx12DredSetup::SettingsAbsent : detail::Dx12DredSetup::NotRequested;
        out.page_faults = page_faults ? detail::Dx12DredSetup::SettingsAbsent : detail::Dx12DredSetup::NotRequested;
        return;
    }
    const auto enablement = [](bool on) { return on ? D3D12_DRED_ENABLEMENT_FORCED_ON : D3D12_DRED_ENABLEMENT_FORCED_OFF; };
    if (applied.breadcrumbs != breadcrumbs)
    {
        settings->SetAutoBreadcrumbsEnablement(enablement(breadcrumbs));
        applied.breadcrumbs = breadcrumbs;
    }
    if (applied.page_faults != page_faults)
    {
        settings->SetPageFaultEnablement(enablement(page_faults));
        applied.page_faults = page_faults;
    }
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings1;
    if (SUCCEEDED(settings.As(&settings1)))
    {
        if (applied.breadcrumb_contexts != contexts)
        {
            settings1->SetBreadcrumbContextEnablement(enablement(contexts));
            applied.breadcrumb_contexts = contexts;
        }
        out.breadcrumb_contexts = contexts;
    }
    out.breadcrumbs = breadcrumbs ? detail::Dx12DredSetup::Set : detail::Dx12DredSetup::NotRequested;
    out.page_faults = page_faults ? detail::Dx12DredSetup::Set : detail::Dx12DredSetup::NotRequested;
}

void instrumentation_failure(Dx12ValidationIssue issue, HRESULT result = S_OK, u64 detail = 0U) noexcept
{
    auto& state = registry();
    const std::lock_guard lock(state.records_mutex);
    for (auto* capture = state.captures; capture != nullptr; capture = capture->next)
    {
        auto& report = capture->report;
        if (report.instrumentation_failures++ == 0U)
        {
            report.first_issue = issue;
            report.first_issue_result = result;
            report.first_issue_detail = detail;
        }
    }
}

void receive_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                     LPCSTR description, void* context) noexcept
{
    const auto* device = static_cast<const DeviceRegistration*>(context);
    auto& state = registry();
    const std::lock_guard lock(state.records_mutex);
    for (auto* capture = state.captures; capture != nullptr; capture = capture->next)
    {
        auto& report = capture->report;
        Dx12ValidationSeverity level = Dx12ValidationSeverity::Info;
        if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || severity == D3D12_MESSAGE_SEVERITY_ERROR)
        {
            ++report.errors;
            level = Dx12ValidationSeverity::Error;
        }
        else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
        {
            ++report.warnings;
            level = Dx12ValidationSeverity::Warning;
        }
        else
        {
            ++report.info;
        }
        if (report.messages == capture->capacity)
        {
            ++report.dropped;
            continue;
        }
        auto& message = capture->records[report.messages++];
        message.severity = level;
        message.id = static_cast<u32>(id);
        message.device = device->id;
        usize length = 0;
        message.identity = ObjectIdentity{}; // records storage is raw (try_allocate) -- set every field explicitly
        if (description != nullptr)
        {
            while (length + 1U < sizeof(message.text) && description[length] != '\0')
            {
                ++length;
            }
            std::memcpy(message.text, description, length);
            message.truncated = description[length] != '\0';
            // DIAG.7a(d1): parse the Cerid token from the FULL description (before the text[] truncation), so a token
            // past the cut still resolves. parse() is pure + noexcept + non-allocating -- safe in this callback.
            (void)parse(std::string_view{description}, message.identity);
        }
        message.text[length] = '\0';
        if (message.truncated)
        {
            ++report.truncated;
        }
    }
}

// D3D12's default storage filter may deny INFO. Startup qualification requires every warning/error/corruption;
// read the actual filter, rather than mistaking any variable-length filter for missing critical diagnostics.
bool critical_messages_visible(ID3D12InfoQueue1* queue, bool storage) noexcept
{
    alignas(D3D12_INFO_QUEUE_FILTER) unsigned char bytes[4096]{};
    auto* filter = reinterpret_cast<D3D12_INFO_QUEUE_FILTER*>(bytes);
    SIZE_T size = sizeof(bytes);
    const HRESULT result = storage ? queue->GetStorageFilter(filter, &size) : queue->GetRetrievalFilter(filter, &size);
    const auto issue = storage ? Dx12ValidationIssue::StorageFilter : Dx12ValidationIssue::RetrievalFilter;
    if (FAILED(result))
    {
        instrumentation_failure(issue, result, size);
        return false;
    }
    const auto& allow = filter->AllowList;
    const auto& deny = filter->DenyList;
    bool visible = allow.NumCategories == 0U && allow.NumSeverities == 0U && allow.NumIDs == 0U
        && deny.NumCategories == 0U && deny.NumIDs == 0U;
    for (UINT index = 0; index < deny.NumSeverities; ++index)
    {
        const auto severity = deny.pSeverityList[index];
        if (severity != D3D12_MESSAGE_SEVERITY_INFO && severity != D3D12_MESSAGE_SEVERITY_MESSAGE)
        {
            visible = false;
        }
    }
    if (!visible)
    {
        instrumentation_failure(issue, result, size);
    }
    return visible;
}

// First-device creation predates its InfoQueue interface. Register first, then replay the stored startup interval:
// asynchronous startup messages cannot fall between the snapshot and registration. The overlap may count a native
// notification twice, conservatively; it cannot erase an error. No commands/resources have been exposed yet.
void replay_startup(DeviceRegistration& device) noexcept
{
    auto* queue = device.queue.Get();
    (void)critical_messages_visible(queue, true);
    (void)critical_messages_visible(queue, false);
    if (queue->GetNumMessagesDiscardedByMessageCountLimit() != 0U)
    {
        instrumentation_failure(Dx12ValidationIssue::StartupOverflow);
    }
    const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    if (count > kMaxMessages)
    {
        instrumentation_failure(Dx12ValidationIssue::StartupOverflow, S_OK, count);
    }
    for (UINT64 index = 0; index < count && index < kMaxMessages; ++index)
    {
        alignas(D3D12_MESSAGE) unsigned char bytes[8192]{};
        SIZE_T size = sizeof(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes);
        const HRESULT result = queue->GetMessage(index, message, &size);
        if (FAILED(result))
        {
            instrumentation_failure(Dx12ValidationIssue::StartupRead, result, size);
            continue;
        }
        receive_message(message->Category, message->Severity, message->ID, message->pDescription, &device);
    }
}
} // namespace

Dx12ValidationCapture::Dx12ValidationCapture(memory::IAllocator* allocator, u32 capacity)
{
    if (capacity == 0U || capacity > kMaxMessages)
    {
        m_readiness = Dx12ValidationReadiness::InvalidCapacity;
        return;
    }
    if (allocator == nullptr)
    {
        return;
    }
    void* storage = allocator->try_allocate(sizeof(detail::Dx12CaptureState), alignof(detail::Dx12CaptureState));
    if (storage == nullptr)
    {
        return;
    }
    m_state = std::construct_at(static_cast<detail::Dx12CaptureState*>(storage));
    m_state->allocator = allocator;
    m_state->records = static_cast<Dx12ValidationMessage*>(allocator->try_allocate(
        sizeof(Dx12ValidationMessage) * capacity, alignof(Dx12ValidationMessage)));
    if (m_state->records == nullptr)
    {
        std::destroy_at(m_state);
        allocator->deallocate(m_state);
        m_state = nullptr;
        return;
    }
    std::uninitialized_value_construct_n(m_state->records, capacity);
    m_state->capacity = capacity;
    auto& state = registry();
    const std::lock_guard device_lock(state.devices_mutex);
    if (state.live_contexts != 0U)
    {
        m_readiness = Dx12ValidationReadiness::StartedAfterDevice;
    }
    else if (!state.enabled)
    {
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            m_readiness = Dx12ValidationReadiness::DebugLayerUnavailable;
        }
        else
        {
            debug->EnableDebugLayer();
            state.enabled = true;
            m_readiness = Dx12ValidationReadiness::Ready;
        }
    }
    else
    {
        m_readiness = Dx12ValidationReadiness::Ready;
    }
    const std::lock_guard record_lock(state.records_mutex);
    m_state->first_context = state.next_context;
    m_state->report.readiness = m_readiness;
    if (state.registration_failed_to_retire)
    {
        m_state->report.instrumentation_failures = 1U;
        m_state->report.first_issue = Dx12ValidationIssue::UnregisterCallback;
    }
    m_state->next = state.captures;
    state.captures = m_state;
}

Dx12ValidationCapture::~Dx12ValidationCapture() noexcept
{
    if (m_state == nullptr)
    {
        return;
    }
    auto& state = registry();
    {
        // The callback holds this same lock while visiting observers, so removal waits for every active visit.
        const std::lock_guard lock(state.records_mutex);
        auto** link = &state.captures;
        while (*link != m_state)
        {
            link = &(*link)->next;
        }
        *link = m_state->next;
    }
    auto* allocator = m_state->allocator;
    std::destroy_n(m_state->records, m_state->capacity);
    allocator->deallocate(m_state->records);
    std::destroy_at(m_state);
    allocator->deallocate(m_state);
}

Dx12ValidationReport Dx12ValidationCapture::report() const noexcept
{
    const std::lock_guard lock(registry().records_mutex);
    if (m_state != nullptr)
    {
        return m_state->report;
    }
    Dx12ValidationReport report;
    report.readiness = m_readiness;
    return report;
}

bool Dx12ValidationCapture::message(u32 index, Dx12ValidationMessage& output) const noexcept
{
    const std::lock_guard lock(registry().records_mutex);
    if (m_state == nullptr || index >= m_state->report.messages)
    {
        return false;
    }
    output = m_state->records[index];
    return true;
}

void detail::dx12_execution_failure(HRESULT result, const char* operation) noexcept
{
    if (SUCCEEDED(result))
    {
        return;
    }
    char text[256]{};
    (void)std::snprintf(text, sizeof(text), "%s failed: HRESULT 0x%08lX", operation,
                       static_cast<unsigned long>(result));
    DeviceRegistration source{};
    receive_message(D3D12_MESSAGE_CATEGORY_EXECUTION, D3D12_MESSAGE_SEVERITY_ERROR,
                    D3D12_MESSAGE_ID_UNKNOWN, text, &source);
    auto& state = registry();
    const std::lock_guard lock(state.records_mutex);
    for (auto* capture = state.captures; capture != nullptr; capture = capture->next)
    {
        ++capture->report.execution_failures;
    }
}

HRESULT detail::Dx12DeviceScope::create(ComPtr<ID3D12Device>& output, IUnknown* adapter) noexcept
{
    if (m_created || output != nullptr)
    {
        return E_UNEXPECTED;
    }
    auto& state = registry();
    const std::lock_guard device_lock(state.devices_mutex);
    // DIAG.7a(f-3): apply the requested validation modes to the PROCESS-GLOBAL debug state before creating this device.
    // Debug layer + GPU-based validation are process-wide; setting them under devices_mutex (the lock the capture ctor
    // uses) keeps the once-guard and this creation coherent. Existing callers request nothing -> zero extra calls.
    m_activation = ValidationActivation{};
    m_activation.requested[0] = m_req_core;
    m_activation.requested[1] = m_req_sync;
    m_activation.requested[2] = m_req_gbv;
    bool debug_ok  = false;
    bool debug1_ok = false;
    {
        const bool any_req = m_req_core || m_req_sync || m_req_gbv;
        // DIAG.7b(g): a scope requesting nothing must still turn an earlier scope's GBV off, or GBV instruments every
        // later device (measured: its descriptor-heap shadow memory exhausted a frame-graph heap -> driver fault).
        const bool reset_gbv = !m_req_gbv && state.gpu_based_validation;
        Microsoft::WRL::ComPtr<ID3D12Debug> debug;
        debug_ok = (any_req || reset_gbv) && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (debug_ok)
        {
            if ((m_req_core || m_req_gbv) && !state.enabled)
            {
                debug->EnableDebugLayer();
                state.enabled = true;
            }
            Microsoft::WRL::ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug.As(&debug1)))
            {
                debug1_ok = true;
                // Explicit set (not TRUE-only-if-requested) so GBV never leaks into a later context's device.
                if (state.gpu_based_validation != m_req_gbv)
                {
                    debug1->SetEnableGPUBasedValidation(m_req_gbv ? TRUE : FALSE);
                    state.gpu_based_validation = m_req_gbv;
                }
            }
        }
    }
    apply_dred_settings(state.dred, m_req_dred_breadcrumbs, m_req_dred_page_faults, m_dred);
    const HRESULT result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&output));
    if (SUCCEEDED(result) && (m_req_dred_breadcrumbs || m_req_dred_page_faults))
    {
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred_data;
        m_dred.readable = SUCCEEDED(output.As(&dred_data));
    }
    {
        const std::lock_guard record_lock(state.records_mutex);
        for (auto* capture = state.captures; capture != nullptr; capture = capture->next)
        {
            if (FAILED(result))
            {
                ++capture->report.device_creation_failures;
            }
            else
            {
                ++capture->report.contexts_started;
                ++capture->report.active_contexts;
            }
        }
    }
    if (FAILED(result))
    {
        return result;
    }
    m_created = true;
    // DIAG.7a(f-3): device created -> resolve per-mode activation. Core active is OBSERVABLE (ID3D12InfoQueue QI on this
    // device succeeds iff the debug layer is on for it -- the post-hoc oracle Vulkan lacks). GPU-assisted active == the
    // Debug1 enable ran pre-device (enabled, not proven instrumenting; (g) proves that). Sync has no D3D12 equivalent.
    {
        ValidationActivation& va = m_activation;
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> iq;
        const bool core_active = m_req_core && SUCCEEDED(output.As(&iq));
        va.active[0] = core_active;
        va.reason[0] = ValidationUnsupportedReason::NotRequested;
        if (m_req_core)
        {
            va.reason[0] = core_active ? ValidationUnsupportedReason::None : ValidationUnsupportedReason::LayerAbsent;
        }
        va.active[1] = false;
        va.reason[1] = m_req_sync ? ValidationUnsupportedReason::BackendHasNoEquivalent
                                  : ValidationUnsupportedReason::NotRequested;
        const bool gbv_active = m_req_gbv && debug_ok && debug1_ok && state.gpu_based_validation;
        va.active[2] = gbv_active;
        va.reason[2] = ValidationUnsupportedReason::NotRequested;
        if (m_req_gbv && !debug_ok)
        {
            va.reason[2] = ValidationUnsupportedReason::LayerAbsent;
        }
        else if (m_req_gbv && !debug1_ok)
        {
            va.reason[2] = ValidationUnsupportedReason::ExtensionAbsent;
        }
        else if (m_req_gbv)
        {
            va.reason[2] = gbv_active ? ValidationUnsupportedReason::None : ValidationUnsupportedReason::FeatureAbsent;
        }
    }
    m_ordinal = state.next_context++;
    ++state.live_contexts;
    if (!state.enabled)
    {
        return result;
    }
    for (u32 index = 0; index < kMaxDevices; ++index)
    {
        auto& device = state.devices[index];
        if (device.identity == output.Get())
        {
            ++device.references;
            m_slot = static_cast<i32>(index);
            return result;
        }
    }
    for (u32 index = 0; index < kMaxDevices; ++index)
    {
        auto& device = state.devices[index];
        if (device.references != 0U || device.quarantined)
        {
            continue;
        }
        device.identity = output.Get();
        device.references = 1;
        device.id = state.next_device++;
        m_slot = static_cast<i32>(index);
        const HRESULT query_result = output.As(&device.queue);
        if (FAILED(query_result))
        {
            instrumentation_failure(Dx12ValidationIssue::QueryInfoQueue, query_result);
            return result;
        }
        const HRESULT registration = device.queue->RegisterMessageCallback(receive_message,
            D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, &device, &device.cookie);
        if (FAILED(registration))
        {
            instrumentation_failure(Dx12ValidationIssue::RegisterCallback, registration);
        }
        replay_startup(device);
        return result;
    }
    instrumentation_failure(Dx12ValidationIssue::DeviceCapacity);
    return result;
}

detail::Dx12DredProcessState detail::dx12_dred_process_state() noexcept
{
    auto& state = registry();
    const std::lock_guard device_lock(state.devices_mutex);
    return state.dred;
}

bool detail::dx12_gbv_process_state() noexcept
{
    auto& state = registry();
    const std::lock_guard device_lock(state.devices_mutex);
    return state.gpu_based_validation;
}

detail::Dx12DeviceScope::~Dx12DeviceScope() noexcept
{
    if (!m_created)
    {
        return;
    }
    auto& state = registry();
    const std::lock_guard device_lock(state.devices_mutex);
    if (m_slot >= 0)
    {
        auto& device = state.devices[static_cast<u32>(m_slot)];
        if (--device.references == 0U)
        {
            // D3D guarantees this call waits for outstanding callbacks. No record lock is held across native calls.
            const HRESULT result = device.cookie != 0U ? device.queue->UnregisterMessageCallback(device.cookie) : S_OK;
            if (FAILED(result))
            {
                instrumentation_failure(Dx12ValidationIssue::UnregisterCallback, result);
                // Keep native ownership and callback storage stable after an uncertain deregistration. This bounded
                // quarantined slot cannot be reused, and later captures cannot silently forget the failed teardown.
                device.quarantined = true;
                state.registration_failed_to_retire = true;
            }
            else
            {
                device.queue.Reset();
                device.identity = nullptr;
                device.cookie = 0;
            }
        }
    }
    --state.live_contexts;
    const std::lock_guard record_lock(state.records_mutex);
    for (auto* capture = state.captures; capture != nullptr; capture = capture->next)
    {
        // A late observer did not witness this context's startup; preserve its failed readiness without underflow.
        if (m_ordinal >= capture->first_context)
        {
            --capture->report.active_contexts;
            ++capture->report.contexts_finished;
        }
    }
}
} // namespace crd::gpu
