#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/asan_poison.hpp>
#include <crd/memory/checked_math.hpp>
#include <crd/memory/log_channel.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace crd::memory
{
namespace
{
// Every block is one C-library malloc with a 16-byte header just below the pointer handed out (DIAG.11b (p)):
//
//   raw              user - 16                 user                       user + size           raw + total
//   | leading pad    | header: size, layout   | the caller's `size` bytes | trailing pad          |
//
// The header holds the requested size (what the statistics count on both sides and what allocation_size() answers)
// and the layout word: the offset from raw to user, with log2(alignment) in its top byte. malloc already returns
// kMallocAlignment-aligned memory, so a request aligned at most to that needs no pad; a larger alignment reserves
// alignment - kMallocAlignment bytes and splits them between the two pads. Under AddressSanitizer everything outside
// the caller's bytes is poisoned, so an under- or overrun is reported as before (use-after-poison where it lands in
// the header or a pad), and the header is read through an unchecked load (CRD_MEM_ASAN_UNCHECKED).
constexpr usize kHeaderBytes = 16U;
constexpr usize kSizeWord = 0U;   // header word: the requested size
constexpr usize kLayoutWord = 1U; // header word: offset | log2(alignment) << kLog2Shift
constexpr usize kHeaderWords = 2U;
constexpr u64 kLog2Shift = 56U;
constexpr u64 kOffsetMask = (u64{1} << kLog2Shift) - 1U;
constexpr u64 kMaxAlignmentLog2 = 55U; // the offset (at most the alignment) must fit below the top byte
constexpr auto kMinAlignmentLog2 = static_cast<u64>(std::countr_zero(u64{kMinAlignment}));
constexpr usize kMallocAlignment = alignof(std::max_align_t);

static_assert(kHeaderWords * sizeof(u64) == kHeaderBytes);
static_assert(kMallocAlignment >= alignof(u64) && kHeaderBytes % kMallocAlignment == 0U,
              "the header must keep raw + 16 at malloc's alignment");
static_assert(kMinAlignment >= alignof(u64), "the header words are stored at the block's alignment");

// The bytes a request reserves beyond its size and the header for its alignment.
[[nodiscard]] constexpr usize padding_budget(usize alignment) noexcept
{
    return alignment > kMallocAlignment ? alignment - kMallocAlignment : 0U;
}

[[nodiscard]] u64* header_words(void* user) noexcept
{
    return static_cast<u64*>(user) - kHeaderWords;
}

struct BlockHeader
{
    u64 size;
    u64 layout;
};

// The header lies in poisoned bytes: read it without the sanitizer's check, as plain loads (a memcpy would be
// intercepted and checked). Every other access to the block stays checked.
CRD_MEM_ASAN_UNCHECKED BlockHeader read_header(const void* user) noexcept
{
    const u64* const words = static_cast<const u64*>(user) - kHeaderWords;
    return BlockHeader{words[kSizeWord], words[kLayoutWord]};
}

// The offset from the C-library block to `user`, after checking that the header is one this allocator wrote: a
// non-zero size, an alignment `user` has, an offset inside that alignment's budget and a malloc-aligned block below
// it. A header that fails (an underwrite of the block, or bytes below a pointer that hold no header of this
// allocator's) is a named fatal in every build rather than a wild pointer handed to free(). It is a check, not
// ownership: reading below a pointer this allocator never handed out may itself fault.
[[nodiscard]] usize checked_offset(const void* user, const BlockHeader& header) noexcept
{
    const u64 log2 = header.layout >> kLog2Shift;
    const u64 offset = header.layout & kOffsetMask;
    bool valid = header.size != 0U && log2 >= kMinAlignmentLog2 && log2 <= kMaxAlignmentLog2;
    if (valid)
    {
        const usize alignment = usize{1} << log2;
        const auto address = reinterpret_cast<std::uintptr_t>(user);
        valid = (address & (alignment - 1U)) == 0U && offset >= kHeaderBytes &&
                offset - kHeaderBytes <= padding_budget(alignment) && offset <= address &&
                ((address - offset) & (kMallocAlignment - 1U)) == 0U;
    }
    if (!valid)
    {
        CRD_LOG_CRITICAL(g_log_memory, "MallocAllocator: block {} has a corrupt header (size {}, layout {:#x})", user,
                         header.size, header.layout);
        CRD_FATAL("MallocAllocator: block header failed its check");
    }
    return static_cast<usize>(offset);
}
} // namespace

MallocAllocator::MallocAllocator() noexcept : MallocAllocator("MallocAllocator") {}

MallocAllocator::MallocAllocator(const char* name) noexcept
{
    m_name = name;
    m_stats.keep_counts();
}

void* MallocAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size > 0);
    void* p = try_allocate(size, alignment);
    if (!p)
    {
        CRD_LOG_CRITICAL(g_log_memory, "MallocAllocator OOM (requested {} bytes, alignment {})", size, alignment);
        CRD_FATAL("Out of memory");
    }
    return p;
}

void* MallocAllocator::try_allocate(usize size, usize alignment)
{
    CRD_ASSERT(is_pow2(alignment));
    if (size == 0)
    {
        return nullptr;
    }
    if (alignment < kMinAlignment)
    {
        alignment = kMinAlignment;
    }
    const auto log2 = static_cast<u64>(std::countr_zero(static_cast<u64>(alignment)));
    usize total = 0;
    // Checked: a huge size or alignment is refused, never wrapped into a small block (DIAG.3a).
    if (log2 > kMaxAlignmentLog2 || !checked_add(size, kHeaderBytes + padding_budget(alignment), &total))
    {
        return nullptr;
    }
    auto* const raw = static_cast<u8*>(std::malloc(total));
    if (raw == nullptr)
    {
        return nullptr;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(raw) + kHeaderBytes;
    const auto lead = static_cast<usize>((alignment - (first & (alignment - 1U))) & (alignment - 1U));
    if (lead > padding_budget(alignment))
    {
        std::free(raw);
        CRD_FATAL("MallocAllocator: malloc returned a block below alignof(std::max_align_t)");
    }
    const usize offset = kHeaderBytes + lead;
    u8* const user = raw + offset;
    u64* const words = header_words(user);
    words[kSizeWord] = static_cast<u64>(size);
    words[kLayoutWord] = static_cast<u64>(offset) | (log2 << kLog2Shift);
    asan_poison(raw, offset);
    asan_poison(user + size, total - offset - size);
    m_stats.on_allocate(size);
    return user;
}

void MallocAllocator::deallocate(void* p) noexcept
{
    if (!p)
    {
        return;
    }
    const BlockHeader header = read_header(p);
    const usize offset = checked_offset(p, header);
    // Under ASan the sanitizer's free poisons the whole block as freed, so the header and pads need no unpoisoning.
    std::free(static_cast<u8*>(p) - offset);
    m_stats.on_deallocate(header.size);
}

bool MallocAllocator::owns(const void* p) const noexcept
{
    // We cannot tell. Convention: malloc-style allocators trust the caller.
    // Composite allocators that need real ownership tests should use a
    // range-based wrapper.
    return p != nullptr;
}

usize MallocAllocator::allocation_size(const void* p) const noexcept
{
    if (!p)
    {
        return 0;
    }
    const BlockHeader header = read_header(p);
    (void)checked_offset(p, header);
    return static_cast<usize>(header.size);
}
} // namespace crd::memory
