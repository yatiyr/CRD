#pragma once

#include <crd/core/types.hpp>

namespace crd::vm
{
// Virtual-memory reservation / commit primitives (ADR-0085 S1).
//
// The foundation for open-world streaming: reserve a huge contiguous address
// range up front (free on 64-bit — only address space, no physical backing),
// then commit/decommit physical pages on demand. Addresses are STABLE across
// commit/decommit, so streaming chunks in and out never invalidates a pointer;
// "fragmentation" collapses to OS page granularity.
//
// This is the lowest-level OS-syscall substrate (crd-vm leaf module): both
// crd-memory (VirtualMemoryAllocator, ADR-0085 S2) and crd-platform may build on
// it without a dependency cycle. It was relocated here from crd-platform when S2
// landed — crd-platform PUBLICly links crd-memory, so the allocator's page source
// had to live below crd-memory (ADR-0085 §1 amendment, 2026-05-27).
//
// Backends: Windows = VirtualAlloc/VirtualFree/VirtualProtect;
// POSIX = mmap(PROT_NONE) + mprotect (commit) + madvise(MADV_DONTNEED) +
// munmap. No vendor types leak through this header.
//
// THREAD-SAFETY: every function here is a stateless OS call (reentrant); safe to
// call from any thread. `page_size()`/`large_page_size()` cache via a
// thread-safe one-time init. The higher-level allocators (ADR-0085 S2+) add
// their own bookkeeping + thread-safety contracts on top.
//
// DETERMINISM (ADR-0063): reserved addresses are process-private and never part
// of simulation/replay state. Commit zero-fills, so committed-then-read bytes
// are deterministic (all-zero until written).

// Page access for protect(). Kept minimal + safe (no execute by default).
enum class Access : crd::u8
{
    None,      // PROT_NONE / PAGE_NOACCESS — any access faults
    ReadOnly,  // PROT_READ / PAGE_READONLY
    ReadWrite, // PROT_READ|PROT_WRITE / PAGE_READWRITE
};

// A reserved virtual address range. POD handle (no RAII — the owning allocator
// manages the lifecycle and calls release()); `base`/`size` are page-rounded.
struct VmRegion
{
    void*      base = nullptr; // reserved base address (allocation-granularity aligned)
    crd::usize size = 0;       // reserved bytes (rounded up to page granularity)

    [[nodiscard]] bool valid() const noexcept { return base != nullptr && size != 0; }
    explicit           operator bool() const noexcept { return valid(); }
};

// ---- Granularity queries (cached) -------------------------------------------
// Commit/protect granularity (typically 4 KiB). Always a power of two, > 0.
[[nodiscard]] crd::usize page_size() noexcept;

// Reservation base alignment (Windows allocation granularity, typically 64 KiB;
// equals page_size() on POSIX). Always a power of two, >= page_size().
[[nodiscard]] crd::usize allocation_granularity() noexcept;

// Large/huge page size (2 MiB typical), or 0 if large pages are unavailable /
// not configured for this process. Informational for S2+; never required.
[[nodiscard]] crd::usize large_page_size() noexcept;

// ---- Reservation lifecycle --------------------------------------------------
// Reserve `bytes` of contiguous address space with NO physical backing and NO
// access (PROT_NONE / PAGE_NOACCESS). `bytes` is rounded up to page_size().
// Returns an INVALID region on failure — reserving very large ranges can
// legitimately fail, so this never fatals; the caller decides.
[[nodiscard]] VmRegion reserve(crd::usize bytes) noexcept;

// Reserve at a specific hint address (nullptr = let the OS choose). The hint is
// advisory; the returned base may differ. For placement / testing.
[[nodiscard]] VmRegion reserve_at(void* hint, crd::usize bytes) noexcept;

// Release the entire reservation (decommits everything + frees the address
// space) and zero the handle. Safe on an invalid/empty region.
void release(VmRegion& region) noexcept;

// ---- Commit / decommit (physical backing within a reservation) --------------
// Commit [ptr, ptr+bytes) as ReadWrite. The range must lie within a prior
// reservation. `ptr` is rounded DOWN and `ptr+bytes` UP to page_size(). Newly
// committed pages read back as zero. Returns false on failure (e.g. out of
// physical memory / commit charge).
[[nodiscard]] bool commit(void* ptr, crd::usize bytes) noexcept;

// Decommit [ptr, ptr+bytes): drop the physical pages but keep the address
// reserved. After decommit the range is inaccessible; a later commit() of the
// same range reads back as zero. `ptr`/`bytes` page-rounded as in commit().
[[nodiscard]] bool decommit(void* ptr, crd::usize bytes) noexcept;

// ---- Protection -------------------------------------------------------------
// Change protection of a COMMITTED range. `ptr`/`bytes` page-rounded.
[[nodiscard]] bool protect(void* ptr, crd::usize bytes, Access access) noexcept;
} // namespace crd::vm
