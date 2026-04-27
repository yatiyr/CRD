#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_fs, "SmokeFS", crd::log::LogLevel::Trace)

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    const auto cwd = crd::platform::fs::current_working_dir();
    const auto exe = crd::platform::fs::executable_dir();
    const auto cfg_dir = crd::platform::fs::user_config_dir("Cerid");

    CRD_LOG_INFO(g_log_smoke_fs, "cwd={}", cwd.generic().data());
    CRD_LOG_INFO(g_log_smoke_fs, "exe_dir={}", exe.generic().data());
    CRD_LOG_INFO(g_log_smoke_fs, "config_dir={}", cfg_dir.generic().data());

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
