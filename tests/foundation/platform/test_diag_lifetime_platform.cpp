// DIAG.10c -- library handles and file-watch callbacks in the lifetime ledger.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): a DynamicLibrary that opened is one
// LibraryHandle until its destructor frees it, a move transfers it without counting it twice, and a failed open
// records nothing; each FileWatcher entry is one CallbackRegistration from add() until remove(), a move-assignment
// over a watcher that still holds entries, or the watcher's destruction. The OS refcounts libraries and exposes no
// per-process count, so the library cases are judged by the ledger alone; the file and range oracles are in the core,
// log and vm tests.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/file_watcher.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace
{
using crd::LifetimeClass;
using crd::LifetimeVerdict;
namespace pfs = crd::platform::fs;

crd::LifetimeReport since(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot());
}

crd::platform::DynamicLibrary open_system_library()
{
#if CRD_OS_WINDOWS
    return crd::platform::DynamicLibrary::open(pfs::Path("kernel32.dll"));
#else
    return crd::platform::DynamicLibrary::open(pfs::Path("libm.so.6"));
#endif
}
} // namespace

TEST_CASE("diag10c: an opened library is one library handle until it is freed", "[platform][lifetime][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        crd::platform::DynamicLibrary a = open_system_library();
        REQUIRE(a.is_valid());
        crd::platform::DynamicLibrary b = open_system_library(); // a second reference is a second handle
        REQUIRE(b.is_valid());
        crd::LifetimeReport r = since(start);
        CHECK(r.verdict == LifetimeVerdict::Outstanding);
        CHECK(r.outstanding == crd::lifetime_class_bit(LifetimeClass::LibraryHandle));
        CHECK(r.live_change(LifetimeClass::LibraryHandle) == 2);

        crd::platform::DynamicLibrary moved(std::move(a)); // ownership moves, the count does not
        CHECK(since(start).live_change(LifetimeClass::LibraryHandle) == 2);
        b = std::move(moved); // b's own handle is freed here
        CHECK(since(start).live_change(LifetimeClass::LibraryHandle) == 1);
    }
    const crd::LifetimeReport r = since(start);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(r.underflowed == 0U);
}

TEST_CASE("diag10c: a library that does not open records nothing", "[platform][lifetime][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        const crd::platform::DynamicLibrary missing =
            crd::platform::DynamicLibrary::open(pfs::Path("crd_no_such_library_diag10c.bin"), /*log_on_failure=*/false);
        CHECK_FALSE(missing.is_valid());
    }
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
}

TEST_CASE("diag10c: each watched path is one callback registration until it is removed or its watcher ends",
          "[platform][lifetime][diag10c]")
{
    const pfs::Path             path("crd_diag10c_never_written.txt"); // watching needs no file to exist
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        crd::platform::FileWatcher w;
        const crd::u64             first = w.add(path, [](const pfs::Path&) {});
        (void)w.add(path, [](const pfs::Path&) {});
        CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 2);

        w.remove(first);
        w.remove(first); // already gone: nothing more is released
        crd::LifetimeReport r = since(start);
        CHECK(r.live_change(LifetimeClass::CallbackRegistration) == 1);
        CHECK(r.underflowed == 0U);

        crd::platform::FileWatcher moved(std::move(w)); // the entry moves with the watcher
        CHECK(moved.count() == 1U);
        CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 1);

        crd::platform::FileWatcher other;
        (void)other.add(path, [](const pfs::Path&) {});
        (void)other.add(path, [](const pfs::Path&) {});
        CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 3);
        other = std::move(moved); // other's two entries end; moved's one is now other's
        CHECK(other.count() == 1U);
        CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 1);
    }
    const crd::LifetimeReport r = since(start);
    char                      line[160];
    (void)crd::format_lifetime_report(r, line, sizeof(line));
    INFO("window: " << line);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
}
