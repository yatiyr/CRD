#pragma once

// timeline_resource.hpp — GEO-9 (D-007 row 74): `TimelineResource` ('TIML') — the first-class editorial resource:
// tracks · clips · transitions · markers · time-effects · param-automation curves, ALL structure in RATIONAL time
// (crd::time::RationalTime — the drift doctrine), REFERENCING media (ResourceId when cooked, URI always — never
// embedding). ONE data model for the game cutscene sequencer AND the video-composition product; `.otio` is the
// interchange edge (asset-io), TIML is the engine's truth.
//
// ── chunks ──────────────────────────────────────────────────────────────────────────────────────────────────────
//   'TMHD' header  — version · counts · global_start · name offset
//   'TMST' strings — NUL-separated pool (names · urls · kinds · transition types · automation targets)
//   'TMTK' tracks  · 'TMIT' items · 'TMMD' media · 'TMFX' effects · 'TMMK' markers — fixed POD records
//   'TMAU' automation directory · 'TMAD' automation data (i64 ticks then f32 values)
//
// Records are zero-initialized PODs (byte-identical rebuilds — the GEO-6 determinism gates); every offset is
// validated at BUILD and again at LOAD (a corrupt artifact refuses, never a partial resource).
//
// Automation is the NATIVE cutscene feature (OTIO has no curve schema — it does not round-trip through `.otio`,
// documented): a target is a free-form param path ("camera.fov", "rig/arm.reach"); keys are i64 ticks at the
// track's own rational rate; interpolation semantics are hesap-interp's keyframe contract (Step · Linear ·
// CubicHermite [in·value·out] triples, split tangents) — the ONE curve engine, sampled with rational-exact
// segment selection (timeline_eval.hpp).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/time/rational_time.hpp>

namespace crd::resources
{
class ResourceManager;
}

namespace crd::timeline
{

// NOLINTBEGIN(readability-identifier-naming) — repo-wide kFourCC_* mnemonic convention
inline constexpr crd::u32 kFourCC_TIML = crd::resources::make_fourcc('T', 'I', 'M', 'L');
inline constexpr crd::u32 kFourCC_TmHd = crd::resources::make_fourcc('T', 'M', 'H', 'D');
inline constexpr crd::u32 kFourCC_TmSt = crd::resources::make_fourcc('T', 'M', 'S', 'T');
inline constexpr crd::u32 kFourCC_TmTk = crd::resources::make_fourcc('T', 'M', 'T', 'K');
inline constexpr crd::u32 kFourCC_TmIt = crd::resources::make_fourcc('T', 'M', 'I', 'T');
inline constexpr crd::u32 kFourCC_TmMd = crd::resources::make_fourcc('T', 'M', 'M', 'D');
inline constexpr crd::u32 kFourCC_TmFx = crd::resources::make_fourcc('T', 'M', 'F', 'X');
inline constexpr crd::u32 kFourCC_TmMk = crd::resources::make_fourcc('T', 'M', 'M', 'K');
inline constexpr crd::u32 kFourCC_TmAu = crd::resources::make_fourcc('T', 'M', 'A', 'U');
inline constexpr crd::u32 kFourCC_TmAd = crd::resources::make_fourcc('T', 'M', 'A', 'D');
// NOLINTEND(readability-identifier-naming)

inline constexpr crd::u32 kInvalidIndex = 0xFFFFFFFFU;

enum class TrackKind : crd::u8
{
    Video = 0,
    Audio,
    Other,
};

enum class ItemType : crd::u8
{
    Clip = 0,
    Gap,
    Transition,
};

enum class MediaKind : crd::u8
{
    External = 0,  // url only (foreign media — the interchange truth)
    Missing,       // a valid edit whose media is not resolvable
    ImageSequence, // url base + prefix/suffix/padding/frame numbering (EXR-sequence masters)
    Resource,      // a COOKED engine resource — `resource` is live (scene takes · GEO-8 clips · GEO-10 audio)
};

enum class EffectType : crd::u8
{
    LinearTimeWarp = 0,
    FreezeFrame,
};

// ── on-disk POD records (zero-init before fill — pads are part of the deterministic byte stream) ───────────────

struct TrackRec
{
    crd::u8  kind = 0; // TrackKind
    crd::u8  pad[3] = {};
    crd::u32 kind_name_off = 0; // TrackKind::Other round-trips the verbatim OTIO kind
    crd::u32 name_off      = 0;
    crd::u32 first_item    = 0;
    crd::u32 item_count    = 0;
    crd::u32 first_marker  = kInvalidIndex;
    crd::u32 marker_count  = 0;
};
static_assert(sizeof(TrackRec) == 28, "TrackRec is the on-disk 'TMTK' record");

struct ItemRec
{
    crd::u8                 type             = 0; // ItemType
    crd::u8                 has_source_range = 0;
    crd::u8                 pad[2]           = {};
    crd::u32                name_off         = 0;
    crd::time::TimeRange    source_range; // Clip: trims media · Gap: duration (builder RESOLVES absent clip trims)
    crd::u32                media_ref = kInvalidIndex; // Clip → media record
    crd::u32                transition_type_off = 0;
    crd::time::RationalTime in_offset;  // Transition only
    crd::time::RationalTime out_offset; // Transition only
    crd::u32                first_effect = kInvalidIndex;
    crd::u32                effect_count = 0;
    crd::u32                first_marker = kInvalidIndex;
    crd::u32                marker_count = 0;
};
static_assert(sizeof(ItemRec) == 96, "ItemRec is the on-disk 'TMIT' record");

struct MediaRec
{
    crd::u8                    kind = 0; // MediaKind
    crd::u8                    has_available_range = 0;
    crd::u8                    pad[2]              = {};
    crd::u32                   name_off            = 0;
    crd::u32                   url_off             = 0; // External/ImageSequence; the interchange truth, kept ALWAYS
    crd::u32                   prefix_off          = 0; // ImageSequence
    crd::u32                   suffix_off          = 0;
    crd::i32                   start_frame         = 0;
    crd::i32                   frame_step          = 1;
    crd::i32                   zero_padding        = 0;
    crd::time::RationalRate    seq_rate;
    crd::time::TimeRange       available_range;
    crd::resources::ResourceId resource; // MediaKind::Resource: the cooked engine resource (nil otherwise)
};
static_assert(sizeof(MediaRec) == 88, "MediaRec is the on-disk 'TMMD' record");

struct EffectRec
{
    crd::u8  type   = 0; // EffectType
    crd::u8  pad[7] = {};
    crd::f64 time_scalar = 1.0;
};
static_assert(sizeof(EffectRec) == 16, "EffectRec is the on-disk 'TMFX' record");

struct MarkerRec
{
    crd::u32             name_off  = 0;
    crd::u32             color_off = 0;
    crd::time::TimeRange range;
};
static_assert(sizeof(MarkerRec) == 40, "MarkerRec is the on-disk 'TMMK' record");

struct AutomationRec
{
    crd::u32                target_off = 0; // param path in the string pool
    crd::time::RationalRate rate;           // key ticks are at THIS rate
    crd::u8                 interp = 1;     // hesap::interp::KeyInterp byte value
    crd::u8                 pad[3] = {};
    crd::u32                key_count  = 0;
    crd::u32                ticks_off  = 0; // i64 index into auto_ticks
    crd::u32                values_off = 0; // f32 index into auto_values (CubicHermite: [in·value·out] triples)
};
static_assert(sizeof(AutomationRec) == 28, "AutomationRec is the on-disk 'TMAU' record");

// ── the runtime resource ───────────────────────────────────────────────────────────────────────────────────────

struct TimelineResource
{
    crd::u32                             version          = 1;
    crd::u8                              has_global_start = 0;
    crd::time::RationalTime              global_start;
    crd::u32                             name_off = 0;
    crd::containers::Array<TrackRec>     tracks;
    crd::containers::Array<ItemRec>      items;
    crd::containers::Array<MediaRec>     media;
    crd::containers::Array<EffectRec>    effects;
    crd::containers::Array<MarkerRec>    markers;
    crd::u32                             first_marker = kInvalidIndex; // timeline-level markers
    crd::u32                             marker_count = 0;
    crd::containers::Array<AutomationRec> automation;
    crd::containers::Array<crd::i64>     auto_ticks;
    crd::containers::Array<crd::f32>     auto_values;
    crd::containers::Array<char>         strings; // NUL-separated; offset 0 is always "" (the empty string)

    explicit TimelineResource(crd::memory::IAllocator* a)
        : tracks(a), items(a), media(a), effects(a), markers(a), automation(a), auto_ticks(a), auto_values(a),
          strings(a)
    {
        strings.push_back('\0'); // offset 0 = ""
    }
    TimelineResource(TimelineResource&&)            = default;
    TimelineResource& operator=(TimelineResource&&) = default;

    [[nodiscard]] const char* str(crd::u32 off) const noexcept
    {
        return off < strings.size() ? strings.data() + off : "";
    }
    [[nodiscard]] const char* name() const noexcept { return str(name_off); }

    // append `s` to the pool, returning its offset (builder-side convenience; "" maps to offset 0 for free)
    crd::u32 intern(const char* s)
    {
        if (s == nullptr || s[0] == '\0') { return 0; }
        const crd::u32 off = static_cast<crd::u32>(strings.size());
        for (const char* p = s; *p != '\0'; ++p) { strings.push_back(*p); }
        strings.push_back('\0');
        return off;
    }
};

// ── build + load ───────────────────────────────────────────────────────────────────────────────────────────────

// Serialize (validated — a malformed timeline never becomes an artifact; empty return = refusal). Validation:
// index ranges in bounds · items grouped per track · every clip/gap duration RESOLVED (non-negative, valid rate) ·
// transitions BETWEEN two clips/gaps with offsets covered by their neighbors · automation tracks strictly
// increasing with the exact value count for their interp mode.
[[nodiscard]] crd::containers::Array<crd::u8>
timeline_build(const TimelineResource& tl, const crd::resources::ResourceId& id, crd::memory::IAllocator* alloc);

class TimelineLoader final : public crd::resources::ILoader
{
public:
    TimelineLoader() = default;
    explicit TimelineLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_TIML; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const crd::resources::LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

void register_timeline_loader(crd::resources::ResourceManager* rm, crd::memory::IAllocator* payload_alloc = nullptr);

} // namespace crd::timeline
