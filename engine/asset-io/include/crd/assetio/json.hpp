#pragma once

// json.hpp — GEO-3: OUR OWN JSON parser (RFC 8259) — the glTF substrate, and the engine's general machine-readable-text
// parser going forward (GEO-11's CLI reports read with the same code). Zero 3rd-party, allocator-aware, span-based.
//
// Design: a FLAT DOM — one `Array<JsonNode>` pool with first-child/next-sibling links (no per-node allocation), string
// bytes (keys + values, escapes decoded, \uXXXX → UTF-8 incl. surrogate pairs) appended to one byte pool. Numbers parse
// to f64 (JSON's own model; glTF integer indices are exact in f64 far beyond any real asset). Depth-capped recursive
// descent (64) — a hostile 1-MB "[[[[…" cannot blow the stack. Parse failures return false with a byte offset, never a
// partial DOM.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio::json
{

inline constexpr crd::u32 kInvalid = 0xFFFFFFFFU;

enum class JsonType : crd::u8
{
    Null = 0,
    Bool,
    Number,
    String,
    Array,
    Object,
};

struct JsonNode
{
    JsonType type    = JsonType::Null;
    bool     boolean = false;
    crd::u32 child   = kInvalid; // Array/Object: first element/member
    crd::u32 next    = kInvalid; // next sibling within the parent
    crd::u32 count   = 0;        // Array/Object: element/member count
    crd::f64 number  = 0.0;
    crd::u32 str_off = 0; // String: decoded bytes in JsonDoc::strings
    crd::u32 str_len = 0;
    crd::u32 key_off = 0; // when this node is an OBJECT MEMBER: its decoded key
    crd::u32 key_len = 0;
};

struct JsonDoc
{
    crd::containers::Array<JsonNode> nodes;
    crd::containers::Array<char>     strings; // decoded string bytes (keys + values), NOT NUL-terminated
    crd::u32                         root      = kInvalid;
    crd::usize                       error_off = 0; // byte offset of the failure when parse() returns false

    explicit JsonDoc(crd::memory::IAllocator* a) : nodes(a), strings(a) {}

    JsonDoc(const JsonDoc&)            = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;
    JsonDoc(JsonDoc&&)                 = default;
    JsonDoc& operator=(JsonDoc&&)      = default;
};

// Parse `bytes` into `doc` (cleared first). False on any RFC violation (error_off = where), with no partial DOM.
[[nodiscard]] bool parse(crd::containers::ConstSpan<crd::u8> bytes, JsonDoc& doc);

// ── accessors (kInvalid-safe: pass kInvalid in, get kInvalid/defaults out — call sites chain without re-checking) ───────

[[nodiscard]] bool str_eq(const JsonDoc& doc, crd::u32 off, crd::u32 len, const char* s) noexcept;

// Object member lookup by key. kInvalid when `obj` is not an object / the key is absent.
[[nodiscard]] crd::u32 find(const JsonDoc& doc, crd::u32 obj, const char* key) noexcept;

// Array element by index (sibling walk). kInvalid when out of range / not an array.
[[nodiscard]] crd::u32 at(const JsonDoc& doc, crd::u32 arr, crd::u32 index) noexcept;

[[nodiscard]] crd::u32 count_of(const JsonDoc& doc, crd::u32 node) noexcept;

// Typed reads with defaults (kInvalid/type-mismatch → the default).
[[nodiscard]] crd::f64 as_f64(const JsonDoc& doc, crd::u32 node, crd::f64 def) noexcept;
[[nodiscard]] crd::i64 as_i64(const JsonDoc& doc, crd::u32 node, crd::i64 def) noexcept;
[[nodiscard]] bool     as_bool(const JsonDoc& doc, crd::u32 node, bool def) noexcept;

// String VALUE equality / copy-out (returns len, 0 when not a string; `buf` gets a NUL-terminated truncated copy).
[[nodiscard]] bool      str_value_eq(const JsonDoc& doc, crd::u32 node, const char* s) noexcept;
[[nodiscard]] crd::u32  str_value(const JsonDoc& doc, crd::u32 node, char* buf, crd::u32 cap) noexcept;

} // namespace crd::assetio::json
