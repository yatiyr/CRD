#pragma once

#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/ring_allocator.hpp>
#include <crd/memory/allocators/virtual_memory_allocator.hpp>

#include <atomic>
#include <mutex>

namespace crd::memory
{
// A resource category — caller-assigned meaning (textures, meshes, audio, ...).
// IDs are [0, num_categories); the StreamingAllocator imposes no taxonomy.
using CategoryId = u32;

class StreamingAllocator;

// Injected residency policy (ADR-0085 D4): the mechanism (budgets + the resident
// store) lives in StreamingAllocator; the DECISION of what to shed under pressure
// is the consumer's. When a resident allocation would exceed its category budget
// (or the resident heap is physically full), the StreamingAllocator calls evict()
// in a loop until the request fits or evict() reports it can free no more.
//
// The policy frees space by calling sa.release_resident(...) on the allocations it
// chose to evict (the policy owns its own knowledge of what is resident — typically
// a future wrapper over the ResourceManager's 2Q-LRU). evict() returns the number
// of bytes it actually freed; returning 0 means "nothing left to shed" and the
// allocation fails gracefully (nullptr). MUST return 0 when it cannot free more, or
// the pressure loop livelocks.
class IResidencyPolicy
{
public:
    virtual ~IResidencyPolicy() = default;
    [[nodiscard]] virtual u64 evict(StreamingAllocator& sa, CategoryId category, u64 needed_bytes) = 0;
};

// The default: sheds nothing. Over-budget / heap-full allocations simply fail
// (return nullptr). Ships until a real consumer (Phase 2.7 texture/mesh streaming)
// supplies a policy wrapping the ResourceManager's 2Q-LRU eviction.
class NullResidencyPolicy final : public IResidencyPolicy
{
public:
    [[nodiscard]] u64 evict(StreamingAllocator& /*sa*/, CategoryId /*category*/, u64 /*needed*/) override
    {
        return 0;
    }
};

// StreamingAllocator — the open-world streaming policy layer (ADR-0085 S5).
//
// Composes the cluster into one residency-and-budget surface:
//   - a VirtualMemoryAllocator (S2) reservation as the malloc-free backing store,
//   - a GrowableTlsfAllocator (S3) over it as the RESIDENT store (real O(1)
//     per-payload free, stable addresses — a resource streams in and out
//     individually),
//   - a RingAllocator (S4) as the thread-safe, epoch/fence-gated STAGING arena for
//     the async load path.
// Per-CategoryId soft byte budgets gate resident allocation; an injected
// IResidencyPolicy sheds under pressure. Mechanism here; policy injected.
//
// NOT wired to crd-resources in this slice (ADR-0085 S5): the ResourceManager
// integration — routing loader payloads through the resident store and driving the
// policy from the 2Q-LRU — lands with the first real streaming consumer (Phase 2.7),
// where the integration shape is known. This is the standalone substrate it builds on.
//
// THREAD-SAFETY: the resident path (try_allocate_resident / release_resident) is
// serialized by an internal mutex (the load path, not the per-frame hot path —
// matching ResourceManager's own mutex). The staging path forwards to RingAllocator
// and is lock-free + multi-producer. Budget counters are atomic (lock-free reads).
class StreamingAllocator final
{
public:
    static constexpr u32 kMaxCategories = 16;

    struct Config
    {
        usize reserve_bytes        = usize{8} << 30;  // total VM reservation (resident + staging)
        usize resident_chunk_bytes = usize{64} << 20; // resident GrowableTlsf chunk granularity
        usize staging_bytes        = usize{64} << 20; // staging RingAllocator capacity
        usize staging_epochs       = 4;               // staging epochs in flight (power-of-two)
        u32   num_categories       = 8;               // <= kMaxCategories; budgets default to unlimited
    };

    // Default ctor kept separate from the Config ctor so the brace default isn't
    // formed inside the class body (clang rejects evaluating Config's default member
    // initializers there — same pattern as VirtualMemoryAllocator).
    StreamingAllocator();
    explicit StreamingAllocator(const Config& cfg, IResidencyPolicy* policy = nullptr,
                                const char* name = "StreamingAllocator");
    ~StreamingAllocator();

    StreamingAllocator(const StreamingAllocator&)            = delete;
    StreamingAllocator& operator=(const StreamingAllocator&) = delete;

    // ---- Resident store (thread-safe; load path) -----------------------
    // Allocate `size` resident bytes charged to `category`. If the category is over
    // budget (or the resident heap is full), the injected policy is asked to shed
    // until it fits; returns nullptr if it cannot (graceful — no fatal). The pointer
    // is stable for its lifetime (no relocation).
    [[nodiscard]] void* try_allocate_resident(CategoryId category, usize size, usize alignment = kDefaultAlignment);

    // Free a resident allocation and un-charge its bytes from `category`. O(1); the
    // bytes return to the resident heap for immediate reuse.
    void release_resident(CategoryId category, void* p) noexcept;

    // ---- Staging (lock-free; async-load path) --------------------------
    [[nodiscard]] void* try_stage(usize size, usize alignment = kDefaultAlignment) noexcept;
    void begin_staging_epoch(u64 fence) noexcept;
    void retire_staging(u64 completed_fence) noexcept;

    // ---- Budgets -------------------------------------------------------
    void set_budget(CategoryId category, u64 bytes) noexcept;  // soft ceiling (default: unlimited)
    [[nodiscard]] u64 budget(CategoryId category) const noexcept;
    [[nodiscard]] u64 used(CategoryId category) const noexcept; // bytes charged to the category

    // ---- Diagnostics ---------------------------------------------------
    [[nodiscard]] u32   num_categories() const noexcept { return m_num_categories; }
    [[nodiscard]] usize resident_committed_bytes() const noexcept { return m_resident_vm.committed_bytes(); }
    [[nodiscard]] usize staging_capacity() const noexcept { return m_staging.capacity(); }
    [[nodiscard]] const char* name() const noexcept { return m_name; }

private:
    // limit/used are atomic so used()/budget() diagnostics read lock-free; ALL
    // mutators run under m_resident_mtx (the atomics are not a substitute for it).
    struct Budget
    {
        std::atomic<u64> limit{~u64{0}}; // unlimited by default
        std::atomic<u64> used{0};
    };

    const char*            m_name;
    u32                    m_num_categories;
    IResidencyPolicy*      m_policy;
    NullResidencyPolicy    m_null_policy; // used when no policy injected

    VirtualMemoryAllocator m_resident_vm;   // backing reservation (constructed first)
    GrowableTlsfAllocator  m_resident_heap; // resident store, parented on m_resident_vm
    RingAllocator          m_staging;       // staging ring, buffer from m_resident_vm

    std::mutex m_resident_mtx;              // guards m_resident_heap + budget charge
    Budget     m_budgets[kMaxCategories];
};
} // namespace crd::memory
