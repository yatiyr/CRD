// vulkan_hlsl_compile.cpp — HLSL → SPIR-V via dxc, the Vulkan backend's PRIVATE compiler (ADR-0103). Relocated from
// crd-shader's compile_hlsl.cpp. dxc's `-spirv` target produces Vulkan-consumable SPIR-V (this is HLSL-as-a-source-for-
// Vulkan; DX12's HLSL→DXIL compiler lives in the dx12 backend). Dynamic dxc load; refcounts managed explicitly.

#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/core/platform.hpp>

#if CRD_HAS_DXC

#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/filesystem.hpp>

#if CRD_OS_WINDOWS
// dxcapi.h on Windows expects the Windows COM types already pulled in: windows.h provides the width types; unknwn.h
// adds the full IUnknown definition the IDxc* COM hierarchy inherits.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#endif

#include <dxc/dxcapi.h>

#include <cstdlib>

namespace fs = crd::platform::fs;

namespace crd::gpu
{
namespace
{

[[nodiscard]] crd::platform::DynamicLibrary try_open_dxc() noexcept
{
#if CRD_OS_WINDOWS
    char*         sdk = nullptr;
    std::size_t   len = 0;
    const errno_t rc  = _dupenv_s(&sdk, &len, "VULKAN_SDK");
    if (rc == 0 && sdk != nullptr && sdk[0] != '\0')
    {
        fs::Path sdk_path = fs::Path(sdk) / "Bin" / "dxcompiler.dll";
        std::free(sdk);
        auto lib = crd::platform::DynamicLibrary::open(sdk_path, /*log_on_failure=*/false);
        if (lib.is_valid()) { return lib; }
    }
    else
    {
        std::free(sdk);
    }
    const fs::Path candidates[] = {fs::Path("dxcompiler.dll")};
#elif CRD_OS_LINUX
    const fs::Path candidates[] = {
        fs::Path("libdxcompiler.so"),
        fs::Path("libdxcompiler.so.3"),
    };
#else
    const fs::Path candidates[] = {fs::Path("libdxcompiler.dylib")};
#endif
    const std::size_t n = sizeof(candidates) / sizeof(candidates[0]);
    for (std::size_t i = 0; i < n; ++i)
    {
        const bool last = (i + 1 == n);
        auto       lib  = crd::platform::DynamicLibrary::open(candidates[i], /*log_on_failure=*/last);
        if (lib.is_valid()) { return lib; }
    }
    return crd::platform::DynamicLibrary{};
}

[[nodiscard]] const wchar_t* target_profile(ShaderStage stage) noexcept
{
    switch (stage)
    {
    case ShaderStage::Vertex: return L"vs_6_0";
    case ShaderStage::Fragment: return L"ps_6_0";
    case ShaderStage::Mesh: return L"ms_6_5"; // B4: mesh shaders need SM 6.5
    case ShaderStage::Task: return L"as_6_5"; // B4: amplification (task) shaders
    case ShaderStage::Compute:
    default: return L"cs_6_0";
    }
}

[[nodiscard]] const wchar_t* default_entry(ShaderStage stage) noexcept
{
    // Compute defaults to `cs_main` (what the geometry-shader-helpers HLSL conformance shader emits); Vertex/Fragment
    // use `main`.
    return (stage == ShaderStage::Compute) ? L"cs_main" : L"main";
}

} // namespace

ShaderCompileResult compile_hlsl_to_spirv(ShaderStage stage, crd::containers::StringView source,
                                          crd::containers::StringView /*name*/, crd::memory::IAllocator* a)
{
    ShaderCompileResult result(a);

    // Lazy-init the dxc library + DxcCreateInstance entry point. Cached for process lifetime — dxcompiler.dll has its
    // own internal singletons.
    static crd::platform::DynamicLibrary s_lib    = try_open_dxc();
    static DxcCreateInstanceProc         s_create = s_lib.is_valid()
                                                        ? s_lib.resolve_as<DxcCreateInstanceProc>("DxcCreateInstance")
                                                        : nullptr;

    if (!s_lib.is_valid() || s_create == nullptr)
    {
        result.error_message = crd::containers::String("dxcompiler not loaded (VULKAN_SDK/Bin/dxcompiler.dll missing?)", a);
        return result;
    }

    IDxcCompiler3* compiler = nullptr;
    HRESULT hr = s_create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&compiler));
    if (FAILED(hr) || compiler == nullptr)
    {
        result.error_message = crd::containers::String("dxc: CLSID_DxcCompiler failed", a);
        return result;
    }

    DxcBuffer src_buf{};
    src_buf.Ptr      = source.data();
    src_buf.Size     = source.size();
    src_buf.Encoding = DXC_CP_UTF8;

    const wchar_t* args[] = {
        L"-T", target_profile(stage),
        L"-E", default_entry(stage),
        L"-spirv",                        // target SPIR-V (Vulkan), not DXIL
        L"-fspv-target-env=vulkan1.3",    // match shaderc's target env
        // D-008 C2-d4: normalize the SPIR-V OpEntryPoint to the canonical `main` regardless of the HLSL function name
        // (`cs_main`, the cross-backend convention). Every Vulkan program we mint then entry-points at `main`, so a
        // consumer's pipeline never needs the source function name — matching the GLSL/IR path (ADR-0103).
        L"-fspv-entrypoint-name=main",
    };

    IDxcResult* dxc_result = nullptr;
    hr = compiler->Compile(&src_buf, args, static_cast<UINT32>(sizeof(args) / sizeof(args[0])),
                           /*include handler*/ nullptr, __uuidof(IDxcResult), reinterpret_cast<void**>(&dxc_result));
    if (FAILED(hr) || dxc_result == nullptr)
    {
        if (dxc_result != nullptr) { dxc_result->Release(); }
        compiler->Release();
        result.error_message = crd::containers::String("dxc: Compile() failed", a);
        return result;
    }

    HRESULT compile_status = E_FAIL;
    dxc_result->GetStatus(&compile_status);
    if (FAILED(compile_status))
    {
        IDxcBlobUtf8* errors = nullptr;
        const HRESULT got_errors =
            dxc_result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&errors), nullptr);
        if (SUCCEEDED(got_errors) && errors != nullptr && errors->GetStringLength() > 0U)
        {
            result.error_message = crd::containers::String(
                crd::containers::StringView(errors->GetStringPointer(), errors->GetStringLength()), a);
        }
        else
        {
            result.error_message = crd::containers::String("dxc: compile failed (no error blob)", a);
        }
        if (errors != nullptr) { errors->Release(); }
        dxc_result->Release();
        compiler->Release();
        return result;
    }

    IDxcBlob* spirv_blob = nullptr;
    hr = dxc_result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&spirv_blob), nullptr);
    if (FAILED(hr) || spirv_blob == nullptr || spirv_blob->GetBufferSize() == 0U)
    {
        if (spirv_blob != nullptr) { spirv_blob->Release(); }
        dxc_result->Release();
        compiler->Release();
        result.error_message = crd::containers::String("dxc: compile succeeded but no SPIR-V output", a);
        return result;
    }

    const auto*      spirv_bytes = static_cast<const crd::u8*>(spirv_blob->GetBufferPointer());
    const crd::usize spirv_size  = static_cast<crd::usize>(spirv_blob->GetBufferSize());
    result.spirv.resize(spirv_size);
    for (crd::usize i = 0U; i < spirv_size; ++i) { result.spirv[i] = spirv_bytes[i]; }
    result.ok = true;

    spirv_blob->Release();
    dxc_result->Release();
    compiler->Release();
    return result;
}

} // namespace crd::gpu

#else // !CRD_HAS_DXC

// Stub: dxc headers were not present at build time. Match the graceful-failure contract — `ok == false` — instead of
// breaking the build on Linux configs without dxc-dev. Consumers (HLSL conformance test) detect it and SKIP.

#include <crd/containers/string.hpp>

namespace crd::gpu
{

ShaderCompileResult compile_hlsl_to_spirv(ShaderStage /*stage*/, crd::containers::StringView /*source*/,
                                          crd::containers::StringView /*name*/, crd::memory::IAllocator* a)
{
    ShaderCompileResult result(a);
    result.error_message = crd::containers::String(
        "compile_hlsl_to_spirv: dxc headers not available at build time (CRD_HAS_DXC=0); install VULKAN_SDK with dxc or "
        "libdxc-dev",
        a);
    return result;
}

} // namespace crd::gpu

#endif // CRD_HAS_DXC
