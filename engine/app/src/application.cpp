#include <crd/app/application.hpp>
#include <crd/app/events/app_events.hpp>
#include <crd/app/events/input_events.hpp>
#include <crd/app/events/window_events.hpp>

namespace crd::app
{
Application::Application(const ApplicationDesc& desc)
    : m_desc(desc), m_context(crd::platform::PlatformContext::create())
{
    if (m_desc.install_crash_handler)
        crd::crash::install(m_desc.crash_dir);

    if (!m_context.is_valid())
    {
        return;
    }

    auto created_window = crd::platform::Window::create(m_context, m_desc.window);
    if (!created_window.is_valid())
    {
        return;
    }

    m_window = std::make_unique<crd::platform::Window>(std::move(created_window));
    m_window->input().enable_event_queue(m_desc.platform_event_queue_capacity);
    m_valid = true;
    m_running = true;
}

Application::~Application() noexcept
{
    for (auto it = m_layer_stack.rbegin(); it != m_layer_stack.rend(); ++it)
    {
        (*it)->on_detach();
    }

    if (m_desc.install_crash_handler)
        crd::crash::uninstall();
}

void Application::run()
{
    if (!m_valid)
        return;
    crd::jobs::init(m_desc.jobs_config);
    while (tick())
    {
    }
    crd::jobs::shutdown();
}

void Application::detach_layer(Layer* layer)
{
    layer->on_detach();
    m_layer_stack.pop_layer(layer);
}

void Application::detach_all_layers()
{
    for (auto it = m_layer_stack.rbegin(); it != m_layer_stack.rend(); ++it)
    {
        (*it)->on_detach();
    }

    m_layer_stack.clear_all_layers();
}

bool Application::tick()
{
    if (!m_valid || !m_running)
    {
        return false;
    }

    m_clock.tick();
    m_window->poll_input();
    m_context.poll_events();
    for (Layer* layer : m_layer_stack)
    {
        layer->on_frame_begin();
    }
    dispatch_platform_events();

    if (!m_running)
    {
        return false;
    }

    AppTickEvent tick_event;
    m_event_bus.publish(tick_event);

    AppUpdateEvent update_event(m_clock.delta_seconds());
    m_event_bus.publish(update_event);
    for (Layer* layer : m_layer_stack)
    {
        layer->on_update(m_clock.delta_seconds());
    }

    if (!m_running)
    {
        return false;
    }

    AppRenderEvent render_event;
    m_event_bus.publish(render_event);
    for (Layer* layer : m_layer_stack)
    {
        layer->on_render();
    }

    return m_running;
}

void Application::close() noexcept
{
    m_running = false;
    if (m_window != nullptr && m_window->is_valid())
    {
        m_window->request_close();
    }
}

void Application::push_layer(std::unique_ptr<Layer> layer)
{
    CRD_ASSERT(layer != nullptr);
    Layer* raw = layer.get();
    m_owned_layers.push_back(std::move(layer));
    m_layer_stack.push_layer(raw);
    raw->on_attach();
}

void Application::push_overlay(std::unique_ptr<Layer> overlay)
{
    CRD_ASSERT(overlay != nullptr);
    Layer* raw = overlay.get();
    m_owned_layers.push_back(std::move(overlay));
    m_layer_stack.push_overlay(raw);
    raw->on_attach();
}

void Application::dispatch_propagated(Event& event)
{
    for (auto it = m_layer_stack.rbegin(); it != m_layer_stack.rend(); ++it)
    {
        (*it)->on_event(event);
        if (event.handled)
        {
            break;
        }
    }
}

void Application::dispatch_platform_events()
{
    crd::platform::InputEvent raw{};
    while (m_window->input().try_pop_event(raw))
    {
        switch (raw.type)
        {
            case crd::platform::InputEvent::Type::KeyDown:
            {
                KeyPressedEvent event(raw.payload.key.key, raw.mods, false);
                dispatch_propagated(event);
                break;
            }
            case crd::platform::InputEvent::Type::KeyRepeat:
            {
                KeyPressedEvent event(raw.payload.key.key, raw.mods, true);
                dispatch_propagated(event);
                break;
            }
            case crd::platform::InputEvent::Type::KeyUp:
            {
                KeyReleasedEvent event(raw.payload.key.key, raw.mods);
                dispatch_propagated(event);
                break;
            }
            case crd::platform::InputEvent::Type::MouseDown:
            {
                MouseButtonPressedEvent event(raw.payload.mouse_button.button, raw.mods);
                dispatch_propagated(event);
                break;
            }
            case crd::platform::InputEvent::Type::MouseUp:
            {
                MouseButtonReleasedEvent event(raw.payload.mouse_button.button, raw.mods);
                dispatch_propagated(event);
                break;
            }
            case crd::platform::InputEvent::Type::MouseMove:
            {
                MouseMovedEvent event(raw.payload.mouse_move.x, raw.payload.mouse_move.y);
                dispatch_propagated(event);
                break;
            }
            case crd::platform::InputEvent::Type::Scroll:
            {
                MouseScrolledEvent event(raw.payload.scroll.dx, raw.payload.scroll.dy);
                dispatch_propagated(event);
                break;
            }
            case crd::platform::InputEvent::Type::Resize:
            {
                WindowResizeEvent event(raw.payload.resize.width, raw.payload.resize.height);
                dispatch_propagated(event);
                break;
            }
            case crd::platform::InputEvent::Type::None:
                break;
        }
    }

    if (m_window->should_close())
    {
        WindowCloseEvent event;
        dispatch_propagated(event);
        if (event.handled)
        {
            m_window->clear_close_request();
        }
        else
        {
            m_running = false;
        }
    }
}
} // namespace crd::app
