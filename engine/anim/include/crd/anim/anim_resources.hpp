#pragma once

// anim_resources.hpp — GEO-8 (D-007 row 73): the ANIMATION resources — `SkeletonResource` ('SKEL') and
// `AnimClipResource` ('ANIM'), decomposed from source skins/animations at cook time (never stored as glTF; the
// per-type philosophy). Built for the animation-heavy road ahead (cinematics · gameplay · engineering sim ·
// AI-predicted/Cascadeur-class motion): NOTHING here assumes a humanoid — a skeleton is an arbitrary joint DAG
// of any size (a hand rig, a film face rig, an excavator linkage), and a clip is an arbitrary track set (joint
// TRS + free float tracks for morph weights / future consumers).
//
// ── SKEL ('SKLJ' chunk + 'SKLN' names) ──────────────────────────────────────────────────────────────────────────
// Joints are stored in TOPOLOGICAL order — `parents[i] < i` always (roots: -1), pinned by the COOK — so pose
// composition is a single forward pass, no per-frame sorting, no recursion. Per joint: parent (i32), the REST
// pose local TRS (10 f32: t3 · r4 xyzw · s3 — sampling starts from rest, tracks override), and the inverse bind
// matrix (16 f32, column-major). Names ship in a NUL-string pool — the retarget/debug/AI-tooling key.
//
// ── ANIM ('ANMT' track directory + 'ANMD' f32 blob) ─────────────────────────────────────────────────────────────
// Per track: target joint (0xFFFFFFFF = a free float track), channel (T/R/S/Float), interpolation
// (hesap-interp KeyInterp byte values), component count, key count, and times/values offsets into the shared
// blob. CubicHermite values are glTF's [in_tangent · value · out_tangent] triples VERBATIM — the hesap-interp
// keyframe engine (the ONE curve engine GEO-9 timelines and GEO-10 audio params share) consumes them directly.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::resources
{
class ResourceManager;
}

namespace crd::anim
{

// NOLINTBEGIN(readability-identifier-naming) — repo-wide kFourCC_* mnemonic convention
inline constexpr crd::u32 kFourCC_SKEL = crd::resources::make_fourcc('S', 'K', 'E', 'L');
inline constexpr crd::u32 kFourCC_SklJ = crd::resources::make_fourcc('S', 'K', 'L', 'J');
inline constexpr crd::u32 kFourCC_SklN = crd::resources::make_fourcc('S', 'K', 'L', 'N');
inline constexpr crd::u32 kFourCC_ANIM = crd::resources::make_fourcc('A', 'N', 'I', 'M');
inline constexpr crd::u32 kFourCC_AnmT = crd::resources::make_fourcc('A', 'N', 'M', 'T');
inline constexpr crd::u32 kFourCC_AnmD = crd::resources::make_fourcc('A', 'N', 'M', 'D');
// NOLINTEND(readability-identifier-naming)

inline constexpr crd::u32 kRestFloats = 10U; // t3 + r4 + s3 per joint
inline constexpr crd::u32 kFreeTrack  = 0xFFFFFFFFU; // AnimTrack::target for non-joint float tracks

enum class AnimChannel : crd::u8
{
    Translation = 0,
    Rotation    = 1,
    Scale       = 2,
    Float       = 3, // morph weights today; any scalar consumer tomorrow
};

struct SkeletonResource
{
    crd::containers::Array<crd::i32> parents;       // per joint; parents[i] < i, -1 = root
    crd::containers::Array<crd::f32> rest;          // kRestFloats per joint
    crd::containers::Array<crd::f32> inverse_binds; // 16 per joint, column-major
    crd::containers::Array<crd::u32> name_offsets;  // per joint, into name_pool
    crd::containers::Array<char>     name_pool;     // NUL-separated

    explicit SkeletonResource(crd::memory::IAllocator* a)
        : parents(a), rest(a), inverse_binds(a), name_offsets(a), name_pool(a)
    {
    }
    SkeletonResource(SkeletonResource&&)            = default;
    SkeletonResource& operator=(SkeletonResource&&) = default;

    [[nodiscard]] crd::u32 joint_count() const noexcept { return static_cast<crd::u32>(parents.size()); }
    [[nodiscard]] const char* joint_name(crd::u32 j) const noexcept
    {
        return j < name_offsets.size() ? name_pool.data() + name_offsets[j] : "";
    }
};

// One keyframe track (20-byte directory record — the on-disk 'ANMT' entry layout, memcpy-clean).
struct AnimTrack
{
    crd::u32 target     = kFreeTrack; // topological joint index, or kFreeTrack
    crd::u8  channel    = 0;          // AnimChannel byte value
    crd::u8  interp     = 1;          // hesap::interp::KeyInterp byte value
    crd::u16 components = 0;
    crd::u32 key_count  = 0;
    crd::u32 times_off  = 0; // f32 index into AnimClipResource::data
    crd::u32 values_off = 0;
};
static_assert(sizeof(AnimTrack) == 20, "AnimTrack is the on-disk 'ANMT' record");

struct AnimClipResource
{
    crd::f32                          duration = 0.0F;
    crd::containers::Array<AnimTrack> tracks;
    crd::containers::Array<crd::f32>  data; // all times + values

    explicit AnimClipResource(crd::memory::IAllocator* a) : tracks(a), data(a) {}
    AnimClipResource(AnimClipResource&&)            = default;
    AnimClipResource& operator=(AnimClipResource&&) = default;

    [[nodiscard]] crd::containers::ConstSpan<crd::f32> track_times(const AnimTrack& t) const noexcept
    {
        return {data.data() + t.times_off, t.key_count};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::f32> track_values(const AnimTrack& t) const noexcept
    {
        const crd::usize span = static_cast<crd::usize>(t.interp == 2 ? 3U : 1U) * t.components;
        return {data.data() + t.values_off, static_cast<crd::usize>(t.key_count) * span};
    }
};

// ── cook-side builders (pure data; validated — a malformed skeleton/clip never becomes an artifact) ─────────────
[[nodiscard]] crd::containers::Array<crd::u8>
skeleton_build(const SkeletonResource& skeleton, const crd::resources::ResourceId& id, crd::memory::IAllocator* alloc);

[[nodiscard]] crd::containers::Array<crd::u8>
anim_clip_build(const AnimClipResource& clip, const crd::resources::ResourceId& id, crd::memory::IAllocator* alloc);

// ── loaders (the mesh/PBRM loader pattern: injectable payload heap, version-gated, never partial) ───────────────
class SkeletonLoader final : public crd::resources::ILoader
{
public:
    SkeletonLoader() = default;
    explicit SkeletonLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_SKEL; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const crd::resources::LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

class AnimClipLoader final : public crd::resources::ILoader
{
public:
    AnimClipLoader() = default;
    explicit AnimClipLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_ANIM; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const crd::resources::LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

void register_anim_loaders(crd::resources::ResourceManager* rm, crd::memory::IAllocator* payload_alloc = nullptr);

} // namespace crd::anim
