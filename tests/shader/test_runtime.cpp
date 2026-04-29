#include <crd/platform/filesystem.hpp>
#include <crd/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] crd::containers::String source_path(const char* relative)
{
    return crd::containers::String((fs::Path(CRD_SOURCE_DIR) / relative).generic().data());
}
} // namespace

TEST_CASE("Shader runtime creates effects and preserves metadata", "[shader]")
{
    auto runtime = crd::shader::create_runtime();
    REQUIRE(runtime != nullptr);

    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("basic");
    desc.source_path = crd::containers::String("shaders/basic.shader");
    desc.parameters.push_back({crd::containers::String("albedo"), crd::shader::ParameterClass::Texture, 0, 0, 0});
    desc.descriptor_bindings.push_back({0, 0, 1, crd::shader::Stage::Fragment});
    desc.push_constants.push_back({0, 64, crd::shader::Stage::Vertex});
    desc.vertex_attributes.push_back({crd::containers::String("POSITION"), 0, crd::rhi::Format::R32G32B32Sfloat, 0});

    const auto handle = runtime->create_effect(desc);
    REQUIRE(handle.is_valid());

    const auto* effect = runtime->find_effect(handle);
    REQUIRE(effect != nullptr);
    REQUIRE(effect->name() == "basic");
    REQUIRE(effect->parameters().size() == 1u);
    REQUIRE(effect->descriptor_bindings().size() == 1u);
    REQUIRE(effect->push_constants().size() == 1u);
    REQUIRE(effect->vertex_attributes().size() == 1u);
}

TEST_CASE("Shader runtime issues opaque variant handles", "[shader]")
{
    auto runtime = crd::shader::create_runtime();

    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("forward");
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/triangle.vert"), crd::shader::Stage::Vertex,
                                     crd::containers::String("main")});
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/triangle.frag"),
                                     crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    request.variant.pass_type = crd::shader::PassType::MainColor;
    request.variant.skinned = false;
    request.variant.alpha_mode = crd::shader::AlphaMode::Opaque;
    request.variant.render_path = crd::shader::RenderPath::Forward;

    const auto variant = runtime->request_variant(request, diagnostics);
    REQUIRE(variant.is_valid());
    REQUIRE(diagnostics.succeeded);
    REQUIRE(runtime->is_variant_ready(variant));
    const auto modules = runtime->variant_modules(variant);
    REQUIRE(modules.size() == 2u);
    const auto* vertex_module = runtime->find_module(modules[0]);
    REQUIRE(vertex_module != nullptr);
    REQUIRE(vertex_module->stage() == crd::shader::Stage::Vertex);
    REQUIRE(vertex_module->code_size_bytes() > 0u);
}

TEST_CASE("Shader runtime reports bad effect handles without crashing", "[shader]")
{
    auto runtime = crd::shader::create_runtime();

    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = {999};

    const auto variant = runtime->request_variant(request, diagnostics);
    REQUIRE_FALSE(variant.is_valid());
    REQUIRE_FALSE(diagnostics.succeeded);
}

TEST_CASE("Shader runtime reports GLSL compile failures non-fatally", "[shader]")
{
    auto runtime = crd::shader::create_runtime();

    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("broken");
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/broken_triangle.vert"),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;

    const auto variant = runtime->request_variant(request, diagnostics);
    REQUIRE_FALSE(variant.is_valid());
    REQUIRE_FALSE(diagnostics.succeeded);
    REQUIRE_FALSE(diagnostics.message.empty());
}

TEST_CASE("Shader runtime exposes observable reload result", "[shader]")
{
    auto runtime = crd::shader::create_runtime();
    const auto handle = runtime->create_effect({crd::containers::String("reloadable")});

    crd::shader::ReloadEvent event;
    REQUIRE(runtime->reload_effect(handle, event));
    REQUIRE(event.succeeded);
    REQUIRE(event.effect.value == handle.value);
    REQUIRE_FALSE(event.using_last_good);
}
