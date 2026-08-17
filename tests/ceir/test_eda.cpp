// CEIR-9e (UNIVERSALITY VALIDATION, U5 PCB/EDA + external provider, U-§87/U-§35/U-§58/U-§34). ⛔ A PROOF, not a
// feature: a PCB/EDA board pipeline (board_document -> drc -> route -> gerber_export) runs on the foundation with ZERO
// new machinery, and its EXTERNAL TOOLS are ordinary CAPABILITY-bearing ops. ⭐ U-§34 is the headline: a COMPLETE EDA
// pipeline expresses itself in ZERO render-flavored vocabulary — every effect family / domain / capability it needs
// already existed for documents, constraints, and external tools. The mock `eda` dialect is INLINE-registered (zero
// central edits). Four proofs: (1) the external tools require `external.process`/`file.write` (8f) and a GPU-only host
// CANNOT run the pipeline (the host-grant contract); (2) each external tool declares a queryable TYPED CONTRACT
// (effects + determinism + capabilities + a `tool` provenance attr — content-addressed, no hard-coded knowledge);
// (3) DRC = a design rule enforced by the board's verifier at commit (the 9d constraint discipline) — ⛔ naming the
// op-LOCAL commit-verify boundary; (4) a program-level walk proves the pipeline is DOMAIN-NEUTRAL (no GPUCommand
// effect, no DeviceTime domain, no gpu.compute capability). Host-only. ASCII names.

#include <crd/ceir/ceir.hpp>   // umbrella: context/ir/dialect/transaction/diagnostic/binary
#include <crd/ceir/binary.hpp> // serialize / stable_hash
#include <crd/ceir/effect.hpp> // EffectRecord / EffectFamily
#include <crd/ceir/semantics.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;
using crd::u64;
using crd::u8;
using crd::usize;
using ByteArray = Array<u8>;

namespace
{
// The DRC design rules ARE the board's verifier (op-LOCAL: commit-verify runs on the MODIFIED op, so a board rule must
// ride the board — a whole-module DRC sweep is a find_*_violation walk / a pass, named-forward). Rules: clearance >= 5,
// trace width >= 3.
bool verify_board(const Context& ctx, const Operation& op) noexcept
{
    const AttrId cl = op.attr("clearance");
    const AttrId tw = op.attr("trace_width");
    const i64    clearance = cl.valid() ? ctx.attr_value(cl).i : 999;
    const i64    trace     = tw.valid() ? ctx.attr_value(tw).i : 999;
    return clearance >= 5 && trace >= 3;
}
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
[[nodiscard]] bool has_cap(const Array<CapabilityId>& s, CapabilityId c) noexcept
{
    for (usize i = 0; i < s.size(); ++i)
    {
        if (s[i] == c) { return true; }
    }
    return false;
}
[[nodiscard]] bool op_has_effect(const Context& ctx, OpId k, EffectFamily f) noexcept
{
    const ConstSpan<EffectRecord> e = ctx.op_effects(k);
    for (usize i = 0; i < e.size(); ++i)
    {
        if (e[i].family == f) { return true; }
    }
    return false;
}

struct EdaOps
{
    OpId board, drc, route, gerber;
};
// The mock `eda` dialect — INLINE-registered (zero central edits). board (design params + the DRC verifier); drc (a
// ConstraintRead stage); route (an EXTERNAL router: ExternalCall + Document r/w, nondeterministic, external.process);
// gerber (an EXTERNAL file export: FileIO + ExternalCall, external.process + file.write). ⛔ explicit determinism+domain.
EdaOps register_eda(Context& ctx)
{
    Dialect* const     d          = ctx.register_dialect("eda");
    const EffectRecord board_e[2] = {EffectRecord{EffectFamily::DocumentRead}, EffectRecord{EffectFamily::ConstraintRead}};
    const EffectRecord drc_e[2]   = {EffectRecord{EffectFamily::ConstraintRead}, EffectRecord{EffectFamily::DocumentRead}};
    const EffectRecord route_e[3] = {EffectRecord{EffectFamily::ExternalCall}, EffectRecord{EffectFamily::DocumentRead},
                                     EffectRecord{EffectFamily::DocumentWrite}};
    const EffectRecord gerb_e[3]  = {EffectRecord{EffectFamily::FileIO}, EffectRecord{EffectFamily::ExternalCall},
                                     EffectRecord{EffectFamily::DocumentRead}};
    const StringView   route_c[1] = {StringView{"external.process"}};
    const StringView   gerb_c[2]  = {StringView{"external.process"}, StringView{"file.write"}};
    EdaOps             o{};
    o.board  = d->register_op("board_document", OpSpec{.verify      = &verify_board,
                                                       .effects     = ConstSpan<EffectRecord>(board_e, 2U),
                                                       .determinism = DeterminismClass::BitExact,
                                                       .domain      = EvalDomain::CookTime});
    o.drc    = d->register_op("drc", OpSpec{.effects     = ConstSpan<EffectRecord>(drc_e, 2U),
                                            .determinism = DeterminismClass::BitExact,
                                            .domain      = EvalDomain::CookTime});
    o.route  = d->register_op("route", OpSpec{.effects      = ConstSpan<EffectRecord>(route_e, 3U),
                                              .determinism  = DeterminismClass::ExternalNondeterminism, // a router may reorder
                                              .domain       = EvalDomain::OfflineTime,
                                              .capabilities = ConstSpan<StringView>(route_c, 1U)});
    o.gerber = d->register_op("gerber_export", OpSpec{.effects      = ConstSpan<EffectRecord>(gerb_e, 3U),
                                                      .determinism  = DeterminismClass::DeterministicWithinTarget,
                                                      .domain       = EvalDomain::OfflineTime,
                                                      .capabilities = ConstSpan<StringView>(gerb_c, 2U)});
    return o;
}
// board_document(clearance=10, trace_width=5) -> drc -> route[tool=mock_router] -> gerber_export[tool=gerber_writer].
Module* build_pipeline(Context& ctx, const EdaOps& o, Operation** board_out, Operation** route_out)
{
    Module* const    m   = ctx.create_module();
    Block* const     top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const board = ctx.create_operation(o.board, {}, 1U, ctx.type_i64());
    ctx.set_attr(board, "clearance", ctx.attr_int(10));
    ctx.set_attr(board, "trace_width", ctx.attr_int(5));
    top->append(board);
    Value* const     drc_in[1] = {board->result(0)};
    Operation* const drc       = ctx.create_operation(o.drc, ConstSpan<Value*>(drc_in, 1U), 1U, ctx.type_i64());
    top->append(drc);
    Value* const     rt_in[1] = {drc->result(0)};
    Operation* const route    = ctx.create_operation(o.route, ConstSpan<Value*>(rt_in, 1U), 1U, ctx.type_i64());
    ctx.set_attr(route, "tool", ctx.attr_string("mock_router")); // provenance: WHICH external binary (module content)
    top->append(route);
    Value* const     gb_in[1] = {route->result(0)};
    Operation* const gerber   = ctx.create_operation(o.gerber, ConstSpan<Value*>(gb_in, 1U), 1U, ctx.type_i64());
    ctx.set_attr(gerber, "tool", ctx.attr_string("gerber_writer"));
    ctx.set_attr(gerber, "path", ctx.attr_string("board.gbr"));
    top->append(gerber);
    if (board_out != nullptr) { *board_out = board; }
    if (route_out != nullptr) { *route_out = route; }
    return m;
}
} // namespace

TEST_CASE("ceir 9e: the external tools require host capabilities a GPU-only host cannot grant", "[ceir][eda]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const EdaOps                 o = register_eda(ctx);
    Module* const                m = build_pipeline(ctx, o, nullptr, nullptr);

    Array<CapabilityId> prog(&root);
    ctx.program_capabilities(*m, prog);
    const CapabilityId ext  = ctx.intern_capability("external.process");
    const CapabilityId file = ctx.intern_capability("file.write");
    CHECK(prog.size() == 2U); // exactly external.process + file.write (sorted-unique; compared as a SET)
    CHECK(has_cap(prog, ext));
    CHECK(has_cap(prog, file));

    // a host that grants only gpu.compute CANNOT run the pipeline; a host granting the external caps CAN.
    const CapabilityId gpu_only[1] = {ctx.intern_capability("gpu.compute")};
    const CapabilityId eda_host[2] = {ext, file};
    CHECK_FALSE(Context::capabilities_satisfied(ConstSpan<CapabilityId>(prog.data(), prog.size()),
                                                ConstSpan<CapabilityId>(gpu_only, 1U)));
    CHECK(Context::capabilities_satisfied(ConstSpan<CapabilityId>(prog.data(), prog.size()),
                                          ConstSpan<CapabilityId>(eda_host, 2U)));

    // EMPTY≠UNKNOWN: a SEPARATE module with an UNREGISTERED op contributes external.process (maximally-capable).
    Module* const    m2  = ctx.create_module();
    Block* const     b2  = ctx.create_block(0U);
    m2->body()->append(b2);
    b2->append(ctx.create_operation(ctx.intern_op("plugin", "mystery"), {}, 0U)); // never register_op'd
    Array<CapabilityId> prog2(&root);
    ctx.program_capabilities(*m2, prog2);
    CHECK(has_cap(prog2, ext)); // external.process, because an unregistered op is treated as maximally-capable
}

TEST_CASE("ceir 9e: each external tool declares a queryable typed contract with content-addressed provenance", "[ceir][eda]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const EdaOps                 o = register_eda(ctx);
    Operation*                   route = nullptr;
    Module* const                m     = build_pipeline(ctx, o, nullptr, &route);

    // capabilities: queried generically, zero hard-coded op knowledge.
    CHECK(ctx.op_capabilities(o.route).size() == 1U);
    CHECK(ctx.op_capabilities(o.gerber).size() == 2U);
    // effects: the external tools declare ExternalCall / FileIO / Document families.
    CHECK(op_has_effect(ctx, o.route, EffectFamily::ExternalCall));
    CHECK(op_has_effect(ctx, o.route, EffectFamily::DocumentWrite));
    CHECK(op_has_effect(ctx, o.gerber, EffectFamily::FileIO));
    CHECK(op_has_effect(ctx, o.gerber, EffectFamily::ExternalCall));
    // determinism metadata: a router is externally nondeterministic; a Gerber export is deterministic per target.
    CHECK(ctx.op_determinism(o.route) == DeterminismClass::ExternalNondeterminism);
    CHECK(ctx.op_determinism(o.gerber) == DeterminismClass::DeterministicWithinTarget);
    // provenance: the `tool` attr names WHICH external binary — and it is module CONTENT (feeds the content hash).
    CHECK(route->attr("tool") == ctx.attr_string("mock_router"));
    const u64 h1 = stable_hash(ctx, *m, &root);
    ctx.set_attr(route, "tool", ctx.attr_string("other_router")); // a re-route with a DIFFERENT router...
    const u64 h2 = stable_hash(ctx, *m, &root);
    CHECK(h1 != h2); // ...is a DIFFERENT program (the external-tool identity is content-addressed)
}

TEST_CASE("ceir 9e: a design-rule violation is rejected by the board verifier at commit", "[ceir][eda]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const EdaOps                 o = register_eda(ctx);
    Operation*                   board = nullptr;
    Module* const                m     = build_pipeline(ctx, o, &board, nullptr);

    // DRC = the board's verifier (clearance >= 5). ⛔ op-LOCAL: the rule fires because the BOARD (its carrier) is the
    // modified op in the touched-set; a whole-module DRC sweep is a find_*_violation walk / a pass (named-forward).
    const ByteArray before = serialize(ctx, *m, &root);
    {
        DiagnosticEngine diag(ctx, &root);
        Transaction      tx(ctx, *m, diag, &root);
        REQUIRE(tx.set_attr(board, "clearance", ctx.attr_int(2))); // 2 < the 5 minimum -> a design-rule violation
        CHECK_FALSE(tx.commit());
        bool verify_failed = false;
        for (usize i = 0; i < diag.count(); ++i)
        {
            if (diag.at(i).code == make_diagnostic_code("ceir.transaction.verify_failed")) { verify_failed = true; }
        }
        CHECK(verify_failed);
    }
    const ByteArray after = serialize(ctx, *m, &root);
    CHECK(blob_eq(before, after));                     // the rejected edit rolled back byte-identically
    CHECK(board->attr("clearance") == ctx.attr_int(10)); // the design-rule-satisfying original restored

    // a rule-satisfying edit (clearance 10 -> 8, still >= 5) commits.
    DiagnosticEngine diag2(ctx, &root);
    Transaction      tx2(ctx, *m, diag2, &root);
    REQUIRE(tx2.set_attr(board, "clearance", ctx.attr_int(8)));
    CHECK(tx2.commit());
}

TEST_CASE("ceir 9e: a complete EDA pipeline uses zero render-flavored vocabulary (domain-neutral)", "[ceir][eda]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const EdaOps                 o = register_eda(ctx);
    Module* const                m = build_pipeline(ctx, o, nullptr, nullptr);

    // ⭐ U-§34, asserted at PROGRAM level (an auditor's walk, not a per-registration tautology): union every op's effect
    // families + domains across the whole module.
    bool has_document = false;
    bool has_constraint = false;
    bool has_external = false;
    bool has_fileio = false;
    bool has_gpu_effect = false;
    bool has_device_domain = false;
    for (Operation* op = m->body()->first_block()->first_op(); op != nullptr; op = op->next_in_block())
    {
        const ConstSpan<EffectRecord> effects = ctx.op_effects(op->kind());
        for (usize i = 0; i < effects.size(); ++i)
        {
            const EffectFamily f = effects[i].family;
            if (f == EffectFamily::DocumentRead || f == EffectFamily::DocumentWrite) { has_document = true; }
            if (f == EffectFamily::ConstraintRead || f == EffectFamily::ConstraintWrite) { has_constraint = true; }
            if (f == EffectFamily::ExternalCall) { has_external = true; }
            if (f == EffectFamily::FileIO) { has_fileio = true; }
            if (f == EffectFamily::GPUCommand) { has_gpu_effect = true; }
        }
        if (ctx.op_domain(op->kind()) == EvalDomain::DeviceTime) { has_device_domain = true; }
    }
    // the pipeline NEEDED document / constraint / external-call / file-IO vocabulary -- all of which already existed...
    CHECK(has_document);
    CHECK(has_constraint);
    CHECK(has_external);
    CHECK(has_fileio);
    // ...and it used NO render/GPU vocabulary: no GPUCommand effect, no DeviceTime domain, no gpu.compute capability.
    CHECK_FALSE(has_gpu_effect);
    CHECK_FALSE(has_device_domain);
    Array<CapabilityId> prog(&root);
    ctx.program_capabilities(*m, prog);
    CHECK_FALSE(has_cap(prog, ctx.intern_capability("gpu.compute")));
}
