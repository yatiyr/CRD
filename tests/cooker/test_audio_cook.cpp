// tests/cooker/test_audio_cook.cpp — GEO-10: a real .flac cooks through the REAL handler into 'ABUF', loads
// through the REAL loader with the normalized samples EXACT; a .mid cooks to 'MIDI'; second cooks are
// byte-identical (GEO-6). The full chain: our encoder wrote the source, the cook decoded it, the resource
// carries the processing-domain truth.

#include <catch2/catch_test_macros.hpp>

#include <crd/audio/audio_resources.hpp>
#include <crd/audio/flac.hpp>
#include <crd/audio/midi.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>

#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
void register_audio_handlers(); // defined in audio_cook.cpp
}

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::MallocAllocator g_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void ensure_registered()
{
    static bool registered = false;
    if (!registered)
    {
        crd::cooker::register_audio_handlers();
        registered = true;
    }
}

} // namespace

TEST_CASE("cooker: .flac cooks to ABUF; normalized samples exact; deterministic", "[cooker][audio]")
{
    ensure_registered();

    // author the source with OUR encoder (the oracle-proven codec)
    crd::audio::AudioPcm pcm(&g_alloc);
    pcm.sample_rate     = 48000;
    pcm.channels        = 2;
    pcm.bits_per_sample = 16;
    for (crd::i32 i = 0; i < 5000; ++i)
    {
        pcm.isamples.push_back((i * 13) % 20000 - 10000);
        pcm.isamples.push_back((i * 7) % 16000 - 8000);
    }
    const crd::containers::Array<crd::u8> flac = crd::audio::flac_encode(pcm, &g_alloc);
    REQUIRE(flac.size() > 0);
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView("cerid_cook_audio.flac")),
                                  crd::containers::as_const_span(flac)));

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView("cerid_cook_audio.flac");
    ctx.meta_path   = crd::containers::StringView("cerid_cook_audio.flac.meta");
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;
    crd::cooker::CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc); // GEO-6: the only road to bytes
    ctx.io          = &ctx_io;

    const crd::cooker::CookHandlerFn handler =
        crd::cooker::find_cook_handler(crd::containers::StringView(".flac"));
    REQUIRE(handler != nullptr);
    const crd::cooker::CookResult result = handler(ctx);
    REQUIRE(result.ok);
    CHECK(result.type_fourcc == crd::audio::kFourCC_ABUF);

    crd::audio::AudioBufferLoader loader(&g_alloc);
    crd::resources::LoadContext   lctx;
    lctx.id        = ctx.id;
    lctx.bytes     = crd::containers::as_const_span(result.cooked_bytes);
    lctx.allocator = &g_alloc;
    auto* buf      = static_cast<crd::audio::AudioBufferResource*>(loader.load(lctx));
    REQUIRE(buf != nullptr);
    CHECK(buf->sample_rate == 48000);
    CHECK(buf->channels == 2);
    CHECK(buf->source_bits == 16);
    REQUIRE(buf->samples.size() == pcm.isamples.size());
    for (crd::usize i = 0; i < 64; ++i) // 16-bit normalization is exact in f32 — spot-verify the head
    {
        REQUIRE(buf->samples[i] == static_cast<crd::f32>(pcm.isamples[i]) / 32768.0F);
    }
    loader.unload(buf);

    // deterministic second cook
    crd::cooker::CookContext ctx2 = ctx;
    crd::cooker::CookIO      ctx_io2(ctx2.source_path, ctx2.meta_path, &g_alloc);
    ctx2.io                       = &ctx_io2;
    const crd::cooker::CookResult again = handler(ctx2);
    REQUIRE(again.ok);
    REQUIRE(again.cooked_bytes.size() == result.cooked_bytes.size());
    CHECK(std::memcmp(again.cooked_bytes.data(), result.cooked_bytes.data(), result.cooked_bytes.size()) == 0);
}

TEST_CASE("cooker: .mid cooks to MIDI; a broken file refuses", "[cooker][audio]")
{
    ensure_registered();

    const crd::u8 smf[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                           'M', 'T', 'r', 'k', 0, 0, 0, 12,
                           0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0, 0x00, 0xFF, 0x2F, 0x00};
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView("cerid_cook_audio.mid")),
                                  {smf, sizeof(smf)}));

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView("cerid_cook_audio.mid");
    ctx.meta_path   = crd::containers::StringView("cerid_cook_audio.mid.meta");
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;
    crd::cooker::CookIO ctx_io(ctx.source_path, ctx.meta_path, &g_alloc);
    ctx.io          = &ctx_io;

    const crd::cooker::CookHandlerFn handler =
        crd::cooker::find_cook_handler(crd::containers::StringView(".mid"));
    REQUIRE(handler != nullptr);
    const crd::cooker::CookResult result = handler(ctx);
    REQUIRE(result.ok);
    CHECK(result.type_fourcc == crd::audio::kFourCC_MIDI);

    crd::audio::MidiLoader      loader(&g_alloc);
    crd::resources::LoadContext lctx;
    lctx.id        = ctx.id;
    lctx.bytes     = crd::containers::as_const_span(result.cooked_bytes);
    lctx.allocator = &g_alloc;
    auto* midi     = static_cast<crd::audio::MidiResource*>(loader.load(lctx));
    REQUIRE(midi != nullptr);
    REQUIRE(midi->notes.size() == 1);
    CHECK(midi->notes[0].note == 60);
    CHECK(midi->notes[0].duration == 96);
    loader.unload(midi);

    // a truncated file refuses through the same handler
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView("cerid_cook_audio_bad.mid")),
                                  {smf, 10}));
    crd::cooker::CookContext bad;
    bad.source_path = crd::containers::StringView("cerid_cook_audio_bad.mid");
    bad.meta_path   = crd::containers::StringView("cerid_cook_audio_bad.mid.meta");
    bad.id          = crd::resources::ResourceId::mint_random();
    bad.allocator   = &g_alloc;
    crd::cooker::CookIO bad_io(bad.source_path, bad.meta_path, &g_alloc);
    bad.io          = &bad_io;
    CHECK_FALSE(handler(bad).ok);
}
