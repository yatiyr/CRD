// CI census of the default D3D12 device used by Cerid's DX12 contexts.
#include <crd/core/types.hpp>
#include <crd/gpu/dx12_context.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>
#include <cstring>

#include <d3d12.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

namespace
{

// Independent census of the native query, with its exact failure/cleanup statuses visible in the CI log.
class KernelCensus
{
public:
    explicit KernelCensus(LUID luid)
    {
        m_adapter.AdapterLuid = luid;
        m_opened = D3DKMTOpenAdapterFromLuid(&m_adapter);
        std::printf("DX12 census: kernel_open=0x%08lx\n", static_cast<unsigned long>(m_opened));
    }

    ~KernelCensus() noexcept
    {
        if (m_opened >= 0)
        {
            D3DKMT_CLOSEADAPTER request{};
            request.hAdapter = m_adapter.hAdapter;
            const NTSTATUS result = D3DKMTCloseAdapter(&request);
            std::printf("DX12 census: kernel_close=0x%08lx\n", static_cast<unsigned long>(result));
        }
    }

    KernelCensus(const KernelCensus&) = delete;
    KernelCensus& operator=(const KernelCensus&) = delete;

    [[nodiscard]] bool query(bool& software, bool& render) const
    {
        if (m_opened < 0) { return false; }
        D3DKMT_ADAPTERTYPE type{};
        D3DKMT_QUERYADAPTERINFO request{};
        request.hAdapter = m_adapter.hAdapter;
        request.Type = KMTQAITYPE_ADAPTERTYPE;
        request.pPrivateDriverData = &type;
        request.PrivateDriverDataSize = sizeof(type);
        const NTSTATUS result = D3DKMTQueryAdapterInfo(&request);
        std::printf("DX12 census: kernel_query=0x%08lx flags=%08x software=%u render=%u\n",
                    static_cast<unsigned long>(result), type.Value, type.SoftwareDevice, type.RenderSupported);
        if (result < 0) { return false; }
        software = type.SoftwareDevice != 0U;
        render = type.RenderSupported != 0U;
        return true;
    }

private:
    D3DKMT_OPENADAPTERFROMLUID m_adapter{};
    NTSTATUS m_opened = 0;
};

} // namespace

int main()
{
    using Microsoft::WRL::ComPtr;
    crd::memory::TlsfAllocator alloc(1U << 20U);
    const auto context = crd::gpu::create_dx12_gpu_context(&alloc);
    if (!context || !context->valid())
    {
        std::puts("DX12 census: unavailable; this run does not qualify DX12 hardware.");
        return 77;
    }

    ComPtr<ID3D12Device> device;
    const HRESULT device_result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(device_result))
    {
        std::printf("DX12 census: default device query failed, hr=0x%08lx\n", static_cast<unsigned long>(device_result));
        return 1;
    }
    ComPtr<IDXGIFactory4> factory;
    const HRESULT factory_result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factory_result))
    {
        std::printf("DX12 census: factory query failed, hr=0x%08lx\n", static_cast<unsigned long>(factory_result));
        return 1;
    }
    const LUID luid = device->GetAdapterLuid();
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT adapter_result = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(adapter_result))
    {
        std::printf("DX12 census: adapter query failed, hr=0x%08lx\n", static_cast<unsigned long>(adapter_result));
        return 1;
    }
    DXGI_ADAPTER_DESC1 descriptor{};
    const HRESULT descriptor_result = adapter->GetDesc1(&descriptor);
    if (FAILED(descriptor_result))
    {
        std::printf("DX12 census: descriptor query failed, hr=0x%08lx\n",
                    static_cast<unsigned long>(descriptor_result));
        return 1;
    }
    char name[512]{};
    if (WideCharToMultiByte(CP_UTF8, 0, descriptor.Description, -1, name, static_cast<int>(sizeof(name)),
                            nullptr, nullptr) == 0)
    {
        std::puts("DX12 census: adapter name conversion failed.");
        return 1;
    }
    const bool software = (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U;
    const KernelCensus kernel(luid);
    bool kernel_software = false;
    bool kernel_render = false;
    const bool kernel_available = kernel.query(kernel_software, kernel_render);
    const bool basic_render = descriptor.VendorId == 0x1414U && descriptor.DeviceId == 0x008cU;
    std::printf("DX12 census: documented_basic_render=%d\n", static_cast<int>(basic_render));
    const auto engine_kind = crd::gpu::dx12_default_adapter_kind();
    const bool engine_software = crd::gpu::dx12_default_adapter_is_software();
    std::printf("DX12 census: adapter=\"%s\" engine_adapter=\"%s\" luid=%08lx:%08lx\n", name,
                context->adapter_name(), static_cast<unsigned long>(luid.HighPart), luid.LowPart);
    std::printf("DX12 census: vendor=%04x device=%04x revision=%u flags=%u software=%d engine_software=%d nodes=%u\n",
                descriptor.VendorId, descriptor.DeviceId, descriptor.Revision, descriptor.Flags,
                static_cast<int>(software), static_cast<int>(engine_software), device->GetNodeCount());
    std::printf("DX12 census: engine_kind=%u (0=unknown, 1=hardware, 2=software)\n",
                static_cast<unsigned int>(engine_kind));

    // Query the package UMD version with IDXGIDevice, not an unsupported D3D12 interface GUID.
    LARGE_INTEGER driver{};
    const HRESULT driver_result = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    if (SUCCEEDED(driver_result))
    {
        const auto version = static_cast<crd::u64>(driver.QuadPart);
        std::printf("DX12 census: driver=%u.%u.%u.%u\n", static_cast<unsigned int>((version >> 48U) & 0xffffU),
                    static_cast<unsigned int>((version >> 32U) & 0xffffU),
                    static_cast<unsigned int>((version >> 16U) & 0xffffU), static_cast<unsigned int>(version & 0xffffU));
    }
    else
    {
        std::printf("DX12 census: driver unavailable, hr=0x%08lx\n", static_cast<unsigned long>(driver_result));
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const HRESULT options_result = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    std::printf("DX12 census: options_hr=0x%08lx binding_tier=%u conservative_tier=%u rovs=%d\n",
                static_cast<unsigned long>(options_result), static_cast<unsigned int>(options.ResourceBindingTier),
                static_cast<unsigned int>(options.ConservativeRasterizationTier), static_cast<int>(options.ROVsSupported));
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 waves{};
    const HRESULT waves_result = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &waves, sizeof(waves));
    std::printf("DX12 census: waves_hr=0x%08lx wave_ops=%d wave_min=%u wave_max=%u\n",
                static_cast<unsigned long>(waves_result), static_cast<int>(waves.WaveOps),
                waves.WaveLaneCountMin, waves.WaveLaneCountMax);
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 ray_tracing{};
    const HRESULT rt_result = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &ray_tracing, sizeof(ray_tracing));
    std::printf("DX12 census: rt_hr=0x%08lx rt_tier=%u\n", static_cast<unsigned long>(rt_result),
                static_cast<unsigned int>(ray_tracing.RaytracingTier));
    // B1-f fallback evidence: SV_Barycentrics needs OPTIONS3.BarycentricsSupported and shader model 6.1. The
    // shader-model query rejects a model the runtime does not know, so probe from the newest known downwards.
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
    const HRESULT options3_result = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3));
    std::printf("DX12 census: options3_hr=0x%08lx barycentrics=%d\n", static_cast<unsigned long>(options3_result),
                static_cast<int>(options3.BarycentricsSupported));
    const D3D_SHADER_MODEL candidates[] = {D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
                                           D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1,
                                           D3D_SHADER_MODEL_6_0};
    HRESULT shader_model_result = E_FAIL;
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{};
    for (const D3D_SHADER_MODEL candidate : candidates)
    {
        shader_model.HighestShaderModel = candidate;
        shader_model_result = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model));
        if (SUCCEEDED(shader_model_result)) { break; }
    }
    std::printf("DX12 census: shader_model_hr=0x%08lx highest=0x%02x\n", static_cast<unsigned long>(shader_model_result),
                static_cast<unsigned int>(shader_model.HighestShaderModel));

    // Unknown feature queries retain their HRESULT; zero-initialized values alone do not mean unsupported.
    const bool expected_software = basic_render || (kernel_available && kernel_software) || software;
    const bool hardware_evidence = kernel_available && kernel_render && !kernel_software;
    if (std::strcmp(name, context->adapter_name()) != 0 || engine_kind == crd::gpu::Dx12AdapterKind::Unknown ||
        expected_software != engine_software ||
        (engine_kind == crd::gpu::Dx12AdapterKind::Hardware && !hardware_evidence) ||
        (engine_kind == crd::gpu::Dx12AdapterKind::Software) != engine_software)
    {
        std::puts("DX12 census: the engine/default adapter identity or classification disagrees.");
        return 1;
    }
    return 0;
}
