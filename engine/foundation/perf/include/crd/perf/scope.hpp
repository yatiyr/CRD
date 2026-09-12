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

namespace crd::perf
{

#if CRD_PERF_ENABLED

class ScopedRegion
{
public:
    explicit ScopedRegion(NameId id, Category cat = Category::User,
                          crd::u32 color_rgba = 0U) noexcept
        : m_id(id), m_cat(cat), m_color(color_rgba), m_begin(push_region(id, cat, color_rgba))
    {
    }

    ~ScopedRegion() noexcept
    {
        pop_region(m_id, m_begin, m_cat, m_color);
    }

    ScopedRegion(const ScopedRegion&)            = delete;
    ScopedRegion& operator=(const ScopedRegion&) = delete;
    ScopedRegion(ScopedRegion&&)                 = delete;
    ScopedRegion& operator=(ScopedRegion&&)      = delete;

private:
    NameId     m_id;
    Category   m_cat;
    crd::u32   m_color;
    BeginToken m_begin;
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

#define CRD_PERF_DETAIL_CAT_INNER(a, b) a##b
#define CRD_PERF_DETAIL_CAT(a, b) CRD_PERF_DETAIL_CAT_INNER(a, b)

#if CRD_PERF_ENABLED

// CRD_PERF_SCOPE("name") -- declare a ScopedRegion in the enclosing
// scope. The NameId is interned once per call site via a TU-local static.
//
// Cost on the hot path:
//   - first hit:  one intern_name() call (mutex on the cold path)
//   - subsequent: one indirect-branch-predictable load of the static
//   - push_region: one MonotonicClock::now() + one thread_local ring write
//   - pop_region: one MonotonicClock::now() + one thread_local ring write
//
// `name` MUST be a string literal (or a static-storage const char*). The
// macro takes the address of the literal as the intern key; passing a
// non-static buffer is undefined behaviour.
#define CRD_PERF_SCOPE(name_literal)                                                                   \
    static const ::crd::perf::NameId CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__) =                  \
        ::crd::perf::intern_name(name_literal);                                                        \
    ::crd::perf::ScopedRegion CRD_PERF_DETAIL_CAT(_crd_perf_scope_, __LINE__)                          \
    {                                                                                                  \
        CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__)                                                 \
    }

#define CRD_PERF_SCOPE_CATEGORY(name_literal, category)                                                \
    static const ::crd::perf::NameId CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__) =                  \
        ::crd::perf::intern_name(name_literal);                                                        \
    ::crd::perf::ScopedRegion CRD_PERF_DETAIL_CAT(_crd_perf_scope_, __LINE__)                          \
    {                                                                                                  \
        CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__), (category)                                     \
    }

#define CRD_PERF_SCOPE_COLOR(name_literal, color_rgba)                                                 \
    static const ::crd::perf::NameId CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__) =                  \
        ::crd::perf::intern_name(name_literal);                                                        \
    ::crd::perf::ScopedRegion CRD_PERF_DETAIL_CAT(_crd_perf_scope_, __LINE__)                          \
    {                                                                                                  \
        CRD_PERF_DETAIL_CAT(_crd_perf_name_, __LINE__), ::crd::perf::Category::User, (color_rgba)      \
    }

#define CRD_PERF_FRAME_MARK() ::crd::perf::frame_mark()

#else // CRD_PERF_ENABLED

#define CRD_PERF_SCOPE(name_literal) ((void)0)
#define CRD_PERF_SCOPE_CATEGORY(name_literal, category) ((void)0)
#define CRD_PERF_SCOPE_COLOR(name_literal, color_rgba) ((void)0)
#define CRD_PERF_FRAME_MARK() ((void)0)

#endif // CRD_PERF_ENABLED
