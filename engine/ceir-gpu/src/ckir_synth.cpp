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
// Read a Tensor's shape type into a CKIR Shape: rank in [1, kMaxRank(8)], all-static. Returns None / RankUnsupported /
// ShapeNotStatic (the shared shape read for the 25b element-op synths — synth_reduce inlines it; these three reuse it).
[[nodiscard]] SynthReject read_static_shape(const Context& ctx, TypeId shape_ty, kir::Shape& out) noexcept
{
    const Type  st   = ctx.type_of(shape_ty);
    const usize rank = st.members.size();
    if (rank == 0U || rank > static_cast<usize>(kir::kMaxRank)) { return SynthReject::RankUnsupported; }
    out.rank = static_cast<int>(rank);
    for (usize i = 0; i < rank; ++i)
    {
        crd::i64 e = 0;
        if (!static_extent(ctx, st.members[i], e)) { return SynthReject::ShapeNotStatic; }
        out.dims[i] = e;
    }
    return SynthReject::None;
}
// Parse a comma-separated non-negative int list into `out` (<= max entries); false on a malformed/empty token or overflow of max.
// ⛔ standalone copy of tensor.cpp's dialect-private parse_int_list — the synth is standalone-robust (does not re-enter the dialect).
[[nodiscard]] bool parse_int_list(containers::StringView s, crd::i64* out, crd::u32 max, crd::u32& count) noexcept
{
    count = 0U;
    if (s.size() == 0U) { return true; }
    usize start = 0U;
    for (usize i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ',')
        {
            crd::i64 v   = 0;
            bool     any = false;
            for (usize j = start; j < i; ++j)
            {
                if (s[j] < '0' || s[j] > '9') { return false; }
                v   = v * 10 + static_cast<crd::i64>(s[j] - '0');
                any = true;
            }
            if (!any || count >= max) { return false; }
            out[count++] = v;
            start = i + 1U;
        }
    }
    return true;
}
// Is `perm` (a parsed int list of `count` entries) a TRUE permutation of [0, rank)? (standalone copy of the dialect's check.)
[[nodiscard]] bool is_permutation(const crd::i64* perm, crd::u32 count, usize rank) noexcept
{
    if (count != rank || rank > static_cast<usize>(kir::kMaxRank)) { return false; }
    bool seen[kir::kMaxRank] = {};
    for (crd::u32 i = 0; i < count; ++i)
    {
        if (perm[i] < 0 || perm[i] >= static_cast<crd::i64>(rank)) { return false; }
        if (seen[perm[i]]) { return false; } // duplicate
        seen[perm[i]] = true;
    }
    return true;
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
    case SynthReject::TransposePermInvalid: return containers::StringView("transpose-perm-invalid");
    case SynthReject::BroadcastShapeUnsupported: return containers::StringView("broadcast-shape-unsupported");
    case SynthReject::ElementwiseFnUnsupported: return containers::StringView("elementwise-fn-unsupported");
    case SynthReject::ElementwiseShapeMismatch: return containers::StringView("elementwise-shape-mismatch");
    }
    return containers::StringView("?");
}

GraphSynth synth_gemm(const Context& ctx, const Operation& op, kir::KGraph& g, GemmEpilogue epilogue)
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
    if (epilogue == GemmEpilogue::Relu)
    {
        // ⭐ CEIR-26e: fuse relu into the store as max(contract, 0). `c` (the contract) is UNCHANGED — same K-loop — so the
        //    result is bit-exact vs the unfused gemm→relu.ckir chain; the contract emitters UNWRAP this Max(Contract, 0) root.
        //    The zero is a [M,N] uniform-fill Const (eval_cpu fills it; the emitter never reads it, it just applies max(acc,0)).
        const int z = g.constant(0.0, kir::make_shape({m, n}), kir::DType::F32);
        return {SynthReject::None, g.binary(kir::KOp::Max, c, z)};
    }
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

GraphSynth synth_transpose(const Context& ctx, const Operation& op, kir::KGraph& g)
{
    if (ctx.op_name(op.kind()) != containers::StringView("tensor.transpose")) { return {SynthReject::OpNotSupported, -1}; }
    if (op.num_operands() < 1U || op.num_results() < 1U) { return {SynthReject::OpNotSupported, -1}; }
    const Value* const vin  = op.operand(0U);
    const Value* const vout = op.result(0U);
    if (!is_tensor(ctx, vin) || vout == nullptr || ctx.type_of(vout->type()).kind != TypeKind::Tensor)
    {
        return {SynthReject::OperandNotTensor, -1};
    }
    if (!is_f32(ctx, tensor_elem(ctx, vin->type())) || !is_f32(ctx, tensor_elem(ctx, vout->type())))
    {
        return {SynthReject::ElementNotF32, -1};
    }
    kir::Shape        src;
    const SynthReject rs = read_static_shape(ctx, tensor_shape(ctx, vin->type()), src);
    if (rs != SynthReject::None) { return {rs, -1}; }

    // perm: a comma-separated int list, a true permutation of [0, rank) (else TransposePermInvalid).
    const AttrValue pv = ctx.attr_value(op.attr(containers::StringView("perm")));
    crd::i64        perm[kir::kMaxRank];
    crd::u32        pc = 0U;
    if (pv.kind != AttrKind::String || !parse_int_list(pv.s, perm, static_cast<crd::u32>(kir::kMaxRank), pc)
        || !is_permutation(perm, pc, static_cast<usize>(src.rank)))
    {
        return {SynthReject::TransposePermInvalid, -1};
    }
    crd::u8 p8[kir::kMaxRank];
    for (crd::u32 i = 0; i < pc; ++i) { p8[i] = static_cast<crd::u8>(perm[i]); }

    const int in  = g.input(src, kir::DType::F32); // iidx 0
    const int out = g.permute(in, p8);             // KOp::Permute (eval_cpu ckir_eval.hpp:207)
    return {SynthReject::None, out};
}

GraphSynth synth_broadcast(const Context& ctx, const Operation& op, kir::KGraph& g)
{
    if (ctx.op_name(op.kind()) != containers::StringView("tensor.broadcast")) { return {SynthReject::OpNotSupported, -1}; }
    if (op.num_operands() < 1U || op.num_results() < 1U) { return {SynthReject::OpNotSupported, -1}; }
    const Value* const vin  = op.operand(0U);
    const Value* const vout = op.result(0U);
    if (!is_tensor(ctx, vin) || vout == nullptr || ctx.type_of(vout->type()).kind != TypeKind::Tensor)
    {
        return {SynthReject::OperandNotTensor, -1};
    }
    if (!is_f32(ctx, tensor_elem(ctx, vin->type())) || !is_f32(ctx, tensor_elem(ctx, vout->type())))
    {
        return {SynthReject::ElementNotF32, -1};
    }
    kir::Shape        src;
    kir::Shape        dst;
    const SynthReject r0 = read_static_shape(ctx, tensor_shape(ctx, vin->type()), src);
    if (r0 != SynthReject::None) { return {r0, -1}; }
    const SynthReject r1 = read_static_shape(ctx, tensor_shape(ctx, vout->type()), dst);
    if (r1 != SynthReject::None) { return {r1, -1}; }

    // ⛔ SAME-RANK envelope: CKIR g.broadcast is same-rank left-indexed (ckir_eval.hpp:217); each source dim 1-or-equal to dst.
    if (src.rank != dst.rank) { return {SynthReject::BroadcastShapeUnsupported, -1}; }
    for (int i = 0; i < src.rank; ++i)
    {
        if (src.dims[i] != 1 && src.dims[i] != dst.dims[i]) { return {SynthReject::BroadcastShapeUnsupported, -1}; }
    }

    const int in  = g.input(src, kir::DType::F32); // iidx 0
    const int out = g.broadcast(in, dst);          // KOp::Broadcast (eval_cpu ckir_eval.hpp:213)
    return {SynthReject::None, out};
}

GraphSynth synth_elementwise(const Context& ctx, const Operation& op, kir::KGraph& g)
{
    if (ctx.op_name(op.kind()) != containers::StringView("tensor.elementwise")) { return {SynthReject::OpNotSupported, -1}; }
    if (op.num_operands() < 2U || op.num_results() < 1U) { return {SynthReject::OpNotSupported, -1}; }
    const Value* const va = op.operand(0U);
    const Value* const vb = op.operand(1U);
    const Value* const vd = op.result(0U);
    if (!is_tensor(ctx, va) || !is_tensor(ctx, vb) || vd == nullptr || ctx.type_of(vd->type()).kind != TypeKind::Tensor)
    {
        return {SynthReject::OperandNotTensor, -1};
    }
    if (!is_f32(ctx, tensor_elem(ctx, va->type())) || !is_f32(ctx, tensor_elem(ctx, vb->type()))
        || !is_f32(ctx, tensor_elem(ctx, vd->type())))
    {
        return {SynthReject::ElementNotF32, -1};
    }

    // ⛔ fn ENVELOPE: the FULL binary vocab {add,sub,mul,div,max,min,pow} → KOp (else ElementwiseFnUnsupported — defensive; fn_in gates it).
    const AttrValue fn  = ctx.attr_value(op.attr(containers::StringView("fn")));
    kir::KOp        kop = kir::KOp::Add;
    if (fn.kind != AttrKind::String) { return {SynthReject::ElementwiseFnUnsupported, -1}; }
    if (fn.s == containers::StringView("add")) { kop = kir::KOp::Add; }
    else if (fn.s == containers::StringView("sub")) { kop = kir::KOp::Sub; }
    else if (fn.s == containers::StringView("mul")) { kop = kir::KOp::Mul; }
    else if (fn.s == containers::StringView("div")) { kop = kir::KOp::Div; }
    else if (fn.s == containers::StringView("max")) { kop = kir::KOp::Max; }
    else if (fn.s == containers::StringView("min")) { kop = kir::KOp::Min; }
    else if (fn.s == containers::StringView("pow")) { kop = kir::KOp::Pow; }
    else { return {SynthReject::ElementwiseFnUnsupported, -1}; }

    // ⛔ SAME-SHAPE envelope: g.binary is same-shape (ckir.hpp:140) — NEVER implicit-broadcast (the bin-bcast OOB scar).
    kir::Shape        sa;
    kir::Shape        sb;
    const SynthReject r0 = read_static_shape(ctx, tensor_shape(ctx, va->type()), sa);
    if (r0 != SynthReject::None) { return {r0, -1}; }
    const SynthReject r1 = read_static_shape(ctx, tensor_shape(ctx, vb->type()), sb);
    if (r1 != SynthReject::None) { return {r1, -1}; }
    if (sa.rank != sb.rank) { return {SynthReject::ElementwiseShapeMismatch, -1}; }
    for (int i = 0; i < sa.rank; ++i)
    {
        if (sa.dims[i] != sb.dims[i]) { return {SynthReject::ElementwiseShapeMismatch, -1}; }
    }

    const int a   = g.input(sa, kir::DType::F32); // iidx 0
    const int b   = g.input(sb, kir::DType::F32); // iidx 1
    const int out = g.binary(kop, a, b);
    return {SynthReject::None, out};
}
} // namespace crd::ceir::gpu
