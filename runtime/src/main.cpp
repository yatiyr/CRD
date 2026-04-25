#include <crd/core/core.hpp>
#include <crd/log/log.hpp>

#include <chrono>
#include <iostream>
#include <thread>

// Demo channels: imagine these belonging to future engine subsystems.
// In a real setup, each subsystem owns its own channel via CRD_DEFINE_LOG_CHANNEL
// inside its own .cpp file. We co-locate them here for the smoke test.
CRD_DEFINE_LOG_CHANNEL(g_log_engine, "Engine", crd::log::LogLevel::Trace)
CRD_DEFINE_LOG_CHANNEL(g_log_renderer, "Renderer", crd::log::LogLevel::Info)
CRD_DEFINE_LOG_CHANNEL(g_log_physics, "Physics", crd::log::LogLevel::Debug)
CRD_DEFINE_LOG_CHANNEL(g_log_audio, "Audio", crd::log::LogLevel::Warn)

int main()
{
    using namespace std::chrono_literals;

    const crd::u32 version_major = CRD_VERSION_MAJOR;
    const crd::u32 version_minor = CRD_VERSION_MINOR;
    const crd::u32 version_patch = CRD_VERSION_PATCH;

    // ---- Bring up the logger ------------------------------------------
    crd::log::LoggerConfig cfg;
    cfg.async = true;
    cfg.async_queue_capacity = 1024;
    cfg.flush_on_critical = true;
    crd::log::init(cfg);

    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());
    crd::log::add_sink(
        std::make_unique<crd::log::FileSink>("engine.log", /*max_bytes*/ 1ull * 1024ull * 1024ull, /*max_files*/ 3));

    // ---- Smoke test ---------------------------------------------------
    CRD_LOG_INFO(g_log_engine, "CRD Engine v{}.{}.{} starting up", version_major, version_minor, version_patch);
    CRD_LOG_INFO(g_log_engine, "platform={} compiler={} arch={}", crd::platform_name(), crd::compiler_name(),
                 crd::arch_name());

    CRD_LOG_TRACE(g_log_engine, "trace: this is the chattiest level");
    CRD_LOG_DEBUG(g_log_engine, "debug: useful while building things");

    CRD_LOG_INFO(g_log_renderer, "Renderer initialised, backend={}", "Vulkan");
    CRD_LOG_DEBUG(g_log_physics, "physics tick budget = {} ms", 4);
    CRD_LOG_WARN(g_log_audio, "no audio device found, falling back to silence");

    // Filtered: Audio's runtime level is Warn, so this Info is dropped at producer.
    CRD_LOG_INFO(g_log_audio, "(this should NOT appear)");

    // Cross-thread: pretend a couple of worker threads emit logs.
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

    std::cout << "(runtime exiting cleanly)\n";
    return 0;
}
