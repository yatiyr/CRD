// Example: allocator selection under the diagnostic host (DIAG.11a).
//
//   diag-consumer allocator <profile.host.toml> <tlsf|diagnostic>
//
// An application chooses the allocator each of its owners gets, and the diagnostic host watches the one it chooses.
// This one has a single owner, an order book, and two choices:
//
//   tlsf        a bounded TLSF heap over a 64 KiB arena the application owns: what a shipped build gives the owner.
//   diagnostic  the same heap behind crd-memory's DiagnosticAllocator: guard bytes (redzones) on both sides of every
//               block, a bounded quarantine of freed blocks and per-block provenance. Its records live in a second,
//               independent arena, so the diagnosis never allocates from the heap it diagnoses. An investigation
//               build's choice.
//
// Whichever is chosen, the owner receives host.watch_allocator(chosen) instead: a try_allocate that fails becomes an
// allocation-failure trigger whose message names the chosen allocator, and while fault injection runs a seeded plan
// can fail it on purpose. The example books 1 KiB orders until the heap is exhausted, which captures the bookings
// before the failure and the releases after it into a flight bundle in the profile's capture directory. With the
// diagnostic allocator it also writes one byte past an order before booking the rest: the redzone check reports the
// overrun with its offset, the application records the violation as a flight event, and the bundle shows it before
// the exhaustion.
//
// Lifetimes: the allocators outlive every block, and the host is stopped before the allocator it watches goes.
#include "consumer_support.hpp"
#include "examples.hpp"

#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/diagnostic_allocator.hpp>

#include <cstdio>
#include <cstring>

namespace diag_consumer
{

namespace containers = crd::containers;

namespace
{

constexpr crd::usize kHeapBytes     = 64U * 1024U;
constexpr crd::usize kMetadataBytes = 128U * 1024U;
constexpr crd::usize kOrderBytes    = 1024U;
constexpr crd::u32   kMaxOrders     = 128U; // more than the heap can hold, so exhaustion ends the booking
constexpr crd::u32   kOverrunOrder  = 2U;

alignas(16) unsigned char g_heap_arena[kHeapBytes];         // NOLINT(modernize-avoid-c-arrays)
alignas(16) unsigned char g_metadata_arena[kMetadataBytes]; // NOLINT(modernize-avoid-c-arrays)

const char* violation_name(crd::memory::ViolationKind kind)
{
    switch (kind)
    {
    case crd::memory::ViolationKind::RedzoneUnderrun:        return "redzone-underrun";
    case crd::memory::ViolationKind::RedzoneOverrun:         return "redzone-overrun";
    case crd::memory::ViolationKind::DoubleFree:             return "double-free";
    case crd::memory::ViolationKind::UnknownPointer:         return "unknown-pointer";
    case crd::memory::ViolationKind::MetadataSaturation:     return "metadata-saturation";
    case crd::memory::ViolationKind::QuarantineUseAfterFree: return "quarantine-use-after-free";
    }
    return "unknown";
}

// What the diagnostic allocator reported, kept by the application and recorded into the flight recorder.
struct ViolationLog
{
    cp::FlightRecorder*        recorder = nullptr;
    crd::u32                   count    = 0U;
    crd::memory::ViolationKind first    = crd::memory::ViolationKind::UnknownPointer;
    crd::usize                 offset   = 0U;
};

void on_violation(const crd::memory::Violation& v, void* user)
{
    auto* const log = static_cast<ViolationLog*>(user);
    if (log->count == 0U)
    {
        log->first  = v.kind;
        log->offset = v.byte_offset;
    }
    ++log->count;
    char text[96];
    (void)std::snprintf(text, sizeof(text), "heap violation %s at byte %llu", violation_name(v.kind),
                        static_cast<unsigned long long>(v.byte_offset));
    if (log->recorder != nullptr)
    {
        log->recorder->record(kHeapViolation, cp::Severity::RecoverableError, containers::StringView{text},
                              static_cast<crd::u64>(v.kind), v.byte_offset);
    }
}

// Book orders from the owner's allocator until it is exhausted, then release them. `diagnostic` is the chosen
// allocator when it is the diagnostic one (it can then check its redzones), null otherwise.
int book_orders(cp::DiagnosticHost& host, crd::memory::IAllocator& chosen, crd::memory::DiagnosticAllocator* diagnostic)
{
    crd::memory::IAllocator* const orders = host.watch_allocator(&chosen, "order-book");
    if (orders == nullptr)
    {
        std::printf("CONSUMER_WATCH_REFUSED\n");
        return 7;
    }
    std::printf("CONSUMER_ALLOCATOR chosen=%s heap_bytes=%llu order_bytes=%llu\n", chosen.name(),
                static_cast<unsigned long long>(kHeapBytes), static_cast<unsigned long long>(kOrderBytes));

    void*    blocks[kMaxOrders] = {}; // NOLINT(modernize-avoid-c-arrays)
    crd::u32 booked             = 0U;
    char     text[48];
    while (booked < kMaxOrders)
    {
        void* const block = orders->try_allocate(kOrderBytes);
        if (block == nullptr)
        {
            break; // the watched allocator has fired its allocation-failure trigger
        }
        std::memset(block, 0x11, kOrderBytes);
        blocks[booked] = block;
        (void)std::snprintf(text, sizeof(text), "order %u reserved", static_cast<unsigned>(booked));
        host.recorder().record(kOrderReserved, cp::Severity::Info, containers::StringView{text}, booked, kOrderBytes);
        if (diagnostic != nullptr && booked == kOverrunOrder)
        {
            // The application's defect: one byte written past the order it owns, then caught by a redzone check.
            static_cast<unsigned char*>(block)[kOrderBytes] = 0x5AU;
            std::printf("CONSUMER_ALLOC_CHECKED violations=%llu\n",
                        static_cast<unsigned long long>(diagnostic->check_all_redzones()));
        }
        ++booked;
    }
    std::printf("CONSUMER_ALLOC_EXHAUSTED booked=%u exhausted=%d\n", static_cast<unsigned>(booked),
                booked < kMaxOrders ? 1 : 0);
    for (crd::u32 i = 0U; i < booked; ++i)
    {
        orders->deallocate(blocks[i]); // the overrun order is reported again by the check its release runs
        (void)std::snprintf(text, sizeof(text), "order %u released", static_cast<unsigned>(i));
        host.recorder().record(kOrderReleased, cp::Severity::Info, containers::StringView{text}, i, 0U);
    }
    if (!wait_for_bundles(host, 1U, 10000U))
    {
        std::printf("CONSUMER_NO_BUNDLE written=%llu\n", static_cast<unsigned long long>(host.bundles_written()));
        return 8;
    }
    print_last_bundle(host, "allocation-failure");
    return 0;
}

int with_tlsf(cp::DiagnosticHost& host)
{
    crd::memory::TlsfAllocator heap{g_heap_arena, kHeapBytes, "orders-tlsf"};
    const int                  result = book_orders(host, heap, nullptr);
    host.stop(); // the host's watch over `heap` goes before `heap` does
    return result;
}

int with_diagnostic(cp::DiagnosticHost& host)
{
    crd::memory::TlsfAllocator heap{g_heap_arena, kHeapBytes, "orders-heap"};
    crd::memory::TlsfAllocator metadata{g_metadata_arena, kMetadataBytes, "orders-metadata"};

    crd::memory::DiagnosticConfig config;
    config.redzone_bytes             = 16U;
    config.quarantine_capacity_bytes = 4U * kOrderBytes;
    config.max_live_records          = 256U;
    config.max_stacks                = 64U;
    crd::memory::DiagnosticAllocator diagnostic{&heap, config, &metadata, "orders-diagnostic"};
    ViolationLog                     log;
    log.recorder = &host.recorder();
    diagnostic.set_violation_handler(&on_violation, &log);

    const int result = book_orders(host, diagnostic, &diagnostic);
    // The leak summary: every order was released, so no block is still live (the newest freed ones sit in quarantine).
    std::printf("CONSUMER_ALLOC_DIAGNOSTIC violations=%u first=%s offset=%llu leaks=%llu quarantined_bytes=%llu\n",
                static_cast<unsigned>(log.count), log.count > 0U ? violation_name(log.first) : "none",
                static_cast<unsigned long long>(log.offset),
                static_cast<unsigned long long>(diagnostic.report_leaks(nullptr, nullptr)),
                static_cast<unsigned long long>(diagnostic.quarantined_bytes()));
    host.stop(); // the host's watch over `diagnostic` goes before `diagnostic` does
    log.recorder = nullptr;
    return result;
}

} // namespace

int example_allocator(const char* profile_path, const char* choice)
{
    const bool diagnostic = std::strcmp(choice, "diagnostic") == 0;
    if (!diagnostic && std::strcmp(choice, "tlsf") != 0)
    {
        std::printf("CONSUMER_USAGE allocator <profile.host.toml> <tlsf|diagnostic>\n");
        return 2;
    }
    cp::DiagnosticHost host;
    if (!start_host(host, profile_path, diagnostic ? "diag-consumer allocator <profile.host.toml> diagnostic"
                                                   : "diag-consumer allocator <profile.host.toml> tlsf"))
    {
        return 4;
    }
    const int result = diagnostic ? with_diagnostic(host) : with_tlsf(host);
    host.stop();
    return result;
}

} // namespace diag_consumer
