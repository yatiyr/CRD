#pragma once

// crd-ceir -- DIAG.9a the host INPUT SEAM. A program reads values the host chooses at run time (a random stream, a time
// domain's reading and its step) through the `input` dialect's ops; both executors deliver them through one
// `InputSource` the host installs (`exec::Interpreter::set_input_source`, the `inputs` argument of `plan::run`). The
// seam delivers the RAW value of one read; the op applies its own semantics to it (input.random reduces it to
// [0, bound); a time read takes it as is), so the reduction is program semantics and a replay that feeds the same raw
// values to an edited program sees what that program would see.
//
// A source is called on the executing thread, once per read, in program order: a run record keeps every read (and a
// read the host could not answer) and a replay feeds them back in that order, never asking a live host. A host with no
// source for a kind answers false, and the read fails with a typed `InputUnavailable` error in both executors; it is
// never answered with a made-up value. Reads are schedule-dependent, like a §20 cell, so a parallel or pooled body may
// not contain one (`exec::region_state_free` refuses them).
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/input_ops.hpp> // register_input_ops, random_kind, clock_kind, time_step_kind, build_*
#include <crd/ceir/ir.hpp>
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
};
inline constexpr InputKind kLastInputKind = InputKind::TimeStep;

// The op that reads the kind: "random", "clock" or "time_step".
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

// Whether `kind` names an op of this dialect that reads a host input (input.random, input.clock, input.time_step). No
// interning: `ctx` may be const.
[[nodiscard]] bool reads_input(const Context& ctx, OpId kind) noexcept;

// input.random's attributes: `stream` in [0, 2^32) and `bound` in [1, 2^32). False when either is absent, not an
// integer or out of range (the arith.const precedent: UndefinedValue at eval, BadConst at plan compile).
[[nodiscard]] bool random_attrs(const Context& ctx, const Operation& op, crd::u32& stream, crd::u64& bound) noexcept;

// input.clock's and input.time_step's attribute: `domain`, the name of one of the time dialect's built-in domains
// (crd/ceir/time.hpp). `domain` receives its ordinal, the seam channel. False when it is absent, not a string or not a
// built-in (UndefinedValue at eval, BadConst at plan compile).
[[nodiscard]] bool time_attrs(const Context& ctx, const Operation& op, crd::u32& domain) noexcept;

// input.random's reduction of a raw draw to [0, bound): the unsigned remainder. `bound` is at least 1.
[[nodiscard]] constexpr crd::i64 reduce_draw(crd::i64 raw, crd::u64 bound) noexcept
{
    return static_cast<crd::i64>(static_cast<crd::u64>(raw) % bound);
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
