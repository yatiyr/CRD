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
// CEIR-8c: the effect family mask widened u32→u64; the §107 projection pushes all 8 bytes little-endian (by-field, the
// struct-padding-in-content-hash scar) so a ≥bit-32 family is part of the caller-visible interface hash.
void push_u64(containers::Array<u8>& out, u64 v)
{
    push_u32(out, static_cast<u32>(v & 0xFFFFFFFFU));
    push_u32(out, static_cast<u32>((v >> 32U) & 0xFFFFFFFFU));
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
    // CEIR-8a (ADR-0111 §2.4 landmine): the type-class discriminates an Extern type — emit it CONDITIONALLY (only for
    // kind==Extern) so two custom types with identical param slots do NOT collide in the §107 interface hash (which would
    // silently break 7b's registry-drift discriminator), while EVERY existing (non-Extern) type hashes byte-identically
    // to pre-8a (zero interface-hash churn — no needless recook).
    if (t.kind == TypeKind::Extern)
    {
        push_str(out, ctx.type_class_name(t.type_class));
        push_u32(out, t.type_class_version);
    }
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

namespace
{
// The CALLER-VISIBLE CONTRACT projection (CEIR-10a extraction — shared by interface_hash + contract_hash, byte-identical
// to the pre-10a inline form): each EXPORTED (Public) func SORTED BY NAME → {sym_name · param types · result types · 5c
// TRANSITIVE effect mask}, followed by the §57 capability contract (`push_caps_projection`). ⛔ NO state schema here —
// that is the one segment `contract_hash` omits so a signature change (Reject) is distinguished from a state-schema-only
// change (Migrate). A func reorder is an impl edit (sort by name, never body order).
void push_funcs_projection(Context& ctx, const Module& module, containers::Array<u8>& proj, memory::IAllocator* scratch)
{
    containers::Array<Operation*> funcs(scratch);
    collect_funcs(ctx, module, funcs);
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
        u64                                       mask = 0U; // CEIR-8c: u64 — a ≥bit-32 (U-§19) family must survive here
        ctx.collect_region_effective_mask(*f->region(0), q, mask);
        push_u64(proj, mask);
    }
}

// ⭐ CEIR-8f (ADR-0116 §2.1) CAPABILITY CONTRACT — the program's REQUIRED host-granted capability set (module-wide,
// sorted + deduped; an unregistered op contributes external.process). Part of the §107 swap-compatible identity (a host
// must re-grant if the set changes). ⛔ the count is pushed UNCONDITIONALLY even when 0 (the 8c no-conditional-width
// lesson — a cap-free module recooks too), then the sorted-unique id VALUES.
void push_caps_projection(Context& ctx, const Module& module, containers::Array<u8>& proj, memory::IAllocator* scratch)
{
    push_str(proj, containers::StringView("caps:"));
    containers::Array<CapabilityId> caps(scratch);
    ctx.program_capabilities(module, caps);
    push_u32(proj, static_cast<crd::u32>(caps.size()));
    for (crd::u32 i = 0; i < static_cast<crd::u32>(caps.size()); ++i) { push_u64(proj, caps[i].value); }
}

// §20 STATE SCHEMA walk — MODULE-WIDE (every StateEdge cell), NOT per-exported-func: a PRIVATE callee's cells are live
// runtime state a hot-swap must migrate; exported-only under-inclusion risks a wrong "compatible" verdict (state
// corruption), while over-inclusion only costs a spurious recook — the safety asymmetry decides it.
// ⭐ CEIR-8d (ADR-0114 §2.7): cells are keyed by their StateEdge op's STABLE ID (was body order). Collect
// (stable_id, value type, §20 depth), SORT by stable id — so a REORDER is invariant (the false-incompatible fixed),
// while the id VALUE keeps delete-id-1 + add-id-2 correctly INCOMPATIBLE (an order-only key would call them compatible
// and 10a migration would silently lose the id-1 state). ⛔ Assigns stable ids first.
void collect_state_cells(Context& ctx, const Module& module, containers::Array<StateCell>& cells)
{
    ctx.assign_stable_ids(module); // ensure every StateEdge op carries a stable id before we read it
    struct StateW
    {
        static void go(Context& c, Region* r, containers::Array<StateCell>& out)
        {
            if (r == nullptr) { return; }
            for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
            {
                for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
                {
                    if (c.has_trait(op->kind(), OpTrait::StateEdge) && op->num_results() >= 1U)
                    {
                        crd::u32     depth    = 1U; // §20 default depth
                        const AttrId depth_id = op->attr("depth");
                        if (depth_id.valid())
                        {
                            const AttrValue dv = c.attr_value(depth_id);
                            if (dv.kind == AttrKind::Int && dv.i >= 1) { depth = static_cast<crd::u32>(dv.i); }
                        }
                        out.push_back(StateCell{op->stable_id().value, op->result(0)->type(), depth});
                    }
                    for (crd::u32 i = 0; i < op->num_regions(); ++i) { go(c, op->region(i), out); }
                }
            }
        }
    };
    StateW::go(ctx, module.body(), cells);
    // insertion sort by stable id (cells are few; a stable id is unique per op, so the order is total + deterministic).
    for (crd::u32 i = 1; i < static_cast<crd::u32>(cells.size()); ++i)
    {
        const StateCell key = cells[i];
        crd::u32        j   = i;
        while (j > 0U && cells[j - 1U].id > key.id)
        {
            cells[j] = cells[j - 1U];
            --j;
        }
        cells[j] = key;
    }
}
} // namespace

u64 interface_hash(Context& ctx, const Module& module, memory::IAllocator* scratch)
{
    containers::Array<u8> proj(scratch);
    push_funcs_projection(ctx, module, proj, scratch);
    // §20 STATE SCHEMA folded in AT THIS byte position (unchanged from CEIR-8d): the "state:" marker, then each cell
    // (stable_id, type, depth) sorted by id. ⛔ Byte order is load-bearing — a change here recooks every 'CEIR' asset.
    push_str(proj, containers::StringView("state:"));
    containers::Array<StateCell> cells(scratch);
    collect_state_cells(ctx, module, cells);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(cells.size()); ++i)
    {
        push_u64(proj, cells[i].id); // ⛔ the id VALUE is hashed (the delete/re-add discriminator), not just the order
        encode_type(ctx, cells[i].type, proj);
        push_u32(proj, cells[i].depth);
    }
    push_caps_projection(ctx, module, proj, scratch);
    return fnv1a(containers::ConstSpan<u8>(proj.data(), proj.size()));
}

// §107 CONTRACT HASH (CEIR-10a) — `interface_hash`'s projection MINUS the state schema (funcs + caps only). `interface_hash`
// ≡ funcs + state + caps, so `contract_hash`-equal + `interface_hash`-differ ⇒ ONLY the state schema changed → a migration
// fn may cover it; a `contract_hash` difference means callers break (Reject). PURE analysis — the cooked format is untouched.
u64 contract_hash(Context& ctx, const Module& module, memory::IAllocator* scratch)
{
    containers::Array<u8> proj(scratch);
    push_funcs_projection(ctx, module, proj, scratch);
    push_caps_projection(ctx, module, proj, scratch);
    return fnv1a(containers::ConstSpan<u8>(proj.data(), proj.size()));
}

containers::Array<StateCell> collect_state_schema(Context& ctx, const Module& module, memory::IAllocator* alloc)
{
    containers::Array<StateCell> cells(alloc);
    collect_state_cells(ctx, module, cells);
    return cells;
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
// CEIR-13c: a KernelRefDep list deduped by NAME (a kernel is one dependency; the first-seen pin is kept — the authoritative
// per-dispatch contract check re-walks the module at cook), sorted by name (the sort_svs precedent).
void add_unique_kref(containers::Array<KernelRefDep>& list, const KernelRefDep& k)
{
    if (k.name.empty()) { return; }
    for (u32 i = 0; i < static_cast<u32>(list.size()); ++i)
    {
        if (list[i].name == k.name) { return; }
    }
    list.push_back(k);
}
void sort_krefs(containers::Array<KernelRefDep>& list)
{
    for (u32 i = 1; i < static_cast<u32>(list.size()); ++i)
    {
        const KernelRefDep key = list[i];
        u32                j   = i;
        while (j > 0U && sv_less(key.name, list[j - 1U].name))
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
                    // §106 CKIR KernelRefs (CEIR-13c): SCHEMA-DRIVEN from op_info.kernel_ref_symbol (the [op.kernel_ref]
                    // marker), NEVER op-name-sniffing (I6 — an op merely NAMED "kernel" on some other dialect is not one).
                    // Read the kernel identity (the symbol attr) + the OPTIONAL interface-hash PIN (the int attr's u64 bit
                    // pattern; absent ⇒ unpinned, a dependency-only ref).
                    if (const OpInfo* const info = c.op_info(op->kind());
                        info != nullptr && !info->kernel_ref_symbol.empty())
                    {
                        const AttrId sid = op->attr(info->kernel_ref_symbol);
                        if (sid.valid())
                        {
                            const AttrValue sv = c.attr_value(sid);
                            if (sv.kind == AttrKind::SymbolRef)
                            {
                                KernelRefDep k{sv.s, 0U, false};
                                if (!info->kernel_ref_interface.empty())
                                {
                                    const AttrId iid = op->attr(info->kernel_ref_interface);
                                    if (iid.valid())
                                    {
                                        const AttrValue iv = c.attr_value(iid);
                                        if (iv.kind == AttrKind::Int)
                                        {
                                            k.interface_hash = static_cast<u64>(iv.i);
                                            k.pinned         = true;
                                        }
                                    }
                                }
                                add_unique_kref(out.ckir_refs, k);
                            }
                        }
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
    sort_krefs(rec.ckir_refs); // CEIR-13c: distinct + sorted by name (deterministic), like the string lists above
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
