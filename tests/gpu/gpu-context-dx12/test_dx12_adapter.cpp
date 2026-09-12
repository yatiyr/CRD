#include "dx12_adapter_classification.hpp"

#include <catch2/catch_test_macros.hpp>

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
