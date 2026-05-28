// smoke_virtual_memory.cpp — ADR-0085 S1.
//
// Demonstrates + self-checks the crd::platform::vm open-world pattern: reserve a
// huge address range once, then stream "scene tiles" in (commit) and out
// (decommit) at STABLE addresses, proving sparse residency + zero-on-recommit.
// Headless; exit 0 on success, non-zero on any failed invariant.

#include <crd/vm/virtual_memory.hpp>

#include <cstdio>

namespace vm = crd::vm;

namespace
{
bool g_ok = true;
void check(bool cond, const char* what)
{
    if (!cond)
    {
        std::printf("  FAIL: %s\n", what);
        g_ok = false;
    }
}
} // namespace

int main()
{
    const crd::usize page = vm::page_size();
    std::printf("smoke_virtual_memory: page=%zu  alloc_granularity=%zu  large_page=%zu\n", page,
                vm::allocation_granularity(), vm::large_page_size());

    // Reserve a 16 GiB world arena (address space only — no physical cost yet).
    const crd::usize world_bytes = crd::usize{16} << 30;
    vm::VmRegion     world       = vm::reserve(world_bytes);
    check(world.valid(), "reserve 16 GiB world arena");
    if (!world.valid())
    {
        std::printf("smoke_virtual_memory: FAILED (could not reserve)\n");
        return 1;
    }
    std::printf("  reserved %.0f GiB at %p (no physical backing)\n",
                static_cast<double>(world.size) / static_cast<double>(crd::usize{1} << 30), world.base);

    // Stream in 8 tiles of 4 MiB each, scattered across the arena (simulating a
    // player moving through an open world touching distant regions).
    const crd::usize tile_bytes = crd::usize{4} << 20; // 4 MiB
    const crd::usize stride     = crd::usize{1} << 31;  // 2 GiB apart → far-flung tiles
    const int        num_tiles  = 8;

    for (int t = 0; t < num_tiles; ++t)
    {
        unsigned char* p = static_cast<unsigned char*>(world.base) + static_cast<crd::usize>(t) * stride;
        check(vm::commit(p, tile_bytes), "commit tile");
        // First touch reads as zero, then we fill it with the tile's signature.
        check(p[0] == 0 && p[tile_bytes - 1] == 0, "tile zero-on-commit");
        for (crd::usize i = 0; i < tile_bytes; i += page) { p[i] = static_cast<unsigned char>(0x40 + t); }
    }
    std::printf("  committed %d x 4 MiB tiles (%.0f MiB resident of 16 GiB reserved)\n", num_tiles,
                static_cast<double>(static_cast<crd::usize>(num_tiles) * tile_bytes) / static_cast<double>(1 << 20));

    // Verify every resident tile holds its own signature (addresses are stable).
    for (int t = 0; t < num_tiles; ++t)
    {
        unsigned char* p = static_cast<unsigned char*>(world.base) + static_cast<crd::usize>(t) * stride;
        check(p[0] == static_cast<unsigned char>(0x40 + t), "tile signature intact");
    }

    // Evict (decommit) the even tiles, recommit them → must read back zero, and
    // the odd tiles must be untouched at the SAME addresses.
    for (int t = 0; t < num_tiles; t += 2)
    {
        unsigned char* p = static_cast<unsigned char*>(world.base) + static_cast<crd::usize>(t) * stride;
        check(vm::decommit(p, tile_bytes), "decommit even tile");
    }
    for (int t = 0; t < num_tiles; t += 2)
    {
        unsigned char* p = static_cast<unsigned char*>(world.base) + static_cast<crd::usize>(t) * stride;
        check(vm::commit(p, tile_bytes), "recommit even tile");
        check(p[0] == 0, "recommitted tile zeroed");
    }
    for (int t = 1; t < num_tiles; t += 2)
    {
        unsigned char* p = static_cast<unsigned char*>(world.base) + static_cast<crd::usize>(t) * stride;
        check(p[0] == static_cast<unsigned char>(0x40 + t), "odd tile survived eviction at stable address");
    }

    vm::release(world);
    std::printf("smoke_virtual_memory: %s\n", g_ok ? "all checks passed" : "FAILED");
    return g_ok ? 0 : 1;
}
