// DIAG.6a(d): the profiler OWNS its name bytes. Before (d) every registry (region/thread/counter/allocator) stored the
// CALLER's `const char*` -- a dynamic name (built in a stack/heap buffer, or living in a module later unloaded) dangled,
// and resolve_name / the capture name-blob writer read freed or overwritten memory (census defect #4). Now the bytes
// are copied into a bounded arena on the cold registration path; the caller's buffer may be reused or freed after the
// call. Content dedup (FNV-1a + strcmp) is preserved, so this is pure ownership, not a keying change. Every case below
// DISCRIMINATES against the old borrowing behaviour: the buffer-reuse and thread-name cases would resolve to the wrong
// (overwritten) bytes, and the heap-dangle / capture cases read freed memory (an ASan heap-use-after-free) under it.
// Proof lanes: win-debug + win-asan (the dangle cases are ASan oracles).
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#if CRD_PERF_ENABLED

namespace
{
struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

// Copy a NUL-terminated string into a caller buffer (memcpy, not the MSVC-deprecated strcpy). Caller guarantees room.
void set_str(char* dst, const char* src) noexcept { std::memcpy(dst, src, std::strlen(src) + 1U); }
} // namespace

TEST_CASE("dynamic names: an interned name survives freeing the caller's heap buffer", "[perf][diag][names]")
{
    PerfFixture fx;
    char*       heap = new char[16];
    set_str(heap, "dyn.heap");
    const auto id = crd::perf::intern_name(heap);
    REQUIRE(id.is_valid());
    std::memset(heap, 0xAB, 16); // scribble, then free: a BORROWED pointer would now read garbage / freed memory
    delete[] heap;
    // Owned copy: intact. Under the old borrow this strcmp is a heap-use-after-free (ASan) and mismatches (win-debug).
    CHECK(std::strcmp(crd::perf::resolve_name(id), "dyn.heap") == 0);
}

TEST_CASE("dynamic names: a reused caller buffer yields distinct, correctly-resolved ids", "[perf][diag][names]")
{
    PerfFixture fx;
    char        buf[32];
    set_str(buf, "dyn.a");
    const auto id_a = crd::perf::intern_name(buf);
    set_str(buf, "dyn.b"); // reuse the SAME buffer for a different name
    const auto id_b = crd::perf::intern_name(buf);
    REQUIRE(id_a.is_valid());
    REQUIRE(id_b.is_valid());
    CHECK(id_a.value != id_b.value);
    // Owned: id_a still resolves to "dyn.a". Under the old borrow both ids pointed at `buf`, so resolve(id_a) would be
    // "dyn.b" -- the mislabel this removes.
    CHECK(std::strcmp(crd::perf::resolve_name(id_a), "dyn.a") == 0);
    CHECK(std::strcmp(crd::perf::resolve_name(id_b), "dyn.b") == 0);
}

TEST_CASE("dynamic names: interning equal content twice dedups to one id (content key preserved)", "[perf][diag][names]")
{
    PerfFixture fx;
    const auto  before = crd::perf::intern_name_count();
    const auto  a      = crd::perf::intern_name("dyn.lit");
    // A separate buffer with the same bytes must dedup to the same id (content-keyed, not pointer-keyed).
    char same[16];
    set_str(same, "dyn.lit");
    const auto b = crd::perf::intern_name(same);
    CHECK(a.value == b.value);
    CHECK(crd::perf::intern_name_count() == before + 1U); // one new entry, not two
}

TEST_CASE("dynamic names: table saturation is explicit (kInvalidNameId + drop count), never a borrow",
          "[perf][diag][names]")
{
    crd::perf::InitConfig cfg{};
    cfg.max_region_names = 8U; // tiny table -> saturates quickly (rounded up to a power of two)
    crd::perf::init(cfg);
    const crd::u32 cap         = crd::perf::intern_name_capacity();
    bool           saw_invalid = false;
    char           nm[32];
    for (crd::u32 i = 0U; i < cap + 8U; ++i)
    {
        std::snprintf(nm, sizeof(nm), "sat.%u", i);
        if (!crd::perf::intern_name(nm).is_valid())
        {
            saw_invalid = true;
        }
    }
    CHECK(saw_invalid);
    CHECK(crd::perf::name_bytes_dropped_count() > 0U);
    CHECK(std::strcmp(crd::perf::resolve_name(crd::perf::kInvalidNameId), "") == 0); // never a borrowed pointer
    crd::perf::shutdown();
}

TEST_CASE("dynamic names: an over-long name is dropped and counted, not truncated", "[perf][diag][names]")
{
    PerfFixture    fx;
    const crd::u64 before = crd::perf::name_bytes_dropped_count();
    char           big[crd::perf::kMaxNameBytes + 16];
    std::memset(big, 'x', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    const auto id = crd::perf::intern_name(big);
    CHECK(!id.is_valid()); // dropped, NOT truncated-and-interned (truncation + dedup could collide two names)
    CHECK(crd::perf::name_bytes_dropped_count() == before + 1U);
}

TEST_CASE("dynamic names: a registered thread name survives the caller's buffer dying", "[perf][diag][names]")
{
    PerfFixture           fx;
    std::atomic<crd::u32> tidx{0xFFU};
    std::thread           t(
        [&]()
        {
            char nm[32];
            set_str(nm, "worker.dyn");
            const crd::u8 i = crd::perf::register_thread(nm);
            set_str(nm, "OVERWRITTEN"); // reuse the buffer after registration
            tidx.store(i, std::memory_order_release);
        });
    t.join(); // the thread's stack (where `nm` lived) is now gone
    const crd::u32 i = tidx.load(std::memory_order_acquire);
    REQUIRE(i != 0xFFU);
    const auto v = crd::perf::thread_samples(static_cast<crd::u8>(i));
    // Owned copy: still "worker.dyn". Under the old borrow this pointed into the dead stack (garbage / ASan UAF).
    CHECK(std::strcmp(v.name != nullptr ? v.name : "", "worker.dyn") == 0);
}

TEST_CASE("dynamic names: a heap name freed before save still resolves in the capture", "[perf][diag][names]")
{
    crd::memory::GrowableTlsfAllocator alloc{64ULL << 20, nullptr, "d-capture"};
    PerfFixture                        fx;

    char* heap = new char[24];
    set_str(heap, "cap.dyn.name");
    const auto id = crd::perf::intern_name(heap);
    REQUIRE(id.is_valid());
    delete[] heap; // freed BEFORE the save walks the name table

    const auto buf = crd::perf::save_capture_to_buffer(&alloc);
    REQUIRE(buf.size() > 0U);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    // The blob writer resolved the interned name from the arena (not the freed heap), so the capture is correct. Under
    // the old borrow the writer read the freed buffer -- an ASan UAF at save time and garbage in the file.
    CHECK(std::strcmp(view.resolve_name(id), "cap.dyn.name") == 0);
}

TEST_CASE("dynamic names: relabeling with an unchanged name does not leak the name arena", "[perf][diag][names]")
{
    crd::perf::InitConfig cfg{};
    cfg.max_region_names = 8U; // tiny table -> tiny (~17 KB) name arena, so a per-call leak would exhaust it quickly
    crd::perf::init(cfg);

    crd::memory::GrowableTlsfAllocator target{1ULL << 20, nullptr, "relabel-target"};
    const crd::u32                     idx = crd::perf::register_allocator("alloc.stable", &target);
    REQUIRE(idx < crd::perf::kMaxAllocators);

    // Re-register the SAME allocator under the SAME name many times (the "relabel" path -- e.g. a component re-registered
    // per level load). With the strcmp guard the name is owned exactly once; without it each call would burn ~13 bytes
    // and exhaust the ~17 KB arena well before 5000 iterations, forcing drops.
    bool all_same = true;
    for (int i = 0; i < 5000; ++i)
    {
        if (crd::perf::register_allocator("alloc.stable", &target) != idx)
        {
            all_same = false;
        }
    }
    CHECK(all_same);
    CHECK(crd::perf::name_bytes_dropped_count() == 0U); // no arena pressure from an unchanged relabel -> nothing dropped

    crd::perf::unregister_allocator(idx);
    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
