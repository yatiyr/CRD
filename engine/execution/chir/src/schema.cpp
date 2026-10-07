// schema.cpp — the CR-D007 graph-schema serialization (CEIR-32b, ADR-0128 D5). See schema.hpp.
//
// Canonical document, LF-only, sections in a FIXED order (header, nodes, pins, attrs, edges, layout):
//   chirgraph 1
//   node <idx> <id-hex16> <kind> <name|-> <parent-idx|-1> <file> <line> <col>
//   pin <node-idx> <in|out> <name> <type>
//   attr <node-idx> <key> <val>
//   edge <from-node> <from-pin> <to-node> <to-pin>
//   layout <id-hex16> <x-bits-hex8> <y-bits-hex8> <group>
// Tokens are whitespace-separated; a name of "-" is the empty (anonymous) name. Floats round-trip via their raw f32 bit
// pattern (exact + canonical). Node ids are printed explicitly (§180 #1 stable ids in the document) and read back
// verbatim — a re-derive (SourceModel::derive_ids) reproducing them is the anti-drift-on-ids tooth (32b gate).

#include <crd/chir/schema.hpp>

#include <crd/core/types.hpp>

#include <algorithm> // std::ranges::any_of — orphan-layout id resolution (the sanctioned any_of idiom)
#include <bit>       // std::bit_cast — the crd::math::float_convert idiom for exact f32<->u32 bit round-trip
#include <ranges>    // std::views::iota — a node-index range to search by stable id

namespace crd::chir
{
namespace
{
using crd::containers::Array;
using crd::containers::StringView;

// ── float <-> raw u32 bits (exact round-trip) ──
inline crd::u32 f32_bits(crd::f32 v) noexcept { return std::bit_cast<crd::u32>(v); }
inline crd::f32 bits_f32(crd::u32 b) noexcept { return std::bit_cast<crd::f32>(b); }

// ── printer primitives ──
void put(Array<char>& o, StringView s)
{
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        o.push_back(s[i]);
    }
}
void put(Array<char>& o, char c) { o.push_back(c); }
void put_hex(Array<char>& o, crd::u64 v, crd::u32 width)
{
    char buf[16];
    for (crd::u32 i = 0; i < width; ++i)
    {
        const crd::u32 nyb = static_cast<crd::u32>((v >> ((width - 1U - i) * 4U)) & 0xFULL);
        buf[i]             = static_cast<char>(nyb < 10U ? ('0' + nyb) : ('a' + (nyb - 10U)));
    }
    for (crd::u32 i = 0; i < width; ++i)
    {
        o.push_back(buf[i]);
    }
}
void put_dec(Array<char>& o, crd::u64 v)
{
    char     tmp[20];
    crd::u32 n = 0;
    if (v == 0U)
    {
        o.push_back('0');
        return;
    }
    while (v > 0U)
    {
        tmp[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        o.push_back(tmp[n - 1U - i]);
    }
}
void put_name(Array<char>& o, StringView s) { s.size() == 0U ? put(o, StringView("-")) : put(o, s); }

// ── tokenizer (parse.cpp style): whitespace-separated tokens, with the line:col of each ──
// The document is line-oriented: `next` finds a record keyword on any later line, `field` reads the record's next field
// and refuses to leave the record's line. Both leave the 1-based position of what they found (or, for a missing field,
// the column just past the record's last token) in `tok_line`/`tok_col`.
struct Tok
{
    const char* p;
    const char* end;
    const char* line_start;
    const char* last_end;
    crd::u32    line     = 1U;
    crd::u32    tok_line = 1U;
    crd::u32    tok_col  = 1U;
    explicit Tok(StringView t) : p(t.data()), end(t.data() + t.size()), line_start(t.data()), last_end(t.data()) {}
    static bool is_blank(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }
    void        mark(const char* at) noexcept
    {
        tok_line = line;
        tok_col  = static_cast<crd::u32>(at - line_start) + 1U;
    }
    bool take(StringView& out) noexcept
    {
        mark(p);
        const char* s = p;
        while (p < end && !is_blank(*p) && *p != '\n')
        {
            ++p;
        }
        last_end = p;
        out      = StringView(s, static_cast<crd::usize>(p - s));
        return true;
    }
    bool next(StringView& out) noexcept
    {
        while (p < end && (is_blank(*p) || *p == '\n'))
        {
            if (*p == '\n')
            {
                ++line;
                line_start = p + 1;
            }
            ++p;
        }
        if (p >= end)
        {
            mark(p);
            return false;
        }
        return take(out);
    }
    bool field(StringView& out) noexcept
    {
        while (p < end && is_blank(*p))
        {
            ++p;
        }
        if (p >= end || *p == '\n')
        {
            mark(last_end); // where the missing field was expected: just past the record's last token
            return false;
        }
        return take(out);
    }
};

// A decimal field that must fit u32 (a node/pin index, a file id, a line, a column, a group).
bool parse_u32_dec(StringView s, crd::u32& out) noexcept
{
    if (s.size() == 0U)
    {
        return false;
    }
    crd::u64 v = 0;
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c < '0' || c > '9')
        {
            return false;
        }
        v = v * 10U + static_cast<crd::u64>(c - '0');
        if (v > 0xFFFFFFFFULL)
        {
            return false;
        }
    }
    out = static_cast<crd::u32>(v);
    return true;
}
// A lowercase hex field of at most `digits` digits (16 for a stable id, 8 for an f32 bit pattern).
bool parse_hex(StringView s, crd::u32 digits, crd::u64& out) noexcept
{
    if (s.size() == 0U || s.size() > digits)
    {
        return false;
    }
    crd::u64 v = 0;
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        crd::u32   d = 0;
        if (c >= '0' && c <= '9')
        {
            d = static_cast<crd::u32>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            d = static_cast<crd::u32>(c - 'a') + 10U;
        }
        else
        {
            return false;
        }
        v = (v << 4U) | d;
    }
    out = v;
    return true;
}

// Where an edge and a layout row were read: add_edge re-sorts edges canonically and set_layout is last-write-wins, so
// the whole-graph checks resolve their offending record through these read-order lists.
struct EdgeAt
{
    crd::u32 line = 0;
    crd::u32 col  = 0;
    Edge     edge;
};
struct LayoutAt
{
    crd::u32 line = 0;
    crd::u32 col  = 0;
    StableId id;
};

SchemaReadResult refused(crd::u32 line, crd::u32 col, const char* msg, crd::u32 node, StableId id) noexcept
{
    SchemaReadResult r;
    r.err_line = line;
    r.err_col  = col;
    r.node     = node;
    r.id       = id;
    r.msg      = StringView(msg);
    return r;
}
// A refusal at the tokenizer's last position, naming an existing node of `m` (or none).
SchemaReadResult refused_at(const Tok& tk, const char* msg, const SourceModel& m, crd::u32 node) noexcept
{
    const StableId id = node < m.node_count() ? m.node(node).id : StableId{};
    return refused(tk.tok_line, tk.tok_col, msg, node < m.node_count() ? node : kInvalidNode, id);
}
StringView unname(StringView s) noexcept { return s == StringView("-") ? StringView("") : s; }
} // namespace

void print_schema(const SourceModel& m, Array<char>& out)
{
    put(out, StringView("chirgraph 1\n"));
    const crd::u32 nc = m.node_count();
    for (crd::u32 i = 0; i < nc; ++i)
    {
        const ChirNode& n = m.node(i);
        put(out, StringView("node "));
        put_dec(out, i);
        put(out, ' ');
        put_hex(out, n.id.value, 16U);
        put(out, ' ');
        put(out, node_kind_name(n.kind));
        put(out, ' ');
        put_name(out, m.str(n.name));
        put(out, ' ');
        if (n.parent == kInvalidNode)
        {
            put(out, StringView("-1"));
        }
        else
        {
            put_dec(out, n.parent);
        }
        put(out, ' ');
        put_dec(out, n.loc.file_id);
        put(out, ' ');
        put_dec(out, n.loc.line);
        put(out, ' ');
        put_dec(out, n.loc.col);
        put(out, '\n');
    }
    for (crd::u32 i = 0; i < nc; ++i)
    {
        const ChirNode& n = m.node(i);
        for (const Pin& p : n.pins)
        {
            put(out, StringView("pin "));
            put_dec(out, i);
            put(out, ' ');
            put(out, p.dir == PinDir::In ? StringView("in") : StringView("out"));
            put(out, ' ');
            put_name(out, m.str(p.name));
            put(out, ' ');
            put_name(out, m.str(p.type));
            put(out, '\n');
        }
    }
    for (crd::u32 i = 0; i < nc; ++i)
    {
        const ChirNode& n = m.node(i);
        for (const Attr& a : n.attrs)
        {
            put(out, StringView("attr "));
            put_dec(out, i);
            put(out, ' ');
            put_name(out, m.str(a.key));
            put(out, ' ');
            put_name(out, m.str(a.val));
            put(out, '\n');
        }
    }
    for (const Edge& e : m.edges())
    {
        put(out, StringView("edge "));
        put_dec(out, e.from_node);
        put(out, ' ');
        put_dec(out, e.from_pin);
        put(out, ' ');
        put_dec(out, e.to_node);
        put(out, ' ');
        put_dec(out, e.to_pin);
        put(out, '\n');
    }
    for (const Layout& l : m.layout())
    {
        put(out, StringView("layout "));
        put_hex(out, l.id.value, 16U);
        put(out, ' ');
        put_hex(out, f32_bits(l.x), 8U);
        put(out, ' ');
        put_hex(out, f32_bits(l.y), 8U);
        put(out, ' ');
        put_dec(out, l.group);
        put(out, '\n');
    }
}

SchemaReadResult read_schema(StringView text, SourceModel& out)
{
    if (out.node_count() != 0U) // must be empty (graceful reject — never a partial/merged model)
    {
        return refused(0U, 0U, "the model to read into is not empty", kInvalidNode, StableId{});
    }
    Tok        tk(text);
    StringView t;
    // header
    if (!tk.next(t) || t != StringView("chirgraph"))
    {
        return refused_at(tk, "the document does not start with the chirgraph header", out, kInvalidNode);
    }
    if (!tk.field(t) || t != StringView("1"))
    {
        return refused_at(tk, "the chirgraph header names no supported version (1)", out, kInvalidNode);
    }

    Array<EdgeAt>   edges_at(out.allocator());
    Array<LayoutAt> layouts_at(out.allocator());
    crd::u32        expected_node = 0;            // node lines must be dense + in index order (0,1,2,...)
    crd::u32        record_line   = tk.tok_line;  // the line of the record read last (the header first)
    crd::u32        record_node   = kInvalidNode; // the node that record named, for a refused trailing token
    while (tk.next(t))
    {
        if (tk.tok_line == record_line)
        {
            return refused_at(tk, "a token follows the last field of its record", out, record_node);
        }
        record_line            = tk.tok_line;
        record_node            = kInvalidNode;
        const crd::u32 rec_col = tk.tok_col;
        StringView     f;
        if (t == StringView("node"))
        {
            crd::u32 idx = 0;
            if (!tk.field(f) || !parse_u32_dec(f, idx) || idx != expected_node)
            {
                return refused_at(tk, "a node record's index is not the next node index", out, kInvalidNode);
            }
            // Past the index, a refusal names the node this record declares (not yet in the model).
            crd::u64 idv = 0;
            if (!tk.field(f) || !parse_hex(f, 16U, idv))
            {
                return refused(tk.tok_line, tk.tok_col, "a node record has no stable id (16 hex digits)", idx,
                               StableId{});
            }
            const StableId id{idv};
            NodeKind       kind{};
            if (!tk.field(f) || !node_kind_from_name(f, kind))
            {
                return refused(tk.tok_line, tk.tok_col, "a node record names no known node kind", idx, id);
            }
            StringView name_s;
            if (!tk.field(name_s))
            {
                return refused(tk.tok_line, tk.tok_col, "a node record has no name", idx, id);
            }
            crd::u32 parent = kInvalidNode;
            if (!tk.field(f))
            {
                return refused(tk.tok_line, tk.tok_col, "a node record has no parent", idx, id);
            }
            if (f != StringView("-1") && (!parse_u32_dec(f, parent) || parent >= expected_node))
            {
                return refused(tk.tok_line, tk.tok_col, "a node's parent is not an earlier node", idx, id);
            }
            crd::u32 loc[3] = {0U, 0U, 0U}; // file, line, col
            for (crd::u32& v : loc)
            {
                if (!tk.field(f) || !parse_u32_dec(f, v))
                {
                    return refused(tk.tok_line, tk.tok_col, "a node record has no decimal file, line or column", idx,
                                   id);
                }
            }
            const crd::u32 ni = out.add_node(kind, unname(name_s), parent, SourceLoc{loc[0], loc[1], loc[2]});
            out.set_node_id(ni, id);
            record_node = ni;
            ++expected_node;
        }
        else if (t == StringView("pin"))
        {
            crd::u32 nv = 0;
            if (!tk.field(f) || !parse_u32_dec(f, nv) || nv >= out.node_count())
            {
                return refused_at(tk, "a pin record names no existing node", out, kInvalidNode);
            }
            record_node = nv;
            PinDir dir{};
            if (!tk.field(f))
            {
                return refused_at(tk, "a pin record has no direction", out, nv);
            }
            if (f == StringView("in"))
            {
                dir = PinDir::In;
            }
            else if (f == StringView("out"))
            {
                dir = PinDir::Out;
            }
            else
            {
                return refused_at(tk, "a pin direction is neither in nor out", out, nv);
            }
            StringView name_s;
            StringView type_s;
            if (!tk.field(name_s) || !tk.field(type_s))
            {
                return refused_at(tk, "a pin record has no name or type", out, nv);
            }
            out.add_pin(nv, dir, unname(name_s), unname(type_s));
        }
        else if (t == StringView("attr"))
        {
            crd::u32 nv = 0;
            if (!tk.field(f) || !parse_u32_dec(f, nv) || nv >= out.node_count())
            {
                return refused_at(tk, "an attr record names no existing node", out, kInvalidNode);
            }
            record_node = nv;
            StringView key_s;
            StringView val_s;
            if (!tk.field(key_s) || !tk.field(val_s))
            {
                return refused_at(tk, "an attr record has no key or value", out, nv);
            }
            out.add_attr(nv, unname(key_s), unname(val_s));
        }
        else if (t == StringView("edge"))
        {
            crd::u32 v[4] = {0U, 0U, 0U, 0U}; // from node, from pin, to node, to pin
            for (crd::u32 k = 0; k < 4U; ++k)
            {
                if (!tk.field(f) || !parse_u32_dec(f, v[k]))
                {
                    return refused_at(tk, "an edge record has no decimal node or pin", out, kInvalidNode);
                }
                if ((k == 0U || k == 2U) && v[k] >= out.node_count())
                {
                    return refused_at(tk, "an edge endpoint names no existing node", out, kInvalidNode);
                }
            }
            const Edge e{v[0], v[1], v[2], v[3]};
            record_node = e.to_node;
            // Single writer: an in-pin has exactly one source. The later edge into an in-pin that is already fed is the
            // offending record (the pin-direction check cannot see it; the 32c parity anchor relies on it).
            for (const EdgeAt& prior : edges_at)
            {
                if (prior.edge.to_node == e.to_node && prior.edge.to_pin == e.to_pin)
                {
                    return refused(record_line, rec_col, "a second edge feeds the same in-pin", e.to_node,
                                   out.node(e.to_node).id);
                }
            }
            out.add_edge(e.from_node, e.from_pin, e.to_node, e.to_pin);
            edges_at.push_back(EdgeAt{record_line, rec_col, e});
        }
        else if (t == StringView("layout"))
        {
            crd::u64 idv = 0;
            crd::u64 xb  = 0;
            crd::u64 yb  = 0;
            crd::u32 g   = 0;
            if (!tk.field(f) || !parse_hex(f, 16U, idv))
            {
                return refused_at(tk, "a layout record has no stable id (16 hex digits)", out, kInvalidNode);
            }
            if (!tk.field(f) || !parse_hex(f, 8U, xb) || !tk.field(f) || !parse_hex(f, 8U, yb))
            {
                return refused_at(tk, "a layout record has no x or y bit pattern (8 hex digits)", out, kInvalidNode);
            }
            if (!tk.field(f) || !parse_u32_dec(f, g))
            {
                return refused_at(tk, "a layout record has no decimal group", out, kInvalidNode);
            }
            out.set_layout(StableId{idv}, bits_f32(static_cast<crd::u32>(xb)), bits_f32(static_cast<crd::u32>(yb)), g);
            layouts_at.push_back(LayoutAt{record_line, rec_col, StableId{idv}});
        }
        else
        {
            return refused_at(tk, "an unknown record type", out, kInvalidNode);
        }
    }

    // ── validate the assembled graph (graceful reject on an authoring error the tokenizer alone cannot see) ──
    // Node/pin/attr endpoints were range-checked as they were read; the remaining invariants need the WHOLE graph:
    //   (1) every layout row resolves to a real node id (no orphaned side-table row after a node was removed/renamed);
    //   (2) every edge connects a real OUT pin to a real IN pin (the classic graph-authoring error: wiring In->In).
    // Each is reported at its first offending record in document order.
    const crd::u32 nc       = out.node_count();
    const auto     node_idx = std::views::iota(crd::u32{0}, nc);
    for (const LayoutAt& l : layouts_at)
    {
        if (!std::ranges::any_of(node_idx, [&](crd::u32 i) { return out.node(i).id == l.id; }))
        {
            SchemaReadResult r = refused(l.line, l.col, "a layout row names no node", kInvalidNode, l.id);
            r.orphan           = true;
            return r;
        }
    }
    for (const EdgeAt& a : edges_at)
    {
        const ChirNode& from = out.node(a.edge.from_node);
        const ChirNode& to   = out.node(a.edge.to_node);
        if (a.edge.from_pin >= from.pins.size() || from.pins[a.edge.from_pin].dir != PinDir::Out)
        {
            return refused(a.line, a.col, "an edge's source is not an out pin of its node", a.edge.from_node, from.id);
        }
        if (a.edge.to_pin >= to.pins.size() || to.pins[a.edge.to_pin].dir != PinDir::In)
        {
            return refused(a.line, a.col, "an edge's target is not an in pin of its node", a.edge.to_node, to.id);
        }
    }
    SchemaReadResult ok;
    ok.ok = true;
    return ok;
}

} // namespace crd::chir
