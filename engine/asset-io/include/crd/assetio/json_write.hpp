#pragma once

// json_write.hpp — GEO-4 (D-007): the JSON WRITER — the emit half of our RFC-8259 surface (json.hpp is the parse
// half). A small streaming builder over crd::containers::String: push/pop objects and arrays, keys, escaped strings,
// numbers. No DOM, no allocation beyond the output string — the glTF exporter streams straight into it.
//
// Number contract: f64 emits with enough digits to round-trip an f32 exactly (%.9g — glTF's numeric payloads are
// f32-valued), integers emit exactly. NaN/Inf are NOT representable in JSON — the writer asserts (an exporter must
// sanitize upstream; silently writing "null" would corrupt a mesh).

#include <crd/containers/string.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <cmath> // std::isfinite — the classification fn (the gltf.cpp idiom; the transcendental ban does not cover it)

#include <cstdio>

namespace crd::assetio
{

class JsonWriter
{
public:
    explicit JsonWriter(crd::memory::IAllocator* alloc) : m_out(alloc) {}

    [[nodiscard]] const crd::containers::String& str() const noexcept { return m_out; }

    // ── structure ──────────────────────────────────────────────────────────────────────────────────────────────────
    void begin_object() { comma(); m_out.append("{"); m_first = true; }
    void end_object() { m_out.append("}"); m_first = false; }
    void begin_array() { comma(); m_out.append("["); m_first = true; }
    void end_array() { m_out.append("]"); m_first = false; }

    // a key inside an object; the NEXT value call attaches to it
    void key(const char* k)
    {
        comma();
        append_escaped(k);
        m_out.append(":");
        m_first = true; // the value after a key never takes a comma
    }

    // ── values ─────────────────────────────────────────────────────────────────────────────────────────────────────
    void value_string(crd::containers::StringView s) { comma(); append_escaped_sv(s); }
    void value_string(const char* s) { comma(); append_escaped(s); }
    void value_bool(bool b) { comma(); m_out.append(b ? "true" : "false"); }
    void value_null() { comma(); m_out.append("null"); }
    void value_u64(crd::u64 v)
    {
        comma();
        char b[24];
        std::snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(v));
        m_out.append(b);
    }
    void value_i64(crd::i64 v)
    {
        comma();
        char b[24];
        std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(v));
        m_out.append(b);
    }
    // f64 carrying an f32-valued payload (the glTF case): %.9g round-trips every finite f32 exactly.
    void value_f64(crd::f64 v)
    {
        CRD_ASSERT(std::isfinite(v) && "JSON cannot represent NaN/Inf — sanitize before export");
        comma();
        char b[32];
        std::snprintf(b, sizeof(b), "%.9g", v);
        m_out.append(b);
    }

    // f64 carrying a TRUE f64 payload (OTIO rational times): %.17g round-trips every finite f64 exactly.
    void value_f64_exact(crd::f64 v)
    {
        CRD_ASSERT(std::isfinite(v) && "JSON cannot represent NaN/Inf — sanitize before export");
        comma();
        char b[32];
        std::snprintf(b, sizeof(b), "%.17g", v);
        m_out.append(b);
    }
    void kv_f64_exact(const char* k, crd::f64 v) { key(k); value_f64_exact(v); }

    // key + value conveniences (the exporter's bread and butter)
    void kv(const char* k, const char* s) { key(k); value_string(s); }
    void kv(const char* k, crd::containers::StringView s) { key(k); value_string(s); }
    void kv(const char* k, bool b) { key(k); value_bool(b); }
    void kv(const char* k, crd::u64 v) { key(k); value_u64(v); }
    void kv(const char* k, crd::u32 v) { key(k); value_u64(v); }
    void kv(const char* k, crd::i64 v) { key(k); value_i64(v); }
    void kv(const char* k, crd::f64 v) { key(k); value_f64(v); }

private:
    void comma()
    {
        if (!m_first) { m_out.append(","); }
        m_first = false;
    }

    void append_escaped(const char* s)
    {
        append_escaped_sv(crd::containers::StringView(s));
    }
    void append_escaped_sv(crd::containers::StringView s)
    {
        m_out.append("\"");
        for (crd::usize i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            const auto u = static_cast<unsigned char>(c);
            if (c == '"') { m_out.append("\\\""); }
            else if (c == '\\') { m_out.append("\\\\"); }
            else if (u >= 0x20U) { m_out.push_back(c); } // UTF-8 bytes pass through verbatim
            else if (c == '\n') { m_out.append("\\n"); }
            else if (c == '\t') { m_out.append("\\t"); }
            else if (c == '\r') { m_out.append("\\r"); }
            else
            {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(u));
                m_out.append(b);
            }
        }
        m_out.append("\"");
    }

    crd::containers::String m_out;
    bool                    m_first = true;
};

} // namespace crd::assetio
