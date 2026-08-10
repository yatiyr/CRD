#pragma once

// crd-ceir — the ANALYSIS + PASS managers (CEIR-8g, ADR-0117, U-§65/U-§73). FRAMEWORKS ONLY — no optimization passes
// (those are CEIR-26/27). ⛔ SEPARATE classes, NOT Context members (the U-§72 god-object bound). An analysis is a
// cached computation over a Module; a pass declares which analyses it PRESERVES, and the manager EVICTS the rest after
// it runs (never-stale). ⛔ NO inter-analysis dependency tracking — that would be a new dependency graph one slice
// before CEIR-8h unifies them; inter-analysis dependencies are a documented pass-author contract, named-forward to 8h.

#include <crd/ceir/context.hpp>
#include <crd/ceir/diagnostic.hpp> // a pass emits into the DiagnosticEngine; a Fatal stops the pipeline
#include <crd/ceir/id.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>

namespace crd::ceir
{
// A typed analysis's compile-time id (the make_interface_id shape, sharing id.hpp's fnv1a_ct).
[[nodiscard]] constexpr AnalysisId make_analysis_id(const char* name) noexcept { return AnalysisId{fnv1a_ct(name)}; }

// The AnalysisManager caches analysis RESULTS keyed by AnalysisId and invalidates them on pass mutation. A registered
// analysis `T` provides `static constexpr AnalysisId kId` and `static const T* compute(Context&, const Module&,
// memory::GrowableLinearAllocator&)` (the result is arena-allocated from the manager's arena). `get<T>` returns the
// cached result or computes+caches it. ⛔ eviction LEAKS the result into the arena (the grow-by-rebuild precedent; a
// Context/manager is a bounded build lifetime).
class AnalysisManager
{
public:
    explicit AnalysisManager(memory::IAllocator* alloc) : m_arena(kArenaChunk, alloc), m_cache(alloc) {}

    template <typename T> [[nodiscard]] const T* get(Context& ctx, const Module& m)
    {
        for (usize i = 0; i < m_cache.size(); ++i)
        {
            if (m_cache[i].id == T::kId) { return static_cast<const T*>(m_cache[i].result); } // cached (never-redundant)
        }
        const T* const result = T::compute(ctx, m, m_arena); // compute once (never-stale after an invalidation)
        m_cache.push_back(Entry{T::kId, result});
        return result;
    }

    // Is analysis `id` currently cached? (For tests + the PassManager's own reasoning.)
    [[nodiscard]] bool is_cached(AnalysisId id) const noexcept
    {
        for (usize i = 0; i < m_cache.size(); ++i)
        {
            if (m_cache[i].id == id) { return true; }
        }
        return false;
    }

    // Evict every cached analysis NOT in `preserved` (the never-stale core). Called by the PassManager after a pass
    // that reported `changed`. In-place compaction (the program_capabilities dedup shape).
    void invalidate(containers::ConstSpan<AnalysisId> preserved) noexcept
    {
        usize w = 0;
        for (usize i = 0; i < m_cache.size(); ++i)
        {
            bool keep = false;
            for (usize p = 0; p < preserved.size() && !keep; ++p) { keep = m_cache[i].id == preserved[p]; }
            if (keep) { m_cache[w++] = m_cache[i]; }
        }
        while (m_cache.size() > w) { m_cache.pop_back(); }
    }
    void invalidate_all() noexcept { m_cache.clear(); } // preserve_none

private:
    static constexpr usize kArenaChunk = 64U * 1024U;
    struct Entry
    {
        AnalysisId  id;
        const void* result;
    };
    memory::GrowableLinearAllocator m_arena; // analysis results (eviction leaks — accepted, documented)
    containers::Array<Entry>        m_cache; // small; linear scan
};

// A pass: a name (for diagnostics — no pass id, nothing queries a pass by id), a run fn returning `changed`, and the
// set of analyses it PRESERVES. ⛔ a pass that returns `changed == false` preserves EVERYTHING regardless of the set (a
// pass that did nothing invalidates nothing). nullptr run ⇒ a no-op identity pass (the FRAMEWORK test consumer).
struct Pass
{
    containers::StringView            name;
    bool                              (*run)(Context&, Module&, DiagnosticEngine&) = nullptr; // returns `changed`
    containers::ConstSpan<AnalysisId> preserved                                   = {};
};

// The PassManager sequences passes, drives the AnalysisManager invalidation after each, and ⛔ STOPS the pipeline when
// a pass emits a Fatal (the "short-circuit a compilation" semantic — a later pass must not run on a fatally-broken IR).
class PassManager
{
public:
    explicit PassManager(memory::IAllocator* alloc) : m_passes(alloc) {}
    void add_pass(const Pass& p) { m_passes.push_back(p); }

    void run(Context& ctx, Module& m, AnalysisManager& am, DiagnosticEngine& diag) const
    {
        for (usize i = 0; i < m_passes.size(); ++i)
        {
            const Pass& p       = m_passes[i];
            const bool  changed = p.run != nullptr && p.run(ctx, m, diag);
            if (changed) { am.invalidate(p.preserved); } // unchanged ⇒ preserve-all (invalidate nothing)
            if (diag.has_fatal()) { break; }             // ⛔ Fatal short-circuits — do NOT run later passes
        }
    }
    [[nodiscard]] usize size() const noexcept { return m_passes.size(); }

private:
    containers::Array<Pass> m_passes;
};
} // namespace crd::ceir
