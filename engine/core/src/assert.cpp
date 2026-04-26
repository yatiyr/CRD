#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace crd
{
namespace
{
// Single-slot handler. Atomic so the bridge from crd-log can be
// installed/uninstalled safely from any thread; we never block in
// the assert path so a relaxed load on the hot path is enough.
std::atomic<AssertHandler> g_assert_handler{nullptr};
} // namespace

void set_assert_handler(AssertHandler h) noexcept
{
    g_assert_handler.store(h, std::memory_order_release);
}

AssertHandler get_assert_handler() noexcept
{
    return g_assert_handler.load(std::memory_order_acquire);
}

namespace detail
{
// Internal helper used by report_assert_failure to invoke the
// user-installed handler (if any) without re-entering itself.
// Re-entrancy guard: thread-local flag.
bool fire_assert_handler(const char* expression, const char* file, int line, const char* message) noexcept
{
    static thread_local bool s_in_handler = false;
    if (s_in_handler)
    {
        return false;
    }
    AssertHandler h = g_assert_handler.load(std::memory_order_acquire);
    if (!h)
    {
        return false;
    }
    s_in_handler = true;
    h(expression, file, line, message);
    s_in_handler = false;
    return true;
}
} // namespace detail
} // namespace crd

namespace crd::detail
{
int report_assert_failure(const char* expression, const char* file, int line, const char* message)
{
    char buffer[1024];

    if (message != nullptr)
    {
        const char* formatted_string_with_msg =
            "Assertion failed! \n\t expr: %s \n\t file: %s \n\t line: %d \n\t message: %s\n";
        std::snprintf(buffer, sizeof(buffer), formatted_string_with_msg, expression, file, line, message);
    }
    else
    {
        const char* formatted_string_without_msg = "Assertion failed! \n\t expr: %s \n\t file: %s \n\t line: %d\n";
        std::snprintf(buffer, sizeof(buffer), formatted_string_without_msg, expression, file, line);
    }

    // Optional bridge: route to crd-log (or any other subsystem) BEFORE
    // we touch stderr / debugger. If a handler is installed and not
    // already on the call stack, it gets called once.
    (void)fire_assert_handler(expression, file, line, message);

    std::fputs(buffer, stderr);

#if CRD_OS_WINDOWS
    OutputDebugStringA(buffer);
    const int result = MessageBoxA(nullptr, buffer, "CRD Assert", MB_ABORTRETRYIGNORE | MB_ICONERROR);
    if (result == IDABORT)
    {
        std::abort();
    }

    return (result == IDRETRY) ? 2 : 0;
#else
    return 2;
#endif
}
} // namespace crd::detail