#include <crd/ceir/cook/hot_reload.hpp>

#include <crd/ceir/attr.hpp>          // AttrValue / AttrKind
#include <crd/ceir/func.hpp>          // func_kind
#include <crd/ceir/ir.hpp>            // Operation / Module / Block
#include <crd/ceir/program_asset.hpp> // interface_hash / contract_hash / collect_dependencies / DependencyRecord
#include <crd/ceir/symbol_table.hpp>  // Visibility / SymbolEntry / SymbolTable

#include <new>     // placement new
#include <utility> // std::move (via containers)

namespace crd::ceir::cook
{
containers::StringView reload_decision_name(ReloadDecision d) noexcept
{
    switch (d) // ⛔ no default: a new decision must be named here (-Werror=switch)
    {
    case ReloadDecision::NoChange: return containers::StringView("no-change");
    case ReloadDecision::HotSwap: return containers::StringView("hot-swap");
    case ReloadDecision::NeedsMigration: return containers::StringView("needs-migration");
    case ReloadDecision::ContractChange: return containers::StringView("contract-change");
    }
    return containers::StringView("?");
}

containers::StringView add_error_name(AddError e) noexcept
{
    switch (e) // ⛔ no default (-Werror=switch)
    {
    case AddError::Ok: return containers::StringView("ok");
    case AddError::InvalidAssetId: return containers::StringView("invalid-asset-id");
    case AddError::AlreadyPresent: return containers::StringView("already-present");
    case AddError::CookFailed: return containers::StringView("cook-failed");
    case AddError::LoadFailed: return containers::StringView("load-failed");
    case AddError::DuplicateSymbol: return containers::StringView("duplicate-symbol");
    case AddError::Reentrant: return containers::StringView("reentrant");
    }
    return containers::StringView("?");
}

namespace
{
// RAII for the RAF-11 reentrant guard: sets the flag on construction, clears it on ANY scope exit (every return path).
struct GuardScope
{
    bool& flag;
    explicit GuardScope(bool& f) noexcept : flag(f) { flag = true; }
    ~GuardScope() { flag = false; }
    GuardScope(const GuardScope&)            = delete;
    GuardScope& operator=(const GuardScope&) = delete;
    GuardScope(GuardScope&&)                 = delete;
    GuardScope& operator=(GuardScope&&)      = delete;
};

// Every PUBLICLY-exported func symbol of a generation's module → `out` (StringViews into that generation's Context arena;
// valid only while the Context is live). Top-level func ops only (funcs are module-body children).
void collect_exports(const Generation* g, containers::Array<containers::StringView>& out)
{
    if (g == nullptr || g->program.module == nullptr) { return; }
    Context&           ctx  = *g->ctx;
    const Module&      m    = *g->program.module;
    const SymbolTable* syms = m.symbols();
    if (m.body() == nullptr) { return; }
    const OpId fk = func::func_kind(ctx);
    for (Block* b = m.body()->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->kind() != fk) { continue; }
            const AttrId nid = op->attr("sym_name");
            if (!nid.valid()) { continue; }
            const AttrValue v = ctx.attr_value(nid);
            if (v.kind != AttrKind::String && v.kind != AttrKind::SymbolRef) { continue; }
            Visibility vis = Visibility::Public; // absent from the table ⇒ conservatively exported
            if (syms != nullptr)
            {
                if (const SymbolEntry* const e = syms->lookup(v.s)) { vis = e->visibility; }
            }
            if (vis == Visibility::Public) { out.push_back(v.s); }
        }
    }
}
} // namespace

ReloadSet::ReloadSet(memory::IAllocator* alloc, Registrar reg, void* user)
    : m_alloc(alloc), m_reg(reg), m_user(user), m_entries(alloc), m_dag(alloc)
{
}

ReloadSet::~ReloadSet()
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        destroy_generation(m_entries[i].zombie);
        destroy_generation(m_entries[i].current);
    }
}

ReloadSet::Entry* ReloadSet::find(AssetId id) noexcept
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].id == id) { return &m_entries[i]; }
    }
    return nullptr;
}

const ReloadSet::Entry* ReloadSet::find(AssetId id) const noexcept
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].id == id) { return &m_entries[i]; }
    }
    return nullptr;
}

Generation* ReloadSet::alloc_generation()
{
    void* const gm = m_alloc->allocate(sizeof(Generation), alignof(Generation));
    Generation* const g = new (gm) Generation();
    void* const cm = m_alloc->allocate(sizeof(Context), alignof(Context));
    g->ctx = new (cm) Context(m_alloc);
    return g;
}

void ReloadSet::destroy_generation(Generation* g) noexcept
{
    if (g == nullptr) { return; }
    if (g->ctx != nullptr)
    {
        g->ctx->~Context();
        m_alloc->deallocate(g->ctx);
    }
    g->~Generation();
    m_alloc->deallocate(g);
}

Generation* ReloadSet::load_generation(containers::ConstSpan<crd::u8> blob, LoadError& out_err)
{
    Generation* const g = alloc_generation();
    m_reg(*g->ctx, m_user); // register the module's dialects BEFORE load_program (its registration check runs first)
    const LoadResult lr = load_program(*g->ctx, blob, m_alloc, m_alloc);
    if (!lr.ok())
    {
        out_err = lr.error;
        destroy_generation(g);
        return nullptr;
    }
    g->program = lr.program;
    out_err    = LoadError::Ok;
    return g;
}

bool ReloadSet::exports_collide(const Generation* cand, AssetId self) const
{
    containers::Array<containers::StringView> ce(m_alloc);
    collect_exports(cand, ce);
    if (ce.size() == 0U) { return false; }
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        const Entry& e = m_entries[i];
        if (e.id == self || e.current == nullptr) { continue; }
        containers::Array<containers::StringView> oe(m_alloc);
        collect_exports(e.current, oe);
        for (crd::usize a = 0; a < ce.size(); ++a)
        {
            for (crd::usize b = 0; b < oe.size(); ++b)
            {
                if (ce[a] == oe[b]) { return true; }
            }
        }
    }
    return false;
}

void ReloadSet::rebuild_graph()
{
    m_dag = containers::IncrementalDag(m_alloc); // wholesale rebuild — arena-string edges die with a retired Context
    // symbol → owner id (parallel arrays; sets are small — the advisor's O(n) rebuild).
    containers::Array<containers::StringView> sym_names(m_alloc);
    containers::Array<crd::u64>               sym_owner(m_alloc);
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        const Entry& e = m_entries[i];
        if (e.current == nullptr) { continue; }
        m_dag.add_node(e.id.value);
        m_dag.set_revision(e.id.value, e.content_hash, e.contract_hash); // ⛔ interface rev = contract_hash (dependent-safety)
        containers::Array<containers::StringView> ex(m_alloc);
        collect_exports(e.current, ex);
        for (crd::usize k = 0; k < ex.size(); ++k)
        {
            sym_names.push_back(ex[k]);
            sym_owner.push_back(e.id.value);
        }
    }
    // edges: A depends on B iff A calls a symbol B exports.
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        const Entry& a = m_entries[i];
        if (a.current == nullptr) { continue; }
        const DependencyRecord deps = collect_dependencies(*a.current->ctx, *a.current->program.module, m_alloc);
        for (crd::usize c = 0; c < deps.called_funcs.size(); ++c)
        {
            const containers::StringView cn = deps.called_funcs[c];
            for (crd::usize k = 0; k < sym_names.size(); ++k)
            {
                if (sym_names[k] == cn && sym_owner[k] != a.id.value) { m_dag.add_edge(a.id.value, sym_owner[k]); }
            }
        }
    }
}

AddResult ReloadSet::add(AssetId id, containers::ConstSpan<crd::u8> blob)
{
    if (m_reloading) { return AddResult{AddError::Reentrant}; } // ⛔ RAF-11: a mutation from inside a registrar/fn
    const GuardScope gs(m_reloading);
    return add_impl(id, blob);
}

AddResult ReloadSet::add_impl(AssetId id, containers::ConstSpan<crd::u8> blob)
{
    if (id.value == 0U) { return AddResult{AddError::InvalidAssetId, LoadError::Ok}; } // the dag silently drops node 0
    if (find(id) != nullptr) { return AddResult{AddError::AlreadyPresent, LoadError::Ok}; }
    LoadError         le   = LoadError::Ok;
    Generation* const cand = load_generation(blob, le);
    if (cand == nullptr) { return AddResult{AddError::LoadFailed, le}; }
    if (exports_collide(cand, id))
    {
        destroy_generation(cand);
        return AddResult{AddError::DuplicateSymbol, LoadError::Ok};
    }
    Entry e;
    e.id             = id;
    e.current        = cand;
    e.content_hash   = cand->program.content_hash;
    e.interface_hash = cand->program.interface_hash;
    e.contract_hash  = contract_hash(*cand->ctx, *cand->program.module, m_alloc);
    e.current_handle = e.slot.install(&cand->program, id);
    m_entries.push_back(e); // Entry copied; slot/handle copied; the heap Generation is stable, so the raw ptr survives
    rebuild_graph();
    return AddResult{AddError::Ok, LoadError::Ok};
}

ReloadResult ReloadSet::reload(AssetId id, containers::ConstSpan<crd::u8> blob)
{
    if (m_reloading) { return ReloadResult{.load_ok = false, .reentrant = true}; } // ⛔ RAF-11 reentrant guard
    const GuardScope gs(m_reloading);
    return reload_impl(id, blob);
}

ReloadResult ReloadSet::reload_impl(AssetId id, containers::ConstSpan<crd::u8> blob)
{
    Entry* const e = find(id);
    if (e == nullptr) { return ReloadResult{.load_ok = false}; } // absent — no-op
    LoadError         le   = LoadError::Ok;
    Generation* const cand = load_generation(blob, le);
    if (cand == nullptr) { return ReloadResult{.load_ok = false, .load_error = le}; }

    const crd::u64 nc = cand->program.content_hash;
    const crd::u64 ni = cand->program.interface_hash;
    const crd::u64 nk = contract_hash(*cand->ctx, *cand->program.module, m_alloc);

    ReloadDecision dec;
    if (nc == e->content_hash) { dec = ReloadDecision::NoChange; }
    else if (ni == e->interface_hash) { dec = ReloadDecision::HotSwap; }
    else if (nk == e->contract_hash) { dec = ReloadDecision::NeedsMigration; } // only the §20 state schema changed → stage 3
    else { dec = ReloadDecision::ContractChange; }

    ReloadResult r;
    r.decision = dec;
    r.load_ok  = true;
    // stage 2: HotSwap installs. stage 3: NeedsMigration installs IFF a migration fn is registered (its PRESENCE gates;
    // the fn itself runs caller-side in migrate_state). NoChange / ContractChange / NeedsMigration-without-fn keep last-good.
    const bool do_install =
        dec == ReloadDecision::HotSwap || (dec == ReloadDecision::NeedsMigration && e->migration_fn != nullptr);
    if (do_install)
    {
        destroy_generation(e->zombie); // drain the previous zombie (one-deep grace)
        e->zombie         = e->current;
        e->current        = cand;
        e->content_hash   = nc;
        e->contract_hash  = nk;
        e->interface_hash = ni;
        e->current_handle = e->slot.install(&cand->program, id); // bumps the generation → old handles go stale
        rebuild_graph();
        r.installed = true;
    }
    else
    {
        destroy_generation(cand); // reject / no-change: the candidate was never installed → destroy it (the leak path)
        r.installed = false;
    }
    return r;
}

bool ReloadSet::cook_source(AssetId id, containers::StringView source, containers::Array<crd::u8>& out_blob,
                            CookError& out_cook)
{
    // transient cook Context — construct / register / cook / destroy (never cached); the cooked bytes are self-contained.
    void* const    cm   = m_alloc->allocate(sizeof(Context), alignof(Context));
    Context* const cctx = new (cm) Context(m_alloc);
    m_reg(*cctx, m_user); // register dialects so the cook-time verifiers are not vacuous (§121 text ≡ builder)
    CookResult cr = cook_program_text(*cctx, source, id.value, m_alloc, m_alloc);
    out_cook       = cr.error;
    const bool cok = cr.ok();
    if (cok) { out_blob = std::move(cr.blob); } // move out BEFORE the cook Context dies (bytes are m_alloc-owned)
    cctx->~Context();
    m_alloc->deallocate(cctx);
    return cok;
}

AddResult ReloadSet::add_source(AssetId id, containers::StringView source)
{
    if (m_reloading) { return AddResult{AddError::Reentrant}; }
    const GuardScope           gs(m_reloading);
    containers::Array<crd::u8> blob(m_alloc);
    CookError                  ce = CookError::Ok;
    if (!cook_source(id, source, blob, ce)) { return AddResult{AddError::CookFailed, LoadError::Ok, ce}; }
    return add_impl(id, containers::ConstSpan<crd::u8>(blob.data(), blob.size()));
}

ReloadResult ReloadSet::reload_source(AssetId id, containers::StringView source)
{
    if (m_reloading) { return ReloadResult{.load_ok = false, .reentrant = true}; }
    const GuardScope           gs(m_reloading);
    containers::Array<crd::u8> blob(m_alloc);
    CookError                  ce = CookError::Ok;
    if (!cook_source(id, source, blob, ce)) { return ReloadResult{.load_ok = false, .cook_error = ce}; }
    return reload_impl(id, containers::ConstSpan<crd::u8>(blob.data(), blob.size()));
}

void ReloadSet::remove(AssetId id)
{
    if (m_reloading) { return; } // ⛔ RAF-11 reentrant guard — a remove from inside a registrar/fn is ignored
    const GuardScope gs(m_reloading);
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].id != id) { continue; }
        destroy_generation(m_entries[i].zombie);
        destroy_generation(m_entries[i].current);
        m_entries[i] = m_entries[m_entries.size() - 1U]; // swap-with-last (entry order is irrelevant)
        m_entries.pop_back();
        rebuild_graph();
        return;
    }
}

void ReloadSet::drain()
{
    for (crd::usize i = 0; i < m_entries.size(); ++i)
    {
        destroy_generation(m_entries[i].zombie);
        m_entries[i].zombie = nullptr;
    }
}

bool ReloadSet::contains(AssetId id) const noexcept { return find(id) != nullptr; }

crd::usize ReloadSet::size() const noexcept { return m_entries.size(); }

ProgramHandle ReloadSet::handle(AssetId id) const
{
    const Entry* const e = find(id);
    return e != nullptr ? e->current_handle : ProgramHandle{};
}

bool ReloadSet::is_current(AssetId id, const ProgramHandle& h) const
{
    const Entry* const e = find(id);
    return e != nullptr && e->slot.is_current(h);
}

const RuntimeProgram* ReloadSet::program(AssetId id) const
{
    const Entry* const e = find(id);
    return (e != nullptr && e->current != nullptr) ? &e->current->program : nullptr;
}

Generation* ReloadSet::generation(AssetId id) const
{
    const Entry* const e = find(id);
    return e != nullptr ? e->current : nullptr;
}

bool ReloadSet::affected(AssetId id, containers::Array<AssetId>& out) const
{
    out.clear();
    containers::Array<crd::u64> ids(m_alloc);
    if (!m_dag.affected_by(id.value, ids)) { return false; } // a cycle
    for (crd::usize i = 0; i < ids.size(); ++i) { out.push_back(AssetId{ids[i]}); }
    return true;
}

void ReloadSet::register_migration(AssetId id, MigrationFn fn, void* user)
{
    Entry* const e = find(id); // register AFTER add; a no-op if absent (a re-added asset is a new contract)
    if (e == nullptr) { return; }
    e->migration_fn   = fn; // last-registration-wins (a deliberate re-register flow, not a silent drop)
    e->migration_user = user;
}

Migration ReloadSet::migration(AssetId id) const
{
    const Entry* const e = find(id);
    if (e == nullptr) { return Migration{}; }
    return Migration{e->migration_fn, e->migration_user};
}

crd::u32 migrate_state(const exec::Interpreter& old_in, exec::Interpreter& new_in, const Module& new_module,
                       MigrationFn fn, void* user, memory::IAllocator* scratch)
{
    containers::Array<exec::StateSnapshot> cells(scratch);
    old_in.snapshot_state_by_id(cells, scratch);
    if (fn != nullptr && !fn(cells, user)) { return 0U; } // REFUSED → restore nothing (the new session init-fills)
    return new_in.restore_state_by_id(new_module, containers::ConstSpan<exec::StateSnapshot>(cells.data(), cells.size()));
}
} // namespace crd::ceir::cook
