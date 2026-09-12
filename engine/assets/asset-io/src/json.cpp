// json.cpp — GEO-3: the owned JSON parser. See json.hpp for the design.

#include <crd/assetio/json.hpp>

#include <cstdlib>
#include <cstring>

namespace crd::assetio::json
{
namespace
{

constexpr int kMaxDepth = 64;

struct Parser
{
    const crd::u8* p;
    const crd::u8* begin;
    const crd::u8* end;
    JsonDoc*       doc;

    void skip_ws() noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) { ++p; }
    }
    [[nodiscard]] bool eat(char c) noexcept
    {
        if (p < end && *p == static_cast<crd::u8>(c))
        {
            ++p;
            return true;
        }
        return false;
    }
    void fail() noexcept { doc->error_off = static_cast<crd::usize>(p - begin); }

    // ── string decode (escapes + \uXXXX incl. surrogate pairs → UTF-8) into the pool ──────────────────────────────────
    [[nodiscard]] bool hex4(crd::u32& out) noexcept
    {
        if (end - p < 4) { return false; }
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
            const crd::u8 c = p[i];
            crd::u32      d = 0;
            if (c >= '0' && c <= '9') { d = c - crd::u8{'0'}; }
            else if (c >= 'a' && c <= 'f') { d = 10U + (c - crd::u8{'a'}); }
            else if (c >= 'A' && c <= 'F') { d = 10U + (c - crd::u8{'A'}); }
            else { return false; }
            out = (out << 4U) | d;
        }
        p += 4;
        return true;
    }
    void utf8_append(crd::u32 cp)
    {
        auto& s = doc->strings;
        if (cp < 0x80U) { s.push_back(static_cast<char>(cp)); }
        else if (cp < 0x800U)
        {
            s.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else if (cp < 0x10000U)
        {
            s.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else
        {
            s.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            s.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }
    // expects the opening quote consumed; decodes into the pool; false on malformed
    [[nodiscard]] bool parse_string(crd::u32& off, crd::u32& len)
    {
        off = static_cast<crd::u32>(doc->strings.size());
        for (;;)
        {
            if (p >= end)
            {
                fail();
                return false;
            }
            const crd::u8 c = *p;
            if (c == '"')
            {
                ++p;
                len = static_cast<crd::u32>(doc->strings.size()) - off;
                return true;
            }
            if (c == '\\')
            {
                ++p;
                if (p >= end)
                {
                    fail();
                    return false;
                }
                const crd::u8 e = *p;
                ++p;
                switch (e)
                {
                case '"': doc->strings.push_back('"'); break;
                case '\\': doc->strings.push_back('\\'); break;
                case '/': doc->strings.push_back('/'); break;
                case 'b': doc->strings.push_back('\b'); break;
                case 'f': doc->strings.push_back('\f'); break;
                case 'n': doc->strings.push_back('\n'); break;
                case 'r': doc->strings.push_back('\r'); break;
                case 't': doc->strings.push_back('\t'); break;
                case 'u': {
                    crd::u32 cp = 0;
                    if (!hex4(cp))
                    {
                        fail();
                        return false;
                    }
                    if (cp >= 0xD800U && cp <= 0xDBFFU) // high surrogate: require the low half
                    {
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                        {
                            fail();
                            return false;
                        }
                        p += 2;
                        crd::u32 lo = 0;
                        if (!hex4(lo) || lo < 0xDC00U || lo > 0xDFFFU)
                        {
                            fail();
                            return false;
                        }
                        cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
                    }
                    else if (cp >= 0xDC00U && cp <= 0xDFFFU) // a lone low surrogate is malformed
                    {
                        fail();
                        return false;
                    }
                    utf8_append(cp);
                    break;
                }
                default: fail(); return false;
                }
                continue;
            }
            if (c < 0x20U) // raw control characters are illegal inside JSON strings
            {
                fail();
                return false;
            }
            doc->strings.push_back(static_cast<char>(c));
            ++p;
        }
    }

    [[nodiscard]] bool parse_number(crd::f64& out)
    {
        // bounded copy → strtod (JSON numbers are a strict strtod subset; reject trailing junk via the end pointer)
        char       buf[64];
        crd::usize n = 0;
        const crd::u8* q = p;
        while (q < end
               && ((*q >= '0' && *q <= '9') || *q == '-' || *q == '+' || *q == '.' || *q == 'e' || *q == 'E'))
        {
            if (n + 1 < sizeof(buf)) { buf[n++] = static_cast<char>(*q); }
            ++q;
        }
        buf[n]      = '\0';
        char* endp  = nullptr;
        out         = std::strtod(buf, &endp);
        if (endp == buf || *endp != '\0')
        {
            fail();
            return false;
        }
        p = q;
        return true;
    }

    [[nodiscard]] bool literal(const char* s)
    {
        const crd::usize n = std::strlen(s);
        if (static_cast<crd::usize>(end - p) < n || std::memcmp(p, s, n) != 0)
        {
            fail();
            return false;
        }
        p += n;
        return true;
    }

    // parse one value; returns the node index or kInvalid
    [[nodiscard]] crd::u32 parse_value(int depth)
    {
        if (depth > kMaxDepth)
        {
            fail();
            return kInvalid;
        }
        skip_ws();
        if (p >= end)
        {
            fail();
            return kInvalid;
        }
        const crd::u32 idx = static_cast<crd::u32>(doc->nodes.size());
        doc->nodes.push_back(JsonNode{});
        const crd::u8 c = *p;
        if (c == '{' || c == '[')
        {
            const bool is_obj = c == '{';
            ++p;
            doc->nodes[idx].type = is_obj ? JsonType::Object : JsonType::Array;
            crd::u32 prev  = kInvalid;
            crd::u32 count = 0;
            skip_ws();
            if (eat(is_obj ? '}' : ']')) { return idx; } // empty
            for (;;)
            {
                crd::u32 koff = 0;
                crd::u32 klen = 0;
                if (is_obj)
                {
                    skip_ws();
                    if (!eat('"'))
                    {
                        fail();
                        return kInvalid;
                    }
                    if (!parse_string(koff, klen)) { return kInvalid; }
                    skip_ws();
                    if (!eat(':'))
                    {
                        fail();
                        return kInvalid;
                    }
                }
                const crd::u32 child = parse_value(depth + 1);
                if (child == kInvalid) { return kInvalid; }
                doc->nodes[child].key_off = koff;
                doc->nodes[child].key_len = klen;
                if (prev == kInvalid) { doc->nodes[idx].child = child; }
                else { doc->nodes[prev].next = child; }
                prev = child;
                ++count;
                skip_ws();
                if (eat(','))
                {
                    continue;
                }
                if (eat(is_obj ? '}' : ']'))
                {
                    doc->nodes[idx].count = count;
                    return idx;
                }
                fail();
                return kInvalid;
            }
        }
        if (c == '"')
        {
            ++p;
            doc->nodes[idx].type = JsonType::String;
            crd::u32 off         = 0;
            crd::u32 len         = 0;
            if (!parse_string(off, len)) { return kInvalid; }
            doc->nodes[idx].str_off = off;
            doc->nodes[idx].str_len = len;
            return idx;
        }
        if (c == 't')
        {
            if (!literal("true")) { return kInvalid; }
            doc->nodes[idx].type    = JsonType::Bool;
            doc->nodes[idx].boolean = true;
            return idx;
        }
        if (c == 'f')
        {
            if (!literal("false")) { return kInvalid; }
            doc->nodes[idx].type = JsonType::Bool;
            return idx;
        }
        if (c == 'n')
        {
            if (!literal("null")) { return kInvalid; }
            return idx; // Null
        }
        doc->nodes[idx].type = JsonType::Number;
        if (!parse_number(doc->nodes[idx].number)) { return kInvalid; }
        return idx;
    }
};

} // namespace

bool parse(crd::containers::ConstSpan<crd::u8> bytes, JsonDoc& doc)
{
    doc.nodes.clear();
    doc.strings.clear();
    doc.root      = kInvalid;
    doc.error_off = 0;
    Parser ps{bytes.data(), bytes.data(), bytes.data() + bytes.size(), &doc};
    const crd::u32 root = ps.parse_value(0);
    if (root == kInvalid)
    {
        doc.nodes.clear();
        doc.strings.clear();
        return false;
    }
    ps.skip_ws();
    if (ps.p != ps.end) // trailing junk after the document
    {
        ps.fail();
        doc.nodes.clear();
        doc.strings.clear();
        return false;
    }
    doc.root = root;
    return true;
}

bool str_eq(const JsonDoc& doc, crd::u32 off, crd::u32 len, const char* s) noexcept
{
    const crd::usize n = std::strlen(s);
    if (n != len) { return false; }
    return len == 0U || std::memcmp(doc.strings.data() + off, s, n) == 0;
}

crd::u32 find(const JsonDoc& doc, crd::u32 obj, const char* key) noexcept
{
    if (obj == kInvalid || obj >= doc.nodes.size() || doc.nodes[obj].type != JsonType::Object) { return kInvalid; }
    for (crd::u32 c = doc.nodes[obj].child; c != kInvalid; c = doc.nodes[c].next)
    {
        if (str_eq(doc, doc.nodes[c].key_off, doc.nodes[c].key_len, key)) { return c; }
    }
    return kInvalid;
}

crd::u32 at(const JsonDoc& doc, crd::u32 arr, crd::u32 index) noexcept
{
    if (arr == kInvalid || arr >= doc.nodes.size() || doc.nodes[arr].type != JsonType::Array) { return kInvalid; }
    crd::u32 c = doc.nodes[arr].child;
    for (crd::u32 i = 0; c != kInvalid && i < index; ++i) { c = doc.nodes[c].next; }
    return c;
}

crd::u32 count_of(const JsonDoc& doc, crd::u32 node) noexcept
{
    if (node == kInvalid || node >= doc.nodes.size()) { return 0; }
    return doc.nodes[node].count;
}

crd::f64 as_f64(const JsonDoc& doc, crd::u32 node, crd::f64 def) noexcept
{
    if (node == kInvalid || node >= doc.nodes.size() || doc.nodes[node].type != JsonType::Number) { return def; }
    return doc.nodes[node].number;
}

crd::i64 as_i64(const JsonDoc& doc, crd::u32 node, crd::i64 def) noexcept
{
    if (node == kInvalid || node >= doc.nodes.size() || doc.nodes[node].type != JsonType::Number) { return def; }
    return static_cast<crd::i64>(doc.nodes[node].number);
}

bool as_bool(const JsonDoc& doc, crd::u32 node, bool def) noexcept
{
    if (node == kInvalid || node >= doc.nodes.size() || doc.nodes[node].type != JsonType::Bool) { return def; }
    return doc.nodes[node].boolean;
}

bool str_value_eq(const JsonDoc& doc, crd::u32 node, const char* s) noexcept
{
    if (node == kInvalid || node >= doc.nodes.size() || doc.nodes[node].type != JsonType::String) { return false; }
    return str_eq(doc, doc.nodes[node].str_off, doc.nodes[node].str_len, s);
}

crd::u32 str_value(const JsonDoc& doc, crd::u32 node, char* buf, crd::u32 cap) noexcept
{
    if (cap == 0U) { return 0; }
    buf[0] = '\0';
    if (node == kInvalid || node >= doc.nodes.size() || doc.nodes[node].type != JsonType::String) { return 0; }
    const crd::u32 len = doc.nodes[node].str_len;
    const crd::u32 n   = len < cap - 1U ? len : cap - 1U;
    if (n > 0U) { std::memcpy(buf, doc.strings.data() + doc.nodes[node].str_off, n); }
    buf[n] = '\0';
    return len;
}

} // namespace crd::assetio::json
