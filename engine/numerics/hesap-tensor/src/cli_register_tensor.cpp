// v14-z — CLI registration for the TENSORS cluster (hesap.tensor.*). One command per op family (ADR-0081 + the
// v13-z per-module CLI pattern). The CLI is a smoke/demo surface: inputs are SYNTHETIC (deterministic Philox
// counter-RNG fills keyed by `seed`) or FILES (io/nn); every command prints a compact deterministic Text result
// (shapes + checksums); wall time rides a Hint diagnostic so the Text stays bit-stable run to run.
//
//   hesap.tensor.einsum.f64        : parse "ab,bc->ac", plan (greedy|optimal), execute on Philox operands.
//   hesap.tensor.ew.f64            : Tier-D elementwise binary op (add|sub|mul|div|min|max) on two Philox tensors.
//   hesap.tensor.reduce.f64        : Tier-R reduction (sum|prod|min|max|mean), full or over an axes bitmask.
//   hesap.tensor.permute.f64       : the v14-d permute_copy kernel — order given as a dim list.
//   hesap.tensor.batched.f64       : batched LA (gemm|cholesky|lu|svd) on [B,n,n]-style Philox batches.
//   hesap.tensor.hyperopt          : the v14-g contraction-path optimizer on a matmul-chain network.
//   hesap.tensor.sparse.mttkrp.f64 : COO build -> CSF -> MTTKRP (the SPLATT-class kernel).
//   hesap.tensor.decomp.f64        : CP-ALS / HOSVD / HOOI (fixed-budget, deterministic) on a Philox tensor.
//   hesap.tensor.tt.f64            : tt_svd of the Hilbert (or Philox) tensor -> ranks + reconstruction error.
//   hesap.tensor.io.info           : .npy/.npz/.safetensors header inspection (dtype + shape per entry).
//   hesap.tensor.io.philox.f64     : the deterministic Philox tensor fill — reproducible-by-(seed,stream) proof.
//   hesap.tensor.nn.f32            : MLP/CNN inference (f32|q8|i8 tiers) from a .safetensors file, Philox input.
// Anchor: register_tensor_cli_anchor().

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/tensor/batched.hpp>
#include <crd/hesap/tensor/decomp.hpp>
#include <crd/hesap/tensor/einsum_exec.hpp>
#include <crd/hesap/tensor/elementwise.hpp>
#include <crd/hesap/tensor/hyperopt.hpp>
#include <crd/hesap/tensor/io.hpp>
#include <crd/hesap/tensor/nn.hpp>
#include <crd/hesap/tensor/permute.hpp>
#include <crd/hesap/tensor/reduce.hpp>
#include <crd/hesap/tensor/reduce_axes.hpp>
#include <crd/hesap/tensor/sparse.hpp>
#include <crd/hesap/tensor/sparse_mttkrp.hpp>
#include <crd/hesap/tensor/tensor.hpp>
#include <crd/hesap/tensor/tt.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace ht = crd::hesap::tensor;
using crd::containers::ConstSpan;

// Demo-surface guards: the CLI proves the op families, it does not benchmark them.
constexpr crd::u64 kMaxTotalElems = 1ULL << 24U; // 16M elements per synthetic tensor
constexpr crd::u64 kMaxBatchedN = 256U;
constexpr crd::u64 kMaxBatchedB = 4096U;
constexpr crd::u64 kMaxSparseNnz = 1ULL << 22U;
constexpr crd::u64 kMaxCpRank = 64U;

// ---- result helpers (the house cli_register shape) ----------------------

CommandResult error_result(crd::memory::IAllocator* alloc, const char* msg)
{
    CommandResult r{alloc};
    r.ok = false;
    ResultError e{alloc};
    e.error_kind = crd::containers::String{"InvalidArgument", alloc};
    e.error_message = crd::containers::String{msg, alloc};
    r.value = std::move(e);
    return r;
}

CommandResult status_error(crd::memory::IAllocator* alloc, const char* what, const char* status_name)
{
    crd::containers::String s{alloc};
    s.append(what);
    s.append(": ");
    s.append(status_name);
    CommandResult r{alloc};
    r.ok = false;
    ResultError e{alloc};
    e.error_kind = crd::containers::String{"OperationFailed", alloc};
    e.error_message = std::move(s);
    r.value = std::move(e);
    return r;
}

CommandResult status_error(crd::memory::IAllocator* alloc, const char* what, ht::TensorStatus st)
{
    return status_error(alloc, what, ht::to_string(st));
}

CommandResult text_result(crd::memory::IAllocator* alloc, crd::containers::String&& text)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultText t{alloc};
    t.text = std::move(text);
    r.value = std::move(t);
    return r;
}

// Wall time as a Hint diagnostic — keeps the Text output deterministic.
void add_timing(CommandResult& r, crd::memory::IAllocator* alloc, crd::f64 ms)
{
    Diagnostic d{alloc};
    d.level = DiagnosticLevel::Hint;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "elapsed_ms=%.3f", ms);
    d.message = crd::containers::String{buf, alloc};
    r.diagnostics.push_back(std::move(d));
}

[[nodiscard]] std::chrono::steady_clock::time_point tick() noexcept { return std::chrono::steady_clock::now(); }
[[nodiscard]] crd::f64 tock_ms(std::chrono::steady_clock::time_point t0) noexcept
{
    return std::chrono::duration<crd::f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---- text builders -------------------------------------------------------

void append_u64(crd::containers::String& s, crd::u64 v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    s.append(buf);
}

void append_f64(crd::containers::String& s, crd::f64 v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v); // the json_writer float convention
    s.append(buf);
}

void append_shape(crd::containers::String& s, const crd::u64* shape, crd::u32 rank)
{
    if (rank == 0U)
    {
        s.append("scalar");
        return;
    }
    for (crd::u32 d = 0; d < rank; ++d)
    {
        if (d != 0U)
        {
            s.append("x");
        }
        append_u64(s, shape[d]);
    }
}

// sum + Frobenius norm: the deterministic checksum pair (serial accumulation).
template <typename T> void append_checksums(crd::containers::String& s, const T* p, crd::u64 n)
{
    crd::f64 sum = 0.0;
    crd::f64 sq = 0.0;
    for (crd::u64 i = 0; i < n; ++i)
    {
        const crd::f64 v = static_cast<crd::f64>(p[i]);
        sum += v;
        sq += v * v;
    }
    s.append(" sum=");
    append_f64(s, sum);
    s.append(" fro=");
    append_f64(s, std::sqrt(sq));
}

// ---- arg helpers -----------------------------------------------------------

// I64Array -> u64 shape (strictly positive dims, rank + element-count capped).
bool read_shape(ConstSpan<crd::i64> in, crd::u64* shape, crd::u32& rank, crd::u32 min_rank)
{
    if (in.size() < min_rank || in.size() > ht::kMaxRank)
    {
        return false;
    }
    crd::u64 total = 1;
    for (crd::usize d = 0; d < in.size(); ++d)
    {
        if (in[d] <= 0)
        {
            return false;
        }
        shape[d] = static_cast<crd::u64>(in[d]);
        if (total > kMaxTotalElems / shape[d])
        {
            return false;
        }
        total *= shape[d];
    }
    rank = static_cast<crd::u32>(in.size());
    return true;
}

void add_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc, ParamKind kind,
               bool required)
{
    ParamSchema p{alloc};
    p.name = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind = kind;
    p.required = required;
    s.params.push_back(std::move(p));
}

void add_enum_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc,
                    const char* values, bool required)
{
    ParamSchema p{alloc};
    p.name = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind = ParamKind::Enum;
    p.enum_values = crd::containers::String{values, alloc};
    p.required = required;
    s.params.push_back(std::move(p));
}

CommandSchema make_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc,
                          bool fs_read = false)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = OutputKind::Text;
    s.required_caps.bits = Capability::kHesapCompute | (fs_read ? Capability::kFsRead : 0U);
    s.idempotent = true;
    return s;
}

// =======================================================================
// hesap.tensor.einsum.f64
// =======================================================================

CommandSchema make_einsum_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.einsum.f64",
                                  "Einsum on Philox-filled operands: parse spec, plan the contraction path, "
                                  "execute (TTGT over the deterministic GEMM). Out = shapes + checksums.");
    add_param(s, alloc, "spec", "subscripts, e.g. \"ab,bc->ac\" (letters a-z; no ellipsis in the CLI demo)",
              ParamKind::String, true);
    add_param(s, alloc, "sizes", "(I64Array) extent per distinct index letter, ascending alphabetically",
              ParamKind::I64, true);
    add_enum_param(s, alloc, "optimize", "path optimizer (default greedy)", "greedy|optimal", false);
    add_param(s, alloc, "seed", "Philox seed for the operand fills (default 1)", ParamKind::U64, false);
    return s;
}

CommandResult impl_einsum(const CommandArgs& args)
{
    const auto spec_view = args.get_string("spec");
    const auto sizes = args.get_i64_array("sizes");
    if (spec_view.empty() || sizes.empty())
    {
        return error_result(args.alloc, "tensor.einsum: spec and sizes are required");
    }
    crd::containers::String spec{args.alloc};
    spec.append(spec_view);
    // ranks per operand = letters between commas (left of "->"); the CLI demo rejects ellipsis
    crd::u32 ranks[ht::kEinsumMaxOperands];
    crd::u32 n_ops = 0;
    {
        crd::u32 r = 0;
        for (const char* p = spec.c_str(); *p != '\0' && *p != '-'; ++p)
        {
            if (*p == '.')
            {
                return error_result(args.alloc, "tensor.einsum: ellipsis is not supported by the CLI demo");
            }
            if (*p == ',')
            {
                if (n_ops + 1U >= ht::kEinsumMaxOperands)
                {
                    return error_result(args.alloc, "tensor.einsum: too many operands");
                }
                ranks[n_ops++] = r;
                r = 0;
                continue;
            }
            ++r;
        }
        ranks[n_ops++] = r;
    }
    ht::EinsumExpr e;
    ht::TensorStatus st = ht::einsum_parse(spec.c_str(), {ranks, n_ops}, e);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.einsum: parse failed", st);
    }
    // distinct indices ascending by letter <- sizes
    crd::u64 idx_size[ht::kEinsumMaxIndices];
    for (crd::u32 i = 0; i < ht::kEinsumMaxIndices; ++i)
    {
        idx_size[i] = 1;
    }
    crd::u64 all_mask = e.out_mask;
    for (crd::u32 t = 0; t < e.n_ops; ++t)
    {
        all_mask |= e.term[t].mask;
    }
    {
        crd::usize k = 0;
        for (crd::u32 id = 0; id < ht::kEinsumMaxIndices; ++id)
        {
            if ((all_mask & (1ULL << id)) == 0U)
            {
                continue;
            }
            if (k >= sizes.size() || sizes[k] <= 0)
            {
                return error_result(args.alloc, "tensor.einsum: sizes must list one positive extent per index");
            }
            idx_size[id] = static_cast<crd::u64>(sizes[k]);
            ++k;
        }
        if (k != sizes.size())
        {
            return error_result(args.alloc, "tensor.einsum: sizes length must equal the distinct index count");
        }
    }
    // Philox operands (stream = operand position)
    const crd::u64 seed = args.get_u64("seed").value_or(1U);
    ht::Tensor<crd::f64> ops[ht::kEinsumMaxOperands];
    ht::TensorView<const crd::f64> views[ht::kEinsumMaxOperands];
    for (crd::u32 t = 0; t < e.n_ops; ++t)
    {
        crd::u64 shape[ht::kMaxRank];
        crd::u64 total = 1;
        for (crd::u32 d = 0; d < e.term[t].count; ++d)
        {
            shape[d] = idx_size[e.term[t].idx[d]];
            total *= shape[d];
        }
        if (total > kMaxTotalElems)
        {
            return error_result(args.alloc, "tensor.einsum: operand exceeds the CLI element cap");
        }
        ops[t] = ht::Tensor<crd::f64>(args.alloc);
        st = ops[t].resize({shape, e.term[t].count});
        if (st != ht::TensorStatus::Ok)
        {
            return status_error(args.alloc, "tensor.einsum: operand alloc failed", st);
        }
        (void)ht::philox_fill_uniform<crd::f64>(ops[t].view(), seed, t);
        views[t] = ops[t].view();
    }
    const auto opt_name = args.get_string("optimize");
    const ht::EinsumOptimize mode =
        opt_name == "optimal" ? ht::EinsumOptimize::Optimal : ht::EinsumOptimize::Greedy;
    ht::EinsumPlan plan;
    st = ht::einsum_plan_build(e, idx_size, mode, plan);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.einsum: plan failed", st);
    }
    ht::Tensor<crd::f64> out(args.alloc);
    const auto t0 = tick();
    st = ht::einsum_execute<crd::f64>(plan, {views, e.n_ops}, out, args.alloc);
    const crd::f64 ms = tock_ms(t0);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.einsum: execute failed", st);
    }
    crd::containers::String s{args.alloc};
    s.append("ops=");
    append_u64(s, e.n_ops);
    s.append(" steps=");
    append_u64(s, plan.n_steps);
    s.append(" flops=");
    append_u64(s, plan.total_flops);
    s.append(" out=");
    append_shape(s, out.shape().data(), out.rank());
    append_checksums(s, out.data(), out.size());
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.ew.f64
// =======================================================================

CommandSchema make_ew_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.ew.f64",
                                  "Tier-D elementwise binary op on two Philox tensors of the given shape. "
                                  "Out = shape + checksums of the destination.");
    add_param(s, alloc, "shape", "(I64Array) tensor shape, rank 1..8", ParamKind::I64, true);
    add_enum_param(s, alloc, "op", "binary op (default add)", "add|sub|mul|div|min|max", false);
    add_param(s, alloc, "seed", "Philox seed (default 1; a=stream 0, b=stream 1)", ParamKind::U64, false);
    return s;
}

CommandResult impl_ew(const CommandArgs& args)
{
    crd::u64 shape[ht::kMaxRank];
    crd::u32 rank = 0;
    if (!read_shape(args.get_i64_array("shape"), shape, rank, 1U))
    {
        return error_result(args.alloc, "tensor.ew: shape must be rank 1..8 with positive dims (capped)");
    }
    ht::BinaryOp op = ht::BinaryOp::Add;
    const auto opn = args.get_string("op");
    if (opn == "sub")
    {
        op = ht::BinaryOp::Sub;
    }
    else if (opn == "mul")
    {
        op = ht::BinaryOp::Mul;
    }
    else if (opn == "div")
    {
        op = ht::BinaryOp::Div;
    }
    else if (opn == "min")
    {
        op = ht::BinaryOp::Min;
    }
    else if (opn == "max")
    {
        op = ht::BinaryOp::Max;
    }
    else if (!opn.empty() && opn != "add")
    {
        return error_result(args.alloc, "tensor.ew: op must be add|sub|mul|div|min|max");
    }
    const crd::u64 seed = args.get_u64("seed").value_or(1U);
    ht::Tensor<crd::f64> a(args.alloc);
    ht::Tensor<crd::f64> b(args.alloc);
    ht::Tensor<crd::f64> dst(args.alloc);
    if (a.resize({shape, rank}) != ht::TensorStatus::Ok || b.resize({shape, rank}) != ht::TensorStatus::Ok ||
        dst.resize({shape, rank}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.ew: allocation failed");
    }
    (void)ht::philox_fill_uniform<crd::f64>(a.view(), seed, 0U);
    (void)ht::philox_fill_uniform<crd::f64>(b.view(), seed, 1U);
    const auto t0 = tick();
    const ht::TensorStatus st =
        ht::ew_binary<crd::f64>(op, ht::TensorView<const crd::f64>(a.view()),
                                ht::TensorView<const crd::f64>(b.view()), dst.view());
    const crd::f64 ms = tock_ms(t0);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.ew: ew_binary failed", st);
    }
    crd::containers::String s{args.alloc};
    s.append("shape=");
    append_shape(s, shape, rank);
    append_checksums(s, dst.data(), dst.size());
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.reduce.f64
// =======================================================================

CommandSchema make_reduce_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.reduce.f64",
                                  "Tier-R reduction of a Philox tensor: full (no axes_mask) or over the axes "
                                  "bitmask (bit d = reduce dim d). Out = value or reduced shape + checksums.");
    add_param(s, alloc, "shape", "(I64Array) tensor shape, rank 1..8", ParamKind::I64, true);
    add_enum_param(s, alloc, "op", "reduction (default sum)", "sum|prod|min|max|mean", false);
    add_param(s, alloc, "axes_mask", "bitmask of dims to reduce (absent/0 = reduce ALL)", ParamKind::U64, false);
    add_param(s, alloc, "seed", "Philox seed (default 1)", ParamKind::U64, false);
    return s;
}

CommandResult impl_reduce(const CommandArgs& args)
{
    crd::u64 shape[ht::kMaxRank];
    crd::u32 rank = 0;
    if (!read_shape(args.get_i64_array("shape"), shape, rank, 1U))
    {
        return error_result(args.alloc, "tensor.reduce: shape must be rank 1..8 with positive dims (capped)");
    }
    const auto opn = args.get_string("op");
    const crd::u64 seed = args.get_u64("seed").value_or(1U);
    ht::Tensor<crd::f64> x(args.alloc);
    if (x.resize({shape, rank}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.reduce: allocation failed");
    }
    (void)ht::philox_fill_uniform<crd::f64>(x.view(), seed, 0U);
    const ht::TensorView<const crd::f64> xv = x.view();

    const crd::u64 mask64 = args.get_u64("axes_mask").value_or(0U);
    crd::containers::String s{args.alloc};
    crd::f64 ms = 0.0;
    if (mask64 == 0U) // full reduce -> scalar
    {
        crd::f64 value = 0.0;
        const auto t0 = tick();
        if (opn.empty() || opn == "sum")
        {
            value = ht::reduce_sum<crd::f64>(xv);
        }
        else if (opn == "prod")
        {
            value = ht::reduce_prod<crd::f64>(xv);
        }
        else if (opn == "min")
        {
            value = ht::reduce_min<crd::f64>(xv);
        }
        else if (opn == "max")
        {
            value = ht::reduce_max<crd::f64>(xv);
        }
        else if (opn == "mean")
        {
            value = ht::reduce_mean<crd::f64>(xv);
        }
        else
        {
            return error_result(args.alloc, "tensor.reduce: op must be sum|prod|min|max|mean");
        }
        ms = tock_ms(t0);
        s.append("shape=");
        append_shape(s, shape, rank);
        s.append(" value=");
        append_f64(s, value);
    }
    else
    {
        ht::ReduceOp op = ht::ReduceOp::Sum;
        if (opn == "prod")
        {
            op = ht::ReduceOp::Prod;
        }
        else if (opn == "min")
        {
            op = ht::ReduceOp::Min;
        }
        else if (opn == "max")
        {
            op = ht::ReduceOp::Max;
        }
        else if (opn == "mean")
        {
            op = ht::ReduceOp::Mean;
        }
        else if (!opn.empty() && opn != "sum")
        {
            return error_result(args.alloc, "tensor.reduce: op must be sum|prod|min|max|mean");
        }
        crd::u64 kept[ht::kMaxRank];
        crd::u32 nk = 0;
        for (crd::u32 d = 0; d < rank; ++d)
        {
            if ((mask64 & (1ULL << d)) == 0U)
            {
                kept[nk++] = shape[d];
            }
        }
        ht::Tensor<crd::f64> dst(args.alloc);
        if (dst.resize({kept, nk}) != ht::TensorStatus::Ok)
        {
            return error_result(args.alloc, "tensor.reduce: allocation failed");
        }
        const auto t0 = tick();
        const ht::TensorStatus st = ht::reduce_axes<crd::f64>(op, xv, static_cast<crd::u32>(mask64), dst.view());
        ms = tock_ms(t0);
        if (st != ht::TensorStatus::Ok)
        {
            return status_error(args.alloc, "tensor.reduce: reduce_axes failed", st);
        }
        s.append("shape=");
        append_shape(s, shape, rank);
        s.append(" out=");
        append_shape(s, kept, nk);
        append_checksums(s, dst.data(), dst.size());
    }
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.permute.f64
// =======================================================================

CommandSchema make_permute_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.permute.f64",
                                  "permute_copy (the v14-d kernel) of a Philox tensor: out dim d = in dim "
                                  "order[d]. Out = permuted shape + checksums.");
    add_param(s, alloc, "shape", "(I64Array) tensor shape, rank 1..8", ParamKind::I64, true);
    add_param(s, alloc, "order", "(I64Array) permutation of 0..rank-1", ParamKind::I64, true);
    add_param(s, alloc, "seed", "Philox seed (default 1)", ParamKind::U64, false);
    return s;
}

CommandResult impl_permute(const CommandArgs& args)
{
    crd::u64 shape[ht::kMaxRank];
    crd::u32 rank = 0;
    if (!read_shape(args.get_i64_array("shape"), shape, rank, 1U))
    {
        return error_result(args.alloc, "tensor.permute: shape must be rank 1..8 with positive dims (capped)");
    }
    const auto order_in = args.get_i64_array("order");
    if (order_in.size() != rank)
    {
        return error_result(args.alloc, "tensor.permute: order length must equal the shape rank");
    }
    crd::u32 order[ht::kMaxRank];
    crd::u32 seen = 0;
    for (crd::u32 d = 0; d < rank; ++d)
    {
        if (order_in[d] < 0 || order_in[d] >= static_cast<crd::i64>(rank) ||
            (seen & (1U << static_cast<crd::u32>(order_in[d]))) != 0U)
        {
            return error_result(args.alloc, "tensor.permute: order must be a permutation of 0..rank-1");
        }
        order[d] = static_cast<crd::u32>(order_in[d]);
        seen |= 1U << order[d];
    }
    const crd::u64 seed = args.get_u64("seed").value_or(1U);
    ht::Tensor<crd::f64> x(args.alloc);
    if (x.resize({shape, rank}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.permute: allocation failed");
    }
    (void)ht::philox_fill_uniform<crd::f64>(x.view(), seed, 0U);
    ht::Tensor<crd::f64> dst(args.alloc);
    const auto t0 = tick();
    const ht::TensorStatus st =
        ht::permute_copy<crd::f64>(ht::TensorView<const crd::f64>(x.view()), {order, rank}, dst);
    const crd::f64 ms = tock_ms(t0);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.permute: permute_copy failed", st);
    }
    crd::containers::String s{args.alloc};
    s.append("in=");
    append_shape(s, shape, rank);
    s.append(" out=");
    append_shape(s, dst.shape().data(), dst.rank());
    append_checksums(s, dst.data(), dst.size());
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.batched.f64
// =======================================================================

CommandSchema make_batched_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.batched.f64",
                                  "Batched LA on Philox batches: gemm C=A*B [B,m,k]x[B,k,n]; cholesky/lu factor "
                                  "[B,n,n] (SPD / diag-boosted); svd (one-sided Jacobi, n<=32). Out = shapes, "
                                  "info counts, checksums.");
    add_enum_param(s, alloc, "kind", "op family member", "gemm|cholesky|lu|svd", true);
    add_param(s, alloc, "batch", "batch count B >= 1", ParamKind::I64, true);
    add_param(s, alloc, "n", "matrix dim n >= 1", ParamKind::I64, true);
    add_param(s, alloc, "m", "gemm rows m (default n)", ParamKind::I64, false);
    add_param(s, alloc, "k", "gemm inner dim k (default n)", ParamKind::I64, false);
    add_param(s, alloc, "seed", "Philox seed (default 1)", ParamKind::U64, false);
    return s;
}

CommandResult impl_batched(const CommandArgs& args)
{
    const auto kind = args.get_string("kind");
    const crd::i64 batch_i = args.get_i64("batch").value_or(0);
    const crd::i64 n_i = args.get_i64("n").value_or(0);
    if (batch_i < 1 || n_i < 1 || static_cast<crd::u64>(batch_i) > kMaxBatchedB ||
        static_cast<crd::u64>(n_i) > kMaxBatchedN)
    {
        return error_result(args.alloc, "tensor.batched: need 1 <= batch <= 4096 and 1 <= n <= 256");
    }
    const crd::u64 bsz = static_cast<crd::u64>(batch_i);
    const crd::u64 n = static_cast<crd::u64>(n_i);
    const crd::u64 seed = args.get_u64("seed").value_or(1U);

    crd::containers::String s{args.alloc};
    crd::f64 ms = 0.0;
    if (kind == "gemm")
    {
        const crd::i64 m_i = args.get_i64("m").value_or(n_i);
        const crd::i64 k_i = args.get_i64("k").value_or(n_i);
        if (m_i < 1 || k_i < 1 || static_cast<crd::u64>(m_i) > kMaxBatchedN ||
            static_cast<crd::u64>(k_i) > kMaxBatchedN)
        {
            return error_result(args.alloc, "tensor.batched: need 1 <= m,k <= 256");
        }
        const crd::u64 m = static_cast<crd::u64>(m_i);
        const crd::u64 k = static_cast<crd::u64>(k_i);
        const crd::u64 sa[3] = {bsz, m, k};
        const crd::u64 sb[3] = {bsz, k, n};
        const crd::u64 sc[3] = {bsz, m, n};
        ht::Tensor<crd::f64> a(args.alloc);
        ht::Tensor<crd::f64> b(args.alloc);
        ht::Tensor<crd::f64> c(args.alloc);
        if (a.resize({sa, 3}) != ht::TensorStatus::Ok || b.resize({sb, 3}) != ht::TensorStatus::Ok ||
            c.resize({sc, 3}) != ht::TensorStatus::Ok)
        {
            return error_result(args.alloc, "tensor.batched: allocation failed");
        }
        (void)ht::philox_fill_uniform<crd::f64>(a.view(), seed, 0U);
        (void)ht::philox_fill_uniform<crd::f64>(b.view(), seed, 1U);
        c.zero();
        const auto t0 = tick();
        const ht::TensorStatus st =
            ht::batched_gemm<crd::f64>(1.0, ht::TensorView<const crd::f64>(a.view()),
                                       ht::TensorView<const crd::f64>(b.view()), 0.0, c.view(), args.alloc, 1U);
        ms = tock_ms(t0);
        if (st != ht::TensorStatus::Ok)
        {
            return status_error(args.alloc, "tensor.batched: batched_gemm failed", st);
        }
        s.append("kind=gemm c=");
        append_shape(s, sc, 3U);
        append_checksums(s, c.data(), c.size());
    }
    else if (kind == "cholesky" || kind == "lu" || kind == "svd")
    {
        const crd::u64 sa[3] = {bsz, n, n};
        ht::Tensor<crd::f64> a(args.alloc);
        if (a.resize({sa, 3}) != ht::TensorStatus::Ok)
        {
            return error_result(args.alloc, "tensor.batched: allocation failed");
        }
        (void)ht::philox_fill_uniform<crd::f64>(a.view(), seed, 0U);
        crd::containers::Array<crd::i32> info(args.alloc);
        info.resize(static_cast<crd::usize>(bsz));
        if (kind == "cholesky")
        {
            // symmetrize + diagonal dominance -> SPD
            for (crd::u64 bi = 0; bi < bsz; ++bi)
            {
                crd::f64* p = a.data() + bi * n * n;
                for (crd::u64 i = 0; i < n; ++i)
                {
                    for (crd::u64 j = i + 1U; j < n; ++j)
                    {
                        const crd::f64 v = 0.5 * (p[i * n + j] + p[j * n + i]);
                        p[i * n + j] = v;
                        p[j * n + i] = v;
                    }
                    p[i * n + i] += static_cast<crd::f64>(n);
                }
            }
            const auto t0 = tick();
            const ht::TensorStatus st =
                ht::batched_cholesky_factor<crd::f64>(a.view(), {info.data(), info.size()}, 1U);
            ms = tock_ms(t0);
            if (st != ht::TensorStatus::Ok)
            {
                return status_error(args.alloc, "tensor.batched: batched_cholesky_factor failed", st);
            }
            s.append("kind=cholesky");
        }
        else if (kind == "lu")
        {
            for (crd::u64 bi = 0; bi < bsz; ++bi) // diag boost: keep the demo comfortably non-singular
            {
                crd::f64* p = a.data() + bi * n * n;
                for (crd::u64 i = 0; i < n; ++i)
                {
                    p[i * n + i] += 1.0;
                }
            }
            crd::containers::Array<crd::i32> piv(args.alloc);
            piv.resize(static_cast<crd::usize>(bsz * n));
            const auto t0 = tick();
            const ht::TensorStatus st = ht::batched_lu_factor<crd::f64>(
                a.view(), {piv.data(), piv.size()}, {info.data(), info.size()}, 1U);
            ms = tock_ms(t0);
            if (st != ht::TensorStatus::Ok)
            {
                return status_error(args.alloc, "tensor.batched: batched_lu_factor failed", st);
            }
            crd::i64 piv_sum = 0;
            for (crd::usize i = 0; i < piv.size(); ++i)
            {
                piv_sum += piv[i];
            }
            s.append("kind=lu piv_sum=");
            append_u64(s, static_cast<crd::u64>(piv_sum));
        }
        else // svd
        {
            ht::Tensor<crd::f64> u(args.alloc);
            ht::Tensor<crd::f64> v(args.alloc);
            if (u.resize({sa, 3}) != ht::TensorStatus::Ok || v.resize({sa, 3}) != ht::TensorStatus::Ok)
            {
                return error_result(args.alloc, "tensor.batched: allocation failed");
            }
            crd::containers::Array<crd::f64> sigma(args.alloc);
            sigma.resize(static_cast<crd::usize>(bsz * n));
            const auto t0 = tick();
            const ht::TensorStatus st = ht::batched_svd_small<crd::f64>(
                ht::TensorView<const crd::f64>(a.view()), u.view(), {sigma.data(), sigma.size()}, v.view(),
                {info.data(), info.size()}, 30U, 1U);
            ms = tock_ms(t0);
            if (st != ht::TensorStatus::Ok)
            {
                return status_error(args.alloc, "tensor.batched: batched_svd_small failed", st);
            }
            crd::u64 nonconv = 0;
            for (crd::usize i = 0; i < info.size(); ++i)
            {
                nonconv += info[i] != 0 ? 1U : 0U;
            }
            s.append("kind=svd nonconv=");
            append_u64(s, nonconv);
            s.append(" a=");
            append_shape(s, sa, 3U);
            append_checksums(s, sigma.data(), sigma.size());
            CommandResult rr = text_result(args.alloc, std::move(s));
            add_timing(rr, args.alloc, ms);
            return rr;
        }
        crd::u64 bad = 0;
        for (crd::usize i = 0; i < info.size(); ++i)
        {
            bad += info[i] != 0 ? 1U : 0U;
        }
        s.append(" a=");
        append_shape(s, sa, 3U);
        s.append(" info_bad=");
        append_u64(s, bad);
        append_checksums(s, a.data(), a.size());
    }
    else
    {
        return error_result(args.alloc, "tensor.batched: kind must be gemm|cholesky|lu|svd");
    }
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.hyperopt
// =======================================================================

CommandSchema make_hyperopt_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.hyperopt",
                                  "The v14-g contraction-path hyper-optimizer on a matmul chain: dims[i] are the "
                                  "chain extents (operand i = indices {i,i+1}, output = {0,last}). Deterministic "
                                  "by seed at any worker count. Out = flops, largest intermediate, steps, winner.");
    add_param(s, alloc, "dims", "(I64Array) chain extents, length >= 4 (>= 3 operands)", ParamKind::I64, true);
    add_param(s, alloc, "ntrials", "trial budget (default 8, min 2)", ParamKind::I64, false);
    add_param(s, alloc, "seed", "Philox seed (default 0)", ParamKind::U64, false);
    add_param(s, alloc, "target_size", "slice the winner to this max intermediate size (0 = off)", ParamKind::U64,
              false);
    return s;
}

const char* hyper_status_name(ht::HyperStatus st)
{
    switch (st)
    {
    case ht::HyperStatus::Ok:
        return "Ok";
    case ht::HyperStatus::BadInput:
        return "BadInput";
    case ht::HyperStatus::AllocFailed:
        return "AllocFailed";
    case ht::HyperStatus::NotFound:
        return "NotFound";
    }
    return "Unknown";
}

CommandResult impl_hyperopt(const CommandArgs& args)
{
    const auto dims = args.get_i64_array("dims");
    if (dims.size() < 4U || dims.size() > 32U)
    {
        return error_result(args.alloc, "tensor.hyperopt: dims needs 4..32 chain extents");
    }
    const crd::u32 n_idx = static_cast<crd::u32>(dims.size());
    const crd::u32 n_ops = n_idx - 1U;
    crd::u64 sizes[32];
    for (crd::u32 i = 0; i < n_idx; ++i)
    {
        if (dims[i] < 1)
        {
            return error_result(args.alloc, "tensor.hyperopt: dims must be positive");
        }
        sizes[i] = static_cast<crd::u64>(dims[i]);
    }
    crd::u32 id_pool[2U * 31U];
    ConstSpan<crd::u32> spans[31];
    for (crd::u32 t = 0; t < n_ops; ++t)
    {
        id_pool[2U * t] = t;
        id_pool[2U * t + 1U] = t + 1U;
        spans[t] = ConstSpan<crd::u32>{id_pool + 2U * t, 2U};
    }
    const crd::u32 out_ids[2] = {0U, n_ops};
    ht::HyperOptOptions opts;
    const crd::i64 ntrials = args.get_i64("ntrials").value_or(8);
    opts.ntrials = ntrials < 2 ? 2U : static_cast<crd::u32>(ntrials);
    opts.seed = args.get_u64("seed").value_or(0U);
    opts.target_size = args.get_u64("target_size").value_or(0U);
    ht::HyperOptResult result(args.alloc);
    const auto t0 = tick();
    const ht::HyperStatus st = ht::hyper_optimize({spans, n_ops}, {out_ids, 2U}, {sizes, n_idx}, opts,
                                                  args.alloc, result);
    const crd::f64 ms = tock_ms(t0);
    if (st != ht::HyperStatus::Ok)
    {
        return status_error(args.alloc, "tensor.hyperopt: hyper_optimize failed", hyper_status_name(st));
    }
    crd::containers::String s{args.alloc};
    s.append("ops=");
    append_u64(s, n_ops);
    s.append(" steps=");
    append_u64(s, result.plan.steps.size());
    s.append(" flops=");
    append_f64(s, result.plan.total_flops);
    s.append(" max_size=");
    append_u64(s, result.plan.max_size);
    s.append(" winner_trial=");
    append_u64(s, result.winner_trial);
    s.append(" sliced=");
    append_u64(s, result.sliced ? 1U : 0U);
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.sparse.mttkrp.f64
// =======================================================================

CommandSchema make_sparse_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.sparse.mttkrp.f64",
                                  "Sparse tensor demo: build a Philox COO (random coords/values), canonicalize, "
                                  "convert to CSF (identity mode order) and run MTTKRP against Philox factors. "
                                  "Out = nnz after dedup + output checksums.");
    add_param(s, alloc, "shape", "(I64Array) tensor shape, rank 2..8", ParamKind::I64, true);
    add_param(s, alloc, "nnz", "nonzero draws before dedup (>= 1, capped)", ParamKind::I64, true);
    add_param(s, alloc, "rank", "factor columns R (default 8, <= 64)", ParamKind::I64, false);
    add_param(s, alloc, "seed", "Philox seed (default 1)", ParamKind::U64, false);
    return s;
}

CommandResult impl_sparse(const CommandArgs& args)
{
    crd::u64 shape[ht::kMaxRank];
    crd::u32 rank = 0;
    if (!read_shape(args.get_i64_array("shape"), shape, rank, 2U))
    {
        return error_result(args.alloc, "tensor.sparse.mttkrp: shape must be rank 2..8 with positive dims");
    }
    const crd::i64 nnz_i = args.get_i64("nnz").value_or(0);
    if (nnz_i < 1 || static_cast<crd::u64>(nnz_i) > kMaxSparseNnz)
    {
        return error_result(args.alloc, "tensor.sparse.mttkrp: need 1 <= nnz <= 4194304");
    }
    const crd::i64 fr_i = args.get_i64("rank").value_or(8);
    if (fr_i < 1 || static_cast<crd::u64>(fr_i) > kMaxCpRank)
    {
        return error_result(args.alloc, "tensor.sparse.mttkrp: need 1 <= rank <= 64");
    }
    const crd::u64 fr = static_cast<crd::u64>(fr_i);
    const crd::u64 seed = args.get_u64("seed").value_or(1U);

    ht::SparseCooBuilder<crd::f64> builder(args.alloc, {shape, rank});
    builder.reserve(static_cast<crd::usize>(nnz_i));
    {
        crd::hesap::stats::PhiloxRng rng(seed, 0U);
        for (crd::i64 t = 0; t < nnz_i; ++t)
        {
            crd::u32 idx[ht::kMaxRank];
            for (crd::u32 m = 0; m < rank; ++m)
            {
                crd::u64 v = static_cast<crd::u64>(rng.next_f64() * static_cast<crd::f64>(shape[m]));
                if (v >= shape[m])
                {
                    v = shape[m] - 1U;
                }
                idx[m] = static_cast<crd::u32>(v);
            }
            builder.add({idx, rank}, 2.0 * rng.next_f64() - 1.0);
        }
    }
    ht::SparseCoo<crd::f64> coo(args.alloc);
    ht::TensorStatus st = builder.compress(coo);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.sparse.mttkrp: COO compress failed", st);
    }
    crd::u32 order[ht::kMaxRank];
    for (crd::u32 m = 0; m < rank; ++m)
    {
        order[m] = m;
    }
    ht::SparseCsf<crd::f64> csf(args.alloc);
    st = ht::coo_to_csf<crd::f64>(coo, {order, rank}, csf);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.sparse.mttkrp: coo_to_csf failed", st);
    }
    ht::Tensor<crd::f64> factors[ht::kMaxRank];
    ht::TensorView<const crd::f64> fviews[ht::kMaxRank];
    for (crd::u32 m = 0; m < rank; ++m)
    {
        factors[m] = ht::Tensor<crd::f64>(args.alloc);
        const crd::u64 fshape[2] = {shape[m], fr};
        if (factors[m].resize({fshape, 2U}) != ht::TensorStatus::Ok)
        {
            return error_result(args.alloc, "tensor.sparse.mttkrp: factor allocation failed");
        }
        (void)ht::philox_fill_uniform<crd::f64>(factors[m].view(), seed, 100U + m);
        fviews[m] = factors[m].view();
    }
    ht::Tensor<crd::f64> out(args.alloc);
    const crd::u64 oshape[2] = {shape[0], fr};
    if (out.resize({oshape, 2U}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.sparse.mttkrp: output allocation failed");
    }
    const auto t0 = tick();
    st = ht::mttkrp<crd::f64>(csf, {fviews, rank}, out.view(), args.alloc, 1U);
    const crd::f64 ms = tock_ms(t0);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.sparse.mttkrp: mttkrp failed", st);
    }
    crd::containers::String s{args.alloc};
    s.append("shape=");
    append_shape(s, shape, rank);
    s.append(" nnz=");
    append_u64(s, coo.nnz());
    s.append(" out=");
    append_shape(s, oshape, 2U);
    append_checksums(s, out.data(), out.size());
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.decomp.f64
// =======================================================================

CommandSchema make_decomp_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.decomp.f64",
                                  "Tensor decompositions of a Philox tensor: CP-ALS (rank-R factors) or Tucker "
                                  "via HOSVD/HOOI (rank clamped per mode). Fixed iteration budget (tol=0) -> "
                                  "deterministic. Out = iters, fit, rec_error + factor checksums.");
    add_enum_param(s, alloc, "method", "decomposition", "cp|hosvd|hooi", true);
    add_param(s, alloc, "shape", "(I64Array) tensor shape, rank 2..8", ParamKind::I64, true);
    add_param(s, alloc, "rank", "CP rank / Tucker rank per mode (default 4, <= 64)", ParamKind::I64, false);
    add_param(s, alloc, "iters", "iteration budget (default 10)", ParamKind::I64, false);
    add_param(s, alloc, "seed", "Philox seed (default 1)", ParamKind::U64, false);
    return s;
}

CommandResult impl_decomp(const CommandArgs& args)
{
    const auto method = args.get_string("method");
    crd::u64 shape[ht::kMaxRank];
    crd::u32 rank = 0;
    if (!read_shape(args.get_i64_array("shape"), shape, rank, 2U))
    {
        return error_result(args.alloc, "tensor.decomp: shape must be rank 2..8 with positive dims (capped)");
    }
    const crd::i64 r_i = args.get_i64("rank").value_or(4);
    if (r_i < 1 || static_cast<crd::u64>(r_i) > kMaxCpRank)
    {
        return error_result(args.alloc, "tensor.decomp: need 1 <= rank <= 64");
    }
    const crd::u64 drank = static_cast<crd::u64>(r_i);
    const crd::i64 iters_i = args.get_i64("iters").value_or(10);
    if (iters_i < 1 || iters_i > 1000)
    {
        return error_result(args.alloc, "tensor.decomp: need 1 <= iters <= 1000");
    }
    const crd::u64 seed = args.get_u64("seed").value_or(1U);
    ht::Tensor<crd::f64> x(args.alloc);
    if (x.resize({shape, rank}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.decomp: allocation failed");
    }
    (void)ht::philox_fill_uniform<crd::f64>(x.view(), seed, 0U);
    const ht::TensorView<const crd::f64> xv = x.view();

    ht::Tensor<crd::f64> factors[ht::kMaxRank];
    for (crd::u32 m = 0; m < rank; ++m)
    {
        factors[m] = ht::Tensor<crd::f64>(args.alloc);
    }
    crd::containers::String s{args.alloc};
    crd::f64 ms = 0.0;
    if (method == "cp")
    {
        crd::containers::Array<crd::f64> weights(args.alloc);
        weights.resize(static_cast<crd::usize>(drank));
        ht::CpOptions<crd::f64> opts;
        opts.max_iters = static_cast<crd::u32>(iters_i);
        opts.tol = 0.0; // fixed budget: completed contract, deterministic
        opts.seed = seed;
        ht::CpInfo<crd::f64> info;
        const auto t0 = tick();
        const ht::DecompStatus ds = ht::cp_als<crd::f64>(xv, drank, {factors, rank},
                                                         {weights.data(), weights.size()}, info, args.alloc, opts);
        ms = tock_ms(t0);
        if (ds != ht::DecompStatus::Ok)
        {
            return status_error(args.alloc, "tensor.decomp: cp_als failed", ht::to_string(ds));
        }
        s.append("method=cp shape=");
        append_shape(s, shape, rank);
        s.append(" rank=");
        append_u64(s, drank);
        s.append(" iters=");
        append_u64(s, info.iters);
        s.append(" fit=");
        append_f64(s, static_cast<crd::f64>(info.fit));
        s.append(" rec_error=");
        append_f64(s, static_cast<crd::f64>(info.rec_error));
        append_checksums(s, factors[0].data(), factors[0].size());
    }
    else if (method == "hosvd" || method == "hooi")
    {
        crd::u64 tranks[ht::kMaxRank];
        for (crd::u32 m = 0; m < rank; ++m) // clamp per mode: 1 <= R_m <= min(I_m, prod I_k)
        {
            crd::u64 prod_others = 1;
            for (crd::u32 k = 0; k < rank; ++k)
            {
                if (k != m && prod_others < kMaxTotalElems)
                {
                    prod_others *= shape[k];
                }
            }
            crd::u64 cap = shape[m] < prod_others ? shape[m] : prod_others;
            tranks[m] = drank < cap ? drank : cap;
        }
        ht::Tensor<crd::f64> core(args.alloc);
        ht::TuckerInfo<crd::f64> info;
        ht::DecompStatus ds = ht::DecompStatus::Ok;
        const auto t0 = tick();
        if (method == "hosvd")
        {
            ds = ht::hosvd<crd::f64>(xv, {tranks, rank}, {factors, rank}, core, args.alloc, &info);
        }
        else
        {
            ht::TuckerOptions<crd::f64> topts;
            topts.max_iters = static_cast<crd::u32>(iters_i);
            topts.tol = 0.0; // fixed budget
            ds = ht::hooi<crd::f64>(xv, {tranks, rank}, {factors, rank}, core, info, args.alloc, topts);
        }
        ms = tock_ms(t0);
        if (ds != ht::DecompStatus::Ok)
        {
            return status_error(args.alloc, "tensor.decomp: tucker failed", ht::to_string(ds));
        }
        s.append("method=");
        s.append(method);
        s.append(" shape=");
        append_shape(s, shape, rank);
        s.append(" core=");
        append_shape(s, core.shape().data(), core.rank());
        s.append(" iters=");
        append_u64(s, info.iters);
        s.append(" fit=");
        append_f64(s, static_cast<crd::f64>(info.fit));
        s.append(" rec_error=");
        append_f64(s, static_cast<crd::f64>(info.rec_error));
        append_checksums(s, core.data(), core.size());
    }
    else
    {
        return error_result(args.alloc, "tensor.decomp: method must be cp|hosvd|hooi");
    }
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.tt.f64
// =======================================================================

CommandSchema make_tt_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.tt.f64",
                                  "Tensor-train compression: tt_svd of the Hilbert tensor 1/(1+sum(idx)) (or a "
                                  "Philox tensor) at relative tolerance tol. Out = TT ranks, core elements, "
                                  "compression ratio, reconstruction error.");
    add_param(s, alloc, "shape", "(I64Array) tensor shape, rank 2..8", ParamKind::I64, true);
    add_enum_param(s, alloc, "source", "synthetic input (default hilbert)", "hilbert|uniform", false);
    add_param(s, alloc, "tol", "relative Frobenius tolerance (default 1e-8)", ParamKind::F64, false);
    add_param(s, alloc, "max_rank", "TT rank cap (default 0 = uncapped)", ParamKind::I64, false);
    add_param(s, alloc, "seed", "Philox seed for source=uniform (default 1)", ParamKind::U64, false);
    return s;
}

CommandResult impl_tt(const CommandArgs& args)
{
    crd::u64 shape[ht::kMaxRank];
    crd::u32 rank = 0;
    if (!read_shape(args.get_i64_array("shape"), shape, rank, 2U))
    {
        return error_result(args.alloc, "tensor.tt: shape must be rank 2..8 with positive dims (capped)");
    }
    const crd::f64 tol = args.get_f64("tol").value_or(1e-8);
    if (tol < 0.0)
    {
        return error_result(args.alloc, "tensor.tt: tol must be >= 0");
    }
    const crd::i64 max_rank_i = args.get_i64("max_rank").value_or(0);
    if (max_rank_i < 0)
    {
        return error_result(args.alloc, "tensor.tt: max_rank must be >= 0");
    }
    ht::Tensor<crd::f64> dense(args.alloc);
    if (dense.resize({shape, rank}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.tt: allocation failed");
    }
    const auto source = args.get_string("source");
    if (source == "uniform")
    {
        (void)ht::philox_fill_uniform<crd::f64>(dense.view(), args.get_u64("seed").value_or(1U), 0U);
    }
    else // hilbert: A[idx] = 1/(1 + sum(idx)) — the smooth low-TT-rank standard
    {
        const crd::u32 nd = rank;
        dense.view().for_each(
            [nd](const crd::u64* idx, crd::f64& v)
            {
                crd::u64 t = 0;
                for (crd::u32 d = 0; d < nd; ++d)
                {
                    t += idx[d];
                }
                v = 1.0 / static_cast<crd::f64>(1U + t);
            });
    }
    ht::TtTensor<crd::f64> tt(args.alloc);
    const auto t0 = tick();
    ht::TensorStatus st = ht::tt_svd<crd::f64>(args.alloc, ht::TensorView<const crd::f64>(dense.view()), tol,
                                               static_cast<crd::u64>(max_rank_i), tt);
    const crd::f64 ms = tock_ms(t0);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.tt: tt_svd failed", st);
    }
    ht::Tensor<crd::f64> rec(args.alloc);
    st = ht::tt_contract<crd::f64>(args.alloc, tt, rec);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.tt: tt_contract failed", st);
    }
    crd::f64 num = 0.0;
    crd::f64 den = 0.0;
    for (crd::u64 i = 0; i < dense.size(); ++i)
    {
        const crd::f64 d = dense.data()[i] - rec.data()[i];
        num += d * d;
        den += dense.data()[i] * dense.data()[i];
    }
    const crd::f64 rel_err = std::sqrt(num / (den > 0.0 ? den : 1.0));
    crd::u64 core_elems = 0;
    for (crd::u32 k = 0; k < tt.dims(); ++k)
    {
        core_elems += tt.rank(k) * tt.shape(k) * tt.rank(k + 1U);
    }
    crd::containers::String s{args.alloc};
    s.append("shape=");
    append_shape(s, shape, rank);
    s.append(" ranks=");
    for (crd::u32 k = 0; k <= tt.dims(); ++k)
    {
        if (k != 0U)
        {
            s.append(",");
        }
        append_u64(s, tt.rank(k));
    }
    s.append(" core_elems=");
    append_u64(s, core_elems);
    s.append(" compression=");
    append_f64(s, static_cast<crd::f64>(dense.size()) / static_cast<crd::f64>(core_elems));
    s.append(" rel_err=");
    append_f64(s, rel_err);
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.io.info
// =======================================================================

CommandSchema make_io_info_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.io.info",
                                  "Inspect a tensor container file by extension: .npy (single array), .npz "
                                  "(zip of arrays), .safetensors (named tensors). Out = one 'name dtype shape' "
                                  "line per entry.",
                                  /*fs_read=*/true);
    add_param(s, alloc, "path", "file path (.npy | .npz | .safetensors)", ParamKind::Path, true);
    return s;
}

bool ends_with(crd::containers::StringView v, const char* suffix)
{
    crd::usize n = 0;
    while (suffix[n] != '\0')
    {
        ++n;
    }
    return v.size() >= n && v.substr(v.size() - n) == suffix;
}

CommandResult impl_io_info(const CommandArgs& args)
{
    const auto path = args.get_string("path");
    if (path.empty())
    {
        return error_result(args.alloc, "tensor.io.info: path is required");
    }
    crd::containers::Array<crd::u8> bytes(args.alloc);
    const auto t0 = tick();
    ht::TensorStatus st = ht::io_read_file(path, bytes);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.io.info: file read failed", st);
    }
    crd::containers::String s{args.alloc};
    if (ends_with(path, ".npy"))
    {
        ht::NpyView v;
        st = ht::npy_parse(crd::containers::as_const_span(bytes), v);
        if (st != ht::TensorStatus::Ok)
        {
            return status_error(args.alloc, "tensor.io.info: npy parse failed", st);
        }
        s.append("kind=npy dtype=");
        s.append(ht::io_dtype_name(v.dtype));
        s.append(" shape=");
        append_shape(s, v.shape, v.rank);
    }
    else if (ends_with(path, ".npz"))
    {
        ht::NpzReader reader(args.alloc);
        st = reader.parse(crd::containers::as_const_span(bytes));
        if (st != ht::TensorStatus::Ok)
        {
            return status_error(args.alloc, "tensor.io.info: npz parse failed", st);
        }
        s.append("kind=npz entries=");
        append_u64(s, reader.count());
        for (crd::usize i = 0; i < reader.count(); ++i)
        {
            ht::NpyView v;
            st = reader.npy(i, v);
            if (st != ht::TensorStatus::Ok)
            {
                return status_error(args.alloc, "tensor.io.info: npz member parse failed", st);
            }
            s.append("\n");
            s.append(reader.name(i));
            s.append(" dtype=");
            s.append(ht::io_dtype_name(v.dtype));
            s.append(" shape=");
            append_shape(s, v.shape, v.rank);
        }
    }
    else if (ends_with(path, ".safetensors"))
    {
        ht::SafetensorsFile f(args.alloc);
        st = f.parse(crd::containers::as_const_span(bytes));
        if (st != ht::TensorStatus::Ok)
        {
            return status_error(args.alloc, "tensor.io.info: safetensors parse failed", st);
        }
        s.append("kind=safetensors entries=");
        append_u64(s, f.tensor_count());
        for (crd::usize i = 0; i < f.tensor_count(); ++i)
        {
            const ht::SafetensorsEntry& e = f.tensor(i);
            s.append("\n");
            s.append(e.name_view());
            s.append(" dtype=");
            s.append(ht::io_dtype_name(e.dtype));
            s.append(" shape=");
            append_shape(s, e.shape, e.rank);
        }
    }
    else
    {
        return error_result(args.alloc, "tensor.io.info: extension must be .npy, .npz or .safetensors");
    }
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, tock_ms(t0));
    return r;
}

// =======================================================================
// hesap.tensor.io.philox.f64
// =======================================================================

CommandSchema make_philox_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.io.philox.f64",
                                  "Deterministic Philox counter-RNG tensor fill: element k is a pure function "
                                  "of (seed, stream, k) — bit-identical at any worker count. Out = first value "
                                  "+ checksums (the reproducibility proof).");
    add_param(s, alloc, "shape", "(I64Array) tensor shape, rank 1..8", ParamKind::I64, true);
    add_param(s, alloc, "seed", "Philox key (default 1)", ParamKind::U64, false);
    add_param(s, alloc, "stream", "Philox stream (default 0)", ParamKind::U64, false);
    return s;
}

CommandResult impl_philox(const CommandArgs& args)
{
    crd::u64 shape[ht::kMaxRank];
    crd::u32 rank = 0;
    if (!read_shape(args.get_i64_array("shape"), shape, rank, 1U))
    {
        return error_result(args.alloc, "tensor.io.philox: shape must be rank 1..8 with positive dims (capped)");
    }
    const crd::u64 seed = args.get_u64("seed").value_or(1U);
    const crd::u64 stream = args.get_u64("stream").value_or(0U);
    ht::Tensor<crd::f64> x(args.alloc);
    if (x.resize({shape, rank}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.io.philox: allocation failed");
    }
    const auto t0 = tick();
    const ht::TensorStatus st = ht::philox_fill_uniform<crd::f64>(x.view(), seed, stream);
    const crd::f64 ms = tock_ms(t0);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.io.philox: fill failed", st);
    }
    crd::containers::String s{args.alloc};
    s.append("shape=");
    append_shape(s, shape, rank);
    s.append(" first=");
    append_f64(s, x.data()[0]);
    append_checksums(s, x.data(), x.size());
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

// =======================================================================
// hesap.tensor.nn.f32
// =======================================================================

CommandSchema make_nn_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s = make_schema(alloc, "hesap.tensor.nn.f32",
                                  "NN inference from a .safetensors file (the frozen tiny-MLP/CNN corpus "
                                  "graphs) on a Philox input batch: f32, Q8_0 block-32 or per-tensor int8 "
                                  "linear tiers. Allocation-free infer. Out = output shape + checksums + "
                                  "argmax of sample 0.",
                                  /*fs_read=*/true);
    add_param(s, alloc, "path", "model file (.safetensors)", ParamKind::Path, true);
    add_enum_param(s, alloc, "model", "graph builder", "mlp|cnn", true);
    add_enum_param(s, alloc, "tier", "quantization tier (default f32)", "f32|q8|i8", false);
    add_param(s, alloc, "batch", "input batch size (default 4, <= 64)", ParamKind::I64, false);
    add_param(s, alloc, "seed", "Philox seed for the input fill (default 7)", ParamKind::U64, false);
    return s;
}

CommandResult impl_nn(const CommandArgs& args)
{
    const auto path = args.get_string("path");
    const auto model = args.get_string("model");
    if (path.empty() || (model != "mlp" && model != "cnn"))
    {
        return error_result(args.alloc, "tensor.nn: path and model (mlp|cnn) are required");
    }
    ht::NnQuantTier tier = ht::NnQuantTier::F32;
    const auto tier_name = args.get_string("tier");
    if (tier_name == "q8")
    {
        tier = ht::NnQuantTier::Q8Block32;
    }
    else if (tier_name == "i8")
    {
        tier = ht::NnQuantTier::I8PerTensor;
    }
    else if (!tier_name.empty() && tier_name != "f32")
    {
        return error_result(args.alloc, "tensor.nn: tier must be f32|q8|i8");
    }
    const crd::i64 batch_i = args.get_i64("batch").value_or(4);
    if (batch_i < 1 || batch_i > 64)
    {
        return error_result(args.alloc, "tensor.nn: need 1 <= batch <= 64");
    }
    const crd::u64 batch = static_cast<crd::u64>(batch_i);
    const crd::u64 seed = args.get_u64("seed").value_or(7U);

    crd::containers::Array<crd::u8> bytes(args.alloc); // must outlive build (zero-copy payload spans)
    ht::TensorStatus st = ht::io_read_file(path, bytes);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.nn: model file read failed", st);
    }
    ht::SafetensorsFile f(args.alloc);
    st = f.parse(crd::containers::as_const_span(bytes));
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.nn: safetensors parse failed", st);
    }

    ht::NnSequential net(args.alloc);
    crd::u64 in_shape[4] = {batch, 0U, 0U, 0U};
    crd::u32 in_rank = 2;
    crd::u64 out_dim = 0;
    if (model == "mlp")
    {
        const crd::i64 w1 = f.find("fc1.weight");
        const crd::i64 w3 = f.find("fc3.weight");
        if (w1 < 0 || w3 < 0)
        {
            return error_result(args.alloc, "tensor.nn: not the frozen MLP layout (fc1/fc3 weights missing)");
        }
        in_shape[1] = f.tensor(static_cast<crd::usize>(w1)).shape[1];
        out_dim = f.tensor(static_cast<crd::usize>(w3)).shape[0];
        st = ht::build_mlp_from_safetensors(args.alloc, f, batch, tier, net);
    }
    else
    {
        const crd::i64 c1 = f.find("c1.weight");
        const crd::i64 c2 = f.find("c2.weight");
        const crd::i64 fc = f.find("fc.weight");
        if (c1 < 0 || c2 < 0 || fc < 0)
        {
            return error_result(args.alloc, "tensor.nn: not the frozen CNN layout (c1/c2/fc weights missing)");
        }
        const crd::u64 out_c2 = f.tensor(static_cast<crd::usize>(c2)).shape[0];
        const crd::u64 fc_in = f.tensor(static_cast<crd::usize>(fc)).shape[1];
        if (out_c2 == 0U || fc_in % out_c2 != 0U)
        {
            return error_result(args.alloc, "tensor.nn: cnn fc/conv shapes disagree");
        }
        const crd::u64 per = fc_in / out_c2; // (H/4)*(W/4) after the two maxpool2 stages
        crd::u64 side = 0;
        while ((side + 1U) * (side + 1U) <= per)
        {
            ++side;
        }
        if (side * side != per)
        {
            return error_result(args.alloc, "tensor.nn: cnn input is not square (unsupported by the CLI demo)");
        }
        in_rank = 4;
        in_shape[1] = f.tensor(static_cast<crd::usize>(c1)).shape[1];
        in_shape[2] = side * 4U;
        in_shape[3] = side * 4U;
        out_dim = f.tensor(static_cast<crd::usize>(fc)).shape[0];
        st = ht::build_cnn_from_safetensors(args.alloc, f, batch, tier, net);
    }
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.nn: model build failed", st);
    }

    ht::Tensor<crd::f32> x(args.alloc);
    if (x.resize({in_shape, in_rank}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.nn: input allocation failed");
    }
    (void)ht::philox_fill_uniform<crd::f32>(x.view(), seed, 0U);
    const crd::u64 out_shape[2] = {batch, out_dim};
    ht::Tensor<crd::f32> y(args.alloc);
    ht::Tensor<crd::u8> ws(args.alloc);
    const crd::u64 ws_shape[1] = {net.workspace_bytes()};
    if (y.resize({out_shape, 2U}) != ht::TensorStatus::Ok || ws.resize({ws_shape, 1U}) != ht::TensorStatus::Ok)
    {
        return error_result(args.alloc, "tensor.nn: output/workspace allocation failed");
    }
    const auto t0 = tick();
    st = net.infer(ht::TensorView<const crd::f32>(x.view()), y.view(),
                   {ws.data(), static_cast<crd::usize>(ws.size())}, 1U);
    const crd::f64 ms = tock_ms(t0);
    if (st != ht::TensorStatus::Ok)
    {
        return status_error(args.alloc, "tensor.nn: infer failed", st);
    }
    crd::u64 arg0 = 0;
    for (crd::u64 j = 1; j < out_dim; ++j)
    {
        if (y.data()[j] > y.data()[arg0])
        {
            arg0 = j;
        }
    }
    crd::containers::String s{args.alloc};
    s.append("model=");
    s.append(model);
    s.append(" tier=");
    s.append(tier_name.empty() ? crd::containers::StringView{"f32"} : tier_name);
    s.append(" ops=");
    append_u64(s, net.op_count());
    s.append(" in=");
    append_shape(s, in_shape, in_rank);
    s.append(" out=");
    append_shape(s, out_shape, 2U);
    s.append(" argmax0=");
    append_u64(s, arg0);
    append_checksums(s, y.data(), y.size());
    CommandResult r = text_result(args.alloc, std::move(s));
    add_timing(r, args.alloc, ms);
    return r;
}

} // namespace

namespace crd::hesap::tensor
{
void register_tensor_cli_anchor() noexcept {}
} // namespace crd::hesap::tensor

// Registration uses crd allocators (abort on OOM, never throw); the std bad_alloc path the check
// traces is unreachable, and the registrar ctor is noexcept (would terminate, not escape) regardless.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_einsum_schema(alloc), &impl_einsum);
        reg.register_command(make_ew_schema(alloc), &impl_ew);
        reg.register_command(make_reduce_schema(alloc), &impl_reduce);
        reg.register_command(make_permute_schema(alloc), &impl_permute);
        reg.register_command(make_batched_schema(alloc), &impl_batched);
        reg.register_command(make_hyperopt_schema(alloc), &impl_hyperopt);
        reg.register_command(make_sparse_schema(alloc), &impl_sparse);
        reg.register_command(make_decomp_schema(alloc), &impl_decomp);
        reg.register_command(make_tt_schema(alloc), &impl_tt);
        reg.register_command(make_io_info_schema(alloc), &impl_io_info);
        reg.register_command(make_philox_schema(alloc), &impl_philox);
        reg.register_command(make_nn_schema(alloc), &impl_nn);
    });
