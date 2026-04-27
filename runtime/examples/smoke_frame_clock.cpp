// smoke_frame_clock — runs a synthetic 5-frame loop, prints per-frame
// delta and total time. No window, no GLFW: this exercises only the
// chrono-based timing facade.

#include <crd/log/log.hpp>
#include <crd/platform/timer.hpp>

#include <chrono>
#include <memory>
#include <thread>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke, "Smoke", crd::log::LogLevel::Trace)

int main()
{
    using namespace std::chrono_literals;

    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    crd::platform::FrameClock clock;
    for (int i = 0; i < 5; ++i)
    {
        clock.tick();
        CRD_LOG_INFO(g_log_smoke, "frame={} delta_ms={:.3f} total_ms={:.3f}", clock.frame_count(),
                     clock.delta_seconds() * 1000.0, clock.total_seconds() * 1000.0);
        std::this_thread::sleep_for(8ms);
    }

    crd::platform::Timer t;
    std::this_thread::sleep_for(10ms);
    CRD_LOG_INFO(g_log_smoke, "Timer elapsed after 10ms sleep: {:.3f} ms", t.elapsed_milliseconds());

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
