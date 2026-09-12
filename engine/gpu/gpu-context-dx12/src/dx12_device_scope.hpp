#pragma once

#include <crd/core/types.hpp>

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

private:
    i32 m_slot = -1;
    bool m_created = false;
    u64 m_ordinal = 0;
};
} // namespace crd::gpu::detail
