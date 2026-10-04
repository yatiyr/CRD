#pragma once

#include <crd/core/types.hpp>
#include <crd/gpu/validation.hpp> // DIAG.7a(f-3): ValidationActivation

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl/client.h>

namespace crd::gpu::detail
{
// DIAG.7b(b): DRED (device-removed extended data) setup for one device creation. DRED is device-removal diagnostics,
// not a validation mode, so it stays DX12-local instead of becoming a fourth common ValidationMode. Like GBV the
// settings are process-global and must precede device creation; create() sets them explicitly (forced on when
// requested, forced off when an earlier scope left them on) so no device inherits another scope's breadcrumbs.
enum class Dx12DredSetup : u8
{
    NotRequested,   // this scope did not ask for the feature
    Set,            // the DRED settings interface accepted it before device creation
    SettingsAbsent, // the runtime exposes no DRED settings interface
};

// The creation-time half of DRED capability. Whether a query returns data is only known after a removal (DIAG.7b(c)).
struct Dx12DredActivation
{
    Dx12DredSetup breadcrumbs = Dx12DredSetup::NotRequested;
    Dx12DredSetup page_faults = Dx12DredSetup::NotRequested;
    bool          breadcrumb_contexts = false; // Settings1 present: per-op context strings recorded with breadcrumbs
    bool          readable            = false; // the device exposes ID3D12DeviceRemovedExtendedData1 for the read
};

// The process-global DRED settings as last applied (test observability for the no-inheritance contract).
struct Dx12DredProcessState
{
    bool breadcrumbs = false;
    bool page_faults = false;
    bool breadcrumb_contexts = false;
};
[[nodiscard]] Dx12DredProcessState dx12_dred_process_state() noexcept;

// Declare BEFORE the context's device/resources: unregister only after their destruction. One registration per
// live native device, including shared-device contexts. Device creation and debug enablement are serialized.
class Dx12DeviceScope final
{
public:
    Dx12DeviceScope() = default;
    ~Dx12DeviceScope() noexcept;
    Dx12DeviceScope(const Dx12DeviceScope&) = delete;
    Dx12DeviceScope& operator=(const Dx12DeviceScope&) = delete;
    [[nodiscard]] HRESULT create(Microsoft::WRL::ComPtr<ID3D12Device>& output, IUnknown* adapter = nullptr) noexcept;
    // DIAG.7a(f-3): record requested validation modes BEFORE create(); create() applies the process-global debug
    // layer + GPU-based-validation transition (under devices_mutex) and fills the activation report read here after.
    void request_validation(bool core, bool sync, bool gpu_based) noexcept
    {
        m_req_core = core;
        m_req_sync = sync;
        m_req_gbv  = gpu_based;
    }
    [[nodiscard]] ValidationActivation activation() const noexcept { return m_activation; }
    // DIAG.7b(b): record the requested DRED features BEFORE create(); read the creation-time outcome after it.
    void request_dred(bool breadcrumbs, bool page_faults) noexcept
    {
        m_req_dred_breadcrumbs = breadcrumbs;
        m_req_dred_page_faults = page_faults;
    }
    [[nodiscard]] Dx12DredActivation dred() const noexcept { return m_dred; }

private:
    i32 m_slot = -1;
    bool m_created = false;
    u64 m_ordinal = 0;
    bool m_req_core = false;
    bool m_req_sync = false;
    bool m_req_gbv = false;
    ValidationActivation m_activation{};
    bool m_req_dred_breadcrumbs = false;
    bool m_req_dred_page_faults = false;
    Dx12DredActivation m_dred{};
};
} // namespace crd::gpu::detail
