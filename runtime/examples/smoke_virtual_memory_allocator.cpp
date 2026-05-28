// smoke_virtual_memory_allocator.cpp — ADR-0085 S2.
//
// Self-checks + measures crd::memory::VirtualMemoryAllocator: reserve-huge /
// commit-on-demand bump, stable addresses across purge, and the perf contract
// from docs/research/cerid-streaming-allocators.md (hot allocate, mark/reset).
// Headless; exit 0 on success, non-zero on any failed invariant.

#include <crd/memory/allocators/virtual_memory_allocator.hpp>

#include <chrono>
#include <cstdio>

namespace
{
void check(bool& ok, bool cond, const char* what)
{
    if (!cond)
    {
        std::printf("  FAIL: %s\n", what);
        ok = false;
    }
}
} // namespace

int main()
{
    using crd::memory::VirtualMemoryAllocator;
    using clock = std::chrono::steady_clock;

    bool g_ok = true;

    VirtualMemoryAllocator::Config cfg;
    cfg.reserve_bytes        = crd::usize{8} << 30; // 8 GiB address space (no physical cost)
    cfg.commit_block         = crd::usize{64} << 10;
    cfg.initial_commit_bytes = crd::usize{16} << 20; // pre-warm 16 MiB so the hot path never commits

    VirtualMemoryAllocator a(cfg);
    std::printf("smoke_vma: reserved=%.0f GiB committed=%zu KiB base=%p\n",
                static_cast<double>(a.reserved_bytes()) / static_cast<double>(crd::usize{1} << 30),
                static_cast<crd::usize>(a.committed_bytes() >> 10), a.base());
    check(g_ok, a.committed_bytes() >= (crd::usize{16} << 20), "initial_commit pre-warm");
    check(g_ok, a.used_bytes() == 0, "nothing allocated yet");

    // --- Correctness: stable address survives a purge of higher pages ---
    auto* keep = static_cast<crd::u8*>(a.allocate(4096));
    keep[0]    = 0x42;
    const auto m = a.mark();
    a.allocate(crd::usize{64} << 20); // grow committed well past `keep`
    a.reset_to(m);
    a.purge();
    check(g_ok, keep[0] == 0x42, "low allocation intact after purge");

    // --- Perf: hot allocate (pre-warmed, no commit crossing) ---
    a.reset();
    constexpr int k_n = 200000;
    const auto     t0 = clock::now();
    crd::u8*       last = nullptr;
    for (int i = 0; i < k_n; ++i)
    {
        last = static_cast<crd::u8*>(a.allocate(64, 16));
    }
    const auto t1 = clock::now();
    check(g_ok, last != nullptr, "allocate returned a pointer");
    const double ns_alloc = std::chrono::duration<double, std::nano>(t1 - t0).count() / k_n;

    // --- Perf: mark / reset_to round-trip ---
    const auto t2 = clock::now();
    for (int i = 0; i < k_n; ++i)
    {
        const auto mk = a.mark();
        a.reset_to(mk);
    }
    const auto t3 = clock::now();
    const double ns_mark = std::chrono::duration<double, std::nano>(t3 - t2).count() / k_n;

    std::printf("smoke_vma: allocate ~%.1f ns/op  mark+reset ~%.1f ns/op  (contract: <=20 / <=5 ns)\n", ns_alloc,
                ns_mark);

    std::printf("smoke_virtual_memory_allocator: %s\n", g_ok ? "all checks passed" : "FAILED");
    return g_ok ? 0 : 1;
}
