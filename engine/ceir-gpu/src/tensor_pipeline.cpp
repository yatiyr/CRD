#include <crd/ceir/gpu/tensor_pipeline.hpp>

#include <crd/ceir/func.hpp>   // func::func_body_block
#include <crd/ceir/ir.hpp>     // Block / Operation / Value traversal
#include <crd/ceir/linalg.hpp> // find_linalg_misuse
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
// f32 byte size of a Tensor type's (all-static) shape; 0 if malformed / a dynamic dim (planning is post-verify + post-synth,
// which already reject dynamic dims — this is the belt-and-braces path).
[[nodiscard]] crd::u64 tensor_bytes(const Context& ctx, TypeId t) noexcept
{
    const Type tt = ctx.type_of(t);
    if (tt.members.size() < 2U) { return 0; }
    const Type  sh = ctx.type_of(tt.members[1]);
    crd::u64    n  = 1;
    for (usize i = 0; i < sh.members.size(); ++i)
    {
        const Type d = ctx.type_of(sh.members[i]);
        if (static_cast<DimKind>(d.cols) != DimKind::Static) { return 0; }
        n *= static_cast<crd::u64>(d.count);
    }
    return n * 4ULL; // f32
}
// Append a buffer, return its index.
[[nodiscard]] crd::i32 add_buffer(TensorPipelinePlan& plan, const PlanBuffer& b)
{
    const crd::i32 idx = static_cast<crd::i32>(plan.buffers.size());
    plan.buffers.push_back(b);
    return idx;
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
    }
    return StringView("?");
}

TensorPipelinePlan plan_tensor_pipeline(Context& ctx, const Module& m, memory::IAllocator* alloc)
{
    TensorPipelinePlan plan(alloc);

    // ── verify-clean FIRST (the plan trusts the misuse walks — F32/static/envelope checks ride the per-stage synth) ──
    if (linalg::find_linalg_misuse(ctx, m).kind != linalg::LinalgMisuseKind::None
        || tensor::find_tensor_misuse(ctx, m).kind != tensor::TensorMisuseKind::None)
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

    const Value* last_result = nullptr;

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
        if (nm == StringView("func.return") || nm == StringView("func.func")) { continue; }

        if (nm == StringView("linalg.gemm"))
        {
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
            last_result = op->result(0U);
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
            last_result = op->result(0U);
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
            last_result = op->result(0U);
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
            last_result = op->result(0U);
            continue;
        }

        // compute.dispatch (the authored viz .ckir) is CEIR-22c-3 (needs the loaded kernel). Any other op is outside the vocab.
        plan.reject    = PlanReject::UnsupportedOp;
        plan.reject_op = op;
        return plan;
    }

    if (last_result == nullptr) { plan.reject = PlanReject::NoOutput; return plan; }
    const crd::i32 bout = find_buffer(plan, last_result);
    if (bout >= 0) { plan.buffers[static_cast<usize>(bout)].role = BufferRole::Output; }
    return plan;
}
} // namespace crd::ceir::gpu
