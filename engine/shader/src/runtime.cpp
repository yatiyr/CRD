#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/shader/runtime.hpp>

#include <cstring>
#include <shaderc/shaderc.h>
#include <spirv_reflect.h>
#include <unordered_map>

namespace fs = crd::platform::fs;

namespace crd::shader
{
namespace
{
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

[[nodiscard]] crd::rhi::Format to_rhi_format(SpvReflectFormat format) noexcept
{
    switch (format)
    {
        case SPV_REFLECT_FORMAT_R32G32_SFLOAT:
            return crd::rhi::Format::R32G32Sfloat;
        case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT:
            return crd::rhi::Format::R32G32B32Sfloat;
        case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT:
            return crd::rhi::Format::R8G8B8A8Unorm;
        default:
            return crd::rhi::Format::Undefined;
    }
}

[[nodiscard]] ParameterClass to_parameter_class(SpvReflectDescriptorType type) noexcept
{
    switch (type)
    {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
            return ParameterClass::Sampler;
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return ParameterClass::Texture;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        default:
            return ParameterClass::Buffer;
    }
}

struct ReflectedData
{
    crd::containers::Array<ParameterDesc> parameters{};
    crd::containers::Array<DescriptorBindingDesc> descriptor_bindings{};
    crd::containers::Array<PushConstantRangeDesc> push_constants{};
    crd::containers::Array<VertexAttributeLayoutDesc> vertex_attributes{};
};

[[nodiscard]] bool reflect_module(const crd::containers::Array<crd::u32>& words, Stage stage, ReflectedData& out,
                                  crd::containers::String& error)
{
    SpvReflectShaderModule module{};
    const SpvReflectResult create_result =
        spvReflectCreateShaderModule(words.size() * sizeof(crd::u32), words.data(), &module);
    if (create_result != SPV_REFLECT_RESULT_SUCCESS)
    {
        error = crd::containers::String("spvReflectCreateShaderModule failed");
        return false;
    }

    for (crd::u32 i = 0; i < module.descriptor_binding_count; ++i)
    {
        const auto& binding = module.descriptor_bindings[i];
        out.descriptor_bindings.push_back({binding.set, binding.binding, binding.count, stage});
        out.parameters.push_back({crd::containers::String(binding.name != nullptr ? binding.name : ""),
                                  to_parameter_class(binding.descriptor_type), binding.set, binding.binding,
                                  binding.block.size});
    }

    for (crd::u32 i = 0; i < module.push_constant_block_count; ++i)
    {
        const auto& block = module.push_constant_blocks[i];
        out.push_constants.push_back({block.offset, block.size, stage});
        out.parameters.push_back({crd::containers::String(block.name != nullptr ? block.name : "push_constants"),
                                  ParameterClass::PushConstant, 0, 0, block.size});
    }

    if (stage == Stage::Vertex)
    {
        for (crd::u32 i = 0; i < module.input_variable_count; ++i)
        {
            const auto* input = module.input_variables[i];
            if (input == nullptr || input->built_in >= 0)
            {
                continue;
            }
            out.vertex_attributes.push_back({crd::containers::String(input->name != nullptr ? input->name : ""),
                                             input->location, to_rhi_format(input->format), 0});
        }
    }

    spvReflectDestroyShaderModule(&module);
    return true;
}

class StoredEffect final : public Effect
{
public:
    StoredEffect(EffectHandle handle, EffectDesc desc) : m_handle(handle), m_desc(std::move(desc)) {}

    [[nodiscard]] EffectHandle handle() const noexcept override { return m_handle; }
    [[nodiscard]] crd::containers::StringView name() const noexcept override { return m_desc.name; }
    [[nodiscard]] crd::containers::ConstSpan<ParameterDesc> parameters() const noexcept override
    {
        return m_desc.parameters;
    }
    [[nodiscard]] crd::containers::ConstSpan<DescriptorBindingDesc> descriptor_bindings() const noexcept override
    {
        return m_desc.descriptor_bindings;
    }
    [[nodiscard]] crd::containers::ConstSpan<PushConstantRangeDesc> push_constants() const noexcept override
    {
        return m_desc.push_constants;
    }
    [[nodiscard]] crd::containers::ConstSpan<VertexAttributeLayoutDesc> vertex_attributes() const noexcept override
    {
        return m_desc.vertex_attributes;
    }

    [[nodiscard]] const EffectDesc& desc() const noexcept { return m_desc; }
    EffectDesc& mutable_desc() noexcept { return m_desc; }

private:
    EffectHandle m_handle{};
    EffectDesc m_desc{};
};

class StoredModule final : public Module
{
public:
    StoredModule(ModuleHandle handle, Stage stage, crd::containers::String entry_point,
                 crd::containers::Array<crd::u32> words, crd::containers::String source_path, ReflectedData reflected)
        : m_handle(handle), m_stage(stage), m_entry_point(std::move(entry_point)), m_words(std::move(words)),
          m_source_path(std::move(source_path)), m_reflected(std::move(reflected))
    {
    }

    [[nodiscard]] ModuleHandle handle() const noexcept override { return m_handle; }
    [[nodiscard]] Stage stage() const noexcept override { return m_stage; }
    [[nodiscard]] crd::containers::StringView entry_point() const noexcept override { return m_entry_point; }
    [[nodiscard]] crd::u64 code_size_bytes() const noexcept override
    {
        return static_cast<crd::u64>(m_words.size() * sizeof(crd::u32));
    }
    [[nodiscard]] crd::containers::ConstSpan<ParameterDesc> parameters() const noexcept override
    {
        return m_reflected.parameters;
    }
    [[nodiscard]] crd::containers::ConstSpan<DescriptorBindingDesc> descriptor_bindings() const noexcept override
    {
        return m_reflected.descriptor_bindings;
    }
    [[nodiscard]] crd::containers::ConstSpan<PushConstantRangeDesc> push_constants() const noexcept override
    {
        return m_reflected.push_constants;
    }
    [[nodiscard]] crd::containers::ConstSpan<VertexAttributeLayoutDesc> vertex_attributes() const noexcept override
    {
        return m_reflected.vertex_attributes;
    }

private:
    ModuleHandle m_handle{};
    Stage m_stage = Stage::Vertex;
    crd::containers::String m_entry_point{};
    crd::containers::Array<crd::u32> m_words{};
    crd::containers::String m_source_path{};
    ReflectedData m_reflected{};
};

struct StoredVariant
{
    VariantHandle handle{};
    crd::containers::Array<ModuleHandle> modules{};
};

struct ShadercApi
{
    using CompilerInitialize = shaderc_compiler_t (*)();
    using CompilerRelease = void (*)(shaderc_compiler_t);
    using OptionsInitialize = shaderc_compile_options_t (*)();
    using OptionsRelease = void (*)(shaderc_compile_options_t);
    using OptionsSetTargetEnv = void (*)(shaderc_compile_options_t, shaderc_target_env, shaderc_env_version);
    using OptionsSetTargetSpirv = void (*)(shaderc_compile_options_t, shaderc_spirv_version);
    using OptionsSetOptimization = void (*)(shaderc_compile_options_t, shaderc_optimization_level);
    using OptionsSetDebugInfo = void (*)(shaderc_compile_options_t);
    using CompileIntoSpv = shaderc_compilation_result_t (*)(shaderc_compiler_t, const char*, size_t,
                                                            shaderc_shader_kind, const char*, const char*,
                                                            const shaderc_compile_options_t);
    using ResultRelease = void (*)(shaderc_compilation_result_t);
    using ResultStatus = shaderc_compilation_status (*)(const shaderc_compilation_result_t);
    using ResultError = const char* (*)(const shaderc_compilation_result_t);
    using ResultBytes = const char* (*)(const shaderc_compilation_result_t);
    using ResultLength = size_t (*)(const shaderc_compilation_result_t);

    CompilerInitialize compiler_initialize = nullptr;
    CompilerRelease compiler_release = nullptr;
    OptionsInitialize options_initialize = nullptr;
    OptionsRelease options_release = nullptr;
    OptionsSetTargetEnv options_set_target_env = nullptr;
    OptionsSetTargetSpirv options_set_target_spirv = nullptr;
    OptionsSetOptimization options_set_optimization = nullptr;
    OptionsSetDebugInfo options_set_debug_info = nullptr;
    CompileIntoSpv compile_into_spv = nullptr;
    ResultRelease result_release = nullptr;
    ResultStatus result_status = nullptr;
    ResultError result_error = nullptr;
    ResultBytes result_bytes = nullptr;
    ResultLength result_length = nullptr;
};

[[nodiscard]] fs::Path shaderc_library_path() noexcept
{
#if defined(CRD_OS_WINDOWS)
    char* sdk = nullptr;
    std::size_t len = 0;
    const errno_t rc = _dupenv_s(&sdk, &len, "VULKAN_SDK");
    if (rc == 0 && sdk != nullptr && sdk[0] != '\0')
    {
        const fs::Path path = fs::Path(sdk) / "Bin" / "shaderc_shared.dll";
        free(sdk);
        return path;
    }
    free(sdk);
    return fs::Path("shaderc_shared.dll");
#elif defined(CRD_OS_LINUX)
    return fs::Path("libshaderc_shared.so");
#else
    return fs::Path("libshaderc_shared.dylib");
#endif
}

class LocalRuntime final : public Runtime
{
public:
    LocalRuntime()
    {
        m_library = crd::platform::DynamicLibrary::open(shaderc_library_path());
        if (!m_library.is_valid())
        {
            return;
        }

        m_api.compiler_initialize = m_library.resolve_as<ShadercApi::CompilerInitialize>("shaderc_compiler_initialize");
        m_api.compiler_release = m_library.resolve_as<ShadercApi::CompilerRelease>("shaderc_compiler_release");
        m_api.options_initialize =
            m_library.resolve_as<ShadercApi::OptionsInitialize>("shaderc_compile_options_initialize");
        m_api.options_release = m_library.resolve_as<ShadercApi::OptionsRelease>("shaderc_compile_options_release");
        m_api.options_set_target_env =
            m_library.resolve_as<ShadercApi::OptionsSetTargetEnv>("shaderc_compile_options_set_target_env");
        m_api.options_set_target_spirv =
            m_library.resolve_as<ShadercApi::OptionsSetTargetSpirv>("shaderc_compile_options_set_target_spirv");
        m_api.options_set_optimization =
            m_library.resolve_as<ShadercApi::OptionsSetOptimization>("shaderc_compile_options_set_optimization_level");
        m_api.options_set_debug_info =
            m_library.resolve_as<ShadercApi::OptionsSetDebugInfo>("shaderc_compile_options_set_generate_debug_info");
        m_api.compile_into_spv = m_library.resolve_as<ShadercApi::CompileIntoSpv>("shaderc_compile_into_spv");
        m_api.result_release = m_library.resolve_as<ShadercApi::ResultRelease>("shaderc_result_release");
        m_api.result_status = m_library.resolve_as<ShadercApi::ResultStatus>("shaderc_result_get_compilation_status");
        m_api.result_error = m_library.resolve_as<ShadercApi::ResultError>("shaderc_result_get_error_message");
        m_api.result_bytes = m_library.resolve_as<ShadercApi::ResultBytes>("shaderc_result_get_bytes");
        m_api.result_length = m_library.resolve_as<ShadercApi::ResultLength>("shaderc_result_get_length");

        if (m_api.compiler_initialize == nullptr || m_api.compiler_release == nullptr ||
            m_api.options_initialize == nullptr || m_api.options_release == nullptr ||
            m_api.options_set_target_env == nullptr || m_api.options_set_target_spirv == nullptr ||
            m_api.options_set_optimization == nullptr || m_api.options_set_debug_info == nullptr ||
            m_api.compile_into_spv == nullptr || m_api.result_release == nullptr || m_api.result_status == nullptr ||
            m_api.result_error == nullptr || m_api.result_bytes == nullptr || m_api.result_length == nullptr)
        {
            m_library = {};
            return;
        }

        m_compiler = m_api.compiler_initialize();
        m_options = m_api.options_initialize();
        m_api.options_set_target_env(m_options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        m_api.options_set_target_spirv(m_options, shaderc_spirv_version_1_6);
        m_api.options_set_optimization(m_options, shaderc_optimization_level_zero);
        m_api.options_set_debug_info(m_options);
    }

    ~LocalRuntime() override
    {
        if (m_options != nullptr && m_api.options_release != nullptr)
        {
            m_api.options_release(m_options);
        }
        if (m_compiler != nullptr && m_api.compiler_release != nullptr)
        {
            m_api.compiler_release(m_compiler);
        }
    }

    [[nodiscard]] EffectHandle create_effect(const EffectDesc& desc) override
    {
        const EffectHandle handle{m_next_effect_handle++};
        m_effects.emplace(handle.value, StoredEffect(handle, desc));
        return handle;
    }

    [[nodiscard]] const Effect* find_effect(EffectHandle handle) const noexcept override
    {
        const auto it = m_effects.find(handle.value);
        return it == m_effects.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const Module* find_module(ModuleHandle handle) const noexcept override
    {
        const auto it = m_modules.find(handle.value);
        return it == m_modules.end() ? nullptr : &it->second;
    }

    [[nodiscard]] VariantHandle request_variant(const VariantCompileRequest& request,
                                                CompileDiagnostics& diagnostics) override
    {
        diagnostics.succeeded = false;
        diagnostics.message = crd::containers::String{};

        if (m_compiler == nullptr || m_options == nullptr)
        {
            diagnostics.message = crd::containers::String("shaderc runtime is unavailable");
            return {};
        }

        const auto effect_it = m_effects.find(request.effect.value);
        if (request.effect.value == 0 || effect_it == m_effects.end())
        {
            diagnostics.message = crd::containers::String("Variant request referenced an unknown effect handle");
            return {};
        }

        StoredEffect& effect = effect_it->second;
        const auto& effect_desc = effect.desc();
        if (effect_desc.frontend_modules.empty())
        {
            diagnostics.message = crd::containers::String("Effect has no frontend modules to compile");
            return {};
        }

        StoredVariant stored_variant;
        stored_variant.handle = VariantHandle{m_next_variant_handle++};

        EffectDesc reflected_effect_desc = effect.desc();
        reflected_effect_desc.parameters.clear();
        reflected_effect_desc.descriptor_bindings.clear();
        reflected_effect_desc.push_constants.clear();
        reflected_effect_desc.vertex_attributes.clear();

        for (const auto& compile_request : effect_desc.frontend_modules)
        {
            crd::containers::String source_text;
            if (!fs::read_file_text(fs::Path(compile_request.source_path), source_text))
            {
                diagnostics.message = crd::containers::String("Failed to read shader source file");
                return {};
            }

            const shaderc_compilation_result_t result = m_api.compile_into_spv(
                m_compiler, source_text.c_str(), source_text.size(), to_shaderc_kind(compile_request.stage),
                compile_request.source_path.c_str(), compile_request.entry_point.c_str(), m_options);
            if (m_api.result_status(result) != shaderc_compilation_status_success)
            {
                diagnostics.message = crd::containers::String(m_api.result_error(result));
                m_api.result_release(result);
                return {};
            }

            const char* bytes = m_api.result_bytes(result);
            const size_t length = m_api.result_length(result);
            crd::containers::Array<crd::u32> words;
            words.resize(length / sizeof(crd::u32));
            std::memcpy(words.data(), bytes, length);

            ReflectedData reflected;
            crd::containers::String reflect_error;
            if (!reflect_module(words, compile_request.stage, reflected, reflect_error))
            {
                diagnostics.message = reflect_error;
                m_api.result_release(result);
                return {};
            }

            const ModuleHandle module_handle{m_next_module_handle++};
            for (const auto& parameter : reflected.parameters)
            {
                reflected_effect_desc.parameters.push_back(parameter);
            }
            for (const auto& descriptor_binding : reflected.descriptor_bindings)
            {
                reflected_effect_desc.descriptor_bindings.push_back(descriptor_binding);
            }
            for (const auto& push_constant : reflected.push_constants)
            {
                reflected_effect_desc.push_constants.push_back(push_constant);
            }
            for (const auto& vertex_attribute : reflected.vertex_attributes)
            {
                reflected_effect_desc.vertex_attributes.push_back(vertex_attribute);
            }

            m_modules.emplace(module_handle.value,
                              StoredModule(module_handle, compile_request.stage, compile_request.entry_point,
                                           std::move(words), compile_request.source_path, std::move(reflected)));
            stored_variant.modules.push_back(module_handle);
            m_api.result_release(result);
        }

        effect.mutable_desc() = std::move(reflected_effect_desc);

        const VariantHandle handle = stored_variant.handle;
        m_variants.emplace(handle.value, std::move(stored_variant));
        diagnostics.succeeded = true;
        diagnostics.message = crd::containers::String("GLSL frontend compiled and reflection metadata consumed");
        return handle;
    }

    [[nodiscard]] bool is_variant_ready(VariantHandle handle) const noexcept override
    {
        return m_variants.find(handle.value) != m_variants.end();
    }

    [[nodiscard]] crd::containers::ConstSpan<ModuleHandle> variant_modules(VariantHandle handle) const noexcept override
    {
        const auto it = m_variants.find(handle.value);
        if (it == m_variants.end())
        {
            return {};
        }
        return it->second.modules;
    }

    [[nodiscard]] bool reload_effect(EffectHandle handle, ReloadEvent& event) override
    {
        event.effect = handle;
        event.using_last_good = false;
        event.succeeded = find_effect(handle) != nullptr;
        return event.succeeded;
    }

private:
    crd::u64 m_next_effect_handle = 1;
    crd::u64 m_next_variant_handle = 1;
    crd::u64 m_next_module_handle = 1;
    std::unordered_map<crd::u64, StoredEffect> m_effects{};
    std::unordered_map<crd::u64, StoredModule> m_modules{};
    std::unordered_map<crd::u64, StoredVariant> m_variants{};
    crd::platform::DynamicLibrary m_library{};
    ShadercApi m_api{};
    shaderc_compiler_t m_compiler = nullptr;
    shaderc_compile_options_t m_options = nullptr;
};
} // namespace

std::unique_ptr<Runtime> create_runtime()
{
    return std::make_unique<LocalRuntime>();
}
} // namespace crd::shader
