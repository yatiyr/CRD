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
#include <crd/containers/incremental_dag.hpp> // CEIR-8h (ADR-0118): the ONE engine this graph is now a thin wrapper over
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
    explicit DependencyGraph(memory::IAllocator* alloc) noexcept : m_dag(alloc), m_alloc(alloc) {}

    // Record "from depends on to" (so `to` must be ordered before `from`). Both
    // ids are materialized as nodes. Self-edges and invalid ids are ignored.
    void add_edge(AssetId from, AssetId to);

    // Ensure a node exists even with no edges (participates in ordering).
    void add_node(AssetId id);

    [[nodiscard]] usize node_count() const noexcept { return m_dag.node_count(); }

    // Deterministic topological order (deps first, ties by ascending id). On a
    // cycle: emits CyclicDependency, clears `out`, returns false.
    bool topo_order(Array<AssetId>& out, DiagnosticList& diags) const;

    // RAF-11 hot-reload REBUILD SET. The transitive DEPENDENTS of `changed` — every
    // asset that, directly or through a chain, depends on it — in deterministic
    // topological order (a dependency before its dependent), EXCLUDING `changed`
    // itself. Recook `changed`, then rebuild `out` in this order and no dependent is
    // ever rebuilt before something it depends on. Empty when nothing depends on
    // `changed` (or `changed` is absent/invalid). On a cycle anywhere in the graph:
    // emits CyclicDependency, clears `out`, returns false (a cyclic graph has no
    // rebuild order — cycles are rejected at cook time, this is the runtime backstop).
    bool affected_by(AssetId changed, Array<AssetId>& out, DiagnosticList& diags) const;

    // Every referenced dependency id must be present in `registry`; otherwise a
    // MissingDependency diagnostic is emitted per missing id. Returns true iff
    // none are missing.
    bool validate_against(const AssetRegistry& registry, DiagnosticList& diags) const;

private:
    // CEIR-8h (ADR-0118): the internals are now the ONE engine (crd::containers::IncrementalDag). The API above is
    // BYTE-STABLE (AssetId ↔ NodeId is lossless + order-preserving — AssetId is a u64 value, valid()==value!=0,
    // value-ordered), so topo_order/affected_by emit the IDENTICAL order (the RAF-11 regression pin). `validate_against`
    // needs AssetRegistry (a render-asset type that cannot move into containers), so it stays here, iterating the engine.
    crd::containers::IncrementalDag m_dag;
    memory::IAllocator*             m_alloc;
};
} // namespace crd::renderasset
