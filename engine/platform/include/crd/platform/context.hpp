#pragma once

#include <crd/core/types.hpp>

namespace crd::platform
{
// PlatformContext owns the GLFW runtime: glfwInit on construction,
// glfwTerminate on destruction. The class is move-only and at most one
// PlatformContext should exist per process.
//
// Construction also installs a GLFW error callback that bridges into
// crd-log at LogLevel::Error, so platform-level GLFW failures show up in
// the engine's normal logging stream.
//
// On creation failure (glfwInit returned GLFW_FALSE) we log Critical and
// return a sentinel context whose `is_valid()` is false. Callers MUST
// check `is_valid()` before constructing a Window.
//
// Use `poll_events()` once per frame to drain the OS event queue. This
// is the only function callers need to invoke regularly.
class PlatformContext
{
public:
    // Default constructor yields an INVALID context (`is_valid() == false`).
    // Use `create()` to actually initialise GLFW. The default ctor exists so
    // callers can hold a context member that is initialised later.
    PlatformContext() noexcept = default;

    [[nodiscard]] static PlatformContext create() noexcept;

    PlatformContext(const PlatformContext&) = delete;
    PlatformContext& operator=(const PlatformContext&) = delete;

    PlatformContext(PlatformContext&& other) noexcept;
    PlatformContext& operator=(PlatformContext&& other) noexcept;

    ~PlatformContext() noexcept;

    [[nodiscard]] bool is_valid() const noexcept { return m_initialised; }

    void poll_events() noexcept;

private:
    explicit PlatformContext(bool initialised) noexcept : m_initialised(initialised) {}

    bool m_initialised = false;
};
} // namespace crd::platform
