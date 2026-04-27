#include <crd/log/log.hpp>
#include <crd/platform/context.hpp>
#include <crd/platform/log_channel.hpp>
#include <crd/platform/window.hpp>

#include <GLFW/glfw3.h>
#include <utility>

namespace crd::platform
{
struct Window::Impl
{
    GLFWwindow* handle = nullptr;
    Input input{};
};

namespace
{
[[nodiscard]] Key glfw_key_to_crd(int glfw_key) noexcept
{
    // Letters: GLFW_KEY_A is 'A' (65), and Cerid Key::A == 1.
    if (glfw_key >= GLFW_KEY_A && glfw_key <= GLFW_KEY_Z)
    {
        return static_cast<Key>(static_cast<crd::u16>(Key::A) + static_cast<crd::u16>(glfw_key - GLFW_KEY_A));
    }
    if (glfw_key >= GLFW_KEY_0 && glfw_key <= GLFW_KEY_9)
    {
        return static_cast<Key>(static_cast<crd::u16>(Key::Num0) + static_cast<crd::u16>(glfw_key - GLFW_KEY_0));
    }
    if (glfw_key >= GLFW_KEY_F1 && glfw_key <= GLFW_KEY_F12)
    {
        return static_cast<Key>(static_cast<crd::u16>(Key::F1) + static_cast<crd::u16>(glfw_key - GLFW_KEY_F1));
    }
    switch (glfw_key)
    {
        case GLFW_KEY_SPACE:
            return Key::Space;
        case GLFW_KEY_ENTER:
            return Key::Enter;
        case GLFW_KEY_ESCAPE:
            return Key::Escape;
        case GLFW_KEY_TAB:
            return Key::Tab;
        case GLFW_KEY_BACKSPACE:
            return Key::Backspace;
        case GLFW_KEY_DELETE:
            return Key::Delete;
        case GLFW_KEY_LEFT:
            return Key::Left;
        case GLFW_KEY_RIGHT:
            return Key::Right;
        case GLFW_KEY_UP:
            return Key::Up;
        case GLFW_KEY_DOWN:
            return Key::Down;
        case GLFW_KEY_LEFT_SHIFT:
            return Key::LeftShift;
        case GLFW_KEY_RIGHT_SHIFT:
            return Key::RightShift;
        case GLFW_KEY_LEFT_CONTROL:
            return Key::LeftCtrl;
        case GLFW_KEY_RIGHT_CONTROL:
            return Key::RightCtrl;
        case GLFW_KEY_LEFT_ALT:
            return Key::LeftAlt;
        case GLFW_KEY_RIGHT_ALT:
            return Key::RightAlt;
        default:
            return Key::Unknown;
    }
}

[[nodiscard]] MouseButton glfw_button_to_crd(int b) noexcept
{
    switch (b)
    {
        case GLFW_MOUSE_BUTTON_LEFT:
            return MouseButton::Left;
        case GLFW_MOUSE_BUTTON_RIGHT:
            return MouseButton::Right;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            return MouseButton::Middle;
        case GLFW_MOUSE_BUTTON_4:
            return MouseButton::X1;
        case GLFW_MOUSE_BUTTON_5:
            return MouseButton::X2;
        default:
            return MouseButton::Count;
    }
}

[[nodiscard]] KeyMods unpack_mods(int glfw_mods) noexcept
{
    KeyMods m{};
    m.shift = (glfw_mods & GLFW_MOD_SHIFT) != 0;
    m.ctrl = (glfw_mods & GLFW_MOD_CONTROL) != 0;
    m.alt = (glfw_mods & GLFW_MOD_ALT) != 0;
    m.super = (glfw_mods & GLFW_MOD_SUPER) != 0;
    return m;
}

[[nodiscard]] Input* input_from(GLFWwindow* w) noexcept
{
    return static_cast<Input*>(glfwGetWindowUserPointer(w));
}

void key_callback(GLFWwindow* w, int key, int /*scancode*/, int action, int mods) noexcept
{
    Input* in = input_from(w);
    if (in == nullptr)
    {
        return;
    }
    const Key k = glfw_key_to_crd(key);
    const KeyMods m = unpack_mods(mods);
    if (action == GLFW_PRESS)
    {
        in->push_key_event(k, InputEvent::Type::KeyDown, m);
    }
    else if (action == GLFW_RELEASE)
    {
        in->push_key_event(k, InputEvent::Type::KeyUp, m);
    }
    else if (action == GLFW_REPEAT)
    {
        in->push_key_event(k, InputEvent::Type::KeyRepeat, m);
    }
}

void mouse_button_callback(GLFWwindow* w, int button, int action, int mods) noexcept
{
    Input* in = input_from(w);
    if (in == nullptr)
    {
        return;
    }
    const MouseButton b = glfw_button_to_crd(button);
    if (b == MouseButton::Count)
    {
        return;
    }
    const KeyMods m = unpack_mods(mods);
    if (action == GLFW_PRESS)
    {
        in->push_mouse_button_event(b, InputEvent::Type::MouseDown, m);
    }
    else if (action == GLFW_RELEASE)
    {
        in->push_mouse_button_event(b, InputEvent::Type::MouseUp, m);
    }
}

void cursor_pos_callback(GLFWwindow* w, double x, double y) noexcept
{
    Input* in = input_from(w);
    if (in != nullptr)
    {
        in->push_mouse_move(static_cast<crd::f32>(x), static_cast<crd::f32>(y));
    }
}

void scroll_callback(GLFWwindow* w, double dx, double dy) noexcept
{
    Input* in = input_from(w);
    if (in != nullptr)
    {
        in->push_scroll(static_cast<crd::f32>(dx), static_cast<crd::f32>(dy));
    }
}

void framebuffer_size_callback(GLFWwindow* w, int width, int height) noexcept
{
    Input* in = input_from(w);
    if (in != nullptr)
    {
        in->push_resize(width, height);
    }
}
} // namespace

Window::Window() noexcept : m_impl(std::make_unique<Impl>()) {}

Window::Window(Window&& other) noexcept = default;

Window& Window::operator=(Window&& other) noexcept
{
    if (this != &other)
    {
        // Destroy any window we currently own before adopting the other side's
        // impl. unique_ptr's default move-assign would do the swap-and-free
        // dance for the Impl struct itself, but Impl is a POD: it would not
        // call glfwDestroyWindow. Doing it here keeps the GLFW handle from
        // leaking on self-overwrite.
        if (m_impl && m_impl->handle != nullptr)
        {
            glfwDestroyWindow(m_impl->handle);
            m_impl->handle = nullptr;
        }
        m_impl = std::move(other.m_impl);
        // The user pointer in the moved GLFWwindow now points to the OLD
        // Impl's Input. Re-bind it to the adopted Input so callbacks
        // continue to write into the right object.
        if (m_impl && m_impl->handle != nullptr)
        {
            glfwSetWindowUserPointer(m_impl->handle, &m_impl->input);
        }
    }
    return *this;
}

Window::~Window() noexcept
{
    if (m_impl && m_impl->handle != nullptr)
    {
        glfwDestroyWindow(m_impl->handle);
        m_impl->handle = nullptr;
    }
}

Window Window::create(const PlatformContext& context, const WindowDesc& desc) noexcept
{
    Window result;

    if (!context.is_valid())
    {
        CRD_LOG_ERROR(g_log_platform, "Window::create called with invalid PlatformContext");
        return result;
    }

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, desc.client_api_none ? GLFW_NO_API : GLFW_OPENGL_API);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);

    GLFWwindow* handle = glfwCreateWindow(desc.size.width, desc.size.height, desc.title.c_str(), /*monitor=*/nullptr,
                                          /*share=*/nullptr);
    if (handle == nullptr)
    {
        CRD_LOG_ERROR(g_log_platform, "glfwCreateWindow failed for '{}' ({}x{})", desc.title.c_str(), desc.size.width,
                      desc.size.height);
        return result;
    }

    result.m_impl->handle = handle;

    // Wire callbacks into the Input owned by this Window's Impl. The user
    // pointer is the Input object directly, so callbacks need a single
    // indirection to mutate state.
    glfwSetWindowUserPointer(handle, &result.m_impl->input);
    glfwSetKeyCallback(handle, &key_callback);
    glfwSetMouseButtonCallback(handle, &mouse_button_callback);
    glfwSetCursorPosCallback(handle, &cursor_pos_callback);
    glfwSetScrollCallback(handle, &scroll_callback);
    glfwSetFramebufferSizeCallback(handle, &framebuffer_size_callback);

    CRD_LOG_INFO(g_log_platform, "Created window '{}' ({}x{})", desc.title.c_str(), desc.size.width, desc.size.height);
    return result;
}

bool Window::is_valid() const noexcept
{
    return m_impl != nullptr && m_impl->handle != nullptr;
}

bool Window::should_close() const noexcept
{
    return is_valid() && glfwWindowShouldClose(m_impl->handle) != 0;
}

void Window::request_close() noexcept
{
    if (is_valid())
    {
        glfwSetWindowShouldClose(m_impl->handle, GLFW_TRUE);
    }
}

void Window::clear_close_request() noexcept
{
    if (is_valid())
    {
        glfwSetWindowShouldClose(m_impl->handle, GLFW_FALSE);
    }
}

Extent2D Window::framebuffer_size() const noexcept
{
    if (!is_valid())
    {
        return Extent2D{};
    }
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(m_impl->handle, &w, &h);
    return Extent2D{w, h};
}

Extent2D Window::window_size() const noexcept
{
    if (!is_valid())
    {
        return Extent2D{};
    }
    int w = 0;
    int h = 0;
    glfwGetWindowSize(m_impl->handle, &w, &h);
    return Extent2D{w, h};
}

void Window::set_title(crd::containers::StringView title) noexcept
{
    if (!is_valid())
    {
        return;
    }
    crd::containers::String owned(title);
    glfwSetWindowTitle(m_impl->handle, owned.c_str());
}

void Window::poll_input() noexcept
{
    if (m_impl)
    {
        m_impl->input.on_poll_begin();
    }
}

Input& Window::input() noexcept
{
    CRD_ASSERT(m_impl != nullptr);
    return m_impl->input;
}

const Input& Window::input() const noexcept
{
    CRD_ASSERT(m_impl != nullptr);
    return m_impl->input;
}

void* Window::native_handle() const noexcept
{
    return is_valid() ? static_cast<void*>(m_impl->handle) : nullptr;
}
} // namespace crd::platform
