// DIAG.10c -- the audio device's native owners in the lifetime ledger, and its stop/restart/shutdown.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): on Windows the device holds, from start() until
// its destructor or the next start(), five OS objects: the MMDevice enumerator, the endpoint, the audio client, the
// period event and the render client (COM references and a kernel event, each an OsObject in the ledger), plus its
// render thread (a Thread) from start() until stop(). A start that fails part-way releases what it obtained. A stopped
// device keeps its session; a start after stop() releases that session before it opens a new one (it used to open a
// second set over the first and leak it). The expected counts are read off the device's own start sequence.
// Without a render endpoint (the hosted runners, and every non-Windows host, where start() always refuses) the
// failed start must balance; the restart half reports that it did not run.

#include <crd/audio/audio_realtime.hpp>
#include <crd/core/lifetime_ledger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace
{
using crd::LifetimeClass;
using crd::LifetimeVerdict;

constexpr crd::i64 kSessionObjects = 5; // enumerator, endpoint, audio client, period event, render client

crd::LifetimeReport since(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot());
}

void render_silence(void* /*user*/, crd::f32* /*out*/, crd::u32 /*frames*/, crd::u32 /*rate*/) {}

// Wait (at most two seconds) until the device has rendered more than `after` frames.
bool rendered_past(const crd::audio::AudioDevice& device, crd::u64 after)
{
    for (crd::u32 i = 0U; i < 200U; ++i)
    {
        if (device.frames_rendered() > after)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST_CASE("diag10c: the audio device holds its session's OS objects until it ends, and a restart replaces them",
          "[audio][realtime][lifetime][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        crd::audio::AudioDevice device;
        if (!device.start(&render_silence, nullptr))
        {
            // No endpoint: every object the attempt obtained was released again, and no thread was left.
            const crd::LifetimeReport refused = since(start);
            CHECK(refused.verdict == LifetimeVerdict::Balanced);
            WARN("no render endpoint on this host: only the refused start was checked");
        }
        else
        {
            crd::LifetimeReport r = since(start);
            CHECK(r.live_change(LifetimeClass::OsObject) == kSessionObjects);
            CHECK(r.live_change(LifetimeClass::Thread) == 1);
            CHECK(rendered_past(device, 0U));

            // Stopped: the render thread is joined; the session stays for a restart.
            device.stop();
            r = since(start);
            CHECK(r.live_change(LifetimeClass::Thread) == 0);
            CHECK(r.live_change(LifetimeClass::OsObject) == kSessionObjects);

            // Restarted: one session, not two.
            for (crd::u32 restart = 0U; restart < 3U; ++restart)
            {
                const crd::u64 frames = device.frames_rendered();
                REQUIRE(device.start(&render_silence, nullptr));
                r = since(start);
                CHECK(r.live_change(LifetimeClass::OsObject) == kSessionObjects);
                CHECK(r.live_change(LifetimeClass::Thread) == 1);
                CHECK(rendered_past(device, frames));
                device.stop();
            }
        }
    }
    const crd::LifetimeReport gone = since(start);
    CHECK(gone.verdict == LifetimeVerdict::Balanced);
    CHECK(gone.underflowed == 0U);
}
