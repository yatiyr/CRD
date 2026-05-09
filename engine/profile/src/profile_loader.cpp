// Phase 3.0 v1n5 — ProfileLoader implementation (ADR-0060 §5).
//
// Validates the CRDR header (type_fourcc == 'PROF'), parses FINF / FRLE /
// FBND chunks into a ProfileResource owning an Array<Profile>. Hard-fails
// (returns nullptr) on any inconsistency; v1n5 has no soft fallback.

#include <crd/core/assert.hpp>
#include <crd/profile/profile_loader.hpp>
#include <crd/profile/profile_resource.hpp>
#include <crd/resources/crdr.hpp>

#include <cstring>
#include <new>

namespace crd::profile
{

ProfileLoader::ProfileLoader(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc)
{
    CRD_ASSERT_MSG(alloc != nullptr, "ProfileLoader: null allocator");
}

crd::u32 ProfileLoader::type_fourcc() const noexcept
{
    return kFourCC_PROF;
}

namespace
{
[[nodiscard]] bool parse_frle(crd::containers::ConstSpan<crd::u8> bytes,
                              crd::u32                            expected_count,
                              crd::containers::Array<Profile>&    out_profiles)
{
    crd::usize cursor = 0;
    for (crd::u32 r = 0; r < expected_count; ++r)
    {
        // Header: u32 priority + u32 predicate_count = 8 bytes.
        if (cursor + 8U > bytes.size())
        {
            return false;
        }
        crd::u32 priority         = 0;
        crd::u32 predicate_count  = 0;
        std::memcpy(&priority,        bytes.data() + cursor + 0U, sizeof(priority));
        std::memcpy(&predicate_count, bytes.data() + cursor + 4U, sizeof(predicate_count));
        cursor += 8U;

        const crd::usize pred_bytes =
            static_cast<crd::usize>(predicate_count) * sizeof(PredicateRecord);
        if (cursor + pred_bytes > bytes.size())
        {
            return false;
        }

        // out_profiles[r] is already constructed (default Profile with the
        // loader's allocator); fill its fields.
        out_profiles[r].priority = priority;
        out_profiles[r].predicates.resize(static_cast<crd::usize>(predicate_count));
        if (predicate_count > 0U)
        {
            std::memcpy(out_profiles[r].predicates.data(),
                        bytes.data() + cursor,
                        pred_bytes);
        }
        cursor += pred_bytes;
    }
    return cursor == bytes.size();
}

[[nodiscard]] bool parse_fbnd(crd::containers::ConstSpan<crd::u8> bytes,
                              crd::u32                            expected_count,
                              crd::containers::Array<Profile>&    out_profiles)
{
    crd::usize cursor = 0;
    for (crd::u32 b = 0; b < expected_count; ++b)
    {
        // Header: u32 rule_idx + u32 preset_id_count = 8 bytes.
        if (cursor + 8U > bytes.size())
        {
            return false;
        }
        crd::u32 rule_idx        = 0;
        crd::u32 preset_id_count = 0;
        std::memcpy(&rule_idx,        bytes.data() + cursor + 0U, sizeof(rule_idx));
        std::memcpy(&preset_id_count, bytes.data() + cursor + 4U, sizeof(preset_id_count));
        cursor += 8U;

        // v1n5 enforces FBND.rule_idx == its sequence index.
        if (rule_idx != b || rule_idx >= out_profiles.size())
        {
            return false;
        }

        const crd::usize id_bytes =
            static_cast<crd::usize>(preset_id_count) * sizeof(crd::resources::ResourceId);
        if (cursor + id_bytes > bytes.size())
        {
            return false;
        }

        out_profiles[b].apply_bundle.resize(static_cast<crd::usize>(preset_id_count));
        if (preset_id_count > 0U)
        {
            std::memcpy(out_profiles[b].apply_bundle.data(),
                        bytes.data() + cursor,
                        id_bytes);
        }
        cursor += id_bytes;
    }
    return cursor == bytes.size();
}
} // namespace

void* ProfileLoader::load(const crd::resources::LoadContext& ctx)
{
    crd::resources::CrdrFile file{m_alloc};
    if (crd::resources::crdr_read(ctx.bytes, file, m_alloc) != crd::resources::CrdrError::Ok)
    {
        return nullptr;
    }
    if (file.type_fourcc != kFourCC_PROF)
    {
        return nullptr;
    }

    const crd::resources::CrdrChunk* finf = crd::resources::crdr_find_chunk(file, kFourCC_FINF);
    const crd::resources::CrdrChunk* frle = crd::resources::crdr_find_chunk(file, kFourCC_FRLE);
    const crd::resources::CrdrChunk* fbnd = crd::resources::crdr_find_chunk(file, kFourCC_FBND);
    if (finf == nullptr || frle == nullptr || fbnd == nullptr)
    {
        return nullptr;
    }
    if (finf->payload.size() != sizeof(ProfileFileInfo))
    {
        return nullptr;
    }

    ProfileFileInfo info{};
    std::memcpy(&info, finf->payload.data(), sizeof(ProfileFileInfo));

    if (info.schema_version != m_schema_version)
    {
        return nullptr;
    }
    if (info.rule_count != info.bundle_count)
    {
        return nullptr; // v1n5 enforces 1 bundle per rule
    }

    auto* res = static_cast<ProfileResource*>(
        m_alloc->allocate(sizeof(ProfileResource), alignof(ProfileResource)));
    if (res == nullptr)
    {
        return nullptr;
    }
    new (res) ProfileResource(m_alloc);
    res->set_schema_version(info.schema_version);

    // Pre-construct Profile entries with the loader's allocator so the
    // FRLE / FBND parse functions can fill their fields in-place.
    auto& profiles = res->mutable_profiles();
    profiles.reserve(static_cast<crd::usize>(info.rule_count));
    for (crd::u32 i = 0; i < info.rule_count; ++i)
    {
        profiles.emplace_back(m_alloc);
    }

    if (!parse_frle(frle->payload, info.rule_count, profiles))
    {
        res->~ProfileResource();
        m_alloc->deallocate(res);
        return nullptr;
    }
    if (!parse_fbnd(fbnd->payload, info.bundle_count, profiles))
    {
        res->~ProfileResource();
        m_alloc->deallocate(res);
        return nullptr;
    }

    return res;
}

void ProfileLoader::unload(void* payload) noexcept
{
    if (payload == nullptr)
    {
        return;
    }
    auto* res = static_cast<ProfileResource*>(payload);
    res->~ProfileResource();
    m_alloc->deallocate(res);
}

} // namespace crd::profile
