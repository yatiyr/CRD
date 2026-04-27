#include <crd/app/app.hpp>
#include <crd/log/log.hpp>

#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_smoke_app, "SmokeApp", crd::log::LogLevel::Trace)

namespace
{
class DemoLayer final : public crd::app::Layer
{
public:
    explicit DemoLayer(crd::app::Application& app) : Layer("DemoLayer"), m_app(app) {}

    void on_attach() override
    {
        m_tick_subscription =
            m_app.event_bus().subscribe<crd::app::AppTickEvent>([this](crd::app::AppTickEvent&) { ++m_ticks; });
    }

    void on_detach() override { m_app.event_bus().unsubscribe(m_tick_subscription); }

    void on_event(crd::app::Event& event) override
    {
        crd::app::EventDispatcher dispatcher(event);
        (void)dispatcher.dispatch<crd::app::KeyPressedEvent>(
            [this](crd::app::KeyPressedEvent& e)
            {
                if (e.key() == crd::platform::Key::Escape)
                {
                    CRD_LOG_INFO(g_log_smoke_app, "ESC pressed after {} ticks", m_ticks);
                    m_app.close();
                    return true;
                }
                return false;
            });
    }

private:
    crd::app::Application& m_app;
    crd::app::EventSubscription m_tick_subscription{};
    crd::u64 m_ticks = 0;
};
} // namespace

int main()
{
    crd::log::LoggerConfig cfg;
    cfg.async = false;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    crd::app::ApplicationDesc desc;
    desc.window.title = crd::containers::String("Cerid - smoke_app");

    crd::app::Application app(desc);
    if (!app.is_valid())
    {
        CRD_LOG_ERROR(g_log_smoke_app, "Application failed to initialise");
        crd::log::shutdown();
        return 1;
    }

    app.push_layer(std::make_unique<DemoLayer>(app));
    app.run();

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
