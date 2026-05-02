#include <crd/platform/filesystem.hpp>
#include <crd/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] crd::containers::String source_path(const char* relative)
{
    return crd::containers::String((fs::Path(CRD_SOURCE_DIR) / relative).generic().data());
}

[[nodiscard]] fs::Path temp_shader_root()
{
    const auto base = fs::current_working_dir() / ".crd_shader_cache_tests";
    const auto stamp = static_cast<crd::u64>(std::chrono::system_clock::now().time_since_epoch().count());
    crd::containers::String leaf("cache_");
    leaf.reserve(leaf.size() + 32U);
    leaf.append(std::to_string(stamp));
    return base / leaf.c_str();
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
    desc.descriptor_bindings.push_back({0, 0, 1, crd::shader::stage_bit(crd::shader::Stage::Fragment)});
    desc.push_constants.push_back({0, 64, crd::shader::stage_bit(crd::shader::Stage::Vertex)});
    desc.vertex_attributes.push_back({crd::containers::String("POSITION"), 0, crd::rhi::Format::R32G32B32Sfloat, 0});

    const auto handle = runtime->create_effect(desc);
    REQUIRE(handle.is_valid());

    const auto* effect = runtime->find_effect(handle);
    REQUIRE(effect != nullptr);
    REQUIRE(effect->name() == "basic");
    REQUIRE(effect->parameters().size() == 1U);
    REQUIRE(effect->descriptor_bindings().size() == 1U);
    REQUIRE(effect->push_constants().size() == 1U);
    REQUIRE(effect->vertex_attributes().size() == 1U);
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
    REQUIRE(modules.size() == 2U);
    const auto* vertex_module = runtime->find_module(modules[0]);
    REQUIRE(vertex_module != nullptr);
    REQUIRE(vertex_module->stage() == crd::shader::Stage::Vertex);
    REQUIRE(vertex_module->code_size_bytes() > 0U);
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

TEST_CASE("Shader runtime consumes reflection metadata from compiled modules", "[shader]")
{
    auto runtime = crd::shader::create_runtime();

    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("reflect");
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/reflect_triangle.vert"),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/reflect_triangle.frag"),
                                     crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect_handle = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect_handle;
    request.variant.pass_type = crd::shader::PassType::MainColor;

    const auto variant = runtime->request_variant(request, diagnostics);
    REQUIRE(variant.is_valid());
    REQUIRE(diagnostics.succeeded);

    const auto* effect = runtime->find_effect(effect_handle);
    REQUIRE(effect != nullptr);
    REQUIRE(effect->descriptor_bindings().size() == 2U);
    REQUIRE(effect->push_constants().size() == 1U);
    REQUIRE(effect->vertex_attributes().size() == 2U);
    REQUIRE(effect->parameters().size() >= 3U);

    const auto modules = runtime->variant_modules(variant);
    REQUIRE(modules.size() == 2U);
    const auto* vertex_module = runtime->find_module(modules[0]);
    const auto* fragment_module = runtime->find_module(modules[1]);
    REQUIRE(vertex_module != nullptr);
    REQUIRE(fragment_module != nullptr);
    REQUIRE(vertex_module->descriptor_bindings().size() == 1U);
    REQUIRE(vertex_module->push_constants().size() == 1U);
    REQUIRE(vertex_module->vertex_attributes().size() == 2U);
    REQUIRE(fragment_module->descriptor_bindings().size() == 1U);
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

TEST_CASE("Variant key is stable for identical structural requests", "[shader]")
{
    const crd::shader::VariantRequest a{};
    const crd::shader::VariantRequest b{};
    REQUIRE(crd::shader::make_variant_key(a).value == crd::shader::make_variant_key(b).value);
}

TEST_CASE("Variant key changes when a structural axis changes", "[shader]")
{
    crd::shader::VariantRequest a{};
    crd::shader::VariantRequest b{};
    b.alpha_mode = crd::shader::AlphaMode::Translucent;
    REQUIRE(crd::shader::make_variant_key(a).value != crd::shader::make_variant_key(b).value);
}

TEST_CASE("Structural variant key ignores specialization values", "[shader]")
{
    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("forward");
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/triangle.vert"), crd::shader::Stage::Vertex,
                                     crd::containers::String("main")});
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/triangle.frag"),
                                     crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::SpecializationValue specials_a[] = {{0, 4}, {1, 64}};
    crd::shader::SpecializationValue specials_b[] = {{0, 8}, {1, 128}};

    crd::shader::VariantCompileRequest request_a;
    request_a.effect = effect;
    request_a.specialization_values = crd::containers::make_span(specials_a);
    crd::shader::VariantCompileRequest request_b;
    request_b.effect = effect;
    request_b.specialization_values = crd::containers::make_span(specials_b);

    crd::shader::CompileDiagnostics diagnostics_a;
    crd::shader::CompileDiagnostics diagnostics_b;
    const auto variant_a = runtime->request_variant(request_a, diagnostics_a);
    const auto variant_b = runtime->request_variant(request_b, diagnostics_b);
    REQUIRE(variant_a.is_valid());
    REQUIRE(variant_b.is_valid());
    REQUIRE(runtime->variant_key(variant_a).value == runtime->variant_key(variant_b).value);
}

TEST_CASE("Mechanism policy matches pinned decisions", "[shader]")
{
    REQUIRE(crd::shader::decide_mechanism(crd::shader::VariantAxis::PassType).mechanism ==
            crd::shader::Mechanism::Permutation);
    REQUIRE(crd::shader::decide_mechanism(crd::shader::VariantAxis::CascadeCount).mechanism ==
            crd::shader::Mechanism::SpecializationConstant);
    REQUIRE(crd::shader::decide_mechanism(crd::shader::VariantAxis::MaterialParameter).mechanism ==
            crd::shader::Mechanism::ResourceBinding);
    REQUIRE(crd::shader::decide_mechanism(crd::shader::VariantAxis::CheapRuntimeToggle).mechanism ==
            crd::shader::Mechanism::DynamicBranch);
}

TEST_CASE("Shader runtime records cache keys and hits on repeated compile", "[shader]")
{
    const auto root = temp_shader_root();
    const bool created_root = fs::create_directories(root) || fs::is_directory(root);
    REQUIRE(created_root);
    REQUIRE(fs::write_file_text(root / "triangle.vert", "#version 460\nlayout(location=0) in vec2 in_position;\nvoid "
                                                        "main(){ gl_Position=vec4(in_position,0.0,1.0); }\n"));
    REQUIRE(fs::write_file_text(
        root / "triangle.frag",
        "#version 460\nlayout(location=0) out vec4 out_color;\nvoid main(){ out_color=vec4(1.0); }\n"));

    auto runtime = crd::shader::create_runtime();

    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("cache_repeat");
    desc.frontend_modules.push_back({crd::containers::String((root / "triangle.vert").generic()),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back({crd::containers::String((root / "triangle.frag").generic()),
                                     crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics a;
    crd::shader::CompileDiagnostics b;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    const auto v1 = runtime->request_variant(request, a);
    const auto v2 = runtime->request_variant(request, b);
    REQUIRE(v1.is_valid());
    REQUIRE(v2.is_valid());
    REQUIRE(a.source_key.is_valid());
    REQUIRE(a.preprocessed_key.is_valid());
    REQUIRE(a.spirv_key.is_valid());
    REQUIRE(b.source_key.value == a.source_key.value);
    REQUIRE(b.preprocessed_key.value == a.preprocessed_key.value);
    REQUIRE(b.spirv_key.value == a.spirv_key.value);
    REQUIRE(b.spirv_cache_hit);

    REQUIRE(fs::remove_all(root));
}

TEST_CASE("Shader cache keys change when included source changes", "[shader]")
{
    const auto root = temp_shader_root();
    const bool created_root = fs::create_directories(root) || fs::is_directory(root);
    REQUIRE(created_root);

    REQUIRE(fs::write_file_text(root / "common.glsl", "vec3 get_color() { return vec3(1.0, 0.0, 0.0); }\n"));
    REQUIRE(fs::write_file_text(root / "main.vert",
                                "#version 460\n#include \"common.glsl\"\nlayout(location=0) in vec2 in_position;\n"
                                "layout(location=0) out vec3 v_color;\nvoid main(){ "
                                "gl_Position=vec4(in_position,0.0,1.0); v_color=get_color(); }\n"));

    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("include_case");
    desc.frontend_modules.push_back({crd::containers::String((root / "main.vert").generic()),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics first;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    const auto variant1 = runtime->request_variant(request, first);
    REQUIRE(variant1.is_valid());

    REQUIRE(fs::write_file_text(root / "common.glsl", "vec3 get_color() { return vec3(0.0, 1.0, 0.0); }\n"));

    crd::shader::CompileDiagnostics second;
    const auto variant2 = runtime->request_variant(request, second);
    REQUIRE(variant2.is_valid());
    REQUIRE(second.source_key.value == first.source_key.value);
    REQUIRE(second.preprocessed_key.value != first.preprocessed_key.value);
    REQUIRE(second.spirv_key.value != first.spirv_key.value);

    REQUIRE(fs::remove_all(root));
}

TEST_CASE("Failed compile does not populate SPIR-V cache hit", "[shader]")
{
    auto runtime = crd::shader::create_runtime();

    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("broken_cache");
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/broken_triangle.vert"),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics first;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    const auto v1 = runtime->request_variant(request, first);
    REQUIRE_FALSE(v1.is_valid());
    REQUIRE_FALSE(first.spirv_cache_hit);

    crd::shader::CompileDiagnostics second;
    const auto v2 = runtime->request_variant(request, second);
    REQUIRE_FALSE(v2.is_valid());
    REQUIRE_FALSE(second.spirv_cache_hit);
}

TEST_CASE("Shader runtime hot reload updates live data atomically", "[shader]")
{
    const auto root = temp_shader_root();
    const bool created_root = fs::create_directories(root) || fs::is_directory(root);
    REQUIRE(created_root);

    REQUIRE(fs::write_file_text(root / "triangle.vert",
                                "#version 460\nlayout(location=0) in vec2 in_position;\n"
                                "layout(location=0) out vec3 v_color;\n"
                                "void main(){ gl_Position=vec4(in_position,0.0,1.0); v_color=vec3(1.0,0.0,0.0); }\n"));
    REQUIRE(fs::write_file_text(
        root / "triangle.frag",
        "#version 460\nlayout(location=0) in vec3 v_color;\nlayout(location=0) out vec4 out_color;\n"
        "void main(){ out_color=vec4(v_color,1.0); }\n"));

    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("reload_case");
    desc.frontend_modules.push_back({crd::containers::String((root / "triangle.vert").generic()),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back({crd::containers::String((root / "triangle.frag").generic()),
                                     crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    const auto variant = runtime->request_variant(request, diagnostics);
    REQUIRE(variant.is_valid());
    const auto original_key = runtime->variant_key(variant);

    REQUIRE(fs::write_file_text(
        root / "triangle.frag",
        "#version 460\nlayout(location=0) in vec3 v_color;\nlayout(location=0) out vec4 out_color;\n"
        "layout(push_constant) uniform T { vec4 tint; } tint_data;\n"
        "void main(){ out_color=vec4(v_color,1.0)*tint_data.tint; }\n"));

    crd::shader::ReloadEvent event;
    REQUIRE(runtime->reload_effect(effect, event));
    REQUIRE(event.succeeded);
    REQUIRE_FALSE(event.using_last_good);
    REQUIRE(runtime->variant_key(variant).value == original_key.value);
    const auto* effect_view = runtime->find_effect(effect);
    REQUIRE(effect_view != nullptr);
    REQUIRE(effect_view->push_constants().size() == 1U);

    REQUIRE(fs::remove_all(root));
}

TEST_CASE("Failed hot reload preserves last-good state", "[shader]")
{
    const auto root = temp_shader_root();
    const bool created_root = fs::create_directories(root) || fs::is_directory(root);
    REQUIRE(created_root);

    REQUIRE(fs::write_file_text(root / "triangle.vert", "#version 460\nlayout(location=0) in vec2 in_position;\n"
                                                        "void main(){ gl_Position=vec4(in_position,0.0,1.0); }\n"));
    REQUIRE(fs::write_file_text(
        root / "triangle.frag",
        "#version 460\nlayout(location=0) out vec4 out_color;\nvoid main(){ out_color=vec4(1.0); }\n"));

    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("reload_fail_case");
    desc.frontend_modules.push_back({crd::containers::String((root / "triangle.vert").generic()),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back({crd::containers::String((root / "triangle.frag").generic()),
                                     crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    const auto variant = runtime->request_variant(request, diagnostics);
    REQUIRE(variant.is_valid());
    const auto original_modules = runtime->variant_modules(variant);
    REQUIRE(original_modules.size() == 2U);

    REQUIRE(fs::write_file_text(
        root / "triangle.frag",
        "#version 460\nlayout(location=0) out vec4 out_color;\nvoid main( { out_color=vec4(1.0); }\n"));

    crd::shader::ReloadEvent event;
    REQUIRE_FALSE(runtime->reload_effect(effect, event));
    REQUIRE_FALSE(event.succeeded);
    REQUIRE(event.using_last_good);
    const auto after_modules = runtime->variant_modules(variant);
    REQUIRE(after_modules.size() == original_modules.size());
    REQUIRE(after_modules[0].value == original_modules[0].value);
    REQUIRE(after_modules[1].value == original_modules[1].value);

    REQUIRE(fs::remove_all(root));
}

TEST_CASE("Shader runtime describes variant handoff for RHI/pipeline creation", "[shader]")
{
    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("handoff_case");
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/reflect_triangle.vert"),
                                     crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back({source_path("runtime/examples/shaders/reflect_triangle.frag"),
                                     crd::shader::Stage::Fragment, crd::containers::String("main")});
    const auto effect = runtime->create_effect(desc);

    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    const auto variant = runtime->request_variant(request, diagnostics);
    REQUIRE(variant.is_valid());

    crd::shader::VariantPipelineDesc handoff;
    REQUIRE(runtime->describe_variant(variant, handoff));
    REQUIRE(handoff.variant.value == variant.value);
    REQUIRE(handoff.modules.size() == 2U);
    REQUIRE(handoff.descriptor_bindings.size() == 2U);
    REQUIRE(handoff.push_constants.size() == 1U);
    REQUIRE(handoff.vertex_attributes.size() == 2U);
}
