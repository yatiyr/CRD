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
    for (crd::usize i = 0; i < s.size(); ++i) { o.push_back(s[i]); }
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
    for (crd::u32 i = 0; i < width; ++i) { o.push_back(buf[i]); }
}
void put_dec(Array<char>& o, crd::u64 v)
{
    char     tmp[20];
    crd::u32 n = 0;
    if (v == 0U) { o.push_back('0'); return; }
    while (v > 0U) { tmp[n++] = static_cast<char>('0' + (v % 10U)); v /= 10U; }
    for (crd::u32 i = 0; i < n; ++i) { o.push_back(tmp[n - 1U - i]); }
}
void put_name(Array<char>& o, StringView s) { s.size() == 0U ? put(o, StringView("-")) : put(o, s); }

// ── tokenizer (parse.cpp style): whitespace-separated tokens ──
struct Tok
{
    StringView   text;
    const char*  p;
    const char*  end;
    explicit Tok(StringView t) : text(t), p(t.data()), end(t.data() + t.size()) {}
    static bool  is_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    bool         next(StringView& out) noexcept
    {
        while (p < end && is_ws(*p)) { ++p; }
        if (p >= end) { return false; }
        const char* s = p;
        while (p < end && !is_ws(*p)) { ++p; }
        out = StringView(s, static_cast<crd::usize>(p - s));
        return true;
    }
};

bool parse_u64_dec(StringView s, crd::u64& out) noexcept
{
    if (s.size() == 0U) { return false; }
    crd::u64 v = 0;
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c < '0' || c > '9') { return false; }
        const crd::u32 dig = static_cast<crd::u32>(c - '0');
        v                  = v * 10U + dig;
    }
    out = v;
    return true;
}
bool parse_u64_hex(StringView s, crd::u64& out) noexcept
{
    if (s.size() == 0U) { return false; }
    crd::u64 v = 0;
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        crd::u32   d = 0;
        if (c >= '0' && c <= '9') { d = static_cast<crd::u32>(c - '0'); }
        else if (c >= 'a' && c <= 'f') { d = static_cast<crd::u32>(c - 'a') + 10U; }
        else { return false; }
        v = (v << 4U) | d;
    }
    out = v;
    return true;
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
        if (n.parent == kInvalidNode) { put(out, StringView("-1")); }
        else { put_dec(out, n.parent); }
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

bool read_schema(StringView text, SourceModel& out)
{
    if (out.node_count() != 0U) { return false; } // must be empty (graceful reject — never a partial/merged model)
    Tok        tk(text);
    StringView t;
    // header
    if (!tk.next(t) || t != StringView("chirgraph")) { return false; }
    if (!tk.next(t) || t != StringView("1")) { return false; }

    crd::u32 expected_node = 0; // node lines must be dense + in index order (0,1,2,...)
    while (tk.next(t))
    {
        if (t == StringView("node"))
        {
            StringView idx_s;
            StringView id_s;
            StringView kind_s;
            StringView name_s;
            StringView par_s;
            StringView file_s;
            StringView line_s;
            StringView col_s;
            if (!tk.next(idx_s) || !tk.next(id_s) || !tk.next(kind_s) || !tk.next(name_s) || !tk.next(par_s) ||
                !tk.next(file_s) || !tk.next(line_s) || !tk.next(col_s))
            {
                return false;
            }
            crd::u64 idx = 0;
            crd::u64 idv = 0;
            crd::u64 fil = 0;
            crd::u64 lin = 0;
            crd::u64 col = 0;
            if (!parse_u64_dec(idx_s, idx) || idx != expected_node) { return false; }
            if (!parse_u64_hex(id_s, idv)) { return false; }
            NodeKind kind{};
            if (!node_kind_from_name(kind_s, kind)) { return false; }
            if (!parse_u64_dec(file_s, fil) || !parse_u64_dec(line_s, lin) || !parse_u64_dec(col_s, col)) { return false; }
            crd::u32 parent = kInvalidNode;
            if (par_s != StringView("-1"))
            {
                crd::u64 pv = 0;
                if (!parse_u64_dec(par_s, pv) || pv >= expected_node) { return false; } // parent must precede (dense order)
                parent = static_cast<crd::u32>(pv);
            }
            const SourceLoc loc{static_cast<crd::u32>(fil), static_cast<crd::u32>(lin), static_cast<crd::u32>(col)};
            const crd::u32  ni = out.add_node(kind, unname(name_s), parent, loc);
            out.set_node_id(ni, StableId{idv});
            ++expected_node;
        }
        else if (t == StringView("pin"))
        {
            StringView node_s;
            StringView dir_s;
            StringView name_s;
            StringView type_s;
            if (!tk.next(node_s) || !tk.next(dir_s) || !tk.next(name_s) || !tk.next(type_s)) { return false; }
            crd::u64 nv = 0;
            if (!parse_u64_dec(node_s, nv) || nv >= out.node_count()) { return false; }
            PinDir dir{};
            if (dir_s == StringView("in")) { dir = PinDir::In; }
            else if (dir_s == StringView("out")) { dir = PinDir::Out; }
            else { return false; }
            out.add_pin(static_cast<crd::u32>(nv), dir, unname(name_s), unname(type_s));
        }
        else if (t == StringView("attr"))
        {
            StringView node_s;
            StringView key_s;
            StringView val_s;
            if (!tk.next(node_s) || !tk.next(key_s) || !tk.next(val_s)) { return false; }
            crd::u64 nv = 0;
            if (!parse_u64_dec(node_s, nv) || nv >= out.node_count()) { return false; }
            out.add_attr(static_cast<crd::u32>(nv), unname(key_s), unname(val_s));
        }
        else if (t == StringView("edge"))
        {
            StringView fn_s;
            StringView fp_s;
            StringView tn_s;
            StringView tp_s;
            if (!tk.next(fn_s) || !tk.next(fp_s) || !tk.next(tn_s) || !tk.next(tp_s)) { return false; }
            crd::u64 fn = 0;
            crd::u64 fp = 0;
            crd::u64 tn = 0;
            crd::u64 tp = 0;
            if (!parse_u64_dec(fn_s, fn) || !parse_u64_dec(fp_s, fp) || !parse_u64_dec(tn_s, tn) ||
                !parse_u64_dec(tp_s, tp))
            {
                return false;
            }
            if (fn >= out.node_count() || tn >= out.node_count()) { return false; }
            out.add_edge(static_cast<crd::u32>(fn), static_cast<crd::u32>(fp), static_cast<crd::u32>(tn),
                         static_cast<crd::u32>(tp));
        }
        else if (t == StringView("layout"))
        {
            StringView id_s;
            StringView x_s;
            StringView y_s;
            StringView g_s;
            if (!tk.next(id_s) || !tk.next(x_s) || !tk.next(y_s) || !tk.next(g_s)) { return false; }
            crd::u64 idv = 0;
            crd::u64 xb  = 0;
            crd::u64 yb  = 0;
            crd::u64 g   = 0;
            if (!parse_u64_hex(id_s, idv) || !parse_u64_hex(x_s, xb) || !parse_u64_hex(y_s, yb) || !parse_u64_dec(g_s, g))
            {
                return false;
            }
            out.set_layout(StableId{idv}, bits_f32(static_cast<crd::u32>(xb)), bits_f32(static_cast<crd::u32>(yb)),
                           static_cast<crd::u32>(g));
        }
        else
        {
            return false; // an unknown record type
        }
    }

    // ── validate the assembled graph (graceful reject on an authoring error the tokenizer alone cannot see) ──
    // Node/pin/attr endpoints were range-checked as they were read; the remaining invariants need the WHOLE graph:
    //   (1) every layout row resolves to a real node id (no orphaned side-table row after a node was removed/renamed);
    //   (2) every edge connects a real OUT pin to a real IN pin (the classic graph-authoring error: wiring In->In).
    const crd::u32 nc       = out.node_count();
    const auto     node_idx = std::views::iota(crd::u32{0}, nc);
    for (const Layout& l : out.layout())
    {
        // an orphaned layout row (no node carries this id) => reject.
        if (!std::ranges::any_of(node_idx, [&](crd::u32 i) { return out.node(i).id == l.id; })) { return false; }
    }
    // every edge must connect a real OUT pin to a real IN pin (endpoints were range-checked as node indices as read).
    const bool edges_ok = std::ranges::all_of(out.edges(), [&](const Edge& e) {
        return e.from_pin < out.node(e.from_node).pins.size() && e.to_pin < out.node(e.to_node).pins.size() &&
               out.node(e.from_node).pins[e.from_pin].dir == PinDir::Out && // from-pin must be an OUTPUT
               out.node(e.to_node).pins[e.to_pin].dir == PinDir::In;        // to-pin must be an INPUT
    });
    if (!edges_ok) { return false; }

    // single-writer: an in-pin has EXACTLY one source. Canonical edge order (SourceModel::add_edge sorts by the consumer
    // key) puts any duplicate (to_node, to_pin) ADJACENT, so one adjacent-scan rejects a graph wiring two sources into
    // one in-pin (an authoring error the pin-direction check cannot see; the 32c parity anchor relies on single-writer).
    if (std::ranges::adjacent_find(out.edges(), [](const Edge& a, const Edge& b) {
            return a.to_node == b.to_node && a.to_pin == b.to_pin;
        }) != out.edges().end())
    {
        return false;
    }
    return true;
}

} // namespace crd::chir
