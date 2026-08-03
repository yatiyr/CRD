#pragma once

// crd-render-asset-core — dependency records + deterministic ordering (RAF-1, mission §5 / Gate 1).
//
// Cooked assets carry a dependency record (this shader depends on those modules;
// this frame graph depends on those techniques). The graph produces a
// DETERMINISTIC topological order — dependencies before dependents, ties broken
// by ascending AssetId — so a rebuild set (RAF-11 hot reload) is reproducible
// regardless of insertion order. It also validates every referenced dependency
// is registered (MissingDependency) and rejects cycles (CyclicDependency).
//
// Dep lists are kept sorted + deduped; ordering uses crd::containers::sort
// (deterministic introsort). No std container anywhere.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderasset/identity.hpp>
#include <crd/renderasset/registry.hpp>

namespace crd::renderasset
{
using crd::containers::Array;

// A single asset's dependency set: owner + sorted/deduped prerequisite ids.
class DependencyRecord
{
public:
    DependencyRecord(AssetId owner, memory::IAllocator* alloc) noexcept : m_owner(owner), m_deps(alloc) {}

    // Insert `dep`, keeping the list sorted ascending and unique. Self-deps and
    // invalid ids are ignored.
    void add(AssetId dep);

    [[nodiscard]] AssetId owner() const noexcept { return m_owner; }
    [[nodiscard]] const Array<AssetId>& deps() const noexcept { return m_deps; }

private:
    AssetId m_owner;
    Array<AssetId> m_deps;
};

// A dependency DAG over many assets with a deterministic topological order.
class DependencyGraph
{
public:
    explicit DependencyGraph(memory::IAllocator* alloc) noexcept : m_nodes(alloc), m_alloc(alloc) {}

    // Record "from depends on to" (so `to` must be ordered before `from`). Both
    // ids are materialized as nodes. Self-edges and invalid ids are ignored.
    void add_edge(AssetId from, AssetId to);

    // Ensure a node exists even with no edges (participates in ordering).
    void add_node(AssetId id);

    [[nodiscard]] usize node_count() const noexcept { return m_nodes.size(); }

    // Deterministic topological order (deps first, ties by ascending id). On a
    // cycle: emits CyclicDependency, clears `out`, returns false.
    bool topo_order(Array<AssetId>& out, DiagnosticList& diags) const;

    // Every referenced dependency id must be present in `registry`; otherwise a
    // MissingDependency diagnostic is emitted per missing id. Returns true iff
    // none are missing.
    bool validate_against(const AssetRegistry& registry, DiagnosticList& diags) const;

private:
    struct Node
    {
        AssetId id;
        Array<AssetId> deps; // sorted ascending, unique
    };

    // Index of `id`'s node, or the count if absent (lower_bound by id).
    [[nodiscard]] usize find_node(AssetId id) const noexcept;
    usize ensure_node(AssetId id);

    Array<Node> m_nodes; // sorted ascending by id
    memory::IAllocator* m_alloc;
};
} // namespace crd::renderasset
