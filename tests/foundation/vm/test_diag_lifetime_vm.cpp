// DIAG.10c -- a crd::vm reservation is a mapped region in the lifetime ledger, and the address space agrees.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): vm::reserve records one MappedRegion of the
// page-rounded size once the OS granted it, vm::release records its release once the OS took it back, and nothing
// else (a failed reserve, the release of an empty region, commit and decommit) changes the ledger. The address
// space is asked independently (crd::os_range_mapped, and crd::os_range_no_access for the release: a released range
// is where the OS places the process's next mapping, which a sanitizer runtime can make at any instruction, so only
// the reservation's own no-access state shows that it is gone).

#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/os_resource_probe.hpp>
#include <crd/vm/virtual_memory.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace vm = crd::vm;
using crd::LifetimeClass;
using crd::LifetimeVerdict;
using crd::OsPresence;

crd::LifetimeReport since(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot());
}
} // namespace

TEST_CASE("diag10c: a reservation is one mapped region of its size until it is released", "[vm][lifetime][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    vm::VmRegion                region = vm::reserve(3U * vm::page_size() + 1U); // rounded up to four pages
    REQUIRE(region.valid());
    const void* const base = region.base;
    const crd::usize  size = region.size;
    CHECK(size == 4U * vm::page_size());

    crd::LifetimeReport r = since(start);
    CHECK(r.verdict == LifetimeVerdict::Outstanding);
    CHECK(r.outstanding == crd::lifetime_class_bit(LifetimeClass::MappedRegion));
    CHECK(r.live_change(LifetimeClass::MappedRegion) == 1);
    CHECK(r.byte_change(LifetimeClass::MappedRegion) == static_cast<crd::i64>(size));
    CHECK(crd::os_range_mapped(base, size).presence == OsPresence::Present);

    // Backing pages come and go inside the reservation; the reservation itself does not.
    REQUIRE(vm::commit(region.base, vm::page_size()));
    CHECK(crd::os_range_no_access(base, size).presence == OsPresence::Partial); // the committed page is accessible
    REQUIRE(vm::decommit(region.base, vm::page_size()));
    r = since(start);
    CHECK(r.live_change(LifetimeClass::MappedRegion) == 1);
    CHECK(r.byte_change(LifetimeClass::MappedRegion) == static_cast<crd::i64>(size));
    CHECK(crd::os_range_no_access(base, size).presence == OsPresence::Present);

    vm::release(region);
    CHECK_FALSE(region.valid());
    r = since(start);
    char line[160];
    (void)crd::format_lifetime_report(r, line, sizeof(line));
    INFO("window: " << line);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(crd::os_range_no_access(base, size).presence == OsPresence::Absent);
}

TEST_CASE("diag10c: what the OS never granted is never recorded", "[vm][lifetime][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    vm::VmRegion                empty = vm::reserve(0U);
    CHECK_FALSE(empty.valid());
    vm::release(empty); // an empty region: nothing to release and nothing recorded
    vm::VmRegion none{};
    vm::release(none);
    const crd::LifetimeReport r = since(start);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(r.underflowed == 0U);
}
