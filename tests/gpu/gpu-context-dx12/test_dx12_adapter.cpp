#include "dx12_adapter_classification.hpp"

#include <crd/gpu/adapter.hpp>
#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>

#include <dxgi1_4.h>
#include <wrl/client.h>

TEST_CASE("DX12 adapter classification requires positive execution evidence", "[dx12][adapter]")
{
    using crd::gpu::Dx12AdapterKind;
    using crd::gpu::detail::classify_dx12_adapter;

    // Software evidence survives missing DXGI flags. A negative kernel software bit alone is not hardware proof.
    CHECK(classify_dx12_adapter({true, false, true, true}) == Dx12AdapterKind::Software);
    CHECK(classify_dx12_adapter({true, true, true, true}) == Dx12AdapterKind::Software);
    CHECK(classify_dx12_adapter({true, false, true, false, true}) == Dx12AdapterKind::Hardware);
    CHECK(classify_dx12_adapter({false, false, true, false, true}) == Dx12AdapterKind::Hardware);
    CHECK(classify_dx12_adapter({false, false, true, true}) == Dx12AdapterKind::Software);

    // DXGI's negative flag and a failed kernel query never establish hardware. Positive software remains useful.
    CHECK(classify_dx12_adapter({true, false, false, false}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({true, true, false, false}) == Dx12AdapterKind::Software);
    CHECK(classify_dx12_adapter({}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({false, true, false, true}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({true, true, true, false, true}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({true, false, true, false, false}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({false, false, true, false, false}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({true, true, true, false, false}) == Dx12AdapterKind::Software);

    // Actual 7201 hosted census: flags 0, kernel 0x10a (display but not render), exact BasicRender identity.
    CHECK(classify_dx12_adapter({true, false, true, false, false, 0x1414U, 0x008cU}) == Dx12AdapterKind::Software);
    CHECK(classify_dx12_adapter({true, false, false, false, false, 0x1414U, 0x008cU}) == Dx12AdapterKind::Software);
    CHECK(classify_dx12_adapter({true, false, true, false, true, 0x1414U, 0x008cU}) == Dx12AdapterKind::Software);
    // Similar vendor/device, unavailable descriptor or an unrelated display-only adapter cannot inherit that rule.
    CHECK(classify_dx12_adapter({true, false, true, false, false, 0x1414U, 0x008dU}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({true, false, true, false, false, 0x10deU, 0x008cU}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({false, false, true, false, false, 0x1414U, 0x008cU}) == Dx12AdapterKind::Unknown);
    CHECK(classify_dx12_adapter({true, false, true, false, true, 0x1414U, 0x008dU}) == Dx12AdapterKind::Hardware);
}

TEST_CASE("DX12 adapter classification queries the explicit WARP adapter", "[dx12][adapter]")
{
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    Microsoft::WRL::ComPtr<IDXGIAdapter1> warp;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    DXGI_ADAPTER_DESC1 desc{};
    REQUIRE(SUCCEEDED(warp->GetDesc1(&desc)));
    // Exercise the production native query on the real software adapter without changing the default device.
    CHECK(crd::gpu::detail::query_dx12_adapter_kind(desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart) ==
          crd::gpu::Dx12AdapterKind::Software);
}

TEST_CASE("DX12 adapter classification keeps an unavailable identity unknown", "[dx12][adapter]")
{
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    const LUID absent{};
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    REQUIRE(factory->EnumAdapterByLuid(absent, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND);
    CHECK(crd::gpu::detail::query_dx12_adapter_kind(absent.LowPart, absent.HighPart) == crd::gpu::Dx12AdapterKind::Unknown);
}

namespace
{
// The driver version Windows itself records for the display adapter with these PCI ids: the DriverVersion of the
// display class's device entry whose MatchingDeviceId names them ("pci\ven_10de&dev_2705..."). Empty when none does
// (WARP has no PCI ids).
void windows_display_driver_version(crd::u32 vendor, crd::u32 device, char (&found)[64])
{
    found[0] = '\0';
    HKEY display = nullptr;
    const auto class_key = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_key, 0U, KEY_READ, &display) != ERROR_SUCCESS)
    {
        return;
    }
    wchar_t ids[64];
    (void)swprintf(ids, 64U, L"ven_%04x&dev_%04x", vendor, device);
    wchar_t sub[256];
    for (DWORD i = 0U; found[0] == '\0'; ++i)
    {
        DWORD sub_size = 256U;
        if (RegEnumKeyExW(display, i, sub, &sub_size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        {
            break;
        }
        wchar_t matching[512];
        DWORD matching_size = sizeof(matching);
        if (RegGetValueW(display, sub, L"MatchingDeviceId", RRF_RT_REG_SZ, nullptr, matching, &matching_size) !=
            ERROR_SUCCESS)
        {
            continue;
        }
        for (wchar_t* c = matching; *c != L'\0'; ++c)
        {
            *c = static_cast<wchar_t>(towlower(*c));
        }
        if (wcsstr(matching, ids) == nullptr)
        {
            continue;
        }
        wchar_t version[64];
        DWORD version_size = sizeof(version);
        if (RegGetValueW(display, sub, L"DriverVersion", RRF_RT_REG_SZ, nullptr, version, &version_size) ==
            ERROR_SUCCESS)
        {
            (void)std::snprintf(found, sizeof(found), "%ls", version);
        }
    }
    RegCloseKey(display);
}
} // namespace

// HW-RESILIENCE.2: the context reads its adapter once from DXGI, with the user-mode driver version Windows records for
// that adapter, and notes it in the process's opened-adapter log; the compute context reports the same adapter.
TEST_CASE("HW-RESILIENCE.2: the DX12 context reports its adapter as Windows records it and notes it as opened",
          "[dx12][adapter][diag][hw-resilience]")
{
    const std::unique_ptr<crd::gpu::IGpuContext> ctx = crd::gpu::create_dx12_gpu_context();
    if (ctx == nullptr || !ctx->valid())
    {
        SKIP("no D3D12 device available");
    }
    const crd::gpu::GpuAdapter a = ctx->adapter();
    REQUIRE(a.known);
    CHECK(std::strcmp(a.backend, "dx12") == 0);
    CHECK(std::strcmp(a.name, ctx->adapter_name()) == 0);
    REQUIRE(a.driver != 0U);
    char text[crd::gpu::kAdapterDriverVersionBytes];
    crd::gpu::format_windows_driver_version(a.driver, text);
    CHECK(std::strcmp(text, a.driver_version) == 0);
    char recorded[64];
    windows_display_driver_version(a.vendor, a.device, recorded);
    if (a.vendor != 0x1414U) // a hardware adapter: Windows' own record of its driver
    {
        CHECK(std::strcmp(recorded, a.driver_version) == 0);
    }
    else
    {
        CHECK(recorded[0] == '\0'); // WARP: no PCI device, no display class entry
    }

    bool noted = false;
    for (crd::u32 i = 0U; i < crd::gpu::opened_adapters().count(); ++i)
    {
        const crd::gpu::GpuAdapter o = crd::gpu::opened_adapters().at(i);
        noted = noted || (std::strcmp(o.backend, "dx12") == 0 && o.vendor == a.vendor && o.device == a.device &&
                          std::strcmp(o.driver_version, a.driver_version) == 0);
    }
    CHECK(noted);

    crd::memory::TlsfAllocator alloc(16U << 20U);
    crd::gpu::Dx12ComputeContext compute(&alloc);
    REQUIRE(compute.valid());
    const crd::gpu::GpuAdapter c = compute.adapter();
    CHECK(c.known);
    CHECK(std::strcmp(c.driver_version, a.driver_version) == 0);
    CHECK(c.device == a.device);
    std::printf("[HW-RESILIENCE.2] dx12 adapter: %s 0x%04x:0x%04x driver %s (Windows records %s)\n", a.name, a.vendor,
                a.device, a.driver_version, recorded[0] == '\0' ? "none" : recorded);
}
