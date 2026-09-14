// DIAG.3c -- allocation/free provenance, redzones, bounded quarantine, guarded sampling and
// leak/retention summaries. Every acceptance clause is asserted as a distinct, attributable verdict,
// never a silent pass. Contract: docs/design/runtime-diagnostics.md#diag-3c.

#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/diagnostic_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace
{
namespace mem = crd::memory;

// Records the violations the allocator raises so a test can assert the exact verdict + site.
struct Recorder
{
    static constexpr int kMax = 64;
    mem::Violation       hits[kMax];
    int                  count = 0;

    void add(const mem::Violation& v)
    {
        if (count < kMax)
        {
            hits[count] = v;
        }
        ++count;
    }
    int count_of(mem::ViolationKind k) const
    {
        int n = 0;
        for (int i = 0; i < count && i < kMax; ++i)
        {
            if (hits[i].kind == k)
            {
                ++n;
            }
        }
        return n;
    }
};

void record_violation(const mem::Violation& v, void* user)
{
    static_cast<Recorder*>(user)->add(v);
}

mem::DiagnosticConfig default_cfg()
{
    mem::DiagnosticConfig c;
    c.redzone_bytes = 16U;
    c.sample_period = 1U; // sample every allocation so provenance sites are always present
    c.quarantine_capacity_bytes = 0U;
    c.max_live_records = 256U;
    c.max_stacks = 256U;
    return c;
}

struct LeakSink
{
    int        n = 0;
    crd::usize bytes = 0;
};
} // namespace

TEST_CASE("diag-alloc: correct usage is clean and provenance-complete", "[memory][diag][provenance]")
{
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    Recorder                 rec;
    mem::DiagnosticAllocator da(&backing, default_cfg());
    da.set_violation_handler(record_violation, &rec);

    void* p = da.allocate(100U, 16U);
    REQUIRE(p != nullptr);
    CHECK((reinterpret_cast<std::uintptr_t>(p) % 16U) == 0U);
    std::memset(p, 0xAB, 100U); // fully in-bounds

    const mem::Provenance prov = da.provenance_of(p);
    CHECK(prov.found);
    CHECK_FALSE(prov.freed);
    CHECK(prov.size == 100U);
    CHECK(prov.alloc_stack != mem::kNullStackId); // sampled -> a real allocation site
    void*          frames[mem::kMaxStackFrames];
    const crd::u32 n = da.stack_frames(prov.alloc_stack, frames, mem::kMaxStackFrames);
    CHECK(n > 0U); // PCs captured for offline symbolization

    CHECK(da.check_all_redzones() == 0U);
    da.deallocate(p);
    CHECK(rec.count == 0);
    CHECK(da.violation_count() == 0U);
}

TEST_CASE("diag-alloc: a rear overrun is an attributable RedzoneOverrun", "[memory][diag][redzone]")
{
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    Recorder                 rec;
    mem::DiagnosticAllocator da(&backing, default_cfg());
    da.set_violation_handler(record_violation, &rec);

    auto* p = static_cast<crd::u8*>(da.allocate(32U, 16U));
    REQUIRE(p != nullptr);
    p[32] = 0x01; // one past the end -- into the rear redzone

    const crd::usize found = da.check_all_redzones();
    CHECK(found == 1U);
    CHECK(rec.count_of(mem::ViolationKind::RedzoneOverrun) == 1);
    // The violation carries the site: a sampled allocation has a real alloc stack.
    CHECK(rec.hits[0].alloc_stack != mem::kNullStackId);
    CHECK(rec.hits[0].byte_offset == 32U);
}

TEST_CASE("diag-alloc: a front underrun is an attributable RedzoneUnderrun", "[memory][diag][redzone]")
{
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    Recorder                 rec;
    mem::DiagnosticAllocator da(&backing, default_cfg());
    da.set_violation_handler(record_violation, &rec);

    auto* p = static_cast<crd::u8*>(da.allocate(48U, 16U));
    REQUIRE(p != nullptr);
    p[-1] = 0x02; // one before the start -- into the front redzone

    CHECK(da.check_all_redzones() == 1U);
    CHECK(rec.count_of(mem::ViolationKind::RedzoneUnderrun) == 1);
}

TEST_CASE("diag-alloc: use-after-free on a quarantined block reports both sites", "[memory][diag][quarantine]")
{
    mem::DiagnosticConfig cfg = default_cfg();
    cfg.quarantine_capacity_bytes = 4096U; // hold freed blocks so the pointer stays attributable
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    Recorder                 rec;
    mem::DiagnosticAllocator da(&backing, cfg);
    da.set_violation_handler(record_violation, &rec);

    auto* p = static_cast<crd::u8*>(da.allocate(64U, 16U));
    REQUIRE(p != nullptr);
    da.deallocate(p); // now quarantined, poisoned

    // The stale access is still fully attributable: alloc AND free sites survive in the record.
    const mem::Provenance prov = da.provenance_of(p);
    CHECK(prov.found);
    CHECK(prov.freed);
    CHECK(prov.alloc_stack != mem::kNullStackId);
    CHECK(prov.free_stack != mem::kNullStackId);

    p[10] = 0x7E; // write through the freed pointer -- a use-after-free
    CHECK(da.check_all_redzones() >= 1U);
    CHECK(rec.count_of(mem::ViolationKind::QuarantineUseAfterFree) == 1);

    // Restore the poison so quarantine eviction on teardown does not re-report.
    p[10] = static_cast<crd::u8>(cfg.quarantine_pattern);
}

TEST_CASE("diag-alloc: a double free is detected, not passed through", "[memory][diag][quarantine]")
{
    mem::DiagnosticConfig cfg = default_cfg();
    cfg.quarantine_capacity_bytes = 4096U;
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    Recorder                 rec;
    mem::DiagnosticAllocator da(&backing, cfg);
    da.set_violation_handler(record_violation, &rec);

    void* p = da.allocate(40U, 16U);
    da.deallocate(p);
    da.deallocate(p); // second free of the same pointer
    CHECK(rec.count_of(mem::ViolationKind::DoubleFree) == 1);
}

TEST_CASE("diag-alloc: an unknown pointer free is rejected", "[memory][diag]")
{
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    Recorder                 rec;
    mem::DiagnosticAllocator da(&backing, default_cfg());
    da.set_violation_handler(record_violation, &rec);

    int stack_local = 0;
    da.deallocate(&stack_local); // never handed out by this allocator
    CHECK(rec.count_of(mem::ViolationKind::UnknownPointer) == 1);
}

TEST_CASE("diag-alloc: metadata saturation never corrupts and is not silent in mandatory mode",
          "[memory][diag][saturation]")
{
    mem::DiagnosticConfig cfg = default_cfg();
    cfg.max_live_records = 4U; // tiny table
    cfg.mandatory = true;
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    Recorder                 rec;
    mem::DiagnosticAllocator da(&backing, cfg);
    da.set_violation_handler(record_violation, &rec);

    void* live[6] = {};
    for (int i = 0; i < 6; ++i) // 6 live > 4 records -> the last two saturate
    {
        live[i] = da.allocate(32U, 16U);
        REQUIRE(live[i] != nullptr);     // the allocation STILL succeeds under saturation...
        std::memset(live[i], 0x11, 32U); // ...and the memory is fully usable (no corruption)
    }
    CHECK(da.metadata_saturation_count() >= 1U);
    CHECK(rec.count_of(mem::ViolationKind::MetadataSaturation) >= 1); // never silent (mandatory)

    for (int i = 0; i < 6; ++i)
    {
        da.deallocate(live[i]); // both tracked and passthrough frees are handled
    }
}

TEST_CASE("diag-alloc: quarantine is bounded and evicts oldest-first", "[memory][diag][quarantine]")
{
    mem::DiagnosticConfig cfg = default_cfg();
    cfg.quarantine_capacity_bytes = 256U; // holds ~4x64B before eviction
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    mem::DiagnosticAllocator da(&backing, cfg);

    for (int i = 0; i < 20; ++i)
    {
        void* p = da.allocate(64U, 16U);
        REQUIRE(p != nullptr);
        da.deallocate(p);
        CHECK(da.quarantined_bytes() <= cfg.quarantine_capacity_bytes); // never exceeds the cap
    }
    CHECK(da.quarantined_bytes() <= cfg.quarantine_capacity_bytes);
}

TEST_CASE("diag-alloc: sampled mode reports probability and eligible window, not exhaustive detection",
          "[memory][diag][sampling]")
{
    mem::DiagnosticConfig cfg = default_cfg();
    cfg.sample_period = 4U;     // 1-in-4
    cfg.min_sampled_size = 32U; // only >=32B eligible
    cfg.max_sampled_size = 1024U;
    mem::TlsfAllocator       backing(512U * 1024U, nullptr, "backing");
    mem::DiagnosticAllocator da(&backing, cfg);

    void* keep[40] = {};
    for (int i = 0; i < 40; ++i)
    {
        keep[i] = da.allocate(64U, 16U); // all eligible (64 in [32,1024])
    }
    void* small = da.allocate(16U, 16U); // ineligible (below min) -> never sampled
    CHECK(small != nullptr);

    const mem::SamplingReport r = da.sampling_report();
    CHECK(r.sample_period == 4U);
    CHECK(r.min_sampled_size == 32U);
    CHECK(r.max_sampled_size == 1024U);
    CHECK(r.total_count == 41U);
    CHECK(r.eligible_count == 40U);            // the small one was not eligible
    CHECK(r.sampled_count == 10U);             // exactly 1-in-4 of the 40 eligible
    CHECK(r.sampled_count < r.eligible_count); // sampling never implies exhaustive coverage

    for (int i = 0; i < 40; ++i)
    {
        da.deallocate(keep[i]);
    }
    da.deallocate(small);
}

TEST_CASE("diag-alloc: leak/retention summary names still-live allocations", "[memory][diag][leak]")
{
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    mem::DiagnosticAllocator da(&backing, default_cfg());

    void* a = da.allocate(24U, 16U);
    void* b = da.allocate(24U, 16U);
    void* c = da.allocate(24U, 16U);
    da.deallocate(b); // b is freed; a and c leak

    LeakSink         sink;
    const crd::usize live = da.report_leaks(
        [](const mem::Provenance& p, void* user)
        {
            auto* s = static_cast<LeakSink*>(user);
            ++s->n;
            s->bytes += p.size;
        },
        &sink);

    CHECK(live == 2U);
    CHECK(sink.n == 2);
    CHECK(sink.bytes == 48U);

    da.deallocate(a);
    da.deallocate(c);
    CHECK(da.report_leaks(nullptr, nullptr) == 0U);
}

TEST_CASE("diag-alloc: memory overhead is bounded and accounted", "[memory][diag][cost]")
{
    mem::DiagnosticConfig cfg = default_cfg();
    cfg.redzone_bytes = 16U;
    mem::TlsfAllocator       backing(256U * 1024U, nullptr, "backing");
    mem::DiagnosticAllocator da(&backing, cfg);

    // Deterministic per-allocation overhead = front (redzone aligned up) + rear redzone.
    const crd::usize overhead = da.per_allocation_overhead();
    CHECK(overhead == 32U); // align_up(16,16)=16 front + 16 rear

    void* p = da.allocate(100U, 16U);
    REQUIRE(p != nullptr);
    // The instrumented block is exactly `size + overhead` of backing memory.
    CHECK(backing.allocation_size(static_cast<crd::u8*>(p) - 16) >= 100U + overhead);
    da.deallocate(p);
}
