// DIAG.8a — authored-source and lowering PROVENANCE (provenance.hpp). One authored CEIR text program is parsed under a
// registered file, optimized by the production passes (CSE through the PassManager), serialized, deserialized into a
// fresh Context, compiled and run. A runtime error, a compile error and a reference-interpreter error each navigate to
// the responsible authored line; the CSE survivor carries BOTH authored origins (many-to-many) through every stage.
// The expected positions come from scanning the text itself, never from the parser. Controls: reformatting the text
// moves every origin but leaves the content and interface hashes unchanged; a builder op without a location reports an
// explicit gap; a corrupt ORIG chunk is rejected; an origin-free module's blob carries no ORIG chunk. ASCII test names.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/pass_manager.hpp>
#include <crd/ceir/passes/canonicalize.hpp>
#include <crd/ceir/passes/cse.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/program_asset.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/ceir/tensor.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
constexpr const char* kFile = "programs/diag/provenance_main.ceir";

void register_dialects(Context& ctx)
{
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)func::register_dialect(ctx);
}

Operation* konst(Context& ctx, Block* b, i64 v)
{
    Operation* const op = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_i32());
    ctx.set_attr(op, "value", ctx.attr_int(v));
    b->append(op);
    return op;
}

Operation* addi(Context& ctx, Block* b, Value* x, Value* y)
{
    Value*           in[2] = {x, y};
    Operation* const op =
        ctx.create_operation(ctx.intern_op("arith", "addi"), ConstSpan<Value*>(in, 2U), 1U, ctx.type_i32());
    b->append(op);
    return op;
}

// @main(%step) -> i32 { %lo = 0; %hi = 3; %a = addi(%hi,%hi); %b = addi(%hi,%hi); %s = addi(%a,%b);
//                       core.for(%lo, %hi, %step) { yield }; return %s }
// %b duplicates %a, so CSE keeps %a as the survivor of BOTH authored addi lines. A zero step faults at the core.for.
Module* build_main(Context& ctx)
{
    Module* const    m  = ctx.create_module();
    Operation* const fn = func::create_func(ctx, *m, "main", Visibility::Public, 1U, ctx.type_i32());
    m->body()->append(ctx.create_block(0U));
    m->body()->first_block()->append(fn);
    Block* const     body = func::func_body_block(fn);
    Operation* const lo   = konst(ctx, body, 0);
    Operation* const hi   = konst(ctx, body, 3);
    Operation* const a    = addi(ctx, body, hi->result(0U), hi->result(0U));
    Operation* const b    = addi(ctx, body, hi->result(0U), hi->result(0U));
    Operation* const s    = addi(ctx, body, a->result(0U), b->result(0U));
    Value*           range[3] = {lo->result(0U), hi->result(0U), body->arg(0U)};
    Operation* const loop =
        ctx.create_operation(ctx.intern_op("core", "for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    Block* const loop_body = ctx.create_block(1U, ctx.type_i32());
    loop->region(0)->append(loop_body);
    loop_body->append(ctx.create_operation(ctx.intern_op("core", "yield"), {}, 0U));
    body->append(loop);
    Value* rv[1] = {s->result(0U)};
    body->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    return m;
}

// One authored op position found by scanning the TEXT (the independent oracle): the 1-based line holding the n-th
// occurrence of `needle`, and the 1-based column of that line's first non-blank character (where an op starts).
struct TextPos
{
    u32 line = 0U;
    u32 col  = 0U;
};
TextPos find_op(StringView text, const char* needle, u32 nth)
{
    const usize nlen = StringView(needle).size();
    u32         line = 1U;
    usize       line_start = 0U;
    u32         seen = 0U;
    for (usize i = 0; i + nlen <= text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            line_start = i + 1U;
            continue;
        }
        if (StringView(text.data() + i, nlen) == StringView(needle))
        {
            if (seen++ == nth)
            {
                usize first = line_start;
                while (text[first] == ' ' || text[first] == '\t')
                {
                    ++first;
                }
                return TextPos{line, static_cast<u32>(first - line_start + 1U)};
            }
        }
    }
    return TextPos{};
}

// Double every line break and indent each line by two more spaces: the same program, every op on a different line.
String reformat(StringView text, memory::IAllocator* alloc)
{
    String out(alloc);
    for (usize i = 0; i < text.size(); ++i)
    {
        out.push_back(text[i]);
        if (text[i] == '\n')
        {
            out.append("\n  ");
        }
    }
    return out;
}

bool contains(const String& s, StringView n)
{
    const StringView hay(s.data(), s.size());
    for (usize i = 0; i + n.size() <= hay.size(); ++i)
    {
        if (StringView(hay.data() + i, n.size()) == n)
        {
            return true;
        }
    }
    return false;
}

// The k-th compiled instr whose dense op is `op` (pre-order over the plan's seqs), as an InstrRef.
plan::InstrRef find_instr(const plan::CompiledPlan& p, plan::Op op, u32 nth)
{
    u32 seen = 0U;
    for (u32 s = 0; s < static_cast<u32>(p.seqs.size()); ++s)
    {
        for (u32 k = 0; k < static_cast<u32>(p.seqs[s].instrs.size()); ++k)
        {
            if (p.seqs[s].instrs[k].op == op && seen++ == nth)
            {
                return plan::InstrRef{s, k};
            }
        }
    }
    return plan::InstrRef{};
}

void check_origin(const Context& ctx, const Origin& o, TextPos at)
{
    CHECK(o.loc.line == at.line);
    CHECK(o.loc.col == at.col);
    CHECK(ctx.file_path(o.loc.file_id) == StringView(kFile));
    CHECK(o.space == OriginSpace::CeirOp);
    CHECK(o.node.valid());
}

// The ORIG chunk's payload offset in `blob` (0 if absent), found by walking the FourCC chunk table.
usize find_chunk(const Array<u8>& blob, u32 fourcc, u32& size_out)
{
    auto rd = [&](usize at) -> u32
    {
        return static_cast<u32>(blob[at]) | (static_cast<u32>(blob[at + 1U]) << 8U) |
               (static_cast<u32>(blob[at + 2U]) << 16U) | (static_cast<u32>(blob[at + 3U]) << 24U);
    };
    const u32 count = rd(8U);
    usize     pos   = 12U;
    for (u32 i = 0; i < count; ++i)
    {
        const u32 cc = rd(pos);
        const u32 sz = rd(pos + 4U);
        if (cc == fourcc)
        {
            size_out = sz;
            return pos + 8U;
        }
        pos += 8U + sz;
    }
    return 0U;
}
constexpr u32 kOrig = static_cast<u32>('O') | (static_cast<u32>('R') << 8U) | (static_cast<u32>('I') << 16U) |
                      (static_cast<u32>('G') << 24U);
} // namespace

TEST_CASE("diag 8a: authored text positions survive CSE, serialization and compilation to a run error", "[ceir][diag]")
{
    memory::GrowableTlsfAllocator root;

    // The authored text: the canonical print of the program (any text the parser accepts would do).
    Context builder_ctx(&root);
    register_dialects(builder_ctx);
    const String     text = print(builder_ctx, *build_main(builder_ctx), &root);
    const StringView src(text.data(), text.size());
    const TextPos    at_a   = find_op(src, "arith.addi", 0U);
    const TextPos    at_b   = find_op(src, "arith.addi", 1U);
    const TextPos    at_s   = find_op(src, "arith.addi", 2U);
    const TextPos    at_for = find_op(src, "core.for", 0U);
    REQUIRE(at_a.line != 0U);
    REQUIRE(at_b.line > at_a.line);
    REQUIRE(at_for.line > at_s.line);

    // Parse under a registered file, then optimize with the production pass driver.
    Context ctx(&root);
    register_dialects(ctx);
    const u32         fid = ctx.register_file(kFile);
    const ParseResult pr  = parse(ctx, src, fid);
    REQUIRE(pr.ok);
    Module& m = *pr.module;
    DiagnosticEngine diag(ctx, &root);
    AnalysisManager  am(&root);
    PassManager      pm(&root);
    pm.add_pass(cse_pass());
    pm.run(ctx, m, am, diag);
    REQUIRE_FALSE(diag.has_errors());

    // Serialize and load into a FRESH Context: provenance must ride the ORIG chunk (file names via SRCM).
    const Array<u8> blob = serialize(ctx, m, &root);
    Context         loaded(&root);
    register_dialects(loaded);
    const ParseResult lr = deserialize(loaded, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(lr.ok);
    CHECK(stable_hash(loaded, *lr.module, &root) == stable_hash(ctx, m, &root));

    const plan::CompileResult cr = plan::compile(loaded, *lr.module, "main", &root);
    REQUIRE(cr.ok());
    CHECK(cr.op == nullptr);

    SECTION("the CSE survivor names both authored addi lines; the consumer names its own")
    {
        const plan::InstrRef survivor = find_instr(cr.plan, plan::Op::AddI, 0U);
        const plan::InstrRef sum      = find_instr(cr.plan, plan::Op::AddI, 1U);
        REQUIRE(survivor.valid());
        REQUIRE(sum.valid());
        CHECK_FALSE(find_instr(cr.plan, plan::Op::AddI, 2U).valid()); // the duplicate is gone

        const Provenance ps = plan::instr_provenance(cr.plan, survivor);
        CHECK(ps.gap == ProvenanceGap::None);
        REQUIRE(ps.origins.size() == 2U);
        check_origin(loaded, ps.origins[0], at_a);
        check_origin(loaded, ps.origins[1], at_b);
        CHECK(ps.origins[0].node == ps.op); // the survivor's own line names the survivor
        CHECK(ps.origins[1].node != ps.op); // the erased duplicate keeps its own identity
        const String r = render_provenance(loaded, ps, &root);
        CHECK(contains(r, StringView(" from ")));

        const Provenance pc = plan::instr_provenance(cr.plan, sum);
        REQUIRE(pc.origins.size() == 1U);
        check_origin(loaded, pc.origins[0], at_s);
        CHECK(pc.origins[0].node == pc.op);
    }

    SECTION("a production run is clean; a zero step faults at the authored core.for line")
    {
        const i64             ok_args[1] = {1};
        const plan::RunResult good       = plan::run(cr.plan, ConstSpan<i64>(ok_args, 1U), &root);
        REQUIRE(good.ok());
        REQUIRE(good.values.size() == 1U);
        CHECK(good.values[0] == 12);
        CHECK_FALSE(good.fault.valid());

        const i64             bad_args[1] = {0};
        const plan::RunResult bad         = plan::run(cr.plan, ConstSpan<i64>(bad_args, 1U), &root);
        CHECK(bad.error == plan::RunError::BadForStep);
        REQUIRE(bad.fault.valid());
        CHECK(cr.plan.seqs[bad.fault.seq].instrs[bad.fault.instr].op == plan::Op::For);
        const Provenance pf = plan::instr_provenance(cr.plan, bad.fault);
        REQUIRE(pf.primary() != nullptr);
        check_origin(loaded, *pf.primary(), at_for);

        String expect(&root);
        expect.append(kFile);
        expect.push_back(':');
        String line_no(&root);
        for (u32 n = at_for.line; n != 0U; n /= 10U)
        {
            line_no.push_back(static_cast<char>('0' + (n % 10U)));
        }
        for (usize i = line_no.size(); i > 0U; --i)
        {
            expect.push_back(line_no.data()[i - 1U]);
        }
        expect.push_back(':');
        CHECK(contains(render_provenance(loaded, pf, &root), StringView(expect.data(), expect.size())));
    }

    SECTION("the reference interpreter's error op resolves to the same authored line")
    {
        exec::Interpreter in(loaded);
        exec::install_builtin_semantics(in);
        const i64              bad_args[1] = {0};
        const exec::ExecResult er          = in.invoke(*lr.module, "main", ConstSpan<i64>(bad_args, 1U));
        CHECK(er.error == exec::ExecError::BadForStep);
        Array<Origin>    storage(&root);
        const Provenance pe = resolve_provenance(loaded, er.op, storage);
        REQUIRE(pe.primary() != nullptr);
        check_origin(loaded, *pe.primary(), at_for);
    }
}

TEST_CASE("diag 8a: invalid text navigates to its line and a compile error to its op", "[ceir][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_dialects(ctx);
    const u32 fid = ctx.register_file(kFile);

    SECTION("a parse error reports line:col and the file")
    {
        const char* const bad = "module {\n  ^bb0:\n    nodialect()\n}\n";
        const ParseResult pr  = parse(ctx, StringView(bad), fid);
        REQUIRE_FALSE(pr.ok);
        CHECK(pr.error_line == 3U);
        CHECK(pr.error_file_id == fid);
        // The column agrees with the byte offset, counted independently here.
        usize line_start = 0U;
        for (usize i = 0; i < pr.error_offset; ++i)
        {
            if (bad[i] == '\n')
            {
                line_start = i + 1U;
            }
        }
        CHECK(pr.error_col == static_cast<u32>(pr.error_offset - line_start + 1U));
    }

    SECTION("an op the plan cannot compile is blamed at its authored line")
    {
        Context bctx(&root);
        register_dialects(bctx);
        Module* const    bm = bctx.create_module();
        Operation* const fn = func::create_func(bctx, *bm, "main", Visibility::Public, 0U, bctx.type_i32());
        bm->body()->append(bctx.create_block(0U));
        bm->body()->first_block()->append(fn);
        Block* const     body = func::func_body_block(fn);
        Operation* const c    = konst(bctx, body, 1);
        Value*           in[1] = {c->result(0U)};
        Operation* const each =
            bctx.create_operation(bctx.intern_op("core", "foreach"), ConstSpan<Value*>(in, 1U), 0U, {}, 1U);
        each->region(0)->append(bctx.create_block(1U, bctx.type_i32()));
        body->append(each);
        Value* rv[1] = {c->result(0U)};
        body->append(func::create_return(bctx, ConstSpan<Value*>(rv, 1U)));
        const String     text = print(bctx, *bm, &root);
        const StringView src(text.data(), text.size());
        const TextPos    at = find_op(src, "core.foreach", 0U);
        REQUIRE(at.line != 0U);

        const ParseResult pr = parse(ctx, src, fid);
        REQUIRE(pr.ok);
        const plan::CompileResult cr = plan::compile(ctx, *pr.module, "main", &root);
        CHECK(cr.error == plan::CompileError::UnsupportedOp);
        REQUIRE(cr.op != nullptr);
        Array<Origin>    storage(&root);
        const Provenance p = resolve_provenance(ctx, cr.op, storage);
        REQUIRE(p.primary() != nullptr);
        check_origin(ctx, *p.primary(), at);
    }

    SECTION("a missing entry has no op to blame")
    {
        Module* const             m  = ctx.create_module();
        const plan::CompileResult cr = plan::compile(ctx, *m, "absent", &root);
        CHECK(cr.error == plan::CompileError::NoEntry);
        CHECK(cr.op == nullptr);
        Array<Origin>    storage(&root);
        const Provenance p = resolve_provenance(ctx, cr.op, storage);
        CHECK(p.gap == ProvenanceGap::NoOperation);
        const String r = render_provenance(ctx, p, &root);
        CHECK(StringView(r.data(), r.size()) == StringView("no-operation"));
    }
}

TEST_CASE("diag 8a: reformatting moves every origin but no content or interface hash", "[ceir][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       bctx(&root);
    register_dialects(bctx);
    const String     text = print(bctx, *build_main(bctx), &root);
    const String     moved = reformat(StringView(text.data(), text.size()), &root);
    const StringView src_a(text.data(), text.size());
    const StringView src_b(moved.data(), moved.size());

    Context ca(&root);
    register_dialects(ca);
    const ParseResult pa = parse(ca, src_a, ca.register_file(kFile));
    Context           cb(&root);
    register_dialects(cb);
    const ParseResult pb = parse(cb, src_b, cb.register_file(kFile));
    REQUIRE(pa.ok);
    REQUIRE(pb.ok);
    CHECK(stable_hash(ca, *pa.module, &root) == stable_hash(cb, *pb.module, &root));
    CHECK(interface_hash(ca, *pa.module, &root) == interface_hash(cb, *pb.module, &root));

    const plan::CompileResult ra = plan::compile(ca, *pa.module, "main", &root);
    const plan::CompileResult rb = plan::compile(cb, *pb.module, "main", &root);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    const Provenance fa = plan::instr_provenance(ra.plan, find_instr(ra.plan, plan::Op::For, 0U));
    const Provenance fb = plan::instr_provenance(rb.plan, find_instr(rb.plan, plan::Op::For, 0U));
    REQUIRE(fa.primary() != nullptr);
    REQUIRE(fb.primary() != nullptr);
    CHECK(fa.primary()->loc.line == find_op(src_a, "core.for", 0U).line);
    CHECK(fb.primary()->loc.line == find_op(src_b, "core.for", 0U).line);
    CHECK(fa.primary()->loc.line != fb.primary()->loc.line);
}

TEST_CASE("diag 8a: unknown attribution is an explicit gap, never a zero location", "[ceir][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    register_dialects(ctx);
    Module* const m = build_main(ctx); // builder ops: no recorded origins, no declared location

    const Array<u8> blob = serialize(ctx, *m, &root);
    u32             orig_size = 0U;
    CHECK(find_chunk(blob, kOrig, orig_size) == 0U); // an origin-free module's blob has no ORIG chunk

    const plan::CompileResult cr = plan::compile(ctx, *m, "main", &root);
    REQUIRE(cr.ok());
    const Provenance p = plan::instr_provenance(cr.plan, find_instr(cr.plan, plan::Op::For, 0U));
    CHECK(p.gap == ProvenanceGap::NoSourceLocation);
    CHECK(p.primary() == nullptr);
    CHECK(p.op.valid()); // the closest-known origin is the op's own stable identity
    CHECK(contains(render_provenance(ctx, p, &root), StringView("no-source-location op#")));
    CHECK(plan::instr_provenance(cr.plan, plan::InstrRef{}).gap == ProvenanceGap::NoOperation);
}

TEST_CASE("diag 8a: a reshape fold carries both reshapes' builder-declared locations", "[ceir][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    (void)tensor::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    const u32    fid     = ctx.register_file(kFile);
    const TypeId f32t    = ctx.type_f32();
    const TypeId din[2]  = {ctx.type_dim_static(8U), ctx.type_dim_static(8U)};
    const TypeId t_in    = ctx.type_tensor(f32t, ctx.type_shape(ConstSpan<TypeId>(din, 2U)));
    const TypeId dmid[1] = {ctx.type_dim_static(64U)};
    const TypeId t_mid   = ctx.type_tensor(f32t, ctx.type_shape(ConstSpan<TypeId>(dmid, 1U)));
    const TypeId dout[2] = {ctx.type_dim_static(4U), ctx.type_dim_static(16U)};
    const TypeId t_out   = ctx.type_tensor(f32t, ctx.type_shape(ConstSpan<TypeId>(dout, 2U)));
    Module* const m      = ctx.create_module();
    Block* const  blk    = ctx.create_block();
    m->body()->append(blk);
    Operation* const x = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, t_in);
    blk->append(x);
    Operation* const r1 = tensor::build_reshape(ctx, x->result(0), t_mid);
    r1->set_loc(SourceLoc{fid, 20U, 5U});
    blk->append(r1);
    Operation* const r2 = tensor::build_reshape(ctx, r1->result(0), t_out);
    r2->set_loc(SourceLoc{fid, 21U, 5U});
    blk->append(r2);
    blk->append(resource::build_export(ctx, r2->result(0)));

    DiagnosticEngine diag(ctx, &root);
    REQUIRE(canonicalize_run(ctx, *m, diag));
    // The export now reads the folded reshape; it is the only reshape with uses.
    const Operation* folded = nullptr;
    for (const Operation* op = blk->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (op->kind() == ctx.intern_op("tensor", "reshape") && op->result(0)->has_uses())
        {
            folded = op;
        }
    }
    REQUIRE(folded != nullptr);
    REQUIRE(folded != r1);
    REQUIRE(folded != r2);
    CHECK(folded->loc().line == 0U); // the fold declares no location of its own (content is untouched)
    Array<Origin>    storage(&root);
    const Provenance p = resolve_provenance(ctx, folded, storage);
    REQUIRE(p.origins.size() == 2U);
    CHECK(p.origins[0].loc.line == 21U); // the outer reshape (the op the fold replaced)
    CHECK(p.origins[0].node == r2->stable_id());
    CHECK(p.origins[1].loc.line == 20U); // the inner reshape it hopped over
    CHECK(p.origins[1].node == r1->stable_id());
    CHECK(r1->stable_id().valid());
    CHECK(r2->stable_id().valid());
}

TEST_CASE("diag 8a: a corrupt ORIG chunk is rejected, never applied", "[ceir][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       bctx(&root);
    register_dialects(bctx);
    const String text = print(bctx, *build_main(bctx), &root);
    Context      ctx(&root);
    register_dialects(ctx);
    const ParseResult pr = parse(ctx, StringView(text.data(), text.size()), ctx.register_file(kFile));
    REQUIRE(pr.ok);
    Array<u8> blob = serialize(ctx, *pr.module, &root);
    u32       size = 0U;
    const usize at = find_chunk(blob, kOrig, size);
    REQUIRE(at != 0U);
    REQUIRE(size >= 12U + 21U); // entry count, op index, origin count, then one 21-byte origin record

    SECTION("an unknown origin space")
    {
        blob[at + 12U] = 0x7FU; // the first origin's space byte (after the entry count, op index and origin count)
        Context           c2(&root);
        const ParseResult r = deserialize(c2, ConstSpan<u8>(blob.data(), blob.size()));
        CHECK_FALSE(r.ok);
        CHECK(StringView(r.error) == StringView("ORIG origin space is unknown"));
    }
    SECTION("an entry count larger than the program")
    {
        blob[at + 3U] = 0x7FU;
        Context           c2(&root);
        const ParseResult r = deserialize(c2, ConstSpan<u8>(blob.data(), blob.size()));
        CHECK_FALSE(r.ok);
        CHECK(StringView(r.error) == StringView("ORIG entry count exceeds the BODY op count"));
    }
}
