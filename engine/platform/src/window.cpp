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
};

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

    // GLFW takes UTF-8 null-terminated strings; crd::containers::String guarantees a
    // null terminator on c_str(), so we can pass it directly.
    GLFWwindow* handle = glfwCreateWindow(desc.size.width, desc.size.height, desc.title.c_str(), /*monitor=*/nullptr,
                                          /*share=*/nullptr);
    if (handle == nullptr)
    {
        CRD_LOG_ERROR(g_log_platform, "glfwCreateWindow failed for '{}' ({}x{})", desc.title.c_str(), desc.size.width,
                      desc.size.height);
        return result;
    }

    result.m_impl->handle = handle;

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
    // GLFW requires a null-terminated string. StringView is not guaranteed to
    // be null-terminated, so we copy through a temporary String.
    crd::containers::String owned(title);
    glfwSetWindowTitle(m_impl->handle, owned.c_str());
}

void* Window::native_handle() const noexcept
{
    return is_valid() ? static_cast<void*>(m_impl->handle) : nullptr;
}
} // namespace crd::platform
