#include <crd/core/core.hpp>
#include <crd/log/log.hpp>

#include <chrono>
#include <thread>

CRD_DEFINE_LOG_CHANNEL(g_log_engine, "Engine", crd::log::LogLevel::Trace)
CRD_DEFINE_LOG_CHANNEL(g_log_renderer, "Renderer", crd::log::LogLevel::Info)
CRD_DEFINE_LOG_CHANNEL(g_log_physics, "Physics", crd::log::LogLevel::Debug)
CRD_DEFINE_LOG_CHANNEL(g_log_audio, "Audio", crd::log::LogLevel::Warn)

int main()
{
    using namespace std::chrono_literals;

    crd::log::LoggerConfig cfg;
    cfg.async = true;
    cfg.async_queue_capacity = 1024;
    cfg.flush_on_critical = true;
    crd::log::init(cfg);

    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());
    crd::log::add_sink(
        std::make_unique<crd::log::FileSink>("engine.log", /*max_bytes*/ 1ULL * 1024ULL * 1024ULL, /*max_files*/ 3));

    CRD_LOG_INFO(g_log_engine, "platform={} compiler={} arch={}", crd::platform_name(), crd::compiler_name(),
                 crd::arch_name());
    CRD_LOG_TRACE(g_log_engine, "trace: this is the chattiest level");
    CRD_LOG_DEBUG(g_log_engine, "debug: useful while building things");
    CRD_LOG_INFO(g_log_renderer, "Renderer initialised, backend={}", "Vulkan");
    CRD_LOG_DEBUG(g_log_physics, "physics tick budget = {} ms", 4);
    CRD_LOG_WARN(g_log_audio, "no audio device found, falling back to silence");
    CRD_LOG_INFO(g_log_audio, "(this should NOT appear)");

    std::thread t1(
        []
        {
            for (int i = 0; i < 3; ++i)
            {
                CRD_LOG_INFO(g_log_renderer, "frame {} submitted", i);
                std::this_thread::sleep_for(2ms);
            }
        });
    std::thread t2(
        []
        {
            for (int i = 0; i < 3; ++i)
            {
                CRD_LOG_DEBUG(g_log_physics, "stepped {} bodies", 32 + i);
                std::this_thread::sleep_for(2ms);
            }
        });

    t1.join();
    t2.join();

    CRD_LOG_ERROR(g_log_renderer, "shader compile failed: '{}'", "tonemap.frag");
    CRD_LOG_CRITICAL(g_log_engine, "fatal subsystem failure simulated -- shutting down");

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
