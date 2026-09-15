#include "fiber_pool.hpp"
#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>

#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif CRD_OS_LINUX
#include <sys/mman.h>
#include <unistd.h>
#else
#error "crd-jobs fiber_pool: unsupported platform"
#endif

namespace crd::jobs::detail
{

// ---------------------------------------------------------------------------
// Platform stack allocation
//
// Layout (low → high address):
//   [guard_bytes — uncommitted/PROT_NONE]
//   [usable_bytes — committed/PAGE_READWRITE]
//
// Stacks grow downward; overflow hits the guard and crashes immediately.
// ---------------------------------------------------------------------------

#if CRD_OS_WINDOWS

static crd::usize platform_page_size() noexcept
{
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    return static_cast<crd::usize>(si.dwPageSize);
}

// Returns the VirtualAlloc base (= guard page start) on success, nullptr on failure.
// out_total receives the total reserved byte count (guard + usable).
static void* platform_stack_alloc(crd::usize usable_bytes, crd::usize guard_bytes,
                                   crd::usize& out_total) noexcept
{
    out_total    = guard_bytes + usable_bytes;
    void* base   = ::VirtualAlloc(nullptr, out_total, MEM_RESERVE, PAGE_NOACCESS);
    if (!base)
        return nullptr;
    // Commit only the usable region; guard stays reserved-but-inaccessible.
    auto* usable = static_cast<crd::u8*>(base) + guard_bytes;
    if (!::VirtualAlloc(usable, usable_bytes, MEM_COMMIT, PAGE_READWRITE))
    {
        ::VirtualFree(base, 0, MEM_RELEASE);
        return nullptr;
    }
    return base;
}

static void platform_stack_free(void* base, crd::usize /*total*/) noexcept
{
    if (base)
        ::VirtualFree(base, 0, MEM_RELEASE);
}

#else // POSIX / Linux

static crd::usize platform_page_size() noexcept
{
    return static_cast<crd::usize>(::sysconf(_SC_PAGESIZE));
}

static void* platform_stack_alloc(crd::usize usable_bytes, crd::usize guard_bytes,
                                   crd::usize& out_total) noexcept
{
    out_total    = guard_bytes + usable_bytes;
    void* base   = ::mmap(nullptr, out_total, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return nullptr;
    auto* usable = static_cast<crd::u8*>(base) + guard_bytes;
    if (::mprotect(usable, usable_bytes, PROT_READ | PROT_WRITE) != 0)
    {
        ::munmap(base, out_total);
        return nullptr;
    }
    return base;
}

static void platform_stack_free(void* base, crd::usize total) noexcept
{
    if (base)
        ::munmap(base, total);
}

#endif

// ---------------------------------------------------------------------------
// Tier init / shutdown
// ---------------------------------------------------------------------------

bool FiberPool::init_tier(Tier& tier, crd::u32 count, crd::usize usable_bytes,
                           FiberTier kind, void (*trampoline)())
{
    CRD_ASSERT_MSG(count > 0, "fiber pool: tier count must be > 0");

    const crd::usize guard_bytes = platform_page_size();
    tier.count       = count;
    tier.stack_bytes = usable_bytes;
    tier.fibers      = std::make_unique<Fiber[]>(count);
    tier.free_head.store(pack_head(kFiberNullIndex, 0U), std::memory_order_relaxed);
    tier.acquired_count.store(0U, std::memory_order_relaxed);
    tier.peak_count.store(0U, std::memory_order_relaxed);

    for (crd::u32 i = 0; i < count; ++i)
    {
        Fiber& f = tier.fibers[i];
        crd::usize total = 0;
        f.stack_alloc   = platform_stack_alloc(usable_bytes, guard_bytes, total);
        if (!f.stack_alloc)
        {
            // Partial failure: free every stack that was successfully committed.
            for (crd::u32 j = 0; j < i; ++j)
            {
                platform_stack_free(tier.fibers[j].stack_alloc, tier.fibers[j].alloc_size);
                tier.fibers[j].stack_alloc = nullptr;
            }
            return false;
        }
        f.alloc_size = total;
        f.pool_index = i;
        f.tier       = kind;
        // Wire the singly-linked free list: 0 → 1 → 2 → … → (count-1) → nil.
        f.next_free.store((i + 1U < count) ? (i + 1U) : kFiberNullIndex, std::memory_order_relaxed);

        // The usable stack starts just after the guard page.
        // fiber_init_stack computes the initial stack top as (usable_base + usable_bytes),
        // which is the high-address end — correct for a downward-growing stack.
        auto* usable_base = static_cast<crd::u8*>(f.stack_alloc) + guard_bytes;
        fiber_init_stack(f.context, usable_base, usable_bytes, trampoline);
        f.usable_base = usable_base;
        f.tsan_fiber  = sanitizer_create_fiber();
        f.usable_size = usable_bytes;
        f.trampoline  = trampoline;
    }

    // Point the Treiber head at fiber 0 (generation 0 for a fresh tier).
    tier.free_head.store(pack_head(0U, 0U), std::memory_order_release);
    return true;
}

void FiberPool::shutdown_tier(Tier& tier) noexcept
{
    if (!tier.fibers)
        return;
    CRD_ASSERT_MSG(tier.acquired_count.load(std::memory_order_relaxed) == 0U,
                   "FiberPool::shutdown: fibers are still Active");
    for (crd::u32 i = 0; i < tier.count; ++i)
    {
        if (tier.fibers[i].stack_alloc)
        {
            sanitizer_destroy_fiber(tier.fibers[i].tsan_fiber);
            platform_stack_free(tier.fibers[i].stack_alloc, tier.fibers[i].alloc_size);
            tier.fibers[i].stack_alloc = nullptr;
        }
    }
    tier.fibers.reset();
    tier.count = 0;
}

// ---------------------------------------------------------------------------
// Public lifecycle
// ---------------------------------------------------------------------------

bool FiberPool::init(const FiberPoolConfig& cfg)
{
    CRD_ASSERT_MSG(!m_initialized, "FiberPool::init called twice");
    CRD_ASSERT_MSG(cfg.trampoline != nullptr,
                   "FiberPool::init: trampoline must not be null");

    static constexpr crd::usize kSmallStack  =   64U * 1024U;
    static constexpr crd::usize kMediumStack =  512U * 1024U;
    static constexpr crd::usize kLargeStack  = 2048U * 1024U;

    if (!init_tier(m_small, cfg.small_count, kSmallStack, FiberTier::Small, cfg.trampoline))
        return false;

    if (!init_tier(m_medium, cfg.medium_count, kMediumStack, FiberTier::Medium, cfg.trampoline))
    {
        shutdown_tier(m_small);
        return false;
    }

    if (!init_tier(m_large, cfg.large_count, kLargeStack, FiberTier::Large, cfg.trampoline))
    {
        shutdown_tier(m_medium);
        shutdown_tier(m_small);
        return false;
    }

    m_initialized = true;
    return true;
}

void FiberPool::shutdown() noexcept
{
    if (!m_initialized)
        return;
    shutdown_tier(m_large);
    shutdown_tier(m_medium);
    shutdown_tier(m_small);
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Treiber stack — acquire (pop)
//
// Memory ordering rationale:
//   - Initial load uses acquire so we see all writes preceding the most-recent push.
//   - CAS success uses acq_rel: the acquire half synchronises with the pusher's release,
//     making fiber->next_free visible; the release half is unused here but harmless.
//   - CAS failure uses acquire so the refreshed `head` value is equally synchronised.
//   - next_free is an atomic link read/written relaxed; making it atomic removes
//     the TSan data race on the link itself. Relaxed suffices because the *ordering* that
//     makes the read observe the correct successor is carried by free_head — a pusher's
//     release CAS publishes both `idx` as head and its prior relaxed next_free store, and
//     our acquire load/CAS synchronises with it. Linearization point: the successful CAS.
//   - ABA: the value read from next_free is only committed if the CAS succeeds, i.e. `head`
//     (index AND generation) is unchanged since our load. Every pop bumps the generation,
//     so an interleaved pop+push that restores the same index cannot restore the same tag;
//     a stale next_free can never be installed. Uniqueness: a fiber is on the free list or
//     acquired, never both — a successful pop removes it before any acquirer can observe it.
// ---------------------------------------------------------------------------

Fiber* FiberPool::acquire_from(Tier& tier) noexcept
{
    crd::u64 head = tier.free_head.load(std::memory_order_acquire);
    while (true)
    {
        const crd::u32 idx = head_idx(head);
        if (idx == kFiberNullIndex)
        {
            // Tally the exhaustion event (readable back via progress_snapshot), then keep the documented
            // contract: assert with Ignore semantics and return nullptr. run_job_in_fiber turns that nullptr into
            // an always-on fatal (worker_pool.cpp) so a Release build fails fast instead of dropping the job;
            // the direct-pool stress tests rely on the graceful nullptr here.
            m_exhaustions.fetch_add(1U, std::memory_order_relaxed);
            CRD_ASSERT_MSG(false, "fiber pool exhausted — raise pool counts in jobs::Config");
            return nullptr;
        }

        const crd::u32 next    = tier.fibers[idx].next_free.load(std::memory_order_relaxed);
        const crd::u64 desired = pack_head(next, head_gen(head) + 1U); // bump gen on every pop

        if (tier.free_head.compare_exchange_weak(head, desired,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            Fiber* const f = &tier.fibers[idx];

#if CRD_ENABLE_ASSERTS
            CRD_ASSERT_MSG(f->state == FiberState::Idle,
                           "fiber pool: acquired fiber was not Idle — double-acquire?");
            f->state = FiberState::Active;
#endif
            // Update peak-usage watermark. All relaxed: these are profiling stats,
            // slight under-counting under heavy concurrency is acceptable.
            const crd::u32 now  = tier.acquired_count.fetch_add(1U, std::memory_order_relaxed) + 1U;
            crd::u32       peak = tier.peak_count.load(std::memory_order_relaxed);
            while (now > peak &&
                   !tier.peak_count.compare_exchange_weak(peak, now, std::memory_order_relaxed))
            {
            }

            return f;
        }
        // CAS failure: `head` has been atomically refreshed by compare_exchange_weak.
    }
}

// ---------------------------------------------------------------------------
// Treiber stack — release (push)
//
// Memory ordering rationale:
//   - Initial load is relaxed: we only need the bits (index), not ordering.
//   - CAS success uses release so our write to fiber->next_free is visible to any
//     subsequent acquire() that loads the head with memory_order_acquire.
//   - CAS failure uses relaxed: we retry with the updated `head`; no ordering needed.
//   - Generation is NOT bumped on push — only pops bump it. One pop between Thread A's
//     load and CAS is sufficient to change the generation and prevent ABA.
// ---------------------------------------------------------------------------

void FiberPool::release_to(Tier& tier, Fiber* fiber) noexcept
{
    CRD_ASSERT_MSG(fiber != nullptr, "FiberPool::release: fiber is nullptr");

#if CRD_ENABLE_ASSERTS
    CRD_ASSERT_MSG(fiber->state == FiberState::Active,
                   "FiberPool::release: fiber state must be Active (double-release?)");
    fiber->state = FiberState::Idle;
#endif

    tier.acquired_count.fetch_sub(1U, std::memory_order_relaxed);

    // Clear the opt-in livelock progress epoch so a recycled or free-list fiber reads as unmonitored (0) until
    // its next task opts in via jobs::note_progress(). Relaxed: the free_head CAS below carries publication.
    fiber->progress_epoch.store(0U, std::memory_order_relaxed);

    const crd::u32 idx = fiber->pool_index;
    crd::u64 head = tier.free_head.load(std::memory_order_relaxed);
    crd::u64 desired;
    do
    {
        fiber->next_free.store(head_idx(head), std::memory_order_relaxed);
        desired          = pack_head(idx, head_gen(head));
    } while (!tier.free_head.compare_exchange_weak(head, desired,
                  std::memory_order_release, std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Public acquire / release dispatch
// ---------------------------------------------------------------------------

Fiber* FiberPool::acquire(FiberTier tier_kind) noexcept
{
    CRD_ASSERT_MSG(m_initialized, "FiberPool::acquire called before init");
    return acquire_from(tier_of(tier_kind));
}

void FiberPool::release(Fiber* fiber) noexcept
{
    CRD_ASSERT_MSG(m_initialized, "FiberPool::release called before init");
    CRD_ASSERT_MSG(fiber != nullptr, "FiberPool::release: fiber is nullptr");
    release_to(tier_of(fiber->tier), fiber);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

crd::u32 FiberPool::available_count(FiberTier tier_kind) const noexcept
{
    const Tier& t        = tier_of(tier_kind);
    const crd::u32 acq   = t.acquired_count.load(std::memory_order_relaxed);
    return (acq <= t.count) ? (t.count - acq) : 0U;
}

crd::u32 FiberPool::peak_acquired(FiberTier tier_kind) const noexcept
{
    return tier_of(tier_kind).peak_count.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Tier accessor (const + non-const via duplication to avoid const_cast)
// ---------------------------------------------------------------------------

FiberPool::Tier& FiberPool::tier_of(FiberTier t) noexcept
{
    switch (t)
    {
    case FiberTier::Small:  return m_small;
    case FiberTier::Medium: return m_medium;
    case FiberTier::Large:  return m_large;
    }
    CRD_FATAL("FiberPool::tier_of: unknown FiberTier value");
    return m_small; // unreachable — suppresses compiler warning
}

const FiberPool::Tier& FiberPool::tier_of(FiberTier t) const noexcept
{
    switch (t)
    {
    case FiberTier::Small:  return m_small;
    case FiberTier::Medium: return m_medium;
    case FiberTier::Large:  return m_large;
    }
    CRD_FATAL("FiberPool::tier_of: unknown FiberTier value");
    return m_small; // unreachable
}

} // namespace crd::jobs::detail
