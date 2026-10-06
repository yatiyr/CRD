// DIAG.3b negative control: the allocator-aware AddressSanitizer boundaries must turn a stale or out-of-range access
// to a logical allocation into a sanitizer report. A sub-allocator hands out slices of one parent buffer, so core ASan
// alone sees only the parent's ends; the arenas and pools poison what is not live (crd/memory/asan_poison.hpp). Each
// mode (argv[1]) builds a real allocator, performs one intentional bad read and, if the read returns, exits 0, which
// the consuming test rejects:
//   linear-reset          LinearAllocator::reset(), then read a slice from before the reset
//   linear-rewind         a LinearScope rewinds, then read a slice the scope allocated
//   linear-overrun        allocate 13 bytes, read byte 13
//   linear-small-overrun  allocate 1 byte, read byte 1
//   linear-underrun       a 64-aligned slice after a 16-byte one: read the byte before it (alignment padding)
//   linear-nested-parent  a child arena over a parent slice is destroyed; read the parent's unhanded tail just past it
//   stack-pop             a StackScope pops a frame, then read the frame
//   glinear-reset         GrowableLinearAllocator::reset(), then read a slice from before the reset
//   pool-freed-slot       read the first byte (the free-list header) of a freed pool slot
//   pool-overrun          two adjacent slots, the upper one freed: read one byte past the lower slot
//   pool-underrun         two adjacent slots, the lower one freed: read one byte before the upper slot
//   pool-odd-freed        a 4099-byte slot (packed at an 8-byte stride) is freed; read its last byte
//   pool-odd-overrun      two adjacent 4099-byte slots, the upper one freed: read one byte past the lower slot
//   gpool-freed-slot      GrowablePoolAllocator: read the first byte of a freed slot
//   gpool-overrun         GrowablePoolAllocator: two adjacent slots, the upper one freed: read one byte past the lower
//   tlsf-freed            TlsfAllocator: read the first byte (a free-list link) of a freed block
//   tlsf-overrun          read one byte past a live 48-byte block (the next block's header)
//   tlsf-underrun         read the byte before a live block (its own header)
//   tlsf-aligned          a 256-aligned block: read into the free leading remainder split off before it
//   tlsf-large            free a 64 KiB block and read its middle
//   tlsf-shrink           reallocate a 256-byte block down to 32 in place, then read byte 32 (the split-off tail)
//   gtlsf-freed           GrowableTlsfAllocator: read a freed block in its second chunk
//   ring-retired          RingAllocator: retire a claim's epoch, then read the claim
//   ring-overrun          read one byte past a 64-byte claim (unclaimed space)
//   ring-underrun         a 64-aligned claim after an 8-byte one: read the byte before it (alignment padding)
//
// Under ASan a death callback checks the report is "use-after-poison" (the allocator's own poison, not some other
// fault) and exits 42; any other report exits 43, an allocator layout the mode did not get (two pool slots that are
// not neighbours, a shrink that moved, a growable heap that did not grow) 95, an unknown mode 96.
// Without ASan the allocators still run every step but the bad read is skipped: the specimen tags itself
// SANITIZER=none and exits 0 (InstrumentAbsent). Immediate
// same-address reuse and an overrun into a LIVE neighbour are raw-pointer limits ASan cannot see; DIAG.3e covers them
// with generations. A pool's logical allocation is its whole slot (allocation_size() reports the slot), so its
// overruns are declared at the slot boundary, into a free neighbour. A TLSF block is its logical allocation the same
// way; every block header is poisoned, so its over- and underruns are caught whether the neighbour is live or free.
// The ring tracks claims per 8-byte granule, so its modes use 8-aligned offsets and sizes.

#define CRD_DIAG_SPECIMEN_ASAN_CLASS // only AddressSanitizer catches this class (see specimen_common.hpp)

#include "specimen_common.hpp"

#include <crd/memory/allocators/growable_linear_allocator.hpp>
#include <crd/memory/allocators/growable_pool_allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/pool_allocator.hpp>
#include <crd/memory/allocators/ring_allocator.hpp>
#include <crd/memory/allocators/stack_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#if CRD_DIAG_HAS_ASAN
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
namespace mem = crd::memory;

#if CRD_DIAG_HAS_ASAN
void on_asan_death()
{
    const char* const kind = __asan_get_report_description();
    std::printf("CRD_DIAG_ASAN_REPORT=%s\n", kind != nullptr ? kind : "(none)");
    std::fflush(stdout);
    const bool poisoned = kind != nullptr && std::strcmp(kind, "use-after-poison") == 0;
    std::_Exit(poisoned ? 42 : 43);
}
#endif

// The intentional bad read. Only performed where ASan can catch it; elsewhere it would be undefined behaviour with
// nothing to observe.
void touch(const void* p, crd::usize offset)
{
#if CRD_DIAG_HAS_ASAN
    const volatile crd::u8* const bytes = static_cast<const crd::u8*>(p);
    const crd::u8 value = bytes[offset];
    std::printf("CRD_DIAG_READ_RETURNED=%u\n", static_cast<unsigned>(value));
    std::fflush(stdout);
#else
    (void)p;
    (void)offset;
#endif
}

void fill(void* p, crd::usize n)
{
    std::memset(p, 0x5A, n);
}

[[nodiscard]] bool run_linear_mode(const char* mode)
{
    constexpr crd::usize bytes = 4096U;
    if (std::strcmp(mode, "linear-reset") == 0)
    {
        mem::LinearAllocator a(bytes, nullptr, "poison-linear");
        void* const p = a.allocate(64U, 16U);
        fill(p, 64U);
        a.reset();
        touch(p, 0U);
        return true;
    }
    if (std::strcmp(mode, "linear-rewind") == 0)
    {
        mem::LinearAllocator a(bytes, nullptr, "poison-linear");
        fill(a.allocate(32U, 16U), 32U); // survives the scope
        void* p = nullptr;
        {
            mem::LinearScope scope(a);
            p = a.allocate(64U, 16U);
            fill(p, 64U);
        }
        touch(p, 0U);
        return true;
    }
    if (std::strcmp(mode, "linear-overrun") == 0)
    {
        mem::LinearAllocator a(bytes, nullptr, "poison-linear");
        void* const p = a.allocate(13U, 16U);
        fill(p, 13U);
        touch(p, 13U);
        return true;
    }
    if (std::strcmp(mode, "linear-small-overrun") == 0)
    {
        mem::LinearAllocator a(bytes, nullptr, "poison-linear");
        void* const p = a.allocate(1U, 16U);
        fill(p, 1U);
        touch(p, 1U);
        return true;
    }
    if (std::strcmp(mode, "linear-underrun") == 0)
    {
        mem::LinearAllocator a(bytes, nullptr, "poison-linear");
        fill(a.allocate(16U, 64U), 16U); // a 64-aligned 16-byte slice, so 48 bytes of padding precede the next one
        auto* const q = static_cast<crd::u8*>(a.allocate(64U, 64U));
        fill(q, 64U);
        touch(q - 1, 0U);
        return true;
    }
    if (std::strcmp(mode, "linear-nested-parent") == 0)
    {
        mem::LinearAllocator parent(bytes, nullptr, "poison-parent");
        auto* const slice = static_cast<crd::u8*>(parent.allocate(256U, 16U));
        {
            mem::LinearAllocator child(slice, 256U, "poison-child");
            fill(child.allocate(64U, 16U), 64U);
        } // the child returns its slice unpoisoned, and nothing beyond it
        fill(slice, 256U);
        touch(slice, 256U);
        return true;
    }
    return false;
}

[[nodiscard]] bool run_arena_mode(const char* mode)
{
    if (std::strcmp(mode, "stack-pop") == 0)
    {
        mem::StackAllocator a(4096U, nullptr, "poison-stack");
        void* p = nullptr;
        {
            mem::StackScope frame(a);
            p = a.allocate(64U, 16U);
            fill(p, 64U);
        }
        touch(p, 0U);
        return true;
    }
    if (std::strcmp(mode, "glinear-reset") == 0)
    {
        mem::GrowableLinearAllocator a(4096U, nullptr, "poison-glinear");
        void* const p = a.allocate(64U, 16U);
        fill(p, 64U);
        a.reset();
        touch(p, 0U);
        return true;
    }
    return run_linear_mode(mode);
}

[[nodiscard]] bool run_ring_mode(const char* mode)
{
    if (std::strcmp(mode, "ring-retired") == 0)
    {
        mem::RingAllocator ring(4096U, nullptr, 4U, "poison-ring");
        void* const p = ring.try_claim(64U, 16U);
        fill(p, 64U);
        ring.begin_epoch(1U);
        ring.retire(0U); // epoch 0, which holds the claim, is done
        touch(p, 0U);
        return true;
    }
    if (std::strcmp(mode, "ring-overrun") == 0)
    {
        mem::RingAllocator ring(4096U, nullptr, 4U, "poison-ring");
        void* const p = ring.try_claim(64U, 16U);
        fill(p, 64U);
        touch(p, 64U);
        return true;
    }
    if (std::strcmp(mode, "ring-underrun") == 0)
    {
        mem::RingAllocator ring(4096U, nullptr, 4U, "poison-ring");
        fill(ring.try_claim(8U, 16U), 8U); // offset 0; the next 64-aligned claim leaves 56 bytes of padding
        auto* const q = static_cast<crd::u8*>(ring.try_claim(64U, 64U));
        fill(q, 64U);
        touch(q - 1, 0U);
        return true;
    }
    return run_arena_mode(mode);
}

[[nodiscard]] bool run_tlsf_mode(const char* mode)
{
    constexpr crd::usize bytes = 64U * 1024U;
    if (std::strcmp(mode, "tlsf-freed") == 0)
    {
        mem::TlsfAllocator a(bytes, nullptr, "poison-tlsf");
        void* const p = a.allocate(64U, 16U);
        fill(a.allocate(64U, 16U), 64U); // a live neighbour, so the freed block does not merge into the tail
        fill(p, 64U);
        a.deallocate(p);
        touch(p, 0U);
        return true;
    }
    if (std::strcmp(mode, "tlsf-overrun") == 0)
    {
        mem::TlsfAllocator a(bytes, nullptr, "poison-tlsf");
        void* const p = a.allocate(48U, 16U);
        fill(a.allocate(48U, 16U), 48U); // the block after is live: its header still faults
        fill(p, 48U);
        touch(p, 48U);
        return true;
    }
    if (std::strcmp(mode, "tlsf-underrun") == 0)
    {
        mem::TlsfAllocator a(bytes, nullptr, "poison-tlsf");
        fill(a.allocate(48U, 16U), 48U);
        auto* const q = static_cast<crd::u8*>(a.allocate(48U, 16U));
        fill(q, 48U);
        touch(q - 1, 0U);
        return true;
    }
    if (std::strcmp(mode, "tlsf-aligned") == 0)
    {
        // A 256-aligned pool makes the layout exact: after a 16-byte block the next natural payload sits at +64, so
        // the 256-aligned block lands at +256 and a 176-byte free remainder is split off in front of its header.
        alignas(256) static crd::u8 pool[bytes];
        mem::TlsfAllocator a(pool, sizeof(pool), "poison-tlsf-aligned");
        fill(a.allocate(16U, 16U), 16U);
        auto* const q = static_cast<crd::u8*>(a.allocate(64U, 256U));
        if (q != pool + 256)
        {
            std::_Exit(95);
        }
        fill(q, 64U);
        touch(q - 17, 0U); // below q's header: the last byte of the free leading remainder
        return true;
    }
    if (std::strcmp(mode, "tlsf-large") == 0)
    {
        mem::TlsfAllocator a(4U * bytes, nullptr, "poison-tlsf");
        void* const p = a.allocate(bytes, 16U);
        fill(a.allocate(64U, 16U), 64U);
        fill(p, bytes);
        a.deallocate(p);
        touch(p, bytes / 2U);
        return true;
    }
    if (std::strcmp(mode, "tlsf-shrink") == 0)
    {
        mem::TlsfAllocator a(bytes, nullptr, "poison-tlsf");
        void* const p = a.allocate(256U, 16U);
        fill(a.allocate(64U, 16U), 64U);
        fill(p, 256U);
        void* const q = a.reallocate(p, 256U, 32U, 16U);
        if (q != p)
        {
            std::_Exit(95); // the shrink did not stay in place
        }
        touch(q, 32U);
        return true;
    }
    if (std::strcmp(mode, "gtlsf-freed") == 0)
    {
        mem::GrowableTlsfAllocator a(4096U, nullptr, "poison-gtlsf");
        fill(a.allocate(3000U, 16U), 3000U);    // the first chunk is sized for this request (8 KiB)
        void* const p = a.allocate(6000U, 16U); // more than the first chunk has left
        fill(a.allocate(64U, 16U), 64U);
        if (a.num_chunks() < 2U)
        {
            std::_Exit(95);
        }
        fill(p, 6000U);
        a.deallocate(p);
        touch(p, 1024U);
        return true;
    }
    return run_ring_mode(mode);
}

// Two adjacent slots of one pool: frees the upper (or the lower) one and returns the other. Exits 95 when the pool
// did not hand out neighbours, which the test rejects like any exit other than 42.
template <typename Pool>
crd::u8* keep_one_of_two_neighbours(Pool& pool, crd::usize size, crd::usize alignment, crd::usize stride,
                                    bool keep_lower)
{
    auto* const a = static_cast<crd::u8*>(pool.allocate(size, alignment));
    auto* const b = static_cast<crd::u8*>(pool.allocate(size, alignment));
    crd::u8* const lower = a < b ? a : b;
    crd::u8* const upper = a < b ? b : a;
    if (lower == nullptr || upper - lower != static_cast<std::ptrdiff_t>(stride))
    {
        std::_Exit(95);
    }
    fill(lower, size);
    fill(upper, size);
    pool.deallocate(keep_lower ? upper : lower);
    return keep_lower ? lower : upper;
}

[[nodiscard]] bool run_pool_mode(const char* mode)
{
    if (std::strcmp(mode, "pool-freed-slot") == 0)
    {
        mem::PoolAllocator a(64U, 8U, 16U, nullptr, "poison-pool");
        void* const p = a.allocate(64U, 16U);
        fill(p, 64U);
        a.deallocate(p);
        touch(p, 0U); // the free-list header itself
        return true;
    }
    if (std::strcmp(mode, "pool-overrun") == 0)
    {
        mem::PoolAllocator a(48U, 8U, 16U, nullptr, "poison-pool");
        touch(keep_one_of_two_neighbours(a, 48U, 16U, 48U, true), 48U);
        return true;
    }
    if (std::strcmp(mode, "pool-underrun") == 0)
    {
        mem::PoolAllocator a(48U, 8U, 16U, nullptr, "poison-pool");
        const crd::u8* const upper = keep_one_of_two_neighbours(a, 48U, 16U, 48U, false);
        touch(upper - 1, 0U);
        return true;
    }
    if (std::strcmp(mode, "pool-odd-freed") == 0)
    {
        mem::PoolAllocator a(4099U, 4U, 8U, nullptr, "poison-pool-odd");
        void* const p = a.allocate(4099U, 8U);
        fill(p, 4099U);
        a.deallocate(p);
        touch(p, 4098U);
        return true;
    }
    if (std::strcmp(mode, "pool-odd-overrun") == 0)
    {
        mem::PoolAllocator a(4099U, 4U, 8U, nullptr, "poison-pool-odd");
        touch(keep_one_of_two_neighbours(a, 4099U, 8U, 4104U, true), 4104U);
        return true;
    }
    if (std::strcmp(mode, "gpool-freed-slot") == 0)
    {
        mem::GrowablePoolAllocator a(64U, 16U, 8U, nullptr, "poison-gpool");
        void* const p = a.allocate(64U, 16U);
        fill(p, 64U);
        a.deallocate(p);
        touch(p, 0U);
        return true;
    }
    if (std::strcmp(mode, "gpool-overrun") == 0)
    {
        mem::GrowablePoolAllocator a(48U, 16U, 8U, nullptr, "poison-gpool");
        touch(keep_one_of_two_neighbours(a, 48U, 16U, 48U, true), 48U);
        return true;
    }
    return run_tlsf_mode(mode);
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
#if CRD_DIAG_HAS_ASAN
    __sanitizer_set_death_callback(&on_asan_death);
#endif
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the bad read returned: under ASan the boundary is missing, and the test's oracle (42) rejects it.
    return run_pool_mode(argv[1]) ? 0 : 96;
}
