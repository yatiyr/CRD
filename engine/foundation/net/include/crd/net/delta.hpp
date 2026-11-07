#pragma once

// crd-net -- baseline deltas (NET.3; contract: docs/design/network-foundation.md#baseline-deltas). A message a schema
// declares `delta` travels as what changed since a message both sides hold. The codec works on the messages' encoded
// bytes, never on their values: the baseline is the very bytes the receiver holds, so a quantised field compares by its
// code and nothing has to be quantised twice, and a receiver rebuilds the whole message as one ordinary encoding that
// the generated decoder then validates and views. scripts/gen-wire-schemas.py writes one table per message, and this
// one interpreter walks them; no allocation.
//
// DeltaWriter and DeltaReader carry them, against BaselineStore's retained messages. One store can serve many writers:
// a server encodes a tick once and sends each client what changed since the tick that client last acknowledged.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/wire.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
namespace cont = crd::containers;

namespace wire
{

enum class DeltaKind : u8
{
    Bits,       // a fixed width: bool, uint, range, enum, float, bounded, quaternion
    Varuint,    // `width` is its group
    Varsint,    // `width` is its group; the encoded number is zigzag
    Bytes,      // bytes or text: `width` length bits, aligned, `low` plus the length of bytes
    Struct,     // its `children` in order
    Extensible, // its `children`, aligned, behind its length
    List,       // `width` count bits, `low` plus the count of the one child, compared by position
    KeyedList,  // the same, compared by the first field of its elements, which ascends strictly
};

// One field of a table, or a list's element.
struct DeltaField
{
    const DeltaField* children = nullptr;
    u16 child_count = 0U;
    u16 since = 0U; // the minor revision that added it
    DeltaKind kind = DeltaKind::Bits;
    bool optional = false;
    u8 width = 0U;
    u8 diff = 0U; // a change travels as the difference in groups of this many bits; zero sends the value
    u32 low = 0U;
};

// A message's table, as generated. `validate` is its generated decoder.
struct DeltaLayout
{
    const DeltaField* fields = nullptr;
    u16 field_count = 0U;
    u16 schema_minor = 0U;
    u32 max_bytes = 0U;
    WireError (*validate)(cont::ConstSpan<u8> bytes, u16 writer_minor) noexcept = nullptr;
};

template <typename M>
[[nodiscard]] inline WireError delta_validate(cont::ConstSpan<u8> bytes, u16 writer_minor) noexcept
{
    M message{};
    return decode(bytes, writer_minor, message);
}

// Rebuilding an extensible struct reserves room for its length, at most eight deep: delta_apply() and delta_encode()
// need this much more room than their result takes.
inline constexpr usize kDeltaSlackBytes = 40U;

// Writes what changed from `baseline` to `current`, both encodings of the layout's message at `minor`. NoRoom when
// `out` is too small, which a caller uses to give up once the delta is no smaller than the message; KeyOrder when a
// keyed list's keys do not ascend. On any refusal `written` is zero.
[[nodiscard]] WireError delta_encode(const DeltaLayout& layout, cont::ConstSpan<u8> baseline,
                                     cont::ConstSpan<u8> current, u16 minor, cont::Span<u8> out,
                                     usize& written) noexcept;

// Rebuilds in `out` the message a writer at `writer_minor` encoded. Hostile input: cost is linear in the baseline and
// the delta, and what comes out still has to pass layout.validate. One delta has one encoding, so a field said to have
// changed that has not is Unchanged. A writer newer than this reader may append anything; the result then lacks it, and
// decodes the same at `writer_minor`. On any refusal `written` is zero.
[[nodiscard]] WireError delta_apply(const DeltaLayout& layout, cont::ConstSpan<u8> baseline, cont::ConstSpan<u8> delta,
                                    u16 writer_minor, cont::Span<u8> out, usize& written) noexcept;

} // namespace wire

// Encoded messages by sequence, oldest given up first: memory fixed at construction, and no allocation after it. A
// message keeps its bytes while fewer than `slots` sequences have passed and later ones have not needed its room.
class BaselineStore
{
public:
    BaselineStore(memory::IAllocator& allocator, u32 slots, u32 capacity_bytes) noexcept;
    ~BaselineStore();
    BaselineStore(const BaselineStore&) = delete;
    BaselineStore& operator=(const BaselineStore&) = delete;

    // False when the allocator refused; such a store keeps nothing.
    [[nodiscard]] bool ready() const noexcept { return m_entries != nullptr; }

    // Keeps a copy. False, keeping nothing new, when it could never fit or the sequence is not above every one kept.
    [[nodiscard]] bool put(u64 sequence, cont::ConstSpan<u8> bytes) noexcept;

    // Whether `sequence` is kept, and its bytes, which stay valid until a later put().
    [[nodiscard]] bool find(u64 sequence, cont::ConstSpan<u8>& bytes) const noexcept;

private:
    struct Entry
    {
        u64 sequence = 0U;
        u32 offset = 0U;
        u32 size = 0U;
        bool used = false;
    };

    memory::IAllocator& m_allocator;
    Entry* m_entries = nullptr;
    u8* m_bytes = nullptr;
    u32 m_slots = 0U;
    u32 m_capacity = 0U;
    u32 m_head = 0U;
    u64 m_newest = 0U;
    bool m_any = false;
};

// A delta frame's body: [sequence, 16 bits][distance back to its baseline, zero for a whole message][hash of the whole
// message, 32 bits][the delta or the message]. Every number is big-endian or the wire formats' 7-bit varuint.
inline constexpr usize kDeltaHeaderMinBytes = 7U;
inline constexpr usize kDeltaHeaderMaxBytes = 9U;

// The sequence nearest `expected` with these low 16 bits (the rule of RFC 9000's packet numbers), at or above zero.
[[nodiscard]] u64 delta_expand_sequence(u64 expected, u16 low) noexcept;

// The sender's side, one per peer and delta stream. It sends what changed since the newest message the peer
// acknowledged that the store still holds, or the whole message when that is no smaller or there is none.
class DeltaWriter
{
public:
    DeltaWriter(const wire::DeltaLayout& layout, const BaselineStore& store) noexcept : m_layout(layout), m_store(store)
    {
    }

    // Writes the frame body of the store's message `sequence`, encoded at `minor`. A sequence must rise; OutOfRange
    // when it does not, or the store does not hold it.
    [[nodiscard]] wire::WireError write(u64 sequence, u16 minor, cont::Span<u8> out, usize& written) noexcept;

    // The peer holds this sequence (BaselineAck). One that does not advance, or that was never written, changes
    // nothing.
    void on_ack(u16 sequence) noexcept;

    [[nodiscard]] u64 whole_frames() const noexcept { return m_whole; }
    [[nodiscard]] u64 delta_frames() const noexcept { return m_deltas; }

private:
    const wire::DeltaLayout& m_layout;
    const BaselineStore& m_store;
    u64 m_last = 0U;
    u64 m_acked = 0U;
    bool m_wrote = false;
    bool m_has_ack = false;
    u64 m_whole = 0U;
    u64 m_deltas = 0U;
};

enum class DeltaRead : u8
{
    Delivered,
    Stale,           // not newer than what this reader has: a repeat, or overtaken on an unreliable channel
    MissingBaseline, // the baseline is gone here; its acknowledgement keeps the writer on one that is not
    Malformed,       // refused; `error` says why
    Mismatch,        // the rebuilt message is not the one its writer hashed: nothing is delivered or kept
    Full,            // the store cannot keep it
};

inline constexpr usize kDeltaReadCount = static_cast<usize>(DeltaRead::Full) + 1U;

// The receiver's side, one per delta stream. `store` is its own.
class DeltaReader
{
public:
    DeltaReader(const wire::DeltaLayout& layout, BaselineStore& store, memory::IAllocator& allocator) noexcept;
    ~DeltaReader();
    DeltaReader(const DeltaReader&) = delete;
    DeltaReader& operator=(const DeltaReader&) = delete;

    [[nodiscard]] bool ready() const noexcept { return m_scratch != nullptr && m_store.ready(); }

    // Reads one frame body, written at `writer_minor`. On Delivered, `message` views the store's copy of the whole
    // message, valid until the next read, and the generated decoder reads it at `writer_minor`. `error` says what a
    // Malformed refused. A reader older than its writer cannot rebuild what it does not know, so it cannot compare the
    // hash of a delta; it validates what it rebuilt.
    [[nodiscard]] DeltaRead read(cont::ConstSpan<u8> body, u16 writer_minor, cont::ConstSpan<u8>& message,
                                 wire::WireError& error) noexcept;

    // The sequence to acknowledge, once anything has been delivered.
    [[nodiscard]] bool has_ack() const noexcept { return m_any; }
    [[nodiscard]] u16 ack() const noexcept { return static_cast<u16>(m_newest); }

    [[nodiscard]] u64 count(DeltaRead verdict) const noexcept { return m_counts[static_cast<usize>(verdict)]; }

private:
    [[nodiscard]] DeltaRead read_body(cont::ConstSpan<u8> body, u16 writer_minor, cont::ConstSpan<u8>& message,
                                      wire::WireError& error) noexcept;

    const wire::DeltaLayout& m_layout;
    BaselineStore& m_store;
    memory::IAllocator& m_allocator;
    u8* m_scratch = nullptr;
    u64 m_newest = 0U;
    bool m_any = false;
    u64 m_counts[kDeltaReadCount] = {};
};

} // namespace crd::net
