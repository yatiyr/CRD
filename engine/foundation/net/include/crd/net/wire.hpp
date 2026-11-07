#pragma once

// crd-net -- what every generated wire format shares (NET.3; contract: docs/design/network-foundation.md#wire-formats):
// the refusals, the descriptions a schema generates for admission, the field codecs generated code calls, the list view
// and the top-level encode and decode. scripts/gen-wire-schemas.py writes the rest, one header per authored schema.
//
// No function here allocates. A decode validates the whole message once, in time linear in its bytes, and then views the
// input: byte and text fields are spans into it, and a list re-reads its elements from it when walked.

#include <crd/containers/span.hpp>
#include <crd/containers/wire_bits.hpp>
#include <crd/core/types.hpp>
#include <crd/core/utf8.hpp>

#include <bit>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace crd::net::wire
{
namespace cont = crd::containers;

enum class WireError : u8
{
    Ok,
    Truncated,     // a field past the end of the message
    BadPadding,    // a padding bit is set
    TrailingBits,  // whole bytes after the last field of a message of a revision this reader knows
    OutOfRange,    // a value outside its declared bounds: a count, a length, a range
    Overlong,      // a variable-length integer longer than its shortest form
    NonFinite,     // a NaN or an infinity in a float field that does not declare them
    BadText,       // a text field that is not well-formed UTF-8
    UnknownValue,  // an enum value the writer's revision does not have
    BadQuaternion, // a quantised quaternion word no encoder writes
    NoRoom,        // encoding: the output span is too small
    NotInRevision, // encoding: a field or value the requested minor revision does not have is set
    BadRevision,   // encoding: a minor revision newer than the schema's
    Unchanged,     // a baseline delta says a field changed that did not: a second encoding of one message
    KeyOrder,      // the keys of a keyed list do not ascend strictly
};

// The channel classes of the design's table.
enum class ChannelClass : u8
{
    Control,
    Input,
    State,
    Event,
    Transaction,
    Bulk,
    Media,
};

// What a channel declares in its authored schema; admission enforces it. A rate or byte budget of zero is no message
// and no byte: a channel states its budgets.
struct ChannelDesc
{
    std::string_view name;
    ChannelClass     channel_class{};
    bool             reliable          = false;
    bool             ordered           = false;
    bool             idempotent        = false; // replay semantics: a second delivery changes nothing
    u8               priority          = 0U;    // 0 sheds last
    u32              max_message_bytes = 0U;
    u32              messages_per_s    = 0U;
    u64              bytes_per_s       = 0U;
    u16              padding_block     = 0U; // its traffic pads to a multiple of this; zero pads nothing
    bool             integrity         = true; // its frames carry frame_hash(); only a Media channel declares otherwise
};

struct DeltaLayout;

// What admission reads of a message before any field of it is decoded.
struct MessageDesc
{
    std::string_view name;
    u32              id        = 0U;
    u16              channel   = 0U; // into WireSchema::channels
    u16              since     = 0U; // the minor revision that introduced it
    u64              operation = 0U; // its operation bit in the S18 schema; zero below S18 (a transport schema)
    u32              min_bytes = 0U; // of its frame's body, at the schema's first minor revision
    u32              max_bytes = 0U; // of its frame's body, at the schema's current minor revision
    const DeltaLayout* delta = nullptr; // a delta message: its frame's body is DeltaWriter's, not the encoding itself
};

// A schema as generated: the fields of the S18 artefact its issuer signs, its channels and its messages. `kind` is the
// value of crypto's ScopeKind, or zero for a transport schema, which runs below S18 and declares no operation.
struct WireSchema
{
    std::string_view          name;
    u8                        id[16] = {};
    u8                        kind   = 0U;
    u16                       major  = 0U;
    u16                       minor  = 0U;
    u64                       operations = 0U;
    u64                       idempotent = 0U;
    cont::ConstSpan<ChannelDesc> channels;
    cont::ConstSpan<MessageDesc> messages;
};

// A message's description by identifier: the generated tables ascend, so a claimed identifier costs a binary search.
[[nodiscard]] constexpr const MessageDesc* find_message(const WireSchema& schema, u32 id) noexcept
{
    usize low = 0U;
    usize high = schema.messages.size();
    while (low < high)
    {
        const usize middle = low + ((high - low) / 2U);
        const u32 found = schema.messages[middle].id;
        if (found == id)
        {
            return &schema.messages[middle];
        }
        if (found < id)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    return nullptr;
}

// ---- Scalars: every put takes a checked value, every take reports what refused it. ----

[[nodiscard]] constexpr WireError wire_error(bool ok, WireError error) noexcept
{
    return ok ? WireError::Ok : error;
}

[[nodiscard]] inline WireError wire_take_bool(cont::BitReader& r, bool& value) noexcept
{
    u64 bit = 0U;
    if (!r.take(1U, bit))
    {
        return WireError::Truncated;
    }
    value = bit != 0U;
    return WireError::Ok;
}

template <typename T>
[[nodiscard]] CRD_FORCEINLINE WireError wire_take_uint(cont::BitReader& r, u32 bits, T& value) noexcept
{
    u64 raw = 0U;
    if (!r.take(bits, raw))
    {
        return WireError::Truncated;
    }
    value = static_cast<T>(raw);
    return WireError::Ok;
}

inline constexpr i64 kWireMinI64 = -9223372036854775807LL - 1LL;

// An integer in [min, min + span], as its offset from min in `bits` bits.
template <typename T>
[[nodiscard]] inline bool wire_range_holds(T value, T min, u64 span) noexcept
{
    return value >= min && (static_cast<u64>(value) - static_cast<u64>(min)) <= span;
}

template <typename T>
inline void wire_put_range(cont::BitWriter& w, T value, T min, u32 bits) noexcept
{
    w.put(static_cast<u64>(value) - static_cast<u64>(min), bits);
}

template <typename T>
[[nodiscard]] inline WireError wire_take_range(cont::BitReader& r, T min, u64 span, u32 bits, T& value) noexcept
{
    u64 offset = 0U;
    if (!r.take(bits, offset))
    {
        return WireError::Truncated;
    }
    if (offset > span)
    {
        return WireError::OutOfRange;
    }
    value = static_cast<T>(static_cast<u64>(min) + offset);
    return WireError::Ok;
}

// A variable-length unsigned integer: groups of `group` bits, least significant first, each behind a bit that says
// another group follows. The last group is never zero unless it is the only one, so a value has one encoding.
inline void wire_put_varuint(cont::BitWriter& w, u64 value, u32 group) noexcept
{
    const u64 mask = (u64{1} << group) - 1U;
    while (value > mask)
    {
        w.put((u64{1} << group) | (value & mask), group + 1U);
        value >>= group;
    }
    w.put(value, group + 1U);
}

[[nodiscard]] constexpr u64 wire_varuint_bits(u64 value, u32 group) noexcept
{
    const auto width  = static_cast<u32>(std::bit_width(value));
    const u32  groups = (width == 0U) ? 1U : ((width + group - 1U) / group);
    return static_cast<u64>(groups) * (group + 1U);
}

[[nodiscard]] inline WireError wire_take_varuint(cont::BitReader& r, u32 group, u64 max, u64& value) noexcept
{
    const u32 limit = (max == 0U) ? 1U : ((static_cast<u32>(std::bit_width(max)) + group - 1U) / group);
    u64       out   = 0U;
    for (u32 index = 0U;; ++index)
    {
        u64 chunk = 0U;
        if (!r.take(group + 1U, chunk))
        {
            return WireError::Truncated;
        }
        const bool more = (chunk >> group) != 0U;
        chunk &= (u64{1} << group) - 1U;
        if (index >= limit)
        {
            return WireError::OutOfRange;
        }
        const u32 shift = index * group;
        if (shift != 0U && chunk > (~u64{0} >> shift))
        {
            return WireError::OutOfRange;
        }
        out |= chunk << shift;
        if (!more)
        {
            if (chunk == 0U && index != 0U)
            {
                return WireError::Overlong;
            }
            break;
        }
    }
    if (out > max)
    {
        return WireError::OutOfRange;
    }
    value = out;
    return WireError::Ok;
}

// A signed one maps by zigzag: 0, -1, 1, -2, ...
[[nodiscard]] constexpr u64 wire_zigzag(i64 value) noexcept
{
    return (static_cast<u64>(value) << 1U) ^ (0U - (static_cast<u64>(value) >> 63U));
}

[[nodiscard]] constexpr i64 wire_unzigzag(u64 value) noexcept
{
    return static_cast<i64>((value >> 1U) ^ (0U - (value & 1U)));
}

template <typename T>
[[nodiscard]] inline WireError wire_take_varuint_as(cont::BitReader& r, u32 group, u64 max, T& value) noexcept
{
    u64 raw = 0U;
    const WireError error = wire_take_varuint(r, group, max, raw);
    if (error == WireError::Ok)
    {
        value = static_cast<T>(raw);
    }
    return error;
}

template <typename T>
[[nodiscard]] inline WireError wire_take_varsint(cont::BitReader& r, u32 group, i64 min, i64 max, T& value) noexcept
{
    const u64 widest = wire_zigzag(min) > wire_zigzag(max) ? wire_zigzag(min) : wire_zigzag(max);
    u64       raw    = 0U;
    const WireError error = wire_take_varuint(r, group, widest, raw);
    if (error != WireError::Ok)
    {
        return error;
    }
    const i64 decoded = wire_unzigzag(raw);
    if (decoded < min || decoded > max)
    {
        return WireError::OutOfRange;
    }
    value = static_cast<T>(decoded);
    return WireError::Ok;
}

[[nodiscard]] constexpr bool wire_finite(f32 value) noexcept
{
    return (std::bit_cast<u32>(value) & 0x7F800000U) != 0x7F800000U;
}

[[nodiscard]] constexpr bool wire_finite(f64 value) noexcept
{
    return (std::bit_cast<u64>(value) & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

template <typename F>
inline void wire_put_float(cont::BitWriter& w, F value) noexcept
{
    if constexpr (sizeof(F) == sizeof(u32))
    {
        w.put(std::bit_cast<u32>(value), 32U);
    }
    else
    {
        w.put(std::bit_cast<u64>(value), 64U);
    }
}

template <typename F>
[[nodiscard]] inline WireError wire_take_float(cont::BitReader& r, bool finite_only, F& value) noexcept
{
    u64 raw = 0U;
    if (!r.take(static_cast<u32>(sizeof(F) * 8U), raw))
    {
        return WireError::Truncated;
    }
    if constexpr (sizeof(F) == sizeof(u32))
    {
        value = std::bit_cast<F>(static_cast<u32>(raw));
    }
    else
    {
        value = std::bit_cast<F>(raw);
    }
    return (finite_only && !wire_finite(value)) ? WireError::NonFinite : WireError::Ok;
}

// The same bits: a float's -0 is not its +0, and an equal message is one that encodes the same.
template <typename T>
[[nodiscard]] constexpr bool wire_same(T a, T b) noexcept
{
    if constexpr (std::is_same_v<T, f32>)
    {
        return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
    }
    else if constexpr (std::is_same_v<T, f64>)
    {
        return std::bit_cast<u64>(a) == std::bit_cast<u64>(b);
    }
    else
    {
        return a == b;
    }
}

// ---- Bytes and text: a length in the bits of its bound, padding to a byte boundary, the bytes viewed. ----

[[nodiscard]] inline bool wire_same_bytes(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

[[nodiscard]] inline cont::ConstSpan<u8> wire_text_bytes(std::string_view text) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

[[nodiscard]] constexpr bool wire_text_valid(cont::ConstSpan<u8> bytes) noexcept
{
    usize at = 0U;
    while (at < bytes.size())
    {
        const usize length = utf8_sequence_length(bytes.data() + at, bytes.size() - at);
        if (length == 0U)
        {
            return false;
        }
        at += length;
    }
    return true;
}

[[nodiscard]] constexpr u64 wire_aligned(u64 position) noexcept
{
    return (position + 7U) & ~u64{7};
}

// The position after `count` bytes with a length of `length_bits` bits (none for a fixed size) written at `position`.
[[nodiscard]] constexpr u64 wire_bytes_end(u64 position, u32 length_bits, usize count) noexcept
{
    return wire_aligned(position + length_bits) + (static_cast<u64>(count) * 8U);
}

inline void wire_put_bytes(cont::BitWriter& w, cont::ConstSpan<u8> bytes, usize min, u32 length_bits) noexcept
{
    if (length_bits != 0U)
    {
        w.put(bytes.size() - min, length_bits);
    }
    w.align();
    w.put_bytes(bytes);
}

[[nodiscard]] inline WireError wire_take_bytes(cont::BitReader& r, usize min, usize max, u32 length_bits,
                                               cont::ConstSpan<u8>& bytes) noexcept
{
    u64 length = 0U;
    if (length_bits != 0U && !r.take(length_bits, length))
    {
        return WireError::Truncated;
    }
    if (length > max - min)
    {
        return WireError::OutOfRange;
    }
    if (!r.align())
    {
        return WireError::BadPadding;
    }
    return wire_error(r.take_bytes(min + static_cast<usize>(length), bytes), WireError::Truncated);
}

[[nodiscard]] inline WireError wire_take_text(cont::BitReader& r, usize max, u32 length_bits,
                                              std::string_view& text) noexcept
{
    cont::ConstSpan<u8> bytes;
    const WireError     error = wire_take_bytes(r, 0U, max, length_bits, bytes);
    if (error != WireError::Ok)
    {
        return error;
    }
    if (!wire_text_valid(bytes))
    {
        return WireError::BadText;
    }
    text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return WireError::Ok;
}

// ---- Lists ----

// A list field. To encode, it holds the caller's elements; decoded, it holds where they lie in the message, already
// validated, and a walk re-reads them from there. `C` is the element's generated codec.
template <typename C>
class WireList
{
public:
    using Value = typename C::Value;

    WireList() noexcept = default;

    // To encode.
    WireList(cont::ConstSpan<Value> items) noexcept : m_items(items), m_count(items.size()) {}

    // As a decoder found them; `from` stands at the first element.
    WireList(cont::BitReader from, usize count, u16 writer_minor) noexcept
        : m_from(from), m_count(count), m_minor(writer_minor), m_view(true)
    {
    }

    [[nodiscard]] usize size() const noexcept
    {
        return m_count;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_count == 0U;
    }

    // The elements in order: next() writes the next one, or returns false after the last.
    class Cursor
    {
    public:
        explicit Cursor(const WireList& list) noexcept : m_list(&list), m_from(list.m_from) {}

        [[nodiscard]] bool next(Value& value) noexcept
        {
            if (m_index == m_list->m_count)
            {
                return false;
            }
            if (m_list->m_view)
            {
                [[maybe_unused]] const WireError error = C::decode(m_from, m_list->m_minor, value);
                CRD_ASSERT(error == WireError::Ok);
            }
            else
            {
                value = m_list->m_items[m_index];
            }
            ++m_index;
            return true;
        }

    private:
        const WireList* m_list;
        cont::BitReader m_from;
        usize           m_index = 0U;
    };

    [[nodiscard]] Cursor cursor() const noexcept
    {
        return Cursor(*this);
    }

private:
    cont::ConstSpan<Value> m_items;
    cont::BitReader        m_from;
    usize                  m_count = 0U;
    u16                    m_minor = 0U;
    bool                   m_view  = false;
};

template <typename C>
[[nodiscard]] inline bool wire_same_list(const WireList<C>& a, const WireList<C>& b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    auto           left  = a.cursor();
    auto           right = b.cursor();
    typename C::Value one{};
    typename C::Value other{};
    while (left.next(one) && right.next(other))
    {
        if (!C::same(one, other))
        {
            return false;
        }
    }
    return true;
}

// Reads a keyed list's key from an element.
template <auto Member>
struct WireKeyOf
{
    template <typename V>
    [[nodiscard]] constexpr auto operator()(const V& value) const noexcept
    {
        return value.*Member;
    }
};

// Whether a keyed list's keys ascend strictly; `key` reads one from an element.
template <typename C, typename K>
[[nodiscard]] inline WireError wire_keys_ascend(const WireList<C>& list, K key) noexcept
{
    auto              cursor = list.cursor();
    typename C::Value value{};
    bool              first = true;
    decltype(key(value)) previous{};
    while (cursor.next(value))
    {
        if (!first && !(previous < key(value)))
        {
            return WireError::KeyOrder;
        }
        previous = key(value);
        first = false;
    }
    return WireError::Ok;
}

template <typename C>
[[nodiscard]] inline WireError wire_put_list(cont::BitWriter& w, const WireList<C>& list, usize min, usize max,
                                             u32 count_bits, u16 minor) noexcept
{
    if (list.size() < min || list.size() > max)
    {
        return WireError::OutOfRange;
    }
    if (count_bits != 0U)
    {
        w.put(list.size() - min, count_bits);
    }
    auto              cursor = list.cursor();
    typename C::Value value{};
    while (cursor.next(value))
    {
        const WireError error = C::encode(value, w, minor);
        if (error != WireError::Ok)
        {
            return error;
        }
    }
    return WireError::Ok;
}

template <typename C>
[[nodiscard]] inline u64 wire_list_end(const WireList<C>& list, u32 count_bits, u16 minor, u64 position) noexcept
{
    position += count_bits;
    auto              cursor = list.cursor();
    typename C::Value value{};
    while (cursor.next(value))
    {
        position = C::end(value, minor, position);
    }
    return position;
}

// Validates every element and leaves the view on the list. No element takes zero bits (the generator refuses one), so
// the walk is linear in the bytes whatever count the message claims.
template <typename C>
[[nodiscard]] inline WireError wire_take_list(cont::BitReader& r, usize min, usize max, u32 count_bits,
                                              u16 writer_minor, WireList<C>& list) noexcept
{
    u64 extra = 0U;
    if (count_bits != 0U && !r.take(count_bits, extra))
    {
        return WireError::Truncated;
    }
    if (extra > max - min)
    {
        return WireError::OutOfRange;
    }
    const usize count = min + static_cast<usize>(extra);
    const cont::BitReader first = r;
    typename C::Value     value{};
    for (usize i = 0U; i < count; ++i)
    {
        const WireError error = C::decode(r, writer_minor, value);
        if (error != WireError::Ok)
        {
            return error;
        }
    }
    list = WireList<C>(first, count, writer_minor);
    return WireError::Ok;
}

// ---- The rule of a message's end, which an extensible struct's end shares ----

// After the last field a reader knows. A message of a newer minor revision may hold anything more. Any other holds only
// zero bits to its byte boundary, so one message has one encoding.
[[nodiscard]] inline WireError wire_finish(cont::BitReader& r, u16 writer_minor, u16 schema_minor) noexcept
{
    if (writer_minor > schema_minor)
    {
        return WireError::Ok;
    }
    if (!r.align())
    {
        return WireError::BadPadding;
    }
    return wire_error(r.bits_left() == 0U, WireError::TrailingBits);
}

// An extensible struct: aligned, its length in bytes as whole 7-bit groups, its body padded to a byte boundary. A reader
// of an older revision skips what it does not know, and the length of the body does not depend on the length's length.
template <typename C>
[[nodiscard]] inline WireError wire_put_extensible(cont::BitWriter& w, const typename C::Value& value, u16 minor) noexcept
{
    w.align();
    const u64 length = wire_aligned(C::body_end(value, minor, 0U)) / 8U;
    wire_put_varuint(w, length, 7U);
    const u64       start = w.bit_position();
    const WireError error = C::encode_body(value, w, minor);
    if (error != WireError::Ok)
    {
        return error;
    }
    w.align();
    return wire_error(w.bit_position() - start == length * 8U, WireError::OutOfRange);
}

template <typename C>
[[nodiscard]] inline u64 wire_extensible_end(const typename C::Value& value, u16 minor, u64 position) noexcept
{
    const u64 length = wire_aligned(C::body_end(value, minor, 0U)) / 8U;
    return wire_aligned(position) + wire_varuint_bits(length, 7U) + (length * 8U);
}

template <typename C>
[[nodiscard]] inline WireError wire_take_extensible(cont::BitReader& r, u16 writer_minor, u16 schema_minor,
                                                    typename C::Value& value) noexcept
{
    if (!r.align())
    {
        return WireError::BadPadding;
    }
    u64             length = 0U;
    const WireError error  = wire_take_varuint(r, 7U, 0xFFFFFFFFU, length);
    if (error != WireError::Ok)
    {
        return error;
    }
    cont::BitReader inner;
    if (!r.sub_bytes(static_cast<usize>(length), inner))
    {
        return WireError::Truncated;
    }
    const WireError body = C::decode_body(inner, writer_minor, value);
    return (body != WireError::Ok) ? body : wire_finish(inner, writer_minor, schema_minor);
}

// ---- Messages ----

// The bytes `message` takes at `minor`. It reads every list, so it is linear in the message.
template <typename M>
[[nodiscard]] inline usize encoded_bytes(const M& message, u16 minor) noexcept
{
    return static_cast<usize>(wire_aligned(M::WireCodec::end(message, minor, 0U)) / 8U);
}

// Writes `message` at minor revision `minor` of its schema. On any refusal `written` is zero and the span holds no
// meaning. A field the revision does not have and that holds its default is left out; one that does not is refused.
template <typename M>
[[nodiscard]] inline WireError encode(const M& message, u16 minor, cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    using C = typename M::WireCodec;
    if (minor > C::kSchemaMinor)
    {
        return WireError::BadRevision;
    }
    cont::BitWriter w(out);
    const WireError error = C::encode(message, w, minor);
    if (error != WireError::Ok)
    {
        return error;
    }
    const usize size = w.finish();
    if (w.overflowed())
    {
        return WireError::NoRoom;
    }
    written = size;
    return WireError::Ok;
}

// Reads a message written at `writer_minor`, which the session announced (a message carries no revision of its own). A
// field that revision does not have takes its default. On any refusal `message` holds no meaning. Its spans and lists
// view `bytes`, which must outlive them.
template <typename M>
[[nodiscard]] inline WireError decode(cont::ConstSpan<u8> bytes, u16 writer_minor, M& message) noexcept
{
    using C = typename M::WireCodec;
    cont::BitReader r(bytes);
    const WireError error = C::decode(r, writer_minor, message);
    return (error != WireError::Ok) ? error : wire_finish(r, writer_minor, C::kSchemaMinor);
}

// Whether two messages encode the same.
template <typename M>
[[nodiscard]] inline bool same(const M& a, const M& b) noexcept
{
    return M::WireCodec::same(a, b);
}

} // namespace crd::net::wire
