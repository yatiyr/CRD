#include "dx12_adapter.hpp"

#include <crd/core/utf16.hpp>

#include <cstring>
#include <cwchar>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace crd::gpu::detail
{
GpuAdapter dx12_adapter(IDXGIAdapter1* dxgi) noexcept
{
    GpuAdapter a{};
    DXGI_ADAPTER_DESC1 desc{};
    if (dxgi == nullptr || FAILED(dxgi->GetDesc1(&desc)))
    {
        return a;
    }
    std::memcpy(a.backend, "dx12", sizeof("dx12"));
    // The description as UTF-8: the whole code points that fit before the last byte, which stays the NUL of the
    // value-initialised name.
    (void)crd::utf16_to_utf8(desc.Description, std::wcslen(desc.Description), reinterpret_cast<crd::u8*>(a.name),
                             sizeof(a.name) - 1U);
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

GpuAdapter dx12_adapter(ID3D12Device* device) noexcept
{
    using Microsoft::WRL::ComPtr;
    if (device == nullptr)
    {
        return GpuAdapter{};
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> dxgi;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&dxgi))))
    {
        return GpuAdapter{};
    }
    return dx12_adapter(dxgi.Get());
}
} // namespace crd::gpu::detail
