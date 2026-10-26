#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- ScopedRegion RAII + CRD_PERF_SCOPE macro (Detour D-003).
//
// Usage:
//
//   void heavy_work()
//   {
//       CRD_PERF_SCOPE("heavy_work");
//       // ... timed body ...
//   }
//
//   void render_pass()
//   {
//       CRD_PERF_SCOPE_CATEGORY("render::geometry", crd::perf::Category::Pass);
//       // ...
//   }
//
//   void custom_color()
//   {
//       // 0xAARRGGBB premultiplied
//       CRD_PERF_SCOPE_COLOR("hot_inner_loop", 0xFFFF0000U);
//       // ...
//   }
//
// When CRD_PERF_ENABLED=0 every macro collapses to `((void)0)` and produces
// byte-identical codegen to a hand-deleted call site. Verified by the v0a
// objdump-equality test.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

#include <atomic>

namespace crd::perf
{

#if CRD_PERF_ENABLED

namespace detail
{
// A zone site's name (CRD_PERF_SCOPE): interned on the site's first pass while a profiler runs and cached after. Its
// constructor is constexpr, so the site's static is constant-initialised: a pass reads no guard of a thread-safe local
// static (a zone with the profiler off spent its difference against Tracy's with no client there, DIAG.11b clause
// (8)), and a site first reached before init() is named once a profiler runs (a static initialised by intern_name
// while none ran kept kInvalidNameId for good). A refusal by a running profiler (its name table full) is cached as
// one, so the site does not retry at every pass.
class SiteName
{
public:
    explicit constexpr SiteName(const char* literal) noexcept : m_literal(literal) {}

    [[nodiscard]] NameId get() noexcept
    {
        const crd::u32 cached = m_id.load(std::memory_order_relaxed);
        if (cached != kUnset)
        {
            return NameId{cached == kRefused ? kInvalidNameId.value : cached};
        }
        const NameId interned = intern_name(m_literal);
        if (interned.value != kInvalidNameId.value)
        {
            m_id.store(interned.value, std::memory_order_relaxed);
        }
        else if (profiler_running())
        {
            m_id.store(kRefused, std::memory_order_relaxed);
        }
        return interned;
    }

private:
    static constexpr crd::u32 kUnset   = kInvalidNameId.value;
    static constexpr crd::u32 kRefused = kInvalidNameId.value - 1U;

    const char*           m_literal;
    std::atomic<crd::u32> m_id{kUnset};
};
} // namespace detail

class ScopedRegion
{
public:
    // With no profiler running a zone makes no call; one whose push recorded nothing (its thread has no ring) pops
    // nothing either (begin_thread stays the invalid index).
    explicit ScopedRegion(NameId id, Category cat = Category::User,
                          crd::u32 color_rgba = 0U) noexcept
        : m_id(id), m_cat(cat), m_color(color_rgba),
          m_begin(detail::profiler_running() ? push_region(id, cat, color_rgba) : BeginToken{})
    {
    }

    // A CRD_PERF_SCOPE site: its name is looked at only while a profiler runs.
    explicit ScopedRegion(detail::SiteName& site, Category cat = Category::User, crd::u32 color_rgba = 0U) noexcept
        : m_cat(cat), m_color(color_rgba)
    {
        if (detail::profiler_running())
        {
            m_id    = site.get();
            m_begin = push_region(m_id, cat, color_rgba);
        }
    }

    ~ScopedRegion() noexcept
    {
        if (m_begin.begin_thread != kNoBeginThread)
        {
            pop_region(m_id, m_begin, m_cat, m_color);
        }
    }

    ScopedRegion(const ScopedRegion&)            = delete;
    ScopedRegion& operator=(const ScopedRegion&) = delete;
    ScopedRegion(ScopedRegion&&)                 = delete;
    ScopedRegion& operator=(ScopedRegion&&)      = delete;

private:
    static constexpr crd::u8 kNoBeginThread = BeginToken{}.begin_thread;

    NameId     m_id{};
    Category   m_cat;
    crd::u32   m_color;
    BeginToken m_begin{};
};

#else

// When the gate is off, the type still exists (so headers can reference
// it generically), but its body is empty -- the constructor evaluates no
// expressions and the destructor is trivial. With -O1 or higher every
// compiler removes the local variable entirely.
class ScopedRegion
{
public:
    explicit ScopedRegion(NameId, Category = Category::User, crd::u32 = 0U) noexcept {}
    ~ScopedRegion() noexcept = default;

    ScopedRegion(const ScopedRegion&)            = delete;
    ScopedRegion& operator=(const ScopedRegion&) = delete;
    ScopedRegion(ScopedRegion&&)                 = delete;
    ScopedRegion& operator=(ScopedRegion&&)      = delete;
};

#endif

} // namespace crd::perf

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

// NOLINTBEGIN(cppcoreguidelines-macro-usage): token pasting, and sites that declare a static at the call site and
// compile out to nothing, are what only a macro does.
#define CRD_PERF_DETAIL_CAT_INNER(a, b) a##b
#define CRD_PERF_DETAIL_CAT(a, b) CRD_PERF_DETAIL_CAT_INNER(a, b)

#if CRD_PERF_ENABLED

// CRD_PERF_SCOPE("name") -- declare a ScopedRegion in the enclosing
// scope. The name is interned once per call site, on its first pass while
// a profiler runs, into a constant-initialised TU-local static (SiteName).
//
// Cost on the hot path:
//   - profiler off: one relaxed load and a branch (no call)
//   - first pass while running: one intern_name() call (mutex on the cold path)
//   - subsequent: one load of the cached id
//   - push_region: one TickClock::now_ns() + one thread_local ring write
//   - pop_region: one TickClock::now_ns() + one thread_local ring write
//
// `name` MUST be a string literal (or a static-storage const char*). The
// macro takes the address of the literal as the intern key; passing a
// non-static buffer is undefined behaviour.
#define CRD_PERF_SCOPE(name_literal)                                                                                   \
    static ::crd::perf::detail::SiteName CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__){name_literal};                 \
    ::crd::perf::ScopedRegion CRD_PERF_DETAIL_CAT(_crd_perf_scope_, __LINE__)                                          \
    {                                                                                                                  \
        CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__)                                                                 \
    }

#define CRD_PERF_SCOPE_CATEGORY(name_literal, category)                                                                \
    static ::crd::perf::detail::SiteName CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__){name_literal};                 \
    ::crd::perf::ScopedRegion CRD_PERF_DETAIL_CAT(_crd_perf_scope_, __LINE__)                                          \
    {                                                                                                                  \
        CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__), (category)                                                     \
    }

#define CRD_PERF_SCOPE_COLOR(name_literal, color_rgba)                                                                 \
    static ::crd::perf::detail::SiteName CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__){name_literal};                 \
    ::crd::perf::ScopedRegion CRD_PERF_DETAIL_CAT(_crd_perf_scope_, __LINE__)                                          \
    {                                                                                                                  \
        CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__), ::crd::perf::Category::User, (color_rgba)                      \
    }

#define CRD_PERF_FRAME_MARK() ::crd::perf::frame_mark()

#else // CRD_PERF_ENABLED

#define CRD_PERF_SCOPE(name_literal) ((void)0)
#define CRD_PERF_SCOPE_CATEGORY(name_literal, category) ((void)0)
#define CRD_PERF_SCOPE_COLOR(name_literal, color_rgba) ((void)0)
#define CRD_PERF_FRAME_MARK() ((void)0)

#endif // CRD_PERF_ENABLED
// NOLINTEND(cppcoreguidelines-macro-usage)
