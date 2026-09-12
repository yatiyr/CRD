#include <crd/platform/platform.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

namespace
{
// Some CI runners don't have a usable display backend even on Windows.
// Setting CRD_PLATFORM_HEADLESS=1 in the environment forces every test that
// would touch a real OS window to short-circuit. PlatformContext init is
// still exercised because GLFW's init path is display-independent on Win32.
[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}
} // namespace

TEST_CASE("PlatformContext: create + destroy", "[platform][context]")
{
    auto ctx = crd::platform::PlatformContext::create();
    REQUIRE(ctx.is_valid());

    // Polling on a fresh context with no windows is a no-op but must be safe.
    ctx.poll_events();
}

TEST_CASE("PlatformContext: move semantics transfer ownership", "[platform][context]")
{
    auto a = crd::platform::PlatformContext::create();
    REQUIRE(a.is_valid());

    crd::platform::PlatformContext b(std::move(a));
    REQUIRE(b.is_valid());
    REQUIRE_FALSE(a.is_valid()); // NOLINT(bugprone-use-after-move)
}

TEST_CASE("PlatformContext: re-creation after destruction works", "[platform][context]")
{
    {
        auto first = crd::platform::PlatformContext::create();
        REQUIRE(first.is_valid());
    }

    auto second = crd::platform::PlatformContext::create();
    REQUIRE(second.is_valid());
}

TEST_CASE("Window: default-constructed Extent2D is zero-init", "[platform][window]")
{
    crd::platform::Extent2D e{};
    REQUIRE(e.width == 0);
    REQUIRE(e.height == 0);
}

TEST_CASE("Window: create + destroy invisible window", "[platform][window]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping window-creating test");
        return;
    }

    auto ctx = crd::platform::PlatformContext::create();
    REQUIRE(ctx.is_valid());

    crd::platform::WindowDesc desc;
    desc.size = {640, 480};
    desc.title = crd::containers::String("crd-platform-tests");
    desc.visible = false;

    auto window = crd::platform::Window::create(ctx, desc);
    if (!window.is_valid())
    {
        SUCCEED("Window backend unavailable on this runner; skipping");
        return;
    }

    REQUIRE_FALSE(window.should_close());
    REQUIRE(window.native_handle() != nullptr);

    const auto fb = window.framebuffer_size();
    REQUIRE(fb.width > 0);
    REQUIRE(fb.height > 0);

    window.request_close();
    REQUIRE(window.should_close());

    window.clear_close_request();
    REQUIRE_FALSE(window.should_close());
}

TEST_CASE("Window: invalid-context create returns invalid window", "[platform][window]")
{
    crd::platform::PlatformContext bad;
    REQUIRE_FALSE(bad.is_valid());

    crd::platform::WindowDesc desc;
    desc.title = crd::containers::String("never-shown");
    desc.visible = false;

    auto window = crd::platform::Window::create(bad, desc);
    REQUIRE_FALSE(window.is_valid());
    REQUIRE(window.native_handle() == nullptr);
}

TEST_CASE("Window: move semantics keep handle alive", "[platform][window]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping window-creating test");
        return;
    }

    auto ctx = crd::platform::PlatformContext::create();
    REQUIRE(ctx.is_valid());

    crd::platform::WindowDesc desc;
    desc.visible = false;
    desc.size = {320, 240};
    desc.title = crd::containers::String("crd-platform-move-test");

    auto a = crd::platform::Window::create(ctx, desc);
    if (!a.is_valid())
    {
        SUCCEED("Window backend unavailable on this runner; skipping");
        return;
    }

    void* original_handle = a.native_handle();
    crd::platform::Window b(std::move(a));
    REQUIRE(b.native_handle() == original_handle);
    REQUIRE_FALSE(a.is_valid()); // NOLINT(bugprone-use-after-move)
}
