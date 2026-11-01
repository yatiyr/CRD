#include "dx12_device_check.hpp"

#include "dx12_adapter.hpp"
#include "dx12_adapter_classification.hpp"

#include <crd/containers/string_view.hpp>
#include <crd/gpu/device_check.hpp>
#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_device_check.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/memory/allocator.hpp>

#include <dxgi1_6.h>
#include <memory>

namespace crd::gpu::detail
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr crd::u32 kMaxDx12Adapters = 16U; // DXGI's list, WARP included

struct Candidate
{
    ComPtr<IDXGIAdapter1> adapter;
    GpuAdapter identity;
    crd::u32 ordinal = 0U; // the place in DXGI's high-performance order; WARP, when DXGI does not list it, after it
    crd::u32 rank = 0U;    // hardware, unclassified, software, the separate WARP adapter
};

[[nodiscard]] crd::u32 adapter_rank(IDXGIAdapter1* adapter) noexcept
{
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)))
    {
        return 1U;
    }
    switch (query_dx12_adapter_kind(desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart))
    {
        case Dx12AdapterKind::Hardware:
            return 0U;
        case Dx12AdapterKind::Software:
            return 2U;
        default:
            return 1U;
    }
}

[[nodiscard]] bool same_luid(IDXGIAdapter1* a, IDXGIAdapter1* b) noexcept
{
    DXGI_ADAPTER_DESC1 da{};
    DXGI_ADAPTER_DESC1 db{};
    return SUCCEEDED(a->GetDesc1(&da)) && SUCCEEDED(b->GetDesc1(&db)) &&
           da.AdapterLuid.LowPart == db.AdapterLuid.LowPart && da.AdapterLuid.HighPart == db.AdapterLuid.HighPart;
}

// DXGI's adapters in high-performance order (the plain enumeration where the runtime has no IDXGIFactory6), then WARP
// unless one of them is it, each with its rank; returns how many `out` holds.
[[nodiscard]] crd::u32 list_adapters(Candidate (&out)[kMaxDx12Adapters]) noexcept
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return 0U;
    }
    ComPtr<IDXGIFactory6> by_preference;
    (void)factory.As(&by_preference);
    crd::u32 count = 0U;
    for (UINT i = 0U; count + 1U < kMaxDx12Adapters; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT listed = by_preference != nullptr
                                   ? by_preference->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                               IID_PPV_ARGS(&adapter))
                                   : factory->EnumAdapters1(i, &adapter);
        if (FAILED(listed))
        {
            break;
        }
        out[count].adapter = adapter;
        out[count].ordinal = count;
        out[count].rank = adapter_rank(adapter.Get());
        ++count;
    }
    ComPtr<IDXGIAdapter1> warp;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
    {
        bool listed = false;
        for (crd::u32 i = 0U; i < count; ++i)
        {
            if (same_luid(out[i].adapter.Get(), warp.Get()))
            {
                listed = true;
                break;
            }
        }
        if (!listed)
        {
            out[count].adapter = warp;
            out[count].ordinal = count;
            out[count].rank = 3U;
            ++count;
        }
    }
    // A stable insertion sort by rank keeps DXGI's order within a rank.
    for (crd::u32 i = 1U; i < count; ++i)
    {
        for (crd::u32 j = i; j > 0U && out[j - 1U].rank > out[j].rank; --j)
        {
            Candidate held = out[j];
            out[j] = out[j - 1U];
            out[j - 1U] = held;
        }
    }
    for (crd::u32 i = 0U; i < count; ++i)
    {
        out[i].identity = dx12_adapter(out[i].adapter.Get());
    }
    return count;
}

// The device check of the adapter `user` (an IDXGIAdapter1) on a probe compute context of its own, gone before the
// caller's device is created. The kernel is lowered and compiled on the host first: a host that cannot (no dxc)
// leaves the adapter unchecked, while a pipeline the driver refuses is the device's failure ("build").
DeviceCheckOutcome check_dx12_adapter(void* user)
{
    crd::memory::IAllocator* const alloc = crd::memory::default_allocator();
    crd::kir::KGraph graph(alloc);
    const crd::kir::KEntry entry = crd::kir::build_device_check_kernel(graph);
    crd::kir::GlslKernel kernel(alloc);
    if (!crd::kir::emit_compute_kernel_hlsl(graph, entry, alloc, kernel))
    {
        return DeviceCheckOutcome{nullptr, "the kernel does not lower to HLSL"};
    }
    const DxilCompileResult dxil =
        compile_hlsl_to_dxil(ShaderStage::Compute, crd::containers::to_view(kernel.source), "ckir_device_check", alloc);
    if (!dxil.ok)
    {
        return DeviceCheckOutcome{nullptr, "no HLSL to DXIL compile"};
    }
    Dx12ComputeContext probe(alloc, Dx12ProbeAdapter{static_cast<IDXGIAdapter1*>(user)});
    if (!probe.valid())
    {
        return DeviceCheckOutcome{"device", nullptr};
    }
    float input[crd::kir::kDeviceCheckLanes] = {};
    for (crd::u32 i = 0U; i < crd::kir::kDeviceCheckLanes; ++i)
    {
        input[i] = crd::kir::device_check_input(i);
    }
    const std::unique_ptr<ComputePipeline> pipeline = probe.create_pipeline_from_dxil(
        crd::containers::ConstSpan<crd::u8>(dxil.dxil.data(), dxil.dxil.size()), crd::kir::kDeviceCheckBindings, 0U);
    DeviceCheckProgram program;
    program.input = crd::containers::ConstSpan<float>(input, crd::kir::kDeviceCheckLanes);
    program.floats = crd::kir::kDeviceCheckFloats;
    program.words = crd::kir::kDeviceCheckWords;
    program.workgroups = crd::kir::kDeviceCheckGroups;
    program.verify = &crd::kir::device_check_mismatch;
    return DeviceCheckOutcome{run_device_check(probe, pipeline.get(), program), nullptr};
}
} // namespace

ComPtr<IDXGIAdapter1> dx12_checked_adapter()
{
    Candidate list[kMaxDx12Adapters];
    const crd::u32 count = list_adapters(list);
    if (count == 0U)
    {
        return nullptr; // nothing was chosen
    }
    DeviceSelection selection;
    selection.selected = list[0].identity;
    for (crd::u32 i = 0U; i < count; ++i)
    {
        const DeviceCheck check =
            check_device_once(list[i].identity, list[i].ordinal, &check_dx12_adapter, list[i].adapter.Get());
        if (check.failed != nullptr)
        {
            if (i == 0U)
            {
                selection.failed = check.failed;
                selection.injected = check.injected;
            }
            ++selection.adapters_failed;
            continue;
        }
        const DeviceCheckStatus first =
            check.unchecked != nullptr ? DeviceCheckStatus::Unchecked : DeviceCheckStatus::Passed;
        selection.status = i == 0U ? first : DeviceCheckStatus::Demoted;
        selection.runs = list[i].identity;
        selection.unchecked = check.unchecked;
        device_checks().note_selection(selection);
        return list[i].adapter;
    }
    selection.status = DeviceCheckStatus::NoDevicePassed;
    device_checks().note_selection(selection);
    return nullptr;
}
} // namespace crd::gpu::detail
