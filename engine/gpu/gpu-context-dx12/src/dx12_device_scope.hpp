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

private:
    i32 m_slot = -1;
    bool m_created = false;
    u64 m_ordinal = 0;
    bool m_req_core = false;
    bool m_req_sync = false;
    bool m_req_gbv = false;
    ValidationActivation m_activation{};
};
} // namespace crd::gpu::detail
