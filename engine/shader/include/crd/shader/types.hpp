#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/rhi/types.hpp>

namespace crd::shader
{
enum class Stage : crd::u8
{
    Vertex,
    Fragment,
    Compute,
};

[[nodiscard]] constexpr crd::u32 stage_bit(Stage stage) noexcept
{
    switch (stage)
    {
        case Stage::Vertex:
            return 1u << 0u;
        case Stage::Fragment:
            return 1u << 1u;
        case Stage::Compute:
        default:
            return 1u << 2u;
    }
}

enum class ParameterClass : crd::u8
{
    Texture,
    Sampler,
    Buffer,
    PushConstant,
    SpecializationConstant,
};

enum class AlphaMode : crd::u8
{
    Opaque,
    Masked,
    Translucent,
};

enum class RenderPath : crd::u8
{
    Forward,
    Deferred,
};

enum class PassType : crd::u8
{
    MainColor,
    Shadow,
    DepthPrepass,
};

struct ModuleHandle
{
    crd::u64 value = 0;
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
};

struct SourceKey
{
    crd::u64 value = 0;
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
};

struct PreprocessedKey
{
    crd::u64 value = 0;
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
};

struct SpirvKey
{
    crd::u64 value = 0;
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
};

struct EffectHandle
{
    crd::u64 value = 0;
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
};

struct VariantHandle
{
    crd::u64 value = 0;
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
};

struct ParameterDesc
{
    crd::containers::String name{};
    ParameterClass parameter_class = ParameterClass::Texture;
    crd::u32 set_index = 0;
    crd::u32 binding = 0;
    crd::u32 size_bytes = 0;
};

struct DescriptorBindingDesc
{
    crd::u32 set_index = 0;
    crd::u32 binding = 0;
    crd::u32 count = 1;
    crd::u32 visibility_mask = stage_bit(Stage::Fragment);
};

struct PushConstantRangeDesc
{
    crd::u32 offset = 0;
    crd::u32 size_bytes = 0;
    crd::u32 visibility_mask = stage_bit(Stage::Vertex);
};

struct VertexAttributeLayoutDesc
{
    crd::containers::String semantic{};
    crd::u32 location = 0;
    crd::rhi::Format format = crd::rhi::Format::Undefined;
    crd::u32 offset_bytes = 0;
};

struct ShaderModuleUsageDesc
{
    ModuleHandle module{};
    Stage stage = Stage::Vertex;
    crd::containers::String entry_point{};
};

struct VariantPipelineDesc
{
    VariantHandle variant{};
    crd::containers::Array<ShaderModuleUsageDesc> modules{};
    crd::containers::Array<DescriptorBindingDesc> descriptor_bindings{};
    crd::containers::Array<PushConstantRangeDesc> push_constants{};
    crd::containers::Array<VertexAttributeLayoutDesc> vertex_attributes{};
};

struct VariantKey
{
    crd::u64 value = 0;
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
};

enum class VariantAxis : crd::u8
{
    PassType,
    Skinning,
    AlphaMode,
    RenderPath,
    CascadeCount,
    LightCap,
    LoopBound,
    MaterialParameter,
    CheapRuntimeToggle,
};

enum class Mechanism : crd::u8
{
    Permutation,
    SpecializationConstant,
    ResourceBinding,
    DynamicBranch,
    Rejected,
};

struct MechanismDecision
{
    VariantAxis axis = VariantAxis::PassType;
    Mechanism mechanism = Mechanism::Rejected;
    crd::containers::String reason{};
};

struct VariantRequest
{
    PassType pass_type = PassType::MainColor;
    bool skinned = false;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    RenderPath render_path = RenderPath::Forward;
};

struct SpecializationValue
{
    crd::u32 constant_id = 0;
    crd::u64 value = 0;
};

[[nodiscard]] VariantKey make_variant_key(const VariantRequest& request) noexcept;
[[nodiscard]] MechanismDecision decide_mechanism(VariantAxis axis) noexcept;

struct FrontendCompileRequest
{
    crd::containers::String source_path{};
    Stage stage = Stage::Vertex;
    crd::containers::String entry_point{"main"};
};

struct CompileDiagnostics
{
    bool succeeded = false;
    crd::containers::String message{};
    SourceKey source_key{};
    PreprocessedKey preprocessed_key{};
    SpirvKey spirv_key{};
    bool source_cache_hit = false;
    bool preprocessed_cache_hit = false;
    bool spirv_cache_hit = false;
};

struct ReloadEvent
{
    EffectHandle effect{};
    bool succeeded = false;
    bool using_last_good = false;
};
} // namespace crd::shader
