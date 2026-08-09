#pragma once

// crd-ceir — the CEIR in-memory IR graph (CEIR-1a). Typed SSA (§12) with intrusive, in-arena def-use.
//
// Node handles are STABLE arena pointers (`Operation*`/`Value*`/`Block*`/`Region*`) — the arena (a
// `crd::memory::GrowableLinearAllocator`) never frees a node individually (see the mutation policy in context.hpp),
// so pointers stay valid for a node's whole life. Nodes are created only through `Context` (context.hpp); this
// header is the graph shape + the O(1)
// structural edits every later band needs (canonicalization/scheduling insert + erase between ops).
//
// Def-use is an INTRUSIVE doubly-linked use-list (the MLIR shape): each operand slot of an `Operation` is a `Use`
// node threaded into the defining `Value`'s chain. `replace_all_uses_with` (RAUW) splices chains — O(uses), ZERO
// allocation. `prev` is a pointer-to-pointer so an unlink is O(1) with no head special-case.

#include <crd/ceir/attr.hpp>
#include <crd/ceir/id.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
class Value;
class Operation;
class Block;
class Region;
class Module;
class SymbolTable; // CEIR-1b — a Module's name→definition index (symbol_table.hpp)

// Where a Value comes from.
enum class ValueKind : u8
{
    OpResult = 0, // a result of an Operation
    BlockArg,     // an argument of a Block
};

// Graph-ordered (order from data/effects) vs explicit basic-block CFG (§13). CEIR-1a carries the tag; the CFG
// verifier (dominance/terminators) lands at CEIR-5b.
enum class RegionKind : u8
{
    Graph = 0,
    SsaCfg,
};

// One operand slot — an intrusive node in the defining Value's use-list. Lives inside its owning Operation's arena
// block (the operands array), so it is never separately allocated.
struct Use
{
    Value*     value = nullptr; // the value this operand references (null = unset)
    Operation* owner = nullptr; // the operation that owns this operand slot
    Use*       next  = nullptr; // next use of `value`
    Use**      prev  = nullptr; // the slot that points AT this Use (&value->m_first_use, or &prior->next) — O(1) unlink
};

// A typed SSA value: an op result or a block argument, plus the head of its intrusive use-list.
class Value
{
public:
    [[nodiscard]] TypeId    type() const noexcept { return m_type; }
    [[nodiscard]] ValueKind kind() const noexcept { return m_kind; }
    [[nodiscard]] u32       index() const noexcept { return m_index; } // result index / block-arg index

    [[nodiscard]] Operation* defining_op() const noexcept; // null for a block arg
    [[nodiscard]] Block*     owner_block() const noexcept;  // null for an op result

    [[nodiscard]] bool       has_uses() const noexcept { return m_first_use != nullptr; }
    [[nodiscard]] const Use* first_use() const noexcept { return m_first_use; } // CEIR-3f escape analysis walks the uses
    [[nodiscard]] u32        num_uses() const noexcept
    {
        u32 n = 0;
        for (const Use* u = m_first_use; u != nullptr; u = u->next) { ++n; }
        return n;
    }

    // RAUW: repoint every use of THIS value to `other`, splicing the chains. O(uses), zero allocation (§12).
    void replace_all_uses_with(Value* other) noexcept
    {
        if (other == this) { return; }
        while (m_first_use != nullptr)
        {
            Use* const u = m_first_use;
            remove_use(u);
            u->value = other;
            other->add_use(u);
        }
    }

    // Link `u` at the head of this value's use-list (caller has set u->value/u->owner).
    void add_use(Use* u) noexcept
    {
        u->next = m_first_use;
        u->prev = &m_first_use;
        if (m_first_use != nullptr) { m_first_use->prev = &u->next; }
        m_first_use = u;
    }

    // O(1) unlink of `u` from whatever list it is in.
    void remove_use(Use* u) noexcept
    {
        *(u->prev) = u->next;
        if (u->next != nullptr) { u->next->prev = u->prev; }
        u->next  = nullptr;
        u->prev  = nullptr;
        u->value = nullptr;
    }

private:
    friend class Context;
    TypeId    m_type{};
    ValueKind m_kind      = ValueKind::OpResult;
    u32       m_index     = 0;
    void*     m_owner     = nullptr; // Operation* (OpResult) or Block* (BlockArg)
    Use*      m_first_use = nullptr;

    void init(TypeId t, ValueKind k, u32 idx, void* owner) noexcept
    {
        m_type  = t;
        m_kind  = k;
        m_index = idx;
        m_owner = owner;
    }
};

// One operation: an interned op-kind + provenance + operand slots (Uses) + results (Values) + nested regions, plus
// intrusive links placing it in a Block.
class Operation
{
public:
    [[nodiscard]] OpId      kind() const noexcept { return m_kind; }
    [[nodiscard]] SourceLoc loc() const noexcept { return m_loc; }
    void                    set_loc(SourceLoc l) noexcept { m_loc = l; }

    [[nodiscard]] u32    num_operands() const noexcept { return m_num_operands; }
    [[nodiscard]] Value* operand(u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_num_operands, "operand index out of range");
        return m_operands[i].value;
    }
    void set_operand(u32 i, Value* v) noexcept; // updates the use-list

    [[nodiscard]] u32    num_results() const noexcept { return m_num_results; }
    [[nodiscard]] Value* result(u32 i) const noexcept // const like operand(): returns a stable handle, never mutates the op
    {
        CRD_ASSERT_MSG(i < m_num_results, "result index out of range");
        return &m_results[i];
    }

    [[nodiscard]] u32     num_regions() const noexcept { return m_num_regions; }
    [[nodiscard]] Region* region(u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_num_regions, "region index out of range");
        return m_regions[i];
    }

    // ── Attributes (CEIR-1c). Mutated via `Context::set_attr` (needs the arena); read here. The dict is a small
    // arena array of interned (name → AttrId) pairs — O(num_attrs) lookup, and dicts are small. ──
    [[nodiscard]] u32 num_attrs() const noexcept { return m_num_attrs; }
    [[nodiscard]] containers::StringView attr_name(u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_num_attrs, "attr index out of range");
        return m_attrs[i].name;
    }
    [[nodiscard]] AttrId attr_id_at(u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_num_attrs, "attr index out of range");
        return m_attrs[i].value;
    }
    // The interned value of attribute `name`, or an invalid AttrId if the op has no such attribute.
    [[nodiscard]] AttrId attr(containers::StringView name) const noexcept
    {
        for (u32 k = 0; k < m_num_attrs; ++k)
        {
            if (m_attrs[k].name == name) { return m_attrs[k].value; }
        }
        return {};
    }
    [[nodiscard]] bool has_attr(containers::StringView name) const noexcept { return attr(name).valid(); }

    [[nodiscard]] Operation* next_in_block() const noexcept { return m_next; }
    [[nodiscard]] Operation* prev_in_block() const noexcept { return m_prev; }
    [[nodiscard]] Block*     parent_block() const noexcept { return m_parent; }
    [[nodiscard]] bool       is_erased() const noexcept { return m_erased; }

    // Drop this op: remove its operands from their values' use-lists, unlink from the block, tombstone. Its results
    // must have NO remaining uses (asserted) — you cannot erase a value that is still consumed.
    void erase() noexcept;

private:
    friend class Context;
    friend class Block;
    OpId      m_kind{};
    SourceLoc m_loc{};
    Use*      m_operands     = nullptr;
    u32       m_num_operands = 0;
    Value*    m_results      = nullptr;
    u32       m_num_results  = 0;
    Region**  m_regions      = nullptr;
    u32       m_num_regions  = 0;
    NamedAttr* m_attrs       = nullptr; // CEIR-1c — the attribute dict (interned name→value pairs); grows by rebuild
    u32        m_num_attrs   = 0;
    Operation* m_next        = nullptr;
    Operation* m_prev        = nullptr;
    Block*     m_parent      = nullptr;
    bool       m_erased      = false;
};

// A sequence of operations (intrusive list) plus block arguments. Insert/erase between ops is O(1).
class Block
{
public:
    [[nodiscard]] u32    num_args() const noexcept { return m_num_args; }
    [[nodiscard]] Value* arg(u32 i) noexcept
    {
        CRD_ASSERT_MSG(i < m_num_args, "block-arg index out of range");
        return &m_args[i];
    }

    [[nodiscard]] Operation* first_op() const noexcept { return m_first; }
    [[nodiscard]] Operation* last_op() const noexcept { return m_last; }
    [[nodiscard]] bool       empty() const noexcept { return m_first == nullptr; }
    [[nodiscard]] u32        num_ops() const noexcept
    {
        u32 n = 0;
        for (const Operation* op = m_first; op != nullptr; op = op->next_in_block()) { ++n; }
        return n;
    }

    void append(Operation* op) noexcept
    {
        op->m_parent = this;
        op->m_next   = nullptr;
        op->m_prev   = m_last;
        if (m_last != nullptr) { m_last->m_next = op; }
        else { m_first = op; }
        m_last = op;
    }

    // Insert `op` before `before`; `before == nullptr` appends.
    void insert_before(Operation* op, Operation* before) noexcept
    {
        if (before == nullptr)
        {
            append(op);
            return;
        }
        CRD_ASSERT_MSG(before->m_parent == this, "insert_before anchor is not in this block");
        op->m_parent = this;
        op->m_next   = before;
        op->m_prev   = before->m_prev;
        if (before->m_prev != nullptr) { before->m_prev->m_next = op; }
        else { m_first = op; }
        before->m_prev = op;
    }

    [[nodiscard]] Block*  next_in_region() const noexcept { return m_next; }
    [[nodiscard]] Region* parent_region() const noexcept { return m_parent; }

private:
    friend class Context;
    friend class Region;
    friend class Operation;

    void unlink(Operation* op) noexcept
    {
        if (op->m_prev != nullptr) { op->m_prev->m_next = op->m_next; }
        else { m_first = op->m_next; }
        if (op->m_next != nullptr) { op->m_next->m_prev = op->m_prev; }
        else { m_last = op->m_prev; }
        op->m_next   = nullptr;
        op->m_prev   = nullptr;
        op->m_parent = nullptr;
    }

    Value*     m_args     = nullptr;
    u32        m_num_args = 0;
    Operation* m_first    = nullptr;
    Operation* m_last     = nullptr;
    Block*     m_next     = nullptr;
    Block*     m_prev     = nullptr;
    Region*    m_parent   = nullptr;
};

// A nested body: a list of blocks with a region kind.
class Region
{
public:
    [[nodiscard]] RegionKind kind() const noexcept { return m_kind; }
    [[nodiscard]] Block*     first_block() const noexcept { return m_first; }
    [[nodiscard]] Block*     last_block() const noexcept { return m_last; }
    [[nodiscard]] bool       empty() const noexcept { return m_first == nullptr; }

    void append(Block* b) noexcept
    {
        b->m_parent = this;
        b->m_next   = nullptr;
        b->m_prev   = m_last;
        if (m_last != nullptr) { m_last->m_next = b; }
        else { m_first = b; }
        m_last = b;
    }

    [[nodiscard]] Operation* parent_op() const noexcept { return m_parent; }

private:
    friend class Context;
    friend class Operation;
    RegionKind m_kind   = RegionKind::Graph;
    Block*     m_first  = nullptr;
    Block*     m_last   = nullptr;
    Operation* m_parent = nullptr;
};

// A top-level program unit: a single-region body + a symbol table (the name→definition index for `ceir.func` and any
// later symbol-defining op; CEIR-1b §34). Both are created by `Context::create_module`.
class Module
{
public:
    [[nodiscard]] Region*      body() const noexcept { return m_body; }
    [[nodiscard]] SymbolTable* symbols() const noexcept { return m_symbols; }

private:
    friend class Context;
    Region*      m_body    = nullptr;
    SymbolTable* m_symbols = nullptr; // CEIR-1b — arena-allocated alongside the module
};

// ── cross-referencing inline defs (need the full class set) ──

inline Operation* Value::defining_op() const noexcept
{
    return m_kind == ValueKind::OpResult ? static_cast<Operation*>(m_owner) : nullptr;
}
inline Block* Value::owner_block() const noexcept
{
    return m_kind == ValueKind::BlockArg ? static_cast<Block*>(m_owner) : nullptr;
}

inline void Operation::set_operand(u32 i, Value* v) noexcept
{
    CRD_ASSERT_MSG(i < m_num_operands, "operand index out of range");
    Use& use = m_operands[i];
    if (use.value != nullptr) { use.value->remove_use(&use); }
    use.owner = this;
    use.value = v;
    if (v != nullptr) { v->add_use(&use); }
}

inline void Operation::erase() noexcept
{
    for (u32 i = 0; i < m_num_results; ++i)
    {
        CRD_ASSERT_MSG(!m_results[i].has_uses(), "cannot erase an operation whose result is still used");
    }
    for (u32 i = 0; i < m_num_operands; ++i)
    {
        if (m_operands[i].value != nullptr) { m_operands[i].value->remove_use(&m_operands[i]); }
    }
    m_num_operands = 0;
    if (m_parent != nullptr) { m_parent->unlink(this); }
    m_erased = true;
}
} // namespace crd::ceir
