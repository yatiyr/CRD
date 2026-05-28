// test_virtual_memory.cpp — crd::vm (ADR-0085 S1; relocated from crd-platform to
// the crd-vm leaf when S2 landed — see ADR-0085 §1 amendment 2026-05-27).
//
// Rock-solid cross-platform coverage (Windows VirtualAlloc + POSIX mmap): page
// granularity invariants, reserve/release, commit/decommit residency, the
// zero-on-(re)commit guarantee, STABLE ADDRESSES across decommit/recommit,
// sparse commit within a huge reservation (the open-world core), protection
// transitions, page-rounding of sub-page requests, and reserve/release churn
// (ASan-clean). Tests never deliberately fault a protected page (that would
// abort); protection is validated by allowed accesses + API success.

#include <crd/core/types.hpp>
#include <crd/vm/virtual_memory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace
{
namespace vm = crd::vm;

[[nodiscard]] bool is_pow2(crd::usize v) noexcept
{
    return v != 0 && (v & (v - 1)) == 0;
}
[[nodiscard]] bool is_aligned(const void* p, crd::usize a) noexcept
{
    return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0;
}
[[nodiscard]] unsigned char* bytes_at(void* base, crd::usize offset) noexcept
{
    return static_cast<unsigned char*>(base) + offset;
}
} // namespace

TEST_CASE("vm granularity queries are sane", "[vm]")
{
    const crd::usize page = vm::page_size();
    const crd::usize gran = vm::allocation_granularity();
    const crd::usize lp   = vm::large_page_size();

    REQUIRE(is_pow2(page));
    REQUIRE(page >= 4096); // every supported platform is >= 4 KiB
    REQUIRE(is_pow2(gran));
    REQUIRE(gran >= page); // Windows allocation granularity (64K) >= page; POSIX equal
    // Large pages: either unavailable (0) or a power-of-two multiple of the page.
    REQUIRE((lp == 0 || (is_pow2(lp) && lp >= page)));
}

TEST_CASE("vm reserve rounds up, aligns, and release invalidates", "[vm]")
{
    const crd::usize page = vm::page_size();
    const crd::usize gran = vm::allocation_granularity();

    vm::VmRegion r = vm::reserve(1); // smallest non-zero request
    REQUIRE(r.valid());
    REQUIRE(r.base != nullptr);
    REQUIRE(is_aligned(r.base, gran)); // reservation base is allocation-granularity aligned
    REQUIRE(r.size == page);           // 1 byte rounds up to one page
    vm::release(r);
    REQUIRE_FALSE(r.valid());
    REQUIRE(r.base == nullptr);

    vm::VmRegion r2 = vm::reserve(page * 3 + 1);
    REQUIRE(r2.valid());
    REQUIRE(r2.size == page * 4); // rounds up to the next page
    vm::release(r2);
}

TEST_CASE("vm reserve(0) is invalid and release of empty is safe", "[vm]")
{
    vm::VmRegion r = vm::reserve(0);
    REQUIRE_FALSE(r.valid());
    vm::release(r); // no-op, must not crash
    vm::VmRegion empty{};
    vm::release(empty); // no-op
    REQUIRE_FALSE(empty.valid());
}

TEST_CASE("vm reserves a huge address range (open-world premise, 64-bit)", "[vm]")
{
    // Reserving 8 GiB of ADDRESS SPACE (no physical commit) must succeed on 64-bit.
    const crd::usize huge = crd::usize{8} << 30;
    vm::VmRegion     r    = vm::reserve(huge);
    REQUIRE(r.valid());
    REQUIRE(r.size >= huge);
    vm::release(r);
}

TEST_CASE("vm commit gives zeroed, writable pages; readback matches", "[vm]")
{
    const crd::usize page = vm::page_size();
    vm::VmRegion     r    = vm::reserve(page * 16);
    REQUIRE(r.valid());

    REQUIRE(vm::commit(r.base, page * 4));
    unsigned char* p = bytes_at(r.base, 0);

    // Zero-on-commit guarantee.
    for (crd::usize i = 0; i < page * 4; ++i) { REQUIRE(p[i] == 0); }

    // Write a pattern across the committed span, read it back (ASan: in-bounds).
    for (crd::usize i = 0; i < page * 4; ++i) { p[i] = static_cast<unsigned char>((i * 31U + 7U) & 0xFF); }
    for (crd::usize i = 0; i < page * 4; ++i) { REQUIRE(p[i] == static_cast<unsigned char>((i * 31U + 7U) & 0xFF)); }

    vm::release(r);
}

TEST_CASE("vm decommit then recommit keeps the address and zeroes the memory", "[vm]")
{
    const crd::usize page = vm::page_size();
    vm::VmRegion     r    = vm::reserve(page * 8);
    REQUIRE(r.valid());
    void* const original_base = r.base;

    REQUIRE(vm::commit(r.base, page * 8));
    unsigned char* p = bytes_at(r.base, 0);
    for (crd::usize i = 0; i < page * 8; ++i) { p[i] = 0xAB; }

    REQUIRE(vm::decommit(r.base, page * 8));
    // Address space is still reserved at the SAME base after decommit.
    REQUIRE(r.base == original_base);

    REQUIRE(vm::commit(r.base, page * 8)); // recommit same range
    p = bytes_at(r.base, 0);
    for (crd::usize i = 0; i < page * 8; ++i) { REQUIRE(p[i] == 0); } // zero-filled again

    vm::release(r);
}

TEST_CASE("vm sparse commit within a huge reservation (the streaming core)", "[vm]")
{
    const crd::usize page = vm::page_size();
    // Reserve 256 MiB of address space; commit only three disjoint single-page windows.
    const crd::usize span = crd::usize{256} << 20;
    vm::VmRegion     r    = vm::reserve(span);
    REQUIRE(r.valid());

    const crd::usize off0 = 0;
    const crd::usize off1 = page * 1000;
    const crd::usize off2 = (span / page - 4) * page; // near the far end, page-aligned, in-range

    REQUIRE(vm::commit(bytes_at(r.base, off0), page));
    REQUIRE(vm::commit(bytes_at(r.base, off1), page));
    REQUIRE(vm::commit(bytes_at(r.base, off2), page));

    bytes_at(r.base, off0)[0]        = 0x10;
    bytes_at(r.base, off1)[page - 1] = 0x20; // last byte of its page
    bytes_at(r.base, off2)[123]      = 0x30;

    REQUIRE(bytes_at(r.base, off0)[0] == 0x10);
    REQUIRE(bytes_at(r.base, off1)[page - 1] == 0x20);
    REQUIRE(bytes_at(r.base, off2)[123] == 0x30);

    // Decommit the middle window, recommit → zero; the others stay intact.
    REQUIRE(vm::decommit(bytes_at(r.base, off1), page));
    REQUIRE(vm::commit(bytes_at(r.base, off1), page));
    REQUIRE(bytes_at(r.base, off1)[page - 1] == 0);
    REQUIRE(bytes_at(r.base, off0)[0] == 0x10); // untouched window survives
    REQUIRE(bytes_at(r.base, off2)[123] == 0x30);

    vm::release(r);
}

TEST_CASE("vm protect transitions on a committed range (no fault paths)", "[vm]")
{
    const crd::usize page = vm::page_size();
    vm::VmRegion     r    = vm::reserve(page * 2);
    REQUIRE(r.valid());
    REQUIRE(vm::commit(r.base, page * 2));

    unsigned char* p = bytes_at(r.base, 0);
    p[0]             = 0x7E;

    REQUIRE(vm::protect(r.base, page * 2, vm::Access::ReadOnly));
    REQUIRE(p[0] == 0x7E); // reads still allowed under ReadOnly

    REQUIRE(vm::protect(r.base, page * 2, vm::Access::ReadWrite));
    p[0] = 0x5F; // writable again
    REQUIRE(p[0] == 0x5F);

    REQUIRE(vm::protect(r.base, page * 2, vm::Access::None)); // API succeeds; no access attempted
    vm::release(r);
}

TEST_CASE("vm commit rounds sub-page requests up to a full page", "[vm]")
{
    const crd::usize page = vm::page_size();
    vm::VmRegion     r    = vm::reserve(page * 2);
    REQUIRE(r.valid());

    // Commit a 1-byte request at base+1: the whole containing page must become usable.
    REQUIRE(vm::commit(bytes_at(r.base, 1), 1));
    unsigned char* p = bytes_at(r.base, 0);
    for (crd::usize i = 0; i < page; ++i) { p[i] = static_cast<unsigned char>(i & 0xFF); }
    for (crd::usize i = 0; i < page; ++i) { REQUIRE(p[i] == static_cast<unsigned char>(i & 0xFF)); }

    vm::release(r);
}

TEST_CASE("vm reserve/release churn does not leak (ASan)", "[vm]")
{
    const crd::usize page = vm::page_size();
    for (int i = 0; i < 64; ++i)
    {
        vm::VmRegion r = vm::reserve(page * 32);
        REQUIRE(r.valid());
        REQUIRE(vm::commit(r.base, page * 4));
        bytes_at(r.base, 0)[0] = static_cast<unsigned char>(i);
        vm::release(r);
    }
}

TEST_CASE("vm bad inputs are rejected without crashing", "[vm]")
{
    REQUIRE_FALSE(vm::commit(nullptr, 4096));
    REQUIRE_FALSE(vm::decommit(nullptr, 4096));
    REQUIRE_FALSE(vm::protect(nullptr, 4096, vm::Access::ReadWrite));
    vm::VmRegion r = vm::reserve(vm::page_size());
    REQUIRE(r.valid());
    REQUIRE_FALSE(vm::commit(r.base, 0)); // zero length
    vm::release(r);
}
