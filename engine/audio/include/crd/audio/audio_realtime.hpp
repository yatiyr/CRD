#pragma once

// audio_realtime.hpp — GEO-10: the REALTIME layer's lock-free substrate. ⛔ THE CALLBACK CONTRACT: the audio
// thread never allocates, never locks, never touches the ResourceManager — it consumes COMMANDS from a
// single-producer/single-consumer ring (own atomics — the counting-semaphore lost-wake scar says OWN the
// primitive) and mixes a PREALLOCATED voice pool over borrowed sample memory (the game thread keeps the
// owning resources alive while any voice plays them; stop-all + drain before unloading).
//
// CLAP-shaped by design: sample-accurate command timestamps ride the ring (a command carries the frame it
// applies at — v1 applies at block start; the field is in the format so CLAP hosting slots in later).

#include <crd/core/types.hpp>

#include <atomic>
#include <cstring>

namespace crd::audio
{

// ── the SPSC command ring ──────────────────────────────────────────────────────────────────────────────────────

enum class AudioCommandType : crd::u8
{
    PlayVoice = 0, // voice ← {samples/frames/channels/gain/loop}
    StopVoice,
    SetVoiceGain,
    StopAll,
};

struct AudioCommand
{
    AudioCommandType type  = AudioCommandType::StopAll;
    crd::u8          loop  = 0;
    crd::u16         voice = 0;
    crd::u32         channels = 0;
    crd::u64         frames   = 0;
    const crd::f32*  samples  = nullptr; // BORROWED — the producer guarantees lifetime
    crd::f32         gain     = 1.0F;
    crd::u32         at_frame = 0; // sample-accurate slot (v1: block start; the CLAP seam)
};
static_assert(sizeof(AudioCommand) <= 64, "one cache line per command");

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // padding after alignas(64) — the POINT is the cache-line isolation
#endif
template <crd::u32 CapacityPow2>
class AudioCommandRing
{
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "capacity is a power of two");

public:
    // producer (game thread) — false when full (the producer retries next frame; commands never block)
    bool try_push(const AudioCommand& cmd) noexcept
    {
        const crd::u32 t = m_tail.load(std::memory_order_relaxed);
        const crd::u32 h = m_head.load(std::memory_order_acquire);
        if (t - h >= CapacityPow2) { return false; }
        m_slots[t & (CapacityPow2 - 1)] = cmd;
        m_tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // consumer (audio thread) — false when empty
    bool try_pop(AudioCommand& out) noexcept
    {
        const crd::u32 h = m_head.load(std::memory_order_relaxed);
        const crd::u32 t = m_tail.load(std::memory_order_acquire);
        if (h == t) { return false; }
        out = m_slots[h & (CapacityPow2 - 1)];
        m_head.store(h + 1, std::memory_order_release);
        return true;
    }

private:
    alignas(64) std::atomic<crd::u32> m_head{0};
    alignas(64) std::atomic<crd::u32> m_tail{0};
    AudioCommand m_slots[CapacityPow2];
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// ── the voice pool (all storage preallocated; mix() is the audio thread's only work) ───────────────────────────

inline constexpr crd::u32 kVoiceCount = 32;

struct Voice
{
    const crd::f32* samples  = nullptr;
    crd::u64        frames   = 0;
    crd::u64        cursor   = 0;
    crd::u32        channels = 0;
    crd::f32        gain     = 1.0F;
    bool            loop     = false;
    bool            active   = false;
};

class VoiceMixer
{
public:
    // audio-thread side: drain commands, then mix `frames` stereo frames ADDITIVELY into `out`
    template <typename Ring>
    void process(Ring& ring, crd::f32* out, crd::u32 frames) noexcept
    {
        AudioCommand cmd;
        while (ring.try_pop(cmd))
        {
            switch (cmd.type)
            {
            case AudioCommandType::PlayVoice:
                if (cmd.voice < kVoiceCount && cmd.samples != nullptr && cmd.channels > 0)
                {
                    Voice& v   = m_voices[cmd.voice];
                    v.samples  = cmd.samples;
                    v.frames   = cmd.frames;
                    v.cursor   = 0;
                    v.channels = cmd.channels;
                    v.gain     = cmd.gain;
                    v.loop     = cmd.loop != 0;
                    v.active   = true;
                }
                break;
            case AudioCommandType::StopVoice:
                if (cmd.voice < kVoiceCount) { m_voices[cmd.voice].active = false; }
                break;
            case AudioCommandType::SetVoiceGain:
                if (cmd.voice < kVoiceCount) { m_voices[cmd.voice].gain = cmd.gain; }
                break;
            case AudioCommandType::StopAll:
            default:
                for (Voice& v : m_voices) { v.active = false; }
                break;
            }
        }
        for (Voice& v : m_voices)
        {
            if (!v.active) { continue; }
            for (crd::u32 i = 0; i < frames; ++i)
            {
                if (v.cursor >= v.frames)
                {
                    if (!v.loop)
                    {
                        v.active = false;
                        break;
                    }
                    v.cursor = 0;
                }
                const crd::f32* s = v.samples + v.cursor * v.channels;
                out[i * 2] += v.gain * s[0];
                out[i * 2 + 1] += v.gain * (v.channels >= 2 ? s[1] : s[0]);
                ++v.cursor;
            }
        }
    }

    [[nodiscard]] crd::u32 active_count() const noexcept
    {
        crd::u32 n = 0;
        for (const Voice& v : m_voices)
        {
            if (v.active) { ++n; }
        }
        return n;
    }

private:
    Voice m_voices[kVoiceCount];
};

// ── the device (WASAPI on Windows; start() returns false where no endpoint exists — an honest skip) ────────────

using AudioRenderFn = void (*)(void* user, crd::f32* interleaved_stereo, crd::u32 frames, crd::u32 sample_rate);

class AudioDevice
{
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&)            = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // open the default render endpoint (shared, event-driven, float32) and start the render thread
    [[nodiscard]] bool start(AudioRenderFn fn, void* user);
    void               stop();

    [[nodiscard]] crd::u32 sample_rate() const noexcept;
    [[nodiscard]] crd::u32 xrun_count() const noexcept;   // event timeouts + starved periods — the soak metric
    [[nodiscard]] crd::u64 frames_rendered() const noexcept;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace crd::audio
