// DIAG.11a: an Application composes the engine's reusable diagnostic host (crd/perf/diagnostic_host.hpp) instead of
// installing its own crash handler: a started host given in ApplicationDesc owns the crash handler, every tick() is one
// of its frames (and delivers a capture whose window has closed), and run() gives the jobs pool the host's watchdog
// window. A host that is not started makes the Application invalid. These cases need a window, so a headless run
// (CRD_PLATFORM_HEADLESS=1) skips them; the host itself is covered without a window by crd-perf-tests.

#include <crd/app/app.hpp>
#include <crd/core/lifetime_ledger.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/platform/context.hpp>

#include <catch2/catch_test_macros.hpp>


namespace
{
namespace cp = crd::perf;

class CloseAfter final : public crd::app::Layer
{
public:
    CloseAfter(crd::app::Application& app, int updates) : Layer("CloseAfter"), m_app(app), m_left(updates) {}

    void on_update(crd::f64 /*dt*/) override
    {
        --m_left;
        if (m_left <= 0)
        {
            m_app.close();
        }
    }

private:
    crd::app::Application& m_app;
    int                    m_left;
};

cp::DiagnosticHostProfile windowed_profile()
{
    cp::DiagnosticHostProfile p;
    p.crash_handler = cp::DiagMode::Off; // the host owns the choice; this process keeps its own handlers
    return p;
}
} // namespace

TEST_CASE("diag 11a: an Application feeds a started diagnostic host one frame per tick", "[app][diag][diag11a]")
{
    if (crd::platform::headless_requested())
    {
        SKIP("CRD_PLATFORM_HEADLESS=1: the Application needs a window");
    }
    cp::DiagnosticHost      host;
    crd::containers::String reason;
    REQUIRE(host.start(windowed_profile(), reason) == cp::DiagHostStart::Started);
    {
        crd::app::ApplicationDesc desc;
        desc.window.visible = false;
        desc.window.title   = crd::containers::String("crd-app-tests diag host");
        desc.diagnostics    = &host;
        crd::app::Application app(desc);
        REQUIRE(app.is_valid());

        // A host that drives tick() itself (as the sandbox does) owns the pool, with the host's window.
        crd::jobs::Config cfg;
        cfg.num_threads = 2U;
        host.configure_jobs(cfg);
        crd::jobs::init(cfg);
        app.push_layer(std::make_unique<CloseAfter>(app, 4));
        CHECK(app.tick());
        CHECK(app.tick());
        CHECK(app.tick());
        CHECK(host.frames() == 3U);
        CHECK_FALSE(app.tick()); // the fourth update closes it; the tick still counts as a frame
        CHECK(host.frames() == 4U);
        crd::jobs::shutdown();
    }
    host.stop();
}

namespace
{
// Counts the threads the pool runs while the Application is inside run(): the lifetime ledger's Thread class counts
// every worker and the hang watchdog (DIAG.10c), so the watchdog shows as one thread more.
class CountThreads final : public crd::app::Layer
{
public:
    explicit CountThreads(crd::app::Application& app) : Layer("CountThreads"), m_app(app) {}

    void on_update(crd::f64 /*dt*/) override
    {
        live = crd::lifetime_ledger().snapshot().of(crd::LifetimeClass::Thread).live;
        m_app.close();
    }

    crd::u64 live = 0U;

private:
    crd::app::Application& m_app;
};

// Runs an Application with a two-thread pool (one worker besides the main thread) and returns how many threads the
// ledger counted live during its one frame, beyond those live before it started.
crd::u64 threads_during_run(cp::DiagnosticHost* host)
{
    const crd::u64            before = crd::lifetime_ledger().snapshot().of(crd::LifetimeClass::Thread).live;
    crd::app::ApplicationDesc desc;
    desc.window.visible          = false;
    desc.window.title            = crd::containers::String("crd-app-tests diag host run");
    desc.jobs_config.num_threads = 2U;
    desc.diagnostics             = host;
    desc.install_crash_handler   = false;
    crd::app::Application app(desc);
    REQUIRE(app.is_valid());
    auto  layer = std::make_unique<CountThreads>(app);
    auto* raw   = layer.get();
    app.push_layer(std::move(layer));
    app.run();
    CHECK(crd::jobs::num_workers() == 0U); // the pool is gone again
    return raw->live - before;
}
} // namespace

TEST_CASE("diag 11a: Application::run gives the pool the host's watchdog window", "[app][diag][diag11a]")
{
    if (crd::platform::headless_requested())
    {
        SKIP("CRD_PLATFORM_HEADLESS=1: the Application needs a window");
    }
    CHECK(threads_during_run(nullptr) == 1U); // the worker; no watchdog without a host asking for one

    cp::DiagnosticHost      host;
    crd::containers::String reason;
    REQUIRE(host.start(windowed_profile(), reason) == cp::DiagHostStart::Started);
    CHECK(threads_during_run(&host) == 2U); // the worker and the watchdog the host's triggers need
    CHECK(host.frames() == 1U);
    host.stop();
}

TEST_CASE("diag 11a: an Application refuses a diagnostic host that is not started", "[app][diag][diag11a]")
{
    if (crd::platform::headless_requested())
    {
        SKIP("CRD_PLATFORM_HEADLESS=1: the Application needs a window");
    }
    cp::DiagnosticHost        host; // never started
    crd::app::ApplicationDesc desc;
    desc.window.visible = false;
    desc.window.title   = crd::containers::String("crd-app-tests diag host refused");
    desc.diagnostics    = &host;
    crd::app::Application app(desc);
    REQUIRE_FALSE(app.is_valid()); // REQUIRE: a valid Application would tick without a jobs pool below
    CHECK_FALSE(app.tick());
    CHECK(host.frames() == 0U);
}
