#pragma once

// crd-net -- HPACK, HTTP/2's field compression (4.2a; contract: docs/design/network-foundation.md#hpack). Sources: RFC
// 7541 and RFC 9113 sections 4.3 and 6.5.2.
//
//   order     a block is decoded whole and in the order the connection received it: there are no acknowledgements and
//             nothing blocks, and the table the two sides hold stays the same only if every block is read to its end.
//   decoder   a section larger than the caller's storage is still read to its end, so the table stays in step, and
//             comes back as TooLarge. Any refusal stops the decoder for good: RFC 9113 section 4.3 makes it a
//             connection error, COMPRESSION_ERROR.
//   table     the size this side allows only binds once the peer acknowledged the SETTINGS carrying it (set_limit()).
//             A limit that fell below the table's size obliges the next block to open with a size update at or under it
//             (RFC 7541 section 4.2). An entry larger than the table empties it and is not an error (section 4.4).
//   encoder   a block is refused whole, before any state changes, when `out` may not hold it. A field goes in the table
//             the first time it is seen unless it would take more than three quarters of it or is of a kind that rarely
//             repeats (:path, content-length and the like); the fields RFC 7541 section 7.1 warns of and any marked
//             never_index stay out and carry the never-indexed form.
//   updates   a side told to use a new size says so at the start of its next block: the smallest size since its last
//             block if lower than the last one signalled, then the final one, at most two.
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
inline constexpr u32 kHpackStaticEntries = 61U;
inline constexpr u32 kHpackDefaultTableSize = 4096U; // SETTINGS_HEADER_TABLE_SIZE's initial value
inline constexpr u32 kHpackMaxTableSize = kHttpMaxTableCapacity;

enum class HpackStatus : u8
{
    Complete,
    TooLarge, // valid and applied to the table, but `fields` or `text` could not hold it
    Failed,   // a connection error, COMPRESSION_ERROR
};

class HpackDecoder
{
public:
    HpackDecoder() noexcept = default;
    ~HpackDecoder();
    HpackDecoder(const HpackDecoder&) = delete;
    HpackDecoder& operator=(const HpackDecoder&) = delete;
    HpackDecoder(HpackDecoder&&) = delete;
    HpackDecoder& operator=(HpackDecoder&&) = delete;

    // `max_table_size` is the largest SETTINGS_HEADER_TABLE_SIZE this side will send. The table also holds the 4096
    // bytes a peer may use before its first SETTINGS is acknowledged. False, holding nothing, for a size out of range
    // or a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, u32 max_table_size) noexcept;

    // This side's SETTINGS_HEADER_TABLE_SIZE, when the peer has acknowledged it; at most init()'s size.
    void set_limit(u32 limit) noexcept;

    // Decodes one whole field block. The fields view `text` or static data; a TooLarge block leaves them unusable, and
    // `count` is zero unless Complete.
    [[nodiscard]] HpackStatus decode(cont::ConstSpan<u8> block, cont::Span<HttpField> fields, cont::Span<u8> text,
                                     usize& count) noexcept;

    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] u32 limit() const noexcept { return m_limit; }
    [[nodiscard]] const HttpFieldTable& table() const noexcept { return m_table; }

private:
    struct NameRef
    {
        cont::ConstSpan<u8> fixed;
        u64 absolute = 0U;
        bool dynamic = false;
    };

    [[nodiscard]] bool size_update(cont::WireReader& in, u8 first, u32& updates) noexcept;
    [[nodiscard]] bool resolve(u64 index, NameRef& name, cont::ConstSpan<u8>* fixed_value) const noexcept;
    void insert(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value, bool oversize) noexcept;
    void release() noexcept;

    memory::IAllocator* m_allocator = nullptr;
    HttpFieldTable m_table;
    u8* m_scratch = nullptr; // a line past the caller's storage, at most the table's size
    u32 m_scratch_bytes = 0U;
    u32 m_limit = kHpackDefaultTableSize;
    u32 m_owed_at_most = 0U; // with m_update_owed: what the next block's first update may be at most
    bool m_update_owed = false;
    bool m_failed = false;
};

class HpackEncoder
{
public:
    HpackEncoder() noexcept = default;
    ~HpackEncoder();
    HpackEncoder(const HpackEncoder&) = delete;
    HpackEncoder& operator=(const HpackEncoder&) = delete;
    HpackEncoder(HpackEncoder&&) = delete;
    HpackEncoder& operator=(HpackEncoder&&) = delete;

    // `table_capacity` is the most this side uses, whatever the peer allows; 0 uses none. False, holding nothing, for a
    // size out of range or a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, u32 table_capacity) noexcept;

    // The peer's SETTINGS_HEADER_TABLE_SIZE, each time it arrives.
    void set_peer_limit(u32 size) noexcept;

    // The most encode() can write for `fields`.
    [[nodiscard]] static usize max_bytes(cont::ConstSpan<HttpField> fields) noexcept;

    // Writes the field block of `fields` into `out`: the bytes it wrote. 0, changing nothing, when `out` is smaller
    // than max_bytes().
    [[nodiscard]] usize encode(cont::ConstSpan<HttpField> fields, cont::Span<u8> out) noexcept;

    [[nodiscard]] const HttpFieldTable& table() const noexcept { return m_table; }

private:
    void write_updates(cont::WireWriter& out) noexcept;
    void write_line(const HttpField& field, cont::WireWriter& out) noexcept;
    void release() noexcept;

    memory::IAllocator* m_allocator = nullptr;
    HttpFieldTable m_table;
    u32 m_max_capacity = 0U;
    u32 m_signalled = kHpackDefaultTableSize; // the size the peer's decoder is using
    u32 m_lowest = kHpackDefaultTableSize;    // the smallest size allowed since the last block
    u32 m_target = kHpackDefaultTableSize;    // the size to use from the next block on
};

} // namespace crd::net
