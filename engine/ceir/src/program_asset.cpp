#include <crd/ceir/program_asset.hpp>

#include <crd/ceir/attr.hpp>   // AttrValue / AttrKind (sym_name, callee, depth reads)
#include <crd/ceir/dialect.hpp> // OpInfo (op_info), OpTrait::StateEdge
#include <crd/ceir/func.hpp>    // func_kind / return_kind / call_kind
#include <crd/ceir/symbol_table.hpp>
#include <crd/containers/hash_map.hpp>

// CEIR-7a — the §105-§107 program-as-asset metadata: interface_hash (§107), collect_dependencies (§106),
// find_unregistered_op (the strict cook-time check). PURE IR analysis (no asset/CRDR/cook edge — ADR-0109 I5). Mirrors
// the stable_hash (content hash) FNV-1a precedent in binary.cpp.

namespace crd::ceir
{
namespace
{
constexpr u64 kFnvOffset = 14695981039346656037ULL;
constexpr u64 kFnvPrime  = 1099511628211ULL;
[[nodiscard]] u64 fnv1a(containers::ConstSpan<u8> bytes) noexcept
{
    u64 h = kFnvOffset;
    for (usize i = 0; i < bytes.size(); ++i)
    {
        h ^= static_cast<u64>(bytes[i]);
        h *= kFnvPrime;
    }
    return h;
}
void push_u32(containers::Array<u8>& out, u32 v)
{
    out.push_back(static_cast<u8>(v & 0xFFU));
    out.push_back(static_cast<u8>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<u8>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<u8>((v >> 24U) & 0xFFU));
}
void push_str(containers::Array<u8>& out, containers::StringView s)
{
    push_u32(out, static_cast<u32>(s.size()));
    for (usize i = 0; i < s.size(); ++i) { out.push_back(static_cast<u8>(s[i])); }
}
// Structural type encoding — recurse into `members`, NEVER emit a Context-local TypeId int, so the projection is
// cross-Context STABLE (identical structural types encode byte-equal regardless of intern history — like the 1f blob).
void encode_type(const Context& ctx, TypeId id, containers::Array<u8>& out)
{
    if (!id.valid()) { out.push_back(0xFFU); return; } // an untyped/opaque value — a distinct, stable marker
    const Type t = ctx.type_of(id);
    out.push_back(static_cast<u8>(t.kind));
    out.push_back(t.is_signed ? 1U : 0U);
    out.push_back(static_cast<u8>(t.fkind));
    push_u32(out, t.count);
    push_u32(out, t.cols);
    push_str(out, t.name);
    push_u32(out, static_cast<u32>(t.members.size()));
    for (usize i = 0; i < t.members.size(); ++i) { encode_type(ctx, t.members[i], out); }
    push_u32(out, static_cast<u32>(t.labels.size()));
    for (usize i = 0; i < t.labels.size(); ++i) { push_str(out, t.labels[i]); }
}
[[nodiscard]] bool sv_less(containers::StringView a, containers::StringView b) noexcept
{
    const usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; ++i)
    {
        const u8 ca = static_cast<u8>(a[i]);
        const u8 cb = static_cast<u8>(b[i]);
        if (ca != cb) { return ca < cb; }
    }
    return a.size() < b.size();
}
[[nodiscard]] containers::StringView op_string_attr(const Context& ctx, const Operation& op, containers::StringView key)
{
    const AttrId id = op.attr(key);
    if (!id.valid()) { return {}; }
    const AttrValue v = ctx.attr_value(id);
    if (v.kind != AttrKind::String && v.kind != AttrKind::SymbolRef) { return {}; }
    return v.s;
}
// Append every `func.func` op in the module body (top-level only — funcs are module-body children) to `out`.
void collect_funcs(Context& ctx, const Module& m, containers::Array<Operation*>& out)
{
    const OpId func_kind = func::func_kind(ctx);
    Region* const body   = m.body();
    if (body == nullptr) { return; }
    for (Block* b = body->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->kind().value == func_kind.value) { out.push_back(op); }
        }
    }
}
// The first `func.return` (pre-order) in `func_op`'s body — its operands' types are the func's RESULT signature.
const Operation* find_return(Operation* func_op, OpId return_kind)
{
    struct W
    {
        static const Operation* go(Region* r, OpId rk)
        {
            if (r == nullptr) { return nullptr; }
            for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
            {
                for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
                {
                    if (op->kind().value == rk.value) { return op; }
                    for (u32 i = 0; i < op->num_regions(); ++i)
                    {
                        if (const Operation* f = go(op->region(i), rk)) { return f; }
                    }
                }
            }
            return nullptr;
        }
    };
    return W::go(func_op->region(0), return_kind);
}
} // namespace

u64 interface_hash(Context& ctx, const Module& module, memory::IAllocator* scratch)
{
    containers::Array<u8>         proj(scratch);
    containers::Array<Operation*> funcs(scratch);
    collect_funcs(ctx, module, funcs);

    // Keep ONLY exported (Public) funcs, then SORT BY NAME (⛔ never body order — a func reorder is an impl edit).
    const SymbolTable* const syms = module.symbols();
    containers::Array<Operation*> exported(scratch);
    for (u32 i = 0; i < static_cast<u32>(funcs.size()); ++i)
    {
        const containers::StringView name = op_string_attr(ctx, *funcs[i], "sym_name");
        Visibility                   vis  = Visibility::Public; // absent from the table ⇒ conservatively exported
        if (syms != nullptr)
        {
            if (const SymbolEntry* const e = syms->lookup(name)) { vis = e->visibility; }
        }
        if (vis == Visibility::Public) { exported.push_back(funcs[i]); }
    }
    for (u32 i = 1; i < static_cast<u32>(exported.size()); ++i) // insertion sort by sym_name (no std::sort)
    {
        Operation* const key = exported[i];
        const containers::StringView kn = op_string_attr(ctx, *key, "sym_name");
        u32 j = i;
        while (j > 0U && sv_less(kn, op_string_attr(ctx, *exported[j - 1U], "sym_name")))
        {
            exported[j] = exported[j - 1U];
            --j;
        }
        exported[j] = key;
    }

    const OpId return_kind = func::return_kind(ctx);
    for (u32 i = 0; i < static_cast<u32>(exported.size()); ++i)
    {
        Operation* const f = exported[i];
        push_str(proj, op_string_attr(ctx, *f, "sym_name"));
        // param types = the entry block's arg types.
        Block* const eb = f->region(0)->first_block();
        const u32    np = (eb != nullptr) ? eb->num_args() : 0U;
        push_u32(proj, np);
        for (u32 a = 0; a < np; ++a) { encode_type(ctx, eb->arg(a)->type(), proj); }
        // result types = the func.return operands' types (the terminator carries the signature).
        const Operation* const ret = find_return(f, return_kind);
        const u32              nr  = (ret != nullptr) ? ret->num_operands() : 0U;
        push_u32(proj, nr);
        for (u32 r = 0; r < nr; ++r) { encode_type(ctx, ret->operand(r)->type(), proj); }
        // caller-visible effects = the 5c TRANSITIVE effective set over the body (fresh visited per func — a shared map
        // yields deterministic-but-WRONG masks). A body edit that adds an effect visible to callers IS an interface change.
        containers::HashMap<const Operation*, u8> visited(scratch);
        const EffectQuery                         q{syms, &visited};
        u32                                       mask = 0U;
        ctx.collect_region_effective_mask(*f->region(0), q, mask);
        push_u32(proj, mask);
    }

    // §20 STATE SCHEMA — MODULE-WIDE (every StateEdge cell, module pre-order), NOT per-exported-func: a PRIVATE callee's
    // cells are live runtime state a 7c hot-swap must migrate; exported-only under-inclusion risks a wrong "compatible"
    // verdict (state corruption), while over-inclusion only costs a spurious recook — the safety asymmetry decides it.
    // ⛔ Cells are INSTANCE-keyed with no stable id until 7c, so a private-func reorder changes this hash — a SAFE
    // false-incompatible (a needless recook, never a missed migration), named as 7c's refinement. Cell = its value type +
    // §20 depth, in body order (layout order IS the migration schema).
    push_str(proj, containers::StringView("state:"));
    struct StateW
    {
        static void go(Context& c, Region* r, containers::Array<u8>& p)
        {
            if (r == nullptr) { return; }
            for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
            {
                for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
                {
                    if (c.has_trait(op->kind(), OpTrait::StateEdge) && op->num_results() >= 1U)
                    {
                        encode_type(c, op->result(0)->type(), p);
                        u32          depth   = 1U; // §20 default depth
                        const AttrId depth_id = op->attr("depth");
                        if (depth_id.valid())
                        {
                            const AttrValue dv = c.attr_value(depth_id);
                            if (dv.kind == AttrKind::Int && dv.i >= 1) { depth = static_cast<u32>(dv.i); }
                        }
                        push_u32(p, depth);
                    }
                    for (u32 i = 0; i < op->num_regions(); ++i) { go(c, op->region(i), p); }
                }
            }
        }
    };
    StateW::go(ctx, module.body(), proj);

    return fnv1a(containers::ConstSpan<u8>(proj.data(), proj.size()));
}

namespace
{
void add_unique(containers::Array<containers::StringView>& list, containers::StringView s)
{
    if (s.empty()) { return; }
    for (u32 i = 0; i < static_cast<u32>(list.size()); ++i)
    {
        if (list[i] == s) { return; }
    }
    list.push_back(s);
}
void sort_svs(containers::Array<containers::StringView>& list)
{
    for (u32 i = 1; i < static_cast<u32>(list.size()); ++i)
    {
        const containers::StringView key = list[i];
        u32                          j   = i;
        while (j > 0U && sv_less(key, list[j - 1U]))
        {
            list[j] = list[j - 1U];
            --j;
        }
        list[j] = key;
    }
}
} // namespace

DependencyRecord collect_dependencies(Context& ctx, const Module& module, memory::IAllocator* alloc)
{
    DependencyRecord     rec(alloc);
    const OpId           call_kind = func::call_kind(ctx);
    const SymbolTable* const syms  = module.symbols();
    // one pre-order walk over the whole module (incl. nested regions).
    struct W
    {
        static void go(Context& c, Region* r, OpId ck, const SymbolTable* s, DependencyRecord& out)
        {
            if (r == nullptr) { return; }
            for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
            {
                for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
                {
                    // §106 called funcs: a func.call whose callee resolves OUTSIDE this module = an IMPORT (a dependency);
                    // an internal call is content, not a dep (recording it would forge a false cross-asset edge at 7c).
                    if (op->kind().value == ck.value)
                    {
                        const AttrId cid = op->attr("callee");
                        if (cid.valid())
                        {
                            const AttrValue cv = c.attr_value(cid);
                            if (cv.kind == AttrKind::SymbolRef && (s == nullptr || s->lookup(cv.s) == nullptr))
                            {
                                add_unique(out.called_funcs, cv.s);
                            }
                        }
                    }
                    // §106 intrinsics + providers: schema-driven (op_info.intrinsic/native_provider — promoted to the
                    // runtime OpInfo at CEIR-7a), NEVER dialect-name-sniffing (I6). An unregistered kind is skipped here
                    // (op_info == nullptr) — the cook's find_unregistered_op rejects it FIRST, closing the vacuous read.
                    if (const OpInfo* const info = c.op_info(op->kind()); info != nullptr && info->intrinsic)
                    {
                        add_unique(out.intrinsics, c.op_name(op->kind()));
                        add_unique(out.providers, info->native_provider);
                    }
                    for (u32 i = 0; i < op->num_regions(); ++i) { go(c, op->region(i), ck, s, out); }
                }
            }
        }
    };
    W::go(ctx, module.body(), call_kind, syms, rec);
    sort_svs(rec.called_funcs);
    sort_svs(rec.intrinsics);
    sort_svs(rec.providers);
    return rec;
}

const Operation* find_unregistered_op(const Context& ctx, const Module& module) noexcept
{
    struct W
    {
        static const Operation* go(const Context& c, Region* r)
        {
            if (r == nullptr) { return nullptr; }
            for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
            {
                for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
                {
                    if (c.op_info(op->kind()) == nullptr) { return op; } // EMPTY≠UNKNOWN — an unregistered kind
                    for (u32 i = 0; i < op->num_regions(); ++i)
                    {
                        if (const Operation* f = go(c, op->region(i))) { return f; }
                    }
                }
            }
            return nullptr;
        }
    };
    return W::go(ctx, module.body());
}
} // namespace crd::ceir
