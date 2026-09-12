#include <crd/ceir/gpu/tensor_pipeline.hpp>

#include <crd/ceir/gpu/partition_ml.hpp> // CEIR-29c-1: MlPartition — plan_tensor_pipeline_partitioned tags stages by provider
#include <crd/ceir/attr.hpp>   // AttrValue / AttrKind (the dequantize `scheme` attr — the symmetric-per-tensor plan gate)
#include <crd/ceir/func.hpp>   // func::func_body_block
#include <crd/ceir/ir.hpp>     // Block / Operation / Value traversal
#include <crd/ceir/linalg.hpp> // find_linalg_misuse
#include <crd/ceir/quant.hpp>  // find_quant_misuse (CEIR-23b: quant.dequantize is a planned op)
#include <crd/ceir/tensor.hpp> // find_tensor_misuse
#include <crd/ceir/type.hpp>   // Type / TypeKind / DimKind
#include <crd/ceir/gen/transform_ops.hpp> // CEIR-27a: transform.fuse / transform.share_storage directive kinds
#include <crd/ceir/tune.hpp>              // CEIR-28a: tune.entry cache rows + load_tune_entries (the config-cache replay)
#include <crd/kir/ckir.hpp>    // kir::KGraph (a scratch graph for the per-stage synth typed-reject check)

namespace crd::ceir::gpu
{
namespace
{
using containers::StringView;

// The buffer that realizes SSA Value `v` (the def-use "same buffer" key), or -1.
[[nodiscard]] crd::i32 find_buffer(const TensorPipelinePlan& plan, const Value* v) noexcept
{
    for (usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].value == v) { return static_cast<crd::i32>(i); }
    }
    return -1;
}
// Byte size of a scalar element type; 0 for anything NOT a whole-byte Int or a recognized Float. ⛔ NO silent default — a wrong
// element size rots silently (the POD-hash scar class); 0 propagates to tensor_bytes = a reject (the belt-and-braces path).
[[nodiscard]] crd::u64 element_bytes(const Context& ctx, TypeId elem) noexcept
{
    const Type e = ctx.type_of(elem);
    if (e.kind == TypeKind::Float)
    {
        switch (e.fkind)
        {
        case FloatKind::F16:
        case FloatKind::BF16: return 2ULL;
        case FloatKind::F32: return 4ULL;
        case FloatKind::F64: return 8ULL;
        case FloatKind::F8E4M3:
        case FloatKind::F8E5M2: return 1ULL;
        }
        return 0ULL; // a future FloatKind — reject until sized (the widen-enum-audit discipline)
    }
    if (e.kind == TypeKind::Int) { return (e.count > 0U && e.count % 8U == 0U) ? static_cast<crd::u64>(e.count) / 8ULL : 0ULL; }
    return 0ULL; // Bool / Index / aggregate / unknown — reject (never a silent size)
}
// Byte size of a Tensor type's (all-static) shape × its ELEMENT size; 0 if malformed / a dynamic dim / an unrecognized element
// (planning is post-verify + post-synth, which already reject those — this is the belt-and-braces path). ⛔ the element size is
// the ELEMENT's (f32=4, i8=1 for the u32-packed Q8 weights — CEIR-23b; N int8 = N bytes == the u32-packed device size), NOT a
// hardcoded 4.
[[nodiscard]] crd::u64 tensor_bytes(const Context& ctx, TypeId t) noexcept
{
    const Type tt = ctx.type_of(t);
    if (tt.members.size() < 2U) { return 0; }
    const crd::u64 eb = element_bytes(ctx, tt.members[0]);
    if (eb == 0ULL) { return 0; }
    const Type sh = ctx.type_of(tt.members[1]);
    crd::u64   n  = 1;
    for (usize i = 0; i < sh.members.size(); ++i)
    {
        const Type d = ctx.type_of(sh.members[i]);
        if (static_cast<DimKind>(d.cols) != DimKind::Static) { return 0; }
        n *= static_cast<crd::u64>(d.count);
    }
    return n * eb;
}
// Append a buffer, return its index.
[[nodiscard]] crd::i32 add_buffer(TensorPipelinePlan& plan, const PlanBuffer& b)
{
    const crd::i32 idx = static_cast<crd::i32>(plan.buffers.size());
    plan.buffers.push_back(b);
    return idx;
}
// Parse a compute.dispatch `access` string (comma-separated r|w|rw) into per-binding WRITE flags; returns the token count.
// is_write[i]=true ONLY for a bare `w` token (⛔ `rw` is treated as a READ for wiring — an in-place read-write viz binding is
// out of the 22c-3 proof scope, name-forward). ⛔ ASSUMES the module is dispatch-verify-clean (find_dispatch_misuse ran first:
// every token is valid r|w|rw + the count == the binding count), so no malformed-token path is needed here.
[[nodiscard]] crd::u32 parse_write_flags(StringView s, bool (&is_write)[8]) noexcept
{
    for (auto& w : is_write) { w = false; }
    crd::u32   count = 0;
    crd::usize start = 0;
    for (crd::usize i = 0; i <= s.size(); ++i)
    {
        if (s.size() == 0U) { break; } // empty access string = zero bindings
        if (i == s.size() || s[i] == ',')
        {
            if (count < 8U) { is_write[count] = (i - start == 1U && s[start] == 'w'); }
            ++count;
            start = i + 1U;
        }
    }
    return count;
}
// ⭐ CEIR-23b: is `dq` (a quant.dequantize) the SYMMETRIC, PER-TENSOR (rank-0 scale) form the plan-path kernels handle? The
// symmetric Q8 kernels (quant_dequantize_q8_sym.ckir + quant_gemm_q8.ckir) read scale[0] and DROP the zero_point subtract — so
// an ASYMMETRIC (zp≠0) or PER-AXIS (rank-1 scale) dequantize is NOT plannable here. Name-forward ⇒ a TYPED reject at the walk +
// non-fusable at the predicate, NEVER a silent symmetric miscompile (the advisor's silent-symmetric scar). ⛔ ONE definition —
// both the fusion predicate AND the unfused Dequant walk consult THIS (three hand copies would drift). Pre-verify-clean
// (find_quant_misuse None) guarantees the attr kind + operand arity this reads (belt-and-braces guards remain).
[[nodiscard]] bool dequant_is_symmetric_per_tensor(const Context& ctx, const Operation* dq) noexcept
{
    if (dq == nullptr || dq->num_operands() < 2U) { return false; }
    const AttrValue sc = ctx.attr_value(dq->attr(StringView("scheme")));
    if (sc.kind != AttrKind::String || sc.s != StringView("symmetric")) { return false; } // symmetric only (no zp subtract)
    const Type stt = ctx.type_of(dq->operand(1U)->type());                                 // scale (operand-1)
    if (stt.members.size() < 2U) { return false; }                                         // not a well-formed tensor
    return ctx.type_of(stt.members[1]).members.size() == 0U;                               // scale shape RANK-0 (per-tensor)
}
// ⭐ CEIR-23b-2b: is `g` (a linalg.gemm) the PLAIN form the fused quant-gemm kernel computes — D = A·B with alpha==1, beta==0,
// no transpose? The fused quant_gemm_q8 kernel applies NO alpha, adds NO beta·C, and indexes A[M,K]·W[K,N] row-major, so a
// scaled / accumulating (beta·C) / transposed gemm would SILENTLY MISCOMPILE under it (the same full-semantic-attr class as
// dequant_is_symmetric_per_tensor — the advisor's silent-miscompile scar). ⛔ AttrValue.f is the f64 BIT PATTERN as u64
// (1.0 = 0x3ff0…, 0.0 = 0x0), not the value — compare bits, no reinterpret needed.
[[nodiscard]] bool gemm_is_plain(const Context& ctx, const Operation* g) noexcept
{
    const AttrValue al = ctx.attr_value(g->attr(StringView("alpha")));
    const AttrValue be = ctx.attr_value(g->attr(StringView("beta")));
    const AttrValue ta = ctx.attr_value(g->attr(StringView("trans_a")));
    const AttrValue tb = ctx.attr_value(g->attr(StringView("trans_b")));
    if (al.kind != AttrKind::Float || al.f != 0x3ff0000000000000ULL) { return false; } // alpha == 1.0
    if (be.kind != AttrKind::Float || be.f != 0x0ULL) { return false; }                // beta  == +0.0
    if (ta.kind == AttrKind::Bool && ta.b) { return false; }                           // trans_a == false
    if (tb.kind == AttrKind::Bool && tb.b) { return false; }                           // trans_b == false
    return true;
}
} // namespace

bool fusable_dequant_into_gemm_weight(const Context& ctx, const Operation* dequant_op) noexcept
{
    if (dequant_op == nullptr || dequant_op->num_results() < 1U) { return false; }
    if (ctx.op_name(dequant_op->kind()) != StringView("quant.dequantize")) { return false; }
    if (!dequant_is_symmetric_per_tensor(ctx, dequant_op)) { return false; } // ⛔ fuse ONLY the symmetric per-tensor form
    const Value* r = dequant_op->result(0U);
    if (r == nullptr || r->num_uses() != 1U) { return false; }   // ⛔ EXACTLY one use (multi-use → unfused fallback)
    const Use* u = r->first_use();
    if (u == nullptr || u->owner == nullptr) { return false; }
    const Operation* g = u->owner;                               // the sole using op
    if (ctx.op_name(g->kind()) != StringView("linalg.gemm")) { return false; }
    if (g->num_operands() < 2U || g->operand(1U) != r) { return false; } // ⛔ specifically the gemm's WEIGHT slot (operand-1)
    return gemm_is_plain(ctx, g); // ⛔ the fused kernel is alpha=1 β=0 no-transpose — a scaled/accumulating/transposed gemm miscompiles
}

bool fusable_gemm_into_relu(const Context& ctx, const Operation* gemm_op) noexcept
{
    if (gemm_op == nullptr || gemm_op->num_results() < 1U) { return false; }
    if (ctx.op_name(gemm_op->kind()) != StringView("linalg.gemm")) { return false; }        // (1) op-name (cheapest)
    if (!gemm_is_plain(ctx, gemm_op)) { return false; }                                     // (2) α=1 β=0 no-transpose (no fused form)
    // (3) ⛔⛆ the WEIGHT (operand-1) is NOT itself a fusable quant.dequantize — a QuantGemm target's f32 result is never allocated,
    //     so folding it here → DanglingOperand + the QuantGemm gates go red. The quant MLP `dequant→gemm→relu` is BOTH; f32-weight only.
    const Operation* const wdq = gemm_op->num_operands() >= 2U ? gemm_op->operand(1U)->defining_op() : nullptr;
    if (wdq != nullptr && fusable_dequant_into_gemm_weight(ctx, wdq)) { return false; }
    // (4) result is single-use by a compute.dispatch{kernel=="relu"} reading bind[0] (operand-3), with the {grid×3, r, w} arity.
    const Value* const r = gemm_op->result(0U);
    if (r == nullptr || r->num_uses() != 1U) { return false; }
    const Use* const u = r->first_use();
    if (u == nullptr || u->owner == nullptr) { return false; }
    const Operation* const d = u->owner;
    if (ctx.op_name(d->kind()) != StringView("compute.dispatch")) { return false; }
    const AttrValue kv = ctx.attr_value(d->attr(StringView("kernel")));
    if (kv.kind != AttrKind::SymbolRef || kv.s != StringView("relu")) { return false; }
    return d->num_operands() == 5U && d->operand(3U) == r; // grid×3 + bind[0]=read(gemm result) + bind[1]=write(h)
}

namespace
{
// ⭐ CEIR-26f — SINGLE-PASS free-list buffer-aliasing: a shareable Intermediate freed STRICTLY before another is born lends its
// physical storage (the tenant's `alias_of` = the LANDLORD root, the landlord's `bytes` grows to max). The plan is a LINEAR CHAIN
// ⇒ per-buffer [produce_stage, last_read_stage] intervals suffice; NO interval-graph coloring. ⛔ PINS func.return operands (a
// readback target keeps its OWN buffer — the 25c-2 dW2 named-out trap) + never shares ExternalIn / Output / value==nullptr.
// ⛔ free_at[L] < produce[B] is STRICT: L's last reader ran at a stage BEFORE B's producing stage, so the per-stage execution
// barriers serialize L's read before B's write — no in-stage output-aliases-input hazard. PURE (mutates plan.buffers only).
void assign_shared_storage(const Context& ctx, Block* body, TensorPipelinePlan& plan)
{
    const usize nb = plan.buffers.size();
    if (nb == 0U || nb > 32U) { return; } // >32 ⇒ skip sharing (conservative; matches the executor's buffer cap)

    // func.return operands are PINNED (read back by the caller — never a tenant/landlord).
    const auto is_returned = [&](const Value* v) -> bool {
        if (v == nullptr) { return false; }
        for (Operation* op = body->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) != StringView("func.return")) { continue; }
            for (crd::u32 i = 0; i < op->num_operands(); ++i) { if (op->operand(i) == v) { return true; } }
        }
        return false;
    };

    // ⛔ advisor 26f-2b: a stage with n_out==0 (an all-read dispatch) emits NO barrier (the executor's per-stage
    // ShaderWrite→ShaderRead barrier rides the trailing outputs). So a buffer whose LAST reader is such a stage has no
    // barrier before a later write to its storage → it must NEVER lend (a WAR that nothing orders). never_free pins its
    // last_read past every produce stage ⇒ it is never chosen as a landlord. Latent (no corpus has an n_out==0 non-final
    // stage today) but the free-list would happily alias across one. Every ACTUAL tenancy stays safe: a landlord is chosen
    // only when its last_read is a real stage < produce[b], and that stage (having a barrier-bearing output) orders the WAR.
    const crd::i32 never_free = 0x7fffffff;

    crd::i32 produce[32];
    crd::i32 last_read[32];
    crd::i32 free_at[32]; // for a ROOT landlord: the stage after which its storage is available (its own or its latest tenant's last read)
    bool     shareable[32];
    for (usize i = 0; i < nb; ++i)
    {
        produce[i]          = -1;
        last_read[i]        = -1;
        const PlanBuffer& b = plan.buffers[i];
        shareable[i]        = b.role == BufferRole::Intermediate && b.value != nullptr && !is_returned(b.value);
    }
    // produce = the stage whose TRAILING output is this buffer; last_read = the MAX stage that binds it as a NON-output (read).
    for (usize s = 0; s < plan.stages.size(); ++s)
    {
        const PlanStage& st        = plan.stages[s];
        const crd::u32   first_out = st.nbind - st.n_out;
        for (crd::u32 k = 0; k < st.nbind; ++k)
        {
            const crd::i32 bi = st.bind[k];
            if (bi < 0 || static_cast<usize>(bi) >= nb) { continue; }
            if (k >= first_out) { produce[static_cast<usize>(bi)] = static_cast<crd::i32>(s); }
            // n_out==0 reader ⇒ never_free (see the barrier note above); a later normal read overwrites it (max-in-stage-order).
            else { last_read[static_cast<usize>(bi)] = (st.n_out == 0U) ? never_free : static_cast<crd::i32>(s); }
        }
    }
    for (usize i = 0; i < nb; ++i) { free_at[i] = shareable[i] ? last_read[i] : -1; }

    // The single pass, in PRODUCE (== stage) order. A buffer with no reader (last_read<0) is never a tenant (nothing to alias into)
    // and never lends (freeing it early gains nothing) — left as its own buffer.
    for (usize s = 0; s < plan.stages.size(); ++s)
    {
        for (usize b = 0; b < nb; ++b)
        {
            if (!shareable[b] || produce[b] != static_cast<crd::i32>(s) || last_read[b] < produce[b]) { continue; }
            // smallest-fit ROOT landlord free strictly before b is born.
            crd::i32 best = -1;
            crd::u64 best_bytes = 0;
            for (usize l = 0; l < nb; ++l)
            {
                if (l == b || !shareable[l] || plan.buffers[l].alias_of >= 0) { continue; }        // l must be a ROOT
                if (free_at[l] < 0 || free_at[l] >= produce[b] || plan.buffers[l].bytes < plan.buffers[b].bytes) { continue; }
                if (best < 0 || plan.buffers[l].bytes < best_bytes) { best = static_cast<crd::i32>(l); best_bytes = plan.buffers[l].bytes; }
            }
            if (best < 0) // no fit → largest free root + GROW to b (advisor's smallest-fit-else-largest-and-grow)
            {
                crd::u64 big = 0;
                for (usize l = 0; l < nb; ++l)
                {
                    if (l == b || !shareable[l] || plan.buffers[l].alias_of >= 0) { continue; }
                    if (free_at[l] < 0 || free_at[l] >= produce[b]) { continue; }
                    if (best < 0 || plan.buffers[l].bytes > big) { best = static_cast<crd::i32>(l); big = plan.buffers[l].bytes; }
                }
            }
            if (best >= 0)
            {
                const usize land = static_cast<usize>(best);
                if (plan.buffers[land].bytes < plan.buffers[b].bytes) { plan.buffers[land].bytes = plan.buffers[b].bytes; } // grow to max
                plan.buffers[b].alias_of = best;         // b tenants the root landlord
                free_at[land]            = last_read[b]; // the landlord is now occupied until b's last read (a later buffer may take it again)
                free_at[b]               = -1;           // b is a tenant, not an independent root
            }
        }
    }
}
} // namespace

containers::StringView plan_reject_name(PlanReject r) noexcept
{
    switch (r)
    {
    case PlanReject::None: return StringView("none");
    case PlanReject::NotVerifyClean: return StringView("not-verify-clean");
    case PlanReject::UnsupportedOp: return StringView("unsupported-op");
    case PlanReject::SynthRejected: return StringView("synth-rejected");
    case PlanReject::ReshapeNotAlias: return StringView("reshape-not-alias");
    case PlanReject::DanglingOperand: return StringView("dangling-operand");
    case PlanReject::NoOutput: return StringView("no-output");
    case PlanReject::DispatchOutputsNotTrailing: return StringView("dispatch-outputs-not-trailing");
    case PlanReject::UnsupportedQuantScheme: return StringView("unsupported-quant-scheme");
    case PlanReject::TuneCacheLockedMiss: return StringView("tune-cache-locked-miss");
    case PlanReject::PartitionLineageMismatch: return StringView("partition-lineage-mismatch");
    }
    return StringView("?");
}

PlanOptions plan_options_from_transform(Context& ctx, const Module& transform_mod, PlanOptions base)
{
    // Walk the schedule module's top-level directives; a known directive's required `enable` bool sets its knob (read
    // defensively — valid + Bool kind), an ABSENT directive keeps `base`, an unknown op is ignored (per-op targeting is
    // the sec-71 named-forward). ⛔ program-global this slice: no payload handle, so a bare module walk suffices.
    const OpId fuse_k  = transform::fuse_kind(ctx);
    const OpId share_k = transform::share_storage_kind(ctx);
    for (const Block* b = transform_mod.body()->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const bool is_fuse  = op->kind() == fuse_k;
            const bool is_share = op->kind() == share_k;
            if (!is_fuse && !is_share) { continue; }
            const AttrId a = op->attr(containers::StringView("enable"));
            if (!a.valid() || ctx.attr_value(a).kind != AttrKind::Bool) { continue; }
            const bool enable = ctx.attr_value(a).b;
            if (is_fuse) { base.fuse_gemm_relu = enable; }
            else { base.share_intermediate_storage = enable; }
        }
    }
    return base;
}

TuneCacheLookup plan_options_from_tune_cache(Context& ctx, const Module& cache_mod, containers::StringView device,
                                             containers::StringView env, u64 program_hash, containers::StringView shape,
                                             PlanOptions base)
{
    // REPLAY: the FIRST tune.entry whose FULL key (device, env, program_hash, shape) matches — unambiguous on a
    // find_tune_misuse-clean cache (DuplicateKey = all-four-equal). A HIT returns the row's schedule; a MISS returns `base`
    // unchanged (the caller — 28c locked mode — chooses fallback-to-default vs typed reject). ⛔ NEVER measures here (sec-80
    // "tune offline, replay at plan time, never at runtime"; the v17 select_schedule discipline, made portable).
    containers::Array<tune::TuneEntry> entries(ctx.allocator());
    (void)tune::load_tune_entries(ctx, cache_mod, entries); // count unused here — we scan for the first key match
    for (usize i = 0; i < entries.size(); ++i)
    {
        const tune::TuneEntry& e = entries[i];
        if (e.device == device && e.env == env && e.program_hash == program_hash && e.shape == shape)
        {
            PlanOptions opts                = base;
            opts.fuse_gemm_relu             = e.fuse;
            opts.share_intermediate_storage = e.share;
            return {true, opts};
        }
    }
    return {false, base};
}

TensorPipelinePlan plan_tensor_pipeline_cached(Context& ctx, const Module& m, memory::IAllocator* alloc, const Module& cache_mod,
                                               containers::StringView device, containers::StringView env,
                                               containers::StringView shape, TunePolicy policy, PlanOptions base)
{
    // ⛔ hash the module the PLANNER consumes (`m`, post-expansion) — the SAME producer the 28b measurer emits, so the cache key
    //    can never drift from the plan. NEVER measure here (replay-only; the v17 discipline made portable).
    const u64             program_hash = tune::program_hash(ctx, m, alloc);
    const TuneCacheLookup look         = plan_options_from_tune_cache(ctx, cache_mod, device, env, program_hash, shape, base);
    if (!look.hit && policy == TunePolicy::Locked)
    {
        TensorPipelinePlan plan(alloc); // a LOCKED miss ships nothing unmeasured — a typed reject, no op at fault (reject_op stays null)
        plan.reject = PlanReject::TuneCacheLockedMiss;
        return plan;
    }
    // HIT → the row's {fuse, share}; MISS + Fallback → `base` (look.opts == base unchanged on a miss). One plan call covers both.
    return plan_tensor_pipeline(ctx, m, alloc, look.opts);
}

// CEIR-29c-1 §102 — the SAME plan, plus a per-stage PROVIDER tag from the partition. METADATA-ONLY (stages/buffers unchanged).
TensorPipelinePlan plan_tensor_pipeline_partitioned(Context& ctx, const Module& m, memory::IAllocator* alloc,
                                                    const MlPartition&                                     partition,
                                                    const containers::HashMap<const Operation*, crd::i32>& lineage,
                                                    PlanOptions                                            opts)
{
    TensorPipelinePlan plan = plan_tensor_pipeline(ctx, m, alloc, opts);
    if (plan.reject != PlanReject::None) { return plan; } // a reject leaves stages partial — nothing to tag
    const crd::i32 nprov = static_cast<crd::i32>(partition.assignments.size());
    for (crd::usize s = 0; s < plan.stages.size(); ++s)
    {
        const crd::i32* const idx = lineage.find(plan.stages[s].op); // the expanded stage op → its source ml op's pre-order index
        if (idx == nullptr) { plan.stages[s].provider = -1; continue; } // a non-ml stage carries no lineage entry → the fallback
        if (*idx < 0 || *idx >= nprov) // a lineage index OUTSIDE the partition: expand_ml_ops and partition_ml disagree on the
        {                              // ml-op pre-order, or the partition was built on a DIFFERENT module. ⛔ a LOUD typed reject,
            plan.reject    = PlanReject::PartitionLineageMismatch; // never a silent -1 that would masquerade as "the fallback
            plan.reject_op = nullptr; // claimed more stages". A KEY-property mismatch between two inputs — no op is at fault.
            return plan;
        }
        plan.stages[s].provider = partition.assignments[static_cast<crd::usize>(*idx)].provider;
    }
    return plan;
}

TensorPipelinePlan plan_tensor_pipeline(Context& ctx, const Module& m, memory::IAllocator* alloc, PlanOptions opts)
{
    TensorPipelinePlan plan(alloc);

    // ── verify-clean FIRST (the plan trusts the misuse walks — F32/static/envelope checks ride the per-stage synth). ⛔ the
    //    dispatch walk is REQUIRED here too (the plan reads compute.dispatch's `access` string RAW — a malformed token count
    //    would otherwise walk straight into parse_write_flags; the advisor-caught verify-clean gap). ──
    if (linalg::find_linalg_misuse(ctx, m).kind != linalg::LinalgMisuseKind::None
        || tensor::find_tensor_misuse(ctx, m).kind != tensor::TensorMisuseKind::None
        || quant::find_quant_misuse(ctx, m).kind != quant::QuantMisuseKind::None
        || ctx.find_dispatch_misuse(m).kind != DispatchMisuseKind::None)
    {
        plan.reject = PlanReject::NotVerifyClean;
        return plan;
    }

    // ── find the func body block (the first func.func's region) ──
    Block* body = nullptr;
    for (Block* b = m.body()->first_block(); b != nullptr && body == nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == StringView("func.func")) { body = func::func_body_block(op); break; }
        }
    }
    if (body == nullptr) { plan.reject = PlanReject::NoOutput; return plan; }

    for (Operation* op = body->first_op(); op != nullptr; op = op->next_in_block())
    {
        const StringView nm = ctx.op_name(op->kind());

        // resource.declare → an ExternalIn tensor (the pipeline's A/B/C + the fft imaginary input, seeded CallerData;
        // the fft stage RE-marks its imaginary operand Zeros below).
        if (nm == StringView("resource.declare"))
        {
            if (op->num_results() >= 1U && ctx.type_of(op->result(0U)->type()).kind == TypeKind::Tensor)
            {
                PlanBuffer buf;
                buf.value = op->result(0U);
                buf.role  = BufferRole::ExternalIn;
                buf.bytes = tensor_bytes(ctx, op->result(0U)->type());
                buf.fill  = FillKind::CallerData;
                (void)add_buffer(plan, buf);
            }
            continue;
        }
        // arith.const materializes a compute.dispatch grid operand (an SSA index) — a Pure value producer, not a buffer or a
        // dispatched stage; skip it (else the UnsupportedOp fallback would reject the viz stage's grid). ⛔ the RESOLVER reads
        // these consts (defining_op) for the authored grid — the asset-drives-it rule (grid is NOT re-derived from numel).
        // resource.export is a RESULTLESS OUTPUT BOUNDARY (the func.return category — it reads the terminal value, dispatches
        // nothing): skip it. The Output is still derived from the FINAL stage's trailing write (below), which the export reads —
        // so they agree. CEIR-30b-3a: the lowered sec-140 reduction ends declare->reduce...->elementwise->export, now plannable.
        if (nm == StringView("func.return") || nm == StringView("func.func") || nm == StringView("arith.const")
            || nm == StringView("resource.export"))
        {
            continue;
        }

        if (nm == StringView("linalg.gemm"))
        {
            // ⭐ 26e: a plain f32 gemm whose result is single-use by a @relu dispatch FUSES forward into a GemmRelu stage emitted
            //    at that dispatch — SKIP it here (no plain Gemm stage; its result buffer z is NEVER allocated). ⛔ BEFORE the
            //    QuantGemm detect: a gemm cannot be BOTH (fusable_gemm_into_relu excludes a QuantGemm-weight gemm), but the skip
            //    ordering makes the exclusion ENFORCED, not implied. ⛔ opts.fuse_gemm_relu gates it (the raw-vs-opt differential
            //    plans the SAME module with the flag OFF — the semantics-preserving witness needs both forms of ONE program).
            if (opts.fuse_gemm_relu && fusable_gemm_into_relu(ctx, op)) { continue; }
            // ⭐ 23b-2b: if the WEIGHT operand (B, operand-1) is a fusable quant.dequantize, COLLAPSE into a QuantGemm stage —
            //    bind {A, W_q8 (the dequant INPUT, alias-through), scale, D}; the dequantize's f32 output is NEVER allocated (§54).
            //    ⛔ M,K,N/grid come from the GEMM operand types (f32 [.,K,N]) but the stage BINDS the dequant INPUT (int8 [.,K,N])
            //    — legal ONLY because dequantize is shape-preserving (the resolver re-derives via the same shared predicate).
            const Operation* const wdq = op->num_operands() >= 2U ? op->operand(1U)->defining_op() : nullptr;
            if (wdq != nullptr && fusable_dequant_into_gemm_weight(ctx, wdq))
            {
                const crd::i32 ba = find_buffer(plan, op->operand(0U));  // A (f32 activations)
                const crd::i32 bw = find_buffer(plan, wdq->operand(0U)); // W_q8 (int8) — the dequant INPUT, not its result
                const crd::i32 bs = find_buffer(plan, wdq->operand(1U)); // scale
                if (ba < 0 || bw < 0 || bs < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
                PlanBuffer d;
                d.value           = op->result(0U);
                d.role            = BufferRole::Intermediate;
                d.bytes           = tensor_bytes(ctx, op->result(0U)->type()); // D f32
                const crd::i32 bd = add_buffer(plan, d);
                PlanStage st;
                st.op      = op;
                st.kind    = StageKind::QuantGemm;
                st.bind[0] = ba; // A
                st.bind[1] = bw; // W_q8
                st.bind[2] = bs; // scale
                st.bind[3] = bd; // D (trailing output)
                st.nbind   = 4;
                st.n_out   = 1;
                plan.stages.push_back(st);
                continue;
            }
            kir::KGraph           g(alloc);
            const GraphSynth      s = synth_gemm(ctx, *op, g);
            PlanStage             st;
            st.op   = op;
            st.kind = StageKind::Gemm;
            if (s.reject != SynthReject::None)
            {
                st.synth_reject = s.reject;
                plan.stages.push_back(st);
                plan.reject    = PlanReject::SynthRejected;
                plan.reject_op = op;
                return plan;
            }
            const crd::i32 ba = find_buffer(plan, op->operand(0U)); // A
            const crd::i32 bb = find_buffer(plan, op->operand(1U)); // B (C = operand 2 is unused under the beta==0 envelope)
            if (ba < 0 || bb < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
            PlanBuffer d;
            d.value          = op->result(0U);
            d.role           = BufferRole::Intermediate;
            d.bytes          = tensor_bytes(ctx, op->result(0U)->type());
            const crd::i32 bd = add_buffer(plan, d);
            st.bind[0]        = ba;
            st.bind[1]        = bb;
            st.bind[2]        = bd;
            st.nbind          = 3;
            st.n_out          = 1; // D
            plan.stages.push_back(st);
            continue;
        }

        // tensor.reshape → a zero-copy ALIAS (the rank bridge; not a dispatched stage) iff the element count is preserved.
        if (nm == StringView("tensor.reshape"))
        {
            const crd::i32 bin = find_buffer(plan, op->operand(0U));
            if (bin < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
            if (tensor_bytes(ctx, op->result(0U)->type()) != tensor_bytes(ctx, op->operand(0U)->type()))
            {
                plan.reject = PlanReject::ReshapeNotAlias; plan.reject_op = op; return plan;
            }
            PlanBuffer a;
            a.value    = op->result(0U);
            a.role     = BufferRole::Alias;
            a.alias_of = bin;
            a.bytes    = plan.buffers[static_cast<usize>(bin)].bytes;
            (void)add_buffer(plan, a);
            continue;
        }

        // CEIR-25b-3: tensor.transpose / tensor.broadcast → a graph-tier Permute / Broadcast stage (1 in, 1 out). The synth
        // envelope-checks (F32/static/rank/perm/same-rank); the resolver (25b-4) re-synthesizes + emits emit_permute/emit_broadcast_nd.
        if (nm == StringView("tensor.transpose") || nm == StringView("tensor.broadcast"))
        {
            const bool       istr = nm == StringView("tensor.transpose");
            kir::KGraph      g(alloc);
            const GraphSynth s = istr ? synth_transpose(ctx, *op, g) : synth_broadcast(ctx, *op, g);
            PlanStage        st;
            st.op   = op;
            st.kind = istr ? StageKind::Transpose : StageKind::Broadcast;
            if (s.reject != SynthReject::None)
            {
                st.synth_reject = s.reject;
                plan.stages.push_back(st);
                plan.reject    = PlanReject::SynthRejected;
                plan.reject_op = op;
                return plan;
            }
            const crd::i32 bin = find_buffer(plan, op->operand(0U));
            if (bin < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
            PlanBuffer d;
            d.value           = op->result(0U);
            d.role            = BufferRole::Intermediate;
            d.bytes           = tensor_bytes(ctx, op->result(0U)->type());
            const crd::i32 bd = add_buffer(plan, d);
            st.bind[0]        = bin;
            st.bind[1]        = bd;
            st.nbind          = 2;
            st.n_out          = 1;
            plan.stages.push_back(st);
            continue;
        }

        // CEIR-25b-3: tensor.elementwise → a graph-tier binary stage (2 in, 1 out) — the reverse pass's adjoint accumulation (fn=add).
        if (nm == StringView("tensor.elementwise"))
        {
            kir::KGraph      g(alloc);
            const GraphSynth s = synth_elementwise(ctx, *op, g);
            PlanStage        st;
            st.op   = op;
            st.kind = StageKind::Elementwise;
            if (s.reject != SynthReject::None)
            {
                st.synth_reject = s.reject;
                plan.stages.push_back(st);
                plan.reject    = PlanReject::SynthRejected;
                plan.reject_op = op;
                return plan;
            }
            const crd::i32 ba = find_buffer(plan, op->operand(0U));
            const crd::i32 bb = find_buffer(plan, op->operand(1U));
            if (ba < 0 || bb < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
            PlanBuffer d;
            d.value           = op->result(0U);
            d.role            = BufferRole::Intermediate;
            d.bytes           = tensor_bytes(ctx, op->result(0U)->type());
            const crd::i32 bd = add_buffer(plan, d);
            st.bind[0]        = ba;
            st.bind[1]        = bb;
            st.bind[2]        = bd;
            st.nbind          = 3;
            st.n_out          = 1;
            plan.stages.push_back(st);
            continue;
        }

        if (nm == StringView("tensor.fft"))
        {
            kir::KGraph      g(alloc);
            const FftSynth   s = synth_fft(ctx, *op, g);
            PlanStage        st;
            st.op   = op;
            st.kind = StageKind::Fft;
            if (s.reject != SynthReject::None)
            {
                st.synth_reject = s.reject;
                plan.stages.push_back(st);
                plan.reject    = PlanReject::SynthRejected;
                plan.reject_op = op;
                return plan;
            }
            const crd::i32 bre = find_buffer(plan, op->operand(0U)); // in_re (the chain)
            const crd::i32 bim = find_buffer(plan, op->operand(1U)); // in_im (a real signal's imaginary → Zeros)
            if (bre < 0 || bim < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
            if (plan.buffers[static_cast<usize>(bim)].role == BufferRole::ExternalIn)
            {
                plan.buffers[static_cast<usize>(bim)].fill = FillKind::Zeros;
            }
            const crd::u64 half_bytes = static_cast<crd::u64>(s.n / 2) * 4ULL; // n/2 twiddle entries (the 22b radix-2 contract)
            PlanBuffer     twr;
            twr.role           = BufferRole::ExternalIn;
            twr.fill           = FillKind::FftTwiddle;
            twr.bytes          = half_bytes;
            const crd::i32 btr = add_buffer(plan, twr);
            PlanBuffer     twi;
            twi.role           = BufferRole::ExternalIn;
            twi.fill           = FillKind::FftTwiddle;
            twi.bytes          = half_bytes;
            const crd::i32 bti = add_buffer(plan, twi);
            PlanBuffer     orr;
            orr.value          = op->result(0U);
            orr.role           = BufferRole::Intermediate;
            orr.bytes          = tensor_bytes(ctx, op->result(0U)->type());
            const crd::i32 bor = add_buffer(plan, orr);
            PlanBuffer     oim;
            oim.value          = op->num_results() >= 2U ? op->result(1U) : nullptr;
            oim.role           = BufferRole::Intermediate;
            oim.bytes          = orr.bytes;
            const crd::i32 boi = add_buffer(plan, oim);
            st.bind[0]         = bre;
            st.bind[1]         = bim;
            st.bind[2]         = btr;
            st.bind[3]         = bti;
            st.bind[4]         = bor;
            st.bind[5]         = boi;
            st.nbind           = 6;
            st.n_out           = 2; // out_re, out_im
            plan.stages.push_back(st);
            continue;
        }

        if (nm == StringView("tensor.reduce"))
        {
            kir::KGraph      g(alloc);
            const GraphSynth s = synth_reduce(ctx, *op, g);
            PlanStage        st;
            st.op   = op;
            st.kind = StageKind::Reduce;
            if (s.reject != SynthReject::None)
            {
                st.synth_reject = s.reject;
                plan.stages.push_back(st);
                plan.reject    = PlanReject::SynthRejected;
                plan.reject_op = op;
                return plan;
            }
            const crd::i32 bin = find_buffer(plan, op->operand(0U));
            if (bin < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
            PlanBuffer o;
            o.value          = op->result(0U);
            o.role           = BufferRole::Intermediate;
            o.bytes          = tensor_bytes(ctx, op->result(0U)->type());
            const crd::i32 bo = add_buffer(plan, o);
            st.bind[0]        = bin;
            st.bind[1]        = bo;
            st.nbind          = 2;
            st.n_out          = 1; // the reduced output
            plan.stages.push_back(st);
            continue;
        }

        // quant.dequantize → a Dequant stage bound to the AUTHORED Q8 dequant .ckir (CEIR-23b). ⛔ SYMMETRIC path (zp≡0): binds
        // {W_q8, scale} (the op's value operands — ExternalIn resource.declares) + the dequantized output (a NEW Intermediate);
        // the zero_point operand is VALIDATED-present but NOT bound — the symmetric kernel drops the subtract (the advisor's
        // "fused = symmetric" floor; the ASYMMETRIC int8/int32-zp plan path is name-forward, its dequant math already proven in
        // the 23b-1 standalone gate). ⛔ 23b-2a ALWAYS unfused (the fused dequantize→gemm collapse is 23b-2b). ⛔ W_q8's element
        // is INT8 ⇒ tensor_bytes = N·1 = the u32-packed device size (the element-size fix); the output is f32. Bind order
        // {W_q8, scale, out} MIRRORS the symmetric kernel's buffer_decl order (packed@0, scale@1, out@2).
        if (nm == StringView("quant.dequantize"))
        {
            // ⭐ 23b-2b: if this dequantize FUSES into a following gemm's weight (single-use), SKIP it — the QuantGemm stage
            //    (emitted at the gemm) binds THIS op's INPUT buffers directly; the dequantize's output is NEVER allocated (§54).
            if (fusable_dequant_into_gemm_weight(ctx, op)) { continue; }
            // ⛔ name-forward: the plan-path Dequant kernel is SYMMETRIC PER-TENSOR only (reads scale[0], no zp). An asymmetric
            //    or per-axis (rank-1 scale) dequantize would MISCOMPILE silently under it — TYPED-REJECT, never silent-symmetric.
            if (!dequant_is_symmetric_per_tensor(ctx, op)) { plan.reject = PlanReject::UnsupportedQuantScheme; plan.reject_op = op; return plan; }
            const crd::i32 bw = find_buffer(plan, op->operand(0U)); // W_q8 (int8, u32-packed device view)
            const crd::i32 bs = find_buffer(plan, op->operand(1U)); // scale
            const crd::i32 bz = find_buffer(plan, op->operand(2U)); // zero_point (validated present; symmetric ⇒ NOT bound)
            if (bw < 0 || bs < 0 || bz < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
            PlanBuffer o;
            o.value           = op->result(0U);
            o.role            = BufferRole::Intermediate;
            o.bytes           = tensor_bytes(ctx, op->result(0U)->type()); // f32 output
            const crd::i32 bo = add_buffer(plan, o);
            PlanStage st;
            st.op      = op;
            st.kind    = StageKind::Dequant;
            st.bind[0] = bw; // packed@0
            st.bind[1] = bs; // scale@1
            st.bind[2] = bo; // out@2
            st.nbind   = 3;
            st.n_out   = 1;  // the dequantized output
            plan.stages.push_back(st);
            continue;
        }

        // compute.dispatch → a VizDispatch stage: the AUTHORED viz .ckir (magnitude / normalize), the §137 "mixed high-level
        // tensor + CKIR" step. ⛔ NOT synthesized by ckir_synth (a hand-authored kernel the RESOLVER ckir_reads) — so this
        // stage never carries a SynthReject; it wires like any other by def-use. Bindings begin at operand 3 (grid 0..2 are the
        // index consts, already skipped). A Tensor Value binds directly — ceir_is_resource_kind covers Tensor (no bridge op).
        if (nm == StringView("compute.dispatch"))
        {
            const crd::u32 nops  = op->num_operands();
            const crd::u32 nbind = nops >= 3U ? nops - 3U : 0U; // grid(3) then the variadic bindings
            if (nbind > 8U) { plan.reject = PlanReject::UnsupportedOp; plan.reject_op = op; return plan; } // the exec bind[8] cap
            bool            is_write[8] = {};
            const AttrValue av          = ctx.attr_value(op->attr(StringView("access")));
            (void)parse_write_flags(av.s, is_write); // dispatch-verify-clean guarantees kind==String + token-count==nbind

            // outputs = the TRAILING contiguous run of `w` bindings; any `w` BEFORE that run violates the executor's
            // "outputs = the last n_out binds" barrier contract (advisor: author inputs then outputs).
            crd::u32 n_out = 0;
            while (n_out < nbind && is_write[nbind - 1U - n_out]) { ++n_out; }
            for (crd::u32 i = 0; i + n_out < nbind; ++i)
            {
                if (is_write[i]) { plan.reject = PlanReject::DispatchOutputsNotTrailing; plan.reject_op = op; return plan; }
            }

            // ⭐ 26e: a @relu dispatch whose read (bind[0] = operand-3) is a fusable plain f32 gemm result → emit the FUSED
            //    GemmRelu stage HERE (the gemm was skipped at its site; its z buffer never allocated). binds {A, B, h} where h is
            //    THIS dispatch's WRITE target; op = the GEMM (the resolver re-synthesizes synth_gemm(op, GemmEpilogue::Relu)). ⛔
            //    AFTER the trailing-write check (a malformed relu access still rejects DispatchOutputsNotTrailing), BEFORE VizDispatch.
            const Operation* const gp = (opts.fuse_gemm_relu && nops >= 4U) ? op->operand(3U)->defining_op() : nullptr;
            if (gp != nullptr && fusable_gemm_into_relu(ctx, gp))
            {
                const crd::i32 ba = find_buffer(plan, gp->operand(0U));            // A
                const crd::i32 bb = find_buffer(plan, gp->operand(1U));            // B (f32 weight — cond (3) guarantees NOT a dequant)
                const crd::i32 bh = find_buffer(plan, op->operand(nops - 1U));     // h = the relu's WRITE target (trailing operand)
                if (ba < 0 || bb < 0 || bh < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
                if (plan.buffers[static_cast<usize>(bh)].role == BufferRole::ExternalIn)
                {
                    plan.buffers[static_cast<usize>(bh)].role = BufferRole::Intermediate; // device-produced by GemmRelu, not a caller upload
                }
                PlanStage gr;
                gr.op      = gp; // the GEMM — the resolver re-synthesizes synth_gemm(gp, Relu) + emit_contract (Max(Contract,0) unwrap)
                gr.kind    = StageKind::GemmRelu;
                gr.bind[0] = ba;
                gr.bind[1] = bb;
                gr.bind[2] = bh; // out (h) trailing
                gr.nbind   = 3;
                gr.n_out   = 1;
                plan.stages.push_back(gr);
                continue;
            }

            PlanStage st;
            st.op   = op;
            st.kind = StageKind::VizDispatch;
            for (crd::u32 i = 0; i < nbind; ++i)
            {
                const crd::i32 bi = find_buffer(plan, op->operand(3U + i));
                if (bi < 0) { plan.reject = PlanReject::DanglingOperand; plan.reject_op = op; return plan; }
                st.bind[i] = bi;
                // a WRITE binding realizes a DEVICE-PRODUCED buffer, not a caller upload — re-mark ExternalIn→Intermediate (the
                // fft im0 re-mark precedent). ⛔ only bare `w` (an `rw` in-place binding stays CallerData — name-forward).
                if (is_write[i] && plan.buffers[static_cast<usize>(bi)].role == BufferRole::ExternalIn)
                {
                    plan.buffers[static_cast<usize>(bi)].role = BufferRole::Intermediate;
                }
            }
            st.nbind = nbind;
            st.n_out = n_out;
            plan.stages.push_back(st);
            continue;
        }

        // Any other op is outside the pipeline vocab.
        plan.reject    = PlanReject::UnsupportedOp;
        plan.reject_op = op;
        return plan;
    }

    // ── Output = the FINAL stage's trailing n_out written buffers. ⛔ NOT an SSA `last_result`: compute.dispatch is RESULTLESS,
    //    so a terminal viz dispatch has no result Value — its produced buffers ARE its trailing write bindings. This is
    //    behavior-identical to the reduce-terminal shape (the reduce's single output is its trailing bind) but ALSO correct for
    //    a dispatch-terminal pipeline (the advisor-caught wrong-buffer-Output bug). NoOutput = no stages / a terminal stage that
    //    writes nothing (an all-read terminal dispatch produces no result). ──
    if (plan.stages.size() == 0U) { plan.reject = PlanReject::NoOutput; return plan; }
    const PlanStage& fin = plan.stages[plan.stages.size() - 1U];
    if (fin.n_out == 0U) { plan.reject = PlanReject::NoOutput; return plan; }
    for (crd::u32 o = 0; o < fin.n_out; ++o)
    {
        const crd::u32 bi = fin.nbind - fin.n_out + o; // the o-th trailing output bind
        plan.buffers[static_cast<usize>(fin.bind[static_cast<usize>(bi)])].role = BufferRole::Output;
    }
    // ⭐ 26f: after roles are final (ExternalIn/Intermediate/Output/Alias), share physical storage among disjoint-lifetime
    //    Intermediates (a free-list pass; the runner honors a tenant's alias_of). Skips func.return-pinned readback targets.
    if (opts.share_intermediate_storage) { assign_shared_storage(ctx, body, plan); }
    return plan;
}
} // namespace crd::ceir::gpu
