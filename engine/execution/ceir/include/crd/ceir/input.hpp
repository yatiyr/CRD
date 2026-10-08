#pragma once

// crd-ceir -- DIAG.9a the host INPUT SEAM. A program reads values the host chooses at run time (a random stream, a time
// domain's reading and its step, the next event of an input event queue) through the `input` dialect's ops; both
// executors deliver them through one `InputSource` the host installs (`exec::Interpreter::set_input_source`, the
// `inputs` argument of `plan::run`). The seam delivers the RAW value of one read; the op applies its own semantics to
// it (input.random reduces it to [0, bound); a time read takes it as is; input.event unpacks it), so those semantics
// are the program's and a replay that feeds the same raw values to an edited program sees what that program would
// see.
//
// A source is called on the executing thread, once per read, in program order: a run record keeps every read (and a
// read the host could not answer) and a replay feeds them back in that order, never asking a live host. A host with no
// source for a kind answers false, and the read fails with a typed `InputUnavailable` error in both executors; it is
// never answered with a made-up value. Reads are schedule-dependent, like a §20 cell, so a parallel or pooled body may
// not contain one (`exec::region_state_free` refuses them).
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/input_ops.hpp> // register_input_ops, random_kind, clock_kind, time_step_kind, event_kind
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir::input
{
// The kinds of host input a program can read. ⛔ Append at end: the ordinal is stored in run records.
// NOLINTNEXTLINE(performance-enum-size)
enum class InputKind : crd::u8
{
    Random = 0, // input.random: one raw 64-bit draw of a random stream (the channel is the stream)
    Clock,      // input.clock: a time domain's current reading (the channel is the built-in domain's ordinal)
    TimeStep,   // input.time_step: the length of a time domain's current step (the channel as for Clock)
    Event,      // input.event: the next packed event of a host input event queue (the channel is the queue)
};
inline constexpr InputKind kLastInputKind = InputKind::Event;

// The op that reads the kind: "random", "clock", "time_step" or "event".
[[nodiscard]] containers::StringView input_kind_name(InputKind k) noexcept;

// The host's input seam. `next` delivers the next raw value of `kind` on `channel` into `out` and returns true, or
// returns false when the host has no value for it. `user` is the host's own.
struct InputSource
{
    bool (*next)(InputKind kind, crd::u32 channel, crd::i64& out, void* user) = nullptr;
    void* user                                                               = nullptr;
};

// One read through `source`. A null source, or one without `next`, has no value: false.
[[nodiscard]] bool read_input(const InputSource* source, InputKind kind, crd::u32 channel, crd::i64& out);

// Whether `kind` names an op of this dialect that reads a host input (input.random, input.clock, input.time_step,
// input.event). No interning: `ctx` may be const.
[[nodiscard]] bool reads_input(const Context& ctx, OpId kind) noexcept;

// input.random's attributes: `stream` in [0, 2^32) and `bound` in [1, 2^32). False when either is absent, not an
// integer or out of range (the arith.const precedent: UndefinedValue at eval, BadConst at plan compile).
[[nodiscard]] bool random_attrs(const Context& ctx, const Operation& op, crd::u32& stream, crd::u64& bound) noexcept;

// input.clock's and input.time_step's attribute: `domain`, the name of one of the time dialect's built-in domains
// (crd/ceir/time.hpp). `domain` receives its ordinal, the seam channel. False when it is absent, not a string or not a
// built-in (UndefinedValue at eval, BadConst at plan compile).
[[nodiscard]] bool time_attrs(const Context& ctx, const Operation& op, crd::u32& domain) noexcept;

// input.event's attribute: `queue` in [0, 2^32). False when it is absent, not an integer or out of range
// (UndefinedValue at eval, BadConst at plan compile).
[[nodiscard]] bool event_attrs(const Context& ctx, const Operation& op, crd::u32& queue) noexcept;

// input.random's reduction of a raw draw to [0, bound): the unsigned remainder. `bound` is at least 1.
[[nodiscard]] constexpr crd::i64 reduce_draw(crd::i64 raw, crd::u64 bound) noexcept
{
    return static_cast<crd::i64>(static_cast<crd::u64>(raw) % bound);
}

// The type of a host input event, in the platform's InputEvent::Type order. ⛔ Append at end: the value is stored in
// run records as part of a packed event. None is an empty queue's answer.
// NOLINTNEXTLINE(performance-enum-size)
enum class EventType : crd::u8
{
    None = 0,
    KeyDown,
    KeyUp,
    KeyRepeat,
    MouseDown,
    MouseUp,
    MouseMove, // x, y: the pointer position in whole window pixels
    Scroll,    // x, y: the scroll offset in hundredths of a step
    Resize,    // x, y: the new size in pixels
};
inline constexpr EventType kLastEventType = EventType::Resize;

// The modifier bits of an event's `mods`.
inline constexpr crd::u8 kModShift = 1U;
inline constexpr crd::u8 kModCtrl  = 2U;
inline constexpr crd::u8 kModAlt   = 4U;
inline constexpr crd::u8 kModSuper = 8U;

// "none", "key_down", "key_up", "key_repeat", "mouse_down", "mouse_up", "mouse_move", "scroll", "resize" ("?" past the
// last type).
[[nodiscard]] containers::StringView event_type_name(EventType t) noexcept;

// The type named `name` (see event_type_name; "none" included). False when it names none.
[[nodiscard]] bool event_type_of(containers::StringView name, EventType& out) noexcept;

// One host input event, as input.event's results see it. The packed form (pack_event) is what the seam delivers and a
// run record keeps: bits 0..7 the type, 8..23 the code, 24..31 the mods, 32..47 x and 48..63 y (two's complement).
struct Event
{
    crd::u8  type = 0U; // an EventType (a packed value may hold one past the last; the op unpacks it as is)
    crd::u16 code = 0U; // the key or mouse button
    crd::u8  mods = 0U; // kMod* bits
    crd::i16 x    = 0;
    crd::i16 y    = 0;
};

[[nodiscard]] constexpr crd::i64 pack_event(const Event& e) noexcept
{
    const crd::u64 bits = static_cast<crd::u64>(e.type) | (static_cast<crd::u64>(e.code) << 8U) |
                          (static_cast<crd::u64>(e.mods) << 24U) |
                          (static_cast<crd::u64>(static_cast<crd::u16>(e.x)) << 32U) |
                          (static_cast<crd::u64>(static_cast<crd::u16>(e.y)) << 48U);
    return static_cast<crd::i64>(bits);
}

// input.event's unpacking of a raw read: every field, whatever its value (a raw 0 is a None event).
[[nodiscard]] constexpr Event unpack_event(crd::i64 raw) noexcept
{
    const auto bits = static_cast<crd::u64>(raw);
    Event      e;
    e.type = static_cast<crd::u8>(bits & 0xFFU);
    e.code = static_cast<crd::u16>((bits >> 8U) & 0xFFFFU);
    e.mods = static_cast<crd::u8>((bits >> 24U) & 0xFFU);
    e.x    = static_cast<crd::i16>(static_cast<crd::u16>((bits >> 32U) & 0xFFFFU));
    e.y    = static_cast<crd::i16>(static_cast<crd::u16>((bits >> 48U) & 0xFFFFU));
    return e;
}

// A host input source whose random streams are drawn from one seed. Draw `n` of stream `s` is a splitmix64 mix of
// (seed, s, n): streams are independent (a draw from one never moves another) and one seed always gives the same
// draws. Not cryptographic. Other kinds have no value. One run's source: construct it per run (or `reset` it between
// runs), use it on one thread at a time.
class SeededInputs
{
public:
    SeededInputs(crd::u64 seed, memory::IAllocator* alloc);
    SeededInputs(const SeededInputs&)            = delete;
    SeededInputs& operator=(const SeededInputs&) = delete;
    SeededInputs(SeededInputs&&)                 = delete;
    SeededInputs& operator=(SeededInputs&&)      = delete;
    ~SeededInputs()                              = default;

    // The source to install; it points at this object.
    [[nodiscard]] const InputSource* source() const noexcept { return &m_source; }
    [[nodiscard]] crd::u64           seed() const noexcept { return m_seed; }

    // Start every stream over at draw 0 of `seed`: the next run reads what a new source for `seed` would deliver. Not
    // while a run reads through `source()`.
    void reset(crd::u64 seed);

    // Draw `n` (0-based) of stream `stream` for `seed`: what the source delivers for that read.
    [[nodiscard]] static crd::i64 draw(crd::u64 seed, crd::u32 stream, crd::u64 n) noexcept;

private:
    static bool next(InputKind kind, crd::u32 channel, crd::i64& out, void* user);

    crd::u64                                m_seed;
    containers::HashMap<crd::u32, crd::u64> m_drawn; // per stream: the draws delivered so far
    InputSource                             m_source;
};

// A host's clock: the reading and the step of each of the time dialect's built-in domains, answering Clock and
// TimeStep reads (other kinds have no value). A domain has no reading and no step until the host sets them; a read of
// one it has not set has no value. The wall domain can instead be LIVE: its reading is then the host's monotonic clock
// in nanoseconds since `use_live_wall` (the clock's epoch), never calendar time. The monotonic reader is the host's
// (crd-ceir links no clock): it returns nanoseconds from any fixed origin. Holds no allocator and never allocates. One
// run's source: the host changes it only while no run reads through `source()`.
class HostClock
{
public:
    // Nanoseconds on the host's monotonic clock, from any fixed origin.
    using MonotonicReader = crd::i64 (*)(void* user);

    HostClock() noexcept;
    HostClock(const HostClock&)            = delete;
    HostClock& operator=(const HostClock&) = delete;
    HostClock(HostClock&&)                 = delete;
    HostClock& operator=(HostClock&&)      = delete;
    ~HostClock()                           = default;

    // The source to install; it points at this object.
    [[nodiscard]] const InputSource* source() const noexcept { return &m_source; }

    // Domain `domain` (a built-in ordinal; any other is ignored) reads `now`; its step stays as it was.
    void set_reading(crd::u32 domain, crd::i64 now) noexcept;
    // Domain `domain`'s current step is `step`; its reading stays as it was.
    void set_step(crd::u32 domain, crd::i64 step) noexcept;
    // One step of `step`: domain `domain` reads its reading plus `step` (wrapping; from 0 when it had none) and its
    // current step is `step`. A live wall's reading is the monotonic clock's, so only its step changes.
    void advance(crd::u32 domain, crd::i64 step) noexcept;
    // Make the wall domain live: its reading is `read(user)` minus the reading at this call. `read` must be callable on
    // the executing thread.
    void use_live_wall(MonotonicReader read, void* user) noexcept;
    // No domain has a reading or a step, and the wall is not live.
    void clear() noexcept;

    [[nodiscard]] bool live_wall() const noexcept { return m_wall_read != nullptr; }

private:
    struct Domain
    {
        bool     has_now  = false;
        bool     has_step = false;
        crd::i64 now      = 0;
        crd::i64 step     = 0;
    };

    static bool next(InputKind kind, crd::u32 channel, crd::i64& out, void* user);

    Domain          m_domains[6]; // the built-in domains, by ordinal (time::kBuiltinDomainCount)
    MonotonicReader m_wall_read  = nullptr;
    void*           m_wall_user  = nullptr;
    crd::i64        m_wall_epoch = 0;
    InputSource     m_source;
};

// A host's input event queues, answering Event reads (other kinds have no value). A queue exists once the host opens
// it or pushes to it; a read takes that queue's next event in push order (packed, see pack_event) and, once none is
// left, delivers 0 (a None event). A read of a queue the host does not have has no value. Queues are independent: a
// read of one never moves another. It holds at most kMaxEvents events and kMaxQueues queues; a push or open past
// either is refused. Reads never allocate; `open` and `push` grow the arrays on the host's allocator. One run's
// source: the host changes it only while no run reads through `source()`.
class HostEvents
{
public:
    static constexpr crd::u32 kMaxEvents = 4096U;
    static constexpr crd::u32 kMaxQueues = 16U;

    explicit HostEvents(memory::IAllocator* alloc);
    HostEvents(const HostEvents&)            = delete;
    HostEvents& operator=(const HostEvents&) = delete;
    HostEvents(HostEvents&&)                 = delete;
    HostEvents& operator=(HostEvents&&)      = delete;
    ~HostEvents()                            = default;

    // The source to install; it points at this object.
    [[nodiscard]] const InputSource* source() const noexcept { return &m_source; }

    // Queue `queue` exists (empty until pushed). False when it would be one queue past kMaxQueues.
    bool open(crd::u32 queue);
    // Append `event` (packed) to queue `queue`, opening it. False (nothing kept) past kMaxEvents or kMaxQueues.
    bool push(crd::u32 queue, crd::i64 event);
    // Every queue reads again from its first event.
    void rewind() noexcept;
    // No queue and no event.
    void clear() noexcept;

    // The events queue `queue` has not delivered yet (0 for a queue it does not have).
    [[nodiscard]] crd::u32 pending(crd::u32 queue) const noexcept;
    // Every event held, delivered or not.
    [[nodiscard]] crd::u32 size() const noexcept { return static_cast<crd::u32>(m_events.size()); }

private:
    struct Entry
    {
        crd::u32 queue = 0U;
        crd::i64 event = 0;
    };
    struct Queue
    {
        crd::u32 id   = 0U;
        crd::u32 next = 0U; // the index in m_events its next event is searched from
    };

    [[nodiscard]] crd::usize find(crd::u32 queue) const noexcept; // m_queues.size() when it has none
    static bool              next(InputKind kind, crd::u32 channel, crd::i64& out, void* user);

    containers::Array<Entry> m_events;
    containers::Array<Queue> m_queues;
    InputSource              m_source;
};

// One source over several: each kind goes to the source the host routed it to (none: no value). The sources are the
// host's and must outlive every read. Never allocates.
class InputRouter
{
public:
    InputRouter() noexcept;
    InputRouter(const InputRouter&)            = delete;
    InputRouter& operator=(const InputRouter&) = delete;
    InputRouter(InputRouter&&)                 = delete;
    InputRouter& operator=(InputRouter&&)      = delete;
    ~InputRouter()                             = default;

    // The source to install; it points at this object.
    [[nodiscard]] const InputSource* source() const noexcept { return &m_source; }

    // Reads of `kind` go to `to` (nullptr: no value).
    void route(InputKind kind, const InputSource* to) noexcept;

private:
    static bool next(InputKind kind, crd::u32 channel, crd::i64& out, void* user);

    const InputSource* m_routes[static_cast<crd::u32>(kLastInputKind) + 1U] = {};
    InputSource        m_source;
};
} // namespace crd::ceir::input
