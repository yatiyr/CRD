#pragma once

// audio_resources.hpp — GEO-10: the audio RESOURCES.
//
// ── 'ABUF' AudioBufferResource ─────────────────────────────────────────────────────────────────────────────────
// Decoded, normalized f32 interleaved PCM + rate/channels/source-bits — what voices and graph Sources sample.
// Cooked from .wav/.aiff/.flac by the GEO-6 processor ('ABDT' payload chunk, compressed). 16/24-bit sources
// normalize EXACTLY into f32 (the mantissa holds them — the codec suite proves the integer domain; this
// resource is the PROCESSING domain).
//
// ── 'AGRF' AudioGraphResource ──────────────────────────────────────────────────────────────────────────────────
// The processing graph IS a resource (the material-graph philosophy): Source/Gain/Biquad/Mix/Send nodes +
// edges + parameter AUTOMATION on the ONE curve engine (the GEO-9 record shape — i64 ticks at a rational rate,
// hesap-interp semantics). Send is a TAP: same gain math as Gain, its meaning is the extra routing edge.
// ONE validator gates build AND load: indices in bounds, the graph is a DAG (cycles refused), exactly one
// output node, automation targets exist, ticks strictly increase.

#include <crd/audio/audio_pcm.hpp>
#include <crd/containers/array.hpp>
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
inline constexpr crd::u32 kFourCC_ABUF = crd::resources::make_fourcc('A', 'B', 'U', 'F');
inline constexpr crd::u32 kFourCC_AbDt = crd::resources::make_fourcc('A', 'B', 'D', 'T');
inline constexpr crd::u32 kFourCC_AGRF = crd::resources::make_fourcc('A', 'G', 'R', 'F');
inline constexpr crd::u32 kFourCC_AgNd = crd::resources::make_fourcc('A', 'G', 'N', 'D');
inline constexpr crd::u32 kFourCC_AgEg = crd::resources::make_fourcc('A', 'G', 'E', 'G');
inline constexpr crd::u32 kFourCC_AgAu = crd::resources::make_fourcc('A', 'G', 'A', 'U');
inline constexpr crd::u32 kFourCC_AgAd = crd::resources::make_fourcc('A', 'G', 'A', 'D');
inline constexpr crd::u32 kFourCC_AgSt = crd::resources::make_fourcc('A', 'G', 'S', 'T');
// NOLINTEND(readability-identifier-naming)

inline constexpr crd::u32 kInvalidNode = 0xFFFFFFFFU;

// ── ABUF ───────────────────────────────────────────────────────────────────────────────────────────────────────

struct AudioBufferResource
{
    crd::u32                         sample_rate = 0;
    crd::u16                         channels    = 0;
    crd::u16                         source_bits = 0; // 0 = float source
    crd::containers::Array<crd::f32> samples;         // interleaved, normalized

    explicit AudioBufferResource(crd::memory::IAllocator* a) : samples(a) {}
    AudioBufferResource(AudioBufferResource&&)            = default;
    AudioBufferResource& operator=(AudioBufferResource&&) = default;

    [[nodiscard]] crd::u64 frame_count() const noexcept
    {
        return channels == 0 ? 0 : samples.size() / channels;
    }
};

[[nodiscard]] crd::containers::Array<crd::u8>
audio_buffer_build(const AudioPcm& pcm, const crd::resources::ResourceId& id, crd::memory::IAllocator* alloc);

class AudioBufferLoader final : public crd::resources::ILoader
{
public:
    AudioBufferLoader() = default;
    explicit AudioBufferLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_ABUF; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const crd::resources::LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

// ── AGRF ───────────────────────────────────────────────────────────────────────────────────────────────────────

enum class AudioNodeType : crd::u8
{
    Source = 0, // plays an ABUF buffer (start_frame offset, optional loop)
    Gain,       // gain_db (automatable: param 0)
    Biquad,     // RBJ filter: filter_type + cutoff (Nyquist fraction, automatable: param 1) + q
    Mix,        // sums every input edge
    Send,       // a TAP with its own gain_db — routing meaning lives in its edges
};

enum class BiquadType : crd::u8
{
    Lowpass = 0,
    Highpass,
    Bandpass,
    Notch,
};

enum class AudioParam : crd::u8
{
    GainDb = 0,
    CutoffNyq,
};

struct AudioNodeRec
{
    crd::u8                    type   = 0; // AudioNodeType
    crd::u8                    filter = 0; // BiquadType (Biquad)
    crd::u8                    loop   = 0; // Source
    crd::u8                    pad    = 0;
    crd::u32                   name_off = 0;
    crd::f32                   gain_db  = 0.0F; // Gain/Send
    crd::f32                   cutoff   = 0.5F; // Biquad, Nyquist fraction (0,1)
    crd::f32                   q        = 0.7071F;
    crd::u32                   pad2     = 0;
    crd::i64                   start_frame = 0; // Source: offset into the buffer
    crd::resources::ResourceId buffer;          // Source: the ABUF
};
static_assert(sizeof(AudioNodeRec) == 48, "AudioNodeRec is the on-disk 'AGND' record");

struct AudioEdgeRec
{
    crd::u32 from = 0;
    crd::u32 to   = 0;
};
static_assert(sizeof(AudioEdgeRec) == 8, "AudioEdgeRec is the on-disk 'AGEG' record");

struct AudioAutoRec // the GEO-9 automation shape, targeting (node, param)
{
    crd::u32                node  = 0;
    crd::u8                 param = 0; // AudioParam
    crd::u8                 interp = 1;
    crd::u8                 pad[2] = {};
    crd::time::RationalRate rate;
    crd::u32                key_count  = 0;
    crd::u32                ticks_off  = 0;
    crd::u32                values_off = 0;
};
static_assert(sizeof(AudioAutoRec) == 28, "AudioAutoRec is the on-disk 'AGAU' record");

struct AudioGraphResource
{
    crd::u32                              sample_rate = 48000;
    crd::u32                              out_node    = kInvalidNode;
    crd::u32                              name_off    = 0;
    crd::containers::Array<AudioNodeRec>  nodes;
    crd::containers::Array<AudioEdgeRec>  edges;
    crd::containers::Array<AudioAutoRec>  automation;
    crd::containers::Array<crd::i64>      auto_ticks;
    crd::containers::Array<crd::f32>      auto_values;
    crd::containers::Array<char>          strings;

    explicit AudioGraphResource(crd::memory::IAllocator* a)
        : nodes(a), edges(a), automation(a), auto_ticks(a), auto_values(a), strings(a)
    {
        strings.push_back('\0');
    }
    AudioGraphResource(AudioGraphResource&&)            = default;
    AudioGraphResource& operator=(AudioGraphResource&&) = default;

    crd::u32 intern(const char* s)
    {
        if (s == nullptr || s[0] == '\0') { return 0; }
        const crd::u32 off = static_cast<crd::u32>(strings.size());
        for (const char* p = s; *p != '\0'; ++p) { strings.push_back(*p); }
        strings.push_back('\0');
        return off;
    }
};

[[nodiscard]] crd::containers::Array<crd::u8>
audio_graph_build(const AudioGraphResource& graph, const crd::resources::ResourceId& id,
                  crd::memory::IAllocator* alloc);

class AudioGraphLoader final : public crd::resources::ILoader
{
public:
    AudioGraphLoader() = default;
    explicit AudioGraphLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_AGRF; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void*    load(const crd::resources::LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

void register_audio_loaders(crd::resources::ResourceManager* rm, crd::memory::IAllocator* payload_alloc = nullptr);

} // namespace crd::audio
