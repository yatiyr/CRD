// node.hpp — the CHIR-0 structured SOURCE MODEL (CEIR-32b, ADR-0128 D5).
//
// CHIR is the Cerid high-level language layer (ADR-0108/0109). This is its ONE canonical structured source model:
// semantic nodes + stable ids (ADR-0114) + source spans + a layout side-table keyed by stable id. TEXT and the CR-D007
// GRAPH are two PROJECTIONS of this ONE model (ADR-0128 D5) — the graph is a SCHEMA, not an editor; both lower through
// the SAME CHIR->CEIR path (§180 #13), and CEIR-33 renders the schema. 32b built the model + the graph projection's DATA
// form (schema.hpp); 32c added the TEXT projection (text.hpp). ~~NOT the text parser (32c)~~ (added 32c) — only the
// CHIR->CEIR lowering (32d) is still pending.
//
// ⛔ Operation-SHAPED but NOT CEIR IR (ADR-0128 ruling 1): a `ChirNode` mirrors the op shape (kind + id + span + pins +
// attrs + child regions) so 32d's lowering is a structural walk, but it carries SOURCE-language semantics (a `state x: T`
// declaration, a typed pin with a source-level type NAME) that must not live in `ceir::Context`. Only the `StableId` /
// `SourceLoc` TYPES are reused from crd-ceir (that IS "shares the identity model", ADR-0128 D5) — the VALUES are CHIR's.
// ⛔ NOT a CHIR type system (ADR-0128 "don't build"): a pin type is an interned type NAME (a StringView into the pool),
// resolved at 32d. ⛔ host-only, deps crd-ceir + core/log/memory/containers/units ONLY (ADR-0128 D6); no std containers.

#pragma once

#include <crd/ceir/id.hpp> // StableId + SourceLoc — the SHARED identity types (ADR-0114/0128 D5)
#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp> // fnv1a_64 — the CHIR stable-id derivation (ADR-0128 ruling 3)
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::chir
{
using crd::ceir::SourceLoc;
using crd::ceir::StableId;
using crd::containers::StringView;

inline constexpr crd::u32 kInvalidNode = 0xFFFFFFFFU;

// The CHIR-0 v1 node kinds — the §143 five constructs + Program scaffolding (ADR-0128 D1). Append at END (the
// widening-enum discipline: a total switch over this must handle every case, gcc -Werror=switch).
enum class NodeKind : crd::u8
{
    Program,      // the root — a CHIR program/module
    EventHandler, // `on event` -> a func.func carrying a time-domain attr (ADR-0128 D1; NO event op exists)
    StateDecl,    // `state x: T` -> a core.state StateEdge cell (§20); the RELOAD-STABLE declaration (ADR-0128 D3)
    Query,        // `query entities` -> the ECS bridge (register_ecs)
    ParallelFor,  // `parallel update` -> task.parallel_for (⛔ STATE-FREE + self-contained body, ADR-0128 D2)
    Await,        // `await async task` -> async.launch + async.await
    StateUpdate,  // `update state` -> a write to a StateDecl's cell (SEQUENTIAL, after the parallel body, ADR-0128 D2)
};

[[nodiscard]] StringView node_kind_name(NodeKind k) noexcept;      // canonical schema token
[[nodiscard]] bool       node_kind_from_name(StringView s, NodeKind& out) noexcept; // the inverse (schema reader)

enum class PinDir : crd::u8
{
    In,
    Out,
};

// A byte range into the model's string pool (offsets stay valid across pool growth — we store offsets, not pointers).
struct StringRef
{
    crd::u32 off = 0;
    crd::u32 len = 0;
};

// A typed pin: a source-level NAME + a source-level TYPE NAME (interned; NOT a CHIR type system — resolved at 32d).
struct Pin
{
    StringRef name;
    StringRef type;
    PinDir    dir = PinDir::In;
};

// A node attribute (key=value; both interned) — e.g. domain=event on an EventHandler, over=q on a ParallelFor.
struct Attr
{
    StringRef key;
    StringRef val;
};

// A dataflow edge: (from node's out-pin) -> (to node's in-pin). Node/pin refer by index (the graph projection's wires).
struct Edge
{
    crd::u32 from_node = kInvalidNode;
    crd::u32 from_pin  = 0;
    crd::u32 to_node   = kInvalidNode;
    crd::u32 to_pin    = 0;
};

// A layout side-table entry, keyed by stable id — coordinates + group, SEPARATE from semantics (ADR-0128 D5, §180 #9).
// Moving a node's coordinates must NOT change the semantic model (semantic_hash ignores this — §180 #10).
struct Layout
{
    StableId id;
    crd::f32 x     = 0.0F;
    crd::f32 y     = 0.0F;
    crd::u32 group = 0U;
};

// A semantic node. Operation-shaped (kind + id + span + pins + attrs + child regions) but source-language, not CEIR IR.
struct ChirNode
{
    NodeKind  kind   = NodeKind::Program;
    StableId  id{};                 // the CHIR-side stable id (ADR-0128 ruling 3; derived by derive_ids)
    SourceLoc loc{};                // 0 = none (a graph-authored node has no text position)
    StringRef name;                 // the declaration name (empty = anonymous)
    crd::u32  parent = kInvalidNode; // parent node index (kInvalidNode for the root)
    crd::containers::Array<Pin>      pins;
    crd::containers::Array<Attr>     attrs;
    crd::containers::Array<crd::u32> children; // child node indices (the node's region)

    explicit ChirNode(crd::memory::IAllocator* a) : pins(a), attrs(a), children(a) {}
};

// The CHIR source model — the arena-pooled node graph + the string pool + the layout side-table. Allocator-parametrized
// (ADR-0128 ruling 1, the D4 handles+arenas idiom). Node handles are `u32` indices into `m_nodes` (stable across growth).
class SourceModel
{
public:
    explicit SourceModel(crd::memory::IAllocator* alloc)
        : m_alloc(alloc), m_nodes(alloc), m_strings(alloc), m_edges(alloc), m_layout(alloc)
    {
    }

    // Append a node under `parent` (kInvalidNode for the root). Returns its index. Names/types are interned immediately.
    crd::u32 add_node(NodeKind kind, StringView name, crd::u32 parent, SourceLoc loc = SourceLoc{});
    void     add_pin(crd::u32 node, PinDir dir, StringView name, StringView type);
    void     add_attr(crd::u32 node, StringView key, StringView val);
    void     add_edge(crd::u32 from_node, crd::u32 from_pin, crd::u32 to_node, crd::u32 to_pin);
    void     set_layout(StableId id, crd::f32 x, crd::f32 y, crd::u32 group);
    void     set_node_id(crd::u32 idx, StableId id) noexcept { m_nodes[idx].id = id; } // the schema reader restores it verbatim

    // ⛔ ADR-0128 ruling 3: derive every node's StableId from its QUALIFIED SEMANTIC PATH (position-INDEPENDENT):
    //   named node   -> fnv1a(parent.id ‖ kind ‖ name)
    //   anonymous    -> fnv1a(parent.id ‖ kind ‖ ordinal-among-same-kind-siblings)
    // So a NAMED declaration keeps its id when siblings reorder (reload-stable state schema, ADR-0128 D3), and both
    // projections of the same program pin the SAME ids. Parents are derived before children (add order guarantees it).
    void derive_ids() noexcept;

    // A hash of the SEMANTIC content — kind + name + pins + attrs + children + edges — IGNORING layout AND source spans
    // (§180 #10 semantic diff): move a node's coordinates -> unchanged; delete/rename a node -> changes. Requires ids.
    [[nodiscard]] crd::u64 semantic_hash() const noexcept;

    // accessors
    [[nodiscard]] crd::u32           node_count() const noexcept { return static_cast<crd::u32>(m_nodes.size()); }
    [[nodiscard]] const ChirNode&    node(crd::u32 i) const noexcept { return m_nodes[i]; }
    [[nodiscard]] const crd::containers::Array<Edge>&   edges() const noexcept { return m_edges; }
    [[nodiscard]] const crd::containers::Array<Layout>& layout() const noexcept { return m_layout; }
    [[nodiscard]] StringView str(StringRef r) const noexcept;
    [[nodiscard]] crd::u32   root() const noexcept; // the first Program node, or kInvalidNode
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

private:
    [[nodiscard]] StringRef intern(StringView s);

    crd::memory::IAllocator*             m_alloc;
    crd::containers::Array<ChirNode>     m_nodes;
    crd::containers::Array<char>         m_strings; // the interning pool (offsets into this stay valid across growth)
    crd::containers::Array<Edge>         m_edges;
    crd::containers::Array<Layout>       m_layout;
};

} // namespace crd::chir
