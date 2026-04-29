#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/shader/shader.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_shader, "SmokeShader", crd::log::LogLevel::Trace)

namespace fs = crd::platform::fs;

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    auto runtime = crd::shader::create_runtime();
    crd::shader::EffectDesc desc;
    desc.name = crd::containers::String("reflect_effect");
    desc.source_path = crd::containers::String("runtime/examples/shaders/reflect_triangle.vert");
    desc.frontend_modules.push_back(
        {crd::containers::String(
             (fs::Path(CRD_SOURCE_DIR) / "runtime/examples/shaders/reflect_triangle.vert").generic().data()),
         crd::shader::Stage::Vertex, crd::containers::String("main")});
    desc.frontend_modules.push_back(
        {crd::containers::String(
             (fs::Path(CRD_SOURCE_DIR) / "runtime/examples/shaders/reflect_triangle.frag").generic().data()),
         crd::shader::Stage::Fragment, crd::containers::String("main")});
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
    const auto modules = runtime->variant_modules(variant);
    const auto* effect_view = runtime->find_effect(effect);

    CRD_LOG_INFO(g_log_smoke_shader, "effect={} variant={} ok={} msg='{}'", effect.value, variant.value,
                 diagnostics.succeeded, diagnostics.message.c_str());
    CRD_LOG_INFO(g_log_smoke_shader, "module_count={}", modules.size());
    if (effect_view != nullptr)
    {
        CRD_LOG_INFO(g_log_smoke_shader, "descriptors={} push_constants={} vertex_attributes={}",
                     effect_view->descriptor_bindings().size(), effect_view->push_constants().size(),
                     effect_view->vertex_attributes().size());
    }

    crd::log::flush();
    crd::log::shutdown();
    return diagnostics.succeeded ? 0 : 1;
}
