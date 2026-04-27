#include <crd/log/log.hpp>
#include <crd/platform/context.hpp>
#include <crd/platform/log_channel.hpp>

#include <GLFW/glfw3.h>

namespace crd::platform
{
namespace
{
void glfw_error_to_log(int error_code, const char* description) noexcept
{
    CRD_LOG_ERROR(g_log_platform, "GLFW error {}: {}", error_code, description ? description : "(null)");
}
} // namespace

PlatformContext PlatformContext::create() noexcept
{
    glfwSetErrorCallback(&glfw_error_to_log);

    if (glfwInit() == GLFW_FALSE)
    {
        CRD_LOG_CRITICAL(g_log_platform, "glfwInit() failed; PlatformContext is invalid");
        return PlatformContext{false};
    }

    int major = 0;
    int minor = 0;
    int rev = 0;
    glfwGetVersion(&major, &minor, &rev);
    CRD_LOG_INFO(g_log_platform, "GLFW initialised (version {}.{}.{})", major, minor, rev);

    return PlatformContext{true};
}

PlatformContext::PlatformContext(PlatformContext&& other) noexcept : m_initialised(other.m_initialised)
{
    other.m_initialised = false;
}

PlatformContext& PlatformContext::operator=(PlatformContext&& other) noexcept
{
    if (this != &other)
    {
        if (m_initialised)
        {
            glfwTerminate();
        }
        m_initialised = other.m_initialised;
        other.m_initialised = false;
    }
    return *this;
}

PlatformContext::~PlatformContext() noexcept
{
    if (m_initialised)
    {
        glfwTerminate();
        m_initialised = false;
    }
}

void PlatformContext::poll_events() noexcept
{
    if (m_initialised)
    {
        glfwPollEvents();
    }
}
} // namespace crd::platform
