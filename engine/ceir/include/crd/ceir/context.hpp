#pragma once

// crd-ceir Context (CEIR-1a) — owns the IR arena + the op-kind name table, and is the ONLY factory for IR nodes.
//
// ── The arena mutation policy (the contract the whole IR is built on) ──
// Nodes are ARENA-allocated (a `crd::memory::GrowableLinearAllocator`) and NEVER freed individually. To edit the IR:
//   • change an operand     → `Operation::set_operand` (updates the def-use lists);
//   • move / insert an op   → `Block::insert_before` / `Block::append` (O(1) intrusive);
//   • remove an op          → `Operation::erase` (unlink + tombstone; its results must be use-free);
//   • grow an operand list  → rebuild the op; the old operand slice LEAKS into the arena BY DESIGN.
// Everything is reclaimed when the Context is destroyed. Node handles (`Operation*`/`Value*`/`Block*`/`Region*`)
// remain valid for the Context's entire life. Constructing the IR does NOT hit the parent allocator per op — the
// arena mallocs a chunk only ~once per thousands of nodes (a first chunk is reserved at construction).

#include <crd/ceir/attr.hpp>
#include <crd/ceir/detail/string_view_hash.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/id.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>

namespace crd::ceir
{
class Context
{
public:
    explicit Context(memory::IAllocator* alloc, usize arena_chunk_bytes = 64U * 1024U);
    ~Context()                         = default;
    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&)                 = delete;
    Context& operator=(Context&&)      = delete;

    // Intern an op-kind identity: the FNV-1a hash of "dialect.op". The name is retained for diagnostics only.
    [[nodiscard]] OpId intern_op(containers::StringView dialect, containers::StringView name);
    // Reverse lookup for diagnostics — the "dialect.op" string, or "" if the id was never interned here.
    [[nodiscard]] containers::StringView op_name(OpId id) const noexcept;

    // Arena-copy a symbol NAME into stable storage (SymbolTable + symbol-ref keys point here). Empty → empty. Not
    // deduplicated (symbols are few); the SymbolTable dedups by VALUE, so two copies of the same name still resolve.
    [[nodiscard]] containers::StringView intern_symbol(containers::StringView name);

    // ── Attributes (CEIR-1c, §7/§8) ── intern a typed attribute VALUE (identical values dedup to one AttrId), then
    // attach it to an op by name. The convenience makers intern the underlying text for String/SymbolRef.
    [[nodiscard]] AttrId intern_attr(const AttrValue& v);
    [[nodiscard]] AttrId attr_int(i64 v) { return intern_attr(AttrValue::of_int(v)); }
    [[nodiscard]] AttrId attr_float(f64 v) { return intern_attr(AttrValue::of_float(v)); }
    [[nodiscard]] AttrId attr_bool(bool v) { return intern_attr(AttrValue::of_bool(v)); }
    [[nodiscard]] AttrId attr_string(containers::StringView s) { return intern_attr(AttrValue::of_string(intern_symbol(s))); }
    [[nodiscard]] AttrId attr_symbol(containers::StringView s) { return intern_attr(AttrValue::of_symbol(intern_symbol(s))); }
    [[nodiscard]] AttrId attr_type(TypeId t) { return intern_attr(AttrValue::of_type(t)); }
    // The interned value behind `id` (by value — the intern table may reallocate; an invalid id yields Int(0)).
    [[nodiscard]] AttrValue attr_value(AttrId id) const noexcept;
    // Attach (or overwrite) attribute `name` = `value` on `op`. The name is interned; the op's dict grows by rebuild
    // (the old slice leaks into the arena, exactly like an operand-list grow — the CEIR-1a mutation policy).
    void set_attr(Operation* op, containers::StringView name, AttrId value);

    // ── Source map / provenance (CEIR-1c, §111) ── register a source file → a stable `file_id` (dedup by path;
    // 0 = unknown). Every op carries provenance via `SourceLoc{file_id,line,col}` from day one (`Operation::set_loc`).
    [[nodiscard]] u32                    register_file(containers::StringView path);
    [[nodiscard]] containers::StringView file_path(u32 file_id) const noexcept;

    // ── Dialect registry + traits/interfaces (CEIR-1d, §6/§7/§101) ── OPEN-WORLD: register dialects/ops/interfaces
    // without editing any central enum; the core dispatches through these, NEVER a switch on op.kind.
    [[nodiscard]] Dialect*       register_dialect(containers::StringView name);
    [[nodiscard]] Dialect*       dialect(containers::StringView name) noexcept;
    [[nodiscard]] const OpInfo*  op_info(OpId kind) const noexcept;
    [[nodiscard]] const Dialect* dialect_of(OpId kind) const noexcept; // nullptr ⇒ unregistered (unknown) op
    [[nodiscard]] bool           has_trait(OpId kind, OpTrait t) const noexcept;
    [[nodiscard]] bool           op_has_trait(const Operation& op, OpTrait t) const noexcept;
    // Verify `op` against its kind's registered verifier; true if the kind has no verifier (opaque/unknown ⇒ valid).
    [[nodiscard]] bool           verify(const Operation& op) const;
    // Interfaces: intern a name, register/query an op-kind's implementation (an opaque function-table pointer).
    [[nodiscard]] InterfaceId    intern_interface(containers::StringView name);
    void                         register_interface(OpId kind, InterfaceId iface, const void* impl);
    [[nodiscard]] const void*    get_interface(OpId kind, InterfaceId iface) const noexcept;

    // Factories (the fluent `ModuleBuilder` is CEIR-1g). Each returns a stable arena handle.
    [[nodiscard]] Module*    create_module(RegionKind body_kind = RegionKind::Graph);
    [[nodiscard]] Region*    create_region(RegionKind kind = RegionKind::Graph);
    [[nodiscard]] Block*     create_block(u32 num_args = 0U, TypeId arg_type = {});
    [[nodiscard]] Operation* create_operation(OpId kind, containers::ConstSpan<Value*> operands, u32 num_results,
                                              TypeId result_type = {}, u32 num_regions = 0U);

    // Retag a region's kind (Graph vs SsaCfg). For CONSTRUCTION / DESERIALIZATION only — `create_operation` makes its
    // regions Graph, so the binary loader (CEIR-1f) uses this to restore a region's serialized kind. Cheap (Context is
    // a friend of Region); do not use it to mutate a region mid-analysis.
    void set_region_kind(Region* r, RegionKind kind) noexcept;

    [[nodiscard]] memory::IAllocator*             allocator() const noexcept { return m_arena.parent(); }
    [[nodiscard]] memory::GrowableLinearAllocator& arena() noexcept { return m_arena; }

private:
    friend class Dialect; // Dialect::register_op registers an OpInfo on its owning Context

    struct OpName
    {
        u64                    hash = 0;
        containers::StringView name;
    };

    memory::GrowableLinearAllocator           m_arena;
    containers::Array<OpName>                  m_op_names;
    containers::Array<AttrValue>               m_attr_values; // attribute-value intern table (AttrId.value = index+1)
    containers::Array<containers::StringView>  m_files;        // source map (file_id = index+1; each path arena-interned)
    containers::HashMap<containers::StringView, Dialect*, detail::StringViewHash> m_dialects; // name → dialect
    containers::HashMap<u64, OpInfo*>          m_op_infos;         // OpId.value → its ODS-lite descriptor
    containers::Array<containers::StringView>  m_interface_names;  // interface intern (InterfaceId.value = index+1)
};
} // namespace crd::ceir
