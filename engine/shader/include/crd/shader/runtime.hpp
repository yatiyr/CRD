#pragma once

#include <crd/shader/effect.hpp>

#include <memory>

namespace crd::shader
{
struct VariantCompileRequest
{
    EffectHandle effect{};
    VariantRequest variant{};
    crd::containers::ConstSpan<SpecializationValue> specialization_values{};
};

class Runtime
{
public:
    virtual ~Runtime() = default;

    [[nodiscard]] virtual EffectHandle create_effect(const EffectDesc& desc) = 0;
    [[nodiscard]] virtual const Effect* find_effect(EffectHandle handle) const noexcept = 0;
    [[nodiscard]] virtual const Module* find_module(ModuleHandle handle) const noexcept = 0;
    [[nodiscard]] virtual VariantHandle request_variant(const VariantCompileRequest& request,
                                                        CompileDiagnostics& diagnostics) = 0;
    [[nodiscard]] virtual bool is_variant_ready(VariantHandle handle) const noexcept = 0;
    [[nodiscard]] virtual VariantKey variant_key(VariantHandle handle) const noexcept = 0;
    [[nodiscard]] virtual bool describe_variant(VariantHandle handle, VariantPipelineDesc& out) const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<ModuleHandle>
    variant_modules(VariantHandle handle) const noexcept = 0;
    [[nodiscard]] virtual bool reload_effect(EffectHandle handle, ReloadEvent& event) = 0;
};

[[nodiscard]] std::unique_ptr<Runtime> create_runtime();
} // namespace crd::shader
