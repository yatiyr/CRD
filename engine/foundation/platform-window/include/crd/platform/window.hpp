#pragma once

#include <crd/containers/string.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/platform/input.hpp>

#include <memory>

namespace crd::platform
{
class PlatformContext;

// Integer 2D extent used for window/framebuffer sizes. Deliberately not
// `crd::math::Vec2<i32>`: `MathScalar` is `f32`/`f64` only by design, and
// dragging integer scalars into the math concept just to satisfy a window
// API would weaken the math contract. A small platform-local POD keeps the
// dependency graph tight (`crd-platform` does not need `crd-math`).
struct Extent2D
{
    crd::i32 width = 0;
    crd::i32 height = 0;
};

// Window creation parameters. Defaults give a Vulkan-ready 1280x720
// resizable window with a sensible title.
//
// `client_api_none` defaults to true: GLFW will create a window with
// GLFW_NO_API, which is the correct setup for a Vulkan-first engine. Set
// this to false only if you intentionally want a legacy OpenGL context
// (we never do, but exposing the toggle keeps the option open for tools
// or experiments).
struct WindowDesc
{
    Extent2D size{1280, 720};
    crd::containers::String title{"Cerid"};
    bool resizable = true;
    bool visible = true;
    bool client_api_none = true;
};

// Window — a single OS window. Concrete type with a PIMPL backend; the
// underlying GLFW handle never appears in this header. If we swap to a
// different backend later (SDL3, custom Win32+Cocoa+Xlib), the public
// API is unchanged.
//
// Construction is via the static `create()` factory because we need the
// backend pointer to be either valid or null without throwing exceptions.
// Failed creation returns a Window whose `is_valid()` is false; the
// platform log channel records the underlying reason.
class Window
{
public:
    [[nodiscard]] static Window create(const PlatformContext& context, const WindowDesc& desc) noexcept;

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    ~Window() noexcept;

    [[nodiscard]] bool is_valid() const noexcept;

    [[nodiscard]] bool should_close() const noexcept;
    void request_close() noexcept;
    void clear_close_request() noexcept;

    [[nodiscard]] Extent2D framebuffer_size() const noexcept;
    [[nodiscard]] Extent2D window_size() const noexcept;

    void set_title(crd::containers::StringView title) noexcept;

    // Per-frame input pump. Call ONCE at the top of every frame, BEFORE
    // PlatformContext::poll_events(). Clears one-frame edge state so the
    // callbacks fired by the upcoming poll can populate a fresh frame's
    // pressed/released flags, mouse delta, and scroll delta. Read the
    // resulting snapshot through `input().state()` after polling events.
    void poll_input() noexcept;

    [[nodiscard]] Input& input() noexcept;
    [[nodiscard]] const Input& input() const noexcept;

    // Native handle escape hatch. Returns the underlying GLFWwindow* as a
    // void*. Graphics code (Vulkan surface creation) casts it back when
    // it links against GLFW directly. Engine code MUST NOT use this for
    // anything else.
    [[nodiscard]] void* native_handle() const noexcept;

private:
    // CRD_NOINLINE: MSVC LTCG/PGO ICE C1001 in win-shipping-profile when
    // std::make_unique<Impl>() is inlined into the LTCG call graph of
    // crd-sandbox.exe (D-003 win-shipping-profile preset; reproduced
    // 2026-05-17 during Phase 3.1.7 v8-close full sweep). Same precedent
    // as evict_block_locked / try_evict_to_budget in crd-resources.
    CRD_NOINLINE Window() noexcept;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace crd::platform
