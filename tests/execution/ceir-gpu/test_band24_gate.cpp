// CEIR-24z — the BAND-24 COMPOSING GATE (the test_band23_gate mold): the §55 ceir.ml dialect COMPOSES + survives both serializers,
// and the §69 partitioner handles a mixed module. ONE module carries BOTH ml ops — ml.mlp (a 3-WEIGHT MLP: a VARIADIC operand tail
// LONGER than the 2-weight proof dims, uniform hidden=8) + ml.attention (single-head SDPA). The band property: find_ml_misuse +
// find_structure_error are None on the ONE module, the module survives a TEXT round-trip (print -> parse -> re-walk None +
// print==reprint) AND a BINARY round-trip (serialize -> deserialize -> re-walk None) — ⛔ the FIRST time ml.mlp's VARIADIC tail +
// ml.attention cross the binary serializer — and the §69 partitioner ASSIGNS the mixed module (coopvec CLAIMS the mlp, the
// attention FALLS BACK — no native attention kernel). Device-free (crd-ceir + the ceir-gpu partitioner), test-only. ASCII names.
// ALSO hosts the §69 CEIR-29a partitioner-DESCRIPTOR gates (29a-*, tagged [band29]) — this file owns the mixed-module +
// coopvec fixture the partitioner tests need, so extending the partition provider (MlProvider) is tested here (DRY), not
// in a duplicate fixture. ⛔ TEST NAMES STAY ASCII (a `§` in a name mangles in the ctest→Catch2 filter round-trip → false-fail).

#include <crd/ceir/gpu/partition_ml.hpp>

#include <crd/ceir/binary.hpp>
#include <crd/ceir/gpu/expand_ml.hpp> // CEIR-29b-2b: expand_ml_ops — a launch-graph claim does NOT suppress expansion
#include <crd/ceir/gpu/tensor_pipeline.hpp> // CEIR-29c-1: plan_tensor_pipeline_partitioned — the per-stage provider tag
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/ml.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/transform.hpp> // CEIR-29a-3b: register_transform_ops + build_assign_provider + find_transform_misuse
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../../gpu/gpu-shared/ceir_asset_slurp.hpp" // CEIR-30c-2: slurp_asset — the shared committed-asset reader (hoisted from here)

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
void register_all(Context& ctx)
{
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)ml::register_dialect(ctx);
}
Value* mkval(Context& ctx, OpId decl, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId sh2(Context& ctx, u32 a, u32 b)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(b)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId tf(Context& ctx, TypeId shape) { return ctx.type_tensor(ctx.type_f32(), shape); }

// Build the composing module in `m`: a 3-weight ml.mlp (variadic tail) + a single-head ml.attention (they COEXIST).
void build_band24(Context& ctx, Module& m)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const b = func::func_body_block(f);

    // ── ml.mlp: x[4,8] · W1[8,8] · W2[8,8] · W3[8,2] {relu} -> y[4,2] (3 weights = a variadic tail > the 2-weight proof; uniform hidden=8) ──
    Value* const x   = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const w1  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const w2  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const w3  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 2U)));
    Value*       ops[4] = {x, w1, w2, w3};
    Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(ops, 4U), 1U, tf(ctx, sh2(ctx, 4U, 2U)), 0U);
    ctx.set_attr(mo, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(mo);

    // ── ml.attention: Q[2,4] · K[3,4] · V[3,2] -> out[2,2] ──
    Value* const q  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 2U, 4U)));
    Value* const ky = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 3U, 4U)));
    Value* const v  = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(ml::build_attention(ctx, q, ky, v, tf(ctx, sh2(ctx, 2U, 2U))));
}

void require_clean(Context& ctx, const Module& m)
{
    REQUIRE(ctx.find_structure_error(m).kind == StructureErrorKind::None);
    REQUIRE(ml::find_ml_misuse(ctx, m).kind == ml::MlMisuseKind::None);
}
gpu::MlProvider coopvec(bool available)
{
    gpu::MlProvider p;
    p.name           = StringView("coopvec");
    p.available      = available;
    p.advertise      = &gpu::coopvec_can_claim_mlp;
    p.provider_class = ProviderClass::Gpu;             // coopvec is a GPU-bridge fused kernel (§69)
    p.memory_domain  = StringView("device_local");     // its outputs live in device-local memory (§24 vocab)
    p.determinism    = DeterminismClass::DeterministicWithinTarget; // the fused coopvec MLP is fixed-order (§27)
    return p;
}

// CEIR-29a-1: a stub provider that ADVERTISES nothing (the real "can't claim" negative) — used to prove a SECOND provider
// row of a DISTINCT class rides the descriptor without changing the partition (its fields are inert this slice).
bool never_claims(const Context& /*ctx*/, const Operation* /*op*/) { return false; }

// CEIR-29a-2: claims EVERY ml op (the partitioner only calls advertise on ml ops) — a permissive native-graph stub, so
// the greedy-grow is exercised without coopvec's shape restrictions.
bool always_claims(const Context& /*ctx*/, const Operation* /*op*/) { return true; }

// A subgraph-claiming (or per-op, per the flag) native-graph stub provider. Distinct class from coopvec; claims any ml op.
gpu::MlProvider subgraph_stub(bool claims_subgraphs)
{
    gpu::MlProvider p;
    p.name             = StringView("native_graph_stub");
    p.available        = true;
    p.advertise        = &always_claims;
    p.provider_class   = ProviderClass::Gpu;
    p.memory_domain    = StringView("device_local");
    p.determinism      = DeterminismClass::DeterministicWithinTarget;
    p.claims_subgraphs = claims_subgraphs;
    return p;
}

// CEIR-29b-2b: the CUDA-Graphs LAUNCH-GRAPH provider row. ⛔ BitExact per §27 = the correctly-rounded REFERENCE value on every
// run AND target: the fmad=false + prec-div/prec-sqrt CUDA emitters round each op exactly like the CPU oracle (29b-1/29b-2a
// proof) + replay is bit-identical (29b-2a) + Win-RTX == WSL-RTX. It satisfies the enum's "every backend" clause because a
// correctly-rounded backend has ONE answer — Vk/DX12 diverge only because THEY contract (fma), not because this provider is
// target-bound. (STRONGER than coopvec's DeterministicWithinTarget, whose fused kernel may reassociate.) claims_subgraphs=true
// (it captures a maximal run into ONE cudaGraph). advertise = cuda_graphs_can_claim.
gpu::MlProvider cuda_graphs(bool available)
{
    gpu::MlProvider p;
    p.name             = StringView("cuda_graphs");
    p.available        = available;
    p.advertise        = &gpu::cuda_graphs_can_claim;
    p.provider_class   = ProviderClass::Gpu;
    p.memory_domain    = StringView("device_local");
    p.determinism      = DeterminismClass::BitExact;
    p.claims_subgraphs = true;
    return p;
}

// CEIR-29c-3a: an EXTERNAL-class provider stub — the second real class the constrained partition discriminates against cuda_graphs
// (Gpu). always_claims (like subgraph_stub) so it can claim BOTH the mlp AND the attention (the asymmetry vs cuda_graphs, which
// refuses attention, is what proves the class filter falls back to -1, not to another class). ⛔ a SEPARATE helper, not
// subgraph_stub with a class override — its only delta IS the class, so a distinct name keeps the gate readable. memory_domain
// "external" (a real §24 vocab member) + DeterminismClass::Unspecified (an external bridge honestly does not know its order).
gpu::MlProvider external_stub(bool available)
{
    gpu::MlProvider p;
    p.name             = StringView("external_stub");
    p.available        = available;
    p.advertise        = &always_claims;
    p.provider_class   = ProviderClass::External;
    p.memory_domain    = StringView("external");
    p.determinism      = DeterminismClass::Unspecified;
    p.claims_subgraphs = false;
    return p;
}

// CEIR-29c-3a: an Npu-class provider that is present + available but REFUSES every op (never_claims). It makes the {Npu} gate
// section DISCRIMINATING: with an in-class-but-refusing provider present, both ops still fall to -1 (never leak to the Gpu/External
// providers that DO claim) — the "or not at all" half of a §102 class requirement, proven non-vacuously.
gpu::MlProvider npu_stub(bool available)
{
    gpu::MlProvider p;
    p.name             = StringView("npu_stub");
    p.available        = available;
    p.advertise        = &never_claims;
    p.provider_class   = ProviderClass::Npu;
    p.memory_domain    = StringView("unified");
    p.determinism      = DeterminismClass::Unspecified;
    p.claims_subgraphs = false;
    return p;
}

// CEIR-29a-2: two ADJACENT ml.mlp ops (ALL inputs declared FIRST, so no op sits between the two mlps) — a fixture that
// proves greedy-grow MERGES a run of 2 (build_band24 can't: its mlp + attention are split by the q/ky/v declares).
void build_two_mlps(Context& ctx, Module& m)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const b = func::func_body_block(f);
    // ALL six inputs declared UP FRONT so the two mlps are adjacent (a declare between them would split the run).
    Value* const x1 = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const a1 = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const a2 = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 2U)));
    Value* const x2 = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const c1 = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const c2 = mkval(ctx, decl, b, tf(ctx, sh2(ctx, 8U, 2U)));
    Value*       o1[3] = {x1, a1, a2};
    Operation* const m1 = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(o1, 3U), 1U, tf(ctx, sh2(ctx, 4U, 2U)), 0U);
    ctx.set_attr(m1, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(m1);
    Value*       o2[3] = {x2, c1, c2};
    Operation* const m2 = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(o2, 3U), 1U, tf(ctx, sh2(ctx, 4U, 2U)), 0U);
    ctx.set_attr(m2, StringView("activation"), ctx.attr_string(StringView("relu")));
    b->append(m2); // ADJACENT to m1 — no op between
}

// CEIR-29a-2: an ml.mlp in EACH of two func bodies (two DIFFERENT blocks) — proves the subgraph id is MONOTONIC ACROSS
// blocks (a per-block-reset counter would give both runs id 0; the whole-module counter gives 0 and 1). The scar the
// row cites, made falsifiable.
void build_two_blocks(Context& ctx, Module& m)
{
    const OpId decl = ctx.intern_op("resource", "declare");
    Block*     top  = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const fa = func::create_func(ctx, m, "fa", Visibility::Public, 0U);
    top->append(fa);
    Block* const ba = func::func_body_block(fa);
    Value* const xa = mkval(ctx, decl, ba, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const aa = mkval(ctx, decl, ba, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const ab = mkval(ctx, decl, ba, tf(ctx, sh2(ctx, 8U, 2U)));
    Value*       oa[3] = {xa, aa, ab};
    Operation* const ma = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(oa, 3U), 1U, tf(ctx, sh2(ctx, 4U, 2U)), 0U);
    ctx.set_attr(ma, StringView("activation"), ctx.attr_string(StringView("relu")));
    ba->append(ma);
    Operation* const fb = func::create_func(ctx, m, "fb", Visibility::Public, 0U);
    top->append(fb);
    Block* const bb = func::func_body_block(fb);
    Value* const xb = mkval(ctx, decl, bb, tf(ctx, sh2(ctx, 4U, 8U)));
    Value* const ca = mkval(ctx, decl, bb, tf(ctx, sh2(ctx, 8U, 8U)));
    Value* const cb = mkval(ctx, decl, bb, tf(ctx, sh2(ctx, 8U, 2U)));
    Value*       ob[3] = {xb, ca, cb};
    Operation* const mb = ctx.create_operation(ctx.intern_op("ml", "mlp"), ConstSpan<Value*>(ob, 3U), 1U, tf(ctx, sh2(ctx, 4U, 2U)), 0U);
    ctx.set_attr(mb, StringView("activation"), ctx.attr_string(StringView("relu")));
    bb->append(mb);
}

// CEIR-29a-3b: a transform SCHEDULE module carrying ONE program-global transform.assign_provider{provider} directive at top
// level (register_transform_ops must have run). The parse-loaded committed .ceir form is 29a-3b-2; this is the in-memory form.
Module* build_pin_module(Context& ctx, StringView provider)
{
    Module* const tm = ctx.create_module();
    Block*        b  = tm->body()->first_block();
    if (b == nullptr)
    {
        b = ctx.create_block(0U);
        tm->body()->append(b);
    }
    b->append(transform::build_assign_provider(ctx, ctx.attr_string(provider)));
    return tm;
}

// CEIR-29c-3b: a transform SCHEDULE module carrying ONE program-global transform.constrain_provider_class{class} directive at
// top level (register_transform_ops must have run). The build_pin_module sibling for the class-filter half; the parse-loaded
// committed .ceir form is the composition section's asset, this is the in-memory oracle.
Module* build_class_module(Context& ctx, StringView class_name)
{
    Module* const tm = ctx.create_module();
    Block*        b  = tm->body()->first_block();
    if (b == nullptr)
    {
        b = ctx.create_block(0U);
        tm->body()->append(b);
    }
    b->append(transform::build_constrain_provider_class(ctx, ctx.attr_string(class_name)));
    return tm;
}
} // namespace

TEST_CASE("ceir 24z band gate: ml.mlp (variadic) + ml.attention COMPOSE, round-trip byte-clean (text + binary), and partition",
          "[ceir][band24]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    Module* const m = ctx.create_module();
    build_band24(ctx, *m);

    // (1) the composing property: find_ml + structure clean on the ONE mixed-op module.
    require_clean(ctx, *m);

    // (2) TEXT round-trip: print -> parse -> re-walk None + print==reprint (byte-exact).
    const String text1 = print(ctx, *m, &root);
    Context      ctx_t(&root);
    register_all(ctx_t);
    const ParseResult pr = parse(ctx_t, StringView(text1.c_str(), text1.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    require_clean(ctx_t, *pr.module);
    const String text2 = print(ctx_t, *pr.module, &root);
    CHECK(StringView(text1.c_str(), text1.size()) == StringView(text2.c_str(), text2.size()));

    // (3) BINARY round-trip: serialize -> deserialize -> re-walk None (the FIRST binary crossing for ml.mlp's VARIADIC tail + ml.attention).
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    REQUIRE(blob.size() > 0U);
    Context ctx_b(&root);
    register_all(ctx_b);
    const ParseResult dr = deserialize(ctx_b, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);
    require_clean(ctx_b, *dr.module);

    // (4) the §69 partitioner handles the MIXED module: coopvec CLAIMS the mlp (uniform hidden, valid dims), the attention FALLS BACK.
    const gpu::MlProvider  provs_on[1] = {coopvec(true)};
    const gpu::MlPartition p_on        = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs_on, 1U), &root);
    REQUIRE(p_on.assignments.size() == 2U);
    CHECK(p_on.claimed_by(0) == 1U); // the mlp -> coopvec
    CHECK(p_on.fallback() == 1U);    // the attention -> CKIR fallback
    // caps OFF (a non-NVIDIA device): everything falls back to the portable CKIR expansion.
    const gpu::MlProvider  provs_off[1] = {coopvec(false)};
    const gpu::MlPartition p_off        = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs_off, 1U), &root);
    CHECK(p_off.fallback() == 2U);
}

// CEIR-29a-1 — the §69 provider-descriptor gains {provider_class, memory_domain, determinism} (INERT this slice: `assign`
// still reads only available+advertise). Device-free. Identity checks, not category: the fields round-trip BY INDEX, and
// the widening leaves partition_ml's 24c behaviour byte-for-byte unchanged.
TEST_CASE("ceir 29a-1: MlProvider carries the provider-class descriptor fields; partition behaviour unchanged (sec 69)",
          "[ceir][ml][partition][band29]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    Module* const m = ctx.create_module();
    build_band24(ctx, *m); // the SAME mixed mlp + attention module as the 24z gate

    // A TWO-provider span of DISTINCT classes: coopvec (Gpu) first, then an External stub that never claims.
    gpu::MlProvider ext;
    ext.name           = StringView("ext_stub");
    ext.available      = true;
    ext.advertise      = &never_claims;
    ext.provider_class = ProviderClass::External;
    ext.memory_domain  = StringView("host_visible_device");
    ext.determinism    = DeterminismClass::Unspecified;
    const gpu::MlProvider provs[2] = {coopvec(true), ext};

    // (1) REGRESSION — the widening did NOT change partition_ml: coopvec (idx 0) still claims the mlp, the attention still
    //     falls back; the External stub (idx 1, never claims) claims nothing.
    const gpu::MlPartition part = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root);
    REQUIRE(part.assignments.size() == 2U);
    CHECK(part.claimed_by(0) == 1U); // mlp -> coopvec (Gpu)
    CHECK(part.claimed_by(1) == 0U); // the External stub claims nothing
    CHECK(part.fallback() == 1U);    // attention -> CKIR fallback

    // (2) the descriptor fields round-trip BY INDEX (identity, not category — External is External, not just != Gpu).
    CHECK(provs[0].provider_class == ProviderClass::Gpu);
    CHECK(provs[1].provider_class == ProviderClass::External);
    CHECK(provs[0].memory_domain == StringView("device_local"));
    CHECK(provs[1].memory_domain == StringView("host_visible_device"));
    CHECK(provs[0].determinism == DeterminismClass::DeterministicWithinTarget);
    CHECK(provs[1].determinism == DeterminismClass::Unspecified);

    // (3) memory_domain reuses the §24 vocabulary (resource.declare's) — one table, validated by is_memory_domain.
    CHECK(is_memory_domain(StringView("device_local")));         // coopvec's domain
    CHECK(is_memory_domain(StringView("host_visible_device")));  // the stub's domain
    CHECK(is_memory_domain(StringView("unified")));              // another §24 member
    CHECK_FALSE(is_memory_domain(StringView("bogus_domain")));   // a typo is NOT in the vocab
    CHECK_FALSE(is_memory_domain(StringView("")));               // "" is unspecified, not a domain

    // (4) the 24c negative re-asserted after the widening: caps OFF on the Gpu row -> everything falls back.
    gpu::MlProvider coop_off       = coopvec(false); // available=false
    const gpu::MlProvider provs2[2] = {coop_off, ext};
    const gpu::MlPartition p_off    = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs2, 2U), &root);
    CHECK(p_off.fallback() == 2U); // both ml ops fall back (coopvec unavailable, ext never claims)
}

// CEIR-29a-2 — greedy-grow: a `claims_subgraphs` provider fuses a maximal run of consecutive sibling ml ops into ONE
// subgraph (§102 maximal-subgraph, no graph-cut). A non-ml op / advertise refusal / different provider / block end splits
// it. Device-free. Identity checks, and the merge is proven on a NEW adjacent-mlp fixture (build_band24's ml ops are split
// by the q/ky/v declares, so it can only show the SPLIT — a merge-only-on-adjacent gate must be able to fail).
TEST_CASE("ceir 29a-2: greedy-grow fuses a maximal subgraph run; a non-ml op splits it; per-op vs subgraph discriminated (sec 102)",
          "[ceir][ml][partition][band29]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);

    // (1) MERGE — two ADJACENT ml.mlp ops; a claims_subgraphs=true provider grows ONE run of 2.
    {
        Module* const m = ctx.create_module();
        build_two_mlps(ctx, *m);
        const gpu::MlProvider  provs[1] = {subgraph_stub(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        REQUIRE(part.assignments.size() == 2U);
        CHECK(part.claimed_by(0) == 2U);                                       // both mlps claimed
        CHECK(part.subgraphs_of(0) == 1U);                                     // ONE subgraph — the run merged
        CHECK(part.assignments[0].subgraph == part.assignments[1].subgraph);   // same id
        CHECK(part.assignments[0].subgraph >= 0);                              // a real subgraph, not a singleton
        CHECK(part.subgraph_size(part.assignments[0].subgraph) == 2U);         // the run has 2 ops
    }

    // (2) DISCRIMINATOR / DEFAULT — the SAME adjacent mlps with claims_subgraphs=FALSE: per-op singletons (-1), NOT a run.
    {
        Module* const m = ctx.create_module();
        build_two_mlps(ctx, *m);
        const gpu::MlProvider  provs[1] = {subgraph_stub(false)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        CHECK(part.claimed_by(0) == 2U);   // still claims both ops
        CHECK(part.subgraphs_of(0) == 0U); // but as per-op singletons — the bool discriminates
        CHECK(part.assignments[0].subgraph == -1);
        CHECK(part.assignments[1].subgraph == -1);
    }

    // (3) SPLIT — build_band24 (ml.mlp, THEN q/ky/v declares, THEN ml.attention): a subgraph provider claiming BOTH ml
    //     kinds CANNOT merge across the intervening declares -> TWO distinct size-1 runs (monotonic, different ids).
    {
        Module* const m = ctx.create_module();
        build_band24(ctx, *m);
        const gpu::MlProvider  provs[1] = {subgraph_stub(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        REQUIRE(part.assignments.size() == 2U);
        CHECK(part.claimed_by(0) == 2U);
        CHECK(part.subgraphs_of(0) == 2U);                                     // TWO runs — the declares split them
        CHECK(part.assignments[0].subgraph != part.assignments[1].subgraph);   // distinct monotonic ids
        CHECK(part.subgraph_size(part.assignments[0].subgraph) == 1U);
        CHECK(part.subgraph_size(part.assignments[1].subgraph) == 1U);
    }

    // (4) DEFAULT PINNED — coopvec (claims_subgraphs defaults FALSE, the 29a-1 helper never sets it) on build_band24:
    //     claims the mlp per-op (-1), attention falls back; subgraphs_of(coopvec)==0. Pins the default is per-op, not a run.
    {
        Module* const m = ctx.create_module();
        build_band24(ctx, *m);
        const gpu::MlProvider  provs[1] = {coopvec(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        CHECK(part.claimed_by(0) == 1U);   // mlp claimed
        CHECK(part.fallback() == 1U);      // attention falls back
        CHECK(part.subgraphs_of(0) == 0U); // coopvec is per-op (claims_subgraphs=false default)
        for (crd::usize i = 0; i < part.assignments.size(); ++i) { CHECK(part.assignments[i].subgraph == -1); }
    }

    // (5) MONOTONIC ACROSS BLOCKS — one module, an mlp in each of two func bodies; a claims_subgraphs=true provider.
    //     The two runs are in DIFFERENT blocks, so a per-block-reset counter would give both id 0 (subgraphs_of==1);
    //     the whole-module counter gives distinct ids (subgraphs_of==2). This is the assertion the monotone scar needs.
    {
        Module* const m = ctx.create_module();
        build_two_blocks(ctx, *m);
        const gpu::MlProvider  provs[1] = {subgraph_stub(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        REQUIRE(part.assignments.size() == 2U);
        CHECK(part.subgraphs_of(0) == 2U);                                   // TWO runs across two blocks (not reset to 1)
        CHECK(part.assignments[0].subgraph != part.assignments[1].subgraph); // distinct monotonic ids across the block boundary
        CHECK(part.subgraph_size(part.assignments[0].subgraph) == 1U);
        CHECK(part.subgraph_size(part.assignments[1].subgraph) == 1U);
    }
}

// CEIR-29a-3a: the partitioner honours an AUTHORED pin (a `pinned` index into the provider span) as a PREFERENCE among
// claimers, NEVER a force. Device-free. Identity on the WINNING provider index (not the class — both stubs are Gpu-class).
// The pin's authored SOURCE (transform.assign_provider {provider} -> a span index by NAME, with the unknown-name reject and
// the duplicate-name guard) lands in CEIR-29a-3b; here the index is driven directly to isolate the partitioner mechanism.
TEST_CASE("ceir 29a-3: an authored provider pin is honoured as a preference among claimers, never a forced claim (sec 102)",
          "[ceir][ml][partition][band29]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);

    // (a) DISCRIMINATOR — two providers that BOTH claim the mlp; the SAME module, the pin flips the winner. IDENTITY on the
    //     index: unpinned falls to first-available (index 0, the 24c invariant); pinned=1 reassigns the SAME ops to index 1.
    {
        Module* const m = ctx.create_module();
        build_two_mlps(ctx, *m);
        const gpu::MlProvider  provs[2] = {coopvec(true), subgraph_stub(false)}; // index 0 + index 1 BOTH claim the mlp
        const gpu::MlPartition none     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root);    // pinned=-1
        CHECK(none.claimed_by(0) == 2U); // unpinned -> first-available (index 0)
        CHECK(none.claimed_by(1) == 0U);
        const gpu::MlPartition pin1 = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root, {.pinned = 1}); // pin index 1
        CHECK(pin1.claimed_by(1) == 2U); // the SAME module now assigns both mlps to the pinned index 1
        CHECK(pin1.claimed_by(0) == 0U); // identity on the WINNING index, not the class (both providers are ProviderClass::Gpu)
    }

    // (b) PREFERENCE, NOT FORCE, and the pin MOVES the winner — providers {always-claims stub @0, coopvec @1}. Unpinned the mlp
    //     goes to the stub (index 0, first-available); pinned=1 the pin MOVES it to coopvec (index 1, which CAN claim), while the
    //     attention coopvec REFUSES escapes to the stub (index 0) — NEVER forced onto the pin. One block proves both properties.
    {
        Module* const m = ctx.create_module();
        build_band24(ctx, *m);
        const gpu::MlProvider  provs[2] = {subgraph_stub(false), coopvec(true)}; // stub @0 claims anything; coopvec @1 is the pin
        const gpu::MlPartition none     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root);    // pinned=-1
        CHECK(none.assignments[0].provider == 0); // unpinned: mlp -> first-available (the stub @0)
        const gpu::MlPartition part = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root, {.pinned = 1}); // pin coopvec @1
        REQUIRE(part.assignments.size() == 2U);
        CHECK(part.assignments[0].provider == 1); // pin MOVES the mlp 0 -> 1 (coopvec CAN claim it)
        CHECK(part.assignments[1].provider == 0); // attention -> the stub @0, because the pin REFUSES it (never forced to 1)
    }

    // (c) A PIN CANNOT OVERRIDE CAPS — the pinned provider is unavailable; the pin is skipped, first-available takes over.
    {
        Module* const m = ctx.create_module();
        build_two_mlps(ctx, *m);
        const gpu::MlProvider  provs[2] = {coopvec(false), subgraph_stub(false)}; // the pin target (index 0) is available=false
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root, {.pinned = 0}); // pin unavailable
        CHECK(part.claimed_by(0) == 0U); // an unavailable pin can't claim
        CHECK(part.claimed_by(1) == 2U); // first-available (index 1) takes both mlps
    }

    // (d) OUT-OF-RANGE PIN = NO-OP — a pin index past the span is ignored (behaves as unpinned). The loader rejects unknown
    //     names (29a-3b), but the partitioner is DEFENSIVE so a stray index can never mis-assign.
    {
        Module* const m = ctx.create_module();
        build_two_mlps(ctx, *m);
        const gpu::MlProvider  provs[1] = {coopvec(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root, {.pinned = 5}); // pin index 5 (out of range)
        CHECK(part.claimed_by(0) == 2U); // behaves exactly like unpinned
    }

    // (e) PIN x GREEDY-GROW — the 29b production path (a subgraph provider, pinned by name): pin the claims_subgraphs=true stub
    //     (index 1) on two adjacent mlps -> it grows ONE run at the pinned index; coopvec (index 0) claims nothing.
    {
        Module* const m = ctx.create_module();
        build_two_mlps(ctx, *m);
        const gpu::MlProvider  provs[2] = {coopvec(true), subgraph_stub(true)}; // index 1 is a subgraph claimer
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root, {.pinned = 1}); // pin the run-claimer
        REQUIRE(part.assignments.size() == 2U);
        CHECK(part.claimed_by(1) == 2U);                                     // both mlps at the pinned index
        CHECK(part.claimed_by(0) == 0U);                                     // coopvec claims nothing (the pin won)
        CHECK(part.subgraphs_of(1) == 1U);                                   // ONE run grown under the pin
        CHECK(part.assignments[0].subgraph == part.assignments[1].subgraph); // same id
        CHECK(part.subgraph_size(part.assignments[0].subgraph) == 2U);       // the run has both ops
    }

    // (f) SPLIT UNDER PIN — pin a PER-OP provider (coopvec @0, claims_subgraphs=false) on build_band24 with a subgraph stub @1:
    //     the mlp is coopvec's per-op singleton (-1), the attention coopvec REFUSES is a size-1 RUN at the stub (index 1). A pin
    //     on a per-op provider can NEVER accidentally join the other provider's run.
    {
        Module* const m = ctx.create_module();
        build_band24(ctx, *m);
        const gpu::MlProvider  provs[2] = {coopvec(true), subgraph_stub(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root, {.pinned = 0}); // pin per-op coopvec
        REQUIRE(part.assignments.size() == 2U);
        CHECK(part.assignments[0].provider == 0);  // mlp -> the pinned coopvec
        CHECK(part.assignments[0].subgraph == -1); // as a per-op singleton, NOT a run (claims_subgraphs=false)
        CHECK(part.assignments[1].provider == 1);  // attention -> the stub (coopvec refuses)
        CHECK(part.assignments[1].subgraph >= 0);  // a real run id
        CHECK(part.subgraphs_of(0) == 0U);         // coopvec grew no run
        CHECK(part.subgraph_size(part.assignments[1].subgraph) == 1U); // the stub's run is size 1 (the mlp did NOT join it)
    }
}

// CEIR-29a-3b-1: the AUTHORED-PIN loader provider_from_transform resolves a transform.assign_provider{provider} directive to an
// MlProvider span INDEX (the pin partition_ml honours), with TYPED rejects for an unknown name, a duplicate-name span, and an
// empty/empty-span pin. The transform module is built IN MEMORY here; the parse-loaded committed .ceir asset is 29a-3b-2.
TEST_CASE("ceir 29a-3b: transform.assign_provider resolves a provider name to a partition pin, with typed rejects (sec 71)",
          "[ceir][ml][partition][band29]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    (void)transform::register_transform_ops(ctx); // the schedule dialect (register_all covers func/resource/ml only)

    const gpu::MlProvider provs[2] = {subgraph_stub(false), coopvec(true)}; // "native_graph_stub" @0, "coopvec" @1

    // (a) RESOLVE + END-TO-END — assign_provider{"coopvec"} => Resolved, index 1; feeding that index to partition_ml on
    //     build_band24 pins the mlp to coopvec (1), and the attention coopvec refuses escapes to the stub (0). Not just resolution.
    {
        Module* const         tm  = build_pin_module(ctx, StringView("coopvec"));
        const gpu::ProviderPin pin = gpu::provider_from_transform(ctx, *tm, ConstSpan<gpu::MlProvider>(provs, 2U));
        REQUIRE(pin.kind == gpu::ProviderPinKind::Resolved);
        CHECK(pin.index == 1);
        CHECK(pin.op != nullptr);
        Module* const m = ctx.create_module();
        build_band24(ctx, *m);
        const gpu::MlPartition part = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root, {.pinned = pin.index});
        REQUIRE(part.assignments.size() == 2U);
        CHECK(part.assignments[0].provider == 1); // mlp -> coopvec (the resolved pin)
        CHECK(part.assignments[1].provider == 0); // attention -> the stub (coopvec refuses; never forced)
    }

    // (b) DISCRIMINATOR — the SAME span, a DIFFERENT name resolves to a DIFFERENT index (identity on the resolved index).
    {
        Module* const         tm  = build_pin_module(ctx, StringView("native_graph_stub"));
        const gpu::ProviderPin pin = gpu::provider_from_transform(ctx, *tm, ConstSpan<gpu::MlProvider>(provs, 2U));
        REQUIRE(pin.kind == gpu::ProviderPinKind::Resolved);
        CHECK(pin.index == 0);
    }

    // (c) UNKNOWN NAME — a name absent from the span is a TYPED reject (index -1, op set), never a silent no-pin.
    {
        Module* const         tm  = build_pin_module(ctx, StringView("no_such_provider"));
        const gpu::ProviderPin pin = gpu::provider_from_transform(ctx, *tm, ConstSpan<gpu::MlProvider>(provs, 2U));
        CHECK(pin.kind == gpu::ProviderPinKind::UnknownProvider);
        CHECK(pin.index == -1);
        CHECK(pin.op != nullptr);
    }

    // (d) NO DIRECTIVE — an empty schedule module is None (no pin; partition_ml runs first-available).
    {
        Module* const tm = ctx.create_module();
        Block* const  b  = ctx.create_block(0U);
        tm->body()->append(b); // an empty top block, no directive
        const gpu::ProviderPin pin = gpu::provider_from_transform(ctx, *tm, ConstSpan<gpu::MlProvider>(provs, 2U));
        CHECK(pin.kind == gpu::ProviderPinKind::None);
        CHECK(pin.index == -1);
    }

    // (e) DUPLICATE-NAME SPAN — two providers share a name: ANY pin is ambiguous BY CONSTRUCTION (a span defect, reported
    //     BEFORE resolving — even though the directive names "coopvec", the reject is DuplicateProviderName, not Resolved).
    {
        const gpu::MlProvider  dup[2] = {coopvec(true), coopvec(true)}; // both named "coopvec"
        Module* const          tm     = build_pin_module(ctx, StringView("coopvec"));
        const gpu::ProviderPin pin    = gpu::provider_from_transform(ctx, *tm, ConstSpan<gpu::MlProvider>(dup, 2U));
        CHECK(pin.kind == gpu::ProviderPinKind::DuplicateProviderName);
        CHECK(pin.index == -1);
    }

    // (f) EMPTY NAME + EMPTY SPAN — an empty `provider` string names nothing (UnknownProvider); a pin against an EMPTY span is
    //     UnknownProvider too, never a silent -1 no-op.
    {
        Module* const tm = build_pin_module(ctx, StringView("")); // empty provider name
        CHECK(gpu::provider_from_transform(ctx, *tm, ConstSpan<gpu::MlProvider>(provs, 2U)).kind == gpu::ProviderPinKind::UnknownProvider);
        Module* const tm2 = build_pin_module(ctx, StringView("coopvec"));
        CHECK(gpu::provider_from_transform(ctx, *tm2, ConstSpan<gpu::MlProvider>(provs, 0U)).kind ==
              gpu::ProviderPinKind::UnknownProvider); // a pin against an empty span
    }

    // (g) DUPLICATE DIRECTIVE — two transform.assign_provider ops: find_transform_misuse is DuplicateDirective pointing at the
    //     SECOND op (the 27a module-wide walk owns duplicates; provider_from_transform assumes a misuse-clean module).
    {
        Module* const tm = ctx.create_module();
        Block* const  b  = ctx.create_block(0U);
        tm->body()->append(b);
        b->append(transform::build_assign_provider(ctx, ctx.attr_string(StringView("coopvec"))));
        Operation* const second = transform::build_assign_provider(ctx, ctx.attr_string(StringView("coopvec")));
        b->append(second);
        const transform::TransformMisuse mis = transform::find_transform_misuse(ctx, *tm);
        CHECK(mis.kind == transform::TransformMisuseKind::DuplicateDirective);
        CHECK(mis.op == second);
    }

    // (h) NESTED DIRECTIVE — a transform.assign_provider inside a func-body region (NOT top-level): the loader is region-
    //     RECURSIVE (matching find_transform_misuse's walk it relies on for at-most-one), so it still Resolves. A top-level-only
    //     walk would return None here (a silent no-pin the guard wouldn't flag) — the falsifiable frontier for the recursion.
    {
        Module* const    tm  = ctx.create_module();
        Block* const     top = ctx.create_block(0U);
        tm->body()->append(top);
        Operation* const f = func::create_func(ctx, *tm, "main", Visibility::Public, 0U);
        top->append(f);
        func::func_body_block(f)->append(transform::build_assign_provider(ctx, ctx.attr_string(StringView("coopvec"))));
        const gpu::ProviderPin pin = gpu::provider_from_transform(ctx, *tm, ConstSpan<gpu::MlProvider>(provs, 2U));
        CHECK(pin.kind == gpu::ProviderPinKind::Resolved); // found nested (recursive), not missed (which a top-level walk would)
        CHECK(pin.index == 1);
    }
}

// CEIR-29a-3b-2: the COMMITTED assign_provider .ceir asset (§146 "the schedule IS an authored asset"): it parse-loads, is
// CANONICAL (anti-drift through the PRINTER vs the build_pin_module oracle — never file-bytes; the 28 committed-asset scar),
// roundtrip-stable, walks clean, and provider_from_transform RESOLVES it to the coopvec index partition_ml pins. Device-free.
// ⛔ if build_pin_module changes, REGENERATE assets/ceir/assign_provider_coopvec.ceir (the 22c-3c committed-asset rule).
TEST_CASE("ceir 29a-3b-2: the committed assign_provider.ceir asset is canonical and resolves to a partition pin (sec 146)",
          "[ceir][ml][partition][band29]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    (void)transform::register_transform_ops(ctx);

    // the in-memory ORACLE + its canonical print (the anti-drift SOURCE — keep build_pin_module, do NOT delete it).
    Module* const            built   = build_pin_module(ctx, StringView("coopvec"));
    const containers::String t_built = print(ctx, *built, &root);

    // parse-load the COMMITTED asset.
    const containers::Array<char> src = test_support::slurp_asset(CRD_REPO_DIR "/assets/ceir/assign_provider_coopvec.ceir", ctx);
    const ParseResult             pr  = parse(ctx, StringView(src.data(), src.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ⭐ ANTI-DRIFT: the committed file's canonical print == the oracle's (regenerate the file if build_pin_module changes).
    const containers::String t_file = print(ctx, *pr.module, &root);
    CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

    // ROUNDTRIP stability: print(parse(file)) re-parses to the identical canonical text (the committed file IS canonical).
    Context ctx2(&root);
    register_all(ctx2);
    (void)transform::register_transform_ops(ctx2);
    const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
    REQUIRE(pr2.ok);
    const containers::String t_file2 = print(ctx2, *pr2.module, &root);
    CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

    // the schedule is well-formed (the assign_provider directive appears at most once — the walk-then-load discipline).
    CHECK(transform::find_transform_misuse(ctx, *pr.module).kind == transform::TransformMisuseKind::None);

    // ⭐ provider_from_transform resolves the LOADED asset against the provider span, and the pin drives partition_ml end-to-end.
    const gpu::MlProvider  provs[2] = {subgraph_stub(false), coopvec(true)}; // coopvec @1
    const gpu::ProviderPin pin      = gpu::provider_from_transform(ctx, *pr.module, ConstSpan<gpu::MlProvider>(provs, 2U));
    REQUIRE(pin.kind == gpu::ProviderPinKind::Resolved);
    CHECK(pin.index == 1); // "coopvec" -> index 1 in this span
    Module* const m = ctx.create_module();
    build_band24(ctx, *m);
    const gpu::MlPartition part = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 2U), &root, {.pinned = pin.index});
    REQUIRE(part.assignments.size() == 2U);
    CHECK(part.assignments[0].provider == 1); // mlp -> coopvec (the authored, LOADED pin)
    CHECK(part.assignments[1].provider == 0); // attention -> the stub (coopvec refuses)

    // ⭐ RESOLUTION vs HONOURING are SEPARATE layers — the production failure mode: a .ceir pinning coopvec shipped to a box
    //    WITHOUT cooperative_vector. provider_from_transform still RESOLVES (index 1, resolution is caps-BLIND — correct); but
    //    partition_ml with coopvec available=false HONOURS the caps and falls the mlp back to the stub (0). Two layers, proven.
    {
        const gpu::MlProvider  capped[2] = {subgraph_stub(false), coopvec(false)}; // coopvec @1 UNAVAILABLE
        const gpu::ProviderPin cp        = gpu::provider_from_transform(ctx, *pr.module, ConstSpan<gpu::MlProvider>(capped, 2U));
        REQUIRE(cp.kind == gpu::ProviderPinKind::Resolved); // resolution is caps-blind: the name still resolves to index 1
        CHECK(cp.index == 1);
        Module* const mc = ctx.create_module();
        build_band24(ctx, *mc);
        const gpu::MlPartition pc = gpu::partition_ml(ctx, *mc, ConstSpan<gpu::MlProvider>(capped, 2U), &root, {.pinned = cp.index});
        CHECK(pc.assignments[0].provider == 0); // mlp -> the stub: the UNAVAILABLE pin can't claim (caps win over the pin)
    }
}

// CEIR-29b-2b: the CUDA-Graphs LAUNCH-GRAPH provider descriptor. Device-free (partition + expand are pure IR). It proves the
// two properties that separate a LAUNCH-GRAPH provider (CUDA Graphs — captures the EXPANDED pipeline, 29b-2a) from a
// SEMANTIC-ENGINE one (coopvec — a native fused kernel that REPLACES the op): (a) it CLAIMS the ml.mlp run as ONE subgraph
// (so the partition can NAME it — 29c pins/compares against it) and REFUSES ml.attention (the specialized-kernel scar:
// relu-only, no CUDA attention emitter); (b) a claim is a NAMING, not a replace — the claimed run STILL EXPANDS via
// expand_ml_ops (a launch-graph NEVER calls apply_partition, which would leave a claimed op in place), because it captures
// the EXPANDED gemm/relu/gemm dispatches. The captured-graph bit-exactness itself is the device gate 29b-2a.
TEST_CASE("ceir 29b-2b: the cuda_graphs launch-graph provider claims the mlp run as one subgraph; a claim does not suppress expansion (partition, sec 70)",
          "[ceir][ml][partition]")
{
    memory::GrowableTlsfAllocator root;

    // (0) IDENTITY: the descriptor's honest §27 class is BitExact — STRONGER than coopvec's DeterministicWithinTarget: the
    //     correctly-rounded (fmad=false) CUDA pipeline bit-matches the CPU reference (29b-1/29b-2a), a fused kernel need not.
    CHECK(cuda_graphs(true).determinism == DeterminismClass::BitExact);
    CHECK(cuda_graphs(true).claims_subgraphs);

    // (1) CLAIM + ATTENTION DISCRIMINATOR — build_band24 (one relu ml.mlp + one ml.attention): cuda_graphs claims ONLY the
    //     mlp as one subgraph (a run of 1); the attention falls back (no CUDA attention emitter — the real can't-claim case).
    SECTION("claims the mlp as one subgraph, refuses attention")
    {
        Context ctx(&root);
        register_all(ctx);
        Module* const m = ctx.create_module();
        build_band24(ctx, *m);
        require_clean(ctx, *m);
        const gpu::MlProvider  provs[1] = {cuda_graphs(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        CHECK(part.claimed_by(0) == 1U);           // the mlp
        CHECK(part.fallback() == 1U);              // the attention (cuda_graphs_can_claim is false for ml.attention)
        CHECK(part.subgraphs_of(0) == 1U);         // claims_subgraphs -> the claimed mlp is one run
        CHECK(part.assignments[0].provider == 0);  // op 0 (the mlp) -> cuda_graphs
        CHECK(part.assignments[0].subgraph >= 0);  // a launch-graph run gets a subgraph id, not a -1 singleton
    }

    // (2) MAXIMAL RUN — two ADJACENT mlps: cuda_graphs (claims_subgraphs=true) grows ONE run of 2 (captured as one cudaGraph).
    //     subgraphs_of==1, subgraph_size==2 (the maximal-subgraph identity, vs a per-op claimer's two -1 singletons).
    SECTION("grows a maximal run of adjacent mlps into one subgraph")
    {
        Context ctx(&root);
        register_all(ctx);
        Module* const m = ctx.create_module();
        build_two_mlps(ctx, *m);
        require_clean(ctx, *m);
        const gpu::MlProvider  provs[1] = {cuda_graphs(true)};
        const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        CHECK(part.claimed_by(0) == 2U);
        CHECK(part.subgraphs_of(0) == 1U);
        const crd::i32 id = part.assignments[0].subgraph;
        REQUIRE(id >= 0);
        CHECK(part.subgraph_size(id) == 2U);
    }

    // (3) LAUNCH-GRAPH != CLAIM-AND-REPLACE (the code-level record of the two-kinds distinction, FALSIFIABLE via a CONTRAST —
    //     expand_ml_ops alone never sees the partition, so its count is claim-independent; the discriminating fact is that
    //     apply_partition DOES honor the claim and expand_ml_ops does NOT). SAME module + SAME cuda_graphs claim, two paths:
    SECTION("a launch-graph claim does not suppress expansion (apply_partition WOULD leave it in place)")
    {
        const gpu::MlProvider provs[1] = {cuda_graphs(true)};

        // SEMANTIC-ENGINE path: apply_partition expands ONLY the fallback ops and LEAVES a claimed op in place. cuda_graphs
        // claims the mlp -> apply_partition expands ONLY the attention (fallback) => expanded==1, the mlp left un-expanded.
        Context ctxa(&root);
        register_all(ctxa);
        Module* const ma = ctxa.create_module();
        build_band24(ctxa, *ma);
        const gpu::MlPartition pa = gpu::partition_ml(ctxa, *ma, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        REQUIRE(pa.claimed_by(0) == 1U); // mlp claimed, attention fallback
        const gpu::MlExpandResult er_apply = gpu::apply_partition(ctxa, *ma, pa);
        CHECK(er_apply.error == gpu::MlExpandError::None);
        CHECK(er_apply.expanded == 1U); // ONLY the attention; the CLAIMED mlp was LEFT IN PLACE (the claim-and-replace model)

        // LAUNCH-GRAPH path: cuda_graphs does NOT call apply_partition — it EXPANDS the whole module (the claimed mlp INCLUDED)
        // and captures the dispatches (29b-2a). SAME module + SAME claim, yet expand_ml_ops expands BOTH (==2). The 1-vs-2
        // contrast is the distinction: apply_partition honors the claim, the launch-graph path deliberately does not use it.
        Context ctxb(&root);
        register_all(ctxb);
        Module* const mb = ctxb.create_module();
        build_band24(ctxb, *mb);
        const gpu::MlPartition pb = gpu::partition_ml(ctxb, *mb, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
        REQUIRE(pb.claimed_by(0) == 1U);
        const gpu::MlExpandResult er_expand = gpu::expand_ml_ops(ctxb, *mb);
        CHECK(er_expand.error == gpu::MlExpandError::None);
        CHECK(er_expand.expanded == 2U); // the claimed mlp expands too -> the launch-graph captures the EXPANDED pipeline
    }
}

// CEIR-29c-1: plan_tensor_pipeline_partitioned TAGS each stage with the provider that claimed the ml op it was expanded from
// (via the expand→source-ml lineage), leaving the plan otherwise byte-identical. Device-free. This is the consumer PlanStage.
// provider exists for: a launch-graph provider's claimed run is the CONTIGUOUS stage range sharing its index (29c-2 captures it).
TEST_CASE("ceir 29c-1: plan_tensor_pipeline_partitioned tags each stage with its ml op's provider; plan otherwise unchanged (partition, sec 102)",
          "[ceir][ml][partition]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    Module* const m = ctx.create_module();
    build_band24(ctx, *m); // relu ml.mlp (pre-order op 0) + ml.attention (op 1)

    // partition BEFORE expansion: cuda_graphs claims the mlp (idx 0); the attention falls back (-1).
    const gpu::MlProvider  provs[1] = {cuda_graphs(true)};
    const gpu::MlPartition part     = gpu::partition_ml(ctx, *m, ConstSpan<gpu::MlProvider>(provs, 1U), &root);
    REQUIRE(part.assignments.size() == 2U);
    REQUIRE(part.assignments[0].provider == 0);  // mlp -> cuda_graphs
    REQUIRE(part.assignments[1].provider == -1); // attention -> fallback

    // expand WITH lineage (created op -> source ml pre-order index) ONCE; expansion is opts-independent (only the PLAN reads
    // PlanOptions), so both configs below tag off this same lineage.
    containers::HashMap<const Operation*, i32> lineage(&root);
    const gpu::MlExpandResult                  er = gpu::expand_ml_ops(ctx, *m, lineage);
    REQUIRE(er.error == gpu::MlExpandError::None);
    REQUIRE(er.expanded == 2U);

    // The two-class contract on a partitioned plan vs its partition-BLIND twin (SAME opts): the tag adds/drops no stage or
    // buffer (metadata-only), and the mlp's stages (provider 0) form a contiguous PREFIX with the attention's (-1) a contiguous
    // SUFFIX. Returns the split k (the mlp's stage count) so a caller can probe the prefix. Expansion is pre-order (mlp precedes
    // attention) and each ml op's stages are contiguous, so a single split k separates the two classes.
    const auto check_two_class = [&](const gpu::TensorPipelinePlan& p, const gpu::TensorPipelinePlan& blind) -> usize {
        REQUIRE(p.reject == gpu::PlanReject::None);
        REQUIRE(blind.reject == gpu::PlanReject::None);
        REQUIRE(p.stages.size() > 1U);
        CHECK(p.stages.size() == blind.stages.size());   // metadata-only: no stage added/dropped by the tag
        CHECK(p.buffers.size() == blind.buffers.size());
        usize k = 0;
        while (k < p.stages.size() && p.stages[k].provider == 0) { ++k; }
        CHECK(k > 0U);              // the mlp claimed at least one stage
        CHECK(k < p.stages.size()); // ...and the attention's stages remain (the two-class split is real)
        for (usize s = 0; s < p.stages.size(); ++s)
        {
            CHECK(p.stages[s].provider == (s < k ? 0 : -1)); // 0-prefix / -1-suffix: no interleave, no untagged stage
        }
        return k;
    };

    // (1) FUSED (default fuse_gemm_relu=true): the mlp collapses gemm->relu into GemmRelu stages; contiguous 0-prefix.
    SECTION("fused plan: mlp stages tag provider 0 as a contiguous prefix, attention -1 suffix")
    {
        const gpu::TensorPipelinePlan plan  = gpu::plan_tensor_pipeline_partitioned(ctx, *m, &root, part, lineage);
        const gpu::TensorPipelinePlan blind = gpu::plan_tensor_pipeline(ctx, *m, &root);
        check_two_class(plan, blind);
    }

    // (2) UNFUSED (fuse_gemm_relu=false — the 29b-1 capture config): each relu is a SEPARATE StageKind::VizDispatch stage whose
    //     op is the mk_dispatch compute.dispatch (created INSIDE the (before, after) expand range -> in the lineage). Were that
    //     dispatch NOT tagged, a relu stage would read -1 mid-mlp and split the prefix -> check_two_class fails. This proves the
    //     CONTIGUOUS-RUN invariant that 29c-2 captures holds on the UNFUSED plan (the config it actually wraps), not just fused.
    SECTION("unfused plan: standalone relu VizDispatch stages still tag provider 0 (contiguity holds for the captured config)")
    {
        gpu::PlanOptions opts;
        opts.fuse_gemm_relu                 = false;
        const gpu::TensorPipelinePlan plan  = gpu::plan_tensor_pipeline_partitioned(ctx, *m, &root, part, lineage, opts);
        const gpu::TensorPipelinePlan blind = gpu::plan_tensor_pipeline(ctx, *m, &root, opts);
        const usize                   k     = check_two_class(plan, blind);
        // NON-VACUOUS: the mlp prefix genuinely contains a standalone relu dispatch (else contiguity would pass trivially — the
        // whole point is that a VizDispatch stage read the lineage correctly).
        usize n_viz = 0;
        for (usize s = 0; s < k; ++s) { n_viz += plan.stages[s].kind == gpu::StageKind::VizDispatch ? 1U : 0U; }
        CHECK(n_viz > 0U);
    }

    // (3) MISMATCH: a partition with FEWER assignments than the module's ml ops (1 < 2) -> the attention's lineage index (1) is
    //     >= assignments.size() (1). plan_tensor_pipeline_partitioned must reject LOUDLY (PartitionLineageMismatch), never
    //     silently tag -1 (which would masquerade as "the fallback claimed the attention"). reject_op = nullptr (a KEY-property
    //     mismatch between two inputs, not an op fault — the TuneCacheLockedMiss mold).
    SECTION("a lineage index outside the partition rejects loudly (PartitionLineageMismatch), never silent -1")
    {
        gpu::MlPartition short_part(&root);
        short_part.assignments.push_back(part.assignments[0]); // only the mlp's assignment; the attention's index (1) is now OOB
        REQUIRE(short_part.assignments.size() == 1U);
        const gpu::TensorPipelinePlan plan = gpu::plan_tensor_pipeline_partitioned(ctx, *m, &root, short_part, lineage);
        CHECK(plan.reject == gpu::PlanReject::PartitionLineageMismatch);
        CHECK(plan.reject_op == nullptr);
    }
}

// CEIR-29c-3a: the CONSTRAINED provider-choice mode — a PartitionConstraint's `provider_class` is a HARD FILTER (a §102 portability
// boundary: "a provider of THIS class, or the CKIR fallback — NEVER another class"), COMPOSED with the 29a-3 `pinned` preference
// (filter OUTER, pin inner). Device-free. TWO real classes discriminate: cuda_graphs (Gpu) + external_stub (External), BOTH
// claiming the mlp; the attention (cuda_graphs REFUSES it, external claims it) is the asymmetry that proves the filter falls back
// to -1, not to the other class. ⛔ partition_ml is a PURE inspection (const module) — build ONCE, partition many.
TEST_CASE("ceir 29c-3a: a provider_class constraint HARD-FILTERS eligible claimers; the pin is a preference WITHIN the class (partition, sec 102)",
          "[ceir][ml][partition]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    Module* const m = ctx.create_module();
    build_band24(ctx, *m); // mlp (op0), attention (op1)
    // provider 0 = cuda_graphs (Gpu; claims the relu mlp, REFUSES attention); provider 1 = external_stub (External; always_claims);
    // provider 2 = npu_stub (Npu; present + available but REFUSES every op — the discriminator for the {Npu} section).
    const gpu::MlProvider provs[3] = {cuda_graphs(true), external_stub(true), npu_stub(true)};
    const auto            span     = ConstSpan<gpu::MlProvider>(provs, 3U);

    // (1) UNCONSTRAINED — the baseline (and the proof BOTH stubs are LIVE claimers): mlp -> first-available (cuda_graphs @0);
    //     attention -> external @1 (cuda_graphs refuses).
    const gpu::MlPartition base = gpu::partition_ml(ctx, *m, span, &root);
    REQUIRE(base.assignments.size() == 2U);
    CHECK(base.assignments[0].provider == 0);
    CHECK(base.assignments[1].provider == 1);

    // (2) {class=Gpu} — external @1 is FILTERED OUT: mlp -> cuda_graphs @0; attention -> -1 (cuda_graphs refuses AND external is
    //     excluded ⇒ FALLBACK, never the other class). ⭐ the load-bearing assertion — a mere preference would have leaked to external.
    const gpu::MlPartition gpu_only =
        gpu::partition_ml(ctx, *m, span, &root, {.has_class = true, .provider_class = ProviderClass::Gpu});
    CHECK(gpu_only.assignments[0].provider == 0);
    CHECK(gpu_only.assignments[1].provider == -1);

    // (3) {class=External} — cuda_graphs @0 is FILTERED OUT even though it is FIRST: mlp AND attention -> external @1.
    const gpu::MlPartition ext_only =
        gpu::partition_ml(ctx, *m, span, &root, {.has_class = true, .provider_class = ProviderClass::External});
    CHECK(ext_only.assignments[0].provider == 1);
    CHECK(ext_only.assignments[1].provider == 1);

    // (4) {class=Npu} — npu_stub @2 IS Npu-class AND available, but REFUSES every op: both ops -> -1 (an in-class-but-refusing
    //     provider yields the fallback, NEVER a leak to the Gpu/External providers that DO claim — the "...or not at all" half of
    //     a §102 class requirement, proven NON-VACUOUSLY: without npu_stub this section would pass under any impl).
    const gpu::MlPartition npu_only =
        gpu::partition_ml(ctx, *m, span, &root, {.has_class = true, .provider_class = ProviderClass::Npu});
    CHECK(npu_only.assignments[0].provider == -1);
    CHECK(npu_only.assignments[1].provider == -1);

    // (5) COMPOSITION — the filter is OUTER, the pin inner. {class=External, pin=0}: the pin targets cuda_graphs @0, which the
    //     External filter EXCLUDES ⇒ the pin is IGNORED, external @1 wins (NOT the pinned Gpu provider). {class=Gpu, pin=0}: the
    //     pin is inside the filtered set ⇒ honoured, mlp -> 0. ⛔ (5) is the section that breaks if "pin-first-then-class" were built.
    const gpu::MlPartition ext_pin0 =
        gpu::partition_ml(ctx, *m, span, &root, {.has_class = true, .provider_class = ProviderClass::External, .pinned = 0});
    CHECK(ext_pin0.assignments[0].provider == 1); // the excluded pin is dropped — external wins, not the pinned Gpu provider
    const gpu::MlPartition gpu_pin0 =
        gpu::partition_ml(ctx, *m, span, &root, {.has_class = true, .provider_class = ProviderClass::Gpu, .pinned = 0});
    CHECK(gpu_pin0.assignments[0].provider == 0); // the in-class pin is honoured
}

// CEIR-29c-3b: the AUTHORED class constraint. transform.constrain_provider_class{class} -> constraint_from_transform resolves the
// `class` NAME (via semantics.hpp provider_class_from_name) to a ProviderClass, with a TYPED reject (UnknownProviderClass) for a
// bogus/empty name; compose_partition_constraint folds it with an assign_provider pin (filter OUTER, pin inner) into ONE
// PartitionConstraint. Device-free. ⛔ the LOAD-BEARING section (e): TWO authored assets (assign_provider + constrain_provider_class)
// COMPOSE into ONE partition decision (§146 schedules-are-assets reaching the partitioner), and the class filter OVERRIDES the pin.
TEST_CASE("ceir 29c-3b: transform.constrain_provider_class resolves a class name to a partition filter, composes with a pin (sec 102)",
          "[ceir][ml][partition]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_all(ctx);
    (void)transform::register_transform_ops(ctx); // the schedule dialect (register_all covers func/resource/ml only)

    // (a) RESOLVE — {class="gpu"} => Resolved, Gpu, op set; AND every ProviderClass name round-trips through the AUTHORED path
    //     (build the directive with provider_class_name(c) -> constraint_from_transform recovers exactly c). Identity per class,
    //     not "some class": Host is Host, External is External.
    {
        const gpu::ClassConstraint cc = gpu::constraint_from_transform(ctx, *build_class_module(ctx, StringView("gpu")));
        REQUIRE(cc.kind == gpu::ClassConstraintKind::Resolved);
        CHECK(cc.provider_class == ProviderClass::Gpu);
        CHECK(cc.op != nullptr);
        for (u8 ci = 0; ci <= static_cast<u8>(ProviderClass::External); ++ci)
        {
            const ProviderClass        c  = static_cast<ProviderClass>(ci);
            const gpu::ClassConstraint rc = gpu::constraint_from_transform(ctx, *build_class_module(ctx, provider_class_name(c)));
            REQUIRE(rc.kind == gpu::ClassConstraintKind::Resolved);
            CHECK(rc.provider_class == c); // the authored name round-trips to the SAME class (identity, all 5)
        }
    }

    // (b) TYPED REJECTS — a bogus/empty `class` name is UnknownProviderClass (op set), NEVER a silent no-constraint (the
    //     graceful-wrong-answer trap: a typo'd class would run unconstrained, leaking to another class). Absent => None.
    {
        const gpu::ClassConstraint bogus = gpu::constraint_from_transform(ctx, *build_class_module(ctx, StringView("bogus_class")));
        CHECK(bogus.kind == gpu::ClassConstraintKind::UnknownProviderClass);
        CHECK(bogus.op != nullptr);
        const gpu::ClassConstraint empty = gpu::constraint_from_transform(ctx, *build_class_module(ctx, StringView("")));
        CHECK(empty.kind == gpu::ClassConstraintKind::UnknownProviderClass);
        CHECK(empty.op != nullptr);
        Module* const nodir = ctx.create_module();
        Block* const  nb    = ctx.create_block(0U);
        nodir->body()->append(nb); // an empty top block, no directive
        const gpu::ClassConstraint none = gpu::constraint_from_transform(ctx, *nodir);
        CHECK(none.kind == gpu::ClassConstraintKind::None);
        CHECK(none.op == nullptr);
    }

    // (c) DUPLICATE DIRECTIVE — two transform.constrain_provider_class ops: find_transform_misuse is DuplicateDirective pointing
    //     at the SECOND op (the 27a module-wide walk owns duplicates; constraint_from_transform assumes a misuse-clean module).
    {
        Module* const tm = ctx.create_module();
        Block* const  b  = ctx.create_block(0U);
        tm->body()->append(b);
        b->append(transform::build_constrain_provider_class(ctx, ctx.attr_string(StringView("gpu"))));
        Operation* const second = transform::build_constrain_provider_class(ctx, ctx.attr_string(StringView("gpu")));
        b->append(second);
        const transform::TransformMisuse mis = transform::find_transform_misuse(ctx, *tm);
        CHECK(mis.kind == transform::TransformMisuseKind::DuplicateDirective);
        CHECK(mis.op == second);
    }

    // (d) COMMITTED ASSET (§146 "the schedule IS an authored asset"): constrain_provider_class_gpu.ceir parse-loads, is CANONICAL
    //     (anti-drift through the PRINTER vs the build_class_module oracle — never file-bytes; the 28 committed-asset scar),
    //     roundtrip-stable, walks clean, and constraint_from_transform RESOLVES it to Gpu. ⛔ if build_class_module changes,
    //     REGENERATE assets/ceir/constrain_provider_class_gpu.ceir (the 22c-3c committed-asset rule).
    {
        Module* const            built   = build_class_module(ctx, StringView("gpu"));
        const containers::String t_built = print(ctx, *built, &root);

        const containers::Array<char> src = test_support::slurp_asset(CRD_REPO_DIR "/assets/ceir/constrain_provider_class_gpu.ceir", ctx);
        const ParseResult             pr  = parse(ctx, StringView(src.data(), src.size()));
        REQUIRE(pr.ok);
        REQUIRE(pr.module != nullptr);

        // ⭐ ANTI-DRIFT: the committed file's canonical print == the oracle's (regenerate the file if build_class_module changes).
        const containers::String t_file = print(ctx, *pr.module, &root);
        CHECK(StringView(t_file.c_str(), t_file.size()) == StringView(t_built.c_str(), t_built.size()));

        // ROUNDTRIP stability: print(parse(file)) re-parses to the identical canonical text (the committed file IS canonical).
        Context ctx2(&root);
        register_all(ctx2);
        (void)transform::register_transform_ops(ctx2);
        const ParseResult pr2 = parse(ctx2, StringView(t_file.c_str(), t_file.size()));
        REQUIRE(pr2.ok);
        const containers::String t_file2 = print(ctx2, *pr2.module, &root);
        CHECK(StringView(t_file2.c_str(), t_file2.size()) == StringView(t_file.c_str(), t_file.size()));

        // well-formed (the directive appears at most once) + it resolves to the Gpu class.
        CHECK(transform::find_transform_misuse(ctx, *pr.module).kind == transform::TransformMisuseKind::None);
        const gpu::ClassConstraint cc = gpu::constraint_from_transform(ctx, *pr.module);
        REQUIRE(cc.kind == gpu::ClassConstraintKind::Resolved);
        CHECK(cc.provider_class == ProviderClass::Gpu);
    }

    // (e) ⭐ COMPOSITION — the FIRST time TWO authored transform assets reach ONE partition decision (§146). assign_provider_coopvec
    //     .ceir (a PIN to "coopvec") + constrain_provider_class_gpu.ceir (a Gpu class FILTER) parse-load separately; their loaders
    //     + compose_partition_constraint fold into ONE PartitionConstraint partition_ml honours. The class filter is OUTER: it
    //     OVERRIDES a pin that names an out-of-class provider. Providers {coopvec(Gpu)@0, external_stub(External)@1}.
    {
        const gpu::MlProvider provs[2] = {coopvec(true), external_stub(true)};
        const auto            span     = ConstSpan<gpu::MlProvider>(provs, 2U);

        const containers::Array<char> pin_src   = test_support::slurp_asset(CRD_REPO_DIR "/assets/ceir/assign_provider_coopvec.ceir", ctx);
        const ParseResult             pin_pr    = parse(ctx, StringView(pin_src.data(), pin_src.size()));
        REQUIRE(pin_pr.ok);
        const containers::Array<char> class_src = test_support::slurp_asset(CRD_REPO_DIR "/assets/ceir/constrain_provider_class_gpu.ceir", ctx);
        const ParseResult             class_pr  = parse(ctx, StringView(class_src.data(), class_src.size()));
        REQUIRE(class_pr.ok);

        const gpu::ProviderPin     pin = gpu::provider_from_transform(ctx, *pin_pr.module, span);
        REQUIRE(pin.kind == gpu::ProviderPinKind::Resolved);
        CHECK(pin.index == 0); // "coopvec" -> index 0 in this span
        const gpu::ClassConstraint cc  = gpu::constraint_from_transform(ctx, *class_pr.module);
        REQUIRE(cc.kind == gpu::ClassConstraintKind::Resolved);
        CHECK(cc.provider_class == ProviderClass::Gpu);

        // COMPOSE the two authored halves -> {has_class=Gpu, pinned=0}. The pin (coopvec @0) IS in the Gpu-filtered set: honoured.
        const gpu::PartitionConstraint pc = gpu::compose_partition_constraint(cc, pin);
        CHECK(pc.has_class);
        CHECK(pc.provider_class == ProviderClass::Gpu);
        CHECK(pc.pinned == 0);

        Module* const m = ctx.create_module();
        build_band24(ctx, *m); // mlp (op0, relu, coopvec-claimable) + attention (op1, coopvec REFUSES)
        const gpu::MlPartition part = gpu::partition_ml(ctx, *m, span, &root, pc);
        REQUIRE(part.assignments.size() == 2U);
        CHECK(part.assignments[0].provider == 0);  // mlp -> coopvec (Gpu, the in-class pin honoured)
        CHECK(part.assignments[1].provider == -1); // attention -> fallback (coopvec refuses; external @1 is FILTERED OUT by Gpu)

        // ⭐ DISCRIMINATOR — the SAME pin, an EXTERNAL class filter: the pin (coopvec @0, Gpu) is now OUT of class, so the filter
        //    IGNORES it and external @1 claims BOTH ops. A pin-first-then-class impl would keep the mlp on coopvec @0 -> this fails.
        const gpu::ClassConstraint     cc_ext{ProviderClass::External, nullptr, gpu::ClassConstraintKind::Resolved};
        const gpu::PartitionConstraint pc_ext = gpu::compose_partition_constraint(cc_ext, pin);
        const gpu::MlPartition         pe      = gpu::partition_ml(ctx, *m, span, &root, pc_ext);
        CHECK(pe.assignments[0].provider == 1); // mlp -> external (the Gpu pin is IGNORED under the External filter)
        CHECK(pe.assignments[1].provider == 1); // attention -> external (always_claims)
    }
}
