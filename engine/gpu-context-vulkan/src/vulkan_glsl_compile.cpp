// vulkan_glsl_compile.cpp — GLSL → SPIR-V via shaderc, the Vulkan backend's PRIVATE compiler (ADR-0103). Relocated
// verbatim from crd-shader's compile.cpp so no portable module owns a shading-language compiler. Dynamic shaderc load:
// a missing library yields ok == false, never a link-time dependency.

#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/filesystem.hpp>

#include <shaderc/shaderc.h>

#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::gpu
{
namespace
{

struct ShadercApi
{
    using CompilerInitialize  = shaderc_compiler_t (*)();
    using CompilerRelease     = void (*)(shaderc_compiler_t);
    using OptionsInitialize   = shaderc_compile_options_t (*)();
    using OptionsRelease      = void (*)(shaderc_compile_options_t);
    using OptionsSetTargetEnv = void (*)(shaderc_compile_options_t, shaderc_target_env, shaderc_env_version);
    using OptionsSetTargetSpv = void (*)(shaderc_compile_options_t, shaderc_spirv_version);
    using OptionsSetOptLevel  = void (*)(shaderc_compile_options_t, shaderc_optimization_level);
    using CompileIntoSpv      = shaderc_compilation_result_t (*)(shaderc_compiler_t, const char*, size_t,
                                                                 shaderc_shader_kind, const char*, const char*,
                                                                 const shaderc_compile_options_t);
    using ResultRelease       = void (*)(shaderc_compilation_result_t);
    using ResultStatus        = shaderc_compilation_status (*)(const shaderc_compilation_result_t);
    using ResultError         = const char* (*)(const shaderc_compilation_result_t);
    using ResultBytes         = const char* (*)(const shaderc_compilation_result_t);
    using ResultLength        = size_t (*)(const shaderc_compilation_result_t);

    CompilerInitialize  compiler_initialize    = nullptr;
    CompilerRelease     compiler_release       = nullptr;
    OptionsInitialize   options_initialize     = nullptr;
    OptionsRelease      options_release        = nullptr;
    OptionsSetTargetEnv options_set_target_env = nullptr;
    OptionsSetTargetSpv options_set_target_spv = nullptr;
    OptionsSetOptLevel  options_set_opt_level  = nullptr; // optional: spirv-opt performance passes
    CompileIntoSpv      compile_into_spv       = nullptr;
    ResultRelease       result_release         = nullptr;
    ResultStatus        result_status          = nullptr;
    ResultError         result_error           = nullptr;
    ResultBytes         result_bytes           = nullptr;
    ResultLength        result_length          = nullptr;
};

// shaderc ships under different SONAMEs (Vulkan SDK: shaderc_shared; distro: libshaderc.so.1; …). Probe a small
// per-OS candidate list so the engine works on SDK-installed and distro-packaged shaderc without manual symlinks.
[[nodiscard]] crd::platform::DynamicLibrary try_open_shaderc() noexcept
{
#if CRD_OS_WINDOWS
    char*         sdk = nullptr;
    std::size_t   len = 0;
    const errno_t rc  = _dupenv_s(&sdk, &len, "VULKAN_SDK");
    if (rc == 0 && sdk != nullptr && sdk[0] != '\0')
    {
        fs::Path sdk_path = fs::Path(sdk) / "Bin" / "shaderc_shared.dll";
        free(sdk);
        auto lib = crd::platform::DynamicLibrary::open(sdk_path, /*log_on_failure=*/false);
        if (lib.is_valid()) { return lib; }
    }
    else
    {
        free(sdk);
    }
    const fs::Path candidates[] = {fs::Path("shaderc_shared.dll")};
#elif CRD_OS_LINUX
    const fs::Path candidates[] = {
        fs::Path("libshaderc_shared.so"), // Vulkan SDK convention
        fs::Path("libshaderc.so.1"),      // Ubuntu / Debian (libshaderc1 package)
        fs::Path("libshaderc.so"),        // Ubuntu / Debian dev symlink
    };
#else
    const fs::Path candidates[] = {
        fs::Path("libshaderc_shared.dylib"),
        fs::Path("libshaderc.dylib"),
    };
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

[[nodiscard]] shaderc_shader_kind to_shaderc_kind(ShaderStage stage) noexcept
{
    switch (stage)
    {
    case ShaderStage::Vertex: return shaderc_vertex_shader;
    case ShaderStage::Fragment: return shaderc_fragment_shader;
    case ShaderStage::Mesh: return shaderc_mesh_shader; // B4: modern amplification path (needs SPIR-V ≥1.4 — we target 1.6)
    case ShaderStage::Task: return shaderc_task_shader;
    case ShaderStage::TessControl: return shaderc_tess_control_shader; // B4-tess: hull
    case ShaderStage::TessEval: return shaderc_tess_evaluation_shader; // B4-tess: domain
    case ShaderStage::RayGen: return shaderc_raygen_shader;            // FA-2: RT pipeline (needs SPIR-V ≥1.4 — we target 1.6)
    case ShaderStage::Intersection: return shaderc_intersection_shader;
    case ShaderStage::AnyHit: return shaderc_anyhit_shader;
    case ShaderStage::ClosestHit: return shaderc_closesthit_shader;
    case ShaderStage::Miss: return shaderc_miss_shader;
    case ShaderStage::Callable: return shaderc_callable_shader;
    case ShaderStage::Compute:
    default: return shaderc_compute_shader;
    }
}

class ShadercLoader
{
public:
    ShadercLoader()
    {
        m_lib = try_open_shaderc();
        if (!m_lib.is_valid()) { return; }

        m_api.compiler_initialize    = m_lib.resolve_as<ShadercApi::CompilerInitialize>("shaderc_compiler_initialize");
        m_api.compiler_release       = m_lib.resolve_as<ShadercApi::CompilerRelease>("shaderc_compiler_release");
        m_api.options_initialize     = m_lib.resolve_as<ShadercApi::OptionsInitialize>("shaderc_compile_options_initialize");
        m_api.options_release        = m_lib.resolve_as<ShadercApi::OptionsRelease>("shaderc_compile_options_release");
        m_api.options_set_target_env = m_lib.resolve_as<ShadercApi::OptionsSetTargetEnv>("shaderc_compile_options_set_target_env");
        m_api.options_set_target_spv = m_lib.resolve_as<ShadercApi::OptionsSetTargetSpv>("shaderc_compile_options_set_target_spirv");
        m_api.options_set_opt_level  = m_lib.resolve_as<ShadercApi::OptionsSetOptLevel>("shaderc_compile_options_set_optimization_level"); // optional
        m_api.compile_into_spv       = m_lib.resolve_as<ShadercApi::CompileIntoSpv>("shaderc_compile_into_spv");
        m_api.result_release         = m_lib.resolve_as<ShadercApi::ResultRelease>("shaderc_result_release");
        m_api.result_status          = m_lib.resolve_as<ShadercApi::ResultStatus>("shaderc_result_get_compilation_status");
        m_api.result_error           = m_lib.resolve_as<ShadercApi::ResultError>("shaderc_result_get_error_message");
        m_api.result_bytes           = m_lib.resolve_as<ShadercApi::ResultBytes>("shaderc_result_get_bytes");
        m_api.result_length          = m_lib.resolve_as<ShadercApi::ResultLength>("shaderc_result_get_length");

        if (m_api.compiler_initialize == nullptr || m_api.compiler_release == nullptr
            || m_api.options_initialize == nullptr || m_api.options_release == nullptr
            || m_api.options_set_target_env == nullptr || m_api.options_set_target_spv == nullptr
            || m_api.compile_into_spv == nullptr || m_api.result_release == nullptr || m_api.result_status == nullptr
            || m_api.result_error == nullptr || m_api.result_bytes == nullptr || m_api.result_length == nullptr)
        {
            m_lib = {};
            return;
        }

        m_compiler = m_api.compiler_initialize();
    }

    ~ShadercLoader()
    {
        if (m_compiler != nullptr && m_api.compiler_release != nullptr) { m_api.compiler_release(m_compiler); }
    }

    ShadercLoader(const ShadercLoader&)            = delete;
    ShadercLoader& operator=(const ShadercLoader&) = delete;
    ShadercLoader(ShadercLoader&&)                 = delete;
    ShadercLoader& operator=(ShadercLoader&&)      = delete;

    [[nodiscard]] bool is_valid() const noexcept { return m_lib.is_valid() && m_compiler != nullptr; }

    [[nodiscard]] ShaderCompileResult compile(ShaderStage stage, crd::containers::StringView source,
                                              crd::containers::StringView name, crd::memory::IAllocator* a,
                                              bool optimize) const
    {
        ShaderCompileResult result(a);

        if (!is_valid())
        {
            result.error_message = crd::containers::String("shaderc library not loaded", a);
            return result;
        }

        const shaderc_compile_options_t opts = m_api.options_initialize();
        m_api.options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        m_api.options_set_target_spv(opts, shaderc_spirv_version_1_6); // mesh included: LocalSizeId is fine with maintenance4,
                                                                       // and bindless nonuniformEXT (the ocean mesh) needs ≥1.5
        // Run shaderc's spirv-opt PERFORMANCE passes — WITHOUT this shaderc defaults to optimization_level_zero and
        // ships UNOPTIMIZED SPIR-V (redundant loads, no code motion / register coalescing) ⇒ the driver JITs poor SASS.
        // This was the ~30× gap between our GLSL GEMM and the identical CUDA schedule (nsys: SM-issue 1.5%). (v17-h)
        // `optimize == false` (the reflection path) leaves shaderc at level_zero so `OpName`s + dead bindings survive.
        if (optimize && m_api.options_set_opt_level != nullptr)
        {
            m_api.options_set_opt_level(opts, shaderc_optimization_level_performance);
        }

        crd::containers::String name_str(name.data(), name.size(), a); // null-terminate for the C API

        const shaderc_compilation_result_t res = m_api.compile_into_spv(
            m_compiler, source.data(), source.size(), to_shaderc_kind(stage), name_str.c_str(), "main", opts);

        m_api.options_release(opts);

        if (m_api.result_status(res) != shaderc_compilation_status_success)
        {
            const char* msg      = m_api.result_error(res);
            result.error_message = crd::containers::String(msg != nullptr ? msg : "unknown error", a);
            m_api.result_release(res);
            return result;
        }

        const char*  bytes  = m_api.result_bytes(res);
        const size_t length = m_api.result_length(res);
        result.spirv.resize(static_cast<crd::usize>(length));
        if (length > 0 && bytes != nullptr) { std::memcpy(result.spirv.data(), bytes, length); }
        m_api.result_release(res);
        result.ok = true;
        return result;
    }

private:
    crd::platform::DynamicLibrary m_lib;
    ShadercApi                    m_api{};
    shaderc_compiler_t            m_compiler = nullptr;
};

ShadercLoader& global_loader()
{
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static ShadercLoader s_loader;
    return s_loader;
}

} // namespace

ShaderCompileResult compile_glsl_to_spirv(ShaderStage stage, crd::containers::StringView source,
                                          crd::containers::StringView name, crd::memory::IAllocator* a, bool optimize)
{
    return global_loader().compile(stage, source, name, a, optimize);
}

} // namespace crd::gpu
