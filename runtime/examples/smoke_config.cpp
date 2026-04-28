#include <crd/config/config.hpp>
#include <crd/log/log.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_config, "SmokeConfig", crd::log::LogLevel::Trace)

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    crd::config::Config config;
    if (!config.load_from_file(crd::platform::fs::Path("engine/config/sample.toml")))
    {
        CRD_LOG_ERROR(g_log_smoke_config, "Failed to load sample config");
        return 1;
    }

    const auto preset = config.get<crd::containers::String>("imgui.theme.preset", crd::containers::String{});
    const auto fps = config.get<int>("app.target_fps", 0);
    const auto clear = config.get<crd::math::Vec4f>("renderer.clear_color", {});

    CRD_LOG_INFO(g_log_smoke_config, "preset={} target_fps={} clear=({}, {}, {}, {})", preset.c_str(), fps, clear.x,
                 clear.y, clear.z, clear.w);

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
