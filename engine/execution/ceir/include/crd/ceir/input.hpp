#pragma once

// crd-ceir -- DIAG.9a the host INPUT SEAM. A program reads values the host chooses at run time (a random stream today)
// through the `input` dialect's ops; both executors deliver them through one `InputSource` the host installs
// (`exec::Interpreter::set_input_source`, the `inputs` argument of `plan::run`). The seam delivers the RAW value of one
// read; the op applies its own semantics to it (input.random reduces it to [0, bound)), so the reduction is program
// semantics and a replay that feeds the same raw values to an edited program sees what that program would see.
//
// A source is called on the executing thread, once per read, in program order: a run record keeps every read (and a
// read the host could not answer) and a replay feeds them back in that order, never asking a live host. A host with no
// source for a kind answers false, and the read fails with a typed `InputUnavailable` error in both executors; it is
// never answered with a made-up value. Reads are schedule-dependent, like a §20 cell, so a parallel or pooled body may
// not contain one (`exec::region_state_free` refuses them).
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/input_ops.hpp> // register_input_ops, random_kind, build_random
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
};
inline constexpr InputKind kLastInputKind = InputKind::Random;

// "random".
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

// Whether `kind` names an op of this dialect that reads a host input (input.random). No interning: `ctx` may be const.
[[nodiscard]] bool reads_input(const Context& ctx, OpId kind) noexcept;

// input.random's attributes: `stream` in [0, 2^32) and `bound` in [1, 2^32). False when either is absent, not an
// integer or out of range (the arith.const precedent: UndefinedValue at eval, BadConst at plan compile).
[[nodiscard]] bool random_attrs(const Context& ctx, const Operation& op, crd::u32& stream, crd::u64& bound) noexcept;

// input.random's reduction of a raw draw to [0, bound): the unsigned remainder. `bound` is at least 1.
[[nodiscard]] constexpr crd::i64 reduce_draw(crd::i64 raw, crd::u64 bound) noexcept
{
    return static_cast<crd::i64>(static_cast<crd::u64>(raw) % bound);
}

// A host input source whose random streams are drawn from one seed. Draw `n` of stream `s` is a splitmix64 mix of
// (seed, s, n): streams are independent (a draw from one never moves another) and one seed always gives the same
// draws. Not cryptographic. Other kinds have no value. One run's source: construct it per run, use it on one thread.
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

    // Draw `n` (0-based) of stream `stream` for `seed`: what the source delivers for that read.
    [[nodiscard]] static crd::i64 draw(crd::u64 seed, crd::u32 stream, crd::u64 n) noexcept;

private:
    static bool next(InputKind kind, crd::u32 channel, crd::i64& out, void* user);

    crd::u64                                m_seed;
    containers::HashMap<crd::u32, crd::u64> m_drawn; // per stream: the draws delivered so far
    InputSource                             m_source;
};
} // namespace crd::ceir::input
