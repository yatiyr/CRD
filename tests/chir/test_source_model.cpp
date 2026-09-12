// test_source_model.cpp — CEIR-32b: the CHIR source model + the CR-D007 graph-schema projection (ADR-0128 D5).
//
// Proves, with ZERO parser (32c) and ZERO lowering (32d): the §143 event handler builds as a source model; the
// graph-schema serialization is a fixed-point (print(read(file)) == file); the committed ids are anti-drift (a
// re-derive reproduces them); stable ids are position-independent (§180 #1); layout is separable from semantics
// (§180 #9/#10). The `build_event_handler_model` oracle is KEPT (the anti-drift printer-oracle mold); the committed
// asset was bootstrapped via the `[.chir-bootstrap]` case (STRIPPED after commit).

#include <crd/chir/node.hpp>
#include <crd/chir/schema.hpp>
#include <crd/chir/text.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream> // slurp the committed .chirgraph asset (test-side, mirroring the audio/ceir reading-gate TUs)

namespace
{
using crd::chir::NodeKind;
using crd::chir::PinDir;
using crd::chir::SourceModel;
using crd::chir::kInvalidNode;
using crd::containers::Array;
using crd::containers::StringView;

// The §143 event handler as a CHIR source model (ADR-0128 D1 — the five constructs + Program/StateDecl scaffolding).
// ⛔ the KEPT anti-drift printer-oracle: regenerate the committed asset from THIS if it changes (the bootstrap case).
// `parallel update` (state-free) precedes `update state` (sequential) — ADR-0128 D2. Edges wire the dataflow.
void build_event_handler_model(SourceModel& m)
{
    const crd::u32 prog = m.add_node(NodeKind::Program, StringView("event_demo"), kInvalidNode);
    // program-scope `state world_state: World` (a core.state cell — the reload-stable declaration, ADR-0128 D3)
    const crd::u32 st = m.add_node(NodeKind::StateDecl, StringView("world_state"), prog);
    m.add_pin(st, PinDir::Out, StringView("cell"), StringView("World"));
    // the event handler `on tick` — domain=event (lowers to a func.func + time-domain attr, ADR-0128 D1)
    const crd::u32 eh = m.add_node(NodeKind::EventHandler, StringView("on_tick"), prog);
    m.add_attr(eh, StringView("domain"), StringView("event"));
    // body statement 1: query entities
    const crd::u32 q = m.add_node(NodeKind::Query, StringView("q"), eh);
    m.add_pin(q, PinDir::Out, StringView("entities"), StringView("EntitySet"));
    m.add_attr(q, StringView("components"), StringView("Position.Velocity"));
    // body statement 2: parallel update (over q) — ⛔ body STATE-FREE (ADR-0128 D2)
    const crd::u32 pf = m.add_node(NodeKind::ParallelFor, StringView("update"), eh);
    m.add_pin(pf, PinDir::In, StringView("entities"), StringView("EntitySet"));
    m.add_pin(pf, PinDir::Out, StringView("updated"), StringView("EntitySet"));
    // body statement 3: await async task
    const crd::u32 aw = m.add_node(NodeKind::Await, StringView("task"), eh);
    m.add_pin(aw, PinDir::Out, StringView("result"), StringView("TaskResult"));
    // body statement 4: update state — SEQUENTIAL, after the parallel body (ADR-0128 D2)
    const crd::u32 su = m.add_node(NodeKind::StateUpdate, StringView("commit"), eh);
    m.add_pin(su, PinDir::In, StringView("cell"), StringView("World"));
    m.add_pin(su, PinDir::In, StringView("updated"), StringView("EntitySet"));
    // dataflow edges
    m.add_edge(q, 0U, pf, 0U);  // q.entities  -> parallel.entities
    m.add_edge(pf, 1U, su, 1U); // parallel.updated -> update.updated
    m.add_edge(st, 0U, su, 0U); // state.cell  -> update.cell
    m.derive_ids();
    // a layout side-table (a simple column) — SEPARATE from semantics (§180 #9); moving it must not change semantic_hash
    m.set_layout(m.node(prog).id, 0.0F, 0.0F, 0U);
    m.set_layout(m.node(eh).id, 0.0F, 100.0F, 1U);
    m.set_layout(m.node(q).id, 40.0F, 160.0F, 1U);
    m.set_layout(m.node(pf).id, 40.0F, 220.0F, 1U);
    m.set_layout(m.node(aw).id, 40.0F, 280.0F, 1U);
    m.set_layout(m.node(su).id, 40.0F, 340.0F, 1U);
}

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

constexpr const char* kAssetPath = CRD_REPO_DIR "/assets/chir/event_handler.chirgraph";
constexpr const char* kChirPath  = CRD_REPO_DIR "/assets/chir/event_handler.chir";
} // namespace

// ⛔ The committed `event_handler.chirgraph` was BOOTSTRAPPED via a `[.chir-bootstrap]` case that wrote
// `print_schema(build_event_handler_model())` to the asset path; that write was STRIPPED after commit per the
// committed-asset recipe ([[reference_ckir_bootstrap_via_write_eval_verify_before_commit]]). The oracle
// `build_event_handler_model` is KEPT (the anti-drift SOURCE); regenerate the asset from it if it changes.

TEST_CASE("chir 32b: the committed event_handler.chirgraph is anti-drift, roundtrip-stable, and complete", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    // ANTI-DRIFT THROUGH THE PRINTER: the oracle's canonical print == the committed file bytes (never file-bytes-first).
    SourceModel oracle(&root);
    build_event_handler_model(oracle);
    Array<char> t_oracle(&root);
    crd::chir::print_schema(oracle, t_oracle);
    const Array<char> t_file = slurp(kAssetPath, &root);
    CHECK(sv(t_oracle) == sv(t_file));

    // ROUNDTRIP fixed-point (pure serialization, no re-derive): print(read(file)) == file.
    SourceModel loaded(&root);
    REQUIRE(crd::chir::read_schema(sv(t_file), loaded));
    Array<char> t_reprint(&root);
    crd::chir::print_schema(loaded, t_reprint);
    CHECK(sv(t_reprint) == sv(t_file));

    // ID ANTI-DRIFT: the committed ids are a pure function of structure — a re-derive reproduces the file's ids exactly.
    SourceModel rederived(&root);
    REQUIRE(crd::chir::read_schema(sv(t_file), rederived));
    Array<char> before(&root);
    crd::chir::print_schema(rederived, before);
    rederived.derive_ids();
    Array<char> after(&root);
    crd::chir::print_schema(rederived, after);
    CHECK(sv(before) == sv(after)); // derive changed nothing => the committed ids match the committed structure

    // COMPLETENESS (the mold's blind spot — a node-deleted asset also parses+prints clean): exactly the §143 kinds+counts.
    crd::u32 n_prog   = 0;
    crd::u32 n_eh     = 0;
    crd::u32 n_state  = 0;
    crd::u32 n_query  = 0;
    crd::u32 n_par    = 0;
    crd::u32 n_await  = 0;
    crd::u32 n_update = 0;
    for (crd::u32 i = 0; i < loaded.node_count(); ++i)
    {
        switch (loaded.node(i).kind)
        {
        case NodeKind::Program: ++n_prog; break;
        case NodeKind::EventHandler: ++n_eh; break;
        case NodeKind::StateDecl: ++n_state; break;
        case NodeKind::Query: ++n_query; break;
        case NodeKind::ParallelFor: ++n_par; break;
        case NodeKind::Await: ++n_await; break;
        case NodeKind::StateUpdate: ++n_update; break;
        }
    }
    CHECK(loaded.node_count() == 7U);
    CHECK(n_prog == 1U);
    CHECK(n_eh == 1U);
    CHECK(n_state == 1U);
    CHECK(n_query == 1U);
    CHECK(n_par == 1U);
    CHECK(n_await == 1U);
    CHECK(n_update == 1U);
    CHECK(loaded.edges().size() == 3U);
}

TEST_CASE("chir 32b: stable ids are DETERMINISTIC and POSITION-INDEPENDENT (ADR-0128 ruling 3, D3)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    // DETERMINISM: two independent builds of the same program derive byte-identical ids (via a schema compare).
    SourceModel a(&root);
    build_event_handler_model(a);
    SourceModel b(&root);
    build_event_handler_model(b);
    Array<char> ta(&root);
    Array<char> tb(&root);
    crd::chir::print_schema(a, ta);
    crd::chir::print_schema(b, tb);
    CHECK(sv(ta) == sv(tb));

    // POSITION-INDEPENDENCE: reordering two NAMED sibling declarations leaves their ids unchanged (the reload-stable
    // state-schema property, ADR-0128 D3). Build a tiny program two ways: [alpha, beta] vs [beta, alpha] under one parent.
    const auto build_two = [](SourceModel& m, bool swapped) {
        const crd::u32 prog = m.add_node(NodeKind::Program, StringView("p"), kInvalidNode);
        if (!swapped)
        {
            m.add_node(NodeKind::StateDecl, StringView("alpha"), prog);
            m.add_node(NodeKind::StateDecl, StringView("beta"), prog);
        }
        else
        {
            m.add_node(NodeKind::StateDecl, StringView("beta"), prog);
            m.add_node(NodeKind::StateDecl, StringView("alpha"), prog);
        }
        m.derive_ids();
    };
    SourceModel m1(&root);
    build_two(m1, false);
    SourceModel m2(&root);
    build_two(m2, true);
    // find alpha's id in each — it must be the same despite the sibling order flip.
    const auto id_of = [](const SourceModel& m, StringView name) -> crd::u64 {
        for (crd::u32 i = 0; i < m.node_count(); ++i)
        {
            if (m.str(m.node(i).name) == name) { return m.node(i).id.value; }
        }
        return 0U;
    };
    CHECK(id_of(m1, StringView("alpha")) == id_of(m2, StringView("alpha")));
    CHECK(id_of(m1, StringView("beta")) == id_of(m2, StringView("beta")));
    CHECK(id_of(m1, StringView("alpha")) != id_of(m1, StringView("beta"))); // distinct decls, distinct ids
}

TEST_CASE("chir 32b: semantic_hash IGNORES layout + spans but reflects structure (ADR-0128 D5, sec-180 #10)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    SourceModel base(&root);
    build_event_handler_model(base);
    const crd::u64 h0 = base.semantic_hash();

    // MOVE a node's layout coordinates -> semantic_hash UNCHANGED (layout is separate from semantics, §180 #9).
    SourceModel moved(&root);
    build_event_handler_model(moved);
    moved.set_layout(moved.node(moved.root()).id, 999.0F, 999.0F, 7U);
    CHECK(moved.semantic_hash() == h0);

    // DELETE a node (the await) -> semantic_hash CHANGES (a real semantic diff, §180 #10).
    SourceModel deleted(&root);
    const crd::u32 prog = deleted.add_node(NodeKind::Program, StringView("event_demo"), kInvalidNode);
    const crd::u32 eh   = deleted.add_node(NodeKind::EventHandler, StringView("on_tick"), prog);
    deleted.add_node(NodeKind::Query, StringView("q"), eh);
    deleted.add_node(NodeKind::ParallelFor, StringView("update"), eh);
    // (no Await)
    deleted.add_node(NodeKind::StateUpdate, StringView("commit"), eh);
    deleted.derive_ids();
    CHECK(deleted.semantic_hash() != h0);
}

TEST_CASE("chir 32b: ANONYMOUS same-kind siblings get distinct, sibling-order-derived, deterministic ids", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    // The §143 program is fully named, so the anonymous-node ordinal path (ADR-0128 ruling 3: id = fnv(parent|kind|
    // ordinal-among-same-kind-anonymous-siblings-in-PARENT-CHILDREN-ORDER)) is otherwise untested. Two anonymous Await
    // siblings under one handler must get DISTINCT ids (the ordinal disambiguates them) and the SAME ids on a rebuild.
    const auto build_two_anon = [](SourceModel& m) {
        const crd::u32 prog = m.add_node(NodeKind::Program, StringView("p"), kInvalidNode);
        const crd::u32 eh   = m.add_node(NodeKind::EventHandler, StringView("h"), prog);
        m.add_node(NodeKind::Await, StringView(""), eh); // anonymous await #0 -> ordinal 0
        m.add_node(NodeKind::Await, StringView(""), eh); // anonymous await #1 -> ordinal 1
        m.derive_ids();
    };
    SourceModel a(&root);
    build_two_anon(a);
    // node 2 and node 3 are the two anonymous awaits.
    CHECK(a.node(2U).id.value != a.node(3U).id.value); // ordinal disambiguates two otherwise-identical anon siblings

    SourceModel b(&root);
    build_two_anon(b);
    CHECK(a.node(2U).id.value == b.node(2U).id.value); // deterministic across independent builds
    CHECK(a.node(3U).id.value == b.node(3U).id.value);
}

TEST_CASE("chir 32b: read_schema GRACEFULLY REJECTS a malformed graph (edge In->In, orphan layout)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    // POSITIVE control: a hand-authored well-formed document reads clean (Out -> In edge, no orphan layout).
    const char* ok = "chirgraph 1\n"
                     "node 0 0000000000000000 program p -1 0 0 0\n"
                     "node 1 0000000000000001 query q 0 0 0 0\n"
                     "node 2 0000000000000002 state_update s 0 0 0 0\n"
                     "pin 1 out entities EntitySet\n"
                     "pin 2 in entities EntitySet\n"
                     "edge 1 0 2 0\n";
    SourceModel good(&root);
    CHECK(crd::chir::read_schema(StringView(ok), good));

    // NEGATIVE 1 — edge wires an INPUT pin as its source (node 1 pin 0 is now `in`): the classic graph-authoring error
    // the tokenizer cannot see. read_schema must reject (ADR-0128 D5 graceful-reject convention).
    const char* in_to_in = "chirgraph 1\n"
                          "node 0 0000000000000000 program p -1 0 0 0\n"
                          "node 1 0000000000000001 query q 0 0 0 0\n"
                          "node 2 0000000000000002 state_update s 0 0 0 0\n"
                          "pin 1 in entities EntitySet\n" // from-pin is an INPUT -> illegal edge source
                          "pin 2 in entities EntitySet\n"
                          "edge 1 0 2 0\n";
    SourceModel bad1(&root);
    CHECK_FALSE(crd::chir::read_schema(StringView(in_to_in), bad1));

    // NEGATIVE 2 — a layout row whose id resolves to no node (an orphaned side-table row after a node was removed).
    const char* orphan_layout = "chirgraph 1\n"
                               "node 0 0000000000000000 program p -1 0 0 0\n"
                               "layout ffffffffffffffff 00000000 00000000 0\n";
    SourceModel bad2(&root);
    CHECK_FALSE(crd::chir::read_schema(StringView(orphan_layout), bad2));

    // NEGATIVE 3 — TWO edges into ONE in-pin (a single-writer violation the pin-direction check cannot see; the 32c
    // parity anchor requires each in-pin to have exactly one source). Both wire an OUT pin -> node 2's in-pin 0.
    const char* double_writer = "chirgraph 1\n"
                               "node 0 0000000000000000 program p -1 0 0 0\n"
                               "node 1 0000000000000001 query a 0 0 0 0\n"
                               "node 2 0000000000000002 query b 0 0 0 0\n"
                               "node 3 0000000000000003 state_update s 0 0 0 0\n"
                               "pin 1 out x A\n"
                               "pin 2 out y B\n"
                               "pin 3 in z C\n"
                               "edge 1 0 3 0\n"
                               "edge 2 0 3 0\n"; // second source into the same (to_node=3, to_pin=0) -> reject
    SourceModel bad3(&root);
    CHECK_FALSE(crd::chir::read_schema(StringView(double_writer), bad3));
}

// ============================ CEIR-32c — the CHIR TEXT projection (print_chir + parse_chir) ============================

// ⛔ Both committed assets (event_handler.chirgraph + event_handler.chir) were BOOTSTRAPPED via a hidden `[.chirboot]`
// case that wrote `print_schema(oracle)` + `print_chir(oracle)` to their paths; that write was STRIPPED after commit
// ([[feedback_committed_asset_antidrift_through_the_printer_not_bytes]] / the committed-asset recipe). The oracle
// `build_event_handler_model` is KEPT as the anti-drift SOURCE — if it changes, the anti-drift CHECK below fails, which
// is the signal to re-add the bootstrap, regen, verify, and strip again. (The 32c correction: SourceModel::add_edge now
// canonicalizes edge order, so the regenerated .chirgraph edge section is sorted by the consumer pin.)

TEST_CASE("chir 32c: event_handler.chir is anti-drift + a text FIXED-POINT (ADR-0128 D5)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    // ANTI-DRIFT THROUGH THE PRINTER: print_chir(oracle) == the committed .chir bytes (never file-bytes-first).
    SourceModel oracle(&root);
    build_event_handler_model(oracle);
    Array<char> t_oracle(&root);
    crd::chir::print_chir(oracle, t_oracle);
    const Array<char> t_file = slurp(kChirPath, &root);
    CHECK(sv(t_oracle) == sv(t_file));

    // TEXT FIXED-POINT: print_chir(parse_chir(file)) == file (a canonical document round-trips byte-exact).
    SourceModel                  parsed(&root);
    const crd::chir::ChirParseResult r = crd::chir::parse_chir(sv(t_file), 7U, parsed);
    CHECK(r.ok);
    Array<char> t_reprint(&root);
    crd::chir::print_chir(parsed, t_reprint);
    CHECK(sv(t_reprint) == sv(t_file));
}

TEST_CASE("chir 32c: the TEXT + GRAPH projections AGREE on semantics (ADR-0128 D5, the 32d precondition)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    SourceModel oracle(&root);
    build_event_handler_model(oracle);

    const Array<char> t_file = slurp(kChirPath, &root);
    SourceModel       from_text(&root);
    REQUIRE(crd::chir::parse_chir(sv(t_file), 1U, from_text).ok);

    // semantic parity to the oracle: semantic_hash ignores span + layout (§180 #10), which is exactly what the text form
    // does not carry — so a text parse (real spans, no layout) still hashes identically to the oracle (loc 0 + layout).
    CHECK(from_text.semantic_hash() == oracle.semantic_hash());

    // CROSS-PROJECTION TOOTH: the two COMMITTED assets lower from the SAME semantics — the literal 32d precondition.
    const Array<char> g_file = slurp(kAssetPath, &root);
    SourceModel       from_graph(&root);
    REQUIRE(crd::chir::read_schema(sv(g_file), from_graph));
    CHECK(from_text.semantic_hash() == from_graph.semantic_hash());
}

TEST_CASE("chir 32c: source spans SURVIVE the parse (contract item 6)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  t_file = slurp(kChirPath, &root);
    SourceModel                        parsed(&root);
    REQUIRE(crd::chir::parse_chir(sv(t_file), 42U, parsed).ok);

    // locate the `query` line in the committed text (no hardcoded number — the assertion self-checks against the asset).
    const StringView text = sv(t_file);
    crd::usize       qoff = text.size();
    for (crd::usize i = 0; i + 5U <= text.size(); ++i)
    {
        if (StringView(text.data() + i, 5) == StringView("query"))
        {
            qoff = i;
            break;
        }
    }
    REQUIRE(qoff != text.size());
    crd::u32 want_line = 1;
    for (crd::usize i = 0; i < qoff; ++i)
    {
        if (text[i] == '\n') { ++want_line; }
    }

    crd::u32 q = kInvalidNode;
    for (crd::u32 i = 0; i < parsed.node_count(); ++i)
    {
        if (parsed.node(i).kind == NodeKind::Query)
        {
            q = i;
            break;
        }
    }
    REQUIRE(q != kInvalidNode);
    CHECK(parsed.node(q).loc.line == want_line);   // the real text line, not 0
    CHECK(parsed.node(q).loc.file_id == 42U);       // the file_id we passed threads through
    CHECK(parsed.node(q).loc.col == 5U);            // depth-2 (4-space) indent + 1
}

TEST_CASE("chir 32c: parse_chir GRACEFULLY REJECTS malformed text with an EXACT line:col (contract item 6)", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    // an UNKNOWN node kind on line 2, column 3 (2-space indent under the program).
    const char* bad = "program p {\n"
                     "  bogus x\n"
                     "}\n";
    SourceModel                        m(&root);
    const crd::chir::ChirParseResult   r = crd::chir::parse_chir(StringView(bad), 0U, m);
    CHECK_FALSE(r.ok);
    CHECK(r.err_line == 2U); // IDENTITY, not "nonzero": the exact line
    CHECK(r.err_col == 3U);  // 2-space indent + 1

    // an in-pin bound to an UNKNOWN source node -> unresolved reference, rejected.
    const char* bad_ref = "program p {\n"
                        "  state_update s (in c: T = nope.x)\n"
                        "}\n";
    SourceModel m2(&root);
    CHECK_FALSE(crd::chir::parse_chir(StringView(bad_ref), 0U, m2).ok);

    // an OUT pin cannot bind a source.
    const char* out_bound = "program p {\n"
                          "  query q (out e: T = a.b)\n"
                          "}\n";
    SourceModel m3(&root);
    CHECK_FALSE(crd::chir::parse_chir(StringView(out_bound), 0U, m3).ok);

    // the ROOT must be a program: a bare `await task` at top level parses as a node but is rejected (not a program root).
    const char* not_a_program = "await task\n";
    SourceModel m4(&root);
    CHECK_FALSE(crd::chir::parse_chir(StringView(not_a_program), 0U, m4).ok);

    // an AMBIGUOUS edge source: two nodes named `q`, one referenced -> the name resolves to two nodes -> reject (never
    // silently first-match-wins). Sources are referenced by NAME, so a text-authored program must keep them unique.
    const char* ambiguous = "program p {\n"
                          "  query q (out e: T)\n"
                          "  query q (out e: T)\n"
                          "  state_update s (in c: T = q.e)\n"
                          "}\n";
    SourceModel m5(&root);
    CHECK_FALSE(crd::chir::parse_chir(StringView(ambiguous), 0U, m5).ok);
}

TEST_CASE("chir 32c: ANONYMOUS nodes round-trip through TEXT and reach the anon-ordinal id path", "[chir]")
{
    crd::memory::GrowableTlsfAllocator root;

    // §143 is fully named, so the text form's anonymous path (kind directly followed by a sibling kind / '}') is
    // otherwise untested. `await await` inside a block is TWO anonymous awaits (a name may not spell a kind keyword).
    const char* src = "program p {\n"
                     "  await await\n"
                     "}\n";
    SourceModel parsed(&root);
    REQUIRE(crd::chir::parse_chir(StringView(src), 0U, parsed).ok);
    REQUIRE(parsed.node_count() == 3U); // program + two anonymous awaits (NOT one await named `await`)
    CHECK(parsed.node(1).kind == NodeKind::Await);
    CHECK(parsed.node(2).kind == NodeKind::Await);
    CHECK(parsed.node(1).name.len == 0U);
    CHECK(parsed.node(2).name.len == 0U);
    CHECK(parsed.node(1).id.value != parsed.node(2).id.value); // the anon-ordinal path, now reached THROUGH text

    // and it is a TEXT fixed-point: re-printing + re-parsing yields the same semantics.
    Array<char> reprint(&root);
    crd::chir::print_chir(parsed, reprint);
    SourceModel reparsed(&root);
    REQUIRE(crd::chir::parse_chir(sv(reprint), 0U, reparsed).ok);
    CHECK(reparsed.semantic_hash() == parsed.semantic_hash());
}
