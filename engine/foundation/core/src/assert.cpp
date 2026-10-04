#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp> // crd::usize (used by the per-site ignore table below; do not rely on the PCH)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

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
std::atomic<AssertPlatformHandler> g_assert_platform_handler{nullptr};

// Headless/non-interactive mode: when set, the default (no-handler) assert path terminates promptly instead of
// showing a modal dialog. Off by default so interactive behaviour is unchanged. See set_assert_headless().
std::atomic<bool> g_assert_headless{false};

// Per-site ignore table: tracks (file, line) pairs the user has chosen to ignore.
// __FILE__ string literals have static lifetime so pointer comparison is stable
// within a single process image.
std::mutex g_ignore_mutex;
constexpr crd::usize kMaxIgnoredSites = 256U;
std::pair<const char*, int> g_ignored_sites[kMaxIgnoredSites];
crd::usize g_ignored_filled = 0U; // number of valid entries (caps at kMaxIgnoredSites)
crd::usize g_ignore_next    = 0U; // FIFO write cursor (wraps at kMaxIgnoredSites)
std::atomic<crd::u64> g_ignored_evictions{0};

// Caller holds g_ignore_mutex. Once the ring has wrapped, all kMaxIgnoredSites slots are valid.
bool ignored_contains(const char* file, int line) noexcept
{
    for (crd::usize i = 0U; i < g_ignored_filled; ++i)
    {
        if (g_ignored_sites[i].first == file && g_ignored_sites[i].second == line)
        {
            return true;
        }
    }
    return false;
}
} // namespace

void set_assert_handler(AssertHandler h) noexcept
{
    g_assert_handler.store(h, std::memory_order_release);
}

AssertHandler get_assert_handler() noexcept
{
    return g_assert_handler.load(std::memory_order_acquire);
}

void set_assert_platform_handler(AssertPlatformHandler h) noexcept
{
    g_assert_platform_handler.store(h, std::memory_order_release);
}

AssertPlatformHandler get_assert_platform_handler() noexcept
{
    return g_assert_platform_handler.load(std::memory_order_acquire);
}

void set_assert_headless(bool headless) noexcept
{
    g_assert_headless.store(headless, std::memory_order_release);
}

bool get_assert_headless() noexcept
{
    return g_assert_headless.load(std::memory_order_acquire);
}

bool ignore_assert_site(const char* file, int line) noexcept
{
    if (file == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_ignore_mutex);
    if (ignored_contains(file, line))
    {
        return true; // already recorded -- no duplicate, no eviction
    }
    if (g_ignored_filled == kMaxIgnoredSites)
    {
        // Table full: overwrite the oldest entry (FIFO), which will fire again next time. Previously this branch
        // silently refused to record -- the "ignored-site churn" defect. Emit ONE stderr line, on the first
        // eviction only (per-eviction lines would spam under real churn); the counter carries the rest.
        if (g_ignored_evictions.fetch_add(1U, std::memory_order_relaxed) == 0U)
        {
            std::fputs("[crd-assert] ignore table full (256 sites); wrapping -- the oldest ignored sites will fire "
                       "again\n",
                       stderr);
        }
    }
    g_ignored_sites[g_ignore_next] = {file, line};
    g_ignore_next                  = (g_ignore_next + 1U) % kMaxIgnoredSites;
    if (g_ignored_filled < kMaxIgnoredSites)
    {
        ++g_ignored_filled;
    }
    return true;
}

u64 assert_ignore_eviction_count() noexcept
{
    return g_ignored_evictions.load(std::memory_order_relaxed);
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
    // Per-site ignore: if this (file, line) was previously ignored, skip silently.
    {
        std::lock_guard<std::mutex> lock(g_ignore_mutex);
        if (ignored_contains(file, line))
        {
            return 0;
        }
    }

    char buffer[1024];

    if (message != nullptr)
    {
        std::snprintf(buffer, sizeof(buffer),
                      "Assertion failed! \n\t expr: %s \n\t file: %s \n\t line: %d \n\t message: %s\n",
                      expression, file, line, message);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer),
                      "Assertion failed! \n\t expr: %s \n\t file: %s \n\t line: %d\n",
                      expression, file, line);
    }

    // Optional bridge: route to crd-log (or any other subsystem) BEFORE
    // we touch stderr / debugger. If a handler is installed and not
    // already on the call stack, it gets called once.
    (void)fire_assert_handler(expression, file, line, message);

    // If a platform handler is installed, delegate entirely to it.
    // The handler receives the formatted buffer and controls output.
    // Tests use this to suppress stderr noise when intentionally triggering asserts.
    if (AssertPlatformHandler platform_handler = get_assert_platform_handler())
    {
        return platform_handler(buffer);
    }

    // Default path: no platform handler — print evidence to stderr (+ the debugger on Windows), then either terminate
    // (headless) or show the OS dialog (interactive).
    std::fputs(buffer, stderr);

#if CRD_OS_WINDOWS
    OutputDebugStringA(buffer);
#endif

    // Headless/CI (declared via set_assert_headless): a modal dialog would hang and a silent ignore would hide a
    // required check, so do neither. The evidence is already emitted; terminate promptly. abort() raises SIGABRT,
    // which crd's crash handler records on Linux (routing the failure to the emergency channel); on Windows it
    // terminates promptly with the evidence above. A platform handler, checked earlier, still takes precedence.
    if (g_assert_headless.load(std::memory_order_acquire))
    {
        std::fflush(stderr);
        std::abort();
    }

#if CRD_OS_WINDOWS
    const int result = MessageBoxA(nullptr, buffer, "CRD Assert", MB_ABORTRETRYIGNORE | MB_ICONERROR);
    if (result == IDABORT)
    {
        std::abort();
    }
    if (result == IDIGNORE)
    {
        (void)crd::ignore_assert_site(file, line); // FIFO-bounded record with eviction evidence; see DIAG.5c(f)
        return 0;
    }
    return (result == IDRETRY) ? 2 : 0;
#else
    return 2;
#endif
}
} // namespace crd::detail
