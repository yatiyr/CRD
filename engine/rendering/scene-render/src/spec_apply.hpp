#pragma once

// CEIR-31b-3-a-ii — the SPEC-SET → KGraph application seat (scene-render internal).
//
// The ONE place a frame pass's authored spec_N params meet an authored `.ckir` program: after ckir_read and before
// create_program the host patches the D12 spec-const node DEFAULTS to the pass's values. The emitted default IS the
// pipeline value on both backends (there is no VkSpecializationInfo path — ckir.hpp:1692), so patching the default is
// the whole specialization. This header owns the two DEVICE-FREE halves of that seat — apply_spec_set (the patch,
// ALL-OR-NOTHING) and spec_set_hash/spec_set_equal (the program cache key + its collision backstop); the device half
// (create_program + the cached raster program) lives in the ui provider (scene_renderer.cpp ensure_ui_program).
//
// scene-render already depends on frame-cook (SceneHost implements IFrameGraphHost) and kir (it cooks `.ckir`), so
// naming SpecSet + KGraph here is not a new module arrow.

#include <cstring> // std::memcpy — the codebase's bit-cast idiom (ckir_asset.hpp:150, ckir.hpp:926)

#include <crd/core/assert.hpp>             // CRD_ASSERT_MSG — the duplicate-id contract check (cook-guaranteed, debug-only)
#include <crd/framecook/frame_asset.hpp>   // SpecConst · SpecSet · kMaxSpecConsts
#include <crd/framecook/frame_runtime.hpp> // FrameExecError
#include <crd/kir/ckir.hpp>                 // KGraph · KNode · is_spec_const · spec_const_id

namespace crd::scenerender
{

// Apply `specs` to `fg`'s spec-const nodes, ALL-OR-NOTHING. If ANY id is not a spec-const in the graph, patch NOTHING
// and report SpecConstNotInProgram (via `err` when non-null) — a PARTIAL apply would be a silent half-specialization
// (the asset-OK-but-WRONG class: some knobs set, the missing one left at a default the author never asked for). On
// success every id's spec-const default is set to its value and the graph is ready for create_program. An empty SpecSet
// is a no-op success (the program at its authored defaults — the honest spec-free resolve). Returns true iff applied.
[[nodiscard]] inline bool apply_spec_set(crd::kir::KGraph& fg, crd::framecook::SpecSet specs,
                                         crd::framecook::FrameExecError* err) noexcept
{
    const auto& nodes = fg.serial_nodes();
    // pass 1: every id MUST be present as a spec-const before we mutate anything (all-or-nothing), and ids MUST be
    // unique. Cook guarantees uniqueness (SpecConstDuplicateId) for the record path, so a duplicate reaching here is a
    // caller that bypassed cook (a test/app hand-building the SpecSet) — a contract violation, asserted like
    // build_pass_spec_set's overflow assert. Uniqueness is also the premise spec_set_hash/spec_set_equal rely on: a
    // duplicate id makes the sorted hash order-dependent and the by-id compare order-sensitive, so forbid it at source.
    for (crd::u32 s = 0U; s < specs.count; ++s)
    {
        for (crd::u32 t = 0U; t < s; ++t)
        {
            CRD_ASSERT_MSG(specs.items[t].id != specs.items[s].id,
                           "apply_spec_set: duplicate spec-const id (this SpecSet bypassed the cook dup check)");
        }
        bool present = false;
        for (crd::usize i = 0; i < nodes.size(); ++i)
        {
            if (crd::kir::is_spec_const(nodes[i]) && crd::kir::spec_const_id(nodes[i]) == specs.items[s].id)
            {
                present = true;
                break;
            }
        }
        if (!present)
        {
            if (err != nullptr) { *err = crd::framecook::FrameExecError::SpecConstNotInProgram; }
            return false;
        }
    }
    // pass 2: apply — each id is proven present, so set_spec_const patches at least one node.
    for (crd::u32 s = 0U; s < specs.count; ++s) { (void)fg.set_spec_const(specs.items[s].id, specs.items[s].value); }
    return true;
}

// A content hash of a SpecSet for the ui provider's program cache. ORDER-INDEPENDENT (set_spec_const patches by id and
// ignores order, so a{0,1} and a{1,0} specialize identically) but VALUE-SENSITIVE: sort a stack copy by id, then FNV-1a
// over the (id, IEEE-bits-of-value) byte stream. This is a BUCKET key, not an identity — the cache verifies the stored
// SpecSet against the incoming one element-wise (spec_set_equal) on a hit, so a rare collision costs a recook, never a
// wrong program.
[[nodiscard]] inline crd::u64 spec_set_hash(crd::framecook::SpecSet specs) noexcept
{
    crd::framecook::SpecConst tmp[crd::framecook::kMaxSpecConsts];
    const crd::u32 n = specs.count <= crd::framecook::kMaxSpecConsts ? specs.count : crd::framecook::kMaxSpecConsts;
    for (crd::u32 i = 0U; i < n; ++i) { tmp[i] = specs.items[i]; }
    for (crd::u32 i = 1U; i < n; ++i) // insertion sort by id (n <= 16)
    {
        const crd::framecook::SpecConst key = tmp[i];
        crd::u32                        j   = i;
        while (j > 0U && tmp[j - 1U].id > key.id)
        {
            tmp[j] = tmp[j - 1U];
            --j;
        }
        tmp[j] = key;
    }
    crd::u64 h = 0xCBF29CE484222325ULL; // FNV-1a offset basis
    for (crd::u32 i = 0U; i < n; ++i)
    {
        crd::u64 valbits = 0U;
        std::memcpy(&valbits, &tmp[i].value, sizeof(valbits)); // f64 -> bits (value-sensitive)
        const crd::u64 idbits = tmp[i].id;
        for (int b = 0; b < 8; ++b) { h = (h ^ ((idbits >> (b * 8)) & 0xFFU)) * 0x100000001B3ULL; }
        for (int b = 0; b < 8; ++b) { h = (h ^ ((valbits >> (b * 8)) & 0xFFU)) * 0x100000001B3ULL; }
    }
    return h;
}

// Order-independent equality of two SpecSets — the cache's collision backstop behind spec_set_hash. Equal iff the counts
// match and every id in `a` is present in `b` with the same value (ids are unique in a valid set — cook rejects
// SpecConstDuplicateId — so count-equal + all-present is a bijection). O(n^2), n <= 16.
[[nodiscard]] inline bool spec_set_equal(crd::framecook::SpecSet a, crd::framecook::SpecSet b) noexcept
{
    if (a.count != b.count) { return false; }
    for (crd::u32 i = 0U; i < a.count; ++i)
    {
        bool matched = false;
        for (crd::u32 j = 0U; j < b.count; ++j)
        {
            if (a.items[i].id == b.items[j].id)
            {
                if (a.items[i].value != b.items[j].value) { return false; } // same id, different value => distinct set
                matched = true;
                break;
            }
        }
        if (!matched) { return false; }
    }
    return true;
}

} // namespace crd::scenerender
