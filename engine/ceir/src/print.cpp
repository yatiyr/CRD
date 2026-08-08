#include <crd/ceir/print.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/containers/hash_map.hpp>

#include <charconv> // std::to_chars — allocation-free int/float formatting (float form is shortest-round-trippable)

namespace crd::ceir
{
namespace
{
constexpr u32 kMaxAttrsInline = 64U; // an op with more attrs than this is pathological; sort spills are truncated-safe

[[nodiscard]] bool sv_less(containers::StringView a, containers::StringView b) noexcept
{
    const usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; ++i)
    {
        if (a[i] != b[i]) { return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[i]); }
    }
    return a.size() < b.size();
}

class Printer
{
public:
    Printer(Context& ctx, containers::String& out) : m_ctx(ctx), m_out(out), m_ids(ctx.allocator()) {}

    void run(const Module& module)
    {
        assign_ids(module.body());
        m_out.append("module ");
        emit_region(module.body(), 0U);
        m_out.push_back('\n');
    }

private:
    // ── pass 1: deterministic SSA value numbering (a fixed pre-order walk) ──
    void assign_ids(Region* r)
    {
        if (r == nullptr) { return; }
        for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
        {
            for (u32 i = 0; i < b->num_args(); ++i) { m_ids.insert(b->arg(i), m_next++); }
            for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
            {
                for (u32 i = 0; i < op->num_results(); ++i) { m_ids.insert(op->result(i), m_next++); }
                for (u32 i = 0; i < op->num_regions(); ++i) { assign_ids(op->region(i)); }
            }
        }
    }

    // ── pass 2: emit ──
    void emit_region(Region* r, u32 depth)
    {
        m_out.append("{\n");
        u32 bi = 0U;
        for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region()) { emit_block(b, depth + 1U, bi++); }
        indent(depth);
        m_out.push_back('}');
    }

    void emit_block(Block* b, u32 depth, u32 block_idx)
    {
        indent(depth);
        m_out.append("^bb");
        emit_u32(block_idx);
        if (b->num_args() > 0U)
        {
            m_out.push_back('(');
            for (u32 i = 0; i < b->num_args(); ++i)
            {
                if (i != 0U) { m_out.append(", "); }
                emit_value_def(b->arg(i));
            }
            m_out.push_back(')');
        }
        m_out.append(":\n");
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block()) { emit_op(op, depth + 1U); }
    }

    void emit_op(Operation* op, u32 depth)
    {
        indent(depth);
        if (op->num_results() > 0U)
        {
            for (u32 i = 0; i < op->num_results(); ++i)
            {
                if (i != 0U) { m_out.append(", "); }
                m_out.push_back('%');
                emit_u32(*m_ids.find(op->result(i)));
            }
            m_out.append(" = ");
        }
        const containers::StringView name = m_ctx.op_name(op->kind()); // "dialect.op" — works for unknown dialects too
        m_out.append(name.data(), name.size());
        m_out.push_back('(');
        for (u32 i = 0; i < op->num_operands(); ++i)
        {
            if (i != 0U) { m_out.append(", "); }
            emit_value_ref(op->operand(i));
        }
        m_out.push_back(')');
        emit_attrs(op);
        if (op->num_results() > 0U && op->result(0)->type().valid()) // single result-type (create_operation shape)
        {
            m_out.append(" : !t");
            emit_u32(op->result(0)->type().value);
        }
        for (u32 i = 0; i < op->num_regions(); ++i)
        {
            m_out.push_back(' ');
            emit_region(op->region(i), depth);
        }
        m_out.push_back('\n');
    }

    void emit_value_def(Value* v)
    {
        m_out.push_back('%');
        emit_u32(*m_ids.find(v));
        if (v->type().valid())
        {
            m_out.append(" : !t");
            emit_u32(v->type().value);
        }
    }

    void emit_value_ref(Value* v)
    {
        m_out.push_back('%');
        const u32* const id = m_ids.find(v);
        emit_u32(id != nullptr ? *id : 0U); // always found for well-formed IR (defs precede uses in the walk)
    }

    void emit_attrs(Operation* op)
    {
        const u32 n = op->num_attrs();
        if (n == 0U) { return; }
        const u32 m = n < kMaxAttrsInline ? n : kMaxAttrsInline;
        u32       order[kMaxAttrsInline];
        for (u32 i = 0; i < m; ++i) { order[i] = i; }
        for (u32 i = 1; i < m; ++i) // insertion sort by attr name → canonical (position-independent) output
        {
            const u32 v = order[i];
            u32       j = i;
            while (j > 0U && sv_less(op->attr_name(v), op->attr_name(order[j - 1U])))
            {
                order[j] = order[j - 1U];
                --j;
            }
            order[j] = v;
        }
        m_out.append(" {");
        for (u32 i = 0; i < m; ++i)
        {
            if (i != 0U) { m_out.append(", "); }
            const containers::StringView an = op->attr_name(order[i]);
            m_out.append(an.data(), an.size());
            m_out.append(" = ");
            emit_attr_value(m_ctx.attr_value(op->attr_id_at(order[i])));
        }
        m_out.push_back('}');
    }

    void emit_attr_value(const AttrValue& v)
    {
        switch (v.kind)
        {
        case AttrKind::Int: emit_i64(v.i); break;
        case AttrKind::Float: emit_f64(v.as_float()); break;
        case AttrKind::Bool: m_out.append(v.b ? "true" : "false"); break;
        case AttrKind::String: emit_quoted(v.s); break;
        case AttrKind::SymbolRef:
            m_out.push_back('@');
            m_out.append(v.s.data(), v.s.size());
            break;
        case AttrKind::Type:
            m_out.append("!t");
            emit_u32(v.t.value);
            break;
        }
    }

    void emit_quoted(containers::StringView s)
    {
        m_out.push_back('"');
        for (usize i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '"' || c == '\\') { m_out.push_back('\\'); }
            m_out.push_back(c);
        }
        m_out.push_back('"');
    }

    void emit_u32(u32 x)
    {
        char       buf[16];
        const auto res = std::to_chars(buf, buf + sizeof(buf), x);
        m_out.append(buf, static_cast<usize>(res.ptr - buf));
    }
    void emit_i64(i64 x)
    {
        char       buf[24];
        const auto res = std::to_chars(buf, buf + sizeof(buf), x);
        m_out.append(buf, static_cast<usize>(res.ptr - buf));
    }
    void emit_f64(f64 x)
    {
        char       buf[40];
        const auto res  = std::to_chars(buf, buf + sizeof(buf), x); // shortest round-trippable decimal
        const auto used = static_cast<usize>(res.ptr - buf);
        m_out.append(buf, used);
        bool marked = false; // ensure it re-reads as a float, not an int (append ".0" to e.g. "4")
        for (usize i = 0; i < used; ++i)
        {
            const char c = buf[i];
            if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'i') { marked = true; break; } // nan/inf too
        }
        if (!marked) { m_out.append(".0"); }
    }

    void indent(u32 depth)
    {
        for (u32 i = 0; i < depth * 2U; ++i) { m_out.push_back(' '); }
    }

    Context&                              m_ctx;
    containers::String&                   m_out;
    containers::HashMap<Value*, u32>      m_ids;
    u32                                   m_next = 0U;
};
} // namespace

void print(Context& ctx, const Module& module, containers::String& out)
{
    Printer p(ctx, out);
    p.run(module);
}

containers::String print(Context& ctx, const Module& module, memory::IAllocator* alloc)
{
    containers::String out(alloc);
    print(ctx, module, out);
    return out;
}
} // namespace crd::ceir
