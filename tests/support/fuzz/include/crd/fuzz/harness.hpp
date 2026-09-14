#pragma once
// The bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// A fuzz target is one translation unit that defines
//     extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
//     void crd_fuzz_seeds(crd::fuzz::SeedSink& sink);
// and is built twice by crd_fuzz_target(): the REPLAY executable (replay_main.cpp walks corpus directories, feeds
// every file to the target and exits non-zero on any failure; registered as the <target>-corpus CTest on every lane)
// and, under CRD_ENABLE_FUZZER with clang, the libFuzzer executable from the same source.
//
// Bounds. kMaxInputBytes: a target returns early above it. BudgetAllocator: a root allocator per input whose
// allocations beyond the budget end in a fatal with a distinctive message (a reported finding with a small footprint,
// not an RSS kill). The fuzzing side adds libFuzzer's -max_len, -timeout, -rss_limit_mb and -malloc_limit_mb
// (scripts/fuzz.py). Oracles: CRD_FUZZ_REQUIRE aborts with file and line; an accepted input must satisfy the same
// round-trip the unit tests use, so a fuzz finding is a loader or serializer defect, never a harness opinion.
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace crd::fuzz
{
[[noreturn]] inline void fatal_exit() noexcept;

inline constexpr usize kMaxInputBytes     = 64U * 1024U;
inline constexpr usize kDefaultBudgetBytes = 64U * 1024U * 1024U;

// Where a target hands its deterministic seed inputs (replay_main --seed <dir> writes them as corpus files).
class SeedSink
{
public:
    virtual ~SeedSink()                                                    = default;
    virtual void add(const char* name, const u8* data, usize size)         = 0;
    void         add_text(const char* name, const char* text) { add(name, reinterpret_cast<const u8*>(text), std::strlen(text)); }
};

// A root allocator for one input: every block carries a header, live and peak bytes are tracked, and an allocation
// that would exceed the budget is a fatal with a message the replay and libFuzzer artifacts carry verbatim.
class BudgetAllocator final : public memory::IAllocator
{
public:
    explicit BudgetAllocator(usize budget = kDefaultBudgetBytes) noexcept : m_budget(budget)
    {
        m_name = "FuzzBudgetAllocator";
    }
    ~BudgetAllocator() override = default;
    BudgetAllocator(const BudgetAllocator&)            = delete;
    BudgetAllocator& operator=(const BudgetAllocator&) = delete;

    void* allocate(usize size, usize alignment = memory::kDefaultAlignment) override
    {
        void* p = try_allocate(size, alignment);
        if (p == nullptr)
        {
            std::fprintf(stderr, "fuzz: allocation budget exceeded (%zu live + %zu requested > %zu bytes)\n",
                         static_cast<std::size_t>(m_live), static_cast<std::size_t>(size),
                         static_cast<std::size_t>(m_budget));
            std::fflush(stderr);
            fatal_exit();
        }
        return p;
    }

    [[nodiscard]] void* try_allocate(usize size, usize alignment = memory::kDefaultAlignment) override
    {
        if (size == 0U || m_live + size > m_budget)
        {
            return nullptr;
        }
        if (alignment < alignof(Header))
        {
            alignment = alignof(Header);
        }
        const usize total = sizeof(Header) + alignment + size;
        void* const base  = m_inner.try_allocate(total, alignof(Header));
        if (base == nullptr)
        {
            return nullptr;
        }
        const auto  raw     = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
        const auto  aligned = (raw + (alignment - 1U)) & ~static_cast<std::uintptr_t>(alignment - 1U);
        auto* const header  = reinterpret_cast<Header*>(aligned - sizeof(Header));
        header->base        = base;
        header->size        = size;
        header->magic       = kMagic;
        m_live += size;
        if (m_live > m_peak)
        {
            m_peak = m_live;
        }
        return reinterpret_cast<void*>(aligned);
    }

    void deallocate(void* p) noexcept override
    {
        if (p == nullptr)
        {
            return;
        }
        Header* const header = header_of(p);
        CRD_ASSERT(header != nullptr && header->magic == kMagic);
        m_live -= header->size;
        header->magic = 0U;
        m_inner.deallocate(header->base);
    }

    [[nodiscard]] bool owns(const void* p) const noexcept override
    {
        if (p == nullptr)
        {
            return false;
        }
        const Header* const header = header_of(p);
        return header->magic == kMagic && m_inner.owns(header->base);
    }

    [[nodiscard]] usize allocation_size(const void* p) const noexcept override
    {
        return p == nullptr ? 0U : header_of(p)->size;
    }

    [[nodiscard]] usize live_bytes() const noexcept { return m_live; }
    [[nodiscard]] usize peak_bytes() const noexcept { return m_peak; }
    [[nodiscard]] usize budget_bytes() const noexcept { return m_budget; }

private:
    struct Header
    {
        void* base;
        usize size;
        u64   magic;
    };
    static constexpr u64 kMagic = 0x4652'5A5A'4255'4447ULL; // "FRZZBUDG"

    static Header* header_of(void* p) noexcept
    {
        return reinterpret_cast<Header*>(reinterpret_cast<std::uintptr_t>(p) - sizeof(Header));
    }
    static const Header* header_of(const void* p) noexcept
    {
        return reinterpret_cast<const Header*>(reinterpret_cast<std::uintptr_t>(p) - sizeof(Header));
    }

    // Every block goes to the system allocator on purpose: ASan and libFuzzer's malloc limit see each allocation
    // one by one, which a TLSF arena would hide (the first complete-tier run's crd-no-malloc-allocator guard).
    memory::MallocAllocator m_inner; // crd-lint-allow-malloc-allocator: sanitizer visibility per block
    usize                   m_budget;
    usize                   m_live = 0U;
    usize                   m_peak = 0U;
};

// Every harness failure ends the process the way a sanitizer report does (abort, which libFuzzer records as a
// crash) without the Debug CRT's "abort() has been called" dialog that would hang a replay CTest on Windows.
[[noreturn]] inline void fatal_exit() noexcept
{
#if defined(_WIN32)
    _set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

[[noreturn]] inline void oracle_failed(const char* expression, const char* file, int line) noexcept
{
    std::fprintf(stderr, "fuzz: oracle failed: %s (%s:%d)\n", expression, file, line);
    std::fflush(stderr);
    fatal_exit();
}

[[nodiscard]] inline bool bytes_equal(const u8* a, usize a_size, const u8* b, usize b_size) noexcept
{
    return a_size == b_size && (a_size == 0U || std::memcmp(a, b, a_size) == 0);
}
} // namespace crd::fuzz

// An accepted input must satisfy the round-trip oracle; a violation is a finding, reported with its site.
#define CRD_FUZZ_REQUIRE(expression)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expression))                                                                                             \
        {                                                                                                              \
            ::crd::fuzz::oracle_failed(#expression, __FILE__, __LINE__);                                               \
        }                                                                                                              \
    } while (false)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
void           crd_fuzz_seeds(crd::fuzz::SeedSink& sink);
