#include <crd/ceir/gpu/tensor_pipeline.hpp>

#include <crd/ceir/attr.hpp>   // AttrValue / AttrKind (the dequantize `scheme` attr — the symmetric-per-tensor plan gate)
#include <crd/ceir/func.hpp>   // func::func_body_block
#include <crd/ceir/ir.hpp>     // Block / Operation / Value traversal
#include <crd/ceir/linalg.hpp> // find_linalg_misuse
#include <crd/ceir/quant.hpp>  // find_quant_misuse (CEIR-23b: quant.dequantize is a planned op)
#include <crd/ceir/tensor.hpp> // find_tensor_misuse
#include <crd/ceir/type.hpp>   // Type / TypeKind / DimKind
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
    }
    return StringView("?");
}

TensorPipelinePlan plan_tensor_pipeline(Context& ctx, const Module& m, memory::IAllocator* alloc)
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
        if (nm == StringView("func.return") || nm == StringView("func.func") || nm == StringView("arith.const")) { continue; }

        if (nm == StringView("linalg.gemm"))
        {
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
    return plan;
}
} // namespace crd::ceir::gpu
