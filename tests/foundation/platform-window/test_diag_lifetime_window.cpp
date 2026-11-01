// DIAG.10c -- a window in the lifetime ledger, judged by the OS.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): a Window that created its OS window is one
// OsObject until its destructor, or a move-assignment over it, destroys that window; a move transfers it without
// counting it twice, and a failed create records nothing. On Windows the oracle is the OS itself, sharing nothing
// with the wired site: FindWindow by the window's unique title, and the process's count of USER objects
// (GetGuiResources(GR_USEROBJECTS)), which rises while a window lives and returns to the count it had before the
// platform context once the context is gone (the window backend may keep objects of its own until it terminates).
// A runner with CRD_PLATFORM_HEADLESS=1 creates no window, and its platform context alone must leave the ledger
// balanced; a runner without a display cannot create one, and the failed create must leave the ledger balanced.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/platform.hpp>
#include <crd/platform/context.hpp>
#include <crd/platform/platform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
using crd::LifetimeClass;
using crd::LifetimeVerdict;

crd::LifetimeReport since(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot());
}

crd::platform::WindowDesc hidden_desc(const char* title = "crd-diag10c-window")
{
    crd::platform::WindowDesc desc;
    desc.size    = {320, 240};
    desc.title   = crd::containers::String(title);
    desc.visible = false;
    return desc;
}

#if CRD_OS_WINDOWS
DWORD user_objects() noexcept
{
    return GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
}

// Does a top-level window with this title exist, hidden or not?
bool os_window_exists(const wchar_t* title) noexcept
{
    return FindWindowW(nullptr, title) != nullptr;
}
#endif

// The window half: a context, two windows, a move and a move-assignment. Returns whether a window could be created.
bool window_lifetime()
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    auto                        ctx   = crd::platform::PlatformContext::create();
    if (crd::platform::headless_requested())
    {
        // The runner asked for no window: the context alone holds no OS object.
        CHECK(since(start).verdict == LifetimeVerdict::Balanced);
        return false;
    }
    if (!ctx.is_valid())
    {
        // No display: nothing can be created, and the attempt holds nothing.
        auto none = crd::platform::Window::create(ctx, hidden_desc());
        CHECK_FALSE(none.is_valid());
        CHECK(since(start).verdict == LifetimeVerdict::Balanced);
        return false;
    }
    const crd::LifetimeSnapshot with_context = crd::lifetime_ledger().snapshot();
    {
        auto window = crd::platform::Window::create(ctx, hidden_desc("crd-diag10c-window-first"));
        if (!window.is_valid())
        {
            CHECK(since(with_context).verdict == LifetimeVerdict::Balanced);
            return false;
        }
        const crd::LifetimeReport r = since(with_context);
        CHECK(r.verdict == LifetimeVerdict::Outstanding);
        CHECK(r.outstanding == crd::lifetime_class_bit(LifetimeClass::OsObject));
        CHECK(r.live_change(LifetimeClass::OsObject) == 1);
#if CRD_OS_WINDOWS
        CHECK(os_window_exists(L"crd-diag10c-window-first"));
#endif

        // A move transfers the window; the count does not move.
        crd::platform::Window moved(std::move(window));
        CHECK(since(with_context).live_change(LifetimeClass::OsObject) == 1);

        // Move-assigning a second window over it destroys the first.
        auto second = crd::platform::Window::create(ctx, hidden_desc("crd-diag10c-window-second"));
        REQUIRE(second.is_valid());
        CHECK(since(with_context).live_change(LifetimeClass::OsObject) == 2);
        moved = std::move(second);
        CHECK(since(with_context).live_change(LifetimeClass::OsObject) == 1);
#if CRD_OS_WINDOWS
        CHECK_FALSE(os_window_exists(L"crd-diag10c-window-first")); // gone as the OS sees it
        CHECK(os_window_exists(L"crd-diag10c-window-second"));
#endif
    }
    const crd::LifetimeReport gone = since(with_context);
    CHECK(gone.verdict == LifetimeVerdict::Balanced);
    CHECK(gone.underflowed == 0U);
#if CRD_OS_WINDOWS
    CHECK_FALSE(os_window_exists(L"crd-diag10c-window-second"));
#endif
    return true;
}
} // namespace

TEST_CASE("diag10c: a window is one OS object until it is destroyed, as the OS sees it",
          "[platform][lifetime][diag10c]")
{
#if CRD_OS_WINDOWS
    const DWORD before_context = user_objects();
#endif
    if (!window_lifetime())
    {
        WARN("no window backend on this runner: only the failed create was checked");
    }
#if CRD_OS_WINDOWS
    CHECK(user_objects() == before_context); // the windows and the context are gone: every USER object with them
#endif
}

TEST_CASE("diag10c: window shutdown cycles leave no OS object behind", "[platform][lifetime][workload][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
#if CRD_OS_WINDOWS
    const DWORD before = user_objects();
#endif
    for (crd::u32 cycle = 0U; cycle < 3U; ++cycle)
    {
        auto ctx = crd::platform::PlatformContext::create();
        if (crd::platform::headless_requested() || !ctx.is_valid())
        {
            break; // no display: the context itself is the whole cycle
        }
        for (crd::u32 i = 0U; i < 4U; ++i)
        {
            auto window = crd::platform::Window::create(ctx, hidden_desc());
            if (window.is_valid())
            {
                CHECK(since(start).live_change(LifetimeClass::OsObject) == 1);
            }
        }
        CHECK(since(start).live_change(LifetimeClass::OsObject) == 0);
    }
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
#if CRD_OS_WINDOWS
    CHECK(user_objects() == before);
#endif
}
