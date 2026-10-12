// DIAG.10c -- lifetime accounting beyond heap bytes: the ledger's own arithmetic and the OS oracle it is judged by.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): the ledger counts, per class, what was acquired,
// released, is live, its high water and its live bytes; a release it cannot match changes nothing and is counted, so
// a window reports Underflowed rather than a wrapped or clamped count; a window is Balanced, Outstanding (count or
// bytes), Drained (released earlier acquisitions) or Underflowed, worst first. The OS probes answer from the process's
// own handle table / descriptors and address space, so a wired engine site is judged by code it shares nothing with.
// The arithmetic cases use a ledger of their own; the engine's process-wide ledger is exercised by the wired modules'
// tests (log, platform, vm, jobs, app, resources, gpu-context).

#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/os_resource_probe.hpp>
#include <crd/core/platform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{
namespace fs = std::filesystem;
using crd::LifetimeClass;
using crd::LifetimeLedger;
using crd::LifetimeVerdict;
using crd::OsPresence;

std::atomic<unsigned> g_unique{0U};

std::FILE* open_file(const char* path, const char* mode)
{
#if defined(_MSC_VER)
    std::FILE* f = nullptr;
    return fopen_s(&f, path, mode) == 0 ? f : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

// A fresh file under the temp directory, created empty; removed by the destructor.
struct TempFile
{
    fs::path path;
    char     text[1024]{};

    explicit TempFile(const char* stem)
    {
        char leaf[96];
        (void)std::snprintf(leaf, sizeof(leaf), "crd_lifetime_%s_%u_%u.bin", stem,
                            static_cast<unsigned>(process_id()), g_unique.fetch_add(1U));
        path = fs::temp_directory_path() / leaf;
        (void)std::snprintf(text, sizeof(text), "%s", path.string().c_str());
        std::FILE* f = open_file(text, "wb");
        REQUIRE(f != nullptr);
        (void)std::fputs("lifetime", f);
        (void)std::fclose(f);
    }
    ~TempFile()
    {
        std::error_code ec;
        fs::remove(path, ec);
    }
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;

    static unsigned long process_id()
    {
#if CRD_OS_WINDOWS
        return static_cast<unsigned long>(::GetCurrentProcessId());
#else
        return static_cast<unsigned long>(::getpid());
#endif
    }
};

crd::usize page_bytes()
{
#if CRD_OS_WINDOWS
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    return static_cast<crd::usize>(si.dwAllocationGranularity);
#else
    return static_cast<crd::usize>(::sysconf(_SC_PAGESIZE));
#endif
}

void* os_reserve(void* hint, crd::usize bytes)
{
#if CRD_OS_WINDOWS
    return ::VirtualAlloc(hint, bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = ::mmap(hint, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void os_release(void* base, crd::usize bytes)
{
#if CRD_OS_WINDOWS
    (void)bytes;
    REQUIRE(::VirtualFree(base, 0, MEM_RELEASE) != 0);
#else
    REQUIRE(::munmap(base, bytes) == 0);
#endif
}

crd::LifetimeReport window(const crd::LifetimeSnapshot& before, const LifetimeLedger& ledger)
{
    return crd::lifetime_report(before, ledger.snapshot());
}
} // namespace

TEST_CASE("diag10c: the ledger counts each class, its high water and its bytes", "[core][lifetime][diag10c]")
{
    LifetimeLedger              ledger;
    const crd::LifetimeSnapshot start = ledger.snapshot();

    ledger.acquired(LifetimeClass::FileHandle);
    ledger.acquired(LifetimeClass::FileHandle);
    ledger.acquired(LifetimeClass::MappedRegion, 4096U);
    ledger.acquired(LifetimeClass::MappedRegion, 8192U);
    CHECK(ledger.released(LifetimeClass::FileHandle));

    const crd::LifetimeSnapshot mid = ledger.snapshot();
    CHECK(mid.of(LifetimeClass::FileHandle).acquired == 2U);
    CHECK(mid.of(LifetimeClass::FileHandle).released == 1U);
    CHECK(mid.of(LifetimeClass::FileHandle).live == 1U);
    CHECK(mid.of(LifetimeClass::FileHandle).high_water == 2U);
    CHECK(mid.of(LifetimeClass::MappedRegion).live == 2U);
    CHECK(mid.of(LifetimeClass::MappedRegion).live_bytes == 12288U);
    CHECK(mid.of(LifetimeClass::LibraryHandle).acquired == 0U);

    const crd::LifetimeReport held = crd::lifetime_report(start, mid);
    CHECK(held.verdict == LifetimeVerdict::Outstanding);
    CHECK(held.outstanding ==
          (crd::lifetime_class_bit(LifetimeClass::FileHandle) | crd::lifetime_class_bit(LifetimeClass::MappedRegion)));
    CHECK(held.drained == 0U);
    CHECK(held.underflowed == 0U);
    CHECK(held.live_change(LifetimeClass::FileHandle) == 1);
    CHECK(held.live_change(LifetimeClass::MappedRegion) == 2);
    CHECK(held.byte_change(LifetimeClass::MappedRegion) == 12288);

    CHECK(ledger.released(LifetimeClass::FileHandle));
    CHECK(ledger.released(LifetimeClass::MappedRegion, 4096U));
    CHECK(ledger.released(LifetimeClass::MappedRegion, 8192U));
    const crd::LifetimeReport after = window(start, ledger);
    CHECK(after.verdict == LifetimeVerdict::Balanced);
    CHECK(after.outstanding == 0U);
    CHECK(ledger.snapshot().of(LifetimeClass::FileHandle).high_water == 2U); // the high water is kept

    char line[128];
    CHECK(crd::format_lifetime_report(after, line, sizeof(line)) == 8U);
    CHECK(std::strcmp(line, "balanced") == 0);
}

TEST_CASE("diag10c: a release the ledger cannot match is counted, never wrapped or clamped",
          "[core][lifetime][diag10c]")
{
    LifetimeLedger              ledger;
    const crd::LifetimeSnapshot start = ledger.snapshot();

    // Nothing live: the release changes nothing but the unmatched count.
    CHECK_FALSE(ledger.released(LifetimeClass::LibraryHandle));
    crd::LifetimeSnapshot s = ledger.snapshot();
    CHECK(s.of(LifetimeClass::LibraryHandle).live == 0U);
    CHECK(s.of(LifetimeClass::LibraryHandle).released == 0U);
    CHECK(s.of(LifetimeClass::LibraryHandle).unmatched_releases == 1U);
    crd::LifetimeReport r = crd::lifetime_report(start, s);
    CHECK(r.verdict == LifetimeVerdict::Underflowed);
    CHECK(r.underflowed == crd::lifetime_class_bit(LifetimeClass::LibraryHandle));
    CHECK(r.live_change(LifetimeClass::LibraryHandle) == 0);

    char line[160];
    (void)crd::format_lifetime_report(r, line, sizeof(line));
    CHECK(std::strcmp(line, "underflowed: library-handle") == 0);

    // The count matches, the bytes do not: the count is released, the bytes stay, and the release is unmatched.
    const crd::LifetimeSnapshot second = ledger.snapshot();
    ledger.acquired(LifetimeClass::MappedRegion, 4096U);
    CHECK_FALSE(ledger.released(LifetimeClass::MappedRegion, 8192U));
    s = ledger.snapshot();
    CHECK(s.of(LifetimeClass::MappedRegion).live == 0U);
    CHECK(s.of(LifetimeClass::MappedRegion).live_bytes == 4096U);
    CHECK(s.of(LifetimeClass::MappedRegion).unmatched_releases == 1U);
    r = crd::lifetime_report(second, s);
    CHECK(r.verdict == LifetimeVerdict::Underflowed); // worse than the 4096 bytes it also leaves outstanding
    CHECK(r.underflowed == crd::lifetime_class_bit(LifetimeClass::MappedRegion));
    CHECK(r.outstanding == crd::lifetime_class_bit(LifetimeClass::MappedRegion));
    CHECK(r.byte_change(LifetimeClass::MappedRegion) == 4096);
}

TEST_CASE("diag10c: a window that releases earlier acquisitions is Drained, and outstanding outranks drained",
          "[core][lifetime][diag10c]")
{
    LifetimeLedger ledger;
    ledger.acquired(LifetimeClass::CallbackRegistration);
    ledger.acquired(LifetimeClass::DeviceObject);
    const crd::LifetimeSnapshot start = ledger.snapshot();

    CHECK(ledger.released(LifetimeClass::CallbackRegistration));
    crd::LifetimeReport r = window(start, ledger);
    CHECK(r.verdict == LifetimeVerdict::Drained);
    CHECK(r.drained == crd::lifetime_class_bit(LifetimeClass::CallbackRegistration));
    CHECK(r.live_change(LifetimeClass::CallbackRegistration) == -1);

    ledger.acquired(LifetimeClass::FileHandle);
    ledger.acquired(LifetimeClass::MappedRegion, 4096U);
    r = window(start, ledger);
    CHECK(r.verdict == LifetimeVerdict::Outstanding);
    CHECK(r.outstanding ==
          (crd::lifetime_class_bit(LifetimeClass::FileHandle) | crd::lifetime_class_bit(LifetimeClass::MappedRegion)));
    CHECK(r.drained == crd::lifetime_class_bit(LifetimeClass::CallbackRegistration));

    char line[160];
    const crd::usize n = crd::format_lifetime_report(r, line, sizeof(line));
    CHECK(std::strcmp(line,
                      "outstanding: file-handle +1, mapped-region +1 (+4096 bytes); drained: callback-registration -1") ==
          0);
    CHECK(n == std::strlen(line));

    // A short buffer is cut, never overrun, and still terminated.
    char small[9];
    std::memset(small, 'x', sizeof(small));
    CHECK(crd::format_lifetime_report(r, small, 8U) == 7U);
    CHECK(small[7] == '\0');
    CHECK(small[8] == 'x');
    CHECK(std::strcmp(small, "outstan") == 0);
    CHECK(crd::format_lifetime_report(r, small, 0U) == 0U);

    CHECK(std::strcmp(crd::lifetime_verdict_name(LifetimeVerdict::Underflowed), "underflowed") == 0);
    CHECK(std::strcmp(crd::lifetime_class_name(LifetimeClass::DeviceObject), "device-object") == 0);
}

TEST_CASE("diag10c: the OS sees a file this process holds open, and stops seeing it once it is closed",
          "[core][lifetime][diag10c]")
{
    const TempFile held("held");
    const TempFile other("other");

    crd::OsProbeResult r = crd::os_file_open_in_process(held.text);
    INFO("reason: " << r.reason);
    REQUIRE(r.presence == OsPresence::Absent); // created and closed: nothing holds it

    std::FILE* f = open_file(held.text, "ab");
    REQUIRE(f != nullptr);
    r = crd::os_file_open_in_process(held.text);
    CHECK(r.presence == OsPresence::Present);
    CHECK(crd::os_file_open_in_process(other.text).presence == OsPresence::Absent); // a handle to another file
    (void)std::fclose(f);
    CHECK(crd::os_file_open_in_process(held.text).presence == OsPresence::Absent);

    // A read-only handle is a handle too.
    f = open_file(held.text, "rb");
    REQUIRE(f != nullptr);
    CHECK(crd::os_file_open_in_process(held.text).presence == OsPresence::Present);
    (void)std::fclose(f);
    CHECK(crd::os_file_open_in_process(held.text).presence == OsPresence::Absent);

    // A path that cannot be identified is Unavailable, never Absent.
    const fs::path missing = held.path.parent_path() / "crd_lifetime_no_such_file.bin";
    r = crd::os_file_open_in_process(missing.string().c_str());
    CHECK(r.presence == OsPresence::Unavailable);
    CHECK(std::strlen(r.reason) > 0U);
    CHECK(crd::os_file_open_in_process(nullptr).presence == OsPresence::Unavailable);
    CHECK(std::strcmp(crd::os_presence_name(OsPresence::Partial), "partial") == 0);
}

TEST_CASE("diag10c: the OS sees a reserved range, part of one, and none of it once released",
          "[core][lifetime][diag10c]")
{
    const crd::usize unit = page_bytes();
    void* const      base = os_reserve(nullptr, 2U * unit);
    REQUIRE(base != nullptr);
    CHECK(crd::os_range_mapped(base, 2U * unit).presence == OsPresence::Present);
    CHECK(crd::os_range_mapped(base, unit).presence == OsPresence::Present);
    auto* const second = static_cast<crd::u8*>(base) + unit;

    // Only the first half stays reserved. Linux unmaps the second page of the mapping; Windows cannot release part
    // of a reservation, so it releases the whole one and reserves its first unit again at the same address.
#if CRD_OS_WINDOWS
    os_release(base, 2U * unit);
    CHECK(crd::os_range_mapped(base, 2U * unit).presence == OsPresence::Absent);
    REQUIRE(os_reserve(base, unit) == base); // an explicit address is reserved there or not at all
#else
    os_release(second, unit);
#endif
    CHECK(crd::os_range_mapped(base, 2U * unit).presence == OsPresence::Partial);
    CHECK(crd::os_range_mapped(second, unit).presence == OsPresence::Absent);
    CHECK(crd::os_range_mapped(base, unit).presence == OsPresence::Present);
    os_release(base, unit);
    CHECK(crd::os_range_mapped(base, 2U * unit).presence == OsPresence::Absent);

    CHECK(crd::os_range_mapped(nullptr, unit).presence == OsPresence::Unavailable);
    CHECK(crd::os_range_mapped(base, 0U).presence == OsPresence::Unavailable);
}

TEST_CASE("diag10c: native GPU objects and OS objects are classes of their own, and a hold records once each way",
          "[core][lifetime][diag10c]")
{
    CHECK(std::strcmp(crd::lifetime_class_name(LifetimeClass::NativeGpuObject), "native-gpu-object") == 0);
    CHECK(std::strcmp(crd::lifetime_class_name(LifetimeClass::OsObject), "os-object") == 0);
    CHECK(crd::kLifetimeClassCount == 8U);
    // Eight classes fill the u8 masks: every bit is distinct and the last is the top bit.
    crd::u32 seen = 0U;
    for (crd::usize i = 0U; i < crd::kLifetimeClassCount; ++i)
    {
        const crd::u8 bit = crd::lifetime_class_bit(static_cast<LifetimeClass>(i));
        CHECK((seen & bit) == 0U);
        seen |= bit;
    }
    CHECK(seen == 0xFFU);

    // The hold records into the process-wide ledger: judge it by a window around it.
    crd::LifetimeLedger&        ledger = crd::lifetime_ledger();
    const crd::LifetimeSnapshot start  = ledger.snapshot();
    const auto                  since  = [&]() { return crd::lifetime_report(start, ledger.snapshot()); };
    {
        crd::LifetimeHold hold(LifetimeClass::OsObject);
        CHECK_FALSE(hold.held());
        CHECK(since().verdict == LifetimeVerdict::Balanced); // nothing recorded until the thing is granted
        hold.hold();
        hold.hold(); // a second hold while held records nothing
        CHECK(hold.held());
        crd::LifetimeReport r = since();
        CHECK(r.verdict == LifetimeVerdict::Outstanding);
        CHECK(r.live_change(LifetimeClass::OsObject) == 1);
        CHECK(r.outstanding == crd::lifetime_class_bit(LifetimeClass::OsObject));

        // A move transfers the hold: still one live, and the moved-from side releases nothing.
        crd::LifetimeHold moved(std::move(hold));
        CHECK(moved.held());
        CHECK(since().live_change(LifetimeClass::OsObject) == 1);
        hold.reset();
        CHECK(since().live_change(LifetimeClass::OsObject) == 1);

        // Move-assigning over a held object releases what that one held first.
        crd::LifetimeHold other(LifetimeClass::NativeGpuObject);
        other.hold();
        CHECK(since().live_change(LifetimeClass::NativeGpuObject) == 1);
        other = std::move(moved);
        r = since();
        CHECK(r.live_change(LifetimeClass::NativeGpuObject) == 0);
        CHECK(r.live_change(LifetimeClass::OsObject) == 1);
        CHECK(other.held());
        CHECK_FALSE(moved.held());

        other.reset();
        other.reset(); // a second reset releases nothing
        CHECK(since().verdict == LifetimeVerdict::Balanced);
        other.hold(); // and destruction releases a live hold
    }
    const crd::LifetimeReport end = since();
    CHECK(end.verdict == LifetimeVerdict::Balanced);
    CHECK(end.underflowed == 0U);
}
