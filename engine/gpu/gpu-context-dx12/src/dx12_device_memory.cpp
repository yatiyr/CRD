#include "dx12_device_memory.hpp"

#include <dxgi1_4.h>
#include <wrl/client.h>

namespace crd::gpu::detail
{
namespace
{
BudgetReading segment_reading(const DXGI_QUERY_VIDEO_MEMORY_INFO& info) noexcept
{
    BudgetReading r;
    r.read     = true;
    r.bounded  = true;
    r.capacity = info.Budget;
    r.in_use   = info.CurrentUsage;
    return r;
}
} // namespace

DeviceMemoryReading dx12_device_memory(ID3D12Device* device) noexcept
{
    DeviceMemoryReading out;
    if (device == nullptr)
    {
        out.reason = "no D3D12 device";
        return out;
    }
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        out.reason = "CreateDXGIFactory1 failed";
        return out;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) || adapter == nullptr)
    {
        out.reason = "DXGI has no IDXGIAdapter3 for the device's adapter";
        return out;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO non_local{};
    const bool local_ok = SUCCEEDED(adapter->QueryVideoMemoryInfo(0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
    const bool non_local_ok =
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0U, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local));
    if (!local_ok && !non_local_ok)
    {
        out.reason = "QueryVideoMemoryInfo failed";
        return out;
    }
    out.source = DeviceMemorySource::Dxgi;
    out.reason = local_ok && non_local_ok ? "" : "QueryVideoMemoryInfo failed for one segment group";
    // An adapter without a segment group (WARP has no non-local one) answers a zero budget and zero use: that is an
    // absent segment, not a full one, so it stays unread.
    if (local_ok && (local.Budget != 0U || local.CurrentUsage != 0U))
    {
        out.local = segment_reading(local);
    }
    else if (local_ok)
    {
        out.reason = "the adapter has no local segment group";
    }
    if (non_local_ok && (non_local.Budget != 0U || non_local.CurrentUsage != 0U))
    {
        out.non_local = segment_reading(non_local);
    }
    else if (non_local_ok && out.reason[0] == '\0')
    {
        out.reason = "the adapter has no non-local segment group";
    }
    if (!out.local.read && !out.non_local.read)
    {
        out.source = DeviceMemorySource::Unavailable;
        out.reason = "the adapter reports no segment group";
    }
    return out;
}
} // namespace crd::gpu::detail
