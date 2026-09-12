#pragma once

// xml.hpp — GEO-5 (D-007): OUR XML parser — the second of the GEO band's named missing parsers (json.hpp was the
// first; same flat-DOM design). Scoped deliberately to the DOCUMENT subset the OPC/3MF world uses: elements,
// attributes, character data, the five predefined entities + numeric character references, comments/PIs/DOCTYPE
// skipped, UTF-8 throughout. Namespace PREFIXES are kept verbatim in names (matching is by qualified name — 3MF
// documents use fixed prefixes; a resolving namespace layer can ride on top when a consumer needs one).
//
// The DOM is a first-child/next-sibling node pool + one decoded string pool (zero per-node allocation, the json.hpp
// pattern); attributes live in one flat array sliced per element. Depth-capped (64) — a hostile "<a><a><a>…" fails
// cleanly. No partial DOM survives failure.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

enum class XmlError : crd::u8
{
    Ok = 0,
    Malformed,    // syntax violation (mismatched tags, bad entity, bad attribute quoting, …)
    TooDeep,      // nesting beyond the cap
    Truncated,    // input ended inside a construct
};

inline constexpr crd::i32 kXmlInvalid = -1;

struct XmlAttr
{
    crd::u32 name_off  = 0; // offsets into the string pool (NUL-terminated)
    crd::u32 value_off = 0;
};

struct XmlNode
{
    crd::u32 name_off    = 0;           // element name (qualified, prefix verbatim); empty for TEXT nodes
    crd::u32 text_off    = 0;           // TEXT nodes: decoded character data
    crd::i32 first_child = kXmlInvalid; // elements AND text children, in document order
    crd::i32 next        = kXmlInvalid;
    crd::u32 attr_begin  = 0;           // slice into XmlDoc::attrs
    crd::u32 attr_count  = 0;
    bool     is_text     = false;
};

class XmlDoc
{
public:
    explicit XmlDoc(crd::memory::IAllocator* a) : m_nodes(a), m_attrs(a), m_pool(a) {}

    // Parse `bytes` (UTF-8 XML). On failure the document is EMPTY (no partial DOM).
    [[nodiscard]] XmlError parse(crd::containers::ConstSpan<crd::u8> bytes);

    [[nodiscard]] crd::i32 root() const noexcept { return m_root; }

    [[nodiscard]] const char* name(crd::i32 node) const noexcept
    {
        return valid(node) ? m_pool.data() + m_nodes[static_cast<crd::usize>(node)].name_off : "";
    }
    [[nodiscard]] bool is_text(crd::i32 node) const noexcept
    {
        return valid(node) && m_nodes[static_cast<crd::usize>(node)].is_text;
    }
    [[nodiscard]] const char* text(crd::i32 node) const noexcept
    {
        return valid(node) ? m_pool.data() + m_nodes[static_cast<crd::usize>(node)].text_off : "";
    }
    [[nodiscard]] crd::i32 first_child(crd::i32 node) const noexcept
    {
        return valid(node) ? m_nodes[static_cast<crd::usize>(node)].first_child : kXmlInvalid;
    }
    [[nodiscard]] crd::i32 next(crd::i32 node) const noexcept
    {
        return valid(node) ? m_nodes[static_cast<crd::usize>(node)].next : kXmlInvalid;
    }

    // kXmlInvalid-safe chained lookups (the json.hpp accessor discipline)
    [[nodiscard]] crd::i32    child(crd::i32 node, const char* element_name) const noexcept; // first ELEMENT child by name
    [[nodiscard]] crd::i32    sibling(crd::i32 node, const char* element_name) const noexcept; // next ELEMENT sibling by name
    [[nodiscard]] const char* attr(crd::i32 node, const char* attr_name) const noexcept;     // nullptr when absent

private:
    [[nodiscard]] bool valid(crd::i32 node) const noexcept
    {
        return node >= 0 && static_cast<crd::usize>(node) < m_nodes.size();
    }

    crd::containers::Array<XmlNode> m_nodes;
    crd::containers::Array<XmlAttr> m_attrs;
    crd::containers::Array<char>    m_pool;
    crd::i32                        m_root = kXmlInvalid;
};

} // namespace crd::assetio
