#pragma once

// crd-ceir — CEIR-28a sec-80 the tune dialect MODEL + cache reader: an AUTHORED `ceir.tune` module (gen/tune_ops.hpp, the
// tune.entry op) IS the TARGET-SPECIFIC CONFIGURATION CACHE (the sec-80 proof). Each tune.entry is one ROW keying
// (device, env, program_hash, shape) -> a schedule (fuse, share = the CEIR-27 PlanOptions, inlined). This header owns the DATA
// model (TuneEntry), the module-wide misuse walk (find_tune_misuse: DuplicateKey), and the load-into-data (load_tune_entries).
// The PlanOptions mapping + the hit/miss LOOKUP live in tensor_pipeline (plan_options_from_tune_cache), next to PlanOptions.
//
// ⛔ the module-wide 'at most one row per KEY' rule is find_tune_misuse's, NOT the generated verifier's (which owns the per-row
//    required-attr contract) -- the find_resource/quant/transform/rewrite_misuse HOUSE pattern (const Context, matches op NAME
//    via ctx.op_name per I6, NOT op.kind -- interning mutates; region-recursive pre-order).

#include <crd/ceir/attr.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/tune_ops.hpp> // tune.entry op + entry_kind (like rewrite_rules.hpp depends on gen/rewrite_ops.hpp)
#include <crd/ceir/ir.hpp>
#include <crd/ceir/print.hpp> // CEIR-28b: canonical print -> program_hash (the program-identity KEY producer)

#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp> // CEIR-28b: fnv1a_64 (the runtime hash over the printed bytes)
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir::tune
{
// One loaded cache row (the data behind a tune.entry op). The string views point into the module's interned attr storage --
// valid as long as the module + ctx live. fuse/share ARE the CEIR-27 PlanOptions.
struct TuneEntry
{
    containers::StringView device;
    containers::StringView env;
    u64                    program_hash = 0; // the fnv1a-64 of the payload's canonical print (28b program_hash producer)
    containers::StringView shape;
    bool                   fuse  = false;
    bool                   share = false;
};

// The module-wide misuse verifier (the house pattern). NOLINTNEXTLINE(performance-enum-size)
enum class TuneMisuseKind : u8
{
    None = 0,
    DuplicateKey, // two tune.entry rows share the SAME (device, env, program_hash, shape) KEY -- the loader would be ambiguous
};
struct TuneMisuse
{
    const Operation* op   = nullptr;
    TuneMisuseKind   kind = TuneMisuseKind::None;
};
[[nodiscard]] inline containers::StringView tune_misuse_kind_name(TuneMisuseKind k) noexcept
{
    switch (k)
    {
    case TuneMisuseKind::None: return containers::StringView("none");
    case TuneMisuseKind::DuplicateKey: return containers::StringView("duplicate-key");
    }
    return containers::StringView("?");
}

namespace detail
{
[[nodiscard]] inline containers::StringView entry_str(const Context& ctx, const Operation& op, containers::StringView name) noexcept
{
    const AttrId a = op.attr(name);
    if (!a.valid()) { return {}; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::String ? v.s : containers::StringView();
}
[[nodiscard]] inline u64 entry_u64(const Context& ctx, const Operation& op, containers::StringView name) noexcept
{
    const AttrId a = op.attr(name);
    if (!a.valid()) { return 0U; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::Int ? static_cast<u64>(v.i) : 0U; // i64 storage read as an opaque u64 KEY (no truncation)
}
[[nodiscard]] inline bool entry_bool(const Context& ctx, const Operation& op, containers::StringView name) noexcept
{
    const AttrId a = op.attr(name);
    if (!a.valid()) { return false; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::Bool && v.b;
}
[[nodiscard]] inline TuneEntry read_entry(const Context& ctx, const Operation& op) noexcept
{
    TuneEntry e;
    e.device       = entry_str(ctx, op, containers::StringView("device"));
    e.env          = entry_str(ctx, op, containers::StringView("env"));
    e.program_hash = entry_u64(ctx, op, containers::StringView("program_hash"));
    e.shape        = entry_str(ctx, op, containers::StringView("shape"));
    e.fuse         = entry_bool(ctx, op, containers::StringView("fuse"));
    e.share        = entry_bool(ctx, op, containers::StringView("share"));
    return e;
}
[[nodiscard]] inline bool key_eq(const TuneEntry& a, const TuneEntry& b) noexcept
{
    return a.device == b.device && a.env == b.env && a.program_hash == b.program_hash && a.shape == b.shape;
}
// Collect every tune.entry op in `r` (region-recursive, document pre-order) into `out` -- the MODULE-WIDE walk that BOTH
// find_tune_misuse and load_tune_entries share, so they agree on scope: a duplicate KEY split across two blocks is CAUGHT by
// the misuse walk (not missed by a per-block check while load loads both -- the silent find/load disagreement, advisor 28a).
inline void collect_tune_entries(const Context& ctx, const Region* r, containers::Array<const Operation*>& out) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == containers::StringView("tune.entry")) { out.push_back(op); }
            for (u32 i = 0; i < op->num_regions(); ++i) { collect_tune_entries(ctx, op->region(i), out); }
        }
    }
}
} // namespace detail

// The FIRST MODULE-WIDE DuplicateKey (document order), or {None}. ⛔ MODULE-WIDE (all blocks + nested regions) -- the SAME scope
// as load_tune_entries, so a duplicate across two blocks is rejected here, never silently loaded (28a(f) is the discriminating
// gate). const -- reads names + attrs, interns nothing; uses ctx.allocator() (const-accessible) for the collect.
[[nodiscard]] inline TuneMisuse find_tune_misuse(const Context& ctx, const Module& m)
{
    containers::Array<const Operation*> ents(ctx.allocator());
    detail::collect_tune_entries(ctx, m.body(), ents);
    for (usize i = 0; i < ents.size(); ++i)
    {
        const TuneEntry ei = detail::read_entry(ctx, *ents[i]);
        for (usize j = 0; j < i; ++j)
        {
            if (detail::key_eq(detail::read_entry(ctx, *ents[j]), ei)) { return {ents[i], TuneMisuseKind::DuplicateKey}; }
        }
    }
    return {};
}

// Load every tune.entry in `cache_mod` into `out` (MODULE-WIDE, the SAME scope as find_tune_misuse; assumes it clean). Returns
// the count appended. Matches the op by NAME (const Context, interns nothing) -- the I6 rule.
[[nodiscard]] inline u32 load_tune_entries(const Context& ctx, const Module& cache_mod, containers::Array<TuneEntry>& out)
{
    containers::Array<const Operation*> ents(ctx.allocator());
    detail::collect_tune_entries(ctx, cache_mod.body(), ents);
    for (usize i = 0; i < ents.size(); ++i) { out.push_back(detail::read_entry(ctx, *ents[i])); }
    return static_cast<u32>(ents.size());
}

// The program-identity KEY producer: the fnv1a-64 of the module's CANONICAL PRINT. ⛔ hash the module the PLANNER consumes --
// the POST-EXPANSION payload (an ml.mlp and its expanded gemm/relu are DIFFERENT programs to the planner) and PRE-plan (the plan
// depends on PlanOptions, the thing being CHOSEN). print-equal modules hash-equal BY CONSTRUCTION (the roundtrip identity 27's
// differentials use); a changed operand type changes the print, hence the hash. Stored in tune.entry.program_hash (i64 storage,
// read back as an opaque u64). ⛔ takes Context& (print is non-const) + an allocator (print needs one -- not hidden).
[[nodiscard]] inline u64 program_hash(Context& ctx, const Module& m, memory::IAllocator* alloc)
{
    const containers::String s = print(ctx, m, alloc);
    return containers::fnv1a_64(s.c_str(), s.size());
}
} // namespace crd::ceir::tune
