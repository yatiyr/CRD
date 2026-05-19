// ---------------------------------------------------------------------------
// crd::shader::compile_hlsl — HLSL → SPIR-V via dxc. v9e-c-dxc-spirv-dispatch
// (2026-05-19).
//
// Loads dxcompiler dynamically (Vulkan SDK ships it next to shaderc_shared)
// and uses its `-spirv` target to produce Vulkan-consumable SPIR-V. Same
// runtime-compile shape as `compile_glsl`; mirrors its dynamic-load pattern.
//
// dxc API is COM-style (IDxcCompiler3, IDxcUtils, IDxcResult…). We manage
// refcounts explicitly via QueryInterface/Release rather than pulling in a
// CComPtr — keeps the dependency footprint minimal and the lifetime visible.
// ---------------------------------------------------------------------------

#include <crd/shader/compile.hpp>

#include <crd/core/platform.hpp>

#if CRD_HAS_DXC

#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/filesystem.hpp>

#if CRD_OS_WINDOWS
// dxcapi.h on Windows expects the consumer to have already pulled in the
// Windows COM types (UINT32, LPCWSTR, IUnknown, …). windows.h provides the
// width types; unknwn.h adds the full IUnknown definition (AddRef/Release/
// QueryInterface) which the IDxc* COM hierarchy needs to inherit.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#endif

#include <dxc/dxcapi.h>

#include <cstdlib>

namespace fs = crd::platform::fs;

namespace crd::shader
{
namespace
{

[[nodiscard]] crd::platform::DynamicLibrary try_open_dxc() noexcept
{
#if CRD_OS_WINDOWS
    char*       sdk = nullptr;
    std::size_t len = 0;
    const errno_t rc = _dupenv_s(&sdk, &len, "VULKAN_SDK");
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
        auto lib = crd::platform::DynamicLibrary::open(candidates[i], /*log_on_failure=*/last);
        if (lib.is_valid()) { return lib; }
    }
    return crd::platform::DynamicLibrary{};
}

[[nodiscard]] const wchar_t* target_profile(Stage stage) noexcept
{
    switch (stage)
    {
        case Stage::Vertex:   return L"vs_6_0";
        case Stage::Fragment: return L"ps_6_0";
        case Stage::Compute:
        default:              return L"cs_6_0";
    }
}

[[nodiscard]] const wchar_t* default_entry(Stage stage) noexcept
{
    // For compute we default to `cs_main` (matches what the geometry-shader-
    // helpers HLSL conformance shader emits). Vertex/Fragment use `main`.
    return (stage == Stage::Compute) ? L"cs_main" : L"main";
}

} // namespace

CompileResult compile_hlsl(Stage stage, crd::containers::StringView source,
                            crd::containers::StringView /*name*/,
                            crd::memory::IAllocator* a)
{
    CompileResult result(a);

    // Lazy-init the dxc library + DxcCreateInstance entry point. Cached for
    // process lifetime — dxcompiler.dll has its own internal singletons.
    static crd::platform::DynamicLibrary s_lib       = try_open_dxc();
    static DxcCreateInstanceProc          s_create   = s_lib.is_valid()
        ? s_lib.resolve_as<DxcCreateInstanceProc>("DxcCreateInstance")
        : nullptr;

    if (!s_lib.is_valid() || s_create == nullptr)
    {
        result.error_message = crd::containers::String(
            "dxcompiler not loaded (VULKAN_SDK/Bin/dxcompiler.dll missing?)", a);
        return result;
    }

    // ---- Compiler + Utils -------------------------------------------------
    IDxcCompiler3* compiler = nullptr;
    HRESULT        hr       = s_create(CLSID_DxcCompiler,
                                        __uuidof(IDxcCompiler3),
                                        reinterpret_cast<void**>(&compiler));
    if (FAILED(hr) || compiler == nullptr)
    {
        result.error_message = crd::containers::String("dxc: CLSID_DxcCompiler failed", a);
        return result;
    }

    // We don't strictly need IDxcUtils for the minimal compile, but the
    // create succeeded path is documented to require it for include handling;
    // we skip include handling for now (no #include in our emitted HLSL).
    // Leave it out — Compile() with a nullptr include handler works for
    // include-less sources.

    // ---- Source buffer + args --------------------------------------------
    DxcBuffer src_buf{};
    src_buf.Ptr      = source.data();
    src_buf.Size     = source.size();
    src_buf.Encoding = DXC_CP_UTF8;

    const wchar_t* args[] = {
        L"-T",  target_profile(stage),
        L"-E",  default_entry(stage),
        L"-spirv",                       // target SPIR-V (Vulkan), not DXIL
        L"-fspv-target-env=vulkan1.3",   // match shaderc's target env
    };

    // ---- Compile ----------------------------------------------------------
    IDxcResult* dxc_result = nullptr;
    hr = compiler->Compile(&src_buf, args, static_cast<UINT32>(sizeof(args) / sizeof(args[0])),
                            /*include handler*/ nullptr,
                            __uuidof(IDxcResult),
                            reinterpret_cast<void**>(&dxc_result));
    if (FAILED(hr) || dxc_result == nullptr)
    {
        if (dxc_result != nullptr) { dxc_result->Release(); }
        compiler->Release();
        result.error_message = crd::containers::String("dxc: Compile() failed", a);
        return result;
    }

    // ---- Status + error blob ---------------------------------------------
    HRESULT compile_status = E_FAIL;
    dxc_result->GetStatus(&compile_status);
    if (FAILED(compile_status))
    {
        IDxcBlobUtf8* errors = nullptr;
        const HRESULT got_errors = dxc_result->GetOutput(
            DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8),
            reinterpret_cast<void**>(&errors), nullptr);
        if (SUCCEEDED(got_errors) && errors != nullptr && errors->GetStringLength() > 0U)
        {
            result.error_message = crd::containers::String(
                crd::containers::StringView(errors->GetStringPointer(), errors->GetStringLength()),
                a);
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

    // ---- SPIR-V output ----------------------------------------------------
    IDxcBlob* spirv_blob = nullptr;
    hr = dxc_result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob),
                                reinterpret_cast<void**>(&spirv_blob), nullptr);
    if (FAILED(hr) || spirv_blob == nullptr || spirv_blob->GetBufferSize() == 0U)
    {
        if (spirv_blob != nullptr) { spirv_blob->Release(); }
        dxc_result->Release();
        compiler->Release();
        result.error_message = crd::containers::String("dxc: compile succeeded but no SPIR-V output", a);
        return result;
    }

    // Copy SPIR-V bytes out of the COM blob into the caller's allocator.
    const auto*       spirv_bytes = static_cast<const crd::u8*>(spirv_blob->GetBufferPointer());
    const crd::usize  spirv_size  = static_cast<crd::usize>(spirv_blob->GetBufferSize());
    result.spirv.resize(spirv_size);
    for (crd::usize i = 0U; i < spirv_size; ++i) { result.spirv[i] = spirv_bytes[i]; }
    result.ok = true;

    spirv_blob->Release();
    dxc_result->Release();
    compiler->Release();
    return result;
}

} // namespace crd::shader

#else // !CRD_HAS_DXC

// Stub: dxc headers were not present at build time. Match the documented
// graceful-failure contract — return CompileResult{ok=false, error_message=…}
// instead of breaking the build on Linux configs without dxc-dev installed.
// Consumers (HLSL conformance test) detect ok=false and SKIP rather than fail.

#include <crd/containers/string.hpp>

namespace crd::shader
{

CompileResult compile_hlsl(Stage /*stage*/,
                            crd::containers::StringView /*source*/,
                            crd::containers::StringView /*name*/,
                            crd::memory::IAllocator* a)
{
    CompileResult result(a);
    result.error_message = crd::containers::String(
        "compile_hlsl: dxc headers not available at build time "
        "(CRD_HAS_DXC=0); install VULKAN_SDK with dxc or libdxc-dev",
        a);
    return result;
}

} // namespace crd::shader

#endif // CRD_HAS_DXC
