// xml.cpp — GEO-5 (D-007): the XML parser. See xml.hpp for the scope contract.

#include <crd/assetio/xml.hpp>

#include <cstring>

namespace crd::assetio
{
namespace
{

constexpr int kMaxDepth = 64;

struct Cursor
{
    const crd::u8* p   = nullptr;
    const crd::u8* end = nullptr;

    [[nodiscard]] bool     done() const noexcept { return p >= end; }
    [[nodiscard]] crd::u8  peek() const noexcept { return p < end ? *p : 0U; }
    void                   advance() noexcept { ++p; }
    [[nodiscard]] bool     starts_with(const char* s) const noexcept
    {
        const crd::usize n = std::strlen(s);
        return static_cast<crd::usize>(end - p) >= n && std::memcmp(p, s, n) == 0;
    }
    void skip(crd::usize n) noexcept { p += n; }
    void skip_ws() noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) { ++p; }
    }
};

[[nodiscard]] bool is_name_start(crd::u8 c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80U;
}
[[nodiscard]] bool is_name_char(crd::u8 c) noexcept
{
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

// Decode one entity at `c` (positioned ON '&') into `pool`. Returns false on malformed/unknown.
[[nodiscard]] bool decode_entity(Cursor& c, crd::containers::Array<char>& pool)
{
    c.advance(); // '&'
    if (c.starts_with("amp;")) { pool.push_back('&'); c.skip(4); return true; }
    if (c.starts_with("lt;")) { pool.push_back('<'); c.skip(3); return true; }
    if (c.starts_with("gt;")) { pool.push_back('>'); c.skip(3); return true; }
    if (c.starts_with("quot;")) { pool.push_back('"'); c.skip(5); return true; }
    if (c.starts_with("apos;")) { pool.push_back('\''); c.skip(5); return true; }
    if (c.peek() == '#')
    {
        c.advance();
        crd::u32 cp   = 0;
        bool     hex  = false;
        bool     any  = false;
        if (c.peek() == 'x' || c.peek() == 'X') { hex = true; c.advance(); }
        while (!c.done() && c.peek() != ';')
        {
            const crd::u8 ch = c.peek();
            crd::u32      d  = 0;
            if (ch >= '0' && ch <= '9') { d = ch - '0'; }
            else if (hex && ch >= 'a' && ch <= 'f') { d = 10U + ch - 'a'; }
            else if (hex && ch >= 'A' && ch <= 'F') { d = 10U + ch - 'A'; }
            else { return false; }
            cp = cp * (hex ? 16U : 10U) + d;
            if (cp > 0x10FFFFU) { return false; }
            any = true;
            c.advance();
        }
        if (!any || c.done()) { return false; }
        c.advance(); // ';'
        // UTF-8 encode (the json.hpp \uXXXX discipline)
        if (cp < 0x80U) { pool.push_back(static_cast<char>(cp)); }
        else if (cp < 0x800U)
        {
            pool.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            pool.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else if (cp < 0x10000U)
        {
            if (cp >= 0xD800U && cp <= 0xDFFFU) { return false; } // surrogates are not characters
            pool.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            pool.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            pool.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else
        {
            pool.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            pool.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            pool.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            pool.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        return true;
    }
    return false; // unknown named entity (external DTDs are out of scope)
}

} // namespace

crd::i32 XmlDoc::child(crd::i32 node, const char* element_name) const noexcept
{
    for (crd::i32 c = first_child(node); c != kXmlInvalid; c = next(c))
    {
        if (!is_text(c) && std::strcmp(name(c), element_name) == 0) { return c; }
    }
    return kXmlInvalid;
}

crd::i32 XmlDoc::sibling(crd::i32 node, const char* element_name) const noexcept
{
    for (crd::i32 c = next(node); c != kXmlInvalid; c = next(c))
    {
        if (!is_text(c) && std::strcmp(name(c), element_name) == 0) { return c; }
    }
    return kXmlInvalid;
}

const char* XmlDoc::attr(crd::i32 node, const char* attr_name) const noexcept
{
    if (!valid(node)) { return nullptr; }
    const XmlNode& n = m_nodes[static_cast<crd::usize>(node)];
    for (crd::u32 a = 0; a < n.attr_count; ++a)
    {
        const XmlAttr& at = m_attrs[n.attr_begin + a];
        if (std::strcmp(m_pool.data() + at.name_off, attr_name) == 0) { return m_pool.data() + at.value_off; }
    }
    return nullptr;
}

XmlError XmlDoc::parse(crd::containers::ConstSpan<crd::u8> bytes)
{
    m_nodes.clear();
    m_attrs.clear();
    m_pool.clear();
    m_root = kXmlInvalid;

    Cursor c{bytes.data(), bytes.data() + bytes.size()};
    if (c.starts_with("\xEF\xBB\xBF")) { c.skip(3); } // UTF-8 BOM

    const auto fail = [&](XmlError e) {
        m_nodes.clear();
        m_attrs.clear();
        m_pool.clear();
        m_root = kXmlInvalid;
        return e;
    };

    // intern the run [start, c.p) with entities decoded → pool offset
    const auto intern_name = [&](const crd::u8* start, const crd::u8* stop) {
        const crd::u32 off = static_cast<crd::u32>(m_pool.size());
        for (const crd::u8* q = start; q < stop; ++q) { m_pool.push_back(static_cast<char>(*q)); }
        m_pool.push_back('\0');
        return off;
    };

    // parent stack (indices into m_nodes) + last-child per level for sibling linking
    crd::i32 stack[kMaxDepth];
    crd::i32 last_child[kMaxDepth];
    int      depth = 0;

    const auto add_node = [&](crd::u32 name_off, bool text, crd::u32 text_off) {
        XmlNode n;
        n.name_off = name_off;
        n.text_off = text_off;
        n.is_text  = text;
        const crd::i32 id = static_cast<crd::i32>(m_nodes.size());
        m_nodes.push_back(n);
        if (depth == 0)
        {
            if (!text && m_root == kXmlInvalid) { m_root = id; }
        }
        else
        {
            const crd::i32 parent = stack[depth - 1];
            if (last_child[depth - 1] == kXmlInvalid) { m_nodes[static_cast<crd::usize>(parent)].first_child = id; }
            else { m_nodes[static_cast<crd::usize>(last_child[depth - 1])].next = id; }
            last_child[depth - 1] = id;
        }
        return id;
    };

    while (!c.done())
    {
        if (c.peek() != '<')
        {
            // character data run (entities decoded); whitespace-only runs between elements are skipped
            const crd::u32 text_off = static_cast<crd::u32>(m_pool.size());
            bool           only_ws  = true;
            while (!c.done() && c.peek() != '<')
            {
                if (c.peek() == '&')
                {
                    if (!decode_entity(c, m_pool)) { return fail(XmlError::Malformed); }
                    only_ws = false;
                }
                else
                {
                    const crd::u8 ch = c.peek();
                    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') { only_ws = false; }
                    m_pool.push_back(static_cast<char>(ch));
                    c.advance();
                }
            }
            m_pool.push_back('\0');
            if (!only_ws)
            {
                if (depth == 0) { return fail(XmlError::Malformed); } // character data outside the root
                (void)add_node(text_off, true, text_off);
            }
            else
            {
                m_pool.resize(static_cast<crd::usize>(text_off)); // drop the interned whitespace
            }
            continue;
        }

        // '<' … dispatch
        if (c.starts_with("<!--"))
        {
            const crd::u8* q = c.p + 4;
            while (q + 3 <= c.end && std::memcmp(q, "-->", 3) != 0) { ++q; }
            if (q + 3 > c.end) { return fail(XmlError::Truncated); }
            c.p = q + 3;
            continue;
        }
        if (c.starts_with("<?") || c.starts_with("<!"))
        {
            while (!c.done() && c.peek() != '>') { c.advance(); }
            if (c.done()) { return fail(XmlError::Truncated); }
            c.advance();
            continue;
        }
        if (c.starts_with("</"))
        {
            c.skip(2);
            const crd::u8* nm = c.p;
            while (!c.done() && is_name_char(c.peek())) { c.advance(); }
            if (depth == 0) { return fail(XmlError::Malformed); }
            const XmlNode& open = m_nodes[static_cast<crd::usize>(stack[depth - 1])];
            const crd::usize nlen = static_cast<crd::usize>(c.p - nm);
            if (std::strlen(m_pool.data() + open.name_off) != nlen
                || std::memcmp(m_pool.data() + open.name_off, nm, nlen) != 0)
            {
                return fail(XmlError::Malformed); // mismatched close tag
            }
            c.skip_ws();
            if (c.done() || c.peek() != '>') { return fail(c.done() ? XmlError::Truncated : XmlError::Malformed); }
            c.advance();
            --depth;
            continue;
        }

        // an open tag
        c.advance(); // '<'
        if (c.done() || !is_name_start(c.peek())) { return fail(c.done() ? XmlError::Truncated : XmlError::Malformed); }
        const crd::u8* nm = c.p;
        while (!c.done() && is_name_char(c.peek())) { c.advance(); }
        const crd::u32 name_off = intern_name(nm, c.p);
        const crd::i32 id       = add_node(name_off, false, name_off);
        if (depth == 0 && id != m_root) { return fail(XmlError::Malformed); } // a document has exactly ONE root
        m_nodes[static_cast<crd::usize>(id)].attr_begin = static_cast<crd::u32>(m_attrs.size());

        // attributes
        for (;;)
        {
            c.skip_ws();
            if (c.done()) { return fail(XmlError::Truncated); }
            if (c.peek() == '>' || c.starts_with("/>")) { break; }
            if (!is_name_start(c.peek())) { return fail(XmlError::Malformed); }
            const crd::u8* an = c.p;
            while (!c.done() && is_name_char(c.peek())) { c.advance(); }
            const crd::u32 aname = intern_name(an, c.p);
            c.skip_ws();
            if (c.done() || c.peek() != '=') { return fail(c.done() ? XmlError::Truncated : XmlError::Malformed); }
            c.advance();
            c.skip_ws();
            if (c.done() || (c.peek() != '"' && c.peek() != '\'')) { return fail(c.done() ? XmlError::Truncated : XmlError::Malformed); }
            const crd::u8 quote = c.peek();
            c.advance();
            const crd::u32 aval = static_cast<crd::u32>(m_pool.size());
            while (!c.done() && c.peek() != quote)
            {
                if (c.peek() == '<') { return fail(XmlError::Malformed); }
                if (c.peek() == '&')
                {
                    if (!decode_entity(c, m_pool)) { return fail(XmlError::Malformed); }
                }
                else
                {
                    m_pool.push_back(static_cast<char>(c.peek()));
                    c.advance();
                }
            }
            if (c.done()) { return fail(XmlError::Truncated); }
            c.advance(); // closing quote
            m_pool.push_back('\0');
            XmlAttr at;
            at.name_off  = aname;
            at.value_off = aval;
            m_attrs.push_back(at);
            m_nodes[static_cast<crd::usize>(id)].attr_count += 1U;
        }

        if (c.starts_with("/>"))
        {
            c.skip(2); // self-closing — never pushed
            continue;
        }
        c.advance(); // '>'
        if (depth >= kMaxDepth) { return fail(XmlError::TooDeep); }
        stack[depth]      = id;
        last_child[depth] = m_nodes[static_cast<crd::usize>(id)].first_child; // kXmlInvalid on a fresh element
        ++depth;
    }

    if (depth != 0) { return fail(XmlError::Truncated); } // an unclosed element
    if (m_root == kXmlInvalid) { return fail(XmlError::Malformed); }
    return XmlError::Ok;
}

} // namespace crd::assetio
