#include <crd/ceir/parse.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/detail/symbol_registration.hpp>
#include <crd/ceir/symbol_table.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>

#include <charconv> // std::from_chars — allocation-free int/float parsing (the exact inverse of the printer's to_chars)

namespace crd::ceir
{
namespace
{
[[nodiscard]] bool is_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
[[nodiscard]] bool is_ident_start(char c) noexcept { return is_alpha(c) || c == '_'; }
// Op names carry a '.' ("dialect.op"); attribute names and keywords do not — a single class covers both.
[[nodiscard]] bool is_ident_char(char c) noexcept { return is_alpha(c) || is_digit(c) || c == '_' || c == '.'; }
[[nodiscard]] bool sv_eq(containers::StringView a, const char* b) noexcept
{
    return a == containers::StringView(b);
}

// A cap on TYPE nesting depth — a pathological `!vec<2x!vec<2x...` must not blow the recursive-descent stack (the
// CEIR-1h OOM lesson, type edition). Deep-but-legal types are vanishingly rare; 64 is far beyond any real program.
constexpr u32 kMaxTypeDepth = 64U;

[[nodiscard]] bool sv_all_digits(containers::StringView s, usize from) noexcept
{
    if (from >= s.size()) { return false; }
    for (usize i = from; i < s.size(); ++i)
    {
        if (!is_digit(s[i])) { return false; }
    }
    return true;
}

[[nodiscard]] u32 sv_to_uint(containers::StringView s, usize from) noexcept
{
    u32 v = 0U;
    for (usize i = from; i < s.size(); ++i) { v = (v * 10U) + static_cast<u32>(s[i] - '0'); }
    return v;
}

// The canonical base-dimension letter → index (CEIR-3e). K = temperature (crd-units `Th`). -1 ⇒ not a base letter.
[[nodiscard]] int base_dim_index(char c) noexcept
{
    switch (c)
    {
    case 'L': return 0;
    case 'M': return 1;
    case 'T': return 2;
    case 'I': return 3;
    case 'K': return 4;
    case 'N': return 5;
    case 'J': return 6;
    case 'A': return 7;
    default: return -1;
    }
}

// One operand whose defining value was not yet numbered when the use was parsed. In a Graph region SSA defs need not
// precede uses textually (free block-insertion order), so operand wiring is a deferred second pass, mirroring the
// printer's own two-pass numbering. `off` is retained purely for the diagnostic on an unresolved id.
struct Fixup
{
    Operation* op  = nullptr;
    u32        idx = 0;
    u32        id  = 0;
    usize      off = 0;
};

// A recursive-descent parser over a raw cursor. The grammar is exactly what print.cpp emits; whitespace/newlines are
// insignificant (layout is regenerated). All failures funnel through `fail`, which latches the FIRST error + offset.
class Parser
{
public:
    Parser(Context& ctx, containers::StringView text)
        : m_ctx(ctx), m_begin(text.data()), m_cur(text.data()), m_end(text.data() + text.size()),
          m_values(ctx.allocator()), m_fixups(ctx.allocator())
    {
    }

    ParseResult run()
    {
        Module* const m = m_ctx.create_module();
        m_module        = m;
        expect_keyword("module");
        parse_region_body(m->body());
        skip_ws();
        if (m_ok && m_cur != m_end) { fail("trailing characters after top-level module"); }
        resolve_fixups();
        if (!m_ok) { return ParseResult{nullptr, false, m_err_off, m_err}; }
        return ParseResult{m, true, 0U, ""};
    }

private:
    // ── cursor primitives ──
    [[nodiscard]] usize offset() const noexcept { return static_cast<usize>(m_cur - m_begin); }
    void                skip_ws() noexcept
    {
        while (m_cur < m_end && is_ws(*m_cur)) { ++m_cur; }
    }
    // Lookahead: the next significant char (whitespace skipped), or '\0' at end. Does not consume the char itself.
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
    void fail(const char* msg) noexcept
    {
        if (m_ok) // latch the first error only — later cascades carry no information
        {
            m_ok      = false;
            m_err_off = offset();
            m_err     = msg;
        }
    }

    void expect_keyword(const char* kw) noexcept
    {
        const containers::StringView id = parse_ident("expected keyword");
        if (m_ok && !sv_eq(id, kw)) { fail("unexpected keyword"); }
    }

    // An identifier: [A-Za-z_][A-Za-z0-9_.]* — a view INTO the source text (stable for the parse; interned by the
    // Context wherever it must outlive the text, e.g. attribute names via set_attr and symbol/string attr values).
    [[nodiscard]] containers::StringView parse_ident(const char* msg) noexcept
    {
        skip_ws();
        const char* const s = m_cur;
        if (m_cur >= m_end || !is_ident_start(*m_cur))
        {
            fail(msg);
            return {};
        }
        while (m_cur < m_end && is_ident_char(*m_cur)) { ++m_cur; }
        return containers::StringView(s, static_cast<usize>(m_cur - s));
    }

    [[nodiscard]] u32 parse_uint() noexcept
    {
        skip_ws();
        if (m_cur >= m_end || !is_digit(*m_cur))
        {
            fail("expected an unsigned integer");
            return 0U;
        }
        u32 v = 0U;
        while (m_cur < m_end && is_digit(*m_cur))
        {
            v = (v * 10U) + static_cast<u32>(*m_cur - '0');
            ++m_cur;
        }
        return v;
    }

    // A type (CEIR-3a, §16): the canonical `!`-sigil grammar print.cpp emits, parsed recursively and interned via the
    // Context. Depth-guarded (nesting cannot blow the stack). The head token after `!` disambiguates scalar vs aggregate.
    [[nodiscard]] TypeId parse_type(u32 depth = 0U) noexcept
    {
        if (depth > kMaxTypeDepth)
        {
            fail("type nesting too deep");
            return {};
        }
        expect('!', "expected '!' starting a type");
        const containers::StringView head = parse_ident("expected a type keyword after '!'");
        if (!m_ok) { return {}; }
        // scalars
        if (sv_eq(head, "bool")) { return m_ctx.type_bool(); }
        if (sv_eq(head, "index")) { return m_ctx.type_index(); }
        if (sv_eq(head, "f16")) { return m_ctx.type_float(FloatKind::F16); }
        if (sv_eq(head, "bf16")) { return m_ctx.type_float(FloatKind::BF16); }
        if (sv_eq(head, "f32")) { return m_ctx.type_float(FloatKind::F32); }
        if (sv_eq(head, "f64")) { return m_ctx.type_float(FloatKind::F64); }
        if (sv_eq(head, "f8e4m3")) { return m_ctx.type_float(FloatKind::F8E4M3); }
        if (sv_eq(head, "f8e5m2")) { return m_ctx.type_float(FloatKind::F8E5M2); }
        if ((head[0] == 'i' || head[0] == 'u') && head.size() > 1U && sv_all_digits(head, 1U))
        {
            return m_ctx.type_int(sv_to_uint(head, 1U), head[0] == 'i');
        }
        // numeric aggregates
        if (sv_eq(head, "vec"))
        {
            expect('<', "expected '<'");
            const u32 n = parse_uint();
            expect('x', "expected 'x'");
            const TypeId e = parse_type(depth + 1U);
            expect('>', "expected '>'");
            return m_ctx.type_vector(e, n);
        }
        if (sv_eq(head, "mat"))
        {
            expect('<', "expected '<'");
            const u32 r = parse_uint();
            expect('x', "expected 'x'");
            const u32 c = parse_uint();
            expect('x', "expected 'x'");
            const TypeId e = parse_type(depth + 1U);
            expect('>', "expected '>'");
            return m_ctx.type_matrix(e, r, c);
        }
        if (sv_eq(head, "complex")) { return m_ctx.type_complex(parse_wrapped(depth)); }
        if (sv_eq(head, "quat")) { return m_ctx.type_quaternion(parse_wrapped(depth)); }
        if (sv_eq(head, "option")) { return m_ctx.type_option(parse_wrapped(depth)); }
        if (sv_eq(head, "array"))
        {
            expect('<', "expected '<'");
            const u32 n = parse_uint();
            expect('x', "expected 'x'");
            const TypeId e = parse_type(depth + 1U);
            expect('>', "expected '>'");
            return m_ctx.type_array(e, n);
        }
        if (sv_eq(head, "result"))
        {
            expect('<', "expected '<'");
            const TypeId ok = parse_type(depth + 1U);
            expect(',', "expected ','");
            const TypeId err = parse_type(depth + 1U);
            expect('>', "expected '>'");
            return m_ctx.type_result(ok, err);
        }
        if (sv_eq(head, "tuple") || sv_eq(head, "variant"))
        {
            const bool is_tuple = sv_eq(head, "tuple");
            expect('<', "expected '<'");
            containers::Array<TypeId> elems(m_ctx.allocator());
            elems.push_back(parse_type(depth + 1U));
            while (accept(',')) { elems.push_back(parse_type(depth + 1U)); }
            expect('>', "expected '>'");
            const containers::ConstSpan<TypeId> sp(elems.data(), elems.size());
            return is_tuple ? m_ctx.type_tuple(sp) : m_ctx.type_variant(sp);
        }
        if (sv_eq(head, "struct"))
        {
            expect('<', "expected '<'");
            const containers::StringView name = parse_ident("expected struct name");
            containers::Array<TypeId>              ftypes(m_ctx.allocator());
            containers::Array<containers::StringView> fnames(m_ctx.allocator());
            while (accept(','))
            {
                fnames.push_back(parse_ident("expected field name"));
                expect(':', "expected ':'");
                ftypes.push_back(parse_type(depth + 1U));
            }
            expect('>', "expected '>'");
            return m_ctx.type_struct(name, containers::ConstSpan<TypeId>(ftypes.data(), ftypes.size()),
                                     containers::ConstSpan<containers::StringView>(fnames.data(), fnames.size()));
        }
        if (sv_eq(head, "enum"))
        {
            expect('<', "expected '<'");
            const containers::StringView              name = parse_ident("expected enum name");
            containers::Array<containers::StringView> cases(m_ctx.allocator());
            while (accept(',')) { cases.push_back(parse_ident("expected enum case name")); }
            expect('>', "expected '>'");
            return m_ctx.type_enum(name, containers::ConstSpan<containers::StringView>(cases.data(), cases.size()));
        }
        // generics (CEIR-3b): !param<T[,trait]*>  !trait<Name[,supertrait]*>  !fn<(P,..)->(R,..)>
        if (sv_eq(head, "param") || sv_eq(head, "trait"))
        {
            const bool is_param = sv_eq(head, "param");
            expect('<', "expected '<'");
            const containers::StringView name = parse_ident(is_param ? "expected type-param name" : "expected trait name");
            containers::Array<TypeId>    ms(m_ctx.allocator());
            while (accept(',')) { ms.push_back(parse_type(depth + 1U)); }
            expect('>', "expected '>'");
            const containers::ConstSpan<TypeId> sp(ms.data(), ms.size());
            return is_param ? m_ctx.type_param(name, sp) : m_ctx.type_trait(name, sp);
        }
        if (sv_eq(head, "fn"))
        {
            expect('<', "expected '<'");
            containers::Array<TypeId> params(m_ctx.allocator());
            containers::Array<TypeId> results(m_ctx.allocator());
            parse_paren_type_list(depth, params);
            expect('-', "expected '->'");
            expect('>', "expected '->'");
            parse_paren_type_list(depth, results);
            expect('>', "expected '>'");
            return m_ctx.type_callable(containers::ConstSpan<TypeId>(params.data(), params.size()),
                                       containers::ConstSpan<TypeId>(results.data(), results.size()));
        }
        // resources + views (CEIR-3c): !buffer<mode[,T]>  !image<dim,fmt>  !sampler<plain|cmp>  !restable<T>
        // !accel / !video / !audio / !external  !view<resource[,range]*>
        if (sv_eq(head, "buffer"))
        {
            expect('<', "expected '<'");
            const containers::StringView mode = parse_ident("expected buffer mode");
            BufferMode                   bm    = BufferMode::Raw;
            if (sv_eq(mode, "raw")) { bm = BufferMode::Raw; }
            else if (sv_eq(mode, "plain")) { bm = BufferMode::Plain; }
            else if (sv_eq(mode, "structured")) { bm = BufferMode::Structured; }
            else if (sv_eq(mode, "typed")) { bm = BufferMode::Typed; }
            else
            {
                fail("unknown buffer mode");
                return {};
            }
            TypeId elem;
            if (bm != BufferMode::Raw)
            {
                expect(',', "expected ','");
                elem = parse_type(depth + 1U);
            }
            expect('>', "expected '>'");
            return m_ctx.type_buffer(bm, elem);
        }
        if (sv_eq(head, "image"))
        {
            expect('<', "expected '<'");
            const containers::StringView dim = parse_ident("expected image dim");
            ImageDim                     id   = ImageDim::Dim1D;
            if (sv_eq(dim, "d1")) { id = ImageDim::Dim1D; }
            else if (sv_eq(dim, "d2")) { id = ImageDim::Dim2D; }
            else if (sv_eq(dim, "d3")) { id = ImageDim::Dim3D; }
            else if (sv_eq(dim, "cube")) { id = ImageDim::Cube; }
            else
            {
                fail("unknown image dim");
                return {};
            }
            expect(',', "expected ','");
            const TypeId fmt = parse_type(depth + 1U);
            expect('>', "expected '>'");
            return m_ctx.type_image(id, fmt);
        }
        if (sv_eq(head, "sampler"))
        {
            expect('<', "expected '<'");
            const containers::StringView k = parse_ident("expected sampler kind");
            bool                         cmp = false;
            if (sv_eq(k, "plain")) { cmp = false; }
            else if (sv_eq(k, "cmp")) { cmp = true; }
            else
            {
                fail("unknown sampler kind");
                return {};
            }
            expect('>', "expected '>'");
            return m_ctx.type_sampler(cmp);
        }
        if (sv_eq(head, "restable")) { return m_ctx.type_resource_table(parse_wrapped(depth)); }
        if (sv_eq(head, "accel")) { return m_ctx.type_accel_struct(); }
        if (sv_eq(head, "video")) { return m_ctx.type_video_frame(); }
        if (sv_eq(head, "audio")) { return m_ctx.type_audio_buffer(); }
        if (sv_eq(head, "external")) { return m_ctx.type_external_resource(); }
        if (sv_eq(head, "view"))
        {
            expect('<', "expected '<'");
            const TypeId underlying = parse_type(depth + 1U);
            u32          mask       = 0U;
            while (accept(','))
            {
                const containers::StringView r = parse_ident("expected view range dimension");
                if (sv_eq(r, "byte")) { mask |= static_cast<u32>(ViewRange::Byte); }
                else if (sv_eq(r, "element")) { mask |= static_cast<u32>(ViewRange::Element); }
                else if (sv_eq(r, "mip")) { mask |= static_cast<u32>(ViewRange::Mip); }
                else if (sv_eq(r, "layer")) { mask |= static_cast<u32>(ViewRange::Layer); }
                else if (sv_eq(r, "aspect")) { mask |= static_cast<u32>(ViewRange::Aspect); }
                else
                {
                    fail("unknown view range dimension");
                    return {};
                }
            }
            expect('>', "expected '>'");
            // the tri-split's PARSER arm: a grammatically-valid but semantically-invalid combination fails with a
            // pointing diagnostic. ⛔ Must bail on a prior error FIRST — else `underlying` is unreliable and the
            // asserting factory would be reached on an already-failed parse (a corrupt-input abort).
            if (!m_ok) { return {}; }
            if (!m_ctx.view_combination_valid(underlying, mask))
            {
                fail("invalid view/resource combination");
                return {};
            }
            return m_ctx.type_view(underlying, mask);
        }
        // shapes + tensors (CEIR-3d): !dim<4|dyn|Name>  !shape<dim,..>  !tensor<elem,shape>  !stensor<elem,shape>
        if (sv_eq(head, "dim"))
        {
            expect('<', "expected '<'");
            TypeId d;
            if (is_digit(la())) { d = m_ctx.type_dim_static(parse_uint()); } // a static extent
            else
            {
                const containers::StringView nm = parse_ident("expected a dim name or 'dyn'");
                if (!m_ok) { return {}; } // ⛔ bail before the asserting symbolic-dim factory
                d = sv_eq(nm, "dyn") ? m_ctx.type_dim_dynamic() : m_ctx.type_dim_symbolic(nm);
            }
            expect('>', "expected '>'");
            return d;
        }
        if (sv_eq(head, "shape"))
        {
            expect('<', "expected '<'");
            containers::Array<TypeId> dims(m_ctx.allocator());
            if (la() != '>')
            {
                dims.push_back(parse_type(depth + 1U));
                while (accept(',')) { dims.push_back(parse_type(depth + 1U)); }
            }
            expect('>', "expected '>'");
            if (!m_ok) { return {}; } // ⛔ bail before the asserting shape factory
            const containers::ConstSpan<TypeId> sp(dims.data(), dims.size());
            if (!m_ctx.shape_members_valid(sp))
            {
                fail("shape members must be dims");
                return {};
            }
            return m_ctx.type_shape(sp);
        }
        if (sv_eq(head, "tensor") || sv_eq(head, "stensor"))
        {
            const bool sparse = sv_eq(head, "stensor");
            expect('<', "expected '<'");
            const TypeId elem = parse_type(depth + 1U);
            expect(',', "expected ','");
            const TypeId shp = parse_type(depth + 1U);
            expect('>', "expected '>'");
            if (!m_ok) { return {}; } // ⛔ bail before the asserting tensor factory
            if (!m_ctx.tensor_composition_valid(elem, shp))
            {
                fail("invalid tensor element/shape composition");
                return {};
            }
            return sparse ? m_ctx.type_sparse_tensor(elem, shp) : m_ctx.type_tensor(elem, shp);
        }
        // physical quantities (CEIR-3e): !qty<UNDERLYING,DIM>  DIM = base-letter+signed-exponent terms, or '1' (dimensionless)
        if (sv_eq(head, "qty"))
        {
            expect('<', "expected '<'");
            const TypeId      underlying = parse_type(depth + 1U);
            expect(',', "expected ','");
            const QuantityDim dim = parse_dimension();
            expect('>', "expected '>'");
            if (!m_ok) { return {}; } // ⛔ bail before the asserting quantity factory
            if (!m_ctx.quantity_composition_valid(underlying))
            {
                fail("a quantity's underlying type must be numeric");
                return {};
            }
            return m_ctx.type_quantity(underlying, dim);
        }
        // ownership / lifetime qualifiers (CEIR-3f, §19): !qual<KIND,T>  KIND ∈ {imm,mut,borrow,own,shared,weak,state,
        // ext,transient} — the keyword set is kept in lockstep with the printer's emit_ownership (no -Werror=switch guard
        // on this if-chain, so the round-trip test must cover ALL nine).
        if (sv_eq(head, "qual"))
        {
            expect('<', "expected '<'");
            const containers::StringView kw = parse_ident("expected an ownership keyword");
            OwnershipKind                own = OwnershipKind::ImmutableValue;
            if (sv_eq(kw, "imm")) { own = OwnershipKind::ImmutableValue; }
            else if (sv_eq(kw, "mut")) { own = OwnershipKind::MutableValue; }
            else if (sv_eq(kw, "borrow")) { own = OwnershipKind::BorrowedView; }
            else if (sv_eq(kw, "own")) { own = OwnershipKind::OwnedResource; }
            else if (sv_eq(kw, "shared")) { own = OwnershipKind::SharedHandle; }
            else if (sv_eq(kw, "weak")) { own = OwnershipKind::WeakHandle; }
            else if (sv_eq(kw, "state")) { own = OwnershipKind::StateSlot; }
            else if (sv_eq(kw, "ext")) { own = OwnershipKind::ExternalHandle; }
            else if (sv_eq(kw, "transient")) { own = OwnershipKind::TransientArena; }
            else
            {
                fail("unknown ownership qualifier");
                return {};
            }
            expect(',', "expected ','");
            const TypeId underlying = parse_type(depth + 1U);
            expect('>', "expected '>'");
            if (!m_ok) { return {}; } // ⛔ bail before the asserting qualified factory
            if (!m_ctx.qualified_composition_valid(underlying))
            {
                fail("this type cannot be ownership-qualified");
                return {};
            }
            return m_ctx.type_qualified(own, underlying);
        }
        fail("unknown type keyword");
        return {};
    }

    // A signed integer: optional '-' then digits. Distinct from parse_uint (which is unsigned) — used by dimension terms.
    [[nodiscard]] i32 parse_signed_int() noexcept
    {
        skip_ws();
        bool neg = false;
        if (m_cur < m_end && *m_cur == '-')
        {
            neg = true;
            ++m_cur;
        }
        if (m_cur >= m_end || !is_digit(*m_cur))
        {
            fail("expected an integer exponent");
            return 0;
        }
        i32 v = 0;
        while (m_cur < m_end && is_digit(*m_cur))
        {
            v = (v * 10) + static_cast<i32>(*m_cur - '0');
            ++m_cur;
        }
        return neg ? -v : v;
    }

    // A physical dimension (CEIR-3e): base-letter+signed-exponent terms in canonical order (no dup, mandatory exponent),
    // or the literal '1' for dimensionless. Parsed character-wise (the '-' rules out parse_ident).
    [[nodiscard]] QuantityDim parse_dimension() noexcept
    {
        QuantityDim d;
        if (la() == '1') // dimensionless (the ONLY spelling for it — an empty dimension is malformed)
        {
            ++m_cur;
            return d;
        }
        int last  = -1;
        int terms = 0;
        for (;;)
        {
            const int base = base_dim_index(la());
            if (base < 0) { break; } // not a base letter ⇒ end of the dimension (the caller expects '>')
            ++m_cur;                 // consume the letter (la() left m_cur on it)
            if (base <= last)
            {
                fail("dimension bases must be in canonical order without duplicates");
                return d;
            }
            last          = base;
            const i32 e   = parse_signed_int();
            if (!m_ok) { return d; }
            if (e > 127 || e < -128)
            {
                fail("dimension exponent out of range");
                return d;
            }
            d.exp[base] = static_cast<i8>(e);
            ++terms;
        }
        if (terms == 0) { fail("a dimension needs '1' (dimensionless) or at least one base-exponent term"); }
        return d;
    }

    // `( [type (, type)*] )` — the possibly-empty parenthesized type list used by a callable's params + results.
    void parse_paren_type_list(u32 depth, containers::Array<TypeId>& out) noexcept
    {
        expect('(', "expected '('");
        if (la() != ')')
        {
            out.push_back(parse_type(depth + 1U));
            while (accept(',')) { out.push_back(parse_type(depth + 1U)); }
        }
        expect(')', "expected ')'");
    }

    // `< type >` — the single-child aggregate wrapper (complex/quat/option).
    [[nodiscard]] TypeId parse_wrapped(u32 depth) noexcept
    {
        expect('<', "expected '<'");
        const TypeId e = parse_type(depth + 1U);
        expect('>', "expected '>'");
        return e;
    }

    // ── SSA value id -> Value* map (dense in the printer's pre-order; a gap-tolerant Array keyed by id) ──
    void ensure_id(u32 id) noexcept
    {
        while (m_values.size() <= static_cast<usize>(id)) { m_values.push_back(nullptr); }
    }
    void register_value(u32 id, Value* v) noexcept
    {
        // A def costs >= 2 text bytes ("%N"), so a well-formed module's def id can never reach the text length. Bound
        // it BEFORE ensure_id grows the map, or a hostile "%4000000000 = ..." demands a multi-GB array (OOM/crash).
        if (static_cast<usize>(id) >= static_cast<usize>(m_end - m_begin))
        {
            fail("SSA value id out of range");
            return;
        }
        ensure_id(id);
        if (m_values[id] != nullptr)
        {
            fail("duplicate SSA value id");
            return;
        }
        m_values[id] = v;
    }
    [[nodiscard]] Value* resolve(u32 id) const noexcept
    {
        return static_cast<usize>(id) < m_values.size() ? m_values[id] : nullptr;
    }

    // ── region / block / op ──
    void parse_region_body(Region* r) noexcept
    {
        expect('{', "expected '{' opening a region");
        while (m_ok && la() == '^') { parse_block(r); }
        expect('}', "expected '}' closing a region");
    }

    void parse_block(Region* r) noexcept
    {
        expect('^', "expected '^' starting a block label");
        (void)parse_ident("expected a block label after '^'"); // "bb<idx>" — positional; the label text is discarded

        u32               num_args = 0U;
        TypeId            first_arg_type{};
        containers::Array<u32> arg_ids(m_ctx.allocator());
        if (la() == '(')
        {
            accept('(');
            while (m_ok)
            {
                expect('%', "expected '%' starting a block argument");
                const u32 id = parse_uint();
                TypeId    t{};
                if (la() == ':')
                {
                    accept(':');
                    t = parse_type();
                }
                if (num_args == 0U) { first_arg_type = t; }
                arg_ids.push_back(id);
                ++num_args;
                if (!accept(',')) { break; }
            }
            expect(')', "expected ')' closing block arguments");
        }
        expect(':', "expected ':' after a block header");
        if (!m_ok) { return; }

        // create_block applies ONE type to all args (the CEIR-1a block model); the printer prints them individually
        // but they are always identical, so the first arg's type reconstructs the block faithfully.
        Block* const b = m_ctx.create_block(num_args, first_arg_type);
        r->append(b);
        for (u32 i = 0; i < num_args; ++i) { register_value(arg_ids[i], b->arg(i)); }

        for (;;)
        {
            const char c = la();
            if (!m_ok || c == '^' || c == '}' || c == '\0') { break; }
            parse_op(b);
        }
    }

    void parse_op(Block* b) noexcept
    {
        const usize op_off = offset();

        // results: "%id[, %id]* ="  (bare — result types come from the trailing ": !tN")
        containers::Array<u32> result_ids(m_ctx.allocator());
        if (la() == '%')
        {
            while (m_ok)
            {
                expect('%', "expected '%' starting a result");
                result_ids.push_back(parse_uint());
                if (!accept(',')) { break; }
            }
            expect('=', "expected '=' after op results");
        }
        const auto num_results = static_cast<u32>(result_ids.size());

        // op name "dialect.op"
        const containers::StringView name = parse_ident("expected an operation name");
        containers::StringView       dialect;
        containers::StringView       opname;
        if (!split_op_name(name, dialect, opname))
        {
            fail("operation name must be 'dialect.op'");
            return;
        }
        const OpId kind = m_ctx.intern_op(dialect, opname);

        // operands "(%id, ...)"
        expect('(', "expected '(' starting operands");
        containers::Array<u32> operand_ids(m_ctx.allocator());
        if (la() != ')')
        {
            while (m_ok)
            {
                expect('%', "expected '%' starting an operand");
                operand_ids.push_back(parse_uint());
                if (!accept(',')) { break; }
            }
        }
        expect(')', "expected ')' closing operands");

        // optional attribute dict "{name = value, ...}" — disambiguated from a region '{' by lookahead
        containers::Array<containers::StringView> attr_names(m_ctx.allocator());
        containers::Array<AttrId>                 attr_vals(m_ctx.allocator());
        if (la() == '{' && brace_opens_attrs()) { parse_attrs(attr_names, attr_vals); }

        // optional single result type ": !tN"
        TypeId result_type{};
        if (la() == ':')
        {
            accept(':');
            result_type = parse_type();
        }

        // trailing region groups — COUNT them before creating the op (create_operation needs num_regions upfront)
        const u32 num_regions = count_trailing_regions();
        if (!m_ok) { return; }

        containers::Array<Value*> operand_vals(m_ctx.allocator());
        for (usize i = 0; i < operand_ids.size(); ++i) { operand_vals.push_back(resolve(operand_ids[i])); }
        Operation* const op = m_ctx.create_operation(
            kind, containers::ConstSpan<Value*>(operand_vals.data(), operand_vals.size()), num_results, result_type,
            num_regions);
        b->append(op);

        for (u32 i = 0; i < num_results; ++i) { register_value(result_ids[i], op->result(i)); }
        for (usize i = 0; i < operand_ids.size(); ++i)
        {
            if (operand_vals[i] == nullptr) { m_fixups.push_back(Fixup{op, static_cast<u32>(i), operand_ids[i], op_off}); }
        }
        for (usize i = 0; i < attr_names.size(); ++i) { m_ctx.set_attr(op, attr_names[i], attr_vals[i]); }

        register_symbol(op); // symbol-defining ops (a `sym_name` attr) re-enter the module's SymbolTable

        for (u32 i = 0; i < num_regions; ++i) { parse_region_body(op->region(i)); }
    }

    // A symbol-defining op carries its identity as a `sym_name` string attr (MLIR's model — the SymbolTable is an
    // INDEX over that attr). Re-register it so the parsed module resolves like the original; a duplicate is an error.
    // Shared with the binary deserializer (detail::register_symbol) so the two loaders never drift.
    void register_symbol(Operation* op) noexcept
    {
        if (!detail::register_symbol(m_ctx, *m_module, op)) { fail("duplicate symbol definition"); }
    }

    void parse_attrs(containers::Array<containers::StringView>& names, containers::Array<AttrId>& vals) noexcept
    {
        expect('{', "expected '{' opening an attribute dict");
        while (m_ok)
        {
            names.push_back(parse_ident("expected an attribute name"));
            expect('=', "expected '=' in an attribute entry");
            vals.push_back(parse_attr_value());
            if (!accept(',')) { break; }
        }
        expect('}', "expected '}' closing an attribute dict");
    }

    [[nodiscard]] AttrId parse_attr_value() noexcept
    {
        const char c = la();
        if (c == '"') { return parse_string_attr(); }
        if (c == '@')
        {
            accept('@');
            const containers::StringView s = parse_ident("expected a symbol name after '@'");
            return m_ctx.attr_symbol(s);
        }
        if (c == '!') { return m_ctx.attr_type(parse_type()); }
        if (c == '-' || c == '+' || c == '.' || is_digit(c)) { return parse_number_attr(); }
        // an identifier: true / false, or a bare float word (nan / inf) the printer can emit
        const containers::StringView w = parse_ident("expected an attribute value");
        if (sv_eq(w, "true")) { return m_ctx.attr_bool(true); }
        if (sv_eq(w, "false")) { return m_ctx.attr_bool(false); }
        return float_from_word(w); // nan / inf
    }

    // "..." with only \" and \\ escapes (print.cpp emit_quoted). Unescape BEFORE interning — interning the raw slice
    // would keep the backslashes and re-escaping would grow the text on every round-trip.
    [[nodiscard]] AttrId parse_string_attr() noexcept
    {
        expect('"', "expected '\"' opening a string");
        containers::String buf(m_ctx.allocator());
        while (m_cur < m_end && *m_cur != '"')
        {
            char ch = *m_cur++;
            if (ch == '\\' && m_cur < m_end) { ch = *m_cur++; } // the escaped byte is emitted verbatim
            buf.push_back(ch);
        }
        expect('"', "unterminated string literal");
        return m_ctx.attr_string(containers::StringView(buf.data(), buf.size()));
    }

    [[nodiscard]] AttrId parse_number_attr() noexcept
    {
        skip_ws();
        const char* const s = m_cur;
        while (m_cur < m_end)
        {
            const char c = *m_cur;
            if (is_alpha(c) || is_digit(c) || c == '.' || c == '+' || c == '-') { ++m_cur; }
            else { break; }
        }
        const containers::StringView run(s, static_cast<usize>(m_cur - s));
        bool                         is_float = false;
        for (usize i = 0; i < run.size(); ++i)
        {
            const char c = run[i];
            if (c == '.' || c == 'e' || c == 'E' || is_alpha(c)) { is_float = true; break; }
        }
        if (is_float) { return float_from_word(run); }
        i64        v   = 0;
        const auto res = std::from_chars(run.data(), run.data() + run.size(), v);
        if (res.ec != std::errc{} || res.ptr != run.data() + run.size())
        {
            fail("malformed integer attribute");
            return {};
        }
        return m_ctx.attr_int(v);
    }

    [[nodiscard]] AttrId float_from_word(containers::StringView run) noexcept
    {
        f64        v   = 0.0;
        const auto res = std::from_chars(run.data(), run.data() + run.size(), v, std::chars_format::general);
        if (res.ec != std::errc{} || res.ptr != run.data() + run.size())
        {
            fail("malformed float attribute");
            return {};
        }
        return m_ctx.attr_float(v);
    }

    // Decide whether a '{' at the cursor opens an attribute dict (next significant char is an identifier: `name =`)
    // or a region (next is '^' for a block, or '}' for an empty region). Non-consuming.
    [[nodiscard]] bool brace_opens_attrs() noexcept
    {
        const char* const save = m_cur;
        accept('{');
        skip_ws();
        const char c = m_cur < m_end ? *m_cur : '\0';
        m_cur        = save;
        return is_ident_start(c);
    }

    // Count the trailing region groups without consuming them: each is a balanced `{ ... }`. Restores the cursor so
    // the real region parse re-reads from here. Balanced-brace scanning skips string literals (a nested op's string
    // attr may contain a brace).
    [[nodiscard]] u32 count_trailing_regions() noexcept
    {
        const char* const save = m_cur;
        u32               n    = 0U;
        for (;;)
        {
            skip_ws();
            if (m_cur >= m_end || *m_cur != '{') { break; }
            if (!skip_balanced_braces()) { break; }
            ++n;
        }
        m_cur = save;
        return n;
    }

    // Consumes a balanced `{ ... }` starting at the cursor ('{' assumed). Returns false if it runs off the end.
    bool skip_balanced_braces() noexcept
    {
        ++m_cur; // the opening '{'
        int depth = 1;
        while (m_cur < m_end && depth > 0)
        {
            const char c = *m_cur;
            if (c == '"')
            {
                skip_string_literal();
                continue;
            }
            if (c == '{') { ++depth; }
            else if (c == '}') { --depth; }
            ++m_cur;
        }
        return depth == 0;
    }

    void skip_string_literal() noexcept
    {
        ++m_cur; // opening '"'
        while (m_cur < m_end && *m_cur != '"')
        {
            if (*m_cur == '\\' && (m_cur + 1) < m_end) { m_cur += 2; }
            else { ++m_cur; }
        }
        if (m_cur < m_end) { ++m_cur; } // closing '"'
    }

    void resolve_fixups() noexcept
    {
        if (!m_ok) { return; }
        for (usize i = 0; i < m_fixups.size(); ++i)
        {
            const Fixup& f = m_fixups[i];
            Value* const v = resolve(f.id);
            if (v == nullptr)
            {
                m_ok      = false;
                m_err_off = f.off;
                m_err     = "operand references an undefined SSA value";
                return;
            }
            f.op->set_operand(f.idx, v);
        }
    }

    // "dialect.op" -> (dialect, op) on the FIRST '.', reproducing intern_op's concatenation exactly.
    [[nodiscard]] static bool split_op_name(containers::StringView full, containers::StringView& dialect,
                                            containers::StringView& op) noexcept
    {
        for (usize i = 0; i < full.size(); ++i)
        {
            if (full[i] == '.')
            {
                dialect = containers::StringView(full.data(), i);
                op      = containers::StringView(full.data() + i + 1U, full.size() - i - 1U);
                return !dialect.empty() && !op.empty();
            }
        }
        return false;
    }

    Context&                        m_ctx;
    Module*                         m_module = nullptr;
    const char*                     m_begin;
    const char*                     m_cur;
    const char*                     m_end;
    containers::Array<Value*>       m_values; // SSA id -> defining value
    containers::Array<Fixup>        m_fixups; // operands whose def was not yet numbered when the use was parsed
    bool                            m_ok      = true;
    usize                           m_err_off = 0;
    const char*                     m_err     = "";
};
} // namespace

ParseResult parse(Context& ctx, containers::StringView text)
{
    Parser p(ctx, text);
    return p.run();
}
} // namespace crd::ceir
