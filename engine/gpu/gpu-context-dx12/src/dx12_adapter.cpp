#include "dx12_adapter.hpp"

#include <cstring>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace crd::gpu::detail
{
GpuAdapter dx12_adapter(ID3D12Device* device) noexcept
{
    using Microsoft::WRL::ComPtr;
    GpuAdapter a{};
    if (device == nullptr)
    {
        return a;
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> dxgi;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetDesc1(&desc)))
    {
        return a;
    }
    std::memcpy(a.backend, "dx12", sizeof("dx12"));
    if (WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, a.name, sizeof(a.name), nullptr, nullptr) == 0)
    {
        a.name[0] = '\0';
    }
    a.vendor = desc.VendorId;
    a.device = desc.DeviceId;
    LARGE_INTEGER umd{};
    if (SUCCEEDED(dxgi->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
    {
        a.driver = static_cast<crd::u64>(umd.QuadPart);
        format_windows_driver_version(a.driver, a.driver_version);
    }
    a.known = true;
    return a;
}
} // namespace crd::gpu::detail
