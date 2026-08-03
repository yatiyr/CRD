#pragma once

// crd-render-asset-core — id registry + collision detection (RAF-1, mission §5 / Gate 1).
//
// A registry maps each AssetId to the canonical path that produced it, so a
// second registration of the SAME id with a DIFFERENT canonical path is caught
// as an IdCollision diagnostic (the astronomically-rare hash clash) or a
// DuplicateRegistration (a genuine authoring mistake — two refs colliding).
// Registering the same id + same canonical path is idempotent.
//
// Storage is a sorted-by-id Array with binary search — deterministic final state
// regardless of registration order, and no std container / no hash-map iteration
// nondeterminism.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderasset/identity.hpp>

namespace crd::renderasset
{
using crd::containers::Array;
using crd::containers::String;
using crd::containers::StringView;

class AssetRegistry
{
public:
    explicit AssetRegistry(memory::IAllocator* alloc) noexcept : m_entries(alloc), m_alloc(alloc) {}

    // Register `ref`. Returns true if it is now present with a consistent
    // mapping (freshly added OR an idempotent re-register of the same path).
    // On a same-id/different-path clash, emits a diagnostic (IdCollision) into
    // `diags` and returns false. Registering an invalid ref emits nothing and
    // returns false.
    bool register_ref(const AssetRef& ref, DiagnosticList& diags);

    // Convenience: parse `raw` then register it. Parse diagnostics AND collision
    // diagnostics land in `diags`. Returns false if parse or registration fails.
    bool register_path(StringView raw, DiagnosticList& diags);

    // Lower-level id+path registration (the seam register_ref/register_path build
    // on). Exposed so a test can force the collision branch — real FNV-1a
    // collisions between distinct canonical strings are unreachable by hand.
    // Same id+same path is idempotent; same id+different path emits IdCollision.
    bool register_id(AssetId id, StringView canonical, DiagnosticList& diags);

    [[nodiscard]] bool contains(AssetId id) const noexcept;
    // Look up the canonical path for `id`. Returns false if absent.
    [[nodiscard]] bool lookup(AssetId id, StringView& out_canonical) const noexcept;

    [[nodiscard]] usize size() const noexcept { return m_entries.size(); }

private:
    struct Entry
    {
        AssetId id;
        String canonical;
    };

    // Returns the index where `id` is or would be inserted (lower_bound by id).
    [[nodiscard]] usize lower_bound(AssetId id) const noexcept;

    Array<Entry> m_entries; // sorted ascending by id.value
    memory::IAllocator* m_alloc;
};
} // namespace crd::renderasset
