#pragma once

#include "fiber.hpp"
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <memory>

// MSVC: C4324 fires on any struct that receives trailing padding due to an alignas member.
// This is expected — we use alignas(64) intentionally to prevent inter-tier false sharing.
// Suppress it around the structs that carry the alignment attribute.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::jobs::detail
{

struct FiberPoolConfig
{
    crd::u32  small_count  = 128;
    crd::u32  medium_count = 64;
    crd::u32  large_count  = 16;
    void    (*trampoline)() = nullptr; // entry point burned into every fiber stack at init()
};

// Pre-allocated, lock-free pool of fibers in three stack-size tiers.
//
// Stack layout per fiber (low address → high address, stack grows downward):
//   [ guard page — uncommitted / PROT_NONE ]  ← stack overflow crashes here immediately
//   [ usable stack — committed / PAGE_READWRITE ]
//
// Free list per tier: Treiber stack with a tagged 64-bit head.
//   Bits [31: 0] = fiber index into the tier's fibers[] array (kFiberNullIndex if empty).
//   Bits [63:32] = pop-generation counter, incremented on every successful pop.
//
// ABA prevention: each successful pop increments the generation. A stale pop-CAS that sees
// the same fiber index must also see the same generation to succeed — impossible if any pop
// has occurred in between. This eliminates ABA without CMPXCHG16B or epoch-based reclamation.
//
// Thread safety: acquire() and release() are fully lock-free from any number of threads.
class FiberPool
{
public:
    FiberPool()  = default;
    ~FiberPool() { shutdown(); }

    FiberPool(const FiberPool&)            = delete;
    FiberPool& operator=(const FiberPool&) = delete;
    FiberPool(FiberPool&&)                 = delete;
    FiberPool& operator=(FiberPool&&)      = delete;

    // Allocate all stacks, call fiber_init_stack on every fiber with cfg.trampoline.
    // Returns false if any platform allocation fails; any partial state is torn down.
    [[nodiscard]] bool init(const FiberPoolConfig& cfg);

    // Free all stacks. Must be called only when no fibers are Active (debug-asserted).
    void shutdown() noexcept;

    // Pop a fiber from the tier's free list.
    //   Debug: asserts (with Ignore semantics) if the pool is exhausted, then returns nullptr.
    //   Release: returns nullptr silently on exhaustion.
    [[nodiscard]] Fiber* acquire(FiberTier tier) noexcept;

    // Push a fiber back to its tier's free list.
    //   Debug: asserts fiber->state == Active before transitioning it back to Idle.
    void release(Fiber* fiber) noexcept;

    // Number of currently available (Idle) fibers in the given tier.
    [[nodiscard]] crd::u32 available_count(FiberTier tier) const noexcept;

    // Highest simultaneous Active count recorded since init() (debug/profiling).
    [[nodiscard]] crd::u32 peak_acquired(FiberTier tier) const noexcept;

    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }

private:
    // One free-list tier. alignas(64) places each tier on its own cache line so concurrent
    // acquire()/release() on different tiers never invalidate each other's lines.
    // Fields within a tier are touched together (free_head + acquired_count in one call),
    // so no further intra-tier separation is needed.
    struct alignas(64) Tier
    {
        std::unique_ptr<Fiber[]> fibers;
        crd::u32                 count       = 0;
        crd::usize               stack_bytes = 0;
        std::atomic<crd::u64>    free_head   {pack_head(kFiberNullIndex, 0u)};
        std::atomic<crd::u32>    acquired_count{0u};
        std::atomic<crd::u32>    peak_count    {0u};
    };

    // Tagged-head helpers: [gen:32 | idx:32]
    static constexpr crd::u64 pack_head(crd::u32 idx, crd::u32 gen) noexcept
    {
        return (crd::u64(gen) << 32) | crd::u64(idx);
    }
    static constexpr crd::u32 head_idx(crd::u64 h) noexcept { return crd::u32(h); }
    static constexpr crd::u32 head_gen(crd::u64 h) noexcept { return crd::u32(h >> 32); }

    [[nodiscard]] bool init_tier(Tier& tier, crd::u32 count, crd::usize usable_bytes,
                                 FiberTier kind, void (*trampoline)());
    void               shutdown_tier(Tier& tier) noexcept;

    [[nodiscard]] Fiber* acquire_from(Tier& tier) noexcept;
    void                 release_to(Tier& tier, Fiber* fiber) noexcept;

    [[nodiscard]] Tier&       tier_of(FiberTier t)       noexcept;
    [[nodiscard]] const Tier& tier_of(FiberTier t) const noexcept;

    Tier m_small;
    Tier m_medium;
    Tier m_large;
    bool m_initialized = false;
};

} // namespace crd::jobs::detail

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
