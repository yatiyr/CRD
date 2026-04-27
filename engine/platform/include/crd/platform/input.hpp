#pragma once

#include <crd/containers/ring_buffer.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <array>
#include <new>

namespace crd::platform
{
// Subset of physical keys we surface to engine code today. The values are
// arbitrary contiguous indices, NOT GLFW key codes, so swapping backends
// later doesn't reshuffle the enum. Translation tables live in input.cpp.
//
// We deliberately ship a useful but small set instead of "every key GLFW
// can name". Adding more is a one-line append to this enum and one
// translation entry in input.cpp.
enum class Key : crd::u16
{
    Unknown = 0,

    // Letters
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    // Numbers (top row)
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,

    // Function keys
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    // Editing / navigation
    Space,
    Enter,
    Escape,
    Tab,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,

    // Modifiers
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,

    // Sentinel — keep last
    Count,
};

enum class MouseButton : crd::u8
{
    Left = 0,
    Right,
    Middle,
    X1,
    X2,

    // Sentinel — keep last
    Count,
};

// Bitfield for modifier keys held while another input event fired. Mirrors
// the relevant GLFW_MOD_* values but the public API doesn't depend on
// that mapping; translation lives inside input.cpp.
struct KeyMods
{
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool super = false; // Windows / Cmd
};

// One hardware-level event. POD union (tagged) so it costs nothing to
// queue and cheap to switch on. No propagation / consumption flags here;
// that responsibility lives in a future crd-app layer.
struct InputEvent
{
    enum class Type : crd::u8
    {
        None,
        KeyDown,
        KeyUp,
        KeyRepeat,
        MouseDown,
        MouseUp,
        MouseMove, // payload = mouse_move (absolute position)
        Scroll,    // payload = scroll
        Resize,    // payload = resize (window_size)
    };

    Type type = Type::None;
    KeyMods mods{};
    union Payload
    {
        struct
        {
            Key key;
        } key;
        struct
        {
            MouseButton button;
        } mouse_button;
        struct
        {
            crd::f32 x, y;
        } mouse_move;
        struct
        {
            crd::f32 dx, dy;
        } scroll;
        struct
        {
            crd::i32 width, height;
        } resize;
    } payload{};
};

// Frame-coherent input snapshot. `down` is the live held state. `pressed`
// and `released` flag transitions that happened between the previous and
// the current `Input::poll()` call. Mouse `position` is in window-content
// coordinates as reported by GLFW; `delta` is `position - previous`.
//
// Reading is pure: the snapshot does not change between polls, so multiple
// systems on the same frame see identical values.
class InputState
{
public:
    [[nodiscard]] bool is_key_down(Key k) const noexcept { return m_key_down[static_cast<crd::usize>(k)]; }
    [[nodiscard]] bool was_key_pressed(Key k) const noexcept { return m_key_pressed[static_cast<crd::usize>(k)]; }
    [[nodiscard]] bool was_key_released(Key k) const noexcept { return m_key_released[static_cast<crd::usize>(k)]; }

    [[nodiscard]] bool is_mouse_down(MouseButton b) const noexcept { return m_mouse_down[static_cast<crd::usize>(b)]; }
    [[nodiscard]] bool was_mouse_pressed(MouseButton b) const noexcept
    {
        return m_mouse_pressed[static_cast<crd::usize>(b)];
    }
    [[nodiscard]] bool was_mouse_released(MouseButton b) const noexcept
    {
        return m_mouse_released[static_cast<crd::usize>(b)];
    }

    [[nodiscard]] crd::f32 mouse_x() const noexcept { return m_mouse_x; }
    [[nodiscard]] crd::f32 mouse_y() const noexcept { return m_mouse_y; }
    [[nodiscard]] crd::f32 mouse_dx() const noexcept { return m_mouse_dx; }
    [[nodiscard]] crd::f32 mouse_dy() const noexcept { return m_mouse_dy; }
    [[nodiscard]] crd::f32 scroll_dx() const noexcept { return m_scroll_dx; }
    [[nodiscard]] crd::f32 scroll_dy() const noexcept { return m_scroll_dy; }

    [[nodiscard]] const KeyMods& mods() const noexcept { return m_mods; }

    // Internal: only Input is allowed to mutate.
    friend class Input;

private:
    std::array<bool, static_cast<crd::usize>(Key::Count)> m_key_down{};
    std::array<bool, static_cast<crd::usize>(Key::Count)> m_key_pressed{};
    std::array<bool, static_cast<crd::usize>(Key::Count)> m_key_released{};

    std::array<bool, static_cast<crd::usize>(MouseButton::Count)> m_mouse_down{};
    std::array<bool, static_cast<crd::usize>(MouseButton::Count)> m_mouse_pressed{};
    std::array<bool, static_cast<crd::usize>(MouseButton::Count)> m_mouse_released{};

    crd::f32 m_mouse_x = 0.0f;
    crd::f32 m_mouse_y = 0.0f;
    crd::f32 m_mouse_dx = 0.0f;
    crd::f32 m_mouse_dy = 0.0f;
    crd::f32 m_scroll_dx = 0.0f;
    crd::f32 m_scroll_dy = 0.0f;

    KeyMods m_mods{};

    bool m_mouse_seeded = false;
};

// Input — owned by the Window it's attached to. GLFW callbacks write into
// this object's pending buffer; engine code reads either through the
// frame-coherent `state()` snapshot (default) or by draining the opt-in
// event queue with `try_pop_event()`.
//
// Hybrid model from v1a's design: snapshot is always there, the queue is
// only for callers that need ordered presses (text editing-like flows,
// debug overlays). The queue defaults to empty; call
// `enable_event_queue(capacity_pow2)` once to allocate it.
class Input
{
public:
    explicit Input() noexcept;
    ~Input() noexcept = default;

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) noexcept = default;
    Input& operator=(Input&&) noexcept = default;

    // Read-only frame snapshot.
    [[nodiscard]] const InputState& state() const noexcept { return m_state; }

    // Enable the optional ordered event queue. Capacity must be a power
    // of two. Calling this more than once replaces the queue.
    void enable_event_queue(crd::usize capacity_pow2);

    // Drain one event in arrival order. Returns false when the queue is
    // empty or was never enabled.
    [[nodiscard]] bool try_pop_event(InputEvent& out) noexcept;

    // Returns true if the queue is currently allocated.
    [[nodiscard]] bool event_queue_enabled() const noexcept { return m_queue.has_value(); }

    // Internal: Window calls these. Not part of the user-facing API.
    void on_poll_begin() noexcept;
    void push_key_event(Key k, InputEvent::Type type, KeyMods mods) noexcept;
    void push_mouse_button_event(MouseButton b, InputEvent::Type type, KeyMods mods) noexcept;
    void push_mouse_move(crd::f32 x, crd::f32 y) noexcept;
    void push_scroll(crd::f32 dx, crd::f32 dy) noexcept;
    void push_resize(crd::i32 width, crd::i32 height) noexcept;

private:
    // Tiny optional<RingBuffer<T>> shim. RingBuffer doesn't have a default
    // ctor (it requires capacity at construction), so we hold it as a heap
    // pointer that's null until enable_event_queue() is called. Inline so
    // we don't need explicit template instantiation.
    template <typename T> class OptionalQueue
    {
    public:
        OptionalQueue() noexcept = default;

        ~OptionalQueue() noexcept { reset(); }

        OptionalQueue(const OptionalQueue&) = delete;
        OptionalQueue& operator=(const OptionalQueue&) = delete;

        OptionalQueue(OptionalQueue&& other) noexcept : m_storage(other.m_storage) { other.m_storage = nullptr; }

        OptionalQueue& operator=(OptionalQueue&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_storage = other.m_storage;
                other.m_storage = nullptr;
            }
            return *this;
        }

        [[nodiscard]] bool has_value() const noexcept { return m_storage != nullptr; }

        void emplace(crd::usize capacity_pow2)
        {
            reset();
            auto* alloc = crd::memory::default_allocator();
            void* mem =
                alloc->allocate(sizeof(crd::containers::RingBuffer<T>), alignof(crd::containers::RingBuffer<T>));
            m_storage = ::new (mem) crd::containers::RingBuffer<T>(capacity_pow2, alloc);
        }

        void reset() noexcept
        {
            if (m_storage != nullptr)
            {
                m_storage->~RingBuffer();
                crd::memory::default_allocator()->deallocate(m_storage);
                m_storage = nullptr;
            }
        }

        crd::containers::RingBuffer<T>* operator->() noexcept { return m_storage; }
        crd::containers::RingBuffer<T>& operator*() noexcept { return *m_storage; }

    private:
        crd::containers::RingBuffer<T>* m_storage = nullptr;
    };

    void enqueue(const InputEvent& evt) noexcept;

    InputState m_state{};
    OptionalQueue<InputEvent> m_queue{};
};
} // namespace crd::platform
