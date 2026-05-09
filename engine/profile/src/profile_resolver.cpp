// Phase 3.0 v1n6 — ProfileResolver + predicate evaluation + context detection.

#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/profile/profile_resolver.hpp>

#include <algorithm>
#include <thread>

namespace crd::profile
{

namespace
{
// Read the relevant ProfileContext axis as a canonical signed 64-bit
// integer so signed comparands and unsigned-enum field values share a
// single comparison path.
[[nodiscard]] crd::i64 read_ctx_field_signed(PredicateField        field,
                                             const ProfileContext& ctx) noexcept
{
    switch (field)
    {
        case PredicateField::Os:
            return static_cast<crd::i64>(static_cast<crd::u8>(ctx.os));
        case PredicateField::GpuTier:
            return static_cast<crd::i64>(static_cast<crd::u8>(ctx.gpu_tier));
        case PredicateField::Domain:
            return static_cast<crd::i64>(static_cast<crd::u8>(ctx.domain));
        case PredicateField::Mode:
            return static_cast<crd::i64>(static_cast<crd::u8>(ctx.mode));
        case PredicateField::TargetFps:
            return static_cast<crd::i64>(ctx.target_fps);
        case PredicateField::CpuCores:
            return static_cast<crd::i64>(ctx.cpu_cores);
    }
    return 0;
}

// Reinterpret the predicate's u32 value as i32 (preserves sign for integer
// fields). Enum fields store positive small integers, so the sign bit is
// never set — the bitcast is a no-op there.
[[nodiscard]] crd::i64 predicate_value_signed(crd::u32 packed) noexcept
{
    crd::i32 as_signed = 0;
    static_assert(sizeof(as_signed) == sizeof(packed));
    std::memcpy(&as_signed, &packed, sizeof(packed));
    return static_cast<crd::i64>(as_signed);
}
} // namespace

bool evaluate_predicate(const PredicateRecord& record,
                        const ProfileContext&  ctx) noexcept
{
    const crd::i64 lhs = read_ctx_field_signed(record.field, ctx);

    switch (record.op)
    {
        case PredicateOp::Equal:
            return lhs == predicate_value_signed(record.value);
        case PredicateOp::GreaterEq:
            return lhs >= predicate_value_signed(record.value);
        case PredicateOp::LessEq:
            return lhs <= predicate_value_signed(record.value);
        case PredicateOp::InMask:
        {
            // Only meaningful for enum fields whose values stay small. v1n5's
            // enum schema (≤ 5 values) fits comfortably in a u32 mask.
            if (lhs < 0 || lhs >= 32)
            {
                return false;
            }
            const crd::u32 bit = 1U << static_cast<crd::u32>(lhs);
            return (record.value & bit) != 0U;
        }
    }
    return false;
}

ProfileResolver::ProfileResolver(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc)
{
    CRD_ASSERT_MSG(alloc != nullptr, "ProfileResolver: null allocator");
}

void ProfileResolver::set_resource(const ProfileResource* resource) noexcept
{
    m_resource = resource;
}

crd::u32 ProfileResolver::resolve(
    const ProfileContext&                                ctx,
    crd::containers::Array<crd::resources::ResourceId>&  out) const
{
    out.clear();
    if (m_resource == nullptr)
    {
        return 0U;
    }

    const auto profiles = m_resource->profiles();
    if (profiles.empty())
    {
        return 0U;
    }

    // Collect indices of matching profiles into a small scratch array.
    struct Match
    {
        crd::u32 priority;
        crd::u32 file_idx;
    };
    crd::containers::Array<Match> matches(m_alloc);
    matches.reserve(profiles.size());

    for (crd::u32 i = 0; i < static_cast<crd::u32>(profiles.size()); ++i)
    {
        const auto& p = profiles[i];
        bool all_match = true;
        for (const auto& pred : p.predicates)
        {
            if (!evaluate_predicate(pred, ctx))
            {
                all_match = false;
                break;
            }
        }
        if (all_match)
        {
            matches.push_back(Match{p.priority, i});
        }
    }

    // Stable sort by priority ascending — tie-break falls back to FRLE
    // byte order (the file_idx) per ADR-0060 §7.
    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) noexcept {
                         return a.priority < b.priority;
                     });

    // Concatenate matched bundles in priority order. No deduplication —
    // duplicate IDs across bundles are intentionally preserved; the
    // consumer's apply step is idempotent and the LAST application wins.
    for (const auto& m : matches)
    {
        const auto& bundle = profiles[m.file_idx].apply_bundle;
        for (const auto& id : bundle)
        {
            out.push_back(id);
        }
    }

    return static_cast<crd::u32>(matches.size());
}

OperatingSystem detect_os() noexcept
{
#if CRD_OS_WINDOWS
    return OperatingSystem::Windows;
#elif CRD_OS_LINUX
    return OperatingSystem::Linux;
#elif CRD_OS_MACOS
    return OperatingSystem::MacOS;
#else
    return OperatingSystem::Unknown;
#endif
}

crd::i32 detect_cpu_cores() noexcept
{
    const unsigned int cores = std::thread::hardware_concurrency();
    return cores == 0U ? 1 : static_cast<crd::i32>(cores);
}

} // namespace crd::profile
