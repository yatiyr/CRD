#include <crd/ceir/transaction.hpp>

#include <crd/ceir/detail/symbol_registration.hpp>
#include <crd/memory/construct.hpp>

namespace crd::ceir
{
namespace
{
// Stable diagnostic codes for the authored-mutation surface (the 8g DiagnosticCode FNV model).
constexpr DiagnosticCode kErrRejected  = make_diagnostic_code("ceir.transaction.rejected");
constexpr DiagnosticCode kErrDupSymbol = make_diagnostic_code("ceir.transaction.duplicate_symbol");
constexpr DiagnosticCode kErrVerify    = make_diagnostic_code("ceir.transaction.verify_failed");

[[nodiscard]] bool contains_op(const containers::Array<Operation*>& a, const Operation* op) noexcept
{
    for (usize i = 0; i < a.size(); ++i)
    {
        if (a[i] == op) { return true; }
    }
    return false;
}
void push_unique_id(containers::Array<StableId>& a, StableId id)
{
    for (usize i = 0; i < a.size(); ++i)
    {
        if (a[i] == id) { return; }
    }
    a.push_back(id);
}
[[nodiscard]] bool contains_block(const containers::Array<Block*>& a, const Block* b) noexcept
{
    for (usize i = 0; i < a.size(); ++i)
    {
        if (a[i] == b) { return true; }
    }
    return false;
}
// Collect the ops (root + all descendants) and blocks of the subtree rooted at `root`, in pre-order.
void collect_subtree(Operation* root, containers::Array<Operation*>& ops, containers::Array<Block*>& blocks)
{
    ops.push_back(root);
    for (u32 i = 0; i < root->num_regions(); ++i)
    {
        for (Block* b = root->region(i)->first_block(); b != nullptr; b = b->next_in_region())
        {
            blocks.push_back(b);
            for (Operation* o = b->first_op(); o != nullptr; o = o->next_in_block()) { collect_subtree(o, ops, blocks); }
        }
    }
}
// True iff `v` is defined INSIDE the collected subtree (a subtree op's result, or a subtree block's argument).
[[nodiscard]] bool value_inside(const Value* v, const containers::Array<Operation*>& ops,
                                const containers::Array<Block*>& blocks) noexcept
{
    if (v == nullptr) { return true; } // an unset operand crosses nothing
    if (v->kind() == ValueKind::OpResult) { return contains_op(ops, v->defining_op()); }
    return contains_block(blocks, v->owner_block());
}
} // namespace

Transaction::Transaction(Context& ctx, Module& module, DiagnosticEngine& diag, memory::IAllocator* alloc)
    : m_ctx(ctx), m_module(module), m_diag(diag), m_alloc(alloc), m_journal(alloc), m_created(alloc), m_erased(alloc),
      m_modified(alloc), m_touched(alloc), m_removed(alloc)
{
    // ⛔ settle pre-existing ids up front: `serialize` already assigns them lazily, so this makes begin's state the
    // canonical serialized form — the ONLY id assignments in the transaction window are then to tx-created ops (all
    // erased on rollback), so byte-identity is UNCONDITIONAL (ADR-0119 §2.5).
    m_ctx.assign_stable_ids(m_module);
    m_begin_watermark = m_module.stable_id_watermark();
}

Transaction::~Transaction()
{
    if (is_open()) { rollback(); } // no partial edit escapes a dropped-open transaction
}

bool Transaction::guard_open()
{
    if (m_poisoned) { return false; } // already poisoned — the original diagnostic stands; further edits are no-ops
    if (m_committed || m_rolled_back)
    {
        reject(containers::StringView("edit on a closed transaction"), SourceLoc{});
        return false;
    }
    return true;
}

void Transaction::reject(containers::StringView msg, SourceLoc loc)
{
    m_poisoned = true;
    m_diag.emit(Severity::Error, kErrRejected, containers::StringView("ceir.transaction.rejected"), loc, msg);
}

bool Transaction::region_in_module(Region* r) const noexcept
{
    while (r != nullptr)
    {
        Operation* const parent = r->parent_op();
        if (parent == nullptr) { return r == m_module.body(); } // reached a root region — is it THIS module's body?
        Block* const pb = parent->parent_block();
        if (pb == nullptr) { return false; } // a detached parent op (not reachable for a live op)
        r = pb->parent_region();
    }
    return false; // a detached region (parent_op chain hit null before the body)
}

bool Transaction::belongs(const Operation* op) const noexcept
{
    if (op == nullptr || op->is_erased() || op->parent_block() == nullptr) { return false; }
    return region_in_module(op->parent_block()->parent_region());
}

void Transaction::mark_modified(Operation* op)
{
    if (op != nullptr && !contains_op(m_modified, op)) { m_modified.push_back(op); }
}

void Transaction::note_symbol_touch(const Operation* op)
{
    if (op != nullptr && op->has_attr(containers::StringView("sym_name"))) { m_symbols_dirty = true; }
}

Operation* Transaction::insert(OpId kind, containers::ConstSpan<Value*> operands, u32 num_results, Block* block,
                               Operation* before, TypeId result_type, u32 num_regions)
{
    if (!guard_open()) { return nullptr; }
    if (block == nullptr || !region_in_module(block->parent_region()))
    {
        reject(containers::StringView("insert into a block outside this module"), SourceLoc{});
        return nullptr;
    }
    if (before != nullptr && before->parent_block() != block)
    {
        reject(containers::StringView("insert anchor is not in the target block"), SourceLoc{});
        return nullptr;
    }
    Operation* const op = m_ctx.create_operation(kind, operands, num_results, result_type, num_regions);
    block->insert_before(op, before);
    Record r{};
    r.kind = MutKind::Insert;
    r.op   = op;
    m_journal.push_back(r);
    m_created.push_back(op);
    note_symbol_touch(op);
    return op;
}

bool Transaction::erase(Operation* op)
{
    if (!guard_open()) { return false; }
    if (!belongs(op))
    {
        reject(containers::StringView("erase of an op outside this module"), op != nullptr ? op->loc() : SourceLoc{});
        return false;
    }
    for (u32 i = 0; i < op->num_results(); ++i)
    {
        if (op->result(i)->has_uses())
        {
            reject(containers::StringView("erase of an op whose result is still used"), op->loc());
            return false;
        }
    }
    // ⛔ A region-bearing op erases its whole SUBTREE, but Operation::erase does NOT recurse — so a nested op's cross-
    // boundary SSA edge would leak (an IN-edge: a nested operand defined outside the subtree stays threaded into that
    // external value's use-list after the tombstone; an OUT-edge: a nested result used outside becomes a live use into a
    // dead subtree). Reject BOTH directions here (rewire across the boundary first), same graceful-reject as the rest.
    if (op->num_regions() > 0U)
    {
        containers::Array<Operation*> sub(m_alloc);
        containers::Array<Block*>     blks(m_alloc);
        collect_subtree(op, sub, blks);
        for (usize si = 0; si < sub.size(); ++si)
        {
            Operation* const n = sub[si];
            if (n == op) { continue; } // root's operands are detached by erase(); root's results by the check above
            for (u32 i = 0; i < n->num_operands(); ++i)
            {
                if (!value_inside(n->operand(i), sub, blks)) // IN-edge
                {
                    reject(containers::StringView("erase of a region-bearing op with a nested operand defined outside the erased subtree"),
                           op->loc());
                    return false;
                }
            }
            for (u32 i = 0; i < n->num_results(); ++i)
            {
                for (const Use* u = n->result(i)->first_use(); u != nullptr; u = u->next) // OUT-edge
                {
                    if (!contains_op(sub, u->owner))
                    {
                        reject(containers::StringView("erase of a region-bearing op whose nested result is used outside the erased subtree"),
                               op->loc());
                        return false;
                    }
                }
            }
        }
    }
    note_symbol_touch(op);
    const u32 n    = op->num_operands();
    Value**   snap = nullptr;
    if (n > 0U)
    {
        snap = memory::construct_array<Value*>(m_ctx.arena(), n); // Context-arena (read only at rollback, before tx death)
        for (u32 i = 0; i < n; ++i) { snap[i] = op->operand(i); }
    }
    Record r{};
    r.kind     = MutKind::Erase;
    r.op       = op;
    r.block    = op->parent_block();
    r.anchor   = op->next_in_block(); // reinsert BEFORE this op restores the position
    r.operands = snap;
    r.count    = n;
    m_journal.push_back(r);
    m_erased.push_back(op);
    op->erase();
    return true;
}

bool Transaction::set_operand(Operation* op, u32 index, Value* value)
{
    if (!guard_open()) { return false; }
    if (!belongs(op))
    {
        reject(containers::StringView("set_operand on an op outside this module"), op != nullptr ? op->loc() : SourceLoc{});
        return false;
    }
    if (index >= op->num_operands())
    {
        reject(containers::StringView("set_operand index out of range"), op->loc());
        return false;
    }
    Record r{};
    r.kind  = MutKind::SetOperand;
    r.op    = op;
    r.index = index;
    r.value = op->operand(index); // old value
    m_journal.push_back(r);
    op->set_operand(index, value);
    mark_modified(op);
    return true;
}

bool Transaction::replace_all_uses_with(Value* from, Value* to)
{
    if (!guard_open()) { return false; }
    if (from == nullptr || to == nullptr)
    {
        reject(containers::StringView("replace_all_uses_with a null value"), SourceLoc{});
        return false;
    }
    if (from == to) { return true; } // no-op
    containers::Array<Use*> moved(m_alloc);
    m_ctx.rauw_recording(from, to, moved);
    for (usize i = 0; i < moved.size(); ++i)
    {
        Record r{};
        r.kind  = MutKind::PointUse;
        r.use   = moved[i];
        r.value = from; // undo restores the use to `from`
        m_journal.push_back(r);
        mark_modified(moved[i]->owner); // the touched node is the USE OWNER, not from's defining op (ADR-0119 §2.6)
    }
    return true;
}

bool Transaction::set_attr(Operation* op, containers::StringView name, AttrId value)
{
    if (!guard_open()) { return false; }
    if (!belongs(op))
    {
        reject(containers::StringView("set_attr on an op outside this module"), op != nullptr ? op->loc() : SourceLoc{});
        return false;
    }
    // Snapshot the WHOLE prior dict into the CONTEXT arena — uniform undo for set_attr's overwrite-in-place AND
    // grow-by-rebuild branches, and (⛔ the lifetime landmine) the snapshot BECOMES live module state on rollback, so it
    // must outlive the Transaction.
    const u32  pc   = op->num_attrs();
    NamedAttr* snap = nullptr;
    if (pc > 0U)
    {
        snap = memory::construct_array<NamedAttr>(m_ctx.arena(), pc);
        for (u32 i = 0; i < pc; ++i) { snap[i] = NamedAttr{op->attr_name(i), op->attr_id_at(i)}; }
    }
    Record r{};
    r.kind  = MutKind::SetAttr;
    r.op    = op;
    r.attrs = snap;
    r.count = pc;
    m_journal.push_back(r);
    m_ctx.set_attr(op, name, value);
    mark_modified(op);
    if (name == containers::StringView("sym_name") || name == containers::StringView("sym_visibility"))
    {
        m_symbols_dirty = true;
    }
    return true;
}

void Transaction::compute_touched_removed()
{
    for (usize i = 0; i < m_created.size(); ++i)
    {
        Operation* const op = m_created[i];
        if (contains_op(m_erased, op)) { continue; } // inserted-then-erased ⇒ net-out (neither set)
        push_unique_id(m_touched, op->stable_id());
    }
    for (usize i = 0; i < m_modified.size(); ++i)
    {
        Operation* const op = m_modified[i];
        if (contains_op(m_erased, op) || contains_op(m_created, op)) { continue; } // erased ⇒ removed; created ⇒ already touched
        push_unique_id(m_touched, op->stable_id());
    }
    for (usize i = 0; i < m_erased.size(); ++i)
    {
        Operation* const op = m_erased[i];
        if (contains_op(m_created, op)) { continue; }     // created-then-erased ⇒ net-out
        push_unique_id(m_removed, op->stable_id());        // erase() left m_stable_id intact
    }
}

bool Transaction::commit()
{
    if (m_committed) { return true; }
    if (m_rolled_back) { return false; }
    if (m_poisoned)
    {
        rollback();
        return false;
    }
    // ── verify (dry-run, no mutation) BEFORE any id assignment: a failed commit never advances the watermark ──
    for (usize i = 0; i < m_created.size(); ++i)
    {
        Operation* const op = m_created[i];
        if (!contains_op(m_erased, op) && !m_ctx.verify(*op))
        {
            m_diag.emit(Severity::Error, kErrVerify, containers::StringView("ceir.transaction.verify_failed"), op->loc(),
                        containers::StringView("a committed op fails its verifier"));
            rollback();
            return false;
        }
    }
    for (usize i = 0; i < m_modified.size(); ++i)
    {
        Operation* const op = m_modified[i];
        if (!contains_op(m_erased, op) && !contains_op(m_created, op) && !m_ctx.verify(*op))
        {
            m_diag.emit(Severity::Error, kErrVerify, containers::StringView("ceir.transaction.verify_failed"), op->loc(),
                        containers::StringView("a modified op fails its verifier"));
            rollback();
            return false;
        }
    }
    // The LAST fallible check — resync_symbols mutates only on success, so a duplicate-symbol failure leaves the index
    // untouched (rollback then needs nothing for symbols).
    if (m_symbols_dirty)
    {
        const Operation* dup = nullptr;
        if (!m_ctx.resync_symbols(m_module, dup))
        {
            m_diag.emit(Severity::Error, kErrDupSymbol, containers::StringView("ceir.transaction.duplicate_symbol"),
                        dup != nullptr ? dup->loc() : SourceLoc{},
                        containers::StringView("transaction introduces a duplicate symbol"));
            rollback();
            return false;
        }
    }
    // ── point of no return — infallible ──
    m_ctx.assign_stable_ids(m_module); // transaction-created ops draw ids (pre-existing settled at begin)
    compute_touched_removed();
    m_committed = true;
    return true;
}

void Transaction::rollback()
{
    if (m_committed || m_rolled_back) { return; }
    for (usize k = m_journal.size(); k-- > 0U;) // reverse order — the correctness proof (ADR-0119 §2.1)
    {
        const Record& r = m_journal[k];
        switch (r.kind)
        {
        case MutKind::Insert:
            r.op->erase(); // results use-free: any later record that added a use was undone first
            break;
        case MutKind::Erase:
            m_ctx.reinsert_erased_op(r.op, r.block, r.anchor, containers::ConstSpan<Value*>(r.operands, r.count));
            break;
        case MutKind::SetOperand:
            r.op->set_operand(r.index, r.value);
            break;
        case MutKind::PointUse:
            m_ctx.detach_and_point_use(r.use, r.value);
            break;
        case MutKind::SetAttr:
            m_ctx.restore_attr_dict(r.op, r.attrs, r.count);
            break;
        }
    }
    m_ctx.set_stable_id_watermark(&m_module, m_begin_watermark); // restore the monotone high-water mark (byte-identity)
    m_rolled_back = true;
}

const Operation* Transaction::find(StableId id) const
{
    return m_ctx.find_by_stable_id(m_module, id);
}
} // namespace crd::ceir
