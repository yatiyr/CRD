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
[[nodiscard]] constexpr crd::u64 fnv1a_mix(crd::u64 hash, crd::u64 value) noexcept
{
    constexpr crd::u64 Prime = 1099511628211ull;
    hash ^= value;
    hash *= Prime;
    return hash;
}

[[nodiscard]] crd::u64 hash_bytes(const char* data, size_t size) noexcept
{
    crd::u64 hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash = fnv1a_mix(hash, static_cast<unsigned char>(data[i]));
    }
    return hash;
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

void merge_descriptor_binding(crd::containers::Array<DescriptorBindingDesc>& out,
                              const DescriptorBindingDesc& candidate)
{
    for (auto& existing : out)
    {
        if (existing.set_index == candidate.set_index && existing.binding == candidate.binding)
        {
            existing.count = std::max(existing.count, candidate.count);
            existing.visibility_mask |= candidate.visibility_mask;
            return;
        }
    }
    out.push_back(candidate);
}

void merge_push_constant(crd::containers::Array<PushConstantRangeDesc>& out, const PushConstantRangeDesc& candidate)
{
    for (auto& existing : out)
    {
        if (existing.offset == candidate.offset && existing.size_bytes == candidate.size_bytes)
        {
            existing.visibility_mask |= candidate.visibility_mask;
            return;
        }
    }
    out.push_back(candidate);
}

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
        out.descriptor_bindings.push_back({binding.set, binding.binding, binding.count, stage_bit(stage)});
        out.parameters.push_back({crd::containers::String(binding.name != nullptr ? binding.name : ""),
                                  to_parameter_class(binding.descriptor_type), binding.set, binding.binding,
                                  binding.block.size});
    }

    for (crd::u32 i = 0; i < module.push_constant_block_count; ++i)
    {
        const auto& block = module.push_constant_blocks[i];
        out.push_constants.push_back({block.offset, block.size, stage_bit(stage)});
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

struct PreprocessResult
{
    crd::containers::String preprocessed_text{};
    crd::containers::Array<crd::containers::String> include_paths{};
};

[[nodiscard]] bool preprocess_file(const fs::Path& path, PreprocessResult& out,
                                   crd::containers::Array<crd::containers::String>& include_stack,
                                   crd::containers::String& error)
{
    const crd::containers::String path_str(path.generic());
    for (const auto& active : include_stack)
    {
        if (active == path_str)
        {
            error = crd::containers::String("Detected cyclic shader include");
            return false;
        }
    }

    crd::containers::String source_text;
    if (!fs::read_file_text(path, source_text))
    {
        error = crd::containers::String("Failed to read shader source file");
        return false;
    }

    include_stack.push_back(path_str);

    crd::usize line_start = 0;
    while (line_start <= source_text.size())
    {
        crd::usize line_end = line_start;
        while (line_end < source_text.size() && source_text.data()[line_end] != '\n')
        {
            ++line_end;
        }

        const crd::containers::StringView line(source_text.data() + line_start, line_end - line_start);
        if (line.starts_with("#include \"") && line.size() > 11 && line[line.size() - 1] == '"')
        {
            const auto include_name = line.substr(10, line.size() - 11);
            const fs::Path include_path = fs::Path(path.generic()) / ".." / include_name;
            out.include_paths.push_back(crd::containers::String(include_path.generic()));
            PreprocessResult nested;
            if (!preprocess_file(include_path, nested, include_stack, error))
            {
                include_stack.pop_back();
                return false;
            }
            out.preprocessed_text.reserve(out.preprocessed_text.size() + nested.preprocessed_text.size() + 8u);
            out.preprocessed_text.append(nested.preprocessed_text.data(), nested.preprocessed_text.size());
            for (const auto& nested_include : nested.include_paths)
            {
                out.include_paths.push_back(nested_include);
            }
        }
        else
        {
            out.preprocessed_text.reserve(out.preprocessed_text.size() + line.size() + 8u);
            out.preprocessed_text.append(line.data(), line.size());
            out.preprocessed_text.push_back('\n');
        }

        line_start = line_end + 1;
        if (line_end == source_text.size())
        {
            break;
        }
    }

    include_stack.pop_back();
    return true;
}

[[nodiscard]] fs::Path shader_cache_dir() noexcept
{
    return fs::executable_dir() / "cache" / "shaders";
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
                 crd::containers::Array<crd::u32> words, crd::containers::String source_path, ReflectedData reflected,
                 SourceKey source_key, PreprocessedKey preprocessed_key, SpirvKey spirv_key)
        : m_handle(handle), m_stage(stage), m_entry_point(std::move(entry_point)), m_words(std::move(words)),
          m_source_path(std::move(source_path)), m_reflected(std::move(reflected)), m_source_key(source_key),
          m_preprocessed_key(preprocessed_key), m_spirv_key(spirv_key)
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
    SourceKey m_source_key{};
    PreprocessedKey m_preprocessed_key{};
    SpirvKey m_spirv_key{};
};

struct StoredVariant
{
    VariantHandle handle{};
    VariantKey key{};
    VariantCompileRequest request{};
    crd::containers::Array<ModuleHandle> modules{};
};

struct CachedSpirv
{
    crd::containers::Array<crd::u32> words{};
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
        StoredVariant stored_variant;
        stored_variant.handle = VariantHandle{m_next_variant_handle++};
        stored_variant.key = make_variant_key(request.variant);
        stored_variant.request = request;

        EffectDesc reflected_effect_desc;
        if (!compile_variant_modules(request, stored_variant, reflected_effect_desc, diagnostics))
        {
            return {};
        }

        StoredEffect& effect = m_effects.find(request.effect.value)->second;
        effect.mutable_desc() = std::move(reflected_effect_desc);
        const VariantHandle handle = stored_variant.handle;
        m_variants.emplace(handle.value, std::move(stored_variant));
        return handle;
    }

    [[nodiscard]] bool is_variant_ready(VariantHandle handle) const noexcept override
    {
        return m_variants.find(handle.value) != m_variants.end();
    }

    [[nodiscard]] VariantKey variant_key(VariantHandle handle) const noexcept override
    {
        const auto it = m_variants.find(handle.value);
        if (it == m_variants.end())
        {
            return {};
        }
        return it->second.key;
    }

    [[nodiscard]] bool describe_variant(VariantHandle handle, VariantPipelineDesc& out) const noexcept override
    {
        const auto variant_it = m_variants.find(handle.value);
        if (variant_it == m_variants.end())
        {
            return false;
        }

        out = {};
        out.variant = handle;
        for (const auto& module_handle : variant_it->second.modules)
        {
            const auto* module = find_module(module_handle);
            if (module == nullptr)
            {
                return false;
            }

            out.modules.push_back({module->handle(), module->stage(), crd::containers::String(module->entry_point())});
            for (const auto& binding : module->descriptor_bindings())
            {
                merge_descriptor_binding(out.descriptor_bindings, binding);
            }
            for (const auto& push_constant : module->push_constants())
            {
                merge_push_constant(out.push_constants, push_constant);
            }
            if (module->stage() == Stage::Vertex)
            {
                for (const auto& attribute : module->vertex_attributes())
                {
                    out.vertex_attributes.push_back(attribute);
                }
            }
        }

        return true;
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

        const auto effect_it = m_effects.find(handle.value);
        if (handle.value == 0 || effect_it == m_effects.end())
        {
            event.succeeded = false;
            return false;
        }

        StoredEffect& effect = effect_it->second;
        const EffectDesc previous_effect_desc = effect.desc();

        struct ReloadedVariant
        {
            VariantHandle handle{};
            StoredVariant variant{};
            EffectDesc reflected_effect_desc{};
            CompileDiagnostics diagnostics{};
        };

        crd::containers::Array<ReloadedVariant> refreshed_variants;
        for (const auto& [variant_id, variant] : m_variants)
        {
            if (variant.request.effect.value != handle.value)
            {
                continue;
            }

            ReloadedVariant refreshed;
            refreshed.handle = VariantHandle{variant_id};
            refreshed.variant = variant;
            refreshed.variant.modules.clear();
            if (!compile_variant_modules(variant.request, refreshed.variant, refreshed.reflected_effect_desc,
                                         refreshed.diagnostics, false))
            {
                event.succeeded = false;
                event.using_last_good = true;
                return false;
            }
            refreshed_variants.push_back(std::move(refreshed));
        }

        // No live variants yet: effect exists, so reload is a no-op success.
        if (refreshed_variants.empty())
        {
            event.succeeded = true;
            return true;
        }

        for (auto& refreshed : refreshed_variants)
        {
            StoredVariant& live_variant = m_variants.find(refreshed.handle.value)->second;

            const crd::usize reuse_count = std::min(live_variant.modules.size(), refreshed.variant.modules.size());
            for (crd::usize i = 0; i < reuse_count; ++i)
            {
                const auto new_handle = refreshed.variant.modules[i];
                const auto old_handle = live_variant.modules[i];
                auto new_module_it = m_modules.find(new_handle.value);
                auto old_module_it = m_modules.find(old_handle.value);
                CRD_ASSERT(new_module_it != m_modules.end());
                CRD_ASSERT(old_module_it != m_modules.end());
                old_module_it->second = std::move(new_module_it->second);
                m_modules.erase(new_module_it);
                refreshed.variant.modules[i] = old_handle;
            }

            for (crd::usize i = reuse_count; i < live_variant.modules.size(); ++i)
            {
                m_modules.erase(live_variant.modules[i].value);
            }

            live_variant = std::move(refreshed.variant);
            effect.mutable_desc() = std::move(refreshed.reflected_effect_desc);
        }

        event.succeeded = true;
        return true;
    }

private:
    [[nodiscard]] bool compile_variant_modules(const VariantCompileRequest& request, StoredVariant& stored_variant,
                                               EffectDesc& reflected_effect_desc, CompileDiagnostics& diagnostics,
                                               bool reuse_module_handles = true)
    {
        diagnostics = {};

        if (m_compiler == nullptr || m_options == nullptr)
        {
            diagnostics.message = crd::containers::String("shaderc runtime is unavailable");
            return false;
        }

        const auto effect_it = m_effects.find(request.effect.value);
        if (request.effect.value == 0 || effect_it == m_effects.end())
        {
            diagnostics.message = crd::containers::String("Variant request referenced an unknown effect handle");
            return false;
        }

        const auto& effect_desc = effect_it->second.desc();
        if (effect_desc.frontend_modules.empty())
        {
            diagnostics.message = crd::containers::String("Effect has no frontend modules to compile");
            return false;
        }

        reflected_effect_desc = effect_desc;
        reflected_effect_desc.parameters.clear();
        reflected_effect_desc.descriptor_bindings.clear();
        reflected_effect_desc.push_constants.clear();
        reflected_effect_desc.vertex_attributes.clear();

        for (const auto& compile_request : effect_desc.frontend_modules)
        {
            crd::containers::String source_text;
            const fs::Path source_path(compile_request.source_path);
            if (!fs::read_file_text(source_path, source_text))
            {
                diagnostics.message = crd::containers::String("Failed to read shader source file");
                return false;
            }

            diagnostics.source_key = SourceKey{hash_bytes(source_text.data(), source_text.size())};
            diagnostics.source_cache_hit = m_source_cache.find(diagnostics.source_key.value) != m_source_cache.end();
            if (!diagnostics.source_cache_hit)
            {
                m_source_cache.emplace(diagnostics.source_key.value, source_text);
            }

            PreprocessResult preprocessed;
            crd::containers::Array<crd::containers::String> include_stack;
            crd::containers::String preprocess_error;
            if (!preprocess_file(source_path, preprocessed, include_stack, preprocess_error))
            {
                diagnostics.message = preprocess_error;
                return false;
            }

            crd::u64 preprocess_hash =
                hash_bytes(preprocessed.preprocessed_text.data(), preprocessed.preprocessed_text.size());
            for (const auto& include_path : preprocessed.include_paths)
            {
                preprocess_hash = fnv1a_mix(preprocess_hash, hash_bytes(include_path.data(), include_path.size()));
            }
            diagnostics.preprocessed_key = PreprocessedKey{preprocess_hash};
            diagnostics.preprocessed_cache_hit =
                m_preprocessed_cache.find(diagnostics.preprocessed_key.value) != m_preprocessed_cache.end();
            if (!diagnostics.preprocessed_cache_hit)
            {
                m_preprocessed_cache.emplace(diagnostics.preprocessed_key.value, preprocessed.preprocessed_text);
            }

            crd::u64 spirv_hash = diagnostics.preprocessed_key.value;
            spirv_hash = fnv1a_mix(spirv_hash, static_cast<crd::u64>(compile_request.stage));
            spirv_hash = fnv1a_mix(spirv_hash,
                                   hash_bytes(compile_request.entry_point.data(), compile_request.entry_point.size()));
            spirv_hash = fnv1a_mix(spirv_hash, stored_variant.key.value);
            diagnostics.spirv_key = SpirvKey{spirv_hash};

            ModuleHandle module_handle{};
            crd::containers::Array<crd::u32> words;
            diagnostics.spirv_cache_hit = false;

            if (reuse_module_handles)
            {
                if (const auto module_it = m_module_cache.find(diagnostics.spirv_key.value);
                    module_it != m_module_cache.end())
                {
                    module_handle = module_it->second;
                    diagnostics.spirv_cache_hit = true;
                }
            }

            if (!module_handle.is_valid())
            {
                crd::containers::String cache_name(std::to_string(diagnostics.spirv_key.value).c_str());
                cache_name.append(".spv");
                const fs::Path cache_path = shader_cache_dir() / cache_name;

                if (const auto spirv_it = m_spirv_cache.find(diagnostics.spirv_key.value);
                    spirv_it != m_spirv_cache.end())
                {
                    words = spirv_it->second.words;
                    diagnostics.spirv_cache_hit = true;
                }
                else
                {
                    crd::containers::Array<crd::u8> cached_bytes;
                    if (fs::read_file_binary(cache_path, cached_bytes) &&
                        (cached_bytes.size() % sizeof(crd::u32)) == 0u)
                    {
                        words.resize(cached_bytes.size() / sizeof(crd::u32));
                        std::memcpy(words.data(), cached_bytes.data(), cached_bytes.size());
                        diagnostics.spirv_cache_hit = true;
                    }
                    else
                    {
                        const shaderc_compilation_result_t result = m_api.compile_into_spv(
                            m_compiler, preprocessed.preprocessed_text.c_str(), preprocessed.preprocessed_text.size(),
                            to_shaderc_kind(compile_request.stage), compile_request.source_path.c_str(),
                            compile_request.entry_point.c_str(), m_options);
                        if (m_api.result_status(result) != shaderc_compilation_status_success)
                        {
                            diagnostics.message = crd::containers::String(m_api.result_error(result));
                            m_api.result_release(result);
                            return false;
                        }
                        const char* bytes = m_api.result_bytes(result);
                        const size_t length = m_api.result_length(result);
                        words.resize(length / sizeof(crd::u32));
                        std::memcpy(words.data(), bytes, length);
                        m_api.result_release(result);

                        (void)fs::create_directories(shader_cache_dir());
                        const auto byte_count = words.size() * sizeof(crd::u32);
                        (void)fs::write_file_binary(cache_path,
                                                    crd::containers::ConstSpan<crd::u8>(
                                                        reinterpret_cast<const crd::u8*>(words.data()), byte_count));
                    }

                    m_spirv_cache.emplace(diagnostics.spirv_key.value, CachedSpirv{words});
                }

                ReflectedData reflected;
                crd::containers::String reflect_error;
                if (!reflect_module(words, compile_request.stage, reflected, reflect_error))
                {
                    diagnostics.message = reflect_error;
                    return false;
                }

                module_handle = ModuleHandle{m_next_module_handle++};
                m_modules.emplace(module_handle.value,
                                  StoredModule(module_handle, compile_request.stage, compile_request.entry_point,
                                               std::move(words), compile_request.source_path, std::move(reflected),
                                               diagnostics.source_key, diagnostics.preprocessed_key,
                                               diagnostics.spirv_key));
                if (reuse_module_handles)
                {
                    m_module_cache.emplace(diagnostics.spirv_key.value, module_handle);
                }
            }

            const auto* module = find_module(module_handle);
            CRD_ASSERT(module != nullptr);
            for (const auto& parameter : module->parameters())
            {
                reflected_effect_desc.parameters.push_back(parameter);
            }
            for (const auto& descriptor_binding : module->descriptor_bindings())
            {
                reflected_effect_desc.descriptor_bindings.push_back(descriptor_binding);
            }
            for (const auto& push_constant : module->push_constants())
            {
                reflected_effect_desc.push_constants.push_back(push_constant);
            }
            for (const auto& vertex_attribute : module->vertex_attributes())
            {
                reflected_effect_desc.vertex_attributes.push_back(vertex_attribute);
            }

            stored_variant.modules.push_back(module_handle);
        }

        diagnostics.succeeded = true;
        diagnostics.message = crd::containers::String("GLSL frontend compiled, reflected, and cached");
        return true;
    }

    crd::u64 m_next_effect_handle = 1;
    crd::u64 m_next_variant_handle = 1;
    crd::u64 m_next_module_handle = 1;
    std::unordered_map<crd::u64, StoredEffect> m_effects{};
    std::unordered_map<crd::u64, StoredModule> m_modules{};
    std::unordered_map<crd::u64, StoredVariant> m_variants{};
    std::unordered_map<crd::u64, crd::containers::String> m_source_cache{};
    std::unordered_map<crd::u64, crd::containers::String> m_preprocessed_cache{};
    std::unordered_map<crd::u64, CachedSpirv> m_spirv_cache{};
    std::unordered_map<crd::u64, ModuleHandle> m_module_cache{};
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
