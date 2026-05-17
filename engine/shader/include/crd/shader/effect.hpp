#pragma once

#include <crd/shader/types.hpp>

namespace crd::shader
{
struct EffectDesc
{
    crd::containers::String name{};
    crd::containers::String source_path{};
    crd::containers::Array<FrontendCompileRequest> frontend_modules{};
    crd::containers::Array<ParameterDesc> parameters{};
    crd::containers::Array<DescriptorBindingDesc> descriptor_bindings{};
    crd::containers::Array<PushConstantRangeDesc> push_constants{};
    crd::containers::Array<VertexAttributeLayoutDesc> vertex_attributes{};
};

class Effect
{
public:
    virtual ~Effect() = default;

    [[nodiscard]] virtual EffectHandle handle() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::StringView name() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<ParameterDesc> parameters() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<DescriptorBindingDesc> descriptor_bindings() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<PushConstantRangeDesc> push_constants() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<VertexAttributeLayoutDesc> vertex_attributes() const noexcept = 0;
};

class Module
{
public:
    virtual ~Module() = default;

    [[nodiscard]] virtual ModuleHandle handle() const noexcept = 0;
    [[nodiscard]] virtual Stage stage() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::StringView entry_point() const noexcept = 0;
    [[nodiscard]] virtual crd::u64 code_size_bytes() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<crd::u8> code_bytes() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<ParameterDesc> parameters() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<DescriptorBindingDesc> descriptor_bindings() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<PushConstantRangeDesc> push_constants() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<VertexAttributeLayoutDesc> vertex_attributes() const noexcept = 0;
    // Phase 3.1.7.6 v0e — populated for Stage::Compute modules only;
    // std::nullopt for Stage::Vertex / Stage::Fragment (those genuinely
    // have no workgroup size — `optional` is the honest sentinel).
    [[nodiscard]] virtual std::optional<WorkgroupSize> workgroup_size() const noexcept = 0;
    // Phase 3.1.7.6 v0e — per-module spec const reflection. Non-empty
    // for any stage that uses `layout(constant_id = N) const T x = v;`.
    // Per-module (not auto-merged across an Effect's modules; that's a
    // consumer-side aggregation if needed).
    [[nodiscard]] virtual crd::containers::ConstSpan<SpecializationConstantReflection>
    specialization_constants() const noexcept = 0;
};
} // namespace crd::shader
