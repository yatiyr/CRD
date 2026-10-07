// DIAG.8a — CHIR lowering provenance (lower.hpp, crd/ceir/provenance.hpp). The committed CHIR text program is parsed,
// lowered under its authored file name, optimized by the production CSE pass, serialized, loaded into a fresh Context
// and compiled. Every lowered op, and every compiled plan instr, names the CHIR node it came from
// (OriginSpace::ChirNode with the node's CHIR StableId) at the node's authored line:col; the `core.state` cell names
// BOTH its StateDecl and the StateUpdate folded into it (many-to-many). The committed program's parallel body reads an
// outer capture: the plan's refusal and the reference interpreter's runtime error both name the authored
// `parallel_for` line. Expected positions come from scanning the text, never from the parser. Controls: the
// self-contained variant compiles; the graph projection (no text positions) lowers to the same content and names the
// same CHIR ids with an explicit no-source-location gap; the model's own file numbering is never copied into the
// Context. ASCII test names (ctest by-name).

#include <crd/chir/lower.hpp>
#include <crd/chir/node.hpp>
#include <crd/chir/schema.hpp>
#include <crd/chir/text.hpp>

#include <crd/ceir/binary.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/pass_manager.hpp>
#include <crd/ceir/passes/cse.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/provenance.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using crd::ceir::Context;
using crd::ceir::Module;
using crd::ceir::Operation;
using crd::ceir::Origin;
using crd::ceir::OriginSpace;
using crd::ceir::Provenance;
using crd::ceir::ProvenanceGap;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

constexpr const char* kTextPath  = CRD_REPO_DIR "/assets/chir/event_handler.chir";
constexpr const char* kGraphPath = CRD_REPO_DIR "/assets/chir/event_handler.chirgraph";
constexpr const char* kFile      = "assets/chir/event_handler.chir";
constexpr u32         kModelFile = 7U; // the parse caller's own numbering: must never reach the Context

Array<char> slurp(const char* path, crd::memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    Array<char> buf(a);
    buf.resize(static_cast<usize>(sz), '\0');
    f.read(buf.data(), sz);
    return buf;
}

StringView sv(const Array<char>& b)
{
    return StringView(b.data(), b.size());
}

StringView sv(const String& s)
{
    return StringView(s.data(), s.size());
}

bool contains(StringView hay, StringView n)
{
    for (usize i = 0; i + n.size() <= hay.size(); ++i)
    {
        if (StringView(hay.data() + i, n.size()) == n)
        {
            return true;
        }
    }
    return false;
}

// The independent oracle: the 1-based line holding the first occurrence of `needle` in the text, and the 1-based
// column of that line's first non-blank character (where a CHIR node's keyword starts).
struct TextPos
{
    u32 line = 0U;
    u32 col  = 0U;
};
TextPos find_node(StringView text, const char* needle)
{
    const StringView n(needle);
    u32              line       = 1U;
    usize            line_start = 0U;
    for (usize i = 0; i + n.size() <= text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            line_start = i + 1U;
            continue;
        }
        if (StringView(text.data() + i, n.size()) == n)
        {
            usize first = line_start;
            while (text[first] == ' ' || text[first] == '\t')
            {
                ++first;
            }
            return TextPos{line, static_cast<u32>(first - line_start + 1U)};
        }
    }
    return TextPos{};
}

// The CHIR StableId of the node named `name`.
u64 chir_id(const crd::chir::SourceModel& m, const char* name)
{
    const StringView want(name);
    for (u32 i = 0; i < m.node_count(); ++i)
    {
        if (m.str(m.node(i).name) == want)
        {
            return m.node(i).id.value;
        }
    }
    return 0U;
}

// The authored nodes of event_handler.chir: CHIR id plus the text position found by scanning.
struct Authored
{
    u64     id = 0U;
    TextPos at{};
};
struct Program
{
    Authored handler;
    Authored pfor;
    Authored await;
    Authored decl;
    Authored update;
};
Program authored(const crd::chir::SourceModel& m, StringView text)
{
    Program p;
    p.handler = Authored{chir_id(m, "on_tick"), find_node(text, "event_handler on_tick")};
    p.pfor    = Authored{chir_id(m, "update"), find_node(text, "parallel_for update")};
    p.await   = Authored{chir_id(m, "task"), find_node(text, "await task")};
    p.decl    = Authored{chir_id(m, "world_state"), find_node(text, "state_decl world_state")};
    p.update  = Authored{chir_id(m, "commit"), find_node(text, "state_update commit")};
    return p;
}

void check_origin(const Context& ctx, const Origin& o, const Authored& want)
{
    CHECK(o.space == OriginSpace::ChirNode);
    CHECK(o.node.value == want.id);
    CHECK(o.loc.line == want.at.line);
    CHECK(o.loc.col == want.at.col);
    CHECK(ctx.file_path(o.loc.file_id) == StringView(kFile));
}

void register_lowered_dialects(Context& ctx)
{
    (void)crd::ceir::func::register_dialect(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::task::register_task_ops(ctx);
    (void)crd::ceir::async::register_async_ops(ctx);
}

// Every op under `r` in pre-order.
void gather(crd::ceir::Region* r, Array<Operation*>& out)
{
    if (r == nullptr)
    {
        return;
    }
    for (crd::ceir::Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            out.push_back(op);
            for (u32 k = 0; k < op->num_regions(); ++k)
            {
                gather(op->region(k), out);
            }
        }
    }
}

// The first op of `dialect.name` in `ops`.
Operation* find_kind(Context& ctx, const Array<Operation*>& ops, const char* dialect, const char* name)
{
    const crd::ceir::OpId kind = ctx.intern_op(dialect, name);
    for (usize i = 0; i < ops.size(); ++i)
    {
        if (ops[i]->kind() == kind)
        {
            return ops[i];
        }
    }
    return nullptr;
}

// The first compiled instr whose dense op is `op` (pre-order over the plan's seqs).
crd::ceir::plan::InstrRef find_instr(const crd::ceir::plan::CompiledPlan& p, crd::ceir::plan::Op op)
{
    for (u32 s = 0; s < static_cast<u32>(p.seqs.size()); ++s)
    {
        for (u32 k = 0; k < static_cast<u32>(p.seqs[s].instrs.size()); ++k)
        {
            if (p.seqs[s].instrs[k].op == op)
            {
                return crd::ceir::plan::InstrRef{s, k};
            }
        }
    }
    return crd::ceir::plan::InstrRef{};
}
// Lower `model` under kFile, optimize it with the production CSE pass, serialize it and load it into `loaded` (whose
// dialects this registers). The lowering Context is gone by the time `loaded` is used: only the blob carries origins.
Module* lower_optimize_reload(const crd::chir::SourceModel& model, Context& loaded, crd::memory::IAllocator* root)
{
    Context       ctx(root);
    Module* const mod = crd::chir::lower_chir(model, ctx, kFile);
    REQUIRE(mod != nullptr);
    crd::ceir::DiagnosticEngine diag(ctx, root);
    crd::ceir::AnalysisManager  am(root);
    crd::ceir::PassManager      pm(root);
    pm.add_pass(crd::ceir::cse_pass());
    pm.run(ctx, *mod, am, diag);
    REQUIRE_FALSE(diag.has_errors());

    const Array<crd::u8> blob = crd::ceir::serialize(ctx, *mod, root);
    register_lowered_dialects(loaded);
    const crd::ceir::ParseResult lr = crd::ceir::deserialize(loaded, ConstSpan<crd::u8>(blob.data(), blob.size()));
    REQUIRE(lr.ok);
    CHECK(crd::ceir::stable_hash(loaded, *lr.module, root) == crd::ceir::stable_hash(ctx, *mod, root));
    return lr.module;
}

// `text` with the first occurrence of `cut` removed (the same program minus one binding; no line moves).
Array<char> without(const Array<char>& text, const char* cut, crd::memory::IAllocator* a)
{
    const StringView c(cut);
    Array<char>      out(a);
    bool             done = false;
    for (usize i = 0; i < text.size(); ++i)
    {
        if (!done && i + c.size() <= text.size() && StringView(text.data() + i, c.size()) == c)
        {
            i += c.size() - 1U;
            done = true;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}
} // namespace

TEST_CASE("diag 8a: every CHIR-lowered op names its authored CHIR node and line", "[chir][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  text = slurp(kTextPath, &root);
    crd::chir::SourceModel             model(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), kModelFile, model).ok);
    const Program want = authored(model, sv(text));
    REQUIRE(want.pfor.at.line != 0U);
    REQUIRE(want.update.at.line > want.pfor.at.line);

    Context ctx(&root);
    // Occupy file ids up to the model's own numbering with other files: copying the model's id would alias one of them.
    for (u32 i = 0; i < kModelFile; ++i)
    {
        const char name[2] = {static_cast<char>('a' + i), '\0'};
        (void)ctx.register_file(StringView(name));
    }
    Module* const mod = crd::chir::lower_chir(model, ctx, kFile);
    REQUIRE(mod != nullptr);

    Array<Operation*> ops(&root);
    gather(mod->body(), ops);
    REQUIRE(ops.size() > 10U);
    Array<Origin> storage(&root);
    for (usize i = 0; i < ops.size(); ++i) // completeness: no lowered op is left without an authored origin
    {
        const Provenance p = crd::ceir::resolve_provenance(ctx, ops[i], storage);
        CHECK(p.gap == ProvenanceGap::None);
        REQUIRE(p.origins.size() != 0U);
        CHECK(p.origins[0].space == OriginSpace::ChirNode);
    }

    struct Expect
    {
        const char*     dialect;
        const char*     name;
        const Authored* node;
    };
    const Expect single[] = {
        {"func", "func", &want.handler},  {"func", "return", &want.handler}, {"task", "parallel_for", &want.pfor},
        {"async", "launch", &want.await}, {"async", "await", &want.await},
    };
    for (const Expect& e : single)
    {
        Operation* const op = find_kind(ctx, ops, e.dialect, e.name);
        REQUIRE(op != nullptr);
        const Provenance p = crd::ceir::resolve_provenance(ctx, op, storage);
        REQUIRE(p.origins.size() == 1U);
        check_origin(ctx, p.origins[0], *e.node);
    }

    // The parallel body's yield and the bounds belong to the ParallelFor; the launch body's const to the Await.
    Operation* const pf = find_kind(ctx, ops, "task", "parallel_for");
    REQUIRE(pf != nullptr);
    const Provenance py = crd::ceir::resolve_provenance(ctx, pf->region(0)->first_block()->last_op(), storage);
    REQUIRE(py.origins.size() == 1U);
    check_origin(ctx, py.origins[0], want.pfor);

    // Many-to-many: the cell is the declaration plus the `update state` folded into it.
    Operation* const cell = find_kind(ctx, ops, "core", "state");
    REQUIRE(cell != nullptr);
    const Provenance pc = crd::ceir::resolve_provenance(ctx, cell, storage);
    REQUIRE(pc.origins.size() == 2U);
    check_origin(ctx, pc.origins[0], want.decl);
    check_origin(ctx, pc.origins[1], want.update);
    const String r = crd::ceir::render_provenance(ctx, pc, &root);
    CHECK(contains(sv(r), StringView(kFile)));
    CHECK(contains(sv(r), StringView(" from ")));
    CHECK(contains(sv(r), StringView(" chir#")));
}

TEST_CASE("diag 8a: the committed program's outer capture is rejected at its authored parallel_for line",
          "[chir][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  text = slurp(kTextPath, &root);
    crd::chir::SourceModel             model(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), kModelFile, model).ok);
    const Program want = authored(model, sv(text));

    // The parallel body reads the query view, an outer value (ADR-0128 D2): the plan refuses the capture. The refusal
    // is blamed on the authored `parallel_for update` line after CSE, serialization and a fresh-Context load.
    Context       loaded(&root);
    Module* const mod = lower_optimize_reload(model, loaded, &root);
    REQUIRE(mod != nullptr);
    const crd::ceir::plan::CompileResult cr = crd::ceir::plan::compile(loaded, *mod, "on_tick", &root);
    CHECK(cr.error == crd::ceir::plan::CompileError::CapturedValue);
    REQUIRE(cr.op != nullptr);
    Array<Origin>    storage(&root);
    const Provenance p = crd::ceir::resolve_provenance(loaded, cr.op, storage);
    REQUIRE(p.origins.size() == 1U);
    check_origin(loaded, p.origins[0], want.pfor);

    // The reference interpreter's runtime error names the same authored node.
    crd::ceir::exec::Interpreter in(loaded);
    crd::ceir::exec::install_builtin_semantics(in);
    crd::ceir::exec::install_async_semantics(in);
    crd::ceir::exec::install_task_semantics(in);
    const crd::i64                    args[2] = {0, 0};
    const crd::ceir::exec::ExecResult er      = in.invoke(*mod, "on_tick", ConstSpan<crd::i64>(args, 2U));
    CHECK(er.error == crd::ceir::exec::ExecError::UndefinedValue);
    const Provenance pe = crd::ceir::resolve_provenance(loaded, er.op, storage);
    REQUIRE(pe.origins.size() == 1U);
    check_origin(loaded, pe.origins[0], want.pfor);
}

TEST_CASE("diag 8a: a self-contained CHIR program compiles and every plan instr names its node", "[chir][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  committed = slurp(kTextPath, &root);
    const Array<char>                  text      = without(committed, " = q.entities", &root);
    REQUIRE(text.size() < committed.size());
    crd::chir::SourceModel model(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), kModelFile, model).ok);
    const Program want = authored(model, sv(text));

    Context       loaded(&root);
    Module* const mod = lower_optimize_reload(model, loaded, &root);
    REQUIRE(mod != nullptr);
    const crd::ceir::plan::CompileResult cr = crd::ceir::plan::compile(loaded, *mod, "on_tick", &root);
    REQUIRE(cr.ok());
    CHECK(cr.op == nullptr);

    // Every compiled instr navigates to an authored CHIR node in the authored file.
    usize instrs = 0U;
    for (u32 s = 0; s < static_cast<u32>(cr.plan.seqs.size()); ++s)
    {
        for (u32 k = 0; k < static_cast<u32>(cr.plan.seqs[s].instrs.size()); ++k)
        {
            const Provenance p = crd::ceir::plan::instr_provenance(cr.plan, crd::ceir::plan::InstrRef{s, k});
            REQUIRE(p.primary() != nullptr);
            CHECK(p.primary()->space == OriginSpace::ChirNode);
            CHECK(loaded.file_path(p.primary()->loc.file_id) == StringView(kFile));
            ++instrs;
        }
    }
    CHECK(instrs > 5U);

    const Provenance pc = crd::ceir::plan::instr_provenance(cr.plan, find_instr(cr.plan, crd::ceir::plan::Op::State));
    REQUIRE(pc.origins.size() == 2U);
    check_origin(loaded, pc.origins[0], want.decl);
    check_origin(loaded, pc.origins[1], want.update);

    const Provenance pp =
        crd::ceir::plan::instr_provenance(cr.plan, find_instr(cr.plan, crd::ceir::plan::Op::ParallelFor));
    REQUIRE(pp.origins.size() == 1U);
    check_origin(loaded, pp.origins[0], want.pfor);

    const Provenance pa = crd::ceir::plan::instr_provenance(cr.plan, find_instr(cr.plan, crd::ceir::plan::Op::Await));
    REQUIRE(pa.origins.size() == 1U);
    check_origin(loaded, pa.origins[0], want.await);
}

TEST_CASE("diag 8a: graph-authored CHIR names its node ids with an explicit gap and the same content", "[chir][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  text = slurp(kTextPath, &root);
    crd::chir::SourceModel             from_text(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), kModelFile, from_text).ok);
    const Array<char>      graph = slurp(kGraphPath, &root);
    crd::chir::SourceModel from_graph(&root);
    REQUIRE(crd::chir::read_schema(sv(graph), from_graph));

    Context       ct(&root);
    Module* const mt = crd::chir::lower_chir(from_text, ct, kFile);
    Context       cg(&root);
    Module* const mg = crd::chir::lower_chir(from_graph, cg, StringView("assets/chir/event_handler.chirgraph"));
    REQUIRE(mt != nullptr);
    REQUIRE(mg != nullptr);

    // Provenance is not content: the two projections print and hash identically although their origins differ.
    CHECK(sv(crd::ceir::print(ct, *mt, &root)) == sv(crd::ceir::print(cg, *mg, &root)));
    CHECK(crd::ceir::stable_hash(ct, *mt, &root) == crd::ceir::stable_hash(cg, *mg, &root));

    Array<Operation*> tops(&root);
    Array<Operation*> gops(&root);
    gather(mt->body(), tops);
    gather(mg->body(), gops);
    REQUIRE(tops.size() == gops.size());
    Array<Origin> ts(&root);
    Array<Origin> gs(&root);
    for (usize i = 0; i < gops.size(); ++i)
    {
        const Provenance pt = crd::ceir::resolve_provenance(ct, tops[i], ts);
        const Provenance pg = crd::ceir::resolve_provenance(cg, gops[i], gs);
        CHECK(pt.gap == ProvenanceGap::None);
        CHECK(pg.gap == ProvenanceGap::NoSourceLocation);
        CHECK(pg.primary() == nullptr);
        REQUIRE(pg.origins.size() == pt.origins.size());
        for (usize k = 0; k < pg.origins.size(); ++k)
        {
            CHECK(pg.origins[k].space == OriginSpace::ChirNode);
            CHECK(pg.origins[k].node == pt.origins[k].node); // both projections name the same CHIR identity
            CHECK(pg.origins[k].loc.line == 0U);
            CHECK(pg.origins[k].loc.file_id == 0U);
        }
    }

    // The rendered gap still names the closest-known authored node.
    Operation* const cell = find_kind(cg, gops, "core", "state");
    REQUIRE(cell != nullptr);
    const String r = crd::ceir::render_provenance(cg, crd::ceir::resolve_provenance(cg, cell, gs), &root);
    String       expect(&root);
    expect.append("chir#");
    u64   id = chir_id(from_graph, "world_state");
    char  digits[20];
    usize n = 0U;
    do
    {
        digits[n++] = static_cast<char>('0' + (id % 10U));
        id /= 10U;
    } while (id != 0U);
    while (n > 0U)
    {
        expect.push_back(digits[--n]);
    }
    CHECK(contains(sv(r), StringView("no-source-location op#")));
    CHECK(contains(sv(r), sv(expect)));
}

TEST_CASE("diag 8a: a lowering without a file name keeps line and column with an unknown file", "[chir][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  text = slurp(kTextPath, &root);
    crd::chir::SourceModel             model(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), kModelFile, model).ok);
    const Program want = authored(model, sv(text));

    Context ctx(&root);
    (void)ctx.register_file(StringView("unrelated.ceir")); // file id 1 exists and must not be claimed
    Module* const mod = crd::chir::lower_chir(model, ctx);
    REQUIRE(mod != nullptr);
    Array<Operation*> ops(&root);
    gather(mod->body(), ops);
    Operation* const pf = find_kind(ctx, ops, "task", "parallel_for");
    REQUIRE(pf != nullptr);
    Array<Origin>    storage(&root);
    const Provenance p = crd::ceir::resolve_provenance(ctx, pf, storage);
    REQUIRE(p.origins.size() == 1U);
    CHECK(p.origins[0].loc.file_id == 0U);
    CHECK(p.origins[0].loc.line == want.pfor.at.line);
    CHECK(p.origins[0].loc.col == want.pfor.at.col);
    CHECK(contains(sv(crd::ceir::render_provenance(ctx, p, &root)), StringView("<unknown>:")));
}
