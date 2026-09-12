#pragma once

// crd-ceir — TRANSACTIONS (CEIR-8i, ADR-0119). The atomic AUTHORED-mutation surface: a `Transaction` records a set of
// IR edits against a `Module`, applies them EAGERLY (so the body sees its own edits), and either COMMITS them
// atomically or ROLLS them back to the byte-identical pre-transaction state. The recorder over the one Context mutation
// path (the frame-graph-is-a-recording-mode discipline) — never a second mutation implementation.
//
// ⛔ REUSES: 8d stable ids (reference a pre-existing op by an id that survives the edit — `find`), 8h IncrementalDag
// (a committed transaction reports the touched node-set — `touched`/`removed` — for the incremental engine), 8g
// DiagnosticEngine (a rejected edit / failed commit reports a Diagnostic, never an assert). ⛔ SCOPE: authored edits
// only; compiler-internal rewrites (5a fold, 8g rewrite driver) adopt transactional journaling at CEIR-26 (ADR-0119
// §2.7). ⛔ This surface is agent/editor-facing → GRACEFUL-REJECT, never assert: a precondition violation emits a
// Diagnostic and POISONS the transaction (a poisoned commit auto-rolls-back).

#include <crd/ceir/context.hpp>
#include <crd/ceir/diagnostic.hpp>
#include <crd/ceir/id.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir
{
// One atomic authored-edit session over a Module. Construct → edit (insert/erase/set_operand/replace_all_uses_with/
// set_attr) → commit() XOR rollback(). Destroying an OPEN transaction auto-rolls-back (no partial edit escapes).
class Transaction
{
public:
    // ⛔ begin() settles every pre-existing op's stable id up front (ADR-0119 §2.5) so byte-identity is unconditional,
    // and records the watermark rollback restores. `alloc` backs the transaction's own bookkeeping; restore PAYLOADS go
    // to the CONTEXT arena (they outlive the Transaction).
    Transaction(Context& ctx, Module& module, DiagnosticEngine& diag, memory::IAllocator* alloc);
    ~Transaction();

    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&)                 = delete;
    Transaction& operator=(Transaction&&)      = delete;

    // ── mutators (record the inverse + apply eagerly; a precondition violation → poison + Diagnostic, no mutation) ──
    // Create an op of `kind` and place it before `before` (nullptr ⇒ append) in `block` (which must belong to this
    // module). Returns the new op (a stable handle) or nullptr on rejection. The op may carry fresh regions/blocks.
    [[nodiscard]] Operation* insert(OpId kind, containers::ConstSpan<Value*> operands, u32 num_results, Block* block,
                                    Operation* before = nullptr, TypeId result_type = {}, u32 num_regions = 0U);
    // Drop `op` (must belong to this module; its results must be use-free — rewire consumers FIRST, else a rejection).
    [[nodiscard]] bool erase(Operation* op);
    // Repoint `op`'s operand `index` at `value` (updates the def-use lists).
    [[nodiscard]] bool set_operand(Operation* op, u32 index, Value* value);
    // RAUW: repoint every use of `from` at `to`.
    [[nodiscard]] bool replace_all_uses_with(Value* from, Value* to);
    // Attach/overwrite attribute `name` = `value` on `op`.
    [[nodiscard]] bool set_attr(Operation* op, containers::StringView name, AttrId value);

    // ── control ──
    // Finalize: verify (per-op verifier + symbol re-sync) BEFORE assigning ids; on success assign ids to new ops and
    // compute touched/removed; on failure emit a Diagnostic, ROLL BACK, and return false. Idempotent.
    [[nodiscard]] bool commit();
    // Replay the inverses in reverse order and restore the watermark — the module is byte-identical to begin. Idempotent;
    // a no-op after a commit.
    void rollback();

    // ── queries ──
    // Resolve a pre-existing op by its 8d stable id (survives edits; nullptr ⇒ none). ⛔ Inside an OPEN transaction this
    // sees the PRE-transaction index — transaction-created ops have no id until commit (hold their `insert` handle).
    [[nodiscard]] const Operation* find(StableId id) const;
    [[nodiscard]] bool             is_open() const noexcept { return !m_committed && !m_rolled_back; }
    [[nodiscard]] bool             is_poisoned() const noexcept { return m_poisoned; }
    // The 8h dirty seam (valid after a successful commit): the StableIds a consumer feeds IncrementalDag/AnalysisManager.
    // `touched` = inserted + modified LIVE ops; `removed` = pre-existing ops erased by the transaction. ⛔ NET-OUT: an op
    // inserted-then-erased is in NEITHER; the consumer computes each node's content/interface revision (the tx reports
    // only WHAT changed — the 8h division of labour).
    [[nodiscard]] containers::ConstSpan<StableId> touched() const noexcept
    {
        return containers::ConstSpan<StableId>(m_touched.data(), m_touched.size());
    }
    [[nodiscard]] containers::ConstSpan<StableId> removed() const noexcept
    {
        return containers::ConstSpan<StableId>(m_removed.data(), m_removed.size());
    }

private:
    // NOLINTNEXTLINE(performance-enum-size) — a 5-arm total switch drives rollback (GCC -Werror=switch guards a 6th).
    enum class MutKind : u8
    {
        Insert,     // undo: erase the inserted op (use-free by reverse order)
        Erase,      // undo: reinsert_erased_op(op, block, anchor, operands)
        SetOperand, // undo: op->set_operand(index, value[old])
        PointUse,   // undo: detach_and_point_use(use, value[from]) — one per RAUW-moved use
        SetAttr,    // undo: restore_attr_dict(op, attrs[snapshot], count)
    };
    struct Record
    {
        MutKind    kind;
        Operation* op       = nullptr; // Insert/Erase/SetOperand/SetAttr subject
        Block*     block    = nullptr; // Erase: the op's owning block
        Operation* anchor   = nullptr; // Erase: the op it was before (nullptr ⇒ it was last)
        Value**    operands = nullptr; // Erase: Context-arena snapshot of the operand values
        u32        count    = 0U;      // Erase: operand count / SetAttr: prior attr count
        u32        index    = 0U;      // SetOperand: operand index
        Value*     value    = nullptr; // SetOperand: old value / PointUse: restore target (from)
        Use*       use      = nullptr; // PointUse: the moved use
        NamedAttr* attrs    = nullptr; // SetAttr: Context-arena snapshot of the prior dict
    };

    [[nodiscard]] bool guard_open();
    void               reject(containers::StringView msg, SourceLoc loc);
    [[nodiscard]] bool belongs(const Operation* op) const noexcept;
    [[nodiscard]] bool region_in_module(Region* r) const noexcept;
    void               mark_modified(Operation* op);
    void               note_symbol_touch(const Operation* op);
    void               compute_touched_removed();

    Context&                      m_ctx;
    Module&                       m_module;
    DiagnosticEngine&             m_diag;
    memory::IAllocator*           m_alloc;
    containers::Array<Record>     m_journal;
    containers::Array<Operation*> m_created;
    containers::Array<Operation*> m_erased;
    containers::Array<Operation*> m_modified;
    containers::Array<StableId>   m_touched;
    containers::Array<StableId>   m_removed;
    u64                           m_begin_watermark = 0U;
    bool                          m_poisoned        = false;
    bool                          m_committed       = false;
    bool                          m_rolled_back     = false;
    bool                          m_symbols_dirty   = false;
};
} // namespace crd::ceir
