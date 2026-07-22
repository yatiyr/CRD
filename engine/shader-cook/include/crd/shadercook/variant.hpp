// crd-shader-cook — D-007 D3 (ADR-0104): the VARIANT / permutation system.
//
// A shader is authored as an übershader with feature toggles; a VARIANT is one point in that toggle space (a `key` bitmask).
// This layer cooks the requested variants (ON-DEMAND — only what the scene asks for, UE5-style), CONTENT-HASH DEDUPs them
// (two keys whose specialized kernels are identical share one cooked bundle), and reports the reduction (requested vs unique).
//
// The specialization itself is the caller's `VariantBuildFn`: it constructs the kernel for a given key, emitting only the LIVE
// path (exactly how real material compilers cook per-permutation) — so keys that produce the same kernel serialize to identical
// IR and dedup automatically. This composes with B7 `specialize()` (an author may build an übergraph then pin options), without
// requiring in-place graph surgery.
#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_lower.hpp> // lower_entry — the material/raster specialization fold
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/shadercook/cook.hpp>

namespace crd::shadercook
{

// Build the compute kernel for feature-bitmask `key` into `g` and return its entry. Emit only the LIVE path for `key` so that
// keys producing the same kernel yield identical IR (⇒ they dedup). `user` is opaque caller context.
using VariantBuildFn = crd::kir::KEntry (*)(crd::kir::KGraph& g, crd::u32 key, void* user);

namespace variant_detail
{
// Collect the addresses of an entry's live root node-refs (the same set lower_entry / cook::specialize_variant gather).
[[nodiscard]] inline int gather_roots(crd::kir::KEntry& e, int** slots, int n) noexcept
{
    if (e.position >= 0) { slots[n++] = &e.position; }
    if (e.frag_depth >= 0) { slots[n++] = &e.frag_depth; }
    if (e.discard_cond >= 0) { slots[n++] = &e.discard_cond; }
    if (e.shading_rate >= 0) { slots[n++] = &e.shading_rate; }
    if (e.storage_write_index >= 0) { slots[n++] = &e.storage_write_index; }
    if (e.storage_write_value >= 0) { slots[n++] = &e.storage_write_value; }
    for (int k = 0; k < e.n_out; ++k) { slots[n++] = &e.out[k].node; }
    return n;
}
} // namespace variant_detail

// ── Übergraph variant style — the elegant alternative to a per-key builder ───────────────────────────────────────────
// TWO ways to author a variant, both flowing through the SAME cook_variant_matrix:
//   (A) per-key builder — a VariantBuildFn constructs the kernel for `key`, emitting only the live path (real-compiler style).
//   (B) übergraph — build ONE graph with `ShaderOption` selector nodes and option-gated `Select`s, then call `specialize()`
//       inside the builder to pin the options to the key's bits and const-fold/DCE the dead branches. Declarative + toggle-based.
// Both fold identical shaders to identical IR ⇒ identical content hash ⇒ the same dedup. `specialize` pins option i to bit i of
// `key` (1.0 if set, else 0.0) and folds — dispatching on the entry's stage so ONE helper covers COMPUTE kernels AND raster
// MATERIALS. All options are pinned BEFORE the single fold, because folding renumbers the graph and would stale the remaining
// option ids. Call it after building the übergraph.
inline void specialize(crd::kir::KGraph& g, crd::kir::KEntry& e, const int* options, crd::u32 key, int n_options)
{
    const int n = n_options < 32 ? n_options : 32;
    if (e.is_kernel())
    {
        crd::f64 values[32];
        for (int i = 0; i < n; ++i) { values[i] = ((key >> static_cast<crd::u32>(i)) & 1U) != 0U ? 1.0 : 0.0; }
        g.specialize_kernel(e, options, values, n); // compute body: pin + gather statement roots + optimize + write back
    }
    else
    {
        // material/raster: pin ALL options, then the B7 specialize sequence over the entry's output roots — optimize folds
        // scalar consts, fold_static_branches collapses const-condition Selects (incl. VECTOR selects, which optimize's
        // scalar-only fold skips — an emissive vec3 Select is exactly that case), then optimize DCEs the dead branches.
        for (int i = 0; i < n; ++i) { g.pin_const(options[i], ((key >> static_cast<crd::u32>(i)) & 1U) != 0U ? 1.0 : 0.0); }
        crd::kir::lower::lower_entry(g, e);
        crd::kir::lower::fold_static_branches(g);
        crd::kir::lower::lower_entry(g, e);
    }
}

// D6: specialize a SHARED vertex+fragment RASTER graph as ONE unit. `optimize`'s renumber rewrites the whole graph, so
// specializing one entry then the other would stale the sibling's node ids — instead this pins all options, then folds ONCE
// over BOTH entries' roots (VS position/varyings + FS out[]/discard), so a MATERIAL übershader cooks correctly as VS+FS
// variants. Same B7 sequence as the single-entry material path (fold → collapse const-condition Selects → DCE).
inline void specialize(
    crd::kir::KGraph& g, crd::kir::KEntry& vs, crd::kir::KEntry& fs, const int* options, crd::u32 key, int n_options)
{
    const int n = n_options < 32 ? n_options : 32;
    for (int i = 0; i < n; ++i) { g.pin_const(options[i], ((key >> static_cast<crd::u32>(i)) & 1U) != 0U ? 1.0 : 0.0); }
    int*       slots[2 * (crd::kir::kMaxStageOutputs + 6)];
    const auto fold = [&]() {
        int ns = variant_detail::gather_roots(vs, slots, 0);
        ns     = variant_detail::gather_roots(fs, slots, ns);
        int roots[2 * (crd::kir::kMaxStageOutputs + 6)];
        for (int i = 0; i < ns; ++i) { roots[i] = *slots[i]; }
        crd::kir::lower::lower(g, roots, ns);
        for (int i = 0; i < ns; ++i) { *slots[i] = roots[i]; }
    };
    fold();
    crd::kir::lower::fold_static_branches(g);
    fold();
}

struct VariantManifestEntry
{
    crd::u32                   key = 0;  // the requested feature bitmask
    crd::resources::ResourceId hash;     // content-addressed id of its cooked bundle (repeats across deduped keys)
};

struct VariantMatrixResult
{
    bool                                         ok        = false;
    crd::u32                                     requested = 0; // # keys requested
    crd::u32                                     unique    = 0; // # DISTINCT cooked bundles — the permutation reduction
    crd::containers::Array<VariantManifestEntry> entries;       // one per requested key, in request order (deduped hashes repeat)
    crd::containers::String                      error;
    explicit VariantMatrixResult(crd::memory::IAllocator* a) : entries(a), error(a) {}
};

// Cook the requested variant keys with CONTENT-HASH DEDUP (identical specialized IR ⇒ cooked once) and ON-DEMAND coverage (only
// these keys). Set `opts.cache_dir` — bundles are written there content-addressed, so a deduped key is a cache hit (no re-cook).
[[nodiscard]] VariantMatrixResult cook_variant_matrix(
    VariantBuildFn build, void* user, const crd::u32* keys, int n_keys, const CookOptions& opts, crd::memory::IAllocator* a);

// On-demand: build + cook a single variant key.
[[nodiscard]] CookResult cook_one_variant(
    VariantBuildFn build, void* user, crd::u32 key, crd::containers::StringView name, const CookOptions& opts,
    crd::memory::IAllocator* a);

// D10: cook the requested variant keys in PARALLEL on the fiber-based crd-jobs scheduler. Determines the unique set serially
// (cheap: build + hash), then cooks each UNIQUE variant concurrently — every job on its OWN allocator, writing content-addressed
// to `opts.cache_dir` (set it). Byte-identical to the serial cook (content-hash dedup is order-independent). The caller must
// have called jobs::init(). Same VariantMatrixResult (manifest + requested/unique) as the serial matrix.
[[nodiscard]] VariantMatrixResult cook_variant_matrix_parallel(
    VariantBuildFn build, void* user, const crd::u32* keys, int n_keys, const CookOptions& opts, crd::memory::IAllocator* a);

// ── D8: the multi-variant container — the shipping form of a permutation set ──────────────────────────────────────────
// Cook the requested keys into ONE `.crdr`: a `VART` table (requested key → UNIQUE-bundle index, content-hash deduped) + the
// unique variant bytecodes (`VB00`, `VB01`, …). One file, key-addressed, deduped — the loader touches only the asked-for
// variant's blob. SpirV backend (compute); the crdr bytes are in `CookResult.crdr`, `requested`/`unique` report the reduction.
[[nodiscard]] CookResult cook_variant_container(
    VariantBuildFn build, void* user, const crd::u32* keys, int n_keys, const CookOptions& opts, crd::memory::IAllocator* a);

// A parsed multi-variant container. The source `bytes` must OUTLIVE this (chunk payloads are views into it).
struct VariantContainer
{
    crd::resources::CrdrFile file;
    explicit VariantContainer(crd::memory::IAllocator* a) : file(a) {}
    // The (shared) variant bytecode for `key` — the D4 runtime load-by-key seam. {} if the key isn't in the container.
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> bytecode(crd::u32 key) const noexcept;
    [[nodiscard]] crd::u32                            unique_count() const noexcept;    // distinct cooked bundles
    [[nodiscard]] crd::u32                            requested_count() const noexcept; // keys in the table
};
[[nodiscard]] bool read_variant_container(crd::containers::ConstSpan<crd::u8> bytes, VariantContainer& out);

} // namespace crd::shadercook
