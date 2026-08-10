#pragma once

// crd-ceir-cook — CEIR-10a HOT-RELOAD SUPERVISOR (stage 2). A `ReloadSet` owns a set of loaded CEIR programs (each in
// its OWN per-generation Context — the 7b wholesale-free pattern) and runs the reload lifecycle: load a new cooked blob
// → DECIDE (HotSwap / NeedsMigration / ContractChange) → atomic install-or-reject (last-good on reject). The dependent-
// safety analysis IS the 8h `IncrementalDag` keyed `(content_hash, contract_hash)`: a CONTRACT change recomputes the
// transitive dependents (Reject, to avoid a mixed-generation call graph); a content-ONLY change hot-swaps (dependents
// stay valid). ⛔ Dependent-safety keys on `contract_hash`, NOT `interface_hash` — callers depend on the contract, not on
// a program's internal §20 state schema. State-VALUE migration (NeedsMigration → the Interpreter cell move) is CEIR-10a
// STAGE 3. Design: docs/design/ceir-10a-hot-reload-and-state-migration.md §8.

#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/program_cook.hpp>    // cook_program_text / CookError (the source-in entry — CEIR-10a stage 4)
#include <crd/ceir/cook/runtime_program.hpp> // RuntimeProgram / ProgramSlot / ProgramHandle / LoadError
#include <crd/ceir/exec.hpp>                  // exec::Interpreter / exec::StateSnapshot (crd-ceir-cook already links crd-ceir)
#include <crd/containers/array.hpp>
#include <crd/containers/incremental_dag.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/identity.hpp> // AssetId

namespace crd::ceir::cook
{
using AssetId = crd::renderasset::AssetId;

// A registrar installs the module's dialects into a FRESH per-generation Context. ⛔ Run immediately after the Context is
// constructed, BEFORE load_program (its registration check runs first — the register-to-verify contract). Mirrors the
// crd-resources ReloadCallback / crd-ceir RewritePattern function-pointer idiom (an open-world hook, no vtable).
using Registrar = void (*)(Context& ctx, void* user);

// A state MIGRATION function (CEIR-10a stage 3). Transforms the OLD generation's §20 cell snapshot IN PLACE so it fits
// the NEW generation's schema (e.g. reshape a depth-1 ring into depth-2). ⛔ It touches ONLY the snapshot array — never
// an interpreter (those belong to the caller). Returns false ⇒ migration REFUSED (the caller does not restore → the new
// generation init-fills; state is lost but coherent). Registered per-asset (last-registration-wins); its PRESENCE is
// what flips a NeedsMigration reload from Reject to install (the fn itself runs caller-side, in `migrate_state`).
using MigrationFn = bool (*)(containers::Array<exec::StateSnapshot>& cells, void* user);

struct Migration
{
    MigrationFn fn   = nullptr;
    void*       user = nullptr;
};

// One loaded GENERATION: its own Context (freed WHOLESALE on retire — arenas free per-Context) + the runtime program the
// slot points at. ⛔ A HEAP UNIT — never stored by value in a growable array, because `ProgramSlot::install` keeps a RAW
// `RuntimeProgram*`; array growth would move the program and dangle every installed pointer + minted handle.
struct Generation
{
    Context*       ctx = nullptr;
    RuntimeProgram program;
};

// The reload DECISION — the reloaded program's stored hashes vs the freshly-loaded candidate's.
// NOLINTNEXTLINE(performance-enum-size)
enum class ReloadDecision : crd::u8
{
    NoChange,       // content hash unchanged — a no-op (candidate destroyed)
    HotSwap,        // interface unchanged (contract + state schema equal) — INSTALL; dependents untouched
    NeedsMigration, // contract equal, ONLY the §20 state schema changed — stage 2 REJECTS (no fn registry); stage 3 migrates
    ContractChange, // the caller contract changed (signature/effects/caps) — REJECT (keep last-good); callers would break
};
[[nodiscard]] containers::StringView reload_decision_name(ReloadDecision d) noexcept;

// Why an add() failed.
// NOLINTNEXTLINE(performance-enum-size)
enum class AddError : crd::u8
{
    Ok,
    InvalidAssetId,  // id == 0 (the dag SILENTLY ignores node 0 — a loud reject instead)
    AlreadyPresent,  // the id is already in the set (use reload)
    CookFailed,      // a source add (add_source) did not verify+cook (see cook_error) — distinct from a load failure
    LoadFailed,      // load_program rejected the blob (see load_error)
    DuplicateSymbol, // the candidate exports a symbol another program already exports (loud, never last-write-wins)
    Reentrant,       // ⛔ called re-entrantly (a mutation from inside a registrar / migration fn) — the RAF-11 guard
};
[[nodiscard]] containers::StringView add_error_name(AddError e) noexcept;

struct AddResult
{
    AddError  error      = AddError::Ok;
    LoadError load_error = LoadError::Ok;  // valid iff error == LoadFailed
    CookError cook_error = CookError::Ok;  // valid iff error == CookFailed
    [[nodiscard]] bool ok() const noexcept { return error == AddError::Ok; }
};

struct ReloadResult
{
    ReloadDecision decision   = ReloadDecision::NoChange;
    bool           installed  = false;         // did the candidate replace the current generation?
    bool           load_ok    = true;          // false ⇒ the blob did not load (see load_error) or the id was absent
    bool           reentrant  = false;         // ⛔ true ⇒ rejected as a re-entrant call (nothing happened)
    LoadError      load_error = LoadError::Ok; // valid iff a load was attempted and failed
    CookError      cook_error = CookError::Ok; // valid iff a source reload did not cook (reload_source)
};

// A set of hot-reloadable CEIR programs under one reload authority. ⛔ NOT thread-safe (single-thread reload authority).
class ReloadSet
{
public:
    ReloadSet(memory::IAllocator* alloc, Registrar reg, void* user);
    ~ReloadSet();
    ReloadSet(const ReloadSet&)            = delete;
    ReloadSet& operator=(const ReloadSet&) = delete;
    ReloadSet(ReloadSet&&)                 = delete;
    ReloadSet& operator=(ReloadSet&&)      = delete;

    // Load a program into the set for the first time. id==0 → InvalidAssetId; a present id → AlreadyPresent; a blob that
    // fails to load → LoadFailed(load_error); a symbol another program already exports → DuplicateSymbol.
    AddResult add(AssetId id, containers::ConstSpan<crd::u8> blob);

    // Run the reload lifecycle for a PRESENT id (add first). An absent id is a no-op (load_ok=false). Loads the candidate,
    // decides, and installs ONLY on HotSwap (stage 2); NeedsMigration / ContractChange / NoChange keep last-good.
    ReloadResult reload(AssetId id, containers::ConstSpan<crd::u8> blob);

    // ---- CEIR-10a stage 4: the SOURCE-in entry (the 7a CookDb/register_cook_handler routing, host-subset form) ----
    // VERIFY+COOK `source` in a transient Context (⛔ per-call — construct/cook/destroy, never cached; text ≡ builder, the
    // §121 no-privileged-path) then delegate to `add` / `reload`. A cook that fails (parse / a verifier reject) is a
    // DISTINCT typed outcome (`CookFailed` / `cook_error`) that installs + destroys NOTHING → last-good keeps running (the
    // most common real hot-reload event). The mtime/filesystem SIGNAL that supplies `source` is production I/O
    // (named-forward) — this is the seam it invokes; ⛔ CEIR owns the swap, never the ResourceManager (two-swap-authority).
    AddResult    add_source(AssetId id, containers::StringView source);
    ReloadResult reload_source(AssetId id, containers::StringView source);

    // Remove a program (the cold-reload half — the honest path for a contract change). Rebuilds the graph.
    void remove(AssetId id);

    // Free retired (zombie) generations. Called after every successful install; exposed for shutdown / tests.
    void drain();

    // Register (or replace — last-registration-wins) a state migration fn for a PRESENT asset (register AFTER add). Its
    // PRESENCE flips a NeedsMigration reload from Reject to install; the caller fetches + applies it via `migration()` +
    // `migrate_state`. A no-op if the id is absent. Dropped when the asset is removed (a re-added asset is a new contract).
    void register_migration(AssetId id, MigrationFn fn, void* user);
    // The registered migration for `id` ({nullptr,nullptr} if none / absent).
    [[nodiscard]] Migration migration(AssetId id) const;

    [[nodiscard]] bool                  contains(AssetId id) const noexcept;
    [[nodiscard]] crd::usize            size() const noexcept;
    [[nodiscard]] ProgramHandle         handle(AssetId id) const;                       // an invalid handle if absent
    [[nodiscard]] bool                  is_current(AssetId id, const ProgramHandle& h) const; // false if absent/stale
    [[nodiscard]] const RuntimeProgram* program(AssetId id) const;                      // nullptr if absent
    [[nodiscard]] Generation*           generation(AssetId id) const;                   // the CURRENT gen (stage-3 migration)
    // The transitive DEPENDENTS of `id` (the "recompiles-affected" set) via the 8h dag. false ⇒ a cycle.
    [[nodiscard]] bool affected(AssetId id, containers::Array<AssetId>& out) const;

private:
    struct Entry
    {
        AssetId       id{};
        ProgramSlot   slot{};                     // generation-tagged handles (RAF-11 staleness) — points at current->program
        ProgramHandle current_handle{};           // the handle the last install minted (what handle() returns)
        Generation*   current = nullptr;
        Generation*   zombie  = nullptr;          // the previous generation, awaiting drain (one-deep grace)
        crd::u64      content_hash   = 0U;
        crd::u64      contract_hash  = 0U;
        crd::u64      interface_hash = 0U;
        MigrationFn   migration_fn   = nullptr; // stage-3: presence flips NeedsMigration from Reject to install
        void*         migration_user = nullptr;
    };

    [[nodiscard]] Entry*       find(AssetId id) noexcept;
    [[nodiscard]] const Entry* find(AssetId id) const noexcept;
    [[nodiscard]] Generation*  alloc_generation();
    [[nodiscard]] Generation*  load_generation(containers::ConstSpan<crd::u8> blob, LoadError& out_err);
    void                       destroy_generation(Generation* g) noexcept;
    // Does `cand`'s module Publicly export a symbol some OTHER entry (id != self) already exports?
    [[nodiscard]] bool         exports_collide(const Generation* cand, AssetId self) const;
    void                       rebuild_graph();
    // Guard-free cores (the public entries + the source entries set the reentrant guard, then delegate here so a source
    // add/reload does not re-trip the guard on its internal call).
    [[nodiscard]] AddResult    add_impl(AssetId id, containers::ConstSpan<crd::u8> blob);
    [[nodiscard]] ReloadResult reload_impl(AssetId id, containers::ConstSpan<crd::u8> blob);
    // Cook `source` in a transient Context; on success delegates to `deleg` (add_impl / reload_impl via a tiny thunk). On a
    // cook failure sets `out_cook`. Returns whether the cook succeeded.
    [[nodiscard]] bool         cook_source(AssetId id, containers::StringView source, containers::Array<crd::u8>& out_blob,
                                           CookError& out_cook);

    memory::IAllocator*        m_alloc;
    Registrar                  m_reg;
    void*                      m_user;
    containers::Array<Entry>   m_entries;
    containers::IncrementalDag m_dag;
    bool                       m_reloading = false; // ⛔ the RAF-11 reentrant guard (a mutation from a registrar/fn rejects)
};

// Move live §20 state from an OLD execution session into a NEW one across a generation swap (CEIR-10a stage 3). Snapshots
// `old_in`'s cells by 8d stable id → (if `fn` != nullptr) transforms them via `fn` → restores into `new_in` against
// `new_module`. `fn == nullptr` = VERBATIM (the HotSwap / CompatibleReuse path — same helper). A `fn` returning false =
// REFUSED → nothing is restored (the new session init-fills). Returns the number of cells actually restored. ⛔ The
// interpreters are the CALLER's — the ReloadSet never owns one (live state is populated by execution, which it does not do).
[[nodiscard]] crd::u32 migrate_state(const exec::Interpreter& old_in, exec::Interpreter& new_in, const Module& new_module,
                                     MigrationFn fn, void* user, memory::IAllocator* scratch);
} // namespace crd::ceir::cook
