#include <crd/ceir/gpu/ckir_synth.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

#include <bit> // std::bit_cast — the Float attr's f64 bit pattern (attr.hpp stores AttrValue::f as the raw bits)

namespace crd::ceir::gpu
{
namespace
{
// A Tensor's element / shape type (members[0]/[1]); {} if malformed (the 21b guard — a Tensor is always [element, shape]).
[[nodiscard]] TypeId tensor_elem(const Context& ctx, TypeId t) noexcept
{
    const Type tt = ctx.type_of(t);
    return tt.members.size() >= 1U ? tt.members[0] : TypeId{};
}
[[nodiscard]] TypeId tensor_shape(const Context& ctx, TypeId t) noexcept
{
    const Type tt = ctx.type_of(t);
    return tt.members.size() >= 2U ? tt.members[1] : TypeId{};
}
[[nodiscard]] bool is_tensor(const Context& ctx, const Value* v) noexcept
{
    return v != nullptr && ctx.type_of(v->type()).kind == TypeKind::Tensor;
}
// The CKIR kernels are F32-only: the element must be a Float of FloatKind::F32.
[[nodiscard]] bool is_f32(const Context& ctx, TypeId elem) noexcept
{
    const Type e = ctx.type_of(elem);
    return e.kind == TypeKind::Float && e.fkind == FloatKind::F32;
}
// A static dim's extent → `out` (true); false if the dim is Symbolic/Dynamic (a device kernel needs a concrete extent).
[[nodiscard]] bool static_extent(const Context& ctx, TypeId dim, crd::i64& out) noexcept
{
    const Type d = ctx.type_of(dim);
    if (static_cast<DimKind>(d.cols) != DimKind::Static) { return false; }
    out = static_cast<crd::i64>(d.count);
    return true;
}
// The Float attr `name`'s f64 value via the valid-checked reader (absent / wrong-kind → false; the absent-reads-as-zero scar).
[[nodiscard]] bool read_float(const Context& ctx, const Operation& op, containers::StringView name, double& out) noexcept
{
    const AttrValue a = ctx.attr_value(op.attr(name));
    if (a.kind != AttrKind::Float) { return false; }
    out = std::bit_cast<double>(a.f);
    return true;
}
// The Bool attr `name` via the valid-checked reader (absent / wrong-kind → false).
[[nodiscard]] bool read_bool(const Context& ctx, const Operation& op, containers::StringView name) noexcept
{
    const AttrValue a = ctx.attr_value(op.attr(name));
    return a.kind == AttrKind::Bool && a.b;
}
} // namespace

containers::StringView synth_reject_name(SynthReject r) noexcept
{
    switch (r)
    {
    case SynthReject::None: return containers::StringView("none");
    case SynthReject::OpNotSupported: return containers::StringView("op-not-supported");
    case SynthReject::ElementNotF32: return containers::StringView("element-not-f32");
    case SynthReject::OperandNotTensor: return containers::StringView("operand-not-tensor");
    case SynthReject::ShapeNotStatic: return containers::StringView("shape-not-static");
    case SynthReject::RankUnsupported: return containers::StringView("rank-unsupported");
    case SynthReject::GemmEpilogueUnsupported: return containers::StringView("gemm-epilogue-unsupported");
    case SynthReject::ReduceFnUnsupported: return containers::StringView("reduce-fn-unsupported");
    case SynthReject::ReduceAxisInvalid: return containers::StringView("reduce-axis-invalid");
    case SynthReject::FftRankUnsupported: return containers::StringView("fft-rank-unsupported");
    case SynthReject::FftLengthNotPow2: return containers::StringView("fft-length-not-pow2");
    case SynthReject::FftDirectionUnknown: return containers::StringView("fft-direction-unknown");
    }
    return containers::StringView("?");
}

GraphSynth synth_gemm(const Context& ctx, const Operation& op, kir::KGraph& g)
{
    // op-name DIALECT-QUALIFIED (the work.consume scar) + arity (the provider is standalone-robust; a malformed under-arity op
    // is out of contract — find_linalg_misuse / the generated verify_gemm reject it before the provider runs).
    if (ctx.op_name(op.kind()) != containers::StringView("linalg.gemm")) { return {SynthReject::OpNotSupported, -1}; }
    if (op.num_operands() < 3U || op.num_results() < 1U) { return {SynthReject::OpNotSupported, -1}; }

    // A, B (+ the result) must be F32 Tensors. C (operand 2) is IGNORED under the beta==0 envelope.
    const Value* const va = op.operand(0U);
    const Value* const vb = op.operand(1U);
    const Value* const vd = op.result(0U);
    if (!is_tensor(ctx, va) || !is_tensor(ctx, vb) || vd == nullptr
        || ctx.type_of(vd->type()).kind != TypeKind::Tensor)
    {
        return {SynthReject::OperandNotTensor, -1};
    }
    if (!is_f32(ctx, tensor_elem(ctx, va->type())) || !is_f32(ctx, tensor_elem(ctx, vb->type()))
        || !is_f32(ctx, tensor_elem(ctx, vd->type())))
    {
        return {SynthReject::ElementNotF32, -1};
    }

    // ⛔ EPILOGUE ENVELOPE: the graph tier has NO scale/add/2D-transpose — synthesize ONLY the plain contract. alpha==1 &&
    // beta==0 && !trans_a && !trans_b, else TYPED-REJECT (a plain-contract synthesis of an α=2 gemm is a wrong-result false-green).
    double alpha = 0.0;
    double beta  = 0.0;
    if (!read_float(ctx, op, containers::StringView("alpha"), alpha)
        || !read_float(ctx, op, containers::StringView("beta"), beta) || alpha != 1.0 || beta != 0.0
        || read_bool(ctx, op, containers::StringView("trans_a")) || read_bool(ctx, op, containers::StringView("trans_b")))
    {
        return {SynthReject::GemmEpilogueUnsupported, -1};
    }

    // Shapes: both rank-2, all static. A = [M, K], B = [K, N]. (The contraction K match is find_linalg_misuse's job — the
    // provider operates on verify-clean ops.)
    const Type sa = ctx.type_of(tensor_shape(ctx, va->type()));
    const Type sb = ctx.type_of(tensor_shape(ctx, vb->type()));
    if (sa.members.size() != 2U || sb.members.size() != 2U) { return {SynthReject::RankUnsupported, -1}; }
    crd::i64 m = 0;
    crd::i64 ka = 0;
    crd::i64 kb = 0;
    crd::i64 n = 0;
    if (!static_extent(ctx, sa.members[0], m) || !static_extent(ctx, sa.members[1], ka)
        || !static_extent(ctx, sb.members[0], kb) || !static_extent(ctx, sb.members[1], n))
    {
        return {SynthReject::ShapeNotStatic, -1};
    }
    (void)kb; // == ka on a verify-clean op (ContractionMismatch is find_linalg_misuse's check)

    // Graph-tier synthesis: input(A)[M,K] iidx 0, input(B)[K,N] iidx 1, contract → [M,N]. DetTier::Exact = bit-exact vs eval_cpu.
    const int a = g.input(kir::make_shape({m, ka}), kir::DType::F32);
    const int b = g.input(kir::make_shape({ka, n}), kir::DType::F32);
    const int c = g.contract(a, b); // DetTier::Exact (default)
    return {SynthReject::None, c};
}

GraphSynth synth_reduce(const Context& ctx, const Operation& op, kir::KGraph& g)
{
    if (ctx.op_name(op.kind()) != containers::StringView("tensor.reduce")) { return {SynthReject::OpNotSupported, -1}; }
    if (op.num_operands() < 1U || op.num_results() < 1U) { return {SynthReject::OpNotSupported, -1}; }
    const Value* const vin = op.operand(0U);
    if (!is_tensor(ctx, vin)) { return {SynthReject::OperandNotTensor, -1}; }
    if (!is_f32(ctx, tensor_elem(ctx, vin->type()))) { return {SynthReject::ElementNotF32, -1}; }

    // ⛔ fn ENVELOPE: sum/prod/max/min → KOp::Reduce{Sum,Prod,Max,Min}; `mean` (needs a post-scale the graph tier lacks) or any
    // unknown token → TYPED-REJECT (never a silent wrong reduction).
    const AttrValue fn  = ctx.attr_value(op.attr(containers::StringView("fn")));
    kir::KOp        kop = kir::KOp::ReduceSum;
    if (fn.kind != AttrKind::String) { return {SynthReject::ReduceFnUnsupported, -1}; }
    if (fn.s == containers::StringView("sum")) { kop = kir::KOp::ReduceSum; }
    else if (fn.s == containers::StringView("prod")) { kop = kir::KOp::ReduceProd; }
    else if (fn.s == containers::StringView("max")) { kop = kir::KOp::ReduceMax; }
    else if (fn.s == containers::StringView("min")) { kop = kir::KOp::ReduceMin; }
    else { return {SynthReject::ReduceFnUnsupported, -1}; } // mean (or an unknown token)

    // shape: rank in [1, CKIR kMaxRank], all-static; axis in [0, rank). mask = 1 << axis (reduce the single axis, keepdims).
    const Type  sin  = ctx.type_of(tensor_shape(ctx, vin->type()));
    const usize rank = sin.members.size();
    if (rank == 0U || rank > static_cast<usize>(kir::kMaxRank)) { return {SynthReject::RankUnsupported, -1}; }
    const AttrValue ax   = ctx.attr_value(op.attr(containers::StringView("axis")));
    const crd::i64  axis = (ax.kind == AttrKind::Int) ? ax.i : -1;
    if (axis < 0 || axis >= static_cast<crd::i64>(rank)) { return {SynthReject::ReduceAxisInvalid, -1}; }
    kir::Shape shape;
    shape.rank = static_cast<int>(rank);
    for (usize i = 0; i < rank; ++i)
    {
        crd::i64 e = 0;
        if (!static_extent(ctx, sin.members[i], e)) { return {SynthReject::ShapeNotStatic, -1}; }
        shape.dims[i] = e;
    }
    const int in  = g.input(shape, kir::DType::F32);
    const int out = g.reduce(kop, in, 1U << static_cast<crd::u32>(axis)); // DetTier::Exact
    return {SynthReject::None, out};
}

FftSynth synth_fft(const Context& ctx, const Operation& op, kir::KGraph& g)
{
    FftSynth out;
    if (ctx.op_name(op.kind()) != containers::StringView("tensor.fft")) { out.reject = SynthReject::OpNotSupported; return out; }
    if (op.num_operands() < 2U || op.num_results() < 2U) { out.reject = SynthReject::OpNotSupported; return out; }
    const Value* const re_in = op.operand(0U); // re_in / im_in / re_out / im_out — the split-complex partners (22a-verified)
    if (!is_tensor(ctx, re_in)) { out.reject = SynthReject::OperandNotTensor; return out; }
    if (!is_f32(ctx, tensor_elem(ctx, re_in->type()))) { out.reject = SynthReject::ElementNotF32; return out; }

    // ⛔ ENVELOPE: rank-1 [n], static, n a power of two >= 2 (the radix dispatch); higher-D / non-innermost fft → name-forward.
    const Type sre = ctx.type_of(tensor_shape(ctx, re_in->type()));
    if (sre.members.size() != 1U) { out.reject = SynthReject::FftRankUnsupported; return out; }
    crd::i64 n = 0;
    if (!static_extent(ctx, sre.members[0], n)) { out.reject = SynthReject::ShapeNotStatic; return out; }
    if (n < 2 || (n & (n - 1)) != 0) { out.reject = SynthReject::FftLengthNotPow2; return out; } // power of two >= 2

    // direction {forward, inverse} → the `inverse` flag.
    const AttrValue dir = ctx.attr_value(op.attr(containers::StringView("direction")));
    if (dir.kind != AttrKind::String) { out.reject = SynthReject::FftDirectionUnknown; return out; }
    if (dir.s == containers::StringView("forward")) { out.inverse = false; }
    else if (dir.s == containers::StringView("inverse")) { out.inverse = true; }
    else { out.reject = SynthReject::FftDirectionUnknown; return out; }

    // kernel-tier synthesis: the RADIX-2 Stockham 1D c2c FFT (6-buffer split re/im; local_size = n/2; twiddles = n/2 entries,
    // the caller's — see the header). ⛔ RADIX-2 (not build_fft1d_batched's radix-4/8/16 dispatch): a STABLE twiddle+local_size
    // contract the provider can publish for ANY 2^k; the batched radix-dispatch is a PERF optimization (different twiddle
    // layout per radix) → name-forward (a schedule-selection slice; CEIR-22 gates CORRECTNESS + the oracle, not peak FFT perf).
    out.plan   = kir::build_fft1d_radix2(g, static_cast<int>(n), out.inverse);
    out.n      = static_cast<int>(n);
    out.reject = SynthReject::None;
    return out;
}
} // namespace crd::ceir::gpu
