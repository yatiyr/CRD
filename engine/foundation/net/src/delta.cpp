// crd-net -- baseline deltas (NET.3; docs/design/network-foundation.md#baseline-deltas).

#include <crd/containers/checksum.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/delta.hpp>

#include <algorithm>
#include <cstring>
#include <new>

namespace crd::net
{
namespace wire
{
namespace
{
using Reader = cont::BitReader;
using Writer = cont::BitWriter;

constexpr u32 kExtensibleLengthGroup = 7U;
constexpr u64 kExtensibleLengthMax = 0xFFFFFFFFU;
constexpr usize kExtensibleLengthMaxBytes = 5U;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): only a macro can return from its caller
#define CRD_DELTA_TRY(expression)                                                                                      \
    if (const WireError delta_error = (expression); delta_error != WireError::Ok)                                      \
    {                                                                                                                  \
        return delta_error;                                                                                            \
    }

[[nodiscard]] WireError take(Reader& r, u32 bits, u64& value) noexcept
{
    return wire_error(bits == 0U || r.take(bits, value), WireError::Truncated);
}

[[nodiscard]] constexpr u64 mask_of(u32 width) noexcept
{
    return (width == 64U) ? ~u64{0U} : ((u64{1U} << width) - 1U);
}

// The difference of two codes of `width` bits, wrapped to that width and folded so a small step either way is a small
// number. Both directions are bijections on `width` bits.
[[nodiscard]] constexpr u64 fold_difference(u64 from, u64 to, u32 width) noexcept
{
    const u64 mask = mask_of(width);
    const u64 difference = (to - from) & mask;
    const bool negative = ((difference >> (width - 1U)) & 1U) != 0U;
    return ((difference << 1U) & mask) ^ (negative ? mask : u64{0U});
}

[[nodiscard]] constexpr u64 unfold_difference(u64 from, u64 folded, u32 width) noexcept
{
    const u64 mask = mask_of(width);
    const u64 difference = (folded >> 1U) ^ (((folded & 1U) != 0U) ? mask : u64{0U});
    return (from + difference) & mask;
}

[[nodiscard]] WireError take_aligned(Reader& r) noexcept
{
    return wire_error(r.align(), WireError::BadPadding);
}

struct BytesField
{
    u64 length = 0U;
    cont::ConstSpan<u8> bytes;
};

[[nodiscard]] WireError take_bytes_field(const DeltaField& field, Reader& r, BytesField& out) noexcept
{
    CRD_DELTA_TRY(take(r, field.width, out.length));
    CRD_DELTA_TRY(take_aligned(r));
    return wire_error(r.take_bytes(static_cast<usize>(field.low + out.length), out.bytes), WireError::Truncated);
}

void put_bytes_field(const DeltaField& field, const BytesField& value, Writer& w) noexcept
{
    if (field.width != 0U)
    {
        w.put(value.length, field.width);
    }
    w.align();
    w.put_bytes(value.bytes);
}

[[nodiscard]] WireError open_extensible(Reader& r, Reader& inner) noexcept
{
    CRD_DELTA_TRY(take_aligned(r));
    u64 length = 0U;
    CRD_DELTA_TRY(wire_take_varuint(r, kExtensibleLengthGroup, kExtensibleLengthMax, length));
    return wire_error(r.sub_bytes(static_cast<usize>(length), inner), WireError::Truncated);
}

// What an extensible struct's bytes need: a nested writer fills the room past the longest length, and once its size is
// known the bytes move back behind the length actually written, so nesting costs one move per level and no second pass.
class ExtensibleBody
{
public:
    explicit ExtensibleBody(Writer& w) noexcept : m_w(w)
    {
        w.align();
        m_room = w.remaining();
    }

    [[nodiscard]] cont::Span<u8> room() const noexcept
    {
        return (m_room.size() > kExtensibleLengthMaxBytes) ? m_room.subspan(kExtensibleLengthMaxBytes)
                                                           : cont::Span<u8>{};
    }

    [[nodiscard]] WireError close(Writer& inner) noexcept
    {
        const usize size = inner.finish();
        if (inner.overflowed())
        {
            return WireError::NoRoom;
        }
        // A body came from past the longest length, so what it holds fits behind the one written.
        wire_put_varuint(m_w, size, kExtensibleLengthGroup);
        if (size != 0U)
        {
            std::memmove(m_w.remaining().data(), m_room.data() + kExtensibleLengthMaxBytes, size);
        }
        m_w.claim(size);
        return WireError::Ok;
    }

private:
    Writer& m_w;
    cont::Span<u8> m_room;
};

// ---- Walking one encoding ----

// Reads a field's value from `r`, and with Write writes it again to `w` at its own position. A value's bits do not
// depend on where it stands, except that bytes align, which is why this does not just copy bits. An extensible struct
// keeps its bytes, known or not.
template <bool Write>
[[nodiscard]] WireError walk_value(const DeltaField& field, Reader& r, [[maybe_unused]] Writer* w, u16 minor) noexcept;

template <bool Write>
[[nodiscard]] WireError walk_field(const DeltaField& field, Reader& r, [[maybe_unused]] Writer* w, u16 minor) noexcept
{
    if (field.optional)
    {
        u64 present = 0U;
        CRD_DELTA_TRY(take(r, 1U, present));
        if constexpr (Write)
        {
            w->put(present, 1U);
        }
        if (present == 0U)
        {
            return WireError::Ok;
        }
    }
    return walk_value<Write>(field, r, w, minor);
}

template <bool Write>
[[nodiscard]] WireError walk_value(const DeltaField& field, Reader& r, [[maybe_unused]] Writer* w, u16 minor) noexcept
{
    switch (field.kind)
    {
        case DeltaKind::Bits:
        {
            u64 code = 0U;
            CRD_DELTA_TRY(take(r, field.width, code));
            if constexpr (Write)
            {
                w->put(code, field.width);
            }
            return WireError::Ok;
        }
        case DeltaKind::Varuint:
        case DeltaKind::Varsint:
        {
            u64 code = 0U;
            CRD_DELTA_TRY(wire_take_varuint(r, field.width, ~u64{0U}, code));
            if constexpr (Write)
            {
                wire_put_varuint(*w, code, field.width);
            }
            return WireError::Ok;
        }
        case DeltaKind::Bytes:
        {
            BytesField value;
            CRD_DELTA_TRY(take_bytes_field(field, r, value));
            if constexpr (Write)
            {
                put_bytes_field(field, value, *w);
            }
            return WireError::Ok;
        }
        case DeltaKind::Struct:
            for (u16 index = 0U; index < field.child_count; ++index)
            {
                CRD_DELTA_TRY(walk_field<Write>(field.children[index], r, w, minor));
            }
            return WireError::Ok;
        case DeltaKind::Extensible:
        {
            Reader inner;
            CRD_DELTA_TRY(open_extensible(r, inner));
            if constexpr (Write)
            {
                cont::ConstSpan<u8> bytes;
                const usize size = static_cast<usize>(inner.bits_left() / 8U);
                CRD_DELTA_TRY(wire_error(inner.take_bytes(size, bytes), WireError::Truncated));
                w->align();
                wire_put_varuint(*w, size, kExtensibleLengthGroup);
                w->put_bytes(bytes);
            }
            return WireError::Ok;
        }
        case DeltaKind::List:
        case DeltaKind::KeyedList:
        {
            u64 extra = 0U;
            CRD_DELTA_TRY(take(r, field.width, extra));
            if constexpr (Write)
            {
                if (field.width != 0U)
                {
                    w->put(extra, field.width);
                }
            }
            const u64 count = field.low + extra;
            for (u64 index = 0U; index < count; ++index)
            {
                CRD_DELTA_TRY(walk_value<Write>(field.children[0], r, w, minor));
            }
            return WireError::Ok;
        }
    }
    return WireError::OutOfRange;
}

[[nodiscard]] WireError skip_value(const DeltaField& field, Reader& r, u16 minor) noexcept
{
    return walk_value<false>(field, r, nullptr, minor);
}

// A keyed list's element's key, without moving `r`. The generator holds the key to a plain uint, range or varuint at
// the head of a plain struct.
[[nodiscard]] WireError peek_key(const DeltaField& element, Reader r, u64& key) noexcept
{
    const DeltaField& field = element.children[0];
    if (field.kind == DeltaKind::Bits)
    {
        return take(r, field.width, key);
    }
    return wire_take_varuint(r, field.width, ~u64{0U}, key);
}

// ---- Comparing two encodings ----

// Whether a value is the same in both; on `same == true` both readers stand past it, otherwise they stand anywhere.
[[nodiscard]] WireError compare_value(const DeltaField& field, Reader& a, Reader& b, u16 minor, bool& same) noexcept;

[[nodiscard]] WireError compare_field(const DeltaField& field, Reader& a, Reader& b, u16 minor, bool& same) noexcept
{
    if (field.optional)
    {
        u64 one = 0U;
        u64 other = 0U;
        CRD_DELTA_TRY(take(a, 1U, one));
        CRD_DELTA_TRY(take(b, 1U, other));
        same = one == other;
        if (!same || one == 0U)
        {
            return WireError::Ok;
        }
    }
    return compare_value(field, a, b, minor, same);
}

[[nodiscard]] WireError compare_value(const DeltaField& field, Reader& a, Reader& b, u16 minor, bool& same) noexcept
{
    same = false;
    switch (field.kind)
    {
        case DeltaKind::Bits:
        {
            u64 one = 0U;
            u64 other = 0U;
            CRD_DELTA_TRY(take(a, field.width, one));
            CRD_DELTA_TRY(take(b, field.width, other));
            same = one == other;
            return WireError::Ok;
        }
        case DeltaKind::Varuint:
        case DeltaKind::Varsint:
        {
            u64 one = 0U;
            u64 other = 0U;
            CRD_DELTA_TRY(wire_take_varuint(a, field.width, ~u64{0U}, one));
            CRD_DELTA_TRY(wire_take_varuint(b, field.width, ~u64{0U}, other));
            same = one == other;
            return WireError::Ok;
        }
        case DeltaKind::Bytes:
        {
            BytesField one;
            BytesField other;
            CRD_DELTA_TRY(take_bytes_field(field, a, one));
            CRD_DELTA_TRY(take_bytes_field(field, b, other));
            same = wire_same_bytes(one.bytes, other.bytes);
            return WireError::Ok;
        }
        case DeltaKind::Struct:
            for (u16 index = 0U; index < field.child_count; ++index)
            {
                CRD_DELTA_TRY(compare_field(field.children[index], a, b, minor, same));
                if (!same)
                {
                    return WireError::Ok;
                }
            }
            same = true;
            return WireError::Ok;
        case DeltaKind::Extensible:
        {
            Reader one;
            Reader other;
            CRD_DELTA_TRY(open_extensible(a, one));
            CRD_DELTA_TRY(open_extensible(b, other));
            cont::ConstSpan<u8> left;
            cont::ConstSpan<u8> right;
            CRD_DELTA_TRY(
                wire_error(one.take_bytes(static_cast<usize>(one.bits_left() / 8U), left), WireError::Truncated));
            CRD_DELTA_TRY(
                wire_error(other.take_bytes(static_cast<usize>(other.bits_left() / 8U), right), WireError::Truncated));
            same = wire_same_bytes(left, right);
            return WireError::Ok;
        }
        case DeltaKind::List:
        case DeltaKind::KeyedList:
        {
            u64 one = 0U;
            u64 other = 0U;
            CRD_DELTA_TRY(take(a, field.width, one));
            CRD_DELTA_TRY(take(b, field.width, other));
            if (one != other)
            {
                return WireError::Ok;
            }
            const u64 count = field.low + one;
            for (u64 index = 0U; index < count; ++index)
            {
                CRD_DELTA_TRY(compare_value(field.children[0], a, b, minor, same));
                if (!same)
                {
                    return WireError::Ok;
                }
            }
            same = true;
            return WireError::Ok;
        }
    }
    return WireError::OutOfRange;
}

// ---- Writing a delta ----

[[nodiscard]] WireError encode_value(const DeltaField& field, Reader& b, Reader& c, Writer& w, u16 minor,
                                     u16 schema_minor) noexcept;

[[nodiscard]] WireError encode_field(const DeltaField& field, Reader& b, Reader& c, Writer& w, u16 minor,
                                     u16 schema_minor) noexcept
{
    if (field.since > minor)
    {
        return WireError::Ok;
    }
    Reader b_after = b;
    Reader c_after = c;
    bool same = false;
    CRD_DELTA_TRY(compare_field(field, b_after, c_after, minor, same));
    if (same)
    {
        w.put(0U, 1U);
        b = b_after;
        c = c_after;
        return WireError::Ok;
    }
    w.put(1U, 1U);
    if (field.optional)
    {
        u64 was = 0U;
        u64 is = 0U;
        CRD_DELTA_TRY(take(b, 1U, was));
        CRD_DELTA_TRY(take(c, 1U, is));
        w.put(is, 1U);
        if (is == 0U)
        {
            return skip_value(field, b, minor);
        }
        if (was == 0U)
        {
            return walk_value<true>(field, c, &w, minor);
        }
    }
    return encode_value(field, b, c, w, minor, schema_minor);
}

[[nodiscard]] WireError encode_keyed_list(const DeltaField& field, u64 old_count, u64 new_count, Reader& b, Reader& c,
                                          Writer& w, u16 minor, u16 schema_minor) noexcept
{
    const DeltaField& element = field.children[0];
    u64 old_at = 0U;
    u64 new_at = 0U;
    while (old_at < old_count && new_at < new_count)
    {
        u64 old_key = 0U;
        u64 new_key = 0U;
        CRD_DELTA_TRY(peek_key(element, b, old_key));
        CRD_DELTA_TRY(peek_key(element, c, new_key));
        if (new_key < old_key)
        {
            w.put(0b110U, 3U);
            CRD_DELTA_TRY(walk_value<true>(element, c, &w, minor));
            ++new_at;
        }
        else if (new_key > old_key)
        {
            w.put(0b111U, 3U);
            CRD_DELTA_TRY(skip_value(element, b, minor));
            ++old_at;
        }
        else
        {
            Reader b_after = b;
            Reader c_after = c;
            bool same = false;
            CRD_DELTA_TRY(compare_value(element, b_after, c_after, minor, same));
            if (same)
            {
                w.put(0U, 1U);
                b = b_after;
                c = c_after;
            }
            else
            {
                w.put(0b10U, 2U);
                CRD_DELTA_TRY(skip_value(element.children[0], b, minor));
                CRD_DELTA_TRY(skip_value(element.children[0], c, minor));
                for (u16 index = 1U; index < element.child_count; ++index)
                {
                    CRD_DELTA_TRY(encode_field(element.children[index], b, c, w, minor, schema_minor));
                }
            }
            ++old_at;
            ++new_at;
        }
    }
    // Once either side has run out, what is left needs no operation.
    for (; new_at < new_count; ++new_at)
    {
        CRD_DELTA_TRY(walk_value<true>(element, c, &w, minor));
    }
    for (; old_at < old_count; ++old_at)
    {
        CRD_DELTA_TRY(skip_value(element, b, minor));
    }
    return WireError::Ok;
}

[[nodiscard]] WireError encode_value(const DeltaField& field, Reader& b, Reader& c, Writer& w, u16 minor,
                                     u16 schema_minor) noexcept
{
    switch (field.kind)
    {
        case DeltaKind::Bits:
        {
            u64 was = 0U;
            u64 is = 0U;
            CRD_DELTA_TRY(take(b, field.width, was));
            CRD_DELTA_TRY(take(c, field.width, is));
            if (field.diff != 0U)
            {
                wire_put_varuint(w, fold_difference(was, is, field.width), field.diff);
            }
            else if (field.width != 1U) // a changed bit is its own news
            {
                w.put(is, field.width);
            }
            return WireError::Ok;
        }
        case DeltaKind::Varuint:
        case DeltaKind::Varsint:
        {
            u64 was = 0U;
            u64 is = 0U;
            CRD_DELTA_TRY(wire_take_varuint(b, field.width, ~u64{0U}, was));
            CRD_DELTA_TRY(wire_take_varuint(c, field.width, ~u64{0U}, is));
            if (field.diff == 0U)
            {
                wire_put_varuint(w, is, field.width);
            }
            else if (field.kind == DeltaKind::Varuint)
            {
                wire_put_varuint(w, fold_difference(was, is, 64U), field.diff);
            }
            else
            {
                wire_put_varuint(
                    w, fold_difference(static_cast<u64>(wire_unzigzag(was)), static_cast<u64>(wire_unzigzag(is)), 64U),
                    field.diff);
            }
            return WireError::Ok;
        }
        case DeltaKind::Bytes:
            CRD_DELTA_TRY(skip_value(field, b, minor));
            return walk_value<true>(field, c, &w, minor);
        case DeltaKind::Struct:
            for (u16 index = 0U; index < field.child_count; ++index)
            {
                CRD_DELTA_TRY(encode_field(field.children[index], b, c, w, minor, schema_minor));
            }
            return WireError::Ok;
        case DeltaKind::Extensible:
        {
            Reader old_body;
            Reader new_body;
            CRD_DELTA_TRY(open_extensible(b, old_body));
            CRD_DELTA_TRY(open_extensible(c, new_body));
            ExtensibleBody body(w);
            Writer inner(body.room());
            for (u16 index = 0U; index < field.child_count; ++index)
            {
                CRD_DELTA_TRY(encode_field(field.children[index], old_body, new_body, inner, minor, schema_minor));
            }
            return body.close(inner);
        }
        case DeltaKind::List:
        case DeltaKind::KeyedList:
        {
            u64 old_extra = 0U;
            u64 new_extra = 0U;
            CRD_DELTA_TRY(take(b, field.width, old_extra));
            CRD_DELTA_TRY(take(c, field.width, new_extra));
            if (field.width != 0U)
            {
                w.put(new_extra, field.width);
            }
            const u64 old_count = field.low + old_extra;
            const u64 new_count = field.low + new_extra;
            if (field.kind == DeltaKind::KeyedList)
            {
                return encode_keyed_list(field, old_count, new_count, b, c, w, minor, schema_minor);
            }
            const DeltaField& element = field.children[0];
            const u64 shared = std::min(old_count, new_count);
            for (u64 index = 0U; index < shared; ++index)
            {
                CRD_DELTA_TRY(encode_field(element, b, c, w, minor, schema_minor));
            }
            for (u64 index = shared; index < new_count; ++index)
            {
                CRD_DELTA_TRY(walk_value<true>(element, c, &w, minor));
            }
            for (u64 index = shared; index < old_count; ++index)
            {
                CRD_DELTA_TRY(skip_value(element, b, minor));
            }
            return WireError::Ok;
        }
    }
    return WireError::OutOfRange;
}

// ---- Rebuilding from a delta ----

[[nodiscard]] WireError apply_value(const DeltaField& field, Reader& b, Reader& d, Writer& w, u16 minor,
                                    u16 schema_minor) noexcept;

[[nodiscard]] WireError apply_field(const DeltaField& field, Reader& b, Reader& d, Writer& w, u16 minor,
                                    u16 schema_minor, bool& any) noexcept
{
    if (field.since > minor)
    {
        return WireError::Ok;
    }
    u64 changed = 0U;
    CRD_DELTA_TRY(take(d, 1U, changed));
    if (changed == 0U)
    {
        return walk_field<true>(field, b, &w, minor);
    }
    any = true;
    if (field.optional)
    {
        u64 was = 0U;
        u64 is = 0U;
        CRD_DELTA_TRY(take(b, 1U, was));
        CRD_DELTA_TRY(take(d, 1U, is));
        w.put(is, 1U);
        if (is == 0U)
        {
            CRD_DELTA_TRY(wire_error(was != 0U, WireError::Unchanged));
            return skip_value(field, b, minor);
        }
        if (was == 0U)
        {
            return walk_value<true>(field, d, &w, minor);
        }
    }
    return apply_value(field, b, d, w, minor, schema_minor);
}

// The rule the writer's merge follows: an insertion stands before the next old key and above everything already passed,
// so a removal followed by an insertion of the same key, which says nothing new, has no encoding.
[[nodiscard]] WireError apply_keyed_list(const DeltaField& field, u64 old_count, u64 new_count, Reader& b, Reader& d,
                                         Writer& w, u16 minor, u16 schema_minor, bool& any) noexcept
{
    const DeltaField& element = field.children[0];
    u64 old_at = 0U;
    u64 new_at = 0U;
    u64 floor = 0U;
    bool passed = false;
    const auto pass = [&](u64 key) noexcept
    {
        floor = key;
        passed = true;
    };
    const auto insert = [&](u64 limit, bool limited) noexcept
    {
        u64 key = 0U;
        CRD_DELTA_TRY(peek_key(element, d, key));
        CRD_DELTA_TRY(wire_error(!passed || key > floor, WireError::KeyOrder));
        CRD_DELTA_TRY(wire_error(!limited || key < limit, WireError::KeyOrder));
        CRD_DELTA_TRY(walk_value<true>(element, d, &w, minor));
        pass(key);
        ++new_at;
        any = true;
        return WireError::Ok;
    };
    const auto remove = [&](u64 key) noexcept
    {
        CRD_DELTA_TRY(skip_value(element, b, minor));
        pass(key);
        ++old_at;
        any = true;
        return WireError::Ok;
    };
    while (old_at < old_count || new_at < new_count)
    {
        if (old_at == old_count)
        {
            CRD_DELTA_TRY(insert(0U, false));
            continue;
        }
        u64 old_key = 0U;
        CRD_DELTA_TRY(peek_key(element, b, old_key));
        if (new_at == new_count)
        {
            CRD_DELTA_TRY(remove(old_key));
            continue;
        }
        u64 operation = 0U;
        CRD_DELTA_TRY(take(d, 1U, operation));
        if (operation == 0U)
        {
            CRD_DELTA_TRY(walk_value<true>(element, b, &w, minor));
        }
        else
        {
            CRD_DELTA_TRY(take(d, 1U, operation));
            if (operation == 0U)
            {
                CRD_DELTA_TRY(walk_value<true>(element.children[0], b, &w, minor));
                bool changed = false;
                for (u16 index = 1U; index < element.child_count; ++index)
                {
                    CRD_DELTA_TRY(apply_field(element.children[index], b, d, w, minor, schema_minor, changed));
                }
                CRD_DELTA_TRY(wire_error(changed, WireError::Unchanged));
                any = true;
            }
            else
            {
                CRD_DELTA_TRY(take(d, 1U, operation));
                CRD_DELTA_TRY((operation == 0U) ? insert(old_key, true) : remove(old_key));
                continue;
            }
        }
        pass(old_key);
        ++old_at;
        ++new_at;
    }
    return WireError::Ok;
}

[[nodiscard]] WireError apply_value(const DeltaField& field, Reader& b, Reader& d, Writer& w, u16 minor,
                                    u16 schema_minor) noexcept
{
    switch (field.kind)
    {
        case DeltaKind::Bits:
        {
            u64 was = 0U;
            CRD_DELTA_TRY(take(b, field.width, was));
            u64 is = was ^ 1U;
            if (field.diff != 0U)
            {
                u64 folded = 0U;
                CRD_DELTA_TRY(wire_take_varuint(d, field.diff, mask_of(field.width), folded));
                is = unfold_difference(was, folded, field.width);
            }
            else if (field.width != 1U)
            {
                CRD_DELTA_TRY(take(d, field.width, is));
            }
            CRD_DELTA_TRY(wire_error(is != was, WireError::Unchanged));
            w.put(is, field.width);
            return WireError::Ok;
        }
        case DeltaKind::Varuint:
        case DeltaKind::Varsint:
        {
            u64 was = 0U;
            CRD_DELTA_TRY(wire_take_varuint(b, field.width, ~u64{0U}, was));
            u64 is = 0U;
            if (field.diff == 0U)
            {
                CRD_DELTA_TRY(wire_take_varuint(d, field.width, ~u64{0U}, is));
            }
            else
            {
                u64 folded = 0U;
                CRD_DELTA_TRY(wire_take_varuint(d, field.diff, ~u64{0U}, folded));
                is = (field.kind == DeltaKind::Varuint)
                         ? unfold_difference(was, folded, 64U)
                         : wire_zigzag(
                               static_cast<i64>(unfold_difference(static_cast<u64>(wire_unzigzag(was)), folded, 64U)));
            }
            CRD_DELTA_TRY(wire_error(is != was, WireError::Unchanged));
            wire_put_varuint(w, is, field.width);
            return WireError::Ok;
        }
        case DeltaKind::Bytes:
        {
            BytesField was;
            BytesField is;
            CRD_DELTA_TRY(take_bytes_field(field, b, was));
            CRD_DELTA_TRY(take_bytes_field(field, d, is));
            CRD_DELTA_TRY(wire_error(!wire_same_bytes(was.bytes, is.bytes), WireError::Unchanged));
            put_bytes_field(field, is, w);
            return WireError::Ok;
        }
        case DeltaKind::Struct:
        {
            bool any = false;
            for (u16 index = 0U; index < field.child_count; ++index)
            {
                CRD_DELTA_TRY(apply_field(field.children[index], b, d, w, minor, schema_minor, any));
            }
            return wire_error(any, WireError::Unchanged);
        }
        case DeltaKind::Extensible:
        {
            Reader old_body;
            Reader changes;
            CRD_DELTA_TRY(open_extensible(b, old_body));
            CRD_DELTA_TRY(open_extensible(d, changes));
            ExtensibleBody body(w);
            Writer inner(body.room());
            bool any = false;
            for (u16 index = 0U; index < field.child_count; ++index)
            {
                CRD_DELTA_TRY(apply_field(field.children[index], old_body, changes, inner, minor, schema_minor, any));
            }
            CRD_DELTA_TRY(wire_finish(changes, minor, schema_minor));
            // What changed may lie in fields a newer writer added, which this reader cannot see.
            CRD_DELTA_TRY(wire_error(any || minor > schema_minor, WireError::Unchanged));
            return body.close(inner);
        }
        case DeltaKind::List:
        case DeltaKind::KeyedList:
        {
            u64 old_extra = 0U;
            u64 new_extra = 0U;
            CRD_DELTA_TRY(take(b, field.width, old_extra));
            CRD_DELTA_TRY(take(d, field.width, new_extra));
            if (field.width != 0U)
            {
                w.put(new_extra, field.width);
            }
            const u64 old_count = field.low + old_extra;
            const u64 new_count = field.low + new_extra;
            bool any = old_count != new_count;
            if (field.kind == DeltaKind::KeyedList)
            {
                CRD_DELTA_TRY(apply_keyed_list(field, old_count, new_count, b, d, w, minor, schema_minor, any));
                return wire_error(any, WireError::Unchanged);
            }
            const DeltaField& element = field.children[0];
            const u64 shared = std::min(old_count, new_count);
            for (u64 index = 0U; index < shared; ++index)
            {
                CRD_DELTA_TRY(apply_field(element, b, d, w, minor, schema_minor, any));
            }
            for (u64 index = shared; index < new_count; ++index)
            {
                CRD_DELTA_TRY(walk_value<true>(element, d, &w, minor));
            }
            for (u64 index = shared; index < old_count; ++index)
            {
                CRD_DELTA_TRY(skip_value(element, b, minor));
            }
            return wire_error(any, WireError::Unchanged);
        }
    }
    return WireError::OutOfRange;
}

[[nodiscard]] WireError finish_writer(Writer& w, usize& written) noexcept
{
    const usize size = w.finish();
    if (w.overflowed())
    {
        return WireError::NoRoom;
    }
    written = size;
    return WireError::Ok;
}

} // namespace

WireError delta_encode(const DeltaLayout& layout, cont::ConstSpan<u8> baseline, cont::ConstSpan<u8> current, u16 minor,
                       cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    if (minor > layout.schema_minor)
    {
        return WireError::BadRevision;
    }
    Reader b(baseline);
    Reader c(current);
    Writer w(out);
    for (u16 index = 0U; index < layout.field_count; ++index)
    {
        CRD_DELTA_TRY(encode_field(layout.fields[index], b, c, w, minor, layout.schema_minor));
    }
    return finish_writer(w, written);
}

WireError delta_apply(const DeltaLayout& layout, cont::ConstSpan<u8> baseline, cont::ConstSpan<u8> delta,
                      u16 writer_minor, cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    Reader b(baseline);
    Reader d(delta);
    Writer w(out);
    bool any = false;
    for (u16 index = 0U; index < layout.field_count; ++index)
    {
        CRD_DELTA_TRY(apply_field(layout.fields[index], b, d, w, writer_minor, layout.schema_minor, any));
    }
    CRD_DELTA_TRY(wire_finish(d, writer_minor, layout.schema_minor));
    return finish_writer(w, written);
}

#undef CRD_DELTA_TRY

} // namespace wire

// ---- The retained messages ----

BaselineStore::BaselineStore(memory::IAllocator& allocator, u32 slots, u32 capacity_bytes) noexcept
    : m_allocator(allocator), m_slots(slots), m_capacity(capacity_bytes)
{
    if (slots == 0U || capacity_bytes == 0U)
    {
        return;
    }
    void* const entries = allocator.try_allocate(sizeof(Entry) * slots, alignof(Entry));
    m_bytes = static_cast<u8*>(allocator.try_allocate(capacity_bytes, 1U));
    if (entries == nullptr || m_bytes == nullptr)
    {
        if (entries != nullptr)
        {
            allocator.deallocate(entries);
        }
        if (m_bytes != nullptr)
        {
            allocator.deallocate(m_bytes);
            m_bytes = nullptr;
        }
        return;
    }
    m_entries = static_cast<Entry*>(entries);
    for (u32 index = 0U; index < slots; ++index)
    {
        new (&m_entries[index]) Entry{};
    }
}

BaselineStore::~BaselineStore()
{
    if (m_entries != nullptr)
    {
        m_allocator.deallocate(m_entries);
        m_allocator.deallocate(m_bytes);
    }
}

bool BaselineStore::put(u64 sequence, cont::ConstSpan<u8> bytes) noexcept
{
    if (m_entries == nullptr || bytes.size() > m_capacity || (m_any && sequence <= m_newest))
    {
        return false;
    }
    const auto size = static_cast<u32>(bytes.size());
    if (size > m_capacity - m_head)
    {
        m_head = 0U;
    }
    const u32 end = m_head + size;
    for (u32 index = 0U; index < m_slots; ++index)
    {
        Entry& entry = m_entries[index];
        if (entry.used && entry.offset < end && m_head < entry.offset + entry.size)
        {
            entry.used = false;
        }
    }
    Entry& entry = m_entries[sequence % m_slots];
    entry = Entry{sequence, m_head, size, true};
    if (size != 0U)
    {
        std::memcpy(m_bytes + m_head, bytes.data(), size);
    }
    m_head = end;
    m_newest = sequence;
    m_any = true;
    return true;
}

bool BaselineStore::find(u64 sequence, cont::ConstSpan<u8>& bytes) const noexcept
{
    if (m_entries == nullptr)
    {
        return false;
    }
    const Entry& entry = m_entries[sequence % m_slots];
    if (!entry.used || entry.sequence != sequence)
    {
        return false;
    }
    bytes = cont::ConstSpan<u8>(m_bytes + entry.offset, entry.size);
    return true;
}

// ---- The frame body ----

namespace
{
constexpr u32 kDeltaSequenceBits = 16U;
constexpr u64 kDeltaSequenceSpan = u64{1U} << kDeltaSequenceBits;
constexpr u32 kDeltaDistanceGroup = 7U;
constexpr u32 kDeltaHashBits = 32U;

[[nodiscard]] u32 message_hash(cont::ConstSpan<u8> bytes) noexcept
{
    return static_cast<u32>(cont::xxh3_64(bytes));
}

} // namespace

u64 delta_expand_sequence(u64 expected, u16 low) noexcept
{
    const u64 half = kDeltaSequenceSpan / 2U;
    const u64 candidate = (expected & ~(kDeltaSequenceSpan - 1U)) | low;
    if (candidate <= expected && expected - candidate >= half && candidate <= ~u64{0U} - kDeltaSequenceSpan)
    {
        return candidate + kDeltaSequenceSpan;
    }
    if (candidate > expected && candidate - expected > half && candidate >= kDeltaSequenceSpan)
    {
        return candidate - kDeltaSequenceSpan;
    }
    return candidate;
}

wire::WireError DeltaWriter::write(u64 sequence, u16 minor, cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    cont::ConstSpan<u8> current;
    if ((m_wrote && sequence <= m_last) || !m_store.find(sequence, current))
    {
        return wire::WireError::OutOfRange;
    }
    if (out.size() < kDeltaHeaderMaxBytes)
    {
        return wire::WireError::NoRoom;
    }
    const u32 hash = message_hash(current);
    const auto put_header = [&](u64 distance) noexcept
    {
        cont::BitWriter w(out);
        w.put(sequence & (kDeltaSequenceSpan - 1U), kDeltaSequenceBits);
        wire::wire_put_varuint(w, distance, kDeltaDistanceGroup);
        w.put(hash, kDeltaHashBits);
        return w.finish();
    };

    cont::ConstSpan<u8> baseline;
    if (m_has_ack && sequence - m_acked < kDeltaSequenceSpan && m_store.find(m_acked, baseline))
    {
        const u64 distance = sequence - m_acked;
        const usize header = put_header(distance);
        // Past the message's size plus the room the rebuild needs, the delta has lost: stop there.
        const cont::Span<u8> room = out.subspan(header);
        const usize limit = std::min(room.size(), current.size() + wire::kDeltaSlackBytes);
        usize size = 0U;
        const wire::WireError error = wire::delta_encode(m_layout, baseline, current, minor, room.first(limit), size);
        if (error == wire::WireError::Ok && size < current.size())
        {
            written = header + size;
            m_last = sequence;
            m_wrote = true;
            ++m_deltas;
            return wire::WireError::Ok;
        }
        if (error != wire::WireError::Ok && error != wire::WireError::NoRoom)
        {
            return error;
        }
    }

    const usize header = put_header(0U);
    if (out.size() - header < current.size())
    {
        return wire::WireError::NoRoom;
    }
    if (!current.empty())
    {
        std::memcpy(out.data() + header, current.data(), current.size());
    }
    written = header + current.size();
    m_last = sequence;
    m_wrote = true;
    ++m_whole;
    return wire::WireError::Ok;
}

void DeltaWriter::on_ack(u16 sequence) noexcept
{
    if (!m_wrote)
    {
        return;
    }
    const u64 expanded = delta_expand_sequence(m_last, sequence);
    if (expanded > m_last || (m_has_ack && expanded <= m_acked))
    {
        return;
    }
    m_acked = expanded;
    m_has_ack = true;
}

DeltaReader::DeltaReader(const wire::DeltaLayout& layout, BaselineStore& store, memory::IAllocator& allocator) noexcept
    : m_layout(layout), m_store(store), m_allocator(allocator)
{
    void* const scratch = allocator.try_allocate(layout.max_bytes + wire::kDeltaSlackBytes, 1U);
    if (scratch != nullptr)
    {
        m_scratch = static_cast<u8*>(scratch);
    }
}

DeltaReader::~DeltaReader()
{
    if (m_scratch != nullptr)
    {
        m_allocator.deallocate(m_scratch);
    }
}

DeltaRead DeltaReader::read(cont::ConstSpan<u8> body, u16 writer_minor, cont::ConstSpan<u8>& message,
                            wire::WireError& error) noexcept
{
    error = wire::WireError::Ok;
    const DeltaRead verdict = read_body(body, writer_minor, message, error);
    ++m_counts[static_cast<usize>(verdict)];
    return verdict;
}

DeltaRead DeltaReader::read_body(cont::ConstSpan<u8> body, u16 writer_minor, cont::ConstSpan<u8>& message,
                                 wire::WireError& error) noexcept
{
    cont::BitReader r(body);
    u64 low = 0U;
    u64 distance = 0U;
    u64 hash = 0U;
    if (!r.take(kDeltaSequenceBits, low))
    {
        error = wire::WireError::Truncated;
        return DeltaRead::Malformed;
    }
    error = wire::wire_take_varuint(r, kDeltaDistanceGroup, kDeltaSequenceSpan - 1U, distance);
    if (error != wire::WireError::Ok)
    {
        return DeltaRead::Malformed;
    }
    if (!r.take(kDeltaHashBits, hash))
    {
        error = wire::WireError::Truncated;
        return DeltaRead::Malformed;
    }
    // The first sequence stands past one span, so no distance reaches below zero.
    const u64 sequence =
        m_any ? delta_expand_sequence(m_newest + 1U, static_cast<u16>(low)) : (kDeltaSequenceSpan + low);
    if (m_any && sequence <= m_newest)
    {
        return DeltaRead::Stale;
    }
    cont::ConstSpan<u8> payload;
    if (!r.take_bytes(static_cast<usize>(r.bits_left() / 8U), payload))
    {
        error = wire::WireError::Truncated;
        return DeltaRead::Malformed;
    }

    cont::ConstSpan<u8> whole = payload;
    if (distance != 0U)
    {
        cont::ConstSpan<u8> baseline;
        if (!m_store.find(sequence - distance, baseline))
        {
            return DeltaRead::MissingBaseline;
        }
        usize size = 0U;
        const usize room = m_layout.max_bytes + wire::kDeltaSlackBytes;
        error = wire::delta_apply(m_layout, baseline, payload, writer_minor, cont::Span<u8>(m_scratch, room), size);
        if (error != wire::WireError::Ok)
        {
            return DeltaRead::Malformed;
        }
        whole = cont::ConstSpan<u8>(m_scratch, size);
    }
    error = m_layout.validate(whole, writer_minor);
    if (error != wire::WireError::Ok)
    {
        return DeltaRead::Malformed;
    }
    if ((distance == 0U || writer_minor <= m_layout.schema_minor) && message_hash(whole) != static_cast<u32>(hash))
    {
        return DeltaRead::Mismatch;
    }
    if (!m_store.put(sequence, whole) || !m_store.find(sequence, message))
    {
        return DeltaRead::Full;
    }
    m_newest = sequence;
    m_any = true;
    return DeltaRead::Delivered;
}

} // namespace crd::net
