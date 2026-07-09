#include <crd/shader/compile.hpp>

#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/filesystem.hpp>

#include <shaderc/shaderc.h>

namespace fs = crd::platform::fs;

namespace crd::shader
{
namespace
{

// ── shaderc function-pointer table ─────────────────────────────────────────

struct ShadercApi
{
    using CompilerInitialize  = shaderc_compiler_t (*)();
    using CompilerRelease     = void (*)(shaderc_compiler_t);
    using OptionsInitialize   = shaderc_compile_options_t (*)();
    using OptionsRelease      = void (*)(shaderc_compile_options_t);
    using OptionsSetTargetEnv = void (*)(shaderc_compile_options_t, shaderc_target_env,
                                         shaderc_env_version);
    using OptionsSetTargetSpv = void (*)(shaderc_compile_options_t, shaderc_spirv_version);
    using OptionsSetOptLevel  = void (*)(shaderc_compile_options_t, shaderc_optimization_level);
    using CompileIntoSpv      = shaderc_compilation_result_t (*)(
        shaderc_compiler_t, const char*, size_t, shaderc_shader_kind,
        const char*, const char*, const shaderc_compile_options_t);
    using ResultRelease  = void (*)(shaderc_compilation_result_t);
    using ResultStatus   = shaderc_compilation_status (*)(const shaderc_compilation_result_t);
    using ResultError    = const char* (*)(const shaderc_compilation_result_t);
    using ResultBytes    = const char* (*)(const shaderc_compilation_result_t);
    using ResultLength   = size_t (*)(const shaderc_compilation_result_t);

    CompilerInitialize  compiler_initialize  = nullptr;
    CompilerRelease     compiler_release     = nullptr;
    OptionsInitialize   options_initialize   = nullptr;
    OptionsRelease      options_release      = nullptr;
    OptionsSetTargetEnv options_set_target_env = nullptr;
    OptionsSetTargetSpv options_set_target_spv = nullptr;
    OptionsSetOptLevel  options_set_opt_level  = nullptr; // optional: spirv-opt performance passes
    CompileIntoSpv      compile_into_spv      = nullptr;
    ResultRelease       result_release        = nullptr;
    ResultStatus        result_status         = nullptr;
    ResultError         result_error          = nullptr;
    ResultBytes         result_bytes          = nullptr;
    ResultLength        result_length         = nullptr;
};

// shaderc ships under different SONAMEs depending on where it came from:
//   - Vulkan SDK (Windows / Linux .tar.gz / macOS):  shaderc_shared.{dll,so,dylib}
//   - Ubuntu / Debian `libshaderc-dev`:               libshaderc.so / libshaderc.so.1
//   - Conda / vcpkg / homebrew:                       libshaderc.so or libshaderc_combined.a
//
// We probe a small list of candidates per OS so the engine works on both
// SDK-installed and distro-packaged shaderc without manual symlinks.
[[nodiscard]] crd::platform::DynamicLibrary try_open_shaderc() noexcept
{
    // Quiet per-candidate probe: a candidate SONAME that isn't installed is
    // expected, not an error. The final candidate uses the loud open so that a
    // total miss still surfaces one ERROR (from DynamicLibrary::open itself).
#if CRD_OS_WINDOWS
    char*       sdk = nullptr;
    std::size_t len = 0;
    const errno_t rc = _dupenv_s(&sdk, &len, "VULKAN_SDK");
    if (rc == 0 && sdk != nullptr && sdk[0] != '\0')
    {
        fs::Path sdk_path = fs::Path(sdk) / "Bin" / "shaderc_shared.dll";
        free(sdk);
        auto lib = crd::platform::DynamicLibrary::open(sdk_path, /*log_on_failure=*/false);
        if (lib.is_valid())
        {
            return lib;
        }
    }
    else
    {
        free(sdk);
    }
    const fs::Path candidates[] = {fs::Path("shaderc_shared.dll")};
#elif CRD_OS_LINUX
    const fs::Path candidates[] = {
        fs::Path("libshaderc_shared.so"),   // Vulkan SDK convention
        fs::Path("libshaderc.so.1"),        // Ubuntu / Debian (libshaderc1 package)
        fs::Path("libshaderc.so"),          // Ubuntu / Debian dev symlink
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
        auto lib = crd::platform::DynamicLibrary::open(candidates[i], /*log_on_failure=*/last);
        if (lib.is_valid())
        {
            return lib;
        }
    }
    return crd::platform::DynamicLibrary{};
}

[[nodiscard]] shaderc_shader_kind to_shaderc_kind(Stage stage) noexcept
{
    switch (stage)
    {
        case Stage::Vertex:
            return shaderc_vertex_shader;
        case Stage::Fragment:
            return shaderc_fragment_shader;
        case Stage::Compute:
        default:
            return shaderc_compute_shader;
    }
}

class ShadercLoader
{
public:
    ShadercLoader()
    {
        m_lib = try_open_shaderc();
        if (!m_lib.is_valid())
        {
            return;
        }

        m_api.compiler_initialize   = m_lib.resolve_as<ShadercApi::CompilerInitialize>(
            "shaderc_compiler_initialize");
        m_api.compiler_release      = m_lib.resolve_as<ShadercApi::CompilerRelease>(
            "shaderc_compiler_release");
        m_api.options_initialize    = m_lib.resolve_as<ShadercApi::OptionsInitialize>(
            "shaderc_compile_options_initialize");
        m_api.options_release       = m_lib.resolve_as<ShadercApi::OptionsRelease>(
            "shaderc_compile_options_release");
        m_api.options_set_target_env = m_lib.resolve_as<ShadercApi::OptionsSetTargetEnv>(
            "shaderc_compile_options_set_target_env");
        m_api.options_set_target_spv = m_lib.resolve_as<ShadercApi::OptionsSetTargetSpv>(
            "shaderc_compile_options_set_target_spirv");
        m_api.options_set_opt_level = m_lib.resolve_as<ShadercApi::OptionsSetOptLevel>(
            "shaderc_compile_options_set_optimization_level"); // optional (older shaderc may lack it)
        m_api.compile_into_spv      = m_lib.resolve_as<ShadercApi::CompileIntoSpv>(
            "shaderc_compile_into_spv");
        m_api.result_release        = m_lib.resolve_as<ShadercApi::ResultRelease>(
            "shaderc_result_release");
        m_api.result_status         = m_lib.resolve_as<ShadercApi::ResultStatus>(
            "shaderc_result_get_compilation_status");
        m_api.result_error          = m_lib.resolve_as<ShadercApi::ResultError>(
            "shaderc_result_get_error_message");
        m_api.result_bytes          = m_lib.resolve_as<ShadercApi::ResultBytes>(
            "shaderc_result_get_bytes");
        m_api.result_length         = m_lib.resolve_as<ShadercApi::ResultLength>(
            "shaderc_result_get_length");

        if (m_api.compiler_initialize == nullptr || m_api.compiler_release == nullptr ||
            m_api.options_initialize  == nullptr || m_api.options_release   == nullptr ||
            m_api.options_set_target_env == nullptr || m_api.options_set_target_spv == nullptr ||
            m_api.compile_into_spv   == nullptr || m_api.result_release    == nullptr ||
            m_api.result_status      == nullptr || m_api.result_error      == nullptr ||
            m_api.result_bytes       == nullptr || m_api.result_length     == nullptr)
        {
            m_lib = {};
            return;
        }

        m_compiler = m_api.compiler_initialize();
    }

    ~ShadercLoader()
    {
        if (m_compiler != nullptr && m_api.compiler_release != nullptr)
        {
            m_api.compiler_release(m_compiler);
        }
    }

    ShadercLoader(const ShadercLoader&)            = delete;
    ShadercLoader& operator=(const ShadercLoader&) = delete;

    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_lib.is_valid() && m_compiler != nullptr;
    }

    [[nodiscard]] CompileResult compile(Stage stage, crd::containers::StringView source,
                                        crd::containers::StringView name,
                                        crd::memory::IAllocator* a) const
    {
        CompileResult result(a);

        if (!is_valid())
        {
            result.error_message = crd::containers::String(
                "shaderc library not loaded", a);
            return result;
        }

        const shaderc_compile_options_t opts = m_api.options_initialize();
        m_api.options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        m_api.options_set_target_spv(opts, shaderc_spirv_version_1_6);
        // Run shaderc's spirv-opt PERFORMANCE passes — WITHOUT this shaderc defaults to optimization_level_zero and
        // ships UNOPTIMIZED SPIR-V (redundant loads, no code motion / register coalescing) ⇒ the driver JITs poor SASS.
        // This was the ~30× gap between our GLSL GEMM and the identical CUDA schedule (nsys: SM-issue 1.5%). (v17-h)
        if (m_api.options_set_opt_level != nullptr) { m_api.options_set_opt_level(opts, shaderc_optimization_level_performance); }

        // Null-terminate the name for C API.
        crd::containers::String name_str(name.data(), name.size(), a);

        const shaderc_compilation_result_t res =
            m_api.compile_into_spv(m_compiler,
                                   source.data(), source.size(),
                                   to_shaderc_kind(stage),
                                   name_str.c_str(), "main",
                                   opts);

        m_api.options_release(opts);

        if (m_api.result_status(res) != shaderc_compilation_status_success)
        {
            const char* msg = m_api.result_error(res);
            result.error_message = crd::containers::String(msg != nullptr ? msg : "unknown error", a);
            m_api.result_release(res);
            return result;
        }

        const char*  bytes  = m_api.result_bytes(res);
        const size_t length = m_api.result_length(res);
        result.spirv.resize(static_cast<crd::usize>(length));
        if (length > 0 && bytes != nullptr)
        {
            std::memcpy(result.spirv.data(), bytes, length);
        }
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

} // anonymous namespace

CompileResult compile_glsl(Stage stage, crd::containers::StringView source,
                            crd::containers::StringView name, crd::memory::IAllocator* a)
{
    return global_loader().compile(stage, source, name, a);
}

} // namespace crd::shader
