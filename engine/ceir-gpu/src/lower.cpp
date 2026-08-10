// crd-ceir-gpu — CEIR-13d the GPU lowering pass (ADR-0125). See lower.hpp for the contract.

#include <crd/ceir/gpu/lower.hpp>

#include <crd/ceir/effect.hpp> // EffectRecord, EffectTarget

namespace crd::ceir::gpu
{
namespace
{
// One precise resource access of a lowered op: the resource Value (nullptr ⇒ whole-Memory / ambient), reads, writes.
struct Access
{
    const Value* resource = nullptr;
    bool         reads    = false;
    bool         writes   = false;
};

// Parse the `access` string into per-binding tokens (1=r, 2=w, 3=rw), comma-separated {r,w,rw}. false on any bad token.
[[nodiscard]] bool parse_access(containers::StringView s, containers::Array<crd::u8>& toks)
{
    toks.clear();
    if (s.size() == 0U) { return true; }
    crd::usize start = 0;
    for (crd::usize i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ',')
        {
            const crd::usize  len = i - start;
            const char* const t   = s.data() + start;
            crd::u8           v   = 0U;
            if (len == 1U && t[0] == 'r') { v = 1U; }
            else if (len == 1U && t[0] == 'w') { v = 2U; }
            else if (len == 2U && t[0] == 'r' && t[1] == 'w') { v = 3U; }
            else { return false; }
            toks.push_back(v);
            start = i + 1U;
        }
    }
    return true;
}

// Gather an op's PRECISE memory accesses. ⛔ mirrors the core's op_access_at + gather_accesses (context.cpp) — the conflict
// rule below mirrors accesses_conflict. A dispatch reads its bindings + `access` (the 13a AMBIENT MemoryReadWrite NARROWED
// here); a MALFORMED/absent access DEGRADES to whole-Memory rw (conservative — standalone-robust, downstream of
// find_dispatch_misuse but never precise-but-wrong). Every OTHER op reads its static per-operand effects (transfers are
// precise by construction). ⛔ EMPTY≠UNKNOWN: an UNREGISTERED op ⇒ whole-Memory rw (it must still barrier). ⛔ GPUCommand
// (Gpu class) is ordering-INERT here — submission/stream order is the executor's, so no memory barrier. ⭐ CEIR-13d part 3
// (12c VIEW HOLE CLOSED): every captured resource is NORMALIZED through `ctx.resource_root`, so a binding of view(%buf)
// conflicts with upload(%buf) — the core + bridge retrofit landed in ONE move. A view laundered through a yield/call result
// still escapes (its root is the yield/call, not the buffer).
void gather(const Context& ctx, const Operation* op, containers::Array<Access>& out)
{
    out.clear();
    const containers::StringView nm       = ctx.op_name(op->kind());
    const bool                   direct   = nm == containers::StringView("compute.dispatch");
    const bool                   indirect = nm == containers::StringView("compute.dispatch_indirect");
    if (direct || indirect)
    {
        const crd::u32             fixed = direct ? 3U : 1U; // grid(3) vs args(1)
        const crd::u32             nbind = op->num_operands() >= fixed ? op->num_operands() - fixed : 0U;
        containers::Array<crd::u8> toks(ctx.allocator());
        const AttrValue            av = ctx.attr_value(op->attr(containers::StringView("access")));
        const bool ok = av.kind == AttrKind::String && parse_access(av.s, toks) && static_cast<crd::u32>(toks.size()) == nbind;
        if (!ok)
        {
            out.push_back({nullptr, true, true}); // malformed/absent access ⇒ conservative whole-Memory rw
        }
        else
        {
            for (crd::u32 i = 0; i < nbind; ++i)
            {
                // ⭐ CEIR-13d part 3: NORMALIZE to the view-root so a binding of view(%buf) conflicts with upload(%buf).
                out.push_back({ctx.resource_root(op->operand(fixed + i)), (toks[i] & 1U) != 0U, (toks[i] & 2U) != 0U});
            }
        }
        if (indirect && op->num_operands() >= 1U)
        {
            out.push_back({ctx.resource_root(op->operand(0U)), true, false}); // args buffer read (view-normalized)
        }
        return; // ⛔ GPUCommand ignored (ordering-inert); the ambient MemoryReadWrite is REPLACED by the precise bindings
    }
    const OpInfo* const info = ctx.op_info(op->kind());
    if (info == nullptr)
    {
        out.push_back({nullptr, true, true}); // ⛔ EMPTY≠UNKNOWN: unregistered ⇒ whole-Memory rw
        return;
    }
    const containers::ConstSpan<EffectRecord> effs = ctx.op_effects(op->kind());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(effs.size()); ++i)
    {
        const EffectRecord& e = effs[i];
        const EffectAccess  a = effect_access(e.family);
        if (a.klass != ResourceClass::Memory && a.klass != ResourceClass::Universe) { continue; } // memory-orderable only
        const Value* res = nullptr;
        if (e.target == EffectTarget::Operand && e.index < op->num_operands()) { res = op->operand(e.index); }
        else if (e.target == EffectTarget::Result && e.index < op->num_results()) { res = op->result(e.index); }
        out.push_back({ctx.resource_root(res), a.reads, a.writes}); // ⭐ CEIR-13d part 3: view-normalized (nullptr passes through)
    }
}

// Two accesses conflict iff both touch memory, ≥1 writes, and they share a resource (same Value, or either nullptr = whole
// class). ⛔ mirrors context.cpp::accesses_conflict; distinct Values are non-aliasing but `gather` view-NORMALIZES every
// resource through `resource_root` first (CEIR-13d part 3), so distinct views of one buffer DO conflict here.
[[nodiscard]] bool conflict(const Access& a, const Access& b)
{
    if (!(a.reads || a.writes) || !(b.reads || b.writes)) { return false; }
    if (!(a.writes || b.writes)) { return false; } // read-read: no order
    return a.resource == b.resource || a.resource == nullptr || b.resource == nullptr;
}
[[nodiscard]] HazardKind pair_hazard(const Access& a, const Access& b) // a BEFORE b (mirrors context.cpp::pair_hazard)
{
    if (a.writes && b.writes) { return HazardKind::Waw; }
    if (a.writes && b.reads) { return HazardKind::Raw; }
    if (a.reads && b.writes) { return HazardKind::War; }
    return HazardKind::None;
}
// ⛔ CEIR-13z-3: the whole-op `precise_hazard` (op-vs-op strongest) was RETIRED — `lower_region` now scans conflicts
// PER-RESOURCE inline (a dispatch reading N buffers from N prior passes needs N barriers, which one op-vs-op strongest
// dropped). `conflict` + `pair_hazard` + `hazard_rank` (the per-pair primitives) stay — they ARE the per-resource core; the
// two-hazard-notions design still holds (this bridge scan stays narrowed; core `ops_hazard` stays conservative).

// Resolve a grid operand to a compile-time u32 (arith.const `value`) — part 1b, all-or-nothing.
[[nodiscard]] bool resolve_const_u32(const Context& ctx, const Value* v, crd::u32& out)
{
    const Operation* const def = v->defining_op();
    if (def == nullptr) { return false; } // block arg ⇒ dynamic dispatch dims (§42)
    if (ctx.op_name(def->kind()) != containers::StringView("arith.const")) { return false; }
    const AttrValue av = ctx.attr_value(def->attr(containers::StringView("value")));
    if (av.kind != AttrKind::Int) { return false; }
    out = static_cast<crd::u32>(av.i);
    return true;
}
// The 13b transfer op → LoweredTransferKind; false if `nm` is not a transfer op.
[[nodiscard]] bool transfer_kind_of(containers::StringView nm, LoweredTransferKind& out)
{
    if (nm == containers::StringView("transfer.copy")) { out = LoweredTransferKind::Copy; return true; }
    if (nm == containers::StringView("transfer.upload")) { out = LoweredTransferKind::Upload; return true; }
    if (nm == containers::StringView("transfer.readback")) { out = LoweredTransferKind::Readback; return true; }
    if (nm == containers::StringView("transfer.clear")) { out = LoweredTransferKind::Clear; return true; }
    if (nm == containers::StringView("transfer.mip_gen")) { out = LoweredTransferKind::MipGen; return true; }
    return false;
}
} // namespace

void lower_region(const Context& ctx, const Block& block, containers::Array<LoweredCommand>& out)
{
    out.clear();
    containers::Array<const Operation*> earlier(ctx.allocator()); // the earlier-emitted lowered ops (dispatch + transfer)
    containers::Array<Access>           aacc(ctx.allocator());     // scratch (earlier op)
    containers::Array<Access>           bacc(ctx.allocator());     // scratch (this op)
    for (const Operation* op = block.first_op(); op != nullptr; op = op->next_in_block())
    {
        const containers::StringView nm       = ctx.op_name(op->kind());
        const bool                   direct   = nm == containers::StringView("compute.dispatch");
        const bool                   indirect = nm == containers::StringView("compute.dispatch_indirect");
        LoweredTransferKind          tk       = LoweredTransferKind::Copy;
        const bool                   transfer = transfer_kind_of(nm, tk);
        if (!(direct || indirect || transfer)) { continue; } // Pure consts/declares emit nothing

        // BARRIER: PER-RESOURCE (⭐ CEIR-13z-3). For each ROOT resource `op` accesses (binding-operand order, deduped), the
        // strongest hazard from any earlier lowered op touching it + the NEAREST such op (the last writer, reverse scan). A
        // dispatch reading N buffers written by N prior passes emits N barriers — the part-2 "one barrier per dispatch,
        // strongest" DROPPED the other conflicts (a 13d correctness completion). A nullptr (ambient) resource conflicts with
        // everything → one whole-class barrier.
        gather(ctx, op, bacc);
        for (crd::u32 bi = 0; bi < static_cast<crd::u32>(bacc.size()); ++bi)
        {
            const Value* const res = bacc[bi].resource;
            bool               seen = false; // dedup: an in-place read+write binds one resource twice
            for (crd::u32 s = 0; s < bi; ++s)
            {
                if (bacc[s].resource == res) { seen = true; break; }
            }
            if (seen) { continue; }

            HazardKind       strongest = HazardKind::None;
            const Operation* bef       = nullptr;
            for (crd::usize k = earlier.size(); k > 0U; --k)
            {
                const Operation* const e = earlier[k - 1U];
                gather(ctx, e, aacc);
                for (crd::u32 ai = 0; ai < static_cast<crd::u32>(aacc.size()); ++ai)
                {
                    if (conflict(aacc[ai], bacc[bi]))
                    {
                        const HazardKind h = pair_hazard(aacc[ai], bacc[bi]);
                        if (hazard_rank(h) > hazard_rank(strongest))
                        {
                            strongest = h;
                            bef       = e;
                        }
                    }
                }
            }
            if (strongest != HazardKind::None)
            {
                LoweredCommand b;
                b.kind     = LoweredKind::Barrier;
                b.hazard   = strongest;
                b.before   = bef;
                b.after    = op;
                b.resource = res;
                out.push_back(b);
            }
        }

        LoweredCommand c;
        c.op = op;
        if (transfer)
        {
            c.kind          = LoweredKind::Transfer;
            c.transfer_kind = tk;
            if (tk == LoweredTransferKind::Clear) // §162: resolve the fill word (an attr read, the grid-resolution precedent)
            {
                const AttrId vid = op->attr(containers::StringView("value"));
                if (vid.valid())
                {
                    const AttrValue vv = ctx.attr_value(vid);
                    if (vv.kind == AttrKind::Int)
                    {
                        c.clear_value     = vv.i;
                        c.has_clear_value = true;
                    }
                }
            }
        }
        else
        {
            c.kind          = LoweredKind::Dispatch;
            c.dispatch_kind = indirect ? crd::gpu::DispatchKind::Indirect : crd::gpu::DispatchKind::Direct;
            if (direct && op->num_operands() >= 3U)
            {
                crd::u32 gx = 0U;
                crd::u32 gy = 0U;
                crd::u32 gz = 0U;
                if (resolve_const_u32(ctx, op->operand(0U), gx) && resolve_const_u32(ctx, op->operand(1U), gy)
                    && resolve_const_u32(ctx, op->operand(2U), gz))
                {
                    c.groups_x = gx;
                    c.groups_y = gy;
                    c.groups_z = gz;
                }
                else
                {
                    c.dynamic_grid = true;
                }
            }
        }
        out.push_back(c);
        earlier.push_back(op);
    }
}
} // namespace crd::ceir::gpu
