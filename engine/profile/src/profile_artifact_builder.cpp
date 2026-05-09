// Phase 3.0 v1n5 — ProfileArtifactBuilder implementation (ADR-0060 §5).
//
// Emits a CRDR blob with the FINF / FRLE / FBND chunks. CrdrWriter sorts
// chunks by FourCC at finish() time (FBND < FINF < FRLE on disk), so the
// byte order is deterministic.

#include <crd/core/assert.hpp>
#include <crd/profile/profile_artifact_builder.hpp>
#include <crd/profile/profile_resource.hpp>
#include <crd/resources/crdr.hpp>

#include <cstring>
#include <utility>

namespace crd::profile
{

ProfileArtifactBuilder::ProfileArtifactBuilder(crd::memory::IAllocator*    alloc,
                                               crd::u32                    schema_version,
                                               crd::resources::ResourceId  id) noexcept
    : m_alloc(alloc)
    , m_schema_version(schema_version)
    , m_id(id)
    , m_rules(alloc)
{
    CRD_ASSERT_MSG(alloc != nullptr,        "ProfileArtifactBuilder: null allocator");
    CRD_ASSERT_MSG(schema_version >= 1U,    "ProfileArtifactBuilder: schema_version < 1");
}

void ProfileArtifactBuilder::add_rule(
    crd::u32                                                  priority,
    crd::containers::ConstSpan<PredicateRecord>               predicates,
    crd::containers::ConstSpan<crd::resources::ResourceId>    apply_bundle)
{
    PendingRule pending{m_alloc};
    pending.priority = priority;

    pending.predicates.resize(predicates.size());
    if (!predicates.empty())
    {
        std::memcpy(pending.predicates.data(), predicates.data(),
                    predicates.size() * sizeof(PredicateRecord));
    }

    pending.apply_bundle.resize(apply_bundle.size());
    if (!apply_bundle.empty())
    {
        std::memcpy(pending.apply_bundle.data(), apply_bundle.data(),
                    apply_bundle.size() * sizeof(crd::resources::ResourceId));
    }

    m_rules.push_back(std::move(pending));
}

crd::containers::Array<crd::u8> ProfileArtifactBuilder::build() const
{
    crd::resources::CrdrWriter writer{m_alloc, m_id, kFourCC_PROF};

    const crd::u32 rule_count = static_cast<crd::u32>(m_rules.size());

    // FINF (16 bytes).
    ProfileFileInfo info{};
    info.schema_version = m_schema_version;
    info.rule_count     = rule_count;
    info.bundle_count   = rule_count; // v1n5: 1 bundle per rule
    info.flags          = 0U;

    writer.add_chunk(kFourCC_FINF,
                     crd::containers::ConstSpan<crd::u8>{
                         reinterpret_cast<const crd::u8*>(&info), sizeof(info)});

    // FRLE — concatenated rule entries.
    {
        // Compute total size up front so we can build a flat byte buffer.
        crd::usize total = 0U;
        for (const auto& r : m_rules)
        {
            total += 8U + r.predicates.size() * sizeof(PredicateRecord);
        }
        crd::containers::Array<crd::u8> frle_bytes(m_alloc);
        frle_bytes.resize(total);

        crd::usize cursor = 0U;
        for (const auto& r : m_rules)
        {
            const crd::u32 priority        = r.priority;
            const crd::u32 predicate_count = static_cast<crd::u32>(r.predicates.size());
            std::memcpy(frle_bytes.data() + cursor + 0U, &priority,        sizeof(priority));
            std::memcpy(frle_bytes.data() + cursor + 4U, &predicate_count, sizeof(predicate_count));
            cursor += 8U;
            if (predicate_count > 0U)
            {
                const crd::usize pred_bytes = predicate_count * sizeof(PredicateRecord);
                std::memcpy(frle_bytes.data() + cursor, r.predicates.data(), pred_bytes);
                cursor += pred_bytes;
            }
        }
        CRD_ASSERT_MSG(cursor == total, "ProfileArtifactBuilder: FRLE size mismatch");

        writer.add_chunk(kFourCC_FRLE,
                         crd::containers::ConstSpan<crd::u8>{frle_bytes.data(), frle_bytes.size()});
    }

    // FBND — concatenated bundle entries; rule_idx == sequence index in v1n5.
    {
        crd::usize total = 0U;
        for (const auto& r : m_rules)
        {
            total += 8U + r.apply_bundle.size() * sizeof(crd::resources::ResourceId);
        }
        crd::containers::Array<crd::u8> fbnd_bytes(m_alloc);
        fbnd_bytes.resize(total);

        crd::usize cursor = 0U;
        for (crd::u32 b = 0; b < rule_count; ++b)
        {
            const auto&    r              = m_rules[b];
            const crd::u32 rule_idx       = b;
            const crd::u32 preset_id_count = static_cast<crd::u32>(r.apply_bundle.size());
            std::memcpy(fbnd_bytes.data() + cursor + 0U, &rule_idx,        sizeof(rule_idx));
            std::memcpy(fbnd_bytes.data() + cursor + 4U, &preset_id_count, sizeof(preset_id_count));
            cursor += 8U;
            if (preset_id_count > 0U)
            {
                const crd::usize id_bytes = preset_id_count * sizeof(crd::resources::ResourceId);
                std::memcpy(fbnd_bytes.data() + cursor, r.apply_bundle.data(), id_bytes);
                cursor += id_bytes;
            }
        }
        CRD_ASSERT_MSG(cursor == total, "ProfileArtifactBuilder: FBND size mismatch");

        writer.add_chunk(kFourCC_FBND,
                         crd::containers::ConstSpan<crd::u8>{fbnd_bytes.data(), fbnd_bytes.size()});
    }

    return writer.finish();
}

} // namespace crd::profile
