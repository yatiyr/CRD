#pragma once

// crd-net -- the field coding HPACK and QPACK share (4.2a; contract: docs/design/network-foundation.md#qpack). Sources:
// RFC 7541 sections 5.1 and 5.2 and appendix B, RFC 9204 sections 1.2 and 4.1.
//
//   integers  prefix integers up to 2^62 - 1 in at most ten continuation bytes; a longer or larger one is invalid.
//   strings   Huffman-coded only when that is strictly shorter. The decoder holds a code to section 5.2: at most seven
//             bits of padding, all ones, and no EOS.
//   table     the dynamic table both compressions use, with the rules they share.
//   memory    only a table's init() allocates; hostile input costs time linear in its length.
//   threads   the codings hold no state; a table and an output belong to one thread at a time.

#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
namespace cont = crd::containers;

// One field line. A decoder's views live in the storage given to it (or in a static table).
struct HttpField
{
    cont::ConstSpan<u8> name;
    cont::ConstSpan<u8> value;
    bool never_index = false; // no intermediary or encoder may index it (RFC 9204 section 7.1.3)
};

inline constexpr usize kHttpFieldOverhead = 32U; // the bytes an entry or a field line counts for beyond its text
inline constexpr u64 kHttpMaxInteger = (u64{1} << 62U) - 1U;
inline constexpr usize kHttpMaxIntegerBytes = 11U; // a first byte and ten continuation bytes

enum class HttpDecode : u8
{
    Ok,
    NeedMore, // the input ended first
    Invalid,
};

// The bytes `value` takes with a prefix of `prefix_bits` (1 to 8).
[[nodiscard]] usize http_int_bytes(u64 value, u8 prefix_bits) noexcept;
// Writes `value` in the low `prefix_bits` of a byte holding `flags` in its high bits.
void put_http_int(cont::WireWriter& out, u8 flags, u8 prefix_bits, u64 value) noexcept;
// Reads the rest of an integer whose first byte, `first`, the caller has taken.
[[nodiscard]] HttpDecode take_http_int(cont::WireReader& in, u8 first, u8 prefix_bits, u64& value) noexcept;

// The bytes `text` takes as a Huffman code.
[[nodiscard]] usize http_huffman_bytes(cont::ConstSpan<u8> text) noexcept;
// Writes the code of `text`, padded with ones; `out` holds at least http_huffman_bytes(text).
void http_huffman_encode(cont::ConstSpan<u8> text, cont::Span<u8> out) noexcept;
// Decodes `coded`. `decoded` is the length of the text however small `out` is, and `out` holds its first bytes, so an
// empty `out` measures and validates. False for a code RFC 7541 section 5.2 refuses.
[[nodiscard]] bool http_huffman_decode(cont::ConstSpan<u8> coded, cont::Span<u8> out, usize& decoded) noexcept;

// The bytes a string takes: its length with a prefix of `prefix_bits` (the Huffman flag is the next bit up) and the
// shorter of its two forms.
[[nodiscard]] usize http_string_bytes(cont::ConstSpan<u8> text, u8 prefix_bits) noexcept;
void put_http_string(cont::WireWriter& out, u8 flags, u8 prefix_bits, cont::ConstSpan<u8> text) noexcept;
// Reads a string whose first byte, `first`, the caller has taken: `coded` views its bytes in the input and `huffman`
// tells their form. Invalid for one longer than `max_coded`, as soon as its length is read.
[[nodiscard]] HttpDecode take_http_string(cont::WireReader& in, u8 first, u8 prefix_bits, usize max_coded,
                                          cont::ConstSpan<u8>& coded, bool& huffman) noexcept;

inline constexpr u32 kHttpMaxTableCapacity = 1U << 20U; // the most a dynamic table is offered or used at

template <usize N> [[nodiscard]] cont::ConstSpan<u8> http_text(const char (&text)[N]) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text), N - 1U);
}

[[nodiscard]] bool http_equal(cont::ConstSpan<u8> left, cont::ConstSpan<u8> right) noexcept;

// RFC 9204 section 7.1 and RFC 7541 section 7.1: a field an attacker could guess at through the size of what it shares
// a table with. Never indexed, however it was offered.
[[nodiscard]] bool http_field_sensitive(cont::ConstSpan<u8> name) noexcept;

struct HttpStaticField
{
    cont::ConstSpan<u8> name;
    cont::ConstSpan<u8> value;
};

struct HttpStaticMatch
{
    i32 index = -1; // of the field if `exact`, else of the first entry with its name; -1 for neither
    bool exact = false;
};

[[nodiscard]] HttpStaticMatch http_find_static(cont::ConstSpan<HttpStaticField> table, cont::ConstSpan<u8> name,
                                               cont::ConstSpan<u8> value) noexcept;

// The dynamic table HPACK and QPACK share the shape of: one ring of bytes and one of entries, of the size SETTINGS
// allowed, from the injected allocator at init(). Eviction moves only the head, so a new entry may name the entry it
// evicts: its bytes are copied in rising order, and a copy never overruns the bytes it has yet to read.
class HttpFieldTable
{
public:
    struct Entry
    {
        u32 at = 0U; // where the name starts in the byte ring, the value following it
        u32 name_bytes = 0U;
        u32 value_bytes = 0U;
    };

    [[nodiscard]] bool init(memory::IAllocator& allocator, u32 max_capacity) noexcept;
    void release(memory::IAllocator& allocator) noexcept;

    [[nodiscard]] u32 capacity() const noexcept { return m_capacity; }
    [[nodiscard]] u32 max_capacity() const noexcept { return m_max_capacity; }
    [[nodiscard]] u32 size() const noexcept { return m_size; } // RFC 9204 section 3.2.1's
    [[nodiscard]] u64 inserted() const noexcept { return m_inserted; }
    [[nodiscard]] u64 dropped() const noexcept { return m_dropped; }
    [[nodiscard]] bool live(u64 absolute) const noexcept { return absolute >= m_dropped && absolute < m_inserted; }
    // Of a live entry.
    [[nodiscard]] const Entry& entry(u64 absolute) const noexcept;

    // Lowers or raises the capacity (at most max_capacity()), evicting what no longer fits.
    void set_capacity(u32 capacity) noexcept;
    // Evicts the oldest entries until one of `entry_bytes` (at most the capacity) fits.
    void make_room(usize entry_bytes) noexcept;

    // Appends an entry after make_room(); `source` is a copy of an entry, possibly just evicted.
    void append(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value) noexcept;
    void append_named(Entry source, cont::ConstSpan<u8> value) noexcept;
    void append_copy(Entry source) noexcept;

    // The newest entry holding the field, or the name.
    [[nodiscard]] bool find_field(const HttpField& field, u64& absolute) const noexcept;
    [[nodiscard]] bool find_name(cont::ConstSpan<u8> name, u64& absolute) const noexcept;

    void copy_name(const Entry& entry, cont::Span<u8> out) const noexcept;
    void copy_value(const Entry& entry, cont::Span<u8> out) const noexcept;
    [[nodiscard]] bool name_is(const Entry& entry, cont::ConstSpan<u8> name) const noexcept;
    [[nodiscard]] bool value_is(const Entry& entry, cont::ConstSpan<u8> value) const noexcept;

private:
    void write(cont::ConstSpan<u8> bytes) noexcept;
    // Copies bytes of the ring to the tail in rising order.
    void write_within(u32 at, u32 bytes) noexcept;
    void read(u32 at, cont::Span<u8> out) const noexcept;
    [[nodiscard]] bool equals(u32 at, cont::ConstSpan<u8> bytes) const noexcept;
    void push(u32 at, u32 name_bytes, u32 value_bytes) noexcept;

    u8* m_bytes = nullptr;
    Entry* m_entries = nullptr;
    u32 m_max_capacity = 0U;
    u32 m_max_entries = 0U;
    u32 m_capacity = 0U;
    u32 m_size = 0U;
    u32 m_tail = 0U;
    u64 m_inserted = 0U;
    u64 m_dropped = 0U;
};

// The bytes an entry counts for: its text and kHttpFieldOverhead.
[[nodiscard]] u64 http_entry_size(const HttpFieldTable::Entry& entry) noexcept;

// Where a decoder puts a field section. Past its storage it goes on measuring, so the section is still validated to its
// end; with `spill` given (HPACK, where every line changes the table) the line being read continues there, reset by
// begin_line(), and `oversize` tells it could not hold even that.
struct HttpFieldOutput
{
    cont::Span<HttpField> fields;
    cont::Span<u8> text;
    cont::Span<u8> spill = {};
    usize count = 0U;
    usize used = 0U;
    usize spilled = 0U;
    bool overflow = false;
    bool oversize = false;

    void begin_line() noexcept
    {
        spilled = 0U;
        oversize = false;
    }

    [[nodiscard]] cont::Span<u8> reserve(usize bytes) noexcept;
    // The literal `coded`, in `text`. False for a Huffman code RFC 7541 section 5.2 refuses.
    [[nodiscard]] bool literal(cont::ConstSpan<u8> coded, bool huffman, cont::ConstSpan<u8>& out) noexcept;
    void table_name(const HttpFieldTable& table, const HttpFieldTable::Entry& entry,
                    cont::ConstSpan<u8>& name) noexcept;
    void table_value(const HttpFieldTable& table, const HttpFieldTable::Entry& entry,
                     cont::ConstSpan<u8>& value) noexcept;
    void emit(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value, bool never_index) noexcept;
};

} // namespace crd::net
