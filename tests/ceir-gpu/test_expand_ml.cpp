// CEIR-24b-3 (device-free) — expand_ml_ops: the ceir.ml EXPANSION provider. Rewrites ml.mlp / ml.attention into the PROVEN
// CEIR-22/23 tensor vocabulary (linalg.gemm + compute.dispatch of the authored transpose/softmax/relu .ckir kernels), the module
// the EXISTING plan_tensor_pipeline consumes (ZERO new StageKinds). Proves: a verify-clean ml module (a) expands the expected
// count, (b) leaves NO ml ops (post-expansion find_ml_misuse == None), (c) is STRUCTURALLY clean (find_structure_error == None),
// and (d) PLANS through plan_tensor_pipeline (reject == None) — i.e. every emitted op is in the 22/23 plan vocab and the shapes
// wire. ⛔ device-free: a pure module rewrite; the @transpose/@softmax/@relu symbols resolve to .ckir at record time (24b-4).
// ml.mlp is FLOAT-only (the dialect requires Float+equal weights; quantized MLP is proven hand-authored at 23c). ASCII names.

#include <crd/ceir/gpu/coopvec_mlp.hpp>  // CEIR-24c-2a: the coopvec CLAIM conversion (config + transposed fp16 weights)
#include <crd/ceir/gpu/expand_ml.hpp>
#include <crd/ceir/gpu/partition_ml.hpp> // CEIR-24c-1: the §69 advertise/assign partitioner
#include <crd/ceir/gpu/tensor_pipeline.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp>
#include <crd/ceir/parse.hpp> // CEIR-24b-5: parse the authored ml .ceir asset
#include <crd/ceir/print.hpp> // CEIR-24b-5: canonical text for the anti-drift + roundtrip checks
#include <crd/ceir/type.hpp>

#include <crd/containers/array.hpp> // CEIR-24b-5: slurp the .ceir asset into a char buffer
#include <crd/math/float_convert.hpp> // CEIR-24c-2a: f32<->f16 for the coopvec conversion oracle
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream> // CEIR-24b-5: read the authored attention.ceir asset (the parse-load gate)

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;

namespace
{
struct Kit
{
    OpId decl;
    OpId mlp;
    explicit Kit(Context& ctx) : decl(ctx.intern_op("resource", "declare")), mlp(ctx.intern_op("ml", "mlp"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)arith::register_arith_ops(ctx);   // the dispatch grid consts (arith.const)
        (void)compute::register_compute_ops(ctx); // the transpose/softmax/relu dispatches
        (void)linalg::register_dialect(ctx);     // the expanded gemms
        (void)ml::register_dialect(ctx);
    }
};
TypeId sh2(Context& ctx, u32 a, u32 b)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(b)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId tf(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_f32(), shape); }

Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
Value* decl(Context& ctx, const Kit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
// ml.mlp with N weights (the variadic tail; build via create_operation + the `activation` attr).
Operation* mlp(Context& ctx, const Kit& k, Block* b, Value* input, ConstSpan<Value*> weights, TypeId result)
{
    Value* ops[8] = {};
    ops[0] = input;
    for (u32 i = 0; i < weights.size(); ++i) { ops[1U + i] = weights[i]; }
    Operation* const op = ctx.create_operation(k.mlp, ConstSpan<Value*>(ops, 1U + weights.size()), 1U, result, 0U);
    ctx.set_attr(op, "activation", ctx.attr_string(StringView("relu")));
    b->append(op);
    return op;
}
// The CEIR-24b-5 authored asset's SOURCE builder (the anti-drift oracle for assets/ceir/attention.ceir): a single-head SDPA
// ml.attention(Q[2,4], K[3,4], V[3,2]) -> out[2,2] (the 24b proof dims Sq=2, Sk=3, D=4, Dv=2). If this changes, regenerate the file.
void build_attention_asset(Context& ctx, const Kit& k, Block* b)
{
    Value* const q  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
    Value* const ky = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
    Value* const v  = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(ml::build_attention(ctx, q, ky, v, tf(ctx, sh2(ctx, 2U, 2U))));
}
} // namespace

TEST_CASE("ceir 24b-3: expand_ml_ops rewrites a 2-layer ml.mlp into gemm/relu that plans clean", "[ceir][ml][expand]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // x[4,8] · W_1[8,8] · relu · W_2[8,4] -> y[4,4] (h1[4,8] = M·hidden = 32, relu.ckir's baked local_size).
    Value* const x  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const w1 = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const w2 = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 4U)));
    Value* const ws[2] = {w1, w2};
    (void)mlp(ctx, k, b, x, ConstSpan<Value*>(ws, 2U), tf(ctx, sh2(ctx, 4U, 4U)));

    REQUIRE(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None); // verify-clean precondition

    const gpu::MlExpandResult r = gpu::expand_ml_ops(ctx, *m);
    CHECK(r.error == gpu::MlExpandError::None);
    CHECK(r.expanded == 1U);

    CHECK(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None); // no ml ops remain
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    CHECK(plan.reject == gpu::PlanReject::None); // every emitted op is in the 22/23 plan vocab + the shapes wire
}

TEST_CASE("ceir 24b-3: expand_ml_ops rewrites ml.attention into transpose/gemm/softmax/gemm that plans clean",
          "[ceir][ml][expand]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // Q[2,4] · K[3,4] · V[3,2] -> out[2,2] (Sq=2, Sk=3, D=4, Dv=2 -- the 24b proof dims).
    Value* const q  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
    Value* const ky = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
    Value* const v  = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(ml::build_attention(ctx, q, ky, v, tf(ctx, sh2(ctx, 2U, 2U))));

    REQUIRE(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None);

    const gpu::MlExpandResult r = gpu::expand_ml_ops(ctx, *m);
    CHECK(r.error == gpu::MlExpandError::None);
    CHECK(r.expanded == 1U);

    CHECK(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None);
    CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
    CHECK(plan.reject == gpu::PlanReject::None);
}

TEST_CASE("ceir 24b-3: expand_ml_ops is idempotent on an ml-free module (expands 0)", "[ceir][ml][expand]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);
    (void)decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U))); // a lone resource.declare, no ml op

    const gpu::MlExpandResult r = gpu::expand_ml_ops(ctx, *m);
    CHECK(r.error == gpu::MlExpandError::None);
    CHECK(r.expanded == 0U);
}

// CEIR-24b-5 — the "everything is an authorable asset" form of the §138 ML proof (the 23c-b quant_mlp.ceir mold): the authored
// assets/ceir/attention.ceir (single-head SDPA) parse-loads, verifies clean (find_ml_misuse None), MATCHES the C++ builder
// (anti-drift: build_attention_asset IS the asset's source, kept as the oracle — a .ceir has no author-then-delete path), round-
// trips (print/parse fixed point), and EXPANDS + PLANS through the CEIR-22/23 pipeline (the exact module the 24b-4 device gates run).
TEST_CASE("ceir 24b-5: the authored attention.ceir parse-loads, verifies clean, matches the builder, expands + plans",
          "[ceir][ml][expand]")
{
    // ── the C++-built reference + its canonical print (the anti-drift oracle) ──
    memory::GrowableTlsfAllocator rootb;
    Context                       ctxb(&rootb);
    const Kit                     kb(ctxb);
    Module* const                 mb = ctxb.create_module();
    Block* const                  bb = mkmain(ctxb, *mb);
    build_attention_asset(ctxb, kb, bb);
    const containers::String t_built = print(ctxb, *mb, &rootb);

    // ── parse-load the COMMITTED authored asset ──
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    std::ifstream                 fin(CRD_REPO_DIR "/assets/ceir/attention.ceir", std::ios::binary | std::ios::ate);
    REQUIRE(fin.good());
    const std::streamsize sz = fin.tellg();
    fin.seekg(0);
    containers::Array<char> src(&root);
    src.resize(static_cast<usize>(sz), '\0');
    fin.read(src.data(), sz);
    const ParseResult pr = parse(ctx, StringView(src.data(), static_cast<usize>(sz)));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ⭐ ANTI-DRIFT: the committed asset's canonical print == the builder's (regenerate the file if build_attention_asset changes).
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ── ROUNDTRIP stability: print(parse(file)) re-parses to the identical text (the printer/parser fixed point for ml.attention) ──
    Context           ctx2(&root);
    const Kit         k2(ctx2);
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

    // ── verify-clean, then EXPAND + PLAN through the 22/23 pipeline (the exact post-expansion module the 24b-4 device gates run) ──
    CHECK(ml::find_ml_misuse(ctx, *pr.module).kind == ml::MlMisuseKind::None);
    const gpu::MlExpandResult er = gpu::expand_ml_ops(ctx, *pr.module);
    CHECK(er.error == gpu::MlExpandError::None);
    CHECK(er.expanded == 1U);
    CHECK(ctx.find_structure_error(*pr.module).kind == StructureErrorKind::None);
    const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *pr.module, &root);
    CHECK(plan.reject == gpu::PlanReject::None);
}

namespace
{
// Build ml.mlp(x, W_1..W_n) {relu} into `b` with the given per-layer widths [d0, d1, ..., dn] (n = widths-1 layers). Returns the op.
Operation* mlp_widths(Context& ctx, const Kit& k, Block* b, ConstSpan<u32> widths)
{
    const u32    m_rows = 4U;
    Value* const x      = decl(ctx, k, b, tf(ctx, sh2(ctx, m_rows, widths[0])));
    Value*       ws[6]  = {};
    const u32    nw     = static_cast<u32>(widths.size()) - 1U;
    for (u32 i = 0; i < nw; ++i) { ws[i] = decl(ctx, k, b, tf(ctx, sh2(ctx, widths[i], widths[i + 1U]))); }
    return mlp(ctx, k, b, x, ConstSpan<Value*>(ws, nw), tf(ctx, sh2(ctx, m_rows, widths[nw])));
}
// A coopvec provider descriptor (the CLAIM predicate) with a caller-set availability.
gpu::MlProvider coopvec(bool available)
{
    gpu::MlProvider p;
    p.name      = StringView("coopvec");
    p.available = available;
    p.advertise = &gpu::coopvec_can_claim_mlp;
    return p;
}
} // namespace

TEST_CASE("ceir 24c-1: partition_ml assigns a coopvec-claimable ml.mlp to the provider, ml.attention to the CKIR fallback",
          "[ceir][ml][partition]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // one module, both ops: ml.mlp (x[4,8]·W1[8,8]·W2[8,2], relu — coopvec-claimable) + ml.attention (no native kernel).
    const u32 widths[3] = {8U, 8U, 2U};
    (void)mlp_widths(ctx, k, b, ConstSpan<u32>(widths, 3U));
    Value* const q  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
    Value* const ky = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
    Value* const v  = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(ml::build_attention(ctx, q, ky, v, tf(ctx, sh2(ctx, 2U, 2U))));

    // caps ON: coopvec available -> claims the mlp; attention falls back (the real can't-claim case — no native attention kernel).
    const gpu::MlProvider provs_on[1] = {coopvec(true)};
    const gpu::MlPartition p_on        = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs_on, 1U), &root);
    REQUIRE(p_on.assignments.size() == 2U);
    CHECK(p_on.assignments[0].provider == 0);  // the mlp -> coopvec
    CHECK(p_on.assignments[1].provider == -1); // the attention -> CKIR fallback
    CHECK(p_on.claimed_by(0) == 1U);
    CHECK(p_on.fallback() == 1U);

    // caps OFF: coopvec unavailable (a device without cooperative_vector) -> EVERYTHING falls back (the negative is device-free).
    const gpu::MlProvider provs_off[1] = {coopvec(false)};
    const gpu::MlPartition p_off        = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs_off, 1U), &root);
    CHECK(p_off.fallback() == 2U);
    CHECK(p_off.claimed_by(0) == 0U);
}

TEST_CASE("ceir 24c-1: coopvec_can_claim_mlp checks FULL semantic attrs (relu, uniform hidden, arity, element)", "[ceir][ml][partition]")
{
    const auto claims = [](auto build) {
        memory::GrowableTlsfAllocator root;
        Context                       ctx(&root);
        const Kit                     k(ctx);
        Module* const                 m = ctx.create_module();
        Block* const                  b = mkmain(ctx, *m);
        Operation* const              op = build(ctx, k, b);
        return gpu::coopvec_can_claim_mlp(ctx, op);
    };

    // CLAIMABLE: a 2-layer relu MLP with uniform hidden width (in=8, hidden=8, out=2).
    CHECK(claims([](Context& ctx, const Kit& k, Block* b) {
        const u32 w[3] = {8U, 8U, 2U};
        return mlp_widths(ctx, k, b, ConstSpan<u32>(w, 3U));
    }));
    // CLAIMABLE: a 3-layer relu MLP, still uniform hidden (8->8->8->2).
    CHECK(claims([](Context& ctx, const Kit& k, Block* b) {
        const u32 w[4] = {8U, 8U, 8U, 2U};
        return mlp_widths(ctx, k, b, ConstSpan<u32>(w, 4U));
    }));
    // NOT claimable: a NON-uniform hidden width (8->8->4->2) — coopvec has a single `hidden`.
    CHECK(!claims([](Context& ctx, const Kit& k, Block* b) {
        const u32 w[4] = {8U, 8U, 4U, 2U};
        return mlp_widths(ctx, k, b, ConstSpan<u32>(w, 4U));
    }));
    // NOT claimable: a 1-weight linear MLP (no hidden layer — coopvec needs >=1 hidden).
    CHECK(!claims([](Context& ctx, const Kit& k, Block* b) {
        const u32 w[2] = {8U, 2U};
        return mlp_widths(ctx, k, b, ConstSpan<u32>(w, 2U));
    }));
    // NOT claimable: a non-relu activation (gelu) — the relu-only coopvec kernel would be a silent wrong-function.
    CHECK(!claims([](Context& ctx, const Kit& k, Block* b) {
        Value* const x    = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 8U)));
        Value* const w1   = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 8U)));
        Value* const w2   = decl(ctx, k, b, tf(ctx, sh2(ctx, 8U, 2U)));
        Value*       ops[3] = {x, w1, w2};
        Operation* const op = ctx.create_operation(k.mlp, ConstSpan<Value*>(ops, 3U), 1U, tf(ctx, sh2(ctx, 4U, 2U)), 0U);
        ctx.set_attr(op, "activation", ctx.attr_string(StringView("gelu")));
        b->append(op);
        return op;
    }));
    // NOT claimable: ml.attention (no native attention kernel).
    CHECK(!claims([](Context& ctx, const Kit& k, Block* b) {
        Value* const q  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value* const ky = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 4U)));
        Value* const v  = decl(ctx, k, b, tf(ctx, sh2(ctx, 3U, 2U)));
        Operation* const op = ml::build_attention(ctx, q, ky, v, tf(ctx, sh2(ctx, 2U, 2U)));
        b->append(op);
        return op;
    }));
}

TEST_CASE("ceir 24c-1: apply_partition expands ONLY the fallback ops (caps off = all fallback -> plans; caps on = claimed mlp survives)",
          "[ceir][ml][partition]")
{
    // caps OFF: mlp + attention both fall back -> apply_partition expands BOTH -> the module plans clean (the 24b path).
    {
        memory::GrowableTlsfAllocator root;
        Context                       ctx(&root);
        const Kit                     k(ctx);
        Module* const                 m = ctx.create_module();
        Block* const                  b = mkmain(ctx, *m);
        const u32                     w[3] = {8U, 8U, 2U};
        (void)mlp_widths(ctx, k, b, ConstSpan<u32>(w, 3U));
        const gpu::MlProvider provs[1] = {coopvec(false)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        const gpu::MlExpandResult er    = gpu::apply_partition(ctx, *m, part);
        CHECK(er.error == gpu::MlExpandError::None);
        CHECK(er.expanded == 1U); // the single (fallback) mlp expanded
        CHECK(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None); // no ml ops remain
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline(ctx, *m, &root);
        CHECK(plan.reject == gpu::PlanReject::None);
    }
    // caps ON: the mlp is CLAIMED -> apply_partition leaves it in place (0 expanded); the ml.mlp op survives for native dispatch.
    {
        memory::GrowableTlsfAllocator root;
        Context                       ctx(&root);
        const Kit                     k(ctx);
        Module* const                 m = ctx.create_module();
        Block* const                  b = mkmain(ctx, *m);
        const u32                     w[3] = {8U, 8U, 2U};
        (void)mlp_widths(ctx, k, b, ConstSpan<u32>(w, 3U));
        const gpu::MlProvider provs[1] = {coopvec(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        const gpu::MlExpandResult er    = gpu::apply_partition(ctx, *m, part);
        CHECK(er.error == gpu::MlExpandError::None);
        CHECK(er.expanded == 0U);                                          // the claimed mlp was NOT expanded
        CHECK(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None); // the ml.mlp op is still present + well-formed
    }
}

TEST_CASE("ceir 24c-2a: the coopvec CLAIM conversion (config + TRANSPOSED fp16 weights) == the f32 MLP oracle", "[ceir][ml][coopvec]")
{
    namespace nn = crd::kir::neural;
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    // ml.mlp x[4,8] · W1[8,8] · relu · W2[8,2] -> y[4,2].
    constexpr u32    mm      = 4U;
    constexpr u32    d0      = 8U;
    constexpr u32    d1      = 8U;
    constexpr u32    d2      = 2U;
    const u32        w[3]    = {d0, d1, d2};
    Operation* const op      = mlp_widths(ctx, k, b, ConstSpan<u32>(w, 3U));

    // (1) config from the op TYPES.
    const nn::CoopVecMlpConfig cfg = gpu::coopvec_config_from_mlp(ctx, op);
    REQUIRE(cfg.valid());
    CHECK(cfg.in_dim == 8);
    CHECK(cfg.hidden == 8);
    CHECK(cfg.out_dim == 2);
    CHECK(cfg.hidden_layers == 1);

    // (2) ⭐ ASYMMETRIC f32 weights (a symmetric matrix would let a MISSED TRANSPOSE pass). W1[in=8, out=8], W2[in=8, out=2].
    float w1[d0 * d1];
    float w2[d1 * d2];
    for (u32 i = 0; i < d0; ++i)
    {
        for (u32 j = 0; j < d1; ++j) { w1[i * d1 + j] = 0.05F * static_cast<float>(i + 1U) - 0.031F * static_cast<float>(j + 1U); }
    }
    for (u32 i = 0; i < d1; ++i)
    {
        for (u32 j = 0; j < d2; ++j) { w2[i * d2 + j] = 0.1F * static_cast<float>((i % 3U) + 1U) - 0.043F * static_cast<float>(j + 1U); }
    }
    const float* wptrs[2] = {&w1[0], &w2[0]};

    // convert -> coopvec concatenated fp16 weights (TRANSPOSED per layer); zero bias (the ml.mlp dialect has no bias).
    crd::u16 wf16[8 * 8 + 2 * 8];
    REQUIRE(gpu::coopvec_weights_from_mlp(cfg, wptrs, wf16));
    crd::u16 bf16[8 * 1 + 2] = {}; // hidden*hidden_layers + out_dim, all zero
    for (int i = 0; i < cfg.bias_count(); ++i) { bf16[i] = crd::math::f32_to_f16_bits(0.0F); }

    // inputs: 4 samples of 8 (some negatives so relu bites) -> fp16.
    float    x_f32[mm * d0];
    crd::u16 x_f16[mm * d0];
    for (u32 s = 0; s < mm; ++s)
    {
        for (u32 c = 0; c < d0; ++c)
        {
            x_f32[s * d0 + c] = 0.2F * static_cast<float>(static_cast<int>(s) - 1) + 0.06F * static_cast<float>(static_cast<int>(c) - 4);
            x_f16[s * d0 + c] = crd::math::f32_to_f16_bits(x_f32[s * d0 + c]);
        }
    }

    // (3) coopvec CPU reference over the CONVERTED weights.
    crd::u16 out_f16[mm * d2];
    nn::eval_coopvec_mlp_cpu(cfg, wf16, bf16, x_f16, static_cast<int>(mm), out_f16);

    // f32 MLP oracle in ml.mlp's x·W layout, fp16-rounded to MATCH eval (round activations through fp16, round-then-relu on hidden).
    const auto r16 = [](float v) { return crd::math::f16_bits_to_f32(crd::math::f32_to_f16_bits(v)); };
    for (u32 s = 0; s < mm; ++s)
    {
        float h1[d1];
        for (u32 n = 0; n < d1; ++n)
        {
            float acc = 0.0F;
            for (u32 c = 0; c < d0; ++c) { acc += r16(x_f32[s * d0 + c]) * r16(w1[c * d1 + n]); }
            float v = r16(acc);
            h1[n]   = v < 0.0F ? 0.0F : v; // fp16 store THEN relu (hidden), matching eval_coopvec_mlp_cpu
        }
        for (u32 o = 0; o < d2; ++o)
        {
            float acc = 0.0F;
            for (u32 n = 0; n < d1; ++n) { acc += h1[n] * r16(w2[n * d2 + o]); }
            const float ref = r16(acc); // linear output
            const float got = crd::math::f16_bits_to_f32(out_f16[s * d2 + o]);
            CHECK(crd::math::abs(got - ref) <= 5e-3F * (1.0F + crd::math::abs(ref))); // TRANSPOSE + fp16 conversion correct
        }
    }
}

TEST_CASE("ceir 24z: expand_ml_ops TYPED-REJECTS a baked-kernel shape mismatch (never a silent OOB/wrong-shape kernel)",
          "[ceir][ml][expand]")
{
    // ml.mlp whose relu'd intermediate h1 = M·hidden = 4·16 = 64 != 32 (relu.ckir's baked local_size) -> a TYPED reject.
    {
        memory::GrowableTlsfAllocator root;
        Context                       ctx(&root);
        const Kit                     k(ctx);
        Module* const                 m = ctx.create_module();
        Block* const                  b = mkmain(ctx, *m);
        const u32                     w[3] = {8U, 16U, 4U}; // h1 = 4·16 = 64, not 32
        (void)mlp_widths(ctx, k, b, ConstSpan<u32>(w, 3U));
        REQUIRE(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None); // verify-clean, but the baked kernel can't take it
        const gpu::MlExpandResult r = gpu::expand_ml_ops(ctx, *m);
        CHECK(r.error == gpu::MlExpandError::BakedKernelShapeUnsupported);
        CHECK(r.expanded == 0U); // rejected BEFORE emitting any ops
    }
    // ml.attention with Sk=4 (transpose.ckir/softmax.ckir bake Sk=3) -> a TYPED reject.
    {
        memory::GrowableTlsfAllocator root;
        Context                       ctx(&root);
        const Kit                     k(ctx);
        Module* const                 m = ctx.create_module();
        Block* const                  b = mkmain(ctx, *m);
        Value* const                  q  = decl(ctx, k, b, tf(ctx, sh2(ctx, 2U, 4U)));
        Value* const                  ky = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 4U))); // Sk=4, not 3
        Value* const                  v  = decl(ctx, k, b, tf(ctx, sh2(ctx, 4U, 2U)));
        b->append(ml::build_attention(ctx, q, ky, v, tf(ctx, sh2(ctx, 2U, 2U))));
        REQUIRE(ml::find_ml_misuse(ctx, *m).kind == ml::MlMisuseKind::None);
        const gpu::MlExpandResult r = gpu::expand_ml_ops(ctx, *m);
        CHECK(r.error == gpu::MlExpandError::BakedKernelShapeUnsupported);
        CHECK(r.expanded == 0U);
    }
}
