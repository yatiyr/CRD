#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <crd/platform/async_file.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <atomic>
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
    crd::jobs::Counter* c = f.read_async(0U, crd::containers::Span<crd::u8>(dst, sizeof(dst)));
    REQUIRE(c != nullptr);
    crd::jobs::wait(c);

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
    crd::jobs::Counter* c = f.read_async(0U, crd::containers::Span<crd::u8>(dst, sizeof(dst)));
    CHECK(c == nullptr);

    (void)fs::remove_file(p);
}
