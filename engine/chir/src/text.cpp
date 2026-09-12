// text.cpp — the CHIR TEXT projection: print_chir + parse_chir (CEIR-32c, ADR-0128 D5). See text.hpp.
//
// GRAMMAR (canonical form; whitespace is insignificant on parse, 2-space indent on print). One uniform node form covers
// all 7 kinds — kind, optional name, attrs, dir+name+type pins with an optional in-pin source binding, a child block.
// ⛔ Edge sources are referenced by NAME, so they must be UNIQUELY-NAMED nodes (a duplicate name is rejected at parse, an
// anonymous source is unrepresentable) — a text-projection restriction; the model stays lossless for such programs:
//
//   node    := KIND [NAME] [ '[' attr (',' attr)* ']' ] [ '(' pin (',' pin)* ')' ] [ '{' node* '}' ]
//   attr    := IDENT '=' IDENT
//   pin     := ('in'|'out') IDENT ':' IDENT [ '=' PINREF ]      -- '=' src only on an 'in' pin (the incoming edge)
//   PINREF  := IDENT                                            -- split at the LAST '.': <node-name> '.' <pin-name>
//   KIND    := one of program|event_handler|state_decl|query|parallel_for|await|state_update
//
// ⛔ IDENTs allow '.' (so an attr value like `Position.Velocity` is bare — no quoting machinery); a NODE NAME is
// DOT-FREE (a PINREF splits at its last dot into a dot-free node name + a dot-free pin name). A NAME that spells a KIND
// keyword is not a name — it begins the next sibling (so `await await` is two anonymous awaits, not one named `await`).
// Pre-order emit + pre-order parse reproduce the oracle's node indices exactly, so parse_chir's final derive_ids()
// yields the committed stable ids. Edges are bound at the CONSUMER in-pin, so they arrive in canonical (to_node,to_pin)
// order — SourceModel::add_edge keeps that canonical, making semantic_hash identical across the text + graph projections.

#include <crd/chir/text.hpp>

namespace crd::chir
{
namespace
{
using crd::containers::Array;
using crd::containers::StringView;

[[nodiscard]] bool is_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
[[nodiscard]] bool is_ident_start(char c) noexcept { return is_alpha(c) || c == '_'; }
// IDENT chars include '.' — a dotted attr value/type is bare; a PINREF is one IDENT split at its last dot (node names
// are dot-free, so the split is unambiguous). Mirrors ceir parse.cpp's is_ident_char.
[[nodiscard]] bool is_ident_char(char c) noexcept { return is_alpha(c) || is_digit(c) || c == '_' || c == '.'; }
[[nodiscard]] bool has_dot(StringView s) noexcept
{
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        if (s[i] == '.') { return true; }
    }
    return false;
}

// ── printer ──
void put(Array<char>& o, StringView s)
{
    for (crd::usize i = 0; i < s.size(); ++i) { o.push_back(s[i]); }
}
void put(Array<char>& o, char c) { o.push_back(c); }
void put_indent(Array<char>& o, crd::u32 depth)
{
    for (crd::u32 i = 0; i < depth * 2U; ++i) { o.push_back(' '); }
}

class ChirPrinter
{
public:
    ChirPrinter(const SourceModel& m, Array<char>& out) : m_m(m), m_out(out) {}

    void emit(crd::u32 idx, crd::u32 depth)
    {
        const ChirNode& n = m_m.node(idx);
        put_indent(m_out, depth);
        put(m_out, node_kind_name(n.kind));
        if (n.name.len != 0U)
        {
            put(m_out, ' ');
            put(m_out, m_m.str(n.name));
        }
        if (n.attrs.size() != 0U)
        {
            put(m_out, StringView(" ["));
            for (crd::u32 i = 0; i < n.attrs.size(); ++i)
            {
                if (i != 0U) { put(m_out, StringView(", ")); }
                put(m_out, m_m.str(n.attrs[i].key));
                put(m_out, '=');
                put(m_out, m_m.str(n.attrs[i].val));
            }
            put(m_out, ']');
        }
        if (n.pins.size() != 0U)
        {
            put(m_out, StringView(" ("));
            for (crd::u32 i = 0; i < n.pins.size(); ++i)
            {
                if (i != 0U) { put(m_out, StringView(", ")); }
                emit_pin(idx, n.pins[i], i);
            }
            put(m_out, ')');
        }
        if (n.children.size() != 0U)
        {
            put(m_out, StringView(" {\n"));
            for (const crd::u32 c : n.children) { emit(c, depth + 1U); }
            put_indent(m_out, depth);
            put(m_out, StringView("}\n"));
        }
        else
        {
            put(m_out, '\n');
        }
    }

private:
    void emit_pin(crd::u32 node_idx, const Pin& p, crd::u32 pin_idx)
    {
        put(m_out, p.dir == PinDir::In ? StringView("in") : StringView("out"));
        put(m_out, ' ');
        put(m_out, m_m.str(p.name));
        put(m_out, StringView(": "));
        put(m_out, m_m.str(p.type));
        if (p.dir != PinDir::In) { return; }
        // an incoming edge binds this in-pin: emit ` = <src-node-name>.<src-pin-name>` (single-writer => at most one).
        for (const Edge& e : m_m.edges())
        {
            if (e.to_node == node_idx && e.to_pin == pin_idx)
            {
                put(m_out, StringView(" = "));
                put(m_out, m_m.str(m_m.node(e.from_node).name)); // ⛔ src must be NAMED (anonymous source = deferral)
                put(m_out, '.');
                put(m_out, m_m.str(m_m.node(e.from_node).pins[e.from_pin].name));
                return;
            }
        }
    }

    const SourceModel& m_m;
    Array<char>&       m_out;
};

// ── parser ──
class ChirParser
{
public:
    ChirParser(StringView text, crd::u32 file_id, SourceModel& out)
        : m_model(out), m_file(file_id), m_begin(text.data()), m_cur(text.data()), m_end(text.data() + text.size()),
          m_pending(out.allocator())
    {
    }

    ChirParseResult run()
    {
        const crd::u32 root = parse_node(kInvalidNode);
        if (m_ok)
        {
            skip_ws();
            if (m_cur != m_end) { fail("trailing characters after the program"); }
        }
        if (m_ok && (root == kInvalidNode || m_model.node(root).kind != NodeKind::Program))
        {
            fail_at(0U, "the root node must be a program");
        }
        if (m_ok) { resolve_edges(); }
        if (m_ok) { m_model.derive_ids(); }
        return make_result();
    }

private:
    // ── cursor primitives (mirror ceir parse.cpp) ──
    [[nodiscard]] crd::u32 offset() const noexcept { return static_cast<crd::u32>(m_cur - m_begin); }
    void                   skip_ws() noexcept
    {
        while (m_cur < m_end && is_ws(*m_cur)) { ++m_cur; }
    }
    [[nodiscard]] char la() noexcept
    {
        skip_ws();
        return m_cur < m_end ? *m_cur : '\0';
    }
    bool accept(char c) noexcept
    {
        if (la() == c)
        {
            ++m_cur;
            return true;
        }
        return false;
    }
    void expect(char c, const char* msg) noexcept
    {
        if (!accept(c)) { fail(msg); }
    }
    void fail(const char* msg) noexcept { fail_at(offset(), msg); }
    void fail_at(crd::u32 off, const char* msg) noexcept
    {
        if (m_ok) // latch the FIRST error only — later cascades carry no information
        {
            m_ok      = false;
            m_err_off = off;
            m_err     = msg;
        }
    }

    [[nodiscard]] StringView parse_ident(const char* msg) noexcept
    {
        skip_ws();
        if (m_cur >= m_end || !is_ident_start(*m_cur))
        {
            fail(msg);
            return {};
        }
        const char* s = m_cur;
        while (m_cur < m_end && is_ident_char(*m_cur)) { ++m_cur; }
        return StringView(s, static_cast<crd::usize>(m_cur - s));
    }

    [[nodiscard]] SourceLoc loc_at(crd::u32 off) const noexcept
    {
        crd::u32 line = 1;
        crd::u32 col  = 1;
        for (crd::u32 i = 0; i < off; ++i)
        {
            if (m_begin[i] == '\n')
            {
                ++line;
                col = 1;
            }
            else { ++col; }
        }
        return SourceLoc{m_file, line, col};
    }

    crd::u32 parse_node(crd::u32 parent) noexcept
    {
        skip_ws();
        const crd::u32   koff = offset();
        const StringView kw   = parse_ident("expected a node kind");
        if (!m_ok) { return kInvalidNode; }
        NodeKind kind{};
        if (!node_kind_from_name(kw, kind))
        {
            fail_at(koff, "unknown node kind");
            return kInvalidNode;
        }
        // optional NAME: an ident that does NOT spell a kind keyword (a keyword there begins the next sibling instead,
        // so this node is anonymous). A name must be dot-free (a dot belongs to a PINREF split, never a name).
        StringView name;
        skip_ws();
        if (m_cur < m_end && is_ident_start(*m_cur))
        {
            const char* const save = m_cur;
            const crd::u32    noff = offset();
            const StringView  cand = parse_ident("expected a node name");
            NodeKind          sib{};
            if (node_kind_from_name(cand, sib)) { m_cur = save; } // it is a sibling's kind -> this node is anonymous
            else if (has_dot(cand))
            {
                fail_at(noff, "a node name must not contain '.'");
                return kInvalidNode;
            }
            else { name = cand; }
        }
        const crd::u32 idx = m_model.add_node(kind, name, parent, loc_at(koff));
        if (la() == '[') { parse_attrs(idx); }
        if (m_ok && la() == '(') { parse_pins(idx); }
        if (m_ok && la() == '{') { parse_block(idx); }
        return idx;
    }

    void parse_attrs(crd::u32 idx) noexcept
    {
        expect('[', "expected '['");
        if (la() == ']')
        {
            fail("an empty attribute list");
            return;
        }
        do {
            const StringView k = parse_ident("expected an attribute name");
            expect('=', "expected '=' in an attribute");
            const StringView v = parse_ident("expected an attribute value");
            if (!m_ok) { return; }
            m_model.add_attr(idx, k, v);
        } while (accept(','));
        expect(']', "expected ']'");
    }

    void parse_pins(crd::u32 idx) noexcept
    {
        expect('(', "expected '('");
        if (la() == ')')
        {
            fail("an empty pin list");
            return;
        }
        do {
            const StringView dirs = parse_ident("expected 'in' or 'out'");
            if (!m_ok) { return; }
            PinDir dir{};
            if (dirs == StringView("in")) { dir = PinDir::In; }
            else if (dirs == StringView("out")) { dir = PinDir::Out; }
            else
            {
                fail("a pin direction must be 'in' or 'out'");
                return;
            }
            const StringView pname = parse_ident("expected a pin name");
            expect(':', "expected ':' in a pin");
            const StringView ptype = parse_ident("expected a pin type");
            if (!m_ok) { return; }
            m_model.add_pin(idx, dir, pname, ptype);
            const crd::u32 pin_idx = static_cast<crd::u32>(m_model.node(idx).pins.size()) - 1U;
            if (la() == '=')
            {
                const crd::u32 roff = offset();
                accept('=');
                const StringView ref = parse_ident("expected a pin reference node.pin");
                if (!m_ok) { return; }
                if (dir != PinDir::In)
                {
                    fail_at(roff, "only an 'in' pin can bind a source");
                    return;
                }
                crd::usize dot = ref.size();
                for (crd::usize i = 0; i < ref.size(); ++i)
                {
                    if (ref[i] == '.') { dot = i; }
                }
                if (dot == ref.size())
                {
                    fail_at(roff, "a pin reference must be node.pin");
                    return;
                }
                m_pending.push_back(Pending{StringView(ref.data(), dot),
                                            StringView(ref.data() + dot + 1U, ref.size() - dot - 1U), idx, pin_idx,
                                            roff});
            }
        } while (accept(','));
        expect(')', "expected ')'");
    }

    void parse_block(crd::u32 idx) noexcept
    {
        expect('{', "expected '{'");
        while (m_ok && la() != '}' && la() != '\0') { parse_node(idx); }
        expect('}', "expected '}'");
    }

    void resolve_edges() noexcept
    {
        for (const Pending& p : m_pending)
        {
            // ⛔ an edge source is referenced by NAME, so the name must be UNIQUE — scan ALL nodes (never first-match,
            // which would SILENTLY wire to the wrong duplicate). A text-projection restriction: the graph carries indices.
            crd::u32 fn = kInvalidNode;
            for (crd::u32 i = 0; i < m_model.node_count(); ++i)
            {
                if (m_model.node(i).name.len != 0U && m_model.str(m_model.node(i).name) == p.from_node)
                {
                    if (fn != kInvalidNode)
                    {
                        fail_at(p.off, "ambiguous pin reference: source node name is not unique");
                        return;
                    }
                    fn = i;
                }
            }
            if (fn == kInvalidNode)
            {
                fail_at(p.off, "unresolved pin reference: unknown source node");
                return;
            }
            const ChirNode& src = m_model.node(fn);
            crd::u32        fpi = kInvalidNode;
            for (crd::u32 i = 0; i < src.pins.size(); ++i)
            {
                if (m_model.str(src.pins[i].name) == p.from_pin)
                {
                    fpi = i;
                    break;
                }
            }
            if (fpi == kInvalidNode)
            {
                fail_at(p.off, "unresolved pin reference: unknown source pin");
                return;
            }
            if (src.pins[fpi].dir != PinDir::Out)
            {
                fail_at(p.off, "a pin reference must target an 'out' pin");
                return;
            }
            m_model.add_edge(fn, fpi, p.to_node, p.to_pin);
        }
    }

    [[nodiscard]] ChirParseResult make_result() const noexcept
    {
        ChirParseResult r;
        if (m_ok)
        {
            r.ok = true;
            return r;
        }
        r.ok            = false;
        r.err_off       = m_err_off;
        const SourceLoc l = loc_at(m_err_off);
        r.err_line      = l.line;
        r.err_col       = l.col;
        r.msg           = StringView(m_err);
        return r;
    }

    struct Pending
    {
        StringView from_node;
        StringView from_pin;
        crd::u32   to_node = 0;
        crd::u32   to_pin  = 0;
        crd::u32   off     = 0; // for the diagnostic on an unresolved reference
    };

    SourceModel&    m_model;
    crd::u32        m_file;
    const char*     m_begin;
    const char*     m_cur;
    const char*     m_end;
    bool            m_ok      = true;
    crd::u32        m_err_off = 0;
    const char*     m_err     = "";
    Array<Pending>  m_pending;
};
} // namespace

void print_chir(const SourceModel& m, Array<char>& out)
{
    const crd::u32 root = m.root();
    if (root == kInvalidNode) { return; } // no program => print nothing (a graceful empty projection)
    ChirPrinter(m, out).emit(root, 0U);
}

ChirParseResult parse_chir(StringView text, crd::u32 file_id, SourceModel& out) { return ChirParser(text, file_id, out).run(); }

} // namespace crd::chir
