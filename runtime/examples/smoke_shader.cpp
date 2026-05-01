#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/shader/shader.hpp>

#include <chrono>
#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_shader, "SmokeShader", crd::log::LogLevel::Trace)

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] fs::Path temp_root()
{
    const auto base = fs::current_working_dir() / ".crd_shader_reload_smoke";
    const auto stamp = static_cast<crd::u64>(std::chrono::system_clock::now().time_since_epoch().count());
    crd::containers::String leaf("reload_");
    leaf.reserve(leaf.size() + 32u);
    leaf.append(std::to_string(stamp));
    return base / leaf.c_str();
}
} // namespace

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    const auto root = temp_root();
    (void)fs::create_directories(root);
    const auto vert_path = root / "reflect_triangle.vert";
    const auto frag_path = root / "reflect_triangle.frag";

    crd::containers::String vert_source;
    crd::containers::String frag_source;
    if (!fs::read_file_text(fs::Path(CRD_SOURCE_DIR) / "runtime/examples/shaders/reflect_triangle.vert", vert_source) ||
        !fs::read_file_text(fs::Path(CRD_SOURCE_DIR) / "runtime/examples/shaders/reflect_triangle.frag", frag_source))
    {
        CRD_LOG_ERROR(g_log_smoke_shader, "Failed to read source shaders");
        return 1;
    }
    (void)fs::write_file_text(vert_path, vert_source);
    (void)fs::write_file_text(frag_path, frag_source);

    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("reflect_effect");
    desc.source_path = crd::containers::String(vert_path.generic());
    desc.frontend_modules.push_back(
        {crd::containers::String(vert_path.generic()), crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back(
        {crd::containers::String(frag_path.generic()), crd::shader::Stage::Fragment, crd::containers::String("main")});
    desc.vertex_attributes.push_back({crd::containers::String("POSITION"), 0, crd::rhi::Format::R32G32Sfloat, 0});
    desc.vertex_attributes.push_back({crd::containers::String("COLOR"), 1, crd::rhi::Format::R32G32B32Sfloat, 8});

    const auto effect = runtime->create_effect(desc);
    crd::shader::CompileDiagnostics diagnostics;
    crd::shader::VariantCompileRequest request;
    request.effect = effect;
    request.variant.pass_type = crd::shader::PassType::MainColor;
    request.variant.alpha_mode = crd::shader::AlphaMode::Opaque;
    request.variant.render_path = crd::shader::RenderPath::Forward;
    const auto variant = runtime->request_variant(request, diagnostics);
    crd::shader::CompileDiagnostics diagnostics2;
    const auto variant2 = runtime->request_variant(request, diagnostics2);
    const auto modules = runtime->variant_modules(variant);
    const auto* effect_view = runtime->find_effect(effect);
    const auto key = runtime->variant_key(variant);

    const auto pass_policy = crd::shader::decide_mechanism(crd::shader::VariantAxis::PassType);
    const auto material_policy = crd::shader::decide_mechanism(crd::shader::VariantAxis::MaterialParameter);

    CRD_LOG_INFO(g_log_smoke_shader, "effect={} variant={} ok={} msg='{}'", effect.value, variant.value,
                 diagnostics.succeeded, diagnostics.message.c_str());
    CRD_LOG_INFO(g_log_smoke_shader, "variant_key={}", key.value);
    CRD_LOG_INFO(g_log_smoke_shader, "module_count={}", modules.size());
    CRD_LOG_INFO(g_log_smoke_shader, "source_key={} preprocessed_key={} spirv_key={} hit={}",
                 diagnostics.source_key.value, diagnostics.preprocessed_key.value, diagnostics.spirv_key.value,
                 diagnostics.spirv_cache_hit);
    CRD_LOG_INFO(g_log_smoke_shader, "second_compile spirv_hit={} variant2={}", diagnostics2.spirv_cache_hit,
                 variant2.value);
    CRD_LOG_INFO(g_log_smoke_shader, "pass_policy={} material_policy={}", static_cast<int>(pass_policy.mechanism),
                 static_cast<int>(material_policy.mechanism));
    if (effect_view != nullptr)
    {
        CRD_LOG_INFO(g_log_smoke_shader, "descriptors={} push_constants={} vertex_attributes={}",
                     effect_view->descriptor_bindings().size(), effect_view->push_constants().size(),
                     effect_view->vertex_attributes().size());
    }

    (void)fs::write_file_text(
        frag_path, "#version 460\nlayout(location = 0) in vec3 v_color;\nlayout(location = 0) out vec4 out_color;\n"
                   "void main(){ out_color = vec4(v_color, 1.0); }\n");
    crd::shader::ReloadEvent ok_reload;
    const bool reloaded_ok = runtime->reload_effect(effect, ok_reload);
    CRD_LOG_INFO(g_log_smoke_shader, "reload_ok={} using_last_good={}", reloaded_ok, ok_reload.using_last_good);

    (void)fs::write_file_text(
        frag_path, "#version 460\nlayout(location = 0) in vec3 v_color;\nlayout(location = 0) out vec4 out_color;\n"
                   "void main( { out_color = vec4(v_color, 1.0); }\n");
    crd::shader::ReloadEvent failed_reload;
    const bool reloaded_fail = runtime->reload_effect(effect, failed_reload);
    CRD_LOG_INFO(g_log_smoke_shader, "reload_fail={} using_last_good={}", reloaded_fail, failed_reload.using_last_good);

    (void)fs::remove_all(root);

    crd::log::flush();
    crd::log::shutdown();
    return diagnostics.succeeded ? 0 : 1;
}
