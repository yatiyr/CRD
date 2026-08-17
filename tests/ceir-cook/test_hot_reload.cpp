// CEIR-10a stage 2 - the hot-reload SUPERVISOR (ReloadSet). Cooks CEIR programs to CRDR blobs, loads them into a
// generation-managed set, and drives the reload lifecycle: a body-edit HOT-SWAPS (old handle goes stale, dependents
// untouched); a signature edit is a CONTRACT CHANGE -> REJECT + keeps last-good; a state-schema-only edit is
// NEEDS-MIGRATION -> reject in stage 2 (stage 3 migrates); the 8h dag (keyed content/CONTRACT hash) gives the
// affected-set; dup-symbol + AssetId-0 are loud rejects. Host subset: cross-program calls do NOT execute -- these tests
// assert DECISIONS, affected sets, handle staleness, and last-good. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/cook/plan_cache.hpp>   // CEIR-10b: the ReloadSet pairing rehearsal (the 10z inheritor)
#include <crd/ceir/cook/program_cook.hpp> // cook_program
#include <crd/ceir/exec.hpp>              // Interpreter / install_builtin_semantics / StateSnapshot (the mock runtime)
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility> // std::move

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)  -- ReloadSet / AssetId / cook_program
using namespace crd::ceir::exec; // NOLINT(google-build-using-namespace)  -- Interpreter / StateSnapshot / install_builtin_semantics
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;

namespace
{
struct Reg
{
    OpId cst, state;
    explicit Reg(Context& c) : cst(c.intern_op("arith", "const")), state(c.intern_op("core", "state"))
    {
        (void)arith::register_arith_ops(c);
        (void)core::register_core_ops(c);
        (void)func::register_dialect(c);
    }
};
void registrar(Context& c, void* /*user*/)
{
    (void)arith::register_arith_ops(c);
    (void)core::register_core_ops(c);
    (void)func::register_dialect(c);
}
Operation* konst(Context& c, const Reg& r, Block* b, i64 v, TypeId ty)
{
    Operation* const op = c.create_operation(r.cst, {}, 1U, ty);
    c.set_attr(op, "value", c.attr_int(v));
    b->append(op);
    return op;
}
void ret(Context& c, Block* b, Value* v)
{
    Value* ops[1] = {v};
    b->append(func::create_return(c, ConstSpan<Value*>(ops, 1U)));
}
void module_block(Context& c, Module& m)
{
    if (m.body()->first_block() == nullptr) { m.body()->append(c.create_block(0U)); }
}
ConstSpan<u8> span_of(const Array<u8>& b) { return ConstSpan<u8>(b.data(), b.size()); }

// @fname() -> (i64|i32) { return k }  -- a leaf program exporting `fname`. Signature toggles via `i64_result`.
Array<u8> cook_leaf(crd::memory::GrowableTlsfAllocator& root, u64 asset_id, StringView fname, i64 k, bool i64_result)
{
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    module_block(c, *m);
    const TypeId ty = i64_result ? c.type_i64() : c.type_i32();
    Operation* const f = func::create_func(c, *m, fname, Visibility::Public, 0U, ty);
    m->body()->first_block()->append(f);
    ret(c, func::func_body_block(f), konst(c, r, func::func_body_block(f), k, ty)->result(0U));
    CookResult cr = cook_program(c, *m, asset_id, &root, &root);
    REQUIRE(cr.ok());
    return std::move(cr.blob);
}
// @f() -> i32 { %i=0; %n=nextval; %s=state(%i,%n[,depth]); return %s }  -- same contract, state schema varies by depth.
Array<u8> cook_counter(crd::memory::GrowableTlsfAllocator& root, u64 asset_id, i64 nextval, u32 depth)
{
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    module_block(c, *m);
    Operation* const f = func::create_func(c, *m, "f", Visibility::Public, 0U, c.type_i32());
    m->body()->first_block()->append(f);
    Block* const b = func::func_body_block(f);
    Value* const init = konst(c, r, b, 0, c.type_i32())->result(0U);
    Value* const nxt  = konst(c, r, b, nextval, c.type_i32())->result(0U);
    Value* seed[2] = {init, nxt};
    Operation* const s = c.create_operation(r.state, ConstSpan<Value*>(seed, 2U), 1U, c.type_i32());
    if (depth != 1U) { c.set_attr(s, "depth", c.attr_int(static_cast<i64>(depth))); }
    b->append(s);
    ret(c, b, s->result(0U));
    CookResult cr = cook_program(c, *m, asset_id, &root, &root);
    REQUIRE(cr.ok());
    return std::move(cr.blob);
}
// @fname() -> i32 { call callee(); return 0 }  -- exports `fname`, imports `callee` (an external dependency edge).
Array<u8> cook_caller(crd::memory::GrowableTlsfAllocator& root, u64 asset_id, StringView fname, StringView callee)
{
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    module_block(c, *m);
    Operation* const f = func::create_func(c, *m, fname, Visibility::Public, 0U, c.type_i32());
    m->body()->first_block()->append(f);
    Block* const b = func::func_body_block(f);
    b->append(func::create_call(c, callee, {}, 0U, {}));
    ret(c, b, konst(c, r, b, 0, c.type_i32())->result(0U));
    CookResult cr = cook_program(c, *m, asset_id, &root, &root);
    REQUIRE(cr.ok());
    return std::move(cr.blob);
}

// A migration fn: reshape a depth-1 ring [v] into depth-2 [v, v] (⛔ copy the value first — a push_back self-reference
// would reallocate under it, the push-back-UAF scar).
bool mig_expand_depth(Array<StateSnapshot>& cells, void* /*user*/)
{
    for (crd::usize i = 0; i < cells.size(); ++i)
    {
        if (cells[i].ring.size() == 1U)
        {
            const i64 v = cells[i].ring[0];
            cells[i].ring.push_back(v);
        }
    }
    return true;
}
bool mig_refuse(Array<StateSnapshot>& /*cells*/, void* /*user*/) { return false; }

// The mock RUNTIME: build a fresh interpreter on a generation's Context, install builtin semantics, invoke @f().
i64 run_gen(Generation* g, Interpreter& in)
{
    install_builtin_semantics(in);
    const ExecResult r = in.invoke(*g->program.module, StringView("f"), ConstSpan<i64>());
    REQUIRE(r.ok());
    REQUIRE(r.values.size() == 1U);
    return r.values[0];
}

StringView sv(const String& s) { return StringView(s.data(), s.size()); }

// The SOURCE TEXT of @fname() -> (i64|i32) { return k } (via the printer — the §121 no-privileged-path form).
String source_leaf(crd::memory::GrowableTlsfAllocator& root, StringView fname, i64 k, bool i64_result)
{
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    module_block(c, *m);
    const TypeId ty = i64_result ? c.type_i64() : c.type_i32();
    Operation* const f = func::create_func(c, *m, fname, Visibility::Public, 0U, ty);
    m->body()->first_block()->append(f);
    ret(c, func::func_body_block(f), konst(c, r, func::func_body_block(f), k, ty)->result(0U));
    return print(c, *m, &root);
}

// A registrar that SMUGGLES the ReloadSet back in (via user) and attempts a re-entrant reload during load — the RAF-11
// scar. Records whether it fired and whether the re-entrant call was rejected.
struct ReentrantProbe
{
    ReloadSet*        set = nullptr;
    AssetId           id{};
    ConstSpan<crd::u8> blob;
    bool              fired    = false;
    bool              rejected = false;
};
void reentrant_registrar(Context& c, void* user)
{
    (void)arith::register_arith_ops(c);
    (void)core::register_core_ops(c);
    (void)func::register_dialect(c);
    auto* const p = static_cast<ReentrantProbe*>(user);
    if (p != nullptr && p->set != nullptr && !p->fired)
    {
        p->fired              = true;
        const ReloadResult rr = p->set->reload(p->id, p->blob); // ⛔ re-entrant — must be rejected, not corrupt state
        p->rejected           = rr.reentrant;
    }
}

// A PlanCache resolver backed by a live ReloadSet: a dep's current interface hash (0 if absent — EMPTY≠UNKNOWN).
u64 reloadset_resolve(AssetId a, void* user)
{
    const RuntimeProgram* const p = static_cast<ReloadSet*>(user)->program(a);
    return p != nullptr ? p->interface_hash : 0U;
}
} // namespace

TEST_CASE("ceir 10a: a body-only edit HOT-SWAPS and the old handle goes stale", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet set(&root, &registrar, nullptr);
    const Array<u8> b0 = cook_leaf(root, 100U, "foo", 5, false);
    REQUIRE(set.add(AssetId{100U}, span_of(b0)).ok());
    REQUIRE(set.size() == 1U);
    const ProgramHandle h1 = set.handle(AssetId{100U});
    CHECK(set.is_current(AssetId{100U}, h1));
    const u64 iface0 = set.program(AssetId{100U})->interface_hash;

    const Array<u8>    b1 = cook_leaf(root, 100U, "foo", 6, false); // same signature, different body constant
    const ReloadResult rr = set.reload(AssetId{100U}, span_of(b1));
    CHECK(rr.decision == ReloadDecision::HotSwap);
    CHECK(rr.installed);
    CHECK_FALSE(set.is_current(AssetId{100U}, h1));                 // the pre-reload handle is now stale
    CHECK(set.is_current(AssetId{100U}, set.handle(AssetId{100U}))); // the fresh handle is current
    CHECK(set.program(AssetId{100U})->interface_hash == iface0);    // interface unchanged
}

TEST_CASE("ceir 10a: a signature edit is a CONTRACT CHANGE - rejected, last-good kept", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet set(&root, &registrar, nullptr);
    const Array<u8> b0 = cook_leaf(root, 200U, "foo", 5, false); // -> i32
    REQUIRE(set.add(AssetId{200U}, span_of(b0)).ok());
    const u64           content0 = set.program(AssetId{200U})->content_hash;
    const ProgramHandle h1       = set.handle(AssetId{200U});

    const Array<u8>    b_sig = cook_leaf(root, 200U, "foo", 5, true); // -> i64 : the caller contract changed
    const ReloadResult rr   = set.reload(AssetId{200U}, span_of(b_sig));
    CHECK(rr.decision == ReloadDecision::ContractChange);
    CHECK_FALSE(rr.installed);
    CHECK(set.is_current(AssetId{200U}, h1));                       // last-good: the old handle is STILL current
    CHECK(set.program(AssetId{200U})->content_hash == content0);   // ...and the installed program is unchanged
}

TEST_CASE("ceir 10a: a state-schema-only edit is NEEDS-MIGRATION - rejected in stage 2", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet set(&root, &registrar, nullptr);
    const Array<u8> b0 = cook_counter(root, 300U, 7, /*depth=*/1U);
    REQUIRE(set.add(AssetId{300U}, span_of(b0)).ok());

    const Array<u8>    b_schema = cook_counter(root, 300U, 7, /*depth=*/2U); // same contract, different ring depth
    const ReloadResult rr      = set.reload(AssetId{300U}, span_of(b_schema));
    CHECK(rr.decision == ReloadDecision::NeedsMigration);
    CHECK_FALSE(rr.installed); // no migration-fn registry until stage 3
}

TEST_CASE("ceir 10a: an identical reload is a NO-CHANGE no-op", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet set(&root, &registrar, nullptr);
    const Array<u8> b0 = cook_leaf(root, 350U, "foo", 5, false);
    REQUIRE(set.add(AssetId{350U}, span_of(b0)).ok());
    const Array<u8>    b1 = cook_leaf(root, 350U, "foo", 5, false); // byte-identical program
    const ReloadResult rr = set.reload(AssetId{350U}, span_of(b1));
    CHECK(rr.decision == ReloadDecision::NoChange);
    CHECK_FALSE(rr.installed);
}

TEST_CASE("ceir 10a: the dep graph gives the affected set; dup-symbol and AssetId 0 are rejected", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet set(&root, &registrar, nullptr);
    const Array<u8> ba = cook_caller(root, 400U, "use", "bar"); // A imports "bar"
    const Array<u8> bb = cook_leaf(root, 500U, "bar", 0, false); // B exports "bar"
    REQUIRE(set.add(AssetId{400U}, span_of(ba)).ok());
    REQUIRE(set.add(AssetId{500U}, span_of(bb)).ok());

    // B changes -> A is the affected (recompiles-affected) set; A changes -> nothing depends on A.
    Array<AssetId> aff(&root);
    REQUIRE(set.affected(AssetId{500U}, aff));
    REQUIRE(aff.size() == 1U);
    CHECK(aff[0] == AssetId{400U});
    Array<AssetId> aff2(&root);
    REQUIRE(set.affected(AssetId{400U}, aff2));
    CHECK(aff2.size() == 0U);

    // a second program exporting "bar" collides.
    const Array<u8> bb2 = cook_leaf(root, 600U, "bar", 1, false);
    CHECK(set.add(AssetId{600U}, span_of(bb2)).error == AddError::DuplicateSymbol);
    CHECK(set.size() == 2U); // not added

    // AssetId 0 is a loud reject (the dag silently ignores node 0).
    CHECK(set.add(AssetId{0U}, span_of(bb)).error == AddError::InvalidAssetId);
}

TEST_CASE("ceir 10a: remove takes a program out of the set and the graph", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet set(&root, &registrar, nullptr);
    const Array<u8> ba = cook_caller(root, 700U, "use", "bar");
    const Array<u8> bb = cook_leaf(root, 800U, "bar", 0, false);
    REQUIRE(set.add(AssetId{700U}, span_of(ba)).ok());
    REQUIRE(set.add(AssetId{800U}, span_of(bb)).ok());
    Array<AssetId> aff(&root);
    REQUIRE(set.affected(AssetId{800U}, aff));
    CHECK(aff.size() == 1U); // A depends on B

    set.remove(AssetId{700U}); // drop the dependent
    CHECK(set.size() == 1U);
    CHECK_FALSE(set.contains(AssetId{700U}));
    Array<AssetId> aff2(&root);
    REQUIRE(set.affected(AssetId{800U}, aff2));
    CHECK(aff2.size() == 0U); // nothing depends on B anymore
}

// ─────────────────────────── stage 3: state migration (caller-driven) ───────────────────────────

TEST_CASE("ceir 10a: a registered fn INSTALLS a NeedsMigration reload and migrates the state value", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet       set(&root, &registrar, nullptr);
    const Array<u8> b1 = cook_counter(root, 900U, 7, /*depth=*/1U);
    REQUIRE(set.add(AssetId{900U}, span_of(b1)).ok());

    // mock runtime: run the OLD generation once -> returns 0 (init), the depth-1 cell latches to 7.
    Generation* const g0 = set.generation(AssetId{900U});
    Interpreter       old_in(*g0->ctx);
    CHECK(run_gen(g0, old_in) == 0);

    // register a depth-expanding fn, then reload the depth-2 version: NeedsMigration + fn PRESENT -> INSTALL.
    set.register_migration(AssetId{900U}, &mig_expand_depth, nullptr);
    const Array<u8>    b2 = cook_counter(root, 900U, 7, /*depth=*/2U); // same contract, ring depth 1 -> 2
    const ReloadResult rr = set.reload(AssetId{900U}, span_of(b2));
    CHECK(rr.decision == ReloadDecision::NeedsMigration);
    CHECK(rr.installed); // the fn's PRESENCE flipped it from Reject to install

    // caller-side value move: snapshot old -> expand [7] to [7,7] -> restore into a NEW interpreter on the new generation.
    Generation* const g1 = set.generation(AssetId{900U});
    Interpreter       new_in(*g1->ctx);
    install_builtin_semantics(new_in);
    const Migration mig = set.migration(AssetId{900U});
    const crd::u32  n   = migrate_state(old_in, new_in, *g1->program.module, mig.fn, mig.user, &root);
    CHECK(n == 1U);
    CHECK(new_in.invoke(*g1->program.module, StringView("f"), ConstSpan<i64>()).values[0] == 7); // migrated, not init 0
}

TEST_CASE("ceir 10a: a REFUSING migration fn restores nothing - the new generation init-fills", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet       set(&root, &registrar, nullptr);
    const Array<u8> b1 = cook_counter(root, 910U, 7, /*depth=*/1U);
    REQUIRE(set.add(AssetId{910U}, span_of(b1)).ok());
    Generation* const g0 = set.generation(AssetId{910U});
    Interpreter       old_in(*g0->ctx);
    CHECK(run_gen(g0, old_in) == 0); // warms to 7

    set.register_migration(AssetId{910U}, &mig_refuse, nullptr); // present ⇒ install; the refusal is caller-side
    const Array<u8>    b2 = cook_counter(root, 910U, 7, /*depth=*/2U);
    const ReloadResult rr = set.reload(AssetId{910U}, span_of(b2));
    CHECK(rr.installed); // presence gates install even for a fn that will refuse the value move

    Generation* const g1 = set.generation(AssetId{910U});
    Interpreter       new_in(*g1->ctx);
    install_builtin_semantics(new_in);
    const Migration mig = set.migration(AssetId{910U});
    const crd::u32  n   = migrate_state(old_in, new_in, *g1->program.module, mig.fn, mig.user, &root);
    CHECK(n == 0U);                                                                                 // refused
    CHECK(new_in.invoke(*g1->program.module, StringView("f"), ConstSpan<i64>()).values[0] == 0);    // init-filled, coherent
}

TEST_CASE("ceir 10a: migrate_state carries a cell VERBATIM on a HotSwap (fn == nullptr)", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet       set(&root, &registrar, nullptr);
    const Array<u8> b0 = cook_counter(root, 950U, 7, /*depth=*/1U);
    REQUIRE(set.add(AssetId{950U}, span_of(b0)).ok());
    Generation* const g0 = set.generation(AssetId{950U});
    Interpreter       old_in(*g0->ctx);
    CHECK(run_gen(g0, old_in) == 0); // warms to 7

    const Array<u8>    b1 = cook_counter(root, 950U, 8, /*depth=*/1U); // different body const (next=8) -> HotSwap
    const ReloadResult rr = set.reload(AssetId{950U}, span_of(b1));
    CHECK(rr.decision == ReloadDecision::HotSwap);
    CHECK(rr.installed);

    Generation* const g1 = set.generation(AssetId{950U});
    Interpreter       new_in(*g1->ctx);
    install_builtin_semantics(new_in);
    const crd::u32 n = migrate_state(old_in, new_in, *g1->program.module, nullptr, nullptr, &root); // verbatim
    CHECK(n == 1U);
    CHECK(new_in.invoke(*g1->program.module, StringView("f"), ConstSpan<i64>()).values[0] == 7); // the old cell carried
}

// ─────────────────────────── stage 4: the source-in detect seam + reentrant guard ───────────────────────────

TEST_CASE("ceir 10a: add_source + reload_source drive the lifecycle from TEXT (a body edit hot-swaps)", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet    set(&root, &registrar, nullptr);
    const String s0 = source_leaf(root, "foo", 5, false);
    REQUIRE(set.add_source(AssetId{1000U}, sv(s0)).ok());
    const ProgramHandle h1 = set.handle(AssetId{1000U});

    const String       s1 = source_leaf(root, "foo", 6, false); // a body edit (same signature)
    const ReloadResult rr = set.reload_source(AssetId{1000U}, sv(s1));
    CHECK(rr.cook_error == CookError::Ok);
    CHECK(rr.decision == ReloadDecision::HotSwap);
    CHECK(rr.installed);
    CHECK_FALSE(set.is_current(AssetId{1000U}, h1)); // the old handle went stale through the source path
}

TEST_CASE("ceir 10a: a BAD source edit is CookFailed - last-good keeps running", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet    set(&root, &registrar, nullptr);
    const String s0 = source_leaf(root, "foo", 5, false);
    REQUIRE(set.add_source(AssetId{1100U}, sv(s0)).ok());
    const crd::u64      content0 = set.program(AssetId{1100U})->content_hash;
    const ProgramHandle h1       = set.handle(AssetId{1100U});

    // a syntactically broken source: the cook must fail and NOTHING may change.
    const ReloadResult rr = set.reload_source(AssetId{1100U}, StringView("this is not valid ceir {{{"));
    CHECK(rr.cook_error != CookError::Ok);   // ParseFailed (a cook failure, distinct from a load failure)
    CHECK_FALSE(rr.installed);
    CHECK(set.is_current(AssetId{1100U}, h1));                    // last-good is STILL current
    CHECK(set.program(AssetId{1100U})->content_hash == content0); // ...and unchanged

    // add_source of bad text is CookFailed and adds nothing.
    const AddResult ar = set.add_source(AssetId{1200U}, StringView("garbage"));
    CHECK(ar.error == AddError::CookFailed);
    CHECK_FALSE(set.contains(AssetId{1200U}));
}

TEST_CASE("ceir 10a: a source SIGNATURE edit is a ContractChange - rejected via the source path", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet    set(&root, &registrar, nullptr);
    const String s0 = source_leaf(root, "foo", 5, false); // -> i32
    REQUIRE(set.add_source(AssetId{1300U}, sv(s0)).ok());
    const String       s_sig = source_leaf(root, "foo", 5, true); // -> i64
    const ReloadResult rr    = set.reload_source(AssetId{1300U}, sv(s_sig));
    CHECK(rr.decision == ReloadDecision::ContractChange);
    CHECK_FALSE(rr.installed);
}

TEST_CASE("ceir 10a: a REENTRANT reload from inside the registrar is rejected (RAF-11 guard)", "[ceir][reload]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReentrantProbe  probe{};
    ReloadSet       set(&root, &reentrant_registrar, &probe);
    const Array<u8> b0 = cook_leaf(root, 1400U, "foo", 5, false);
    probe.set  = &set;
    probe.id   = AssetId{1400U};
    probe.blob = span_of(b0);

    // add() runs the registrar during load; the registrar re-enters reload() -> the guard rejects it.
    const AddResult ar = set.add(AssetId{1400U}, span_of(b0));
    CHECK(ar.ok());        // the OUTER add still succeeds
    CHECK(probe.fired);    // the registrar DID attempt a re-entrant reload
    CHECK(probe.rejected); // ...and it was rejected as reentrant (state not corrupted)
    CHECK(set.size() == 1U);
}

// ─────────────────────────── CEIR-10b: the PlanCache × ReloadSet pairing (the 10z rehearsal) ───────────────────────────

TEST_CASE("ceir 10b: a callee interface change stales exactly the ReloadSet affected set of plans", "[ceir][plancache]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet set(&root, &registrar, nullptr);
    const Array<u8> ba = cook_caller(root, 2000U, "usea", "bar"); // A imports "bar"
    const Array<u8> bb = cook_leaf(root, 2100U, "bar", 0, false); // B exports bar()->i32
    const Array<u8> bc = cook_leaf(root, 2200U, "cee", 0, false); // C independent
    REQUIRE(set.add(AssetId{2000U}, span_of(ba)).ok());
    REQUIRE(set.add(AssetId{2100U}, span_of(bb)).ok());
    REQUIRE(set.add(AssetId{2200U}, span_of(bc)).ok());

    // the recompile PREDICTION: which plans a change to B stales = affected(B) ∪ {B}.
    Array<AssetId> aff(&root);
    REQUIRE(set.affected(AssetId{2100U}, aff));
    REQUIRE(aff.size() == 1U);
    CHECK(aff[0] == AssetId{2000U}); // A depends on B; C does not

    PlanCache      cache(&root, &reloadset_resolve, &set);
    const u64      tgt = plan_target(StringView("host"));
    const u8       plan[1] = {1U};
    // cache each program's plan; A records a dep on B at B's CURRENT interface hash.
    const PlanKey  ka{set.program(AssetId{2000U})->content_hash, tgt, 1U};
    const PlanDep  dep_a[1] = {{AssetId{2100U}, set.program(AssetId{2100U})->interface_hash}};
    cache.put(AssetId{2000U}, ka, ConstSpan<u8>(plan, 1U), ConstSpan<PlanDep>(dep_a, 1U));
    const PlanKey kb{set.program(AssetId{2100U})->content_hash, tgt, 1U};
    cache.put(AssetId{2100U}, kb, ConstSpan<u8>(plan, 1U), ConstSpan<PlanDep>());
    const PlanKey kc{set.program(AssetId{2200U})->content_hash, tgt, 1U};
    cache.put(AssetId{2200U}, kc, ConstSpan<u8>(plan, 1U), ConstSpan<PlanDep>());
    REQUIRE(cache.get(ka).hit());
    REQUIRE(cache.get(kb).hit());
    REQUIRE(cache.get(kc).hit());

    // COLD-reload B with a new SIGNATURE (a contract change is Reject via reload, so remove + re-add): new interface + content.
    set.remove(AssetId{2100U});
    const Array<u8> bb2 = cook_leaf(root, 2100U, "bar", 0, true); // bar()->i64 : a new interface
    REQUIRE(set.add(AssetId{2100U}, span_of(bb2)).ok());

    // A's plan: its recorded dep on B drifted → StaleDeps (A must recompile).
    CHECK(cache.get(ka).status == PlanStatus::StaleDeps);
    // B's plan: B's new content → a new key → Miss by construction (B must recompile).
    const PlanKey kb2{set.program(AssetId{2100U})->content_hash, tgt, 1U};
    CHECK(cache.get(kb2).status == PlanStatus::Miss);
    // C's plan: unaffected → still a HIT (NOT in affected(B)).
    CHECK(cache.get(kc).hit());
}
