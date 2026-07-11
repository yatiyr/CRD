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

// D-008 C2-e (ADR-0103 I1): crd-shader owns NO shading-language compiler. The Effect frontend's GLSL→SPIR-V step is
// INJECTED — a backend-linking caller supplies an `ISpirvCompiler` (crd-shader-vulkan wraps `crd::gpu::compile_glsl_to_spirv`).
// crd-shader still reads + preprocesses the GLSL text and reflects the returned SPIR-V (spirv-reflect — bytecode, not a
// language), but it never names shaderc/dxc or a shading language. Compile for REFLECTION (unoptimized: `OpName`s + dead
// bindings must survive).
class ISpirvCompiler
{
public:
    virtual ~ISpirvCompiler() = default;

    // Compile GLSL `source` (already #include-expanded) for `stage` into SPIR-V words. `name` is diagnostics-only.
    // Return false + set `error` on failure. Entry point is `main` (the GLSL convention).
    [[nodiscard]] virtual bool compile(Stage stage, crd::containers::StringView source,
                                       crd::containers::StringView name, crd::containers::Array<crd::u32>& out_words,
                                       crd::containers::String& error) = 0;
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

// D-008 C2-e: the Effect runtime needs an injected `ISpirvCompiler` (crd-shader owns no compiler). `compiler` must
// OUTLIVE the returned Runtime — it is borrowed, not owned. Wire it with `crd::shader::create_vulkan_spirv_compiler()`
// (crd-shader-vulkan) at the app/renderer layer.
[[nodiscard]] std::unique_ptr<Runtime> create_runtime(ISpirvCompiler& compiler);
} // namespace crd::shader
