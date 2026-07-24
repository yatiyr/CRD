// audio_cook.cpp — GEO-10 (D-007 row 75): the audio cook handlers — .wav/.aiff/.flac → 'ABUF' (decoded,
// normalized f32 — the processing domain; the codec suite owns the bit-exact integer domain) and .mid →
// 'MIDI' (the MIDI 2.0-native model). CookIO-only reads; deterministic artifacts (GEO-6).

#include <crd/audio/aiff.hpp>
#include <crd/audio/audio_resources.hpp>
#include <crd/audio/flac.hpp>
#include <crd/audio/midi.hpp>
#include <crd/audio/wav.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>

#include <cstring>

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kAudioHandlerVersion = 1U;

[[nodiscard]] bool ends_with(crd::containers::StringView path, const char* suffix)
{
    const crd::usize n = std::strlen(suffix);
    if (path.size() < n) { return false; }
    for (crd::usize i = 0; i < n; ++i)
    {
        char c = path[path.size() - n + i];
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
        if (c != suffix[i]) { return false; }
    }
    return true;
}

CookResult audio_buffer_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);
    result.type_fourcc     = crd::audio::kFourCC_ABUF;
    result.handler_version = kAudioHandlerVersion;
    if (ctx.io == nullptr) { return result; }
    crd::containers::Array<crd::u8> src(ctx.allocator);
    if (!ctx.io->read_source(src)) { return result; }

    crd::audio::AudioPcm pcm(ctx.allocator);
    bool                 decoded = false;
    if (ends_with(ctx.source_path, ".wav"))
    {
        decoded = crd::audio::wav_decode(crd::containers::as_const_span(src), pcm) == crd::audio::WavError::Ok;
    }
    else if (ends_with(ctx.source_path, ".aiff") || ends_with(ctx.source_path, ".aif"))
    {
        decoded =
            crd::audio::aiff_decode(crd::containers::as_const_span(src), pcm) == crd::audio::AiffError::Ok;
    }
    else if (ends_with(ctx.source_path, ".flac"))
    {
        decoded =
            crd::audio::flac_decode(crd::containers::as_const_span(src), pcm) == crd::audio::FlacError::Ok;
    }
    if (!decoded) { return result; }

    result.cooked_bytes = crd::audio::audio_buffer_build(pcm, ctx.id, ctx.allocator);
    result.ok           = result.cooked_bytes.size() > 0;
    return result;
}

CookResult midi_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);
    result.type_fourcc     = crd::audio::kFourCC_MIDI;
    result.handler_version = kAudioHandlerVersion;
    if (ctx.io == nullptr) { return result; }
    crd::containers::Array<crd::u8> src(ctx.allocator);
    if (!ctx.io->read_source(src)) { return result; }

    crd::audio::MidiResource midi(ctx.allocator);
    if (crd::audio::midi_parse_smf(crd::containers::as_const_span(src), midi) != crd::audio::MidiError::Ok)
    {
        return result;
    }
    result.cooked_bytes = crd::audio::midi_build(midi, ctx.id, ctx.allocator);
    result.ok           = result.cooked_bytes.size() > 0;
    return result;
}

} // namespace

void register_audio_handlers()
{
    register_cook_handler(".wav", audio_buffer_handler, kAudioHandlerVersion);
    register_cook_handler(".aiff", audio_buffer_handler, kAudioHandlerVersion);
    register_cook_handler(".aif", audio_buffer_handler, kAudioHandlerVersion);
    register_cook_handler(".flac", audio_buffer_handler, kAudioHandlerVersion);
    register_cook_handler(".mid", midi_handler, kAudioHandlerVersion);
}

} // namespace crd::cooker
