#pragma once

#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/memory_stats.hpp>

namespace crd::memory
{
// Abstract base for every allocator in the engine.
//
// Design notes:
//  - Containers and other consumers hold an `IAllocator*` (NOT a template
//    parameter). Type stays stable when the allocator changes — open-world
//    streaming becomes a drop-in change later.
//  - Five virtual functions; only three are required to be overridden.
//  - `reallocate` and `allocation_size` have default implementations so a
//    later TLSF / streaming allocator can override them without breaking
//    interface compatibility for existing allocators.
//  - Out-of-memory is fatal. The interface returns nullptr only if the
//    caller asked for size == 0, which is itself an assert in debug builds.
//  - Allocators are NOT thread-safe by default. Hand them out per-thread
//    or wrap them yourself. (One reason: the hot path stays branch-free.)
class IAllocator
{
public:
    virtual ~IAllocator() = default;

    // ---- Required overrides ---------------------------------------

    // Allocate `size` bytes with at least `alignment` byte alignment.
    // alignment must be a power of two.
    // Returns a non-null pointer; allocator triggers CRD_FATAL on OOM.
    virtual void* allocate(usize size, usize alignment = kDefaultAlignment) = 0;

    // Free a pointer previously returned by `allocate` or `reallocate`.
    // Calling deallocate(nullptr) is a no-op.
    virtual void deallocate(void* p) noexcept = 0;

    // True if `p` was allocated by this allocator. Used by composite
    // allocators (later) and by debug ownership checks.
    virtual bool owns(const void* p) const noexcept = 0;

    // ---- Optional overrides (default impls provided) --------------

    // Resize an allocation. Default: allocate + memcpy + deallocate.
    // Streaming/TLSF allocators can override for in-place growth.
    // If `p == nullptr`, behaves like allocate(new_size, alignment).
    // If `new_size == 0`, behaves like deallocate(p) and returns nullptr.
    virtual void* reallocate(void* p, usize old_size, usize new_size, usize alignment = kDefaultAlignment);

    // Return the actual allocation size for `p`, or 0 if unknown.
    // Useful for tools and for shrinking-without-realloc optimizations.
    virtual usize allocation_size(const void* p) const noexcept
    {
        (void)p;
        return 0;
    }

    // Non-throwing allocation: return a valid pointer or nullptr on failure
    // (out-of-memory / size==0), WITHOUT triggering CRD_FATAL. This is the path
    // composite allocators take to fall back gracefully (e.g.
    // GrowableTlsfAllocator::grow asks its parent for a chunk via try_allocate so
    // a VirtualMemoryAllocator parent's exhaustion yields nullptr, not a fatal).
    //
    // Default delegates to allocate(): correct for allocators whose allocate is
    // already non-fatal on exhaustion (bump / stack / pool). Allocators whose
    // allocate is fatal-on-OOM (MallocAllocator, TlsfAllocator,
    // VirtualMemoryAllocator, GrowableTlsfAllocator) OVERRIDE this with a real
    // nullptr-returning path. NOTE: appended at the END of the interface — never
    // insert a new virtual mid-interface (vtable-slot shift → wrong dispatch under
    // LTCG).
    [[nodiscard]] virtual void* try_allocate(usize size, usize alignment = kDefaultAlignment);

    // ---- Diagnostics (non-virtual, free for callers) --------------
    const char* name() const noexcept { return m_name; }
    const MemoryStats& stats() const noexcept { return m_stats; }

protected:
    const char* m_name = "UnnamedAllocator";
    MemoryStats m_stats{};
};

// ----------------------------------------------------------------------
// Process-global default allocator.
//
// Returns a pointer to a `MallocAllocator` instance. Containers, log
// sinks, and any other code that needs "just give me memory" should call
// `default_allocator()` rather than instantiating their own.
//
// The instance is lazily constructed on first call and never destroyed
// (its lifetime is "until process exit"). This avoids static-destruction
// ordering issues with subsystems that allocate during shutdown.
// ----------------------------------------------------------------------
IAllocator* default_allocator() noexcept;
} // namespace crd::memory
