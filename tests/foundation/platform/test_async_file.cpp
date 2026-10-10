#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <crd/platform/async_file.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace crd::platform;

static crd::memory::GrowableTlsfAllocator s_alloc;
static std::atomic<crd::u32>        s_file_counter{0};

// Initialise / shut down the job system around the full test run.
// Using a Catch2 listener avoids calling jobs::init() at static-init time
// (which would fire during catch_discover_tests' test-listing phase).
struct AsyncFileJobsListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override
    {
        crd::jobs::init(crd::jobs::Config{.num_threads = 2});
    }

    void testRunEnded(Catch::TestRunStats const&) override
    {
        crd::jobs::shutdown();
    }
};
CATCH_REGISTER_LISTENER(AsyncFileJobsListener)

// Write known bytes to a uniquely-named temp file; returns the path.
static fs::Path write_temp_file(const crd::u8* data, crd::usize size)
{
    const crd::u32 idx = s_file_counter.fetch_add(1U, std::memory_order_relaxed);
    char name[64];
    std::snprintf(name, sizeof(name), "test_async_%u.bin", idx);
    const fs::Path p(crd::containers::StringView(name, std::strlen(name)));
    crd::containers::Array<crd::u8> bytes(&s_alloc);
    bytes.resize(size);
    std::memcpy(bytes.data(), data, size);
    REQUIRE(fs::write_file_binary(p, crd::containers::as_const_span(bytes)));
    return p;
}

TEST_CASE("AsyncFile: open nonexistent path returns invalid handle", "[platform][async_file]")
{
    const AsyncFile f = AsyncFile::open("__nonexistent_file_xyz_123.bin");
    CHECK(!f.is_open());
    CHECK(f.size() == 0U);
}

TEST_CASE("AsyncFile: open existing file reports correct size", "[platform][async_file]")
{
    const crd::u8 data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    const fs::Path p = write_temp_file(data, sizeof(data));

    const AsyncFile f = AsyncFile::open(p.generic());
    CHECK(f.is_open());
    CHECK(f.size() == sizeof(data));

    (void)fs::remove_file(p);
}

TEST_CASE("AsyncFile: read_async round-trip matches written bytes", "[platform][async_file]")
{
    const crd::u8 data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const fs::Path p = write_temp_file(data, sizeof(data));

    AsyncFile f = AsyncFile::open(p.generic());
    REQUIRE(f.is_open());

    crd::u8 dst[sizeof(data)] = {};
    AsyncReadStatus status = AsyncReadStatus::Failed;
    crd::jobs::Counter* c = f.read_async(0U, crd::containers::Span<crd::u8>(dst, sizeof(dst)), status);
    REQUIRE(c != nullptr);
    crd::jobs::wait(c);
    CHECK(status == AsyncReadStatus::Complete);

    for (crd::usize i = 0; i < sizeof(data); ++i)
    {
        CHECK(dst[i] == data[i]);
    }

    (void)fs::remove_file(p);
}

TEST_CASE("AsyncFile: out-of-range read returns nullptr", "[platform][async_file]")
{
    const crd::u8 data[] = {0x11, 0x22, 0x33};
    const fs::Path p = write_temp_file(data, sizeof(data));

    AsyncFile f = AsyncFile::open(p.generic());
    REQUIRE(f.is_open());

    // offset + size > file size → nullptr counter
    crd::u8 dst[8] = {};
    AsyncReadStatus status = AsyncReadStatus::Pending;
    crd::jobs::Counter* c = f.read_async(0U, crd::containers::Span<crd::u8>(dst, sizeof(dst)), status);
    CHECK(c == nullptr);
    CHECK(status == AsyncReadStatus::Failed);

    (void)fs::remove_file(p);
}

// ── DIAG.9b: the I/O fault seam at the platform's reads ──────────────────────
//
// A test-local hook answers every consult with one scripted fault, so these cases check what each read does with
// each action: an AsyncFile completion reports it through its status (never as silently zeroed bytes), and the
// synchronous reads fail as a real failed or short read does. crd-perf's seeded FaultInjector drives the same seam
// end to end in crd-perf-tests and crd-resources-tests. A shipping build has no seam: installing is refused and every
// read is plain.

namespace
{
struct ScriptedFault
{
    crd::IoFault          fault;
    std::atomic<crd::u32> calls{0U};
    std::atomic<crd::u32> last_op{99U};
};

crd::IoFault scripted_hook(void* user, crd::IoFaultOp op, const char* /*path*/, crd::usize /*path_bytes*/,
                           crd::u64 /*offset*/, crd::u64 /*size*/) noexcept
{
    auto* const s = static_cast<ScriptedFault*>(user);
    s->calls.fetch_add(1U, std::memory_order_relaxed);
    s->last_op.store(static_cast<crd::u32>(op), std::memory_order_relaxed);
    return s->fault;
}

// Installs the scripted hook for one scope (a seam left installed would fault every later read in the binary).
struct ScopedHook
{
    bool installed = false;

    explicit ScopedHook(ScriptedFault& s) : installed(crd::install_io_fault_hook(&scripted_hook, &s)) {}
    ~ScopedHook()
    {
        if (installed)
        {
            crd::uninstall_io_fault_hook();
        }
    }
    ScopedHook(const ScopedHook&)            = delete;
    ScopedHook& operator=(const ScopedHook&) = delete;
    ScopedHook(ScopedHook&&)                 = delete;
    ScopedHook& operator=(ScopedHook&&)      = delete;
};

AsyncReadStatus read_all(AsyncFile& f, crd::u8* dst, crd::usize size)
{
    AsyncReadStatus     status = AsyncReadStatus::Failed;
    crd::jobs::Counter* c      = f.read_async(0U, crd::containers::Span<crd::u8>(dst, size), status);
    REQUIRE(c != nullptr);
    crd::jobs::wait(c);
    return status;
}
} // namespace

TEST_CASE("AsyncFile: each injected completion fault is reported through the read status",
          "[platform][async_file][diag9b]")
{
    const crd::u8  data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const fs::Path p      = write_temp_file(data, sizeof(data));
    AsyncFile      f      = AsyncFile::open(p.generic());
    REQUIRE(f.is_open());

    ScriptedFault script;
    if (!crd::fault_seams_compiled())
    {
        script.fault = crd::IoFault{crd::IoFaultAction::Fail, 0U};
        const ScopedHook hook(script);
        CHECK_FALSE(hook.installed);
        crd::u8 dst[sizeof(data)] = {};
        CHECK(read_all(f, dst, sizeof(dst)) == AsyncReadStatus::Complete);
        CHECK(std::memcmp(dst, data, sizeof(data)) == 0);
        CHECK(script.calls.load() == 0U);
        (void)fs::remove_file(p);
        return;
    }

    SECTION("failed: nothing delivered, dst zeroed")
    {
        script.fault = crd::IoFault{crd::IoFaultAction::Fail, 0U};
        const ScopedHook hook(script);
        REQUIRE(hook.installed);
        crd::u8 dst[sizeof(data)];
        std::memset(dst, 0xEE, sizeof(dst));
        CHECK(read_all(f, dst, sizeof(dst)) == AsyncReadStatus::Failed);
        for (const crd::u8 b : dst)
        {
            CHECK(b == 0U);
        }
        CHECK(script.calls.load() == 1U);
        CHECK(script.last_op.load() == static_cast<crd::u32>(crd::IoFaultOp::AsyncRead));
    }
    SECTION("short: the delivered prefix is real, the rest zeroed")
    {
        script.fault = crd::IoFault{crd::IoFaultAction::Short, 3U};
        const ScopedHook hook(script);
        REQUIRE(hook.installed);
        crd::u8 dst[sizeof(data)];
        std::memset(dst, 0xEE, sizeof(dst));
        CHECK(read_all(f, dst, sizeof(dst)) == AsyncReadStatus::Short);
        CHECK(std::memcmp(dst, data, 3U) == 0);
        for (crd::usize i = 3U; i < sizeof(dst); ++i)
        {
            CHECK(dst[i] == 0U);
        }
    }
    SECTION("short by more than the request still stops one byte early")
    {
        script.fault = crd::IoFault{crd::IoFaultAction::Short, 1000U};
        const ScopedHook hook(script);
        REQUIRE(hook.installed);
        crd::u8 dst[sizeof(data)] = {};
        CHECK(read_all(f, dst, sizeof(dst)) == AsyncReadStatus::Short);
        CHECK(std::memcmp(dst, data, sizeof(data) - 1U) == 0);
        CHECK(dst[sizeof(data) - 1U] == 0U);
    }
    SECTION("cancelled: dst untouched")
    {
        script.fault = crd::IoFault{crd::IoFaultAction::Cancel, 0U};
        const ScopedHook hook(script);
        REQUIRE(hook.installed);
        crd::u8 dst[sizeof(data)];
        std::memset(dst, 0xEE, sizeof(dst));
        CHECK(read_all(f, dst, sizeof(dst)) == AsyncReadStatus::Cancelled);
        for (const crd::u8 b : dst)
        {
            CHECK(b == 0xEEU);
        }
    }
    SECTION("delayed: complete, and not before the delay")
    {
        script.fault = crd::IoFault{crd::IoFaultAction::Delay, 30ULL * 1000ULL * 1000ULL};
        const ScopedHook hook(script);
        REQUIRE(hook.installed);
        crd::u8    dst[sizeof(data)] = {};
        const auto t0                = std::chrono::steady_clock::now();
        CHECK(read_all(f, dst, sizeof(dst)) == AsyncReadStatus::Complete);
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        CHECK(elapsed >= std::chrono::milliseconds(30));
        CHECK(std::memcmp(dst, data, sizeof(data)) == 0);
    }
    SECTION("one hook at a time, and uninstall restores plain reads")
    {
        script.fault = crd::IoFault{crd::IoFaultAction::Fail, 0U};
        {
            const ScopedHook hook(script);
            REQUIRE(hook.installed);
            ScriptedFault    other;
            const ScopedHook second(other);
            CHECK_FALSE(second.installed);
        }
        crd::u8 dst[sizeof(data)] = {};
        CHECK(read_all(f, dst, sizeof(dst)) == AsyncReadStatus::Complete);
        CHECK(std::memcmp(dst, data, sizeof(data)) == 0);
        CHECK(script.calls.load() == 0U);
    }
    (void)fs::remove_file(p);
}

TEST_CASE("fs: synchronous reads fail on an injected failed, short or cancelled read and wait out a delay",
          "[platform][async_file][diag9b]")
{
    const crd::u8  data[] = {0x10, 0x20, 0x30, 0x40};
    const fs::Path p      = write_temp_file(data, sizeof(data));

    ScriptedFault script;
    if (!crd::fault_seams_compiled())
    {
        script.fault = crd::IoFault{crd::IoFaultAction::Fail, 0U};
        const ScopedHook hook(script);
        CHECK_FALSE(hook.installed);
        crd::containers::Array<crd::u8> out(&s_alloc);
        CHECK(fs::read_file_binary(p, out));
        CHECK(out.size() == sizeof(data));
        crd::containers::String text(&s_alloc);
        CHECK(fs::read_file_text(p, text));
        CHECK(text.size() == sizeof(data));
        (void)fs::remove_file(p);
        return;
    }

    const crd::IoFaultAction failing[] = {crd::IoFaultAction::Fail, crd::IoFaultAction::Short,
                                          crd::IoFaultAction::Cancel};
    for (const crd::IoFaultAction action : failing)
    {
        script.fault = crd::IoFault{action, 2U};
        const ScopedHook hook(script);
        REQUIRE(hook.installed);
        crd::containers::Array<crd::u8> whole(&s_alloc);
        whole.push_back(0xAAU);
        CHECK_FALSE(fs::read_file_binary(p, whole));
        CHECK(whole.empty());
        CHECK(script.last_op.load() == static_cast<crd::u32>(crd::IoFaultOp::ReadWhole));
        crd::containers::Array<crd::u8> range(&s_alloc);
        CHECK_FALSE(fs::read_file_range(p, 1U, 2U, range));
        CHECK(range.empty());
        CHECK(script.last_op.load() == static_cast<crd::u32>(crd::IoFaultOp::ReadRange));
        // A text read is a whole-file read on the same seam (configuration, cook sidecars, authored assets).
        script.last_op.store(99U);
        crd::containers::String text(&s_alloc);
        text.push_back('x');
        CHECK_FALSE(fs::read_file_text(p, text));
        CHECK(text.size() == 0U);
        CHECK(script.last_op.load() == static_cast<crd::u32>(crd::IoFaultOp::ReadWhole));
    }

    script.fault = crd::IoFault{crd::IoFaultAction::Delay, 20ULL * 1000ULL * 1000ULL};
    {
        const ScopedHook hook(script);
        REQUIRE(hook.installed);
        crd::containers::Array<crd::u8> range(&s_alloc);
        const auto                      t0 = std::chrono::steady_clock::now();
        CHECK(fs::read_file_range(p, 1U, 2U, range));
        CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(20));
        REQUIRE(range.size() == 2U);
        CHECK(range[0] == 0x20U);
        CHECK(range[1] == 0x30U);
        crd::containers::String text(&s_alloc);
        const auto              t1 = std::chrono::steady_clock::now();
        CHECK(fs::read_file_text(p, text));
        CHECK(std::chrono::steady_clock::now() - t1 >= std::chrono::milliseconds(20));
        REQUIRE(text.size() == sizeof(data));
        CHECK(static_cast<crd::u8>(text.data()[3]) == 0x40U);
    }
    (void)fs::remove_file(p);
}
