#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/profile/profile.hpp>
#include <crd/resources/crdr.hpp>

namespace crd::profile
{
// Phase 3.0 v1n5 — Profile substrate (ADR-0060 §5).
//
// CRDR container with type_fourcc = 'PROF' and three chunks (sorted ascending
// by FourCC at write time per CRDR convention — on disk: FBND < FINF < FRLE):
//
//   'FBND' (variable) — per-bundle entries:
//                         repeating { u32 rule_idx; u32 preset_id_count;
//                                     ResourceId preset_ids[preset_id_count]; }
//                       Bundles are written in FRLE rule order; rule_idx
//                       cross-links them back. v1n5 enforces 1 bundle per
//                       rule (FBND.rule_idx == its sequence index).
//
//   'FINF' (16 bytes) — ProfileFileInfo: schema_version + rule_count +
//                       bundle_count + flags. rule_count == bundle_count
//                       in v1n5 (one bundle per rule).
//
//   'FRLE' (variable) — per-rule entries:
//                         repeating { u32 priority; u32 predicate_count;
//                                     PredicateRecord predicates[predicate_count]; }
//                       Order is canonical (cooker write order); the
//                       resolver sorts by priority at evaluation time.
//
// Determinism (ADR-0060 §7):
//   - Chunk order is FourCC-sorted by CrdrWriter.
//   - Rule order is canonical (matches cooker emit order; tie-break for
//     equal-priority profiles falls back to this).
//   - Predicate evaluation order within a rule is FRLE byte order.
//   - Same source files + same registration → bit-exact CRDR bytes.

inline constexpr crd::u32 kFourCC_FINF = crd::resources::make_fourcc('F', 'I', 'N', 'F');
inline constexpr crd::u32 kFourCC_FRLE = crd::resources::make_fourcc('F', 'R', 'L', 'E');
inline constexpr crd::u32 kFourCC_FBND = crd::resources::make_fourcc('F', 'B', 'N', 'D');
inline constexpr crd::u32 kFourCC_PROF = crd::resources::make_fourcc('P', 'R', 'O', 'F');

// FINF chunk payload (16 bytes; pinned for binary stability).
struct ProfileFileInfo
{
    crd::u32 schema_version{};   // matches Profile schema; v1n5 = 1
    crd::u32 rule_count{};       // number of FRLE entries
    crd::u32 bundle_count{};     // number of FBND entries (== rule_count in v1n5)
    crd::u32 flags{};            // reserved; v1n5 always 0
};
static_assert(sizeof(ProfileFileInfo) == 16,
              "ProfileFileInfo size pinned at 16 bytes for profile schema v1");

// Type-erased preset payload owned by ResourceManager after a load.
class ProfileResource
{
public:
    explicit ProfileResource(crd::memory::IAllocator* a) : m_profiles(a) {}

    ProfileResource(const ProfileResource&)            = delete;
    ProfileResource& operator=(const ProfileResource&) = delete;
    ProfileResource(ProfileResource&&) noexcept        = default;
    ProfileResource& operator=(ProfileResource&&) noexcept = default;
    ~ProfileResource()                                 = default;

    [[nodiscard]] crd::u32 schema_version() const noexcept { return m_schema_version; }

    [[nodiscard]] crd::containers::ConstSpan<Profile> profiles() const noexcept
    {
        return crd::containers::ConstSpan<Profile>{m_profiles.data(), m_profiles.size()};
    }

    // Mutators — used by ProfileLoader during deserialisation. Public to
    // keep the loader free of friend declarations; consumers should treat
    // the resource as read-only after load.
    void                                          set_schema_version(crd::u32 v) noexcept { m_schema_version = v; }
    [[nodiscard]] crd::containers::Array<Profile>& mutable_profiles() noexcept { return m_profiles; }

private:
    crd::u32                          m_schema_version = 1U;
    crd::containers::Array<Profile>   m_profiles;
};

} // namespace crd::profile
