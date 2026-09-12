// test_chir_lower.cpp — CEIR-32d: the CHIR->CEIR lowering + the TEXT/GRAPH PARITY gate (ADR-0128 D1/D5).
//
// The runnable §143 proof's core claim: the SAME program authored two ways lowers to the SAME CEIR. So we lower BOTH
// committed projections — the text `.chir` (via parse_chir) AND the graph `.chirgraph` (via read_schema) — and assert
// the CEIR modules print BYTE-IDENTICAL (printer-canonical, §180 #13), each is structure-verifier-clean, and each
// round-trips through the CEIR text parser. Plus the D1 node->op map is present, and the D2 query-view read path (the
// parallel body referencing the func's view param) is structurally sound (find_structure_error == None).

#include <crd/chir/lower.hpp>
#include <crd/chir/node.hpp>
#include <crd/chir/schema.hpp>
#include <crd/chir/text.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/program_asset.hpp> // CEIR-32e: interface_hash / contract_hash / collect_state_schema (the reload gate)

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp> // CEIR-35b: ConstSpan<u8> seed/mutant view for the parse_chir fuzz
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

namespace
{
using crd::containers::Array;
using crd::containers::String;
using crd::containers::StringView;

Array<char> slurp(const char* path, crd::memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    Array<char> buf(a);
    buf.resize(static_cast<crd::usize>(sz), '\0');
    f.read(buf.data(), sz);
    return buf;
}

StringView sv(const Array<char>& b) { return StringView(b.data(), b.size()); }
StringView sv(const String& s) { return StringView(s.data(), s.size()); }

// does the printed CEIR contain the substring `needle` (a coarse "the op landed" check)?
bool contains(StringView hay, const char* needle)
{
    const StringView n(needle);
    if (n.size() > hay.size()) { return false; }
    for (crd::usize i = 0; i + n.size() <= hay.size(); ++i)
    {
        bool eq = true;
        for (crd::usize j = 0; j < n.size(); ++j)
        {
            if (hay[i + j] != n[j]) { eq = false; break; }
        }
        if (eq) { return true; }
    }
    return false;
}

constexpr const char* kGraphPath = CRD_REPO_DIR "/assets/chir/event_handler.chirgraph";
constexpr const char* kTextPath  = CRD_REPO_DIR "/assets/chir/event_handler.chir";

// ── CEIR-32e helpers: build the §143 program programmatically (the committed .chir shape) with knobs for the reload edits.
// ⛔ derive_ids() is called at the end — WITHOUT it every node id is 0, every cell hashes to slot 1, and the pin degrades to
// positional (the reorder test would then pass for the WRONG reason). The knobs: two_cells adds a second `state`; swap_decls
// declares them in the opposite order (a reorder); rename_ws renames the first cell; extra_component grows the query (a
// SIGNATURE change); extra_await adds a body statement (a BODY edit).
void build143(crd::chir::SourceModel& m, bool two_cells, bool swap_decls, bool rename_ws, bool extra_component,
              bool extra_await, bool two_handlers = false)
{
    using NK                 = crd::chir::NodeKind;
    using PD                 = crd::chir::PinDir;
    const crd::u32   prog    = m.add_node(NK::Program, StringView("event_demo"), crd::chir::kInvalidNode);
    const StringView ws_name = rename_ws ? StringView("game_state") : StringView("world_state");
    crd::u32         ws      = crd::chir::kInvalidNode;
    if (two_cells && swap_decls)
    {
        const crd::u32 sc = m.add_node(NK::StateDecl, StringView("score"), prog);
        m.add_pin(sc, PD::Out, StringView("cell"), StringView("Score"));
        ws = m.add_node(NK::StateDecl, ws_name, prog);
        m.add_pin(ws, PD::Out, StringView("cell"), StringView("World"));
    }
    else
    {
        ws = m.add_node(NK::StateDecl, ws_name, prog);
        m.add_pin(ws, PD::Out, StringView("cell"), StringView("World"));
        if (two_cells)
        {
            const crd::u32 sc = m.add_node(NK::StateDecl, StringView("score"), prog);
            m.add_pin(sc, PD::Out, StringView("cell"), StringView("Score"));
        }
    }
    const crd::u32 h = m.add_node(NK::EventHandler, StringView("on_tick"), prog);
    m.add_attr(h, StringView("domain"), StringView("event"));
    const crd::u32 q = m.add_node(NK::Query, StringView("q"), h);
    m.add_attr(q, StringView("components"),
               extra_component ? StringView("Position.Velocity.Mass") : StringView("Position.Velocity"));
    m.add_pin(q, PD::Out, StringView("entities"), StringView("EntitySet"));
    const crd::u32 pf = m.add_node(NK::ParallelFor, StringView("update"), h);
    m.add_pin(pf, PD::In, StringView("entities"), StringView("EntitySet"));
    m.add_pin(pf, PD::Out, StringView("updated"), StringView("EntitySet"));
    const crd::u32 aw = m.add_node(NK::Await, StringView("task"), h);
    m.add_pin(aw, PD::Out, StringView("result"), StringView("TaskResult"));
    if (extra_await)
    {
        const crd::u32 a2 = m.add_node(NK::Await, StringView("task2"), h);
        m.add_pin(a2, PD::Out, StringView("result"), StringView("TaskResult"));
    }
    const crd::u32 su = m.add_node(NK::StateUpdate, StringView("commit"), h);
    m.add_pin(su, PD::In, StringView("cell"), StringView("World"));
    m.add_pin(su, PD::In, StringView("updated"), StringView("EntitySet"));
    m.add_edge(q, 0U, pf, 0U);  // q.entities   -> update.entities
    m.add_edge(ws, 0U, su, 0U); // world_state.cell -> commit.cell (the writing-update link the 32e collapse resolves)
    m.add_edge(pf, 1U, su, 1U); // update.updated   -> commit.updated
    if (two_handlers) // a SECOND handler over the SAME program-scope state — exercises the duplicate-pin guard
    {
        const crd::u32 h2 = m.add_node(NK::EventHandler, StringView("on_tick2"), prog);
        m.add_attr(h2, StringView("domain"), StringView("event"));
        const crd::u32 q2 = m.add_node(NK::Query, StringView("q2"), h2);
        m.add_attr(q2, StringView("components"), StringView("Position"));
        m.add_pin(q2, PD::Out, StringView("entities"), StringView("EntitySet"));
    }
    m.derive_ids();
}

// the first func.func op at the top of the module body (a non-state op — its stable id is sequential, ABOVE the reserve).
crd::ceir::Operation* first_func(crd::ceir::Context& ctx, crd::ceir::Module& mod)
{
    const crd::ceir::OpId fk = ctx.intern_op("func", "func");
    for (crd::ceir::Operation* op = mod.body()->first_block()->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (op->kind() == fk) { return op; }
    }
    return nullptr;
}

// the CHIR StableId of the StateDecl named `name` (position-independent, name+scope-derived) — the pin seed.
crd::u64 decl_id(const crd::chir::SourceModel& m, const char* name)
{
    const StringView want(name);
    for (crd::u32 i = 0; i < m.node_count(); ++i)
    {
        const crd::chir::ChirNode& n = m.node(i);
        if (n.kind == crd::chir::NodeKind::StateDecl && m.str(n.name) == want) { return n.id.value; }
    }
    return 0U;
}

// the reserved-band CEIR id a decl named `name` pins to (matches lower.cpp plan_state_pins, sans the negligible probe).
crd::u64 expect_cell_id(const crd::chir::SourceModel& m, const char* name)
{
    return crd::u64(1) + (decl_id(m, name) % crd::chir::kChirStateIdReserve);
}

bool contains_id(const crd::containers::Array<crd::ceir::StateCell>& cells, crd::u64 id)
{
    for (crd::u32 i = 0; i < cells.size(); ++i)
    {
        if (cells[i].id == id) { return true; }
    }
    return false;
}
} // namespace

TEST_CASE("chir 32d: the TEXT and GRAPH projections lower to the BYTE-IDENTICAL CEIR module (ADR-0128 D5, sec-180 #13)",
          "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    // TEXT projection -> CHIR model -> CEIR module MA (its own Context).
    const Array<char>          text = slurp(kTextPath, &root);
    crd::chir::SourceModel     from_text(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), 1U, from_text).ok);
    crd::ceir::Context         ctx_a(&root);
    crd::ceir::Module* const   ma = crd::chir::lower_chir(from_text, ctx_a);
    REQUIRE(ma != nullptr);

    // GRAPH projection -> CHIR model -> CEIR module MB (a SEPARATE Context — same ctx would clash on the func symbol).
    const Array<char>        graph = slurp(kGraphPath, &root);
    crd::chir::SourceModel   from_graph(&root);
    REQUIRE(crd::chir::read_schema(sv(graph), from_graph));
    crd::ceir::Context       ctx_b(&root);
    crd::ceir::Module* const mb = crd::chir::lower_chir(from_graph, ctx_b);
    REQUIRE(mb != nullptr);

    // ⭐ PARITY: both projections lower to the byte-identical CEIR (printer-canonical — the §143 proof, §180 #13).
    const String ta = crd::ceir::print(ctx_a, *ma, &root);
    const String tb = crd::ceir::print(ctx_b, *mb, &root);
    CHECK(sv(ta) == sv(tb));
}

TEST_CASE("chir 32d: the lowered CEIR is structure-verifier-clean, round-trips, and carries the D1 op map", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    const Array<char>        text = slurp(kTextPath, &root);
    crd::chir::SourceModel   model(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), 1U, model).ok);
    crd::ceir::Context       ctx(&root);
    crd::ceir::Module* const mod = crd::chir::lower_chir(model, ctx);
    REQUIRE(mod != nullptr);

    // structure-verifier-clean — including the D2 query-view read path: the task.parallel_for body references the func's
    // view param (an OUTER value), which is legal because parallel_for is NOT IsolatedFromAbove (only func.func is), so
    // the param dominates the body (no CaptureThroughIsolation / UseBeforeDef). Self-containment is a 32e/exec caveat.
    CHECK(ctx.find_structure_error(*mod).kind == crd::ceir::StructureErrorKind::None);

    // CEIR TEXT round-trip: print -> parse -> ok (the ops parse back on the same registered ctx).
    const String                    printed = crd::ceir::print(ctx, *mod, &root);
    const crd::ceir::ParseResult    rt      = crd::ceir::parse(ctx, sv(printed));
    CHECK(rt.ok);

    // the ADR-0128 D1 node->op map landed (a coarse presence check on the canonical print).
    const StringView p = sv(printed);
    CHECK(contains(p, "func.func"));         // EventHandler
    CHECK(contains(p, "domain"));            // + its time-domain attr (ADR-0116, NO event op)
    CHECK(contains(p, "task.parallel_for")); // ParallelFor
    CHECK(contains(p, "async.launch"));      // Await (launch...
    CHECK(contains(p, "async.await"));       // ...await)
    CHECK(contains(p, "core.state"));        // StateUpdate (the §20 cell)
    CHECK_FALSE(contains(p, "event."));      // ⛔ zero-new-ops: NO event dialect/op was invented

    // ⭐ D2 read-path IDENTITY (not category): the task.parallel_for body's yield operand IS the func's view param arg0
    // — the parallel body actually READS the query result (the whole point of the D2 prerequisite), not just %iv.
    const crd::ceir::OpId func_kind  = ctx.intern_op("func", "func");
    const crd::ceir::OpId pf_kind    = ctx.intern_op("task", "parallel_for");
    const crd::ceir::OpId yield_kind = ctx.intern_op("core", "yield");
    crd::ceir::Operation* fn         = nullptr;
    for (crd::ceir::Operation* op = mod->body()->first_block()->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (op->kind() == func_kind) { fn = op; break; }
    }
    REQUIRE(fn != nullptr);
    crd::ceir::Block* const fb = crd::ceir::func::func_body_block(fn);
    REQUIRE(fb->num_args() == 2U); // the query view: Position + Velocity (the `components` attr, dot-joined)
    crd::ceir::Operation* pf = nullptr;
    for (crd::ceir::Operation* op = fb->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (op->kind() == pf_kind) { pf = op; break; }
    }
    REQUIRE(pf != nullptr);
    crd::ceir::Block* const     pb   = pf->region(0)->first_block();
    crd::ceir::Operation* const term = pb->last_op();
    REQUIRE(term != nullptr);
    CHECK(term->kind() == yield_kind);
    REQUIRE(term->num_operands() == 1U);
    CHECK(term->operand(0) == fb->arg(0)); // the yield reads the VIEW param (arg0) — the D2 read path, NOT %iv
}

TEST_CASE("chir 32d: the lowering THREADS the CHIR edges (oracle-minus-edges lowers DIFFERENTLY)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    using crd::chir::NodeKind;
    using crd::chir::PinDir;
    using crd::chir::kInvalidNode;

    // ⛔ PARITY IS NECESSARY, NOT SUFFICIENT: a lowering that fabricated its inputs would pass parity even having lost
    // every wire. This falsifier builds a tiny `query -> parallel(in entities = q.entities)` two ways — WITH the edge and
    // WITHOUT — and asserts they lower to DIFFERENT CEIR: with the edge the parallel body yields the view (func arg0),
    // without it yields %iv. That is the tooth proving the edges are actually threaded.
    const auto build = [](crd::chir::SourceModel& m, bool with_edge) {
        const crd::u32 prog = m.add_node(NodeKind::Program, StringView("p"), kInvalidNode);
        const crd::u32 h    = m.add_node(NodeKind::EventHandler, StringView("h"), prog);
        const crd::u32 q    = m.add_node(NodeKind::Query, StringView("q"), h);
        m.add_pin(q, PinDir::Out, StringView("entities"), StringView("E"));
        m.add_attr(q, StringView("components"), StringView("A")); // 1 component => the func has 1 view param
        const crd::u32 pf = m.add_node(NodeKind::ParallelFor, StringView("u"), h);
        m.add_pin(pf, PinDir::In, StringView("entities"), StringView("E"));
        if (with_edge) { m.add_edge(q, 0U, pf, 0U); } // q.entities -> parallel.entities
        m.derive_ids();
    };

    crd::chir::SourceModel with_edge(&root);
    build(with_edge, true);
    crd::chir::SourceModel no_edge(&root);
    build(no_edge, false);

    crd::ceir::Context       ca(&root);
    crd::ceir::Context       cb(&root);
    crd::ceir::Module* const ma = crd::chir::lower_chir(with_edge, ca);
    crd::ceir::Module* const mb = crd::chir::lower_chir(no_edge, cb);
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);
    const String ta = crd::ceir::print(ca, *ma, &root);
    const String tb = crd::ceir::print(cb, *mb, &root);
    CHECK(sv(ta) != sv(tb)); // the edge changes the parallel body's yield operand => the print differs
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// CEIR-32e — HOT RELOAD + STATE MIGRATION (ADR-0128 D3, the §143 fifth requirement). CHIR pins each `state` cell's CEIR
// stable id from its SOURCE identity (name + lexical scope), NOT textual position, so a re-lower after an edit reproduces
// the state schema the CEIR-10a supervisor keys on (`collect_state_schema` / `interface_hash` / `contract_hash`), and the
// hot-reload value-move (exec `restore_state_by_id`, which matches by id VALUE) MIGRATES the cell instead of losing it.
// The decision table (from test_reload_migration.cpp): interface== -> CompatibleReuse; interface≠ + contract== -> Migrate;
// interface≠ + contract≠ -> Reject.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

TEST_CASE("chir 32e: a BODY-only edit keeps the state schema (reload = CompatibleReuse); the pin lands in the reserved band",
          "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::chir::SourceModel             base(&root);
    build143(base, false, false, false, false, /*extra_await=*/false);
    crd::chir::SourceModel body(&root);
    build143(body, false, false, false, false, /*extra_await=*/true); // + a second `await` statement (a body edit)

    crd::ceir::Context       ca(&root);
    crd::ceir::Context       cb(&root);
    crd::ceir::Module* const ma = crd::chir::lower_chir(base, ca);
    crd::ceir::Module* const mb = crd::chir::lower_chir(body, cb);
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);

    // a body edit touches neither the signature nor the state schema => interface_hash EQUAL => CompatibleReuse (no reload).
    CHECK(crd::ceir::interface_hash(ca, *ma, &root) == crd::ceir::interface_hash(cb, *mb, &root));
    CHECK(crd::ceir::contract_hash(ca, *ma, &root) == crd::ceir::contract_hash(cb, *mb, &root));

    // ⭐ the pin LANDED: the ONE cell's id sits in the reserved band [1, reserve] and is the source-derived value; a
    //   NON-state op (func.func) got a sequential id ABOVE the band (the watermark floor) — which is what makes the
    //   band-membership check a real falsifier: unpin the cell and it would land above the reserve like the func.
    const crd::containers::Array<crd::ceir::StateCell> sa = crd::ceir::collect_state_schema(ca, *ma, &root);
    REQUIRE(sa.size() == 1U);
    CHECK(sa[0].id >= 1U);
    CHECK(sa[0].id <= crd::chir::kChirStateIdReserve);
    CHECK(contains_id(sa, expect_cell_id(base, "world_state")));
    crd::ceir::Operation* const fn = first_func(ca, *ma);
    REQUIRE(fn != nullptr);
    CHECK(fn->stable_id().value > crd::chir::kChirStateIdReserve);
}

TEST_CASE("chir 32e: REORDERING the state declarations does NOT renumber the cells (source-derived, position-independent)",
          "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::chir::SourceModel             norm(&root);
    build143(norm, /*two_cells=*/true, /*swap_decls=*/false, false, false, false);
    crd::chir::SourceModel swap(&root);
    build143(swap, /*two_cells=*/true, /*swap_decls=*/true, false, false, false); // score declared BEFORE world_state

    crd::ceir::Context       ca(&root);
    crd::ceir::Context       cb(&root);
    crd::ceir::Module* const ma = crd::chir::lower_chir(norm, ca);
    crd::ceir::Module* const mb = crd::chir::lower_chir(swap, cb);
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);

    const crd::containers::Array<crd::ceir::StateCell> sa = crd::ceir::collect_state_schema(ca, *ma, &root);
    const crd::containers::Array<crd::ceir::StateCell> sb = crd::ceir::collect_state_schema(cb, *mb, &root);
    REQUIRE(sa.size() == 2U);
    REQUIRE(sb.size() == 2U);

    // ⛔ THE PIN'S CORE FALSIFIER: each cell keeps its SOURCE-derived id regardless of declaration order. A positional /
    //   rank scheme would give ids {2,3} in decl order -> a reorder would SWAP which cell holds which id, so `world_state`
    //   would carry a different id here than in `norm` (state corruption at reload). The source-hash ids are identical.
    const crd::u64 ws_id = expect_cell_id(norm, "world_state");
    const crd::u64 sc_id = expect_cell_id(norm, "score");
    CHECK(contains_id(sa, ws_id));
    CHECK(contains_id(sa, sc_id));
    CHECK(contains_id(sb, ws_id)); // swapped decl order -> SAME ids (the position-independence the reload rests on)
    CHECK(contains_id(sb, sc_id));
    CHECK(crd::ceir::interface_hash(ca, *ma, &root) == crd::ceir::interface_hash(cb, *mb, &root)); // => CompatibleReuse
}

TEST_CASE("chir 32e: ADDING a state cell is a schema change (Migrate) and PRESERVES the existing cell's id (insert-before)",
          "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::chir::SourceModel             one(&root);
    build143(one, /*two_cells=*/false, false, false, false, false); // 1 cell (world_state)
    crd::chir::SourceModel two(&root);
    build143(two, /*two_cells=*/true, /*swap_decls=*/true, false, false, false); // `score` inserted BEFORE world_state

    crd::ceir::Context       ca(&root);
    crd::ceir::Context       cb(&root);
    crd::ceir::Module* const ma = crd::chir::lower_chir(one, ca);
    crd::ceir::Module* const mb = crd::chir::lower_chir(two, cb);
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);

    const crd::containers::Array<crd::ceir::StateCell> sa = crd::ceir::collect_state_schema(ca, *ma, &root);
    const crd::containers::Array<crd::ceir::StateCell> sb = crd::ceir::collect_state_schema(cb, *mb, &root);
    REQUIRE(sa.size() == 1U);
    REQUIRE(sb.size() == 2U);

    // interface DIFFERS (the schema grew) but contract is EQUAL (core.state declares ZERO effects, so the caller-visible
    // effect mask is unchanged) => Migrate, not Reject. A migration fn covers the new cell; the old one carries across.
    CHECK(crd::ceir::interface_hash(ca, *ma, &root) != crd::ceir::interface_hash(cb, *mb, &root));
    CHECK(crd::ceir::contract_hash(ca, *ma, &root) == crd::ceir::contract_hash(cb, *mb, &root));

    // ⛔ THE PIN'S CORRECTNESS: world_state keeps its id even though `score` was declared BEFORE it — a POSITION shift a
    //   sequential scheme would renumber, moving world_state's live value to the wrong cell on reload. Here it migrates.
    const crd::u64 ws_id = expect_cell_id(one, "world_state");
    CHECK(contains_id(sa, ws_id));
    CHECK(contains_id(sb, ws_id));
}

TEST_CASE("chir 32e: RENAMING a state cell changes the schema (Migrate) - a fresh identity, not a silent recolor", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::chir::SourceModel             base(&root);
    build143(base, false, false, /*rename_ws=*/false, false, false);
    crd::chir::SourceModel ren(&root);
    build143(ren, false, false, /*rename_ws=*/true, false, false); // world_state -> game_state

    crd::ceir::Context       ca(&root);
    crd::ceir::Context       cb(&root);
    crd::ceir::Module* const ma = crd::chir::lower_chir(base, ca);
    crd::ceir::Module* const mb = crd::chir::lower_chir(ren, cb);
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);

    const crd::containers::Array<crd::ceir::StateCell> sa = crd::ceir::collect_state_schema(ca, *ma, &root);
    const crd::containers::Array<crd::ceir::StateCell> sb = crd::ceir::collect_state_schema(cb, *mb, &root);
    REQUIRE(sa.size() == 1U);
    REQUIRE(sb.size() == 1U);

    // the id is DERIVED from the name+scope, so a rename yields a DIFFERENT cell id (the user's req 3: rename => hash CHANGES).
    CHECK(sa[0].id != sb[0].id);
    CHECK(crd::ceir::interface_hash(ca, *ma, &root) != crd::ceir::interface_hash(cb, *mb, &root)); // => Migrate
    CHECK(crd::ceir::contract_hash(ca, *ma, &root) == crd::ceir::contract_hash(cb, *mb, &root));   // chir_decl is not caller-visible
}

TEST_CASE("chir 32e: a SIGNATURE change (a new query component => a new func param) is Reject, not Migrate", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::chir::SourceModel             base(&root);
    build143(base, false, false, false, /*extra_component=*/false, false); // 2 components -> 2 params
    crd::chir::SourceModel sig(&root);
    build143(sig, false, false, false, /*extra_component=*/true, false); // 3 components -> 3 params

    crd::ceir::Context       ca(&root);
    crd::ceir::Context       cb(&root);
    crd::ceir::Module* const ma = crd::chir::lower_chir(base, ca);
    crd::ceir::Module* const mb = crd::chir::lower_chir(sig, cb);
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);

    // the func's param list changed => the caller-visible contract broke => BOTH hashes differ => Reject (a migration fn
    // cannot save callers). The state schema is beside the point.
    CHECK(crd::ceir::interface_hash(ca, *ma, &root) != crd::ceir::interface_hash(cb, *mb, &root));
    CHECK(crd::ceir::contract_hash(ca, *ma, &root) != crd::ceir::contract_hash(cb, *mb, &root));
}

TEST_CASE("chir 32e: the TEXT and GRAPH projections pin the SAME state-cell ids (one reload schema, two projections)",
          "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  text = slurp(kTextPath, &root);
    crd::chir::SourceModel             from_text(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), 1U, from_text).ok);
    const Array<char>      graph = slurp(kGraphPath, &root);
    crd::chir::SourceModel from_graph(&root);
    REQUIRE(crd::chir::read_schema(sv(graph), from_graph));

    crd::ceir::Context       ca(&root);
    crd::ceir::Context       cb(&root);
    crd::ceir::Module* const ma = crd::chir::lower_chir(from_text, ca);
    crd::ceir::Module* const mb = crd::chir::lower_chir(from_graph, cb);
    REQUIRE(ma != nullptr);
    REQUIRE(mb != nullptr);

    const crd::containers::Array<crd::ceir::StateCell> sa = crd::ceir::collect_state_schema(ca, *ma, &root);
    const crd::containers::Array<crd::ceir::StateCell> sb = crd::ceir::collect_state_schema(cb, *mb, &root);
    REQUIRE(sa.size() == 1U);
    REQUIRE(sb.size() == 1U);
    CHECK(sa[0].id == sb[0].id); // the same SOURCE identity => the same pinned id from both projections
    CHECK(crd::ceir::interface_hash(ca, *ma, &root) == crd::ceir::interface_hash(cb, *mb, &root));
}

TEST_CASE("chir 32e: program-scope state cells are emitted ONCE across handlers (the duplicate-pin guard)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::chir::SourceModel             model(&root);
    build143(model, false, false, false, false, false, /*two_handlers=*/true); // world_state + TWO handlers

    crd::ceir::Context       ctx(&root);
    crd::ceir::Module* const mod = crd::chir::lower_chir(model, ctx);
    REQUIRE(mod != nullptr);

    // ⛔ the duplicate-pin guard: `world_state` is program-scope and both handlers lower to their own func, but the cell
    //   is emitted ONCE (in the first handler's body), NOT once per handler. Without the guard, each handler would emit a
    //   core.state pinned to the SAME source-derived id -> two ops sharing one stable id (a duplicate migration key =
    //   state corruption). Exactly ONE cell, carrying world_state's source id, and the module is structure-verifier-clean.
    const crd::containers::Array<crd::ceir::StateCell> sc = crd::ceir::collect_state_schema(ctx, *mod, &root);
    REQUIRE(sc.size() == 1U);
    CHECK(sc[0].id == expect_cell_id(model, "world_state"));
    CHECK(ctx.find_structure_error(*mod).kind == crd::ceir::StructureErrorKind::None);
}

// ── CEIR-35b: mutation-robustness fuzz for parse_chir (the .chir TEXT language loader). ───────────────────────────────
// The 3rd + LAST CEIR-35b parser arm (after ceir parse/deserialize + kir ckir_read). Mutate a canonical CHIR seed
// thousands of ways and prove parse_chir NEVER crashes/throws (ASan-clean under the asan configs) and ALWAYS returns a
// WELL-FORMED ChirParseResult. Four teeth: (1) well-formed result (a rejection carries an in-range offset + a non-empty
// static message); (2) any ACCEPTED mutant is CANONICAL-IDEMPOTENT -- print_chir(parse_chir(mutant)) =: C1 re-parses and
// re-prints to the SAME bytes (parse_chir/print_chir's documented text FIXED POINT holds on the parser's OWN canonical
// output, regardless of whether the mutant itself was canonical); (3) determinism (same seed -> identical (ok,err_off)
// trace); (4) non-vacuity (>=1 accept AND >=1 reject). Deterministic splitmix64 (no <random>); the mutator is duplicated
// per exe (not shared-headered), per the advisor. Seed = print_chir(build143(...)) -- the committed .chir shape, built
// IN-TEST (no file I/O, no CRD_REPO_DIR). GrowableTlsfAllocator: each per-mutant SourceModel recycles into the arena.
namespace
{
struct ChFuzzRng
{
    crd::u64 state;
    explicit ChFuzzRng(crd::u64 seed) noexcept : state(seed) {}
    crd::u64 next() noexcept
    {
        crd::u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z          = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z          = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    crd::u32 below(crd::u32 n) noexcept { return n == 0U ? 0U : static_cast<crd::u32>(next() % n); }
};

// the first `count` bytes of `src` in a fresh Array — the TRUNCATION primitive (count == size copies whole).
[[nodiscard]] Array<crd::u8> ch_prefix(crd::containers::ConstSpan<crd::u8> src, crd::usize count, crd::memory::IAllocator* a)
{
    Array<crd::u8> b(a);
    b.reserve(count);
    for (crd::usize i = 0U; i < count; ++i) { b.push_back(src[i]); }
    return b;
}

// ONE byte-level mutation (bit-flip / delete / insert-any-byte incl NUL + 0x80-0xFF / duplicate / swap), copy-with-transform.
[[nodiscard]] Array<crd::u8> ch_mutate(crd::containers::ConstSpan<crd::u8> src, ChFuzzRng& rng, crd::memory::IAllocator* a)
{
    Array<crd::u8>   b(a);
    const crd::usize n = src.size();
    if (n == 0U)
    {
        b.push_back(static_cast<crd::u8>(rng.next()));
        return b;
    }
    const crd::u32   kind = rng.below(5U);
    const crd::usize pos  = rng.below(static_cast<crd::u32>(n));
    switch (kind)
    {
    case 0U: // bit-flip one byte
        b.reserve(n);
        for (crd::usize i = 0U; i < n; ++i)
        {
            b.push_back(i == pos ? static_cast<crd::u8>(src[i] ^ static_cast<crd::u8>(1U << rng.below(8U))) : src[i]);
        }
        break;
    case 1U: // delete the byte at pos
        b.reserve(n - 1U);
        for (crd::usize i = 0U; i < n; ++i)
        {
            if (i != pos) { b.push_back(src[i]); }
        }
        break;
    case 2U: // insert an arbitrary byte before pos
    {
        const crd::u8 v = static_cast<crd::u8>(rng.next());
        b.reserve(n + 1U);
        for (crd::usize i = 0U; i < n; ++i)
        {
            if (i == pos) { b.push_back(v); }
            b.push_back(src[i]);
        }
        break;
    }
    case 3U: // duplicate the byte at pos
        b.reserve(n + 1U);
        for (crd::usize i = 0U; i < n; ++i)
        {
            b.push_back(src[i]);
            if (i == pos) { b.push_back(src[i]); }
        }
        break;
    default: // swap two bytes
    {
        const crd::usize q = rng.below(static_cast<crd::u32>(n));
        b.reserve(n);
        for (crd::usize i = 0U; i < n; ++i)
        {
            crd::u8 v = src[i];
            if (i == pos) { v = src[q]; }
            else if (i == q) { v = src[pos]; }
            b.push_back(v);
        }
        break;
    }
    }
    return b;
}

// A ChirParseResult is WELL-FORMED iff a rejection carries an in-range byte offset + a non-empty static message.
[[nodiscard]] bool chir_result_wf(const crd::chir::ChirParseResult& r, crd::usize input_size) noexcept
{
    if (r.ok) { return true; }
    return static_cast<crd::usize>(r.err_off) <= input_size && r.msg.size() > 0U;
}

// byte-equality of two char arrays (the canonical-idempotence tooth's compare).
[[nodiscard]] bool ch_bytes_eq(const Array<char>& x, const Array<char>& y) noexcept
{
    if (x.size() != y.size()) { return false; }
    for (crd::usize i = 0U; i < x.size(); ++i)
    {
        if (x[i] != y[i]) { return false; }
    }
    return true;
}
} // namespace

TEST_CASE("ceir fuzz: parse_chir survives byte mutation of a .chir and never crashes", "[chir][fuzz][ceir35b]")
{
    crd::memory::GrowableTlsfAllocator root;

    // Seed = print_chir(build143) — CANONICAL CHIR text, built IN-TEST (no file I/O, no CRD_REPO_DIR).
    crd::chir::SourceModel seed_m(&root);
    build143(seed_m, false, false, false, false, false);
    Array<char> seed_buf(&root);
    crd::chir::print_chir(seed_m, seed_buf);
    const crd::containers::ConstSpan<crd::u8> seed(reinterpret_cast<const crd::u8*>(seed_buf.data()), seed_buf.size());
    REQUIRE(seed.size() > 16U);
    { // (0) non-vacuity floor: the unmutated canonical seed parses ok
        crd::chir::SourceModel m(&root);
        REQUIRE(crd::chir::parse_chir(sv(seed_buf), 1U, m).ok);
    }

    crd::usize accepts = 0U;
    crd::usize rejects = 0U;

    auto run_one = [&](crd::containers::ConstSpan<crd::u8> mutant) -> crd::u64 {
        crd::chir::SourceModel           m(&root);
        const crd::chir::ChirParseResult r = crd::chir::parse_chir(
            StringView(reinterpret_cast<const char*>(mutant.data()), mutant.size()), 1U, m);
        CHECK(chir_result_wf(r, mutant.size())); // (invariant 1) well-formedness
        if (r.ok)
        {
            ++accepts;
            // (invariant 2) CANONICAL-IDEMPOTENCE: print the accepted model, re-parse, re-print -> SAME bytes (the
            // documented parse_chir/print_chir text fixed point holds on the parser's OWN canonical output).
            Array<char> c1(&root);
            crd::chir::print_chir(m, c1);
            crd::chir::SourceModel           m2(&root);
            const crd::chir::ChirParseResult r2 = crd::chir::parse_chir(sv(c1), 1U, m2);
            CHECK(r2.ok);
            if (r2.ok)
            {
                Array<char> c2(&root);
                crd::chir::print_chir(m2, c2);
                CHECK(ch_bytes_eq(c1, c2));
            }
        }
        else { ++rejects; }
        return (static_cast<crd::u64>(r.ok) << 63U) ^ static_cast<crd::u64>(r.err_off);
    };

    for (crd::usize off = 0U; off <= seed.size(); ++off) // (a) exhaustive truncation
    {
        const Array<crd::u8> t = ch_prefix(seed, off, &root);
        (void)run_one(crd::containers::ConstSpan<crd::u8>(t.data(), t.size()));
    }

    auto random_pass = [&](crd::u64 seed_val) -> crd::u64 { // (b) random byte-level mutations
        ChFuzzRng          rng(seed_val);
        crd::u64           trace        = 1469598103934665603ULL; // FNV-1a offset basis
        constexpr crd::u32 mutant_count = 256U * 5U;              // ~256 per mutation kind
        for (crd::u32 i = 0U; i < mutant_count; ++i)
        {
            const Array<crd::u8> mm = ch_mutate(seed, rng, &root);
            trace = (trace ^ run_one(crd::containers::ConstSpan<crd::u8>(mm.data(), mm.size()))) * 1099511628211ULL;
        }
        return trace;
    };
    CHECK(random_pass(0xCC1235B2ULL) == random_pass(0xCC1235B2ULL)); // (invariant 3) determinism

    CHECK(accepts > 0U); // (invariant 4) non-vacuity
    CHECK(rejects > 0U);
}
