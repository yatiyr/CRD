#pragma once

// crd-net -- QPACK, HTTP/3's field compression (4.2a; contract: docs/design/network-foundation.md#qpack). Source:
// RFC 9204.
//
//   sans-I/O  the encoder and decoder streams go in as bytes split anywhere, instructions to send come out of
//             take_instructions(); an HTTP/3 connection carries them on its unidirectional streams.
//   table     one ring of bytes and one of entries, of the size SETTINGS allowed, from the injected allocator at init().
//             Eviction moves only the head, so a new entry may name the entry it evicts (section 3.2.2): its bytes are
//             copied in rising order, and a copy never overruns the bytes it has yet to read.
//   decoder   a section whose Required Insert Count is ahead of the table blocks, up to the promised number of streams
//             (one more is the connection error section 2.2.1 names); the caller keeps its bytes and decodes it again
//             when next_unblocked() names it. Every reference is checked against the table and the Required Insert
//             Count. A section larger than the caller's storage is still read to its end and acknowledged, so the
//             dynamic table stays in step, and comes back as TooLarge.
//   encoder   sections reference an entry the peer has acknowledged, or an unacknowledged one only while the peer's
//             blocked streams allow. A field is inserted when seen for the second time, so one-off values cost the
//             stream nothing, and only if room can be made from evictable entries: acknowledged, with no outstanding
//             section referencing them (section 2.1.1). The fields RFC 9204 section 7.1 warns of (authorization,
//             cookies) and any marked never_index are never indexed, so no attacker-driven request can probe them
//             through the table; the N bit tells intermediaries the same.
//   limits    instructions waiting to be sent are bounded. A decoder that cannot queue one ends the connection with
//             H3_EXCESSIVE_LOAD (the peer withheld credit on the decoder stream); an encoder that cannot simply stops
//             inserting. An encoder tracks a fixed number of sections and writes literals beyond it.
//   memory    everything comes from the injected allocator at init(); nothing allocates after it.
//   threads   one thread at a time.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/http_fields.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
inline constexpr u64 kQpackDecompressionFailed = 0x200U;
inline constexpr u64 kQpackEncoderStreamError = 0x201U;
inline constexpr u64 kQpackDecoderStreamError = 0x202U;
inline constexpr u64 kHttp3ExcessiveLoad = 0x107U;

inline constexpr u32 kQpackMaxTableCapacity = kHttpMaxTableCapacity;
inline constexpr u32 kQpackMaxBlockedStreams = 4096U;
inline constexpr u32 kQpackMaxSections = 4096U;
inline constexpr u32 kQpackMinInstructionBytes = 64U;
inline constexpr u32 kQpackMaxInstructionBytes = 1U << 22U;
inline constexpr u32 kQpackStaticEntries = 99U;

struct QpackDecoderConfig
{
    u32 table_capacity = 4096U;   // the SETTINGS_QPACK_MAX_TABLE_CAPACITY this side sends
    u32 blocked_streams = 16U;    // the SETTINGS_QPACK_BLOCKED_STREAMS this side sends
    u32 instruction_bytes = 1024U; // decoder stream bytes waiting at most
};

enum class QpackStatus : u8
{
    Complete,
    Blocked,  // decode again after next_unblocked() names the stream
    TooLarge, // valid and acknowledged, but `fields` or `text` could not hold it
    Failed,   // a connection error: error() gives its code
};

class QpackDecoder
{
public:
    QpackDecoder() noexcept = default;
    ~QpackDecoder();
    QpackDecoder(const QpackDecoder&) = delete;
    QpackDecoder& operator=(const QpackDecoder&) = delete;
    QpackDecoder(QpackDecoder&&) = delete;
    QpackDecoder& operator=(QpackDecoder&&) = delete;

    // False, holding nothing, for a configuration out of range or a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, const QpackDecoderConfig& config) noexcept;

    // Bytes from the peer's encoder stream, split anywhere. False is a connection error (error()).
    [[nodiscard]] bool on_encoder_stream(cont::ConstSpan<u8> bytes) noexcept;

    // Decodes one whole field section read from `stream`. The fields view `text` or static data; a TooLarge section
    // leaves them unusable, and `count` is zero unless Complete.
    [[nodiscard]] QpackStatus decode(u64 stream, cont::ConstSpan<u8> section, cont::Span<HttpField> fields,
                                     cont::Span<u8> text, usize& count) noexcept;

    // A stream this side will not read further (reset, or abandoned): it stops being blocked and the peer is told.
    void cancel(u64 stream) noexcept;

    // Takes one blocked stream whose section can now be decoded, in the order they blocked. Call until it returns
    // false after every on_encoder_stream(), before decoding another stream's section: a stream it would name still
    // counts against the blocked limit until then.
    [[nodiscard]] bool next_unblocked(u64& stream) noexcept;

    // Moves decoder stream bytes into `out`, possibly cutting an instruction (it is a stream); the bytes it wrote.
    [[nodiscard]] usize take_instructions(cont::Span<u8> out) noexcept;

    [[nodiscard]] u64 error() const noexcept { return m_error; } // 0 for none
    [[nodiscard]] u32 blocked() const noexcept { return m_blocked_count; }
    [[nodiscard]] const HttpFieldTable& table() const noexcept { return m_table; }

private:
    struct Blocked
    {
        u64 stream = 0U;
        u64 required = 0U;
    };

    enum class Parse : u8
    {
        Applied,
        NeedMore,
        Failed,
    };

    [[nodiscard]] usize consume(cont::ConstSpan<u8> bytes) noexcept;
    [[nodiscard]] Parse instruction(cont::WireReader& in) noexcept;
    [[nodiscard]] bool literal_entry(cont::ConstSpan<u8> name, bool name_huffman, cont::ConstSpan<u8> value,
                                     bool value_huffman) noexcept;
    [[nodiscard]] bool name_exists(bool from_static, u64 index) const noexcept;
    // `index` names something name_exists() found.
    [[nodiscard]] bool named_entry(bool from_static, u64 index, cont::ConstSpan<u8> value, bool huffman) noexcept;
    [[nodiscard]] bool read_prefix(cont::WireReader& in, u64& required, u64& base) const noexcept;
    [[nodiscard]] QpackStatus block(u64 stream, u64 required) noexcept;
    void forget_blocked(u64 stream) noexcept;
    void queue_instruction(u8 flags, u8 prefix_bits, u64 value) noexcept;
    bool fail(u64 code) noexcept;
    void release() noexcept;

    memory::IAllocator* m_allocator = nullptr;
    QpackDecoderConfig m_config;
    HttpFieldTable m_table;
    u8* m_partial = nullptr; // an instruction arriving in parts
    usize m_partial_bytes = 0U;
    usize m_partial_capacity = 0U;
    u8* m_scratch = nullptr; // Huffman-decoded text of an instruction, at most the table's capacity
    u8* m_out = nullptr;
    usize m_out_bytes = 0U;
    Blocked* m_blocked = nullptr;
    u32 m_blocked_count = 0U;
    u64 m_known_received = 0U; // the Insert Count the peer has been told of
    u64 m_error = 0U;
};

struct QpackEncoderConfig
{
    u32 table_capacity = 4096U;    // the most this side will use, whatever the peer allows; 0 uses none
    u32 sections = 64U;            // sections with dynamic references outstanding at most
    u32 instruction_bytes = 8192U; // encoder stream bytes waiting at most
};

class QpackEncoder
{
public:
    QpackEncoder() noexcept = default;
    ~QpackEncoder();
    QpackEncoder(const QpackEncoder&) = delete;
    QpackEncoder& operator=(const QpackEncoder&) = delete;
    QpackEncoder(QpackEncoder&&) = delete;
    QpackEncoder& operator=(QpackEncoder&&) = delete;

    // False, holding nothing, for a configuration out of range or a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, const QpackEncoderConfig& config) noexcept;

    // The peer's SETTINGS, once: until then the maximum capacity is zero (RFC 9204 section 3.2.3) and sections use the
    // static table only.
    void peer_settings(u64 max_table_capacity, u64 blocked_streams) noexcept;

    // Bytes from the peer's decoder stream, split anywhere. False is a connection error (error()).
    [[nodiscard]] bool on_decoder_stream(cont::ConstSpan<u8> bytes) noexcept;

    // Writes the field section of `fields` for `stream` into `out`; the bytes it wrote, 0 when `out` is too small.
    // Instructions it needs join the encoder stream first, so send those before or with the section.
    [[nodiscard]] usize encode(u64 stream, cont::ConstSpan<HttpField> fields, cont::Span<u8> out) noexcept;

    // Moves encoder stream bytes into `out`; the bytes it wrote, possibly cutting an instruction (it is a stream).
    [[nodiscard]] usize take_instructions(cont::Span<u8> out) noexcept;

    [[nodiscard]] u64 error() const noexcept { return m_error; } // 0 for none
    [[nodiscard]] const HttpFieldTable& table() const noexcept { return m_table; }
    [[nodiscard]] u64 known_received() const noexcept { return m_known_received; }
    [[nodiscard]] u32 outstanding() const noexcept { return m_section_count; }

private:
    struct Section
    {
        u64 stream = 0U;
        u64 required = 0U;
        u64 lowest = 0U; // the oldest entry it references
    };

    // The section being written.
    struct Work
    {
        u64 stream = 0U;
        u64 base = 0U;
        u64 required = 0U;
        u64 lowest = ~u64{0};
        bool may_reference = false; // a slot is free for this section
        bool may_block = false;
    };

    [[nodiscard]] usize write_line(const HttpField& field, Work& work, cont::Span<u8> out) noexcept;
    [[nodiscard]] bool usable(u64 absolute, const Work& work) const noexcept;
    // Duplicates the entry `absolute` if it is close to eviction; `absolute` becomes the copy if it may be referenced.
    void refresh(u64& absolute, const Work& work) noexcept;
    [[nodiscard]] bool seen_before(const HttpField& field) noexcept;
    [[nodiscard]] bool insert(const HttpField& field, i32 static_name, const Work& work) noexcept;
    [[nodiscard]] bool room_for(usize entry_bytes, const Work& work) const noexcept;
    // The streams with a blocking section outstanding, other than `except`.
    [[nodiscard]] usize blocking_streams(u64 except) const noexcept;
    [[nodiscard]] bool instruction(u8 first, u64 value) noexcept;
    bool fail() noexcept;
    void release() noexcept;

    memory::IAllocator* m_allocator = nullptr;
    QpackEncoderConfig m_config;
    HttpFieldTable m_table;
    Section* m_sections = nullptr; // oldest first
    u32 m_section_count = 0U;
    u8* m_out = nullptr;
    usize m_out_bytes = 0U;
    u8 m_partial[kHttpMaxIntegerBytes] = {};
    usize m_partial_bytes = 0U;
    u64 m_peer_max_entries = 0U; // floor(MaxTableCapacity / 32)
    u64 m_peer_blocked = 0U;
    u64 m_known_received = 0U;
    u64 m_error = 0U;
    bool m_settings_known = false;
    u32 m_seen[64] = {}; // hashes of the fields seen once
};

} // namespace crd::net
