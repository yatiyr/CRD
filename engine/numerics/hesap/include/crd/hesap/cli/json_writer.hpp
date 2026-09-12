#pragma once

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::cli
{
// -----------------------------------------------------------------------
// JsonWriter — minimal write-only JSON emitter.
//
// Why hand-rolled rather than pulling toml++ or a JSON dep:
//   - The whole emitter is ~120 LOC. A new dep is way more cost.
//   - We only need the MCP-tool-descriptor shape: nested objects, arrays,
//     strings, bools, numbers, null. No parsing, no streaming input.
//   - JSON output is the wire format for meta.export-mcp-tools (ADR-0081
//     §6.4) and the JSON-RPC layer in Phase 4.0; this writer is the
//     shared backbone.
//
// API style: each call appends to the underlying String. The writer
// tracks indentation + comma state via a small fixed-depth stack. Max
// nesting depth is kMaxDepth (32) — plenty for any MCP descriptor.
//
// String escaping: the writer escapes control chars, quote, backslash,
// per RFC 8259 §7. Non-ASCII UTF-8 bytes pass through unmodified.
// -----------------------------------------------------------------------

class JsonWriter
{
public:
    static constexpr crd::usize kMaxDepth = 32;

    explicit JsonWriter(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

    void begin_object() noexcept;
    void end_object() noexcept;

    void begin_array() noexcept;
    void end_array() noexcept;

    // For inside an object — emits the key + ':'.
    void key(crd::containers::StringView name) noexcept;

    void value_string(crd::containers::StringView s) noexcept;
    void value_bool(bool b) noexcept;
    void value_null() noexcept;
    void value_u32(crd::u32 n) noexcept;
    void value_u64(crd::u64 n) noexcept;
    void value_i32(crd::i32 n) noexcept;
    void value_i64(crd::i64 n) noexcept;
    void value_f64(crd::f64 v) noexcept;

    // Composite — emits "key": "value" in one call (must be inside an object).
    void key_value(crd::containers::StringView name, crd::containers::StringView v) noexcept;
    void key_value(crd::containers::StringView name, bool v) noexcept;
    void key_value(crd::containers::StringView name, crd::u32 v) noexcept;
    void key_value(crd::containers::StringView name, crd::i32 v) noexcept;

    // Pretty-print toggle. Default: compact (no whitespace).
    void set_pretty(bool pretty) noexcept { m_pretty = pretty; }

    [[nodiscard]] const crd::containers::String& str() const noexcept { return m_out; }
    [[nodiscard]] crd::containers::String take() noexcept;

private:
    enum class Frame : crd::u8
    {
        Object = 0,
        Array = 1,
    };

    void write_comma_if_needed() noexcept;
    void write_newline_indent() noexcept;
    void write_escaped_string(crd::containers::StringView s) noexcept;
    void push_frame(Frame f) noexcept;
    void pop_frame() noexcept;
    [[nodiscard]] bool inside_object() const noexcept;

    crd::containers::String m_out;
    Frame m_stack[kMaxDepth]{};
    bool m_first_in_frame[kMaxDepth]{};
    crd::usize m_depth = 0;
    bool m_pretty = false;
};

} // namespace crd::hesap::cli
