#pragma once

#include <crd/memory/allocator.hpp>

#include <crd/vm/virtual_memory.hpp>

namespace crd::memory
{
// VirtualMemoryAllocator — stable-address bump arena over crd::vm (ADR-0085 S2).
//
// The CPU open-world streaming strategy: reserve one huge contiguous address
// range up front (default 64 GiB — free on 64-bit, only address space), then
// commit physical pages on demand as the bump pointer advances. Addresses are
// STABLE for the arena's lifetime (the reservation never moves, we never
// re-reserve), so anything built on top keeps its pointers valid as memory is
// streamed in and out; "fragmentation" collapses to OS page granularity.
//
// Shape = BUMP ARENA, by design (ADR-0085 D1, user-confirmed 2026-05-27):
//   - allocate(): O(1) align + commit-on-demand + bump.
//   - deallocate(): NO-OP. A bump arena cannot free one allocation in the
//     middle. Fine-grained per-object free is delegated to a TlsfAllocator
//     parented on top (ADR-0085 S3). This arena is the wholesale PAGE SOURCE.
//   - reset()/reset_to(): free in bulk (logical; keeps pages committed for reuse).
//   - purge()/reset_and_purge(): hand physical pages back to the OS (RSS drops).
//
// Decommit policy = MANUAL (ADR-0085 D-mem-stream, user-confirmed): reset() is
// cheap and logical; physical decommit happens only on purge()/release(). Timed
// decay-based auto-purge layers in at S5 (StreamingAllocator), where budget and
// pressure context exist — keeping this allocator deterministic and syscall-free
// on the hot path.
//
// NOT thread-safe (IAllocator convention + ADR-0085 D3): one arena per
// thread/subsystem. RingAllocator (S4) and GpuAllocator (S6) are the cluster's
// thread-safe members; the VM arena is not, by design (branch-free hot path).
//
// Under AddressSanitizer the committed-but-unhanded-out tail is poisoned, so a
// read past the live top trips ASan even though the page is legitimately
// committed (win-asan is a DoD-required config).
class VirtualMemoryAllocator final : public IAllocator
{
public:
    // Opaque bump offset captured by mark(); pass back to reset_to().
    using Marker = usize;

    struct Config
    {
        // Address space reserved up front (default 64 GiB). Free on 64-bit; only
        // committed pages cost RAM. The arena NEVER re-reserves (that would break
        // stable addresses) — size this for the worst case; exhaustion is fatal.
        usize reserve_bytes = usize{64} << 30;
        // Commit granularity: a bump that crosses the committed high-water mark
        // commits the next multiple of this (default 64 KiB = a typical Windows
        // allocation granularity). Amortizes the commit syscall. Rounded up to a
        // multiple of the OS page size, floored at one page.
        usize commit_block = usize{64} << 10;
        // Pre-commit [0, initial_commit_bytes) at construction (rounded to
        // commit_block). 0 = fully lazy. Lets a caller pre-warm a known minimum
        // without committing the whole (huge) reservation.
        usize initial_commit_bytes = 0;
    };

    // Default 64 GiB reservation (Config{}). Kept separate from the Config ctor so
    // the brace default isn't formed inside the class body — clang rejects
    // evaluating Config's default member initializers there (forms Config{} in the
    // delegating ctor's member-init context instead).
    VirtualMemoryAllocator();
    explicit VirtualMemoryAllocator(const Config& cfg, const char* name = "VirtualMemoryAllocator");
    ~VirtualMemoryAllocator() override; // releases the reservation

    VirtualMemoryAllocator(const VirtualMemoryAllocator&)            = delete;
    VirtualMemoryAllocator& operator=(const VirtualMemoryAllocator&) = delete;

    // ---- IAllocator ---------------------------------------------------------
    // Bump + commit-on-demand. Fatal on reservation exhaustion or commit failure
    // (honors the IAllocator OOM contract); use try_allocate() for the graceful
    // streaming/pressure path. Any power-of-two alignment works (the reservation
    // base is allocation-granularity aligned, typically 64 KiB); alignments larger
    // than that simply waste up to (alignment - 1) bytes of bump padding.
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    // NO-OP (bump arena). Asserts owns(p) in debug to catch cross-allocator frees.
    void  deallocate(void* p) noexcept override;
    [[nodiscard]] bool owns(const void* p) const noexcept override;
    // In-place grow iff p is the most-recent allocation (the arena top); else
    // bump-copy. Lets a top-of-arena container grow without a copy.
    void* reallocate(void* p, usize old_size, usize new_size, usize alignment = kDefaultAlignment) override;
    // Bump arenas don't track per-allocation size — use used_bytes() for the arena.
    [[nodiscard]] usize allocation_size(const void* p) const noexcept override;

    // ---- Non-throwing path --------------------------------------------------
    // Returns nullptr on size==0, reservation exhaustion, or commit failure
    // (commit charge / physical RAM) — the path S5's residency logic retries
    // after evicting. Same semantics as GrowableTlsfAllocator::try_allocate.
    [[nodiscard]] void* try_allocate(usize size, usize alignment = kDefaultAlignment) override;

    // ---- Bulk lifetime ------------------------------------------------------
    [[nodiscard]] Marker mark() const noexcept { return m_alloc_pos; } // capture top (scratch pattern)
    void reset_to(Marker m) noexcept;       // pop back to a marker (keeps pages committed)
    void reset() noexcept;                  // pop everything (keeps pages committed for reuse)
    void purge() noexcept;                  // decommit the committed tail above the live top -> RSS drops
    void reset_and_purge() noexcept;        // reset() + purge() — hand RAM back to the OS

    // ---- Diagnostics --------------------------------------------------------
    [[nodiscard]] usize used_bytes() const noexcept { return m_alloc_pos; }
    [[nodiscard]] usize committed_bytes() const noexcept { return m_commit_pos; }
    [[nodiscard]] usize reserved_bytes() const noexcept { return m_reserved; }
    [[nodiscard]] const void* base() const noexcept { return m_base; }

private:
    // Core bump used by both allocate() (fatal on fail) and try_allocate (nullptr).
    void* bump(usize size, usize alignment) noexcept;
    // Ensure [0, target) is committed (target page/commit-block rounded). False on fail.
    [[nodiscard]] bool ensure_committed(usize target) noexcept;

    vm::VmRegion m_region{};               // the single reservation
    u8*          m_base          = nullptr; // == m_region.base
    usize        m_reserved      = 0;       // == m_region.size
    usize        m_page          = 0;       // vm::page_size()
    usize        m_commit_block  = 0;       // commit granularity (>= m_page)
    usize        m_commit_pos    = 0;       // committed high-water mark (page-aligned)
    usize        m_alloc_pos     = 0;       // current bump offset (<= m_commit_pos)
    usize        m_last_offset   = 0;       // offset of the most-recent allocation (for reallocate)
    bool         m_has_last      = false;   // whether m_last_offset is meaningful
};
} // namespace crd::memory
