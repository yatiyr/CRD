#pragma once

// midi.hpp — GEO-10: `MidiResource` ('MIDI') — a MIDI 2.0-NATIVE data model with an SMF (Standard MIDI File)
// parse floor. The doctrine: bake MIDI-1's 7-bit assumptions into the model and per-note expression never
// fits later — so velocities are 32-bit (MIDI-1 upscales by the spec's bit-replication), controllers carry
// 32-bit values, and notes are INTERVALS (paired on/off with both velocities) ready for per-note controllers.
// Ticks are exact i64 at the file's division; the tempo map converts tick → RationalTime EXACTLY
// (microseconds-per-quarter is integral — seconds are rational, the GEO-9 rule).
//
// Parse floor: SMF format 0 + 1 (tracks merge on the shared tick axis), running status, all channel voice
// messages, meta tempo/end-of-track (others skipped by length), VLQ delta times. SMPTE division refuses in
// v1 (music-time files are the substrate's floor; SMPTE-division files are rare exports — a typed refusal,
// never a wrong tempo). Format 2 refuses (independent-pattern files are a sequencer feature, not a floor).

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

namespace crd::audio
{

// NOLINTBEGIN(readability-identifier-naming) — repo-wide kFourCC_* mnemonic convention
inline constexpr crd::u32 kFourCC_MIDI = crd::resources::make_fourcc('M', 'I', 'D', 'I');
inline constexpr crd::u32 kFourCC_MdNt = crd::resources::make_fourcc('M', 'D', 'N', 'T');
inline constexpr crd::u32 kFourCC_MdCc = crd::resources::make_fourcc('M', 'D', 'C', 'C');
inline constexpr crd::u32 kFourCC_MdTp = crd::resources::make_fourcc('M', 'D', 'T', 'P');
// NOLINTEND(readability-identifier-naming)

// MIDI-1 7-bit → 32-bit by the MIDI 2.0 upscaling rule (bit replication fills the added resolution)
[[nodiscard]] constexpr crd::u32 midi1_velocity_to_32(crd::u8 v7) noexcept
{
    if (v7 == 0) { return 0; }
    crd::u32 v = static_cast<crd::u32>(v7 & 0x7FU) << 25U;
    v |= v >> 7U;
    v |= v >> 14U;
    v |= v >> 28U;
    return v;
}

struct MidiNote
{
    crd::i64 tick     = 0; // note-on tick
    crd::i64 duration = 0; // ticks to the matching note-off
    crd::u8  channel  = 0;
    crd::u8  note     = 0;
    crd::u8  pad[2]   = {};
    crd::u32 velocity = 0; // 32-bit (MIDI 2.0 domain)
    crd::u32 off_velocity = 0;
    crd::u32 pad2         = 0; // explicit tail pad — the record is 8-aligned on disk
};
static_assert(sizeof(MidiNote) == 32, "MidiNote is the on-disk 'MDNT' record");

struct MidiControl // CC / pitch bend / channel pressure / program on one 32-bit timeline
{
    crd::i64 tick    = 0;
    crd::u8  channel = 0;
    crd::u8  kind    = 0; // 0 = CC (index live) · 1 = pitch bend (signed center 0) · 2 = pressure · 3 = program
    crd::u8  index   = 0; // CC number / program number
    crd::u8  pad     = 0;
    crd::i32 value   = 0; // CC/pressure: 0..2^32-1 downshifted to i32 range? — stored as the 32-bit UPSCALED value reinterpreted; bend: signed
};
static_assert(sizeof(MidiControl) == 16, "MidiControl is the on-disk 'MDCC' record");

struct MidiTempo
{
    crd::i64 tick           = 0;
    crd::u32 us_per_quarter = 500000; // 120 BPM default per SMF
    crd::u32 pad            = 0;
};
static_assert(sizeof(MidiTempo) == 16, "MidiTempo is the on-disk 'MDTP' record");

struct MidiResource
{
    crd::u16                            division = 480; // ticks per quarter note
    crd::containers::Array<MidiNote>    notes;          // tick-ordered
    crd::containers::Array<MidiControl> controls;       // tick-ordered
    crd::containers::Array<MidiTempo>   tempo;          // tick-ordered; implicit 120 BPM at tick 0 when empty

    explicit MidiResource(crd::memory::IAllocator* a) : notes(a), controls(a), tempo(a) {}
    MidiResource(MidiResource&&)            = default;
    MidiResource& operator=(MidiResource&&) = default;
};

enum class MidiError : crd::u8
{
    Ok = 0,
    NotSmf,            // missing/short MThd
    UnsupportedFormat, // format 2 / SMPTE division
    Malformed,         // truncated VLQ / event / contradictory sizes
};

// Parse SMF bytes into `out` (cleared first). Notes pair on/off (a dangling on closes at end-of-track).
[[nodiscard]] MidiError midi_parse_smf(crd::containers::ConstSpan<crd::u8> bytes, MidiResource& out);

// EXACT tick → time: seconds = Σ over tempo segments of (ticks × us_per_quarter) / (division × 1e6) — all
// integral, so the result is a RationalTime (ticks at a rate derived from the segment). Invalid on tick < 0.
[[nodiscard]] crd::time::RationalTime midi_tick_to_time(const MidiResource& midi, crd::i64 tick) noexcept;

[[nodiscard]] crd::containers::Array<crd::u8>
midi_build(const MidiResource& midi, const crd::resources::ResourceId& id, crd::memory::IAllocator* alloc);

class MidiLoader final : public crd::resources::ILoader
{
public:
    MidiLoader() = default;
    explicit MidiLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_MIDI; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const crd::resources::LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

void register_midi_loader(crd::resources::ResourceManager* rm, crd::memory::IAllocator* payload_alloc = nullptr);

} // namespace crd::audio
