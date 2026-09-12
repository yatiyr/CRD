// CEIR-25a-1 (device-free) — the reverse-mode VJP core: the VjpRegistry (op-kind U kernel-symbol) + the linalg.gemm VJP
// rule (vjp_gemm) + the MissingVjp typed reject. Proves:
//  (STRUCTURAL) vjp_gemm on C=A*B [3,2]x[2,4] emits exactly 2 tensor.transpose (perm 1,0) + 2 plain linalg.gemm, with
//    dA typed [M,K] / dB typed [K,N], operand_adjoints[2] (beta*C) == null, and the module verify-clean
//    (find_tensor_misuse / find_linalg_misuse / find_structure_error all None);
//  (PARTIAL NUMERIC, honest label) synth_gemm -> eval_cpu on each emitted gemm (its transpose operand fed the CPU
//    transpose) == hesap nn_reverse::matmul_vjp within an f32-accumulation tol -- proving the emitted gemm OPERANDS +
//    ORDER are right (dA=dC*Bt, dB=At*dC). FULL numeric (running the emitted transpose on device) closes at 25b;
//  (NEGATIVE) lookup -> nullptr (MissingVjp) for an unregistered op, a compute.dispatch with no registered kernel, and
//    a compute.dispatch with no kernel attr (the kernel-map-only, no-generic-dispatch-VJP contract).

#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/gpu/grad.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>   // CEIR-25c-1: arith.const — the vjp_mlp dispatch grid operand
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp> // CEIR-25c-1: ml.mlp op + register_dialect + find_ml_misuse (the vjp_mlp gate)
#include <crd/ceir/tensor.hpp>
#include <crd/ceir/type.hpp>

#include <crd/hesap/autodiff/nn_reverse.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
struct Kit
{
    OpId decl;
    explicit Kit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)compute::register_compute_ops(ctx); // the compute.dispatch of the MissingVjp negatives
        (void)linalg::register_dialect(ctx);      // linalg.gemm (forward + the emitted VJP gemms)
        (void)tensor::register_dialect(ctx);      // the emitted tensor.transpose
    }
};
Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
TypeId sh2(Context& ctx, u32 a, u32 b)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(b)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId  tf(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_f32(), shape); }
Value*  decl(Context& ctx, const Kit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
Operation* mk_gemm(Context& ctx, Block* b, Value* a, Value* bb, Value* c, TypeId out)
{
    Operation* const op = linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                             ctx.attr_bool(false), ctx.attr_bool(false), out);
    b->append(op);
    return op;
}
u32 count_ops(const Context& ctx, Block* b, StringView name)
{
    u32 n = 0;
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) == name) { ++n; }
    }
    return n;
}
u32 count_all(Block* b)
{
    u32 n = 0;
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block()) { ++n; }
    return n;
}
TypeId sh1(Context& ctx, u32 a)
{
    const TypeId d[1] = {ctx.type_dim_static(a)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 1U));
}
Operation* mk_reduce(Context& ctx, Block* b, Value* src, i64 axis, StringView fn, TypeId out)
{
    Operation* const op = tensor::build_reduce(ctx, src, ctx.attr_int(axis), ctx.attr_string(fn), out);
    b->append(op);
    return op;
}
} // namespace

TEST_CASE("ceir 25a-1: vjp_gemm emits 2 transpose + 2 gemm (right shapes/wiring), verify-clean, and matches matmul_vjp",
          "[ceir][autodiff]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    constexpr u32 mm = 3U;
    constexpr u32 kk = 2U;
    constexpr u32 nn = 4U;

    // forward: C = A[3,2] * B[2,4] -> [3,4].
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, mm, kk)));
    Value* const     bb = decl(ctx, k, b, tf(ctx, sh2(ctx, kk, nn)));
    Value* const     c  = decl(ctx, k, b, tf(ctx, sh2(ctx, mm, nn)));
    Operation* const g  = mk_gemm(ctx, b, a, bb, c, tf(ctx, sh2(ctx, mm, nn)));
    Value* const     dc = decl(ctx, k, b, tf(ctx, sh2(ctx, mm, nn))); // the output adjoint dC[3,4]
    Operation* const at = ctx.create_operation(k.decl, {}, 1U, tf(ctx, sh2(ctx, 1U, 1U))); // insertion sentinel
    b->append(at);

    gpu::VjpRegistry reg(&root);
    gpu::register_builtin_vjps(reg, ctx);

    Value*      oa[3]  = {nullptr, nullptr, nullptr};
    Value*      rad[1] = {dc};
    const gpu::GradError e = gpu::vjp_gemm(ctx, reg, b, at, g, ConstSpan<Value*>(rad, 1U), oa);
    REQUIRE(e == gpu::GradError::None);

    // ── STRUCTURAL ──
    REQUIRE(oa[0] != nullptr);
    REQUIRE(oa[1] != nullptr);
    CHECK(oa[2] == nullptr);               // the beta*C term is not differentiated
    CHECK(oa[0]->type() == a->type());     // dA : [M,K]
    CHECK(oa[1]->type() == bb->type());    // dB : [K,N]

    Operation* const da_g = oa[0]->defining_op(); // dA = gemm(dC, Bt)
    REQUIRE(da_g != nullptr);
    CHECK(ctx.op_name(da_g->kind()) == StringView("linalg.gemm"));
    CHECK(da_g->operand(0U) == dc);
    Operation* const bt = da_g->operand(1U)->defining_op(); // transpose(B)
    REQUIRE(bt != nullptr);
    CHECK(ctx.op_name(bt->kind()) == StringView("tensor.transpose"));
    CHECK(bt->operand(0U) == bb);

    Operation* const db_g = oa[1]->defining_op(); // dB = gemm(At, dC)
    REQUIRE(db_g != nullptr);
    CHECK(ctx.op_name(db_g->kind()) == StringView("linalg.gemm"));
    CHECK(db_g->operand(1U) == dc);
    Operation* const atr = db_g->operand(0U)->defining_op(); // transpose(A)
    REQUIRE(atr != nullptr);
    CHECK(ctx.op_name(atr->kind()) == StringView("tensor.transpose"));
    CHECK(atr->operand(0U) == a);

    CHECK(count_ops(ctx, b, StringView("tensor.transpose")) == 2U);
    CHECK(count_ops(ctx, b, StringView("linalg.gemm")) == 3U); // 1 forward + 2 backward

    // verify-clean (every emitted op is well-formed in its dialect + structurally sound).
    CHECK(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
    CHECK(linalg::find_linalg_misuse(ctx, *m).kind == linalg::LinalgMisuseKind::None);
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);

    // ── PARTIAL NUMERIC ── the emitted gemm operands compute dA=dC*Bt, dB=At*dC == nn_reverse::matmul_vjp. The transpose
    // operand is fed its CPU transpose (running the emitted transpose on device is 25b). eval_cpu rounds per-op to F32.
    namespace nnr = crd::hesap::autodiff::reverse::nn;
    f64 av[mm * kk];
    f64 bv[kk * nn];
    f64 dcv[mm * nn];
    for (u32 i = 0; i < mm * kk; ++i) { av[i] = 0.5 + 0.13 * static_cast<f64>(i) - 0.017 * static_cast<f64>(i * i); }
    for (u32 i = 0; i < kk * nn; ++i) { bv[i] = -0.4 + 0.21 * static_cast<f64>(i); }
    for (u32 i = 0; i < mm * nn; ++i) { dcv[i] = 0.3 - 0.05 * static_cast<f64>(i) + 0.02 * static_cast<f64>((i * 7U) % 5U); }

    f64 ga[mm * kk];
    f64 gb[kk * nn];
    nnr::matmul_vjp(av, bv, dcv, ga, gb, static_cast<int>(mm), static_cast<int>(kk), static_cast<int>(nn));

    f64 bt_cpu[nn * kk]; // B[K,N] -> Bt[N,K]
    for (u32 r = 0; r < kk; ++r)
    {
        for (u32 col = 0; col < nn; ++col) { bt_cpu[col * kk + r] = bv[r * nn + col]; }
    }
    f64 at_cpu[kk * mm]; // A[M,K] -> At[K,M]
    for (u32 r = 0; r < mm; ++r)
    {
        for (u32 col = 0; col < kk; ++col) { at_cpu[col * mm + r] = av[r * kk + col]; }
    }

    constexpr f64 tol = 1e-6; // covers f32(eval_cpu)-vs-f64(oracle) accumulation; the assertion's real job is operand ORDER
    {
        kir::KGraph           gda(&root);
        const gpu::GraphSynth s = gpu::synth_gemm(ctx, *da_g, gda); // gemm(dC[M,N], Bt[N,K]) -> [M,K]
        REQUIRE(s.reject == gpu::SynthReject::None);
        const f64* ins[2] = {dcv, bt_cpu};
        f64        out[mm * kk];
        kir::eval_cpu(gda, ins, &root, s.output, out);
        for (u32 i = 0; i < mm * kk; ++i) { CHECK(crd::math::abs(out[i] - ga[i]) <= tol * (1.0 + crd::math::abs(ga[i]))); }
    }
    {
        kir::KGraph           gdb(&root);
        const gpu::GraphSynth s = gpu::synth_gemm(ctx, *db_g, gdb); // gemm(At[K,M], dC[M,N]) -> [K,N]
        REQUIRE(s.reject == gpu::SynthReject::None);
        const f64* ins[2] = {at_cpu, dcv};
        f64        out[kk * nn];
        kir::eval_cpu(gdb, ins, &root, s.output, out);
        for (u32 i = 0; i < kk * nn; ++i) { CHECK(crd::math::abs(out[i] - gb[i]) <= tol * (1.0 + crd::math::abs(gb[i]))); }
    }
}

TEST_CASE("ceir 25a-1: lookup -> MissingVjp for unregistered op / dispatch w/o registered kernel / dispatch w/o kernel attr",
          "[ceir][autodiff]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    gpu::VjpRegistry reg(&root);
    gpu::register_builtin_vjps(reg, ctx); // registers ONLY linalg.gemm

    // registered: a linalg.gemm resolves to vjp_gemm.
    Value* const     a  = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 2U)));
    Value* const     bb = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
    Value* const     c  = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
    Operation* const g  = mk_gemm(ctx, b, a, bb, c, tf(ctx, sh2(ctx, 3U, 4U)));
    CHECK(reg.lookup(ctx, g) == &gpu::vjp_gemm);

    // unregistered op: a bare resource.declare -> nullptr (MissingVjp).
    CHECK(reg.lookup(ctx, a->defining_op()) == nullptr);

    // compute.dispatch with an UNREGISTERED kernel symbol -> nullptr (resolves only through the kernel map).
    Operation* const d1 = ctx.create_operation(ctx.intern_op("compute", "dispatch"), {}, 0U);
    ctx.set_attr(d1, StringView("kernel"), ctx.attr_symbol(StringView("relu")));
    b->append(d1);
    CHECK(reg.lookup(ctx, d1) == nullptr);

    // compute.dispatch with NO kernel attr -> nullptr (never a fall-through to an op-kind rule).
    Operation* const d2 = ctx.create_operation(ctx.intern_op("compute", "dispatch"), {}, 0U);
    b->append(d2);
    CHECK(reg.lookup(ctx, d2) == nullptr);

    // once a kernel VJP is registered, the SAME dispatch resolves (the asset-driven differentiable-pair contract).
    reg.register_kernel(StringView("relu"), &gpu::vjp_gemm); // any non-null fn; the resolution is what's under test
    CHECK(reg.lookup(ctx, d1) != nullptr);
    CHECK(reg.lookup(ctx, d2) == nullptr); // still no kernel attr
}

TEST_CASE("ceir 25a-2: build_gradient differentiates gemm(A,A)->reduce(sum) - backward graph + accumulation + numeric",
          "[ceir][autodiff]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    constexpr u32    nn   = 3U; // A is SQUARE so it is BOTH gemm operands -> exercises (same-shape) adjoint accumulation
    Value* const     a    = decl(ctx, k, b, tf(ctx, sh2(ctx, nn, nn)));
    Value* const     carg = decl(ctx, k, b, tf(ctx, sh2(ctx, nn, nn)));               // the gemm's beta*C operand (beta=0)
    Operation* const g    = mk_gemm(ctx, b, a, a, carg, tf(ctx, sh2(ctx, nn, nn)));   // C = A*A
    Operation* const rd   = mk_reduce(ctx, b, g->result(0U), 1, StringView("sum"), tf(ctx, sh1(ctx, nn))); // loss[N] = row sums
    Value* const     loss = rd->result(0U);

    gpu::VjpRegistry reg(&root);
    gpu::register_builtin_vjps(reg, ctx);

    Value*                wrt[1]   = {a};
    Value*                grads[1] = {nullptr};
    const gpu::GradResult res =
        gpu::build_gradient(ctx, *m, reg, loss, ConstSpan<Value*>(wrt, 1U), containers::Span<Value*>(grads, 1U), &root);
    REQUIRE(res.error == gpu::GradError::None);
    REQUIRE(grads[0] != nullptr);
    CHECK(grads[0]->type() == a->type()); // dA : [N,N]
    REQUIRE(res.seed != nullptr);          // the dLoss seed handle rides out (the header's caller-seed promise, now WITH a handle)
    CHECK(res.seed->type() == loss->type()); // seeded at loss's shape [N] (an executor uploads ALL-ONES into this buffer)

    // ── STRUCTURAL: grads[0] = elementwise{add}(dA0, dA1), each a backward linalg.gemm. ──
    Operation* const add = grads[0]->defining_op();
    REQUIRE(add != nullptr);
    CHECK(ctx.op_name(add->kind()) == StringView("tensor.elementwise"));
    Operation* const da0 = add->operand(0U)->defining_op();
    Operation* const da1 = add->operand(1U)->defining_op();
    REQUIRE(da0 != nullptr);
    REQUIRE(da1 != nullptr);
    CHECK(ctx.op_name(da0->kind()) == StringView("linalg.gemm"));
    CHECK(ctx.op_name(da1->kind()) == StringView("linalg.gemm"));

    // the emitted backward graph exactly (anchor GONE): reduce VJP (reshape+broadcast) + gemm VJP (2 transpose + 2 gemm) +
    // 1 accumulation elementwise; forward adds 1 gemm + 1 reduce; declares = A + Carg + seed + 2 gemm-c = 5 (NOT 6).
    CHECK(count_ops(ctx, b, StringView("tensor.reshape")) == 1U);
    CHECK(count_ops(ctx, b, StringView("tensor.broadcast")) == 1U);
    CHECK(count_ops(ctx, b, StringView("tensor.transpose")) == 2U);
    CHECK(count_ops(ctx, b, StringView("linalg.gemm")) == 3U);
    CHECK(count_ops(ctx, b, StringView("tensor.reduce")) == 1U);
    CHECK(count_ops(ctx, b, StringView("tensor.elementwise")) == 1U);
    CHECK(count_ops(ctx, b, StringView("resource.declare")) == 5U);

    CHECK(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
    CHECK(linalg::find_linalg_misuse(ctx, *m).kind == linalg::LinalgMisuseKind::None);
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);

    // ── PARTIAL NUMERIC: the two backward gemms == matmul_vjp(A,A,dC).ga/.gb; total dA = ga+gb (broadcast/add = 25b). ──
    namespace nnr = crd::hesap::autodiff::reverse::nn;
    f64 av[nn * nn];
    for (u32 i = 0; i < nn * nn; ++i) { av[i] = 0.4 + 0.17 * static_cast<f64>(i) - 0.03 * static_cast<f64>((i * 5U) % 7U); }
    f64 dc[nn * nn];
    for (u32 i = 0; i < nn * nn; ++i) { dc[i] = 1.0; } // seed all-ones -> reduce(axis=1,sum) VJP broadcasts 1 to every C[i,j]
    f64 ga[nn * nn];
    f64 gb[nn * nn];
    nnr::matmul_vjp(av, av, dc, ga, gb, static_cast<int>(nn), static_cast<int>(nn), static_cast<int>(nn));
    f64 at_cpu[nn * nn];
    for (u32 r = 0; r < nn; ++r)
    {
        for (u32 col = 0; col < nn; ++col) { at_cpu[col * nn + r] = av[r * nn + col]; }
    }
    constexpr f64 tol = 1e-6;
    {
        kir::KGraph           gg(&root);
        const gpu::GraphSynth s = gpu::synth_gemm(ctx, *da0, gg); // gemm(dC, Aᵀ) == ga
        REQUIRE(s.reject == gpu::SynthReject::None);
        const f64* ins[2] = {dc, at_cpu};
        f64        out[nn * nn];
        kir::eval_cpu(gg, ins, &root, s.output, out);
        for (u32 i = 0; i < nn * nn; ++i) { CHECK(crd::math::abs(out[i] - ga[i]) <= tol * (1.0 + crd::math::abs(ga[i]))); }
    }
    {
        kir::KGraph           gg(&root);
        const gpu::GraphSynth s = gpu::synth_gemm(ctx, *da1, gg); // gemm(Aᵀ, dC) == gb
        REQUIRE(s.reject == gpu::SynthReject::None);
        const f64* ins[2] = {at_cpu, dc};
        f64        out[nn * nn];
        kir::eval_cpu(gg, ins, &root, s.output, out);
        for (u32 i = 0; i < nn * nn; ++i) { CHECK(crd::math::abs(out[i] - gb[i]) <= tol * (1.0 + crd::math::abs(gb[i]))); }
    }
}

TEST_CASE("ceir 25a-2: build_gradient MissingVjp is a PURE reject (unregistered reduce -> error + op count UNCHANGED)",
          "[ceir][autodiff]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    constexpr u32    nn   = 3U;
    Value* const     a    = decl(ctx, k, b, tf(ctx, sh2(ctx, nn, nn)));
    Value* const     carg = decl(ctx, k, b, tf(ctx, sh2(ctx, nn, nn)));
    Operation* const g    = mk_gemm(ctx, b, a, a, carg, tf(ctx, sh2(ctx, nn, nn)));
    Operation* const rd   = mk_reduce(ctx, b, g->result(0U), 1, StringView("sum"), tf(ctx, sh1(ctx, nn)));

    gpu::VjpRegistry reg(&root);
    reg.register_op(ctx.intern_op("linalg", "gemm"), &gpu::vjp_gemm); // reduce deliberately NOT registered

    const u32             before   = count_all(b);
    Value*                wrt[1]   = {a};
    Value*                grads[1] = {nullptr};
    const gpu::GradResult res =
        gpu::build_gradient(ctx, *m, reg, rd->result(0U), ConstSpan<Value*>(wrt, 1U), containers::Span<Value*>(grads, 1U), &root);
    CHECK(res.error == gpu::GradError::MissingVjp);
    CHECK(res.error_op == rd);         // points at the un-differentiable reduce
    CHECK(count_all(b) == before);     // ⛔ PURE: the dry pre-pass emitted NOTHING before rejecting
}

TEST_CASE("ceir 25a-2: build_gradient TYPED-REJECTS a non-sum reduce VJP (reduce{max} -> ReduceFnUnsupported)",
          "[ceir][autodiff]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    constexpr u32    nn   = 3U;
    Value* const     a    = decl(ctx, k, b, tf(ctx, sh2(ctx, nn, nn)));
    Value* const     carg = decl(ctx, k, b, tf(ctx, sh2(ctx, nn, nn)));
    Operation* const g    = mk_gemm(ctx, b, a, a, carg, tf(ctx, sh2(ctx, nn, nn)));
    Operation* const rd   = mk_reduce(ctx, b, g->result(0U), 1, StringView("max"), tf(ctx, sh1(ctx, nn))); // max VJP = name-forward

    gpu::VjpRegistry reg(&root);
    gpu::register_builtin_vjps(reg, ctx); // reduce IS registered; the rule rejects fn != sum

    Value*                wrt[1]   = {a};
    Value*                grads[1] = {nullptr};
    const gpu::GradResult res =
        gpu::build_gradient(ctx, *m, reg, rd->result(0U), ConstSpan<Value*>(wrt, 1U), containers::Span<Value*>(grads, 1U), &root);
    CHECK(res.error == gpu::GradError::ReduceFnUnsupported);
    CHECK(res.error_op == rd);
}

TEST_CASE("ceir 25c-1: vjp_mlp differentiates a 2-layer ml.mlp - backward graph + recompute + dW shapes + relu_vjp reads the pre-activation",
          "[ceir][autodiff]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    (void)ml::register_dialect(ctx);       // ml.mlp + find_ml_misuse
    (void)arith::register_arith_ops(ctx);  // arith.const (the recompute/relu_vjp dispatch grid)
    Module* const m = ctx.create_module();
    Block* const  b = mkmain(ctx, *m);

    // corpus: out[8,4] = ml.mlp(x[8,4], W1[4,4], W2[4,4]){activation=relu}. x is [8,4] so the hidden relu'd intermediate is
    // 8*4==32 (relu.ckir's baked local_size). loss = out (the mlp result); seed dLoss = M (a non-uniform mask — the 25c-2 device
    // gate uploads it), so dOut = M is fully non-uniform (fixes the 25b-4b all-ones-dC scope gap). Here we gate STRUCTURE only.
    Value* const     xin = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 4U)));
    Value* const     w1  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value* const     w2  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U)));
    Value*           mops[3] = {xin, w1, w2};
    Operation* const mlp = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mops, 3U), 1U, tf(ctx, sh2(ctx, 8U, 4U)));
    ctx.set_attr(mlp, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mlp);

    gpu::VjpRegistry reg(&root);
    gpu::register_builtin_vjps(reg, ctx);
    Value*                wrt[2]   = {w1, w2};
    Value*                grads[2] = {nullptr, nullptr};
    const gpu::GradResult res =
        gpu::build_gradient(ctx, *m, reg, mlp->result(0U), ConstSpan<Value*>(wrt, 2U), containers::Span<Value*>(grads, 2U), &root);
    REQUIRE(res.error == gpu::GradError::None);
    REQUIRE(grads[0] != nullptr); // dW1
    REQUIRE(grads[1] != nullptr); // dW2

    // ── STRUCTURAL: grads are the two backward gemms; dW1==W1's type [4,4], dW2==W2's type [4,4]. ──
    CHECK(grads[0]->type() == w1->type());
    CHECK(grads[1]->type() == w2->type());
    REQUIRE(grads[0]->defining_op() != nullptr);
    REQUIRE(grads[1]->defining_op() != nullptr);
    CHECK(ctx.op_name(grads[0]->defining_op()->kind()) == StringView("linalg.gemm"));
    CHECK(ctx.op_name(grads[1]->defining_op()->kind()) == StringView("linalg.gemm"));

    // op counts: ml.mlp stays (unexpanded); recompute = 1 gemm (z1) + 1 @relu dispatch (h1); backward = 4 gemm (dh1,dW2,dx,dW1)
    // + 4 transpose (W2ᵀ,h1ᵀ,W1ᵀ,xᵀ) + 1 @relu_vjp dispatch. So gemm=5, dispatch=2, transpose=4, ml.mlp=1.
    CHECK(count_ops(ctx, b, StringView("ml.mlp")) == 1U);
    CHECK(count_ops(ctx, b, StringView("linalg.gemm")) == 5U);
    CHECK(count_ops(ctx, b, StringView("compute.dispatch")) == 2U);
    CHECK(count_ops(ctx, b, StringView("tensor.transpose")) == 4U);

    // ⛔ the recompute is the INTERIOR only: z1 = gemm(x, W1) exists; z2 (the last layer) is NOT re-derived (dOut is given).
    //    Find the recompute gemm (operand0==x, operand1==W1) = z1.
    Operation* z1 = nullptr;
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) == StringView("linalg.gemm") && op->num_operands() >= 2U && op->operand(0U) == xin
            && op->operand(1U) == w1)
        {
            z1 = op;
            break;
        }
    }
    REQUIRE(z1 != nullptr);

    // ⛔ relu_vjp reads the PRE-activation z1 (bind[0] = operand(3), after the 3 grid operands), NOT the post-relu h1 — a future
    //    refactor swapping them passes every NUMERIC gate (h1>0 <=> z1>0 makes the mask identical) but violates the contract.
    Operation* relu_vjp_disp = nullptr;
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (ctx.op_name(op->kind()) != StringView("compute.dispatch")) { continue; }
        const AttrValue kv = ctx.attr_value(op->attr(StringView("kernel")));
        if (kv.kind == AttrKind::SymbolRef && kv.s == StringView("relu_vjp")) { relu_vjp_disp = op; break; }
    }
    REQUIRE(relu_vjp_disp != nullptr);
    REQUIRE(relu_vjp_disp->num_operands() >= 4U);
    CHECK(relu_vjp_disp->operand(3U) == z1->result(0U)); // bind[0] == z1 (the pre-activation gemm result)

    // ⛔ dW1 (grads[0]) is the TERMINAL op (emitted last) so it is the plan's single-Output readback target (the 25c-2 gate reads it).
    Operation* last = b->first_op();
    for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block()) { last = op; }
    CHECK(grads[0]->defining_op() == last);

    // verify-clean (ml.mlp composite still present + the emitted backward vocab).
    CHECK(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None);
    CHECK(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
    CHECK(linalg::find_linalg_misuse(ctx, *m).kind == linalg::LinalgMisuseKind::None);
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);

    // TYPED reject: a non-relu activation ⇒ MlpActivationUnsupported (the authored relu_vjp.ckir pair is relu-only this band).
    Operation* const mlp_tanh = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mops, 3U), 1U, tf(ctx, sh2(ctx, 8U, 4U)));
    ctx.set_attr(mlp_tanh, StringView("activation"), ctx.attr_string(StringView("tanh")));
    b->append(mlp_tanh);
    Value*                grads_t[2] = {nullptr, nullptr};
    const gpu::GradResult res_t =
        gpu::build_gradient(ctx, *m, reg, mlp_tanh->result(0U), ConstSpan<Value*>(wrt, 2U), containers::Span<Value*>(grads_t, 2U), &root);
    CHECK(res_t.error == gpu::GradError::MlpActivationUnsupported);
    CHECK(res_t.error_op == mlp_tanh);

    // ⛔ CEIR-26d-4b SUPERSEDED-IN-PLACE (was: an interior relu'd layer whose M·hidden ≠ 32 ⇒ MlpBakedShapeUnsupported + ZERO
    //    backward vocab): the grad baked-shape guard is RETIRED — relu.ckir/relu_vjp.ckir now ship the shape SENTINEL (local_size=0)
    //    and the resolver cook-binds local_size to the intermediate numel, so a vjp at ANY interior width EXPANDS. x[8,8] -> interior
    //    z1 = [8,8] => M·hidden = 64 ≠ 32 (was rejected) now build_gradient succeeds + EMITS the backward vocab; proven device-
    //    resident at h1=64 on both backends by the 26d-4b gates. Here we assert None + backward vocab EMITTED (the RULE ran).
    Module* const bmod = ctx.create_module();
    Block* const  bb   = mkmain(ctx, *bmod);
    Value* const  xb   = decl(ctx, k, bb, tf(ctx, sh2(ctx, 8U, 8U)));  // M = 8
    Value* const  w1b  = decl(ctx, k, bb, tf(ctx, sh2(ctx, 8U, 8U)));  // interior hidden = 8 -> 8*8 == 64 != 32 (was rejected, now expands)
    Value* const  w2b  = decl(ctx, k, bb, tf(ctx, sh2(ctx, 8U, 4U)));
    Value*        mopsb[3] = {xb, w1b, w2b};
    Operation* const mlp_wide =
        ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(mopsb, 3U), 1U, tf(ctx, sh2(ctx, 8U, 4U)));
    ctx.set_attr(mlp_wide, StringView("activation"), ctx.attr_string(StringView("relu")));
    bb->append(mlp_wide);
    Value*                wrtb[2]    = {w1b, w2b};
    Value*                grads_b[2] = {nullptr, nullptr};
    const gpu::GradResult res_b      = gpu::build_gradient(ctx, *bmod, reg, mlp_wide->result(0U),
                                                           ConstSpan<Value*>(wrtb, 2U), containers::Span<Value*>(grads_b, 2U), &root);
    CHECK(res_b.error == gpu::GradError::None); // ⛔ 26d-4b: was MlpBakedShapeUnsupported
    CHECK(grads_b[0] != nullptr);
    CHECK(grads_b[1] != nullptr);
    // the RULE EMITTED the backward vocab (the recompute gemm/relu + backward gemms/transposes/relu_vjp) — the exact op counts are
    // owned by the 25c-1 structural gate; here we require it is NON-empty (the reject would have emitted zero).
    CHECK(count_ops(ctx, bb, StringView("linalg.gemm")) > 0U);
    CHECK(count_ops(ctx, bb, StringView("tensor.transpose")) > 0U);
    CHECK(count_ops(ctx, bb, StringView("compute.dispatch")) > 0U); // relu recompute + relu_vjp
    CHECK(count_ops(ctx, bb, StringView("ml.mlp")) == 1U);          // build_gradient leaves the composite (caller erases)
}
