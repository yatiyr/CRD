#include <crd/hesap/cli/json_writer.hpp>

#include <crd/core/assert.hpp>

#include <cstdio>
#include <utility>

namespace crd::hesap::cli
{

JsonWriter::JsonWriter(crd::memory::IAllocator* alloc) noexcept : m_out(alloc)
{
    for (crd::usize i = 0; i < kMaxDepth; ++i)
    {
        m_first_in_frame[i] = true;
    }
}

crd::containers::String JsonWriter::take() noexcept
{
    return std::move(m_out);
}

void JsonWriter::push_frame(Frame f) noexcept
{
    CRD_ASSERT_MSG(m_depth < kMaxDepth, "JsonWriter nesting overflow");
    m_stack[m_depth] = f;
    m_first_in_frame[m_depth] = true;
    ++m_depth;
}

void JsonWriter::pop_frame() noexcept
{
    CRD_ASSERT_MSG(m_depth > 0, "JsonWriter pop with empty stack");
    --m_depth;
}

bool JsonWriter::inside_object() const noexcept
{
    return m_depth > 0 && m_stack[m_depth - 1] == Frame::Object;
}

void JsonWriter::write_comma_if_needed() noexcept
{
    if (m_depth == 0)
    {
        return;
    }
    if (m_first_in_frame[m_depth - 1])
    {
        m_first_in_frame[m_depth - 1] = false;
    }
    else
    {
        m_out.append(",");
        if (m_pretty)
        {
            write_newline_indent();
        }
    }
}

void JsonWriter::write_newline_indent() noexcept
{
    if (!m_pretty)
    {
        return;
    }
    m_out.append("\n");
    for (crd::usize i = 0; i < m_depth; ++i)
    {
        m_out.append("  ");
    }
}

void JsonWriter::write_escaped_string(crd::containers::StringView s) noexcept
{
    m_out.append("\"");
    for (char c : s)
    {
        switch (c)
        {
        case '\\':
            m_out.append("\\\\");
            break;
        case '"':
            m_out.append("\\\"");
            break;
        case '\b':
            m_out.append("\\b");
            break;
        case '\f':
            m_out.append("\\f");
            break;
        case '\n':
            m_out.append("\\n");
            break;
        case '\r':
            m_out.append("\\r");
            break;
        case '\t':
            m_out.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                m_out.append(buf);
            }
            else
            {
                const char single[2] = {c, '\0'};
                m_out.append(single);
            }
            break;
        }
    }
    m_out.append("\"");
}

void JsonWriter::begin_object() noexcept
{
    write_comma_if_needed();
    m_out.append("{");
    push_frame(Frame::Object);
}

void JsonWriter::end_object() noexcept
{
    CRD_ASSERT_MSG(inside_object(), "JsonWriter::end_object outside object frame");
    pop_frame();
    if (m_pretty)
    {
        write_newline_indent();
    }
    m_out.append("}");
}

void JsonWriter::begin_array() noexcept
{
    write_comma_if_needed();
    m_out.append("[");
    push_frame(Frame::Array);
}

void JsonWriter::end_array() noexcept
{
    CRD_ASSERT_MSG(m_depth > 0 && m_stack[m_depth - 1] == Frame::Array, "JsonWriter::end_array outside array frame");
    pop_frame();
    if (m_pretty)
    {
        write_newline_indent();
    }
    m_out.append("]");
}

void JsonWriter::key(crd::containers::StringView name) noexcept
{
    CRD_ASSERT_MSG(inside_object(), "JsonWriter::key outside object frame");
    write_comma_if_needed();
    if (m_pretty)
    {
        write_newline_indent();
    }
    write_escaped_string(name);
    m_out.append(m_pretty ? ": " : ":");
    // The next value call must NOT emit its own comma — mark this frame as
    // expecting-a-value by toggling first_in_frame to true; the value call
    // then sees first_in_frame and suppresses the comma it would otherwise
    // emit. After the value is written, first_in_frame is false again.
    m_first_in_frame[m_depth - 1] = true;
}

void JsonWriter::value_string(crd::containers::StringView s) noexcept
{
    write_comma_if_needed();
    write_escaped_string(s);
}

void JsonWriter::value_bool(bool b) noexcept
{
    write_comma_if_needed();
    m_out.append(b ? "true" : "false");
}

void JsonWriter::value_null() noexcept
{
    write_comma_if_needed();
    m_out.append("null");
}

void JsonWriter::value_u32(crd::u32 n) noexcept
{
    write_comma_if_needed();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", n);
    m_out.append(buf);
}

void JsonWriter::value_u64(crd::u64 n) noexcept
{
    write_comma_if_needed();
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
    m_out.append(buf);
}

void JsonWriter::value_i32(crd::i32 n) noexcept
{
    write_comma_if_needed();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", n);
    m_out.append(buf);
}

void JsonWriter::value_i64(crd::i64 n) noexcept
{
    write_comma_if_needed();
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
    m_out.append(buf);
}

void JsonWriter::value_f64(crd::f64 v) noexcept
{
    write_comma_if_needed();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    m_out.append(buf);
}

void JsonWriter::key_value(crd::containers::StringView name, crd::containers::StringView v) noexcept
{
    key(name);
    value_string(v);
}

void JsonWriter::key_value(crd::containers::StringView name, bool v) noexcept
{
    key(name);
    value_bool(v);
}

void JsonWriter::key_value(crd::containers::StringView name, crd::u32 v) noexcept
{
    key(name);
    value_u32(v);
}

void JsonWriter::key_value(crd::containers::StringView name, crd::i32 v) noexcept
{
    key(name);
    value_i32(v);
}

} // namespace crd::hesap::cli
