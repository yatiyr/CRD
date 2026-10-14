#pragma once

// DIAG.10a host-check finding, shared by the binary gate (test_binary.cpp) and the CEIR text and binary fuzz seeds: a
// type- or attribute-class name of Context::kMaxClassNameBytes bytes or more reached the fixed intern buffer, whose
// assertion ended the process. A loader must refuse such a name. The builder cannot make one (the intern call asserts),
// so the fixture builds the longest name the buffer takes and lengthens the printed text or the serialized blob by
// one byte. Host-only.

#include <crd/ceir/ceir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

#include <cstring>

namespace crd::ceir::test
{
inline constexpr const char* kLongClassDialect = "longclass";

// One op `test.x` whose `v` attribute names a type class (an Extern type) or an attribute class (an extern attribute)
// "longclass.xxx...x" of kMaxClassNameBytes - 1 bytes, the longest the intern buffer takes. Returns the module.
inline Module* build_longest_class(Context& ctx, bool attr_class)
{
    using crd::containers::String;
    using crd::containers::StringView;
    const usize  dialect = std::strlen(kLongClassDialect);
    const String cls(Context::kMaxClassNameBytes - 2U - dialect, 'x', ctx.allocator());
    AttrId       value;
    if (attr_class)
    {
        value = ctx.attr_extern(ctx.intern_attr_class(kLongClassDialect, StringView(cls.data(), cls.size())),
                                ctx.attr_int(7));
    }
    else
    {
        const Type params;
        value = ctx.attr_type(
            ctx.type_extern(ctx.intern_type_class(kLongClassDialect, StringView(cls.data(), cls.size())), params));
    }
    Module* const    m   = ctx.create_module();
    Block* const     top = ctx.create_block(0U);
    Operation* const op  = ctx.create_operation(ctx.intern_op("test", "x"), {}, 0U);
    m->body()->append(top);
    ctx.set_attr(op, "v", value);
    top->append(op);
    return m;
}

// `text` with one more 'x' right after the first "longclass.": the same class name one byte longer. Returns an empty
// string when the text names no such class.
inline crd::containers::String lengthen_class_text(const crd::containers::String& text, memory::IAllocator* alloc)
{
    crd::containers::String out(alloc);
    const usize             dialect = std::strlen(kLongClassDialect);
    for (usize at = 0U; at + dialect < text.size(); ++at)
    {
        if (std::memcmp(text.data() + at, kLongClassDialect, dialect) == 0 && text.data()[at + dialect] == '.')
        {
            out.append(text.data(), at + dialect + 1U);
            out.push_back('x');
            out.append(text.data() + at + dialect + 1U, text.size() - at - dialect - 1U);
            return out;
        }
    }
    return out;
}

// `blob` with its STRP entry that starts "longclass." one byte longer (an 'x' appended, the entry's length and the
// chunk's payload size raised by one). STRP is the first chunk after the 12-byte header (magic, version, chunk count):
// FourCC, payload size, entry count, then each entry as a u32 length and its bytes. Returns an empty array when the
// blob has no such entry.
inline crd::containers::Array<u8> lengthen_class_blob(const crd::containers::Array<u8>& blob, memory::IAllocator* alloc)
{
    const auto read_u32 = [&](usize at) noexcept
    {
        return static_cast<u32>(blob[at]) | (static_cast<u32>(blob[at + 1U]) << 8U) |
               (static_cast<u32>(blob[at + 2U]) << 16U) | (static_cast<u32>(blob[at + 3U]) << 24U);
    };
    const auto write_u32 = [](crd::containers::Array<u8>& bytes, usize at, u32 value) noexcept
    {
        for (u32 shift = 0U; shift < 32U; shift += 8U)
        {
            bytes[at + shift / 8U] = static_cast<u8>((value >> shift) & 0xFFU);
        }
    };
    crd::containers::Array<u8> out(alloc);
    const usize                dialect = std::strlen(kLongClassDialect);
    if (blob.size() < 24U)
    {
        return out;
    }
    const u32 entries = read_u32(20U);
    usize     at      = 24U;
    for (u32 i = 0U; i < entries && at + 4U <= blob.size(); ++i)
    {
        const u32 length = read_u32(at);
        if (at + 4U + length > blob.size())
        {
            return out;
        }
        if (length > dialect && std::memcmp(blob.data() + at + 4U, kLongClassDialect, dialect) == 0 &&
            blob[at + 4U + dialect] == '.')
        {
            const usize end = at + 4U + length;
            for (usize b = 0U; b < blob.size(); ++b)
            {
                if (b == end)
                {
                    out.push_back(static_cast<u8>('x'));
                }
                out.push_back(blob[b]);
            }
            if (end == blob.size())
            {
                out.push_back(static_cast<u8>('x'));
            }
            write_u32(out, at, length + 1U);
            write_u32(out, 16U, read_u32(16U) + 1U);
            return out;
        }
        at += 4U + length;
    }
    return out;
}
} // namespace crd::ceir::test
