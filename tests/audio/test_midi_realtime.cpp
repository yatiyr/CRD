// test_midi_realtime.cpp — GEO-10: the MIDI gates + the realtime substrate gates.
//  · SMF: a hand-authored format-1 file (tempo change · running status · pitch bend · dangling note) parses
//    to EXACT events; MIDI-1 velocities land in the 32-bit MIDI 2.0 domain by bit replication; the tempo map
//    converts tick → RationalTime EXACTLY across the change; refusals typed (format 2 · SMPTE · truncation)
//  · MIDI resource: build → load round-trip
//  · the SPSC command ring: cross-thread (crd-jobs) flood — every command arrives, in order, no locks
//  · THE SOAK: the WASAPI device renders the voice mixer live — xruns == 0 over the soak window (skips
//    honestly when the host has no endpoint)

#include <crd/audio/audio_realtime.hpp>
#include <crd/audio/midi.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/loader.hpp>
#include <crd/time/rational_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace crd;

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(128U << 20U);
    return a;
}

void push_bytes(containers::Array<u8>& out, const void* p, usize n)
{
    const auto* b = static_cast<const u8*>(p);
    for (usize i = 0; i < n; ++i) { out.push_back(b[i]); }
}

} // namespace

TEST_CASE("smf: the reference file parses to exact MIDI 2.0-domain events", "[audio][midi]")
{
    // format 1, division 480: track 0 = tempo map (120 → 60 BPM at beat 2); track 1 = the performance
    containers::Array<u8> smf(&galloc());
    const u8 head[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x01, 0xE0};
    push_bytes(smf, head, sizeof(head));
    const u8 track0[] = {'M', 'T', 'r', 'k', 0, 0, 0, 19,
                         0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,  // tempo 500000 (120 BPM) at tick 0
                         0x87, 0x40, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40, // tempo 1000000 (60 BPM) at tick 960
                         0x00, 0xFF, 0x2F, 0x00};
    push_bytes(smf, track0, sizeof(track0));
    const u8 track1[] = {'M', 'T', 'r', 'k', 0, 0, 0, 22,
                         0x00, 0x90, 60, 100,       // C4 on, vel 100
                         0x60, 62, 64,              // RUNNING STATUS: D4 on at tick 96
                         0x20, 60, 0,               // C4 off (vel-0 form) at tick 128
                         0x00, 0xE0, 0x00, 0x60,    // pitch bend at tick 128 (14-bit 0x3000, +0x1000)
                         0x40, 0x80, 62, 32,        // D4 explicit off at tick 192, release vel 32
                         0x00, 0xFF, 0x2F, 0x00};
    push_bytes(smf, track1, sizeof(track1));

    audio::MidiResource midi(&galloc());
    REQUIRE(audio::midi_parse_smf(containers::as_const_span(smf), midi) == audio::MidiError::Ok);
    CHECK(midi.division == 480);
    REQUIRE(midi.tempo.size() == 2);
    CHECK(midi.tempo[1].tick == 960);
    CHECK(midi.tempo[1].us_per_quarter == 1000000);

    REQUIRE(midi.notes.size() == 2);
    CHECK(midi.notes[0].note == 60);
    CHECK(midi.notes[0].tick == 0);
    CHECK(midi.notes[0].duration == 128);
    CHECK(midi.notes[0].velocity == audio::midi1_velocity_to_32(100)); // the 2.0 upscale
    CHECK(midi.notes[1].note == 62);
    CHECK(midi.notes[1].tick == 96);
    CHECK(midi.notes[1].duration == 96);
    CHECK(midi.notes[1].off_velocity == audio::midi1_velocity_to_32(32));

    REQUIRE(midi.controls.size() == 1);
    CHECK(midi.controls[0].kind == 1); // bend
    CHECK(midi.controls[0].value > 0);

    // the upscale itself: 127 → 0xFFFFFFFF (full scale), 64 → the bit-replicated midpoint, 0 → 0
    CHECK(audio::midi1_velocity_to_32(127) == 0xFFFFFFFFU);
    CHECK(audio::midi1_velocity_to_32(0) == 0);
    CHECK((audio::midi1_velocity_to_32(64) >> 25U) == 64U);

    // EXACT tick → time across the tempo change: tick 960 = 2 beats at 120 BPM = 1 s EXACTLY;
    // tick 1440 = 1 s + 1 beat at 60 BPM = 2 s EXACTLY
    const time::RationalTime t960 = audio::midi_tick_to_time(midi, 960);
    CHECK(time::compare(t960, time::RationalTime{1, time::make_rate(1, 1)}) == 0);
    const time::RationalTime t1440 = audio::midi_tick_to_time(midi, 1440);
    CHECK(time::compare(t1440, time::RationalTime{2, time::make_rate(1, 1)}) == 0);

    // refusals
    audio::MidiResource junk(&galloc());
    const u8            fmt2[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 2, 0, 1, 0x01, 0xE0};
    CHECK(audio::midi_parse_smf({fmt2, sizeof(fmt2)}, junk) == audio::MidiError::UnsupportedFormat);
    const u8 smpte[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0xE8, 0x50};
    CHECK(audio::midi_parse_smf({smpte, sizeof(smpte)}, junk) == audio::MidiError::UnsupportedFormat);
    CHECK(audio::midi_parse_smf({head, 4}, junk) == audio::MidiError::NotSmf);

    // resource round-trip
    const resources::ResourceId id{7, 8};
    containers::Array<u8>       bytes = audio::midi_build(midi, id, &galloc());
    REQUIRE(bytes.size() > 0);
    audio::MidiLoader      loader(&galloc());
    resources::LoadContext ctx;
    ctx.id        = id;
    ctx.bytes     = containers::as_const_span(bytes);
    ctx.allocator = &galloc();
    auto* loaded  = static_cast<audio::MidiResource*>(loader.load(ctx));
    REQUIRE(loaded != nullptr);
    CHECK(loaded->notes.size() == 2);
    CHECK(loaded->tempo.size() == 2);
    CHECK(loaded->notes[0].velocity == midi.notes[0].velocity);
    loader.unload(loaded);
}

TEST_CASE("the SPSC command ring: cross-thread order and completeness", "[audio][realtime]")
{
    static audio::AudioCommandRing<1024> ring; // static: outlives both sides deterministically
    constexpr u32                        k_count = 100000;

    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (u32 i = 0; i < k_count; ++i)
        {
            audio::AudioCommand cmd;
            cmd.type  = audio::AudioCommandType::SetVoiceGain;
            cmd.voice = static_cast<u16>(i & 0x1F);
            cmd.gain  = static_cast<f32>(i); // the sequence rides the payload — order is checkable
            while (!ring.try_push(cmd)) { std::this_thread::yield(); }
        }
        done.store(true, std::memory_order_release);
    });

    u32                 received = 0;
    f32                 expect   = 0.0F;
    audio::AudioCommand cmd;
    while (received < k_count)
    {
        if (ring.try_pop(cmd))
        {
            REQUIRE(cmd.gain == expect); // strict FIFO — any reorder/loss fails here
            expect += 1.0F;
            ++received;
        }
        else if (done.load(std::memory_order_acquire) && !ring.try_pop(cmd))
        {
            std::this_thread::yield();
        }
    }
    producer.join();
    CHECK(received == k_count);
}

namespace
{
    struct SoakState
    {
        audio::AudioCommandRing<256> ring;
        audio::VoiceMixer            mixer;
    };

    void soak_render(void* user, f32* out, u32 frames, u32 /*rate*/)
    {
        auto* s = static_cast<SoakState*>(user);
        s->mixer.process(s->ring, out, frames); // additive over the zeroed device buffer
    }
} // namespace

TEST_CASE("THE SOAK: the device renders voices live with zero xruns", "[audio][realtime][soak]")
{
    static SoakState      soak;
    static containers::Array<f32> tone(&galloc());
    if (tone.size() == 0)
    {
        for (u32 i = 0; i < 48000; ++i) // 1 s 330 Hz sine at −18 dB, mono
        {
            tone.push_back(0.125F * math::sin(6.2831853F * 330.0F * static_cast<f32>(i) / 48000.0F));
        }
    }

    audio::AudioDevice device;
    if (!device.start(&soak_render, &soak))
    {
        WARN("[soak] no render endpoint on this host — the realtime gate needs audio hardware (skipped)");
        return;
    }

    audio::AudioCommand play;
    play.type     = audio::AudioCommandType::PlayVoice;
    play.voice    = 0;
    play.samples  = tone.data();
    play.frames   = 48000;
    play.channels = 1;
    play.gain     = 1.0F;
    play.loop     = 1;
    REQUIRE(soak.ring.try_push(play));

    std::this_thread::sleep_for(std::chrono::seconds(10)); // the soak window

    audio::AudioCommand stop;
    stop.type = audio::AudioCommandType::StopAll;
    (void)soak.ring.try_push(stop);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    device.stop();

    WARN("[soak] rate=" << device.sample_rate() << " frames=" << device.frames_rendered()
                        << " xruns=" << device.xrun_count());
    CHECK(device.frames_rendered() > 0);
    CHECK(device.xrun_count() == 0); // THE GATE: glitch-free over the soak
}
