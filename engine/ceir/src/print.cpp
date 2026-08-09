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
            m_out.append(" : ");
            emit_type(op->result(0)->type());
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
            m_out.append(" : ");
            emit_type(v->type());
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
        case AttrKind::Type: emit_type(v.t); break;
        }
    }

    // ── types (CEIR-3a, §16) — the canonical `!`-sigil grammar; nested types recurse and keep their own sigil ──
    void emit_type(TypeId id)
    {
        const Type t = m_ctx.type_of(id);
        m_out.push_back('!');
        switch (t.kind)
        {
        case TypeKind::Bool: m_out.append("bool"); break;
        case TypeKind::Index: m_out.append("index"); break;
        case TypeKind::Int:
            m_out.push_back(t.is_signed ? 'i' : 'u');
            emit_u32(t.count);
            break;
        case TypeKind::Float: emit_float_kind(t.fkind); break;
        case TypeKind::Vector:
            m_out.append("vec<");
            emit_u32(t.count);
            m_out.push_back('x');
            emit_type(t.members[0]);
            m_out.push_back('>');
            break;
        case TypeKind::Matrix:
            m_out.append("mat<");
            emit_u32(t.count);
            m_out.push_back('x');
            emit_u32(t.cols);
            m_out.push_back('x');
            emit_type(t.members[0]);
            m_out.push_back('>');
            break;
        case TypeKind::Complex:
            m_out.append("complex<");
            emit_type(t.members[0]);
            m_out.push_back('>');
            break;
        case TypeKind::Quaternion:
            m_out.append("quat<");
            emit_type(t.members[0]);
            m_out.push_back('>');
            break;
        case TypeKind::Array:
            m_out.append("array<");
            emit_u32(t.count);
            m_out.push_back('x');
            emit_type(t.members[0]);
            m_out.push_back('>');
            break;
        case TypeKind::Tuple:
            m_out.append("tuple<");
            emit_type_list(t.members);
            m_out.push_back('>');
            break;
        case TypeKind::Struct:
            m_out.append("struct<");
            m_out.append(t.name.data(), t.name.size());
            for (usize i = 0; i < t.members.size(); ++i)
            {
                m_out.push_back(',');
                const containers::StringView fn = t.labels[i];
                m_out.append(fn.data(), fn.size());
                m_out.push_back(':');
                emit_type(t.members[i]);
            }
            m_out.push_back('>');
            break;
        case TypeKind::Enum:
            m_out.append("enum<");
            m_out.append(t.name.data(), t.name.size());
            for (usize i = 0; i < t.labels.size(); ++i)
            {
                m_out.push_back(',');
                const containers::StringView cn = t.labels[i];
                m_out.append(cn.data(), cn.size());
            }
            m_out.push_back('>');
            break;
        case TypeKind::Variant:
            m_out.append("variant<");
            emit_type_list(t.members);
            m_out.push_back('>');
            break;
        case TypeKind::Option:
            m_out.append("option<");
            emit_type(t.members[0]);
            m_out.push_back('>');
            break;
        case TypeKind::Result:
            m_out.append("result<");
            emit_type(t.members[0]);
            m_out.push_back(',');
            emit_type(t.members[1]);
            m_out.push_back('>');
            break;
        case TypeKind::TypeParam:
            m_out.append("param<");
            m_out.append(t.name.data(), t.name.size());
            for (usize i = 0; i < t.members.size(); ++i) // constraint traits
            {
                m_out.push_back(',');
                emit_type(t.members[i]);
            }
            m_out.push_back('>');
            break;
        case TypeKind::Trait:
            m_out.append("trait<");
            m_out.append(t.name.data(), t.name.size());
            for (usize i = 0; i < t.members.size(); ++i) // supertraits
            {
                m_out.push_back(',');
                emit_type(t.members[i]);
            }
            m_out.push_back('>');
            break;
        case TypeKind::Callable:
            m_out.append("fn<(");
            for (u32 i = 0; i < t.count; ++i) // params
            {
                if (i != 0U) { m_out.push_back(','); }
                emit_type(t.members[i]);
            }
            m_out.append(")->(");
            for (usize i = t.count; i < t.members.size(); ++i) // results
            {
                if (i != static_cast<usize>(t.count)) { m_out.push_back(','); }
                emit_type(t.members[i]);
            }
            m_out.append(")>");
            break;
        case TypeKind::Buffer:
            m_out.append("buffer<");
            emit_buffer_mode(t.count);
            if (t.count != static_cast<u32>(BufferMode::Raw))
            {
                m_out.push_back(',');
                emit_type(t.members[0]);
            }
            m_out.push_back('>');
            break;
        case TypeKind::Image:
            m_out.append("image<");
            emit_image_dim(t.count);
            m_out.push_back(',');
            emit_type(t.members[0]);
            m_out.push_back('>');
            break;
        case TypeKind::Sampler:
            m_out.append(t.is_signed ? "sampler<cmp>" : "sampler<plain>");
            break;
        case TypeKind::ResourceTable:
            m_out.append("restable<");
            emit_type(t.members[0]);
            m_out.push_back('>');
            break;
        case TypeKind::AccelStruct: m_out.append("accel"); break;
        case TypeKind::VideoFrame: m_out.append("video"); break;
        case TypeKind::AudioBuffer: m_out.append("audio"); break;
        case TypeKind::ExternalResource: m_out.append("external"); break;
        case TypeKind::View:
            m_out.append("view<");
            emit_type(t.members[0]);
            emit_view_ranges(t.count); // ",byte" ",mip" ... for each set bit, canonical order
            m_out.push_back('>');
            break;
        case TypeKind::Dim:
            m_out.append("dim<");
            emit_dim_body(t);
            m_out.push_back('>');
            break;
        case TypeKind::Shape:
            m_out.append("shape<");
            emit_type_list(t.members); // the dims, comma-separated
            m_out.push_back('>');
            break;
        case TypeKind::Tensor:
            m_out.append("tensor<");
            emit_type(t.members[0]); // element
            m_out.push_back(',');
            emit_type(t.members[1]); // shape
            m_out.push_back('>');
            break;
        case TypeKind::SparseTensor:
            m_out.append("stensor<");
            emit_type(t.members[0]);
            m_out.push_back(',');
            emit_type(t.members[1]);
            m_out.push_back('>');
            break;
        case TypeKind::Quantity:
            m_out.append("qty<");
            emit_type(t.members[0]); // underlying numeric type
            m_out.push_back(',');
            emit_quantity_dim(unpack_dim(t.count, t.cols));
            m_out.push_back('>');
            break;
        case TypeKind::Qualified:
            m_out.append("qual<");
            emit_ownership(static_cast<OwnershipKind>(t.count)); // ownership keyword in count
            m_out.push_back(',');
            emit_type(t.members[0]); // the qualified type
            m_out.push_back('>');
            break;
        }
    }

    // keyword table kept in lockstep with OwnershipKind (§19) — the parser mirrors this exact set (no -Werror=switch
    // guard on the parser if-chain, so the round-trip test must cover ALL nine)
    void emit_ownership(OwnershipKind k)
    {
        switch (k)
        {
        case OwnershipKind::ImmutableValue: m_out.append("imm"); break;
        case OwnershipKind::MutableValue: m_out.append("mut"); break;
        case OwnershipKind::BorrowedView: m_out.append("borrow"); break;
        case OwnershipKind::OwnedResource: m_out.append("own"); break;
        case OwnershipKind::SharedHandle: m_out.append("shared"); break;
        case OwnershipKind::WeakHandle: m_out.append("weak"); break;
        case OwnershipKind::StateSlot: m_out.append("state"); break;
        case OwnershipKind::ExternalHandle: m_out.append("ext"); break;
        case OwnershipKind::TransientArena: m_out.append("transient"); break;
        }
    }

    void emit_quantity_dim(const QuantityDim& d)
    {
        constexpr char letters[8] = {'L', 'M', 'T', 'I', 'K', 'N', 'J', 'A'}; // canonical base order
        bool           any        = false;
        for (usize i = 0; i < 8U; ++i)
        {
            if (d.exp[i] != 0)
            {
                m_out.push_back(letters[i]);
                emit_i64(static_cast<i64>(d.exp[i])); // signed exponent
                any = true;
            }
        }
        if (!any) { m_out.push_back('1'); } // dimensionless
    }

    void emit_dim_body(const Type& t)
    {
        switch (static_cast<DimKind>(t.cols))
        {
        case DimKind::Static: emit_u32(t.count); break;
        case DimKind::Symbolic: m_out.append(t.name.data(), t.name.size()); break;
        case DimKind::Dynamic: m_out.append("dyn"); break;
        }
    }

    void emit_buffer_mode(u32 c)
    {
        switch (static_cast<BufferMode>(c))
        {
        case BufferMode::Raw: m_out.append("raw"); break;
        case BufferMode::Plain: m_out.append("plain"); break;
        case BufferMode::Structured: m_out.append("structured"); break;
        case BufferMode::Typed: m_out.append("typed"); break;
        }
    }

    void emit_image_dim(u32 c)
    {
        switch (static_cast<ImageDim>(c))
        {
        case ImageDim::Dim1D: m_out.append("d1"); break; // ident-safe (a digit-leading keyword can't be parsed back)
        case ImageDim::Dim2D: m_out.append("d2"); break;
        case ImageDim::Dim3D: m_out.append("d3"); break;
        case ImageDim::Cube: m_out.append("cube"); break;
        }
    }

    void emit_view_ranges(u32 mask)
    {
        if ((mask & static_cast<u32>(ViewRange::Byte)) != 0U) { m_out.append(",byte"); }
        if ((mask & static_cast<u32>(ViewRange::Element)) != 0U) { m_out.append(",element"); }
        if ((mask & static_cast<u32>(ViewRange::Mip)) != 0U) { m_out.append(",mip"); }
        if ((mask & static_cast<u32>(ViewRange::Layer)) != 0U) { m_out.append(",layer"); }
        if ((mask & static_cast<u32>(ViewRange::Aspect)) != 0U) { m_out.append(",aspect"); }
    }

    void emit_type_list(containers::ConstSpan<TypeId> members)
    {
        for (usize i = 0; i < members.size(); ++i)
        {
            if (i != 0U) { m_out.push_back(','); }
            emit_type(members[i]);
        }
    }

    void emit_float_kind(FloatKind fk)
    {
        switch (fk)
        {
        case FloatKind::F16: m_out.append("f16"); break;
        case FloatKind::BF16: m_out.append("bf16"); break;
        case FloatKind::F32: m_out.append("f32"); break;
        case FloatKind::F64: m_out.append("f64"); break;
        case FloatKind::F8E4M3: m_out.append("f8e4m3"); break;
        case FloatKind::F8E5M2: m_out.append("f8e5m2"); break;
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
