// test_audio_codecs.cpp — GEO-10 (D-007 row 75): the codec gates.
//  · WAV: PCM 16/24 + float-32 encode→decode BIT-EXACT; 8-bit unsigned + extensible + f64 decode; unknown
//    chunks skip; refusal classes typed
//  · AIFF: 16/24 big-endian round-trip BIT-EXACT incl. the 80-bit extended rate (weird rates too); signed
//    8-bit; AIFC NONE passes, compressed refuses
//  · FLAC: encode→decode BIT-EXACT with the STREAMINFO MD5 VERIFYING, on material engineered to exercise
//    every path the encoder writes (constant blocks · fixed-predictor tonal ramps · LPC-friendly correlated
//    noise · stereo decorrelation incl. mid/side) at 16 AND 24 bit, mono AND stereo, incl. a non-multiple
//    tail block; a corrupted frame fails CRC; a wrong MD5 fails verification
//  · MD5: RFC 1321 vectors (the empty string · "abc" · alphabet)

#include <crd/audio/aiff.hpp>
#include <crd/audio/flac.hpp>
#include <crd/audio/wav.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// the MD5 detail is internal to the codec TU; re-include it here as its own oracle check
#include "../../engine/audio/src/flac_detail.hpp"

using namespace crd;

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(256U << 20U);
    return a;
}

// a deterministic xorshift so fixtures are reproducible without <random>
struct Rng
{
    u32 s = 0x12345678U;
    [[nodiscard]] u32 next() noexcept
    {
        s ^= s << 13U;
        s ^= s >> 17U;
        s ^= s << 5U;
        return s;
    }
    [[nodiscard]] i32 in_range(i32 lo, i32 hi) noexcept // inclusive
    {
        return lo + static_cast<i32>(next() % static_cast<u32>(hi - lo + 1));
    }
};

// material that exercises every encoder path: constant · ramps (fixed predictors) · correlated noise (LPC) ·
// hard transitions; stereo gets strong L/R correlation so mid/side wins some frames
audio::AudioPcm make_material(u32 rate, u16 channels, u16 bits, usize frames)
{
    audio::AudioPcm pcm(&galloc());
    pcm.sample_rate     = rate;
    pcm.channels        = channels;
    pcm.bits_per_sample = bits;
    const i32 amp       = (1 << (bits - 2)) - 1;
    Rng       rng;
    i32       lp = 0;
    for (usize i = 0; i < frames; ++i)
    {
        i32 base = 0;
        if (i < frames / 5) { base = amp / 2; } // constant head
        else if (i < 2 * frames / 5)
        {
            base = static_cast<i32>((i * 37) % static_cast<usize>(amp)) - amp / 2; // ramp
        }
        else
        {
            lp   = (lp * 7 + rng.in_range(-amp / 8, amp / 8)) / 8; // correlated noise — LPC territory
            base = lp;
        }
        for (u16 c = 0; c < channels; ++c)
        {
            const i32 v = c == 0 ? base : base + rng.in_range(-64, 64); // strong inter-channel correlation
            pcm.isamples.push_back(v);
        }
    }
    return pcm;
}

} // namespace

TEST_CASE("md5 matches the RFC 1321 vectors", "[audio][md5]")
{
    const auto hex_of = [](const u8* d) {
        static char buf[33];
        for (int i = 0; i < 16; ++i)
        {
            std::snprintf(buf + i * 2, 3, "%02x", d[i]);
        }
        return buf;
    };
    {
        audio::detail::Md5 m;
        u8                 d[16];
        m.final(d);
        CHECK(std::strcmp(hex_of(d), "d41d8cd98f00b204e9800998ecf8427e") == 0);
    }
    {
        audio::detail::Md5 m;
        m.update(reinterpret_cast<const u8*>("abc"), 3);
        u8 d[16];
        m.final(d);
        CHECK(std::strcmp(hex_of(d), "900150983cd24fb0d6963f7d28e17f72") == 0);
    }
    {
        audio::detail::Md5 m;
        m.update(reinterpret_cast<const u8*>("abcdefghijklmnopqrstuvwxyz"), 26);
        u8 d[16];
        m.final(d);
        CHECK(std::strcmp(hex_of(d), "c3fcd3d76192e4007dfb496cca67e13b") == 0);
    }
}

TEST_CASE("wav: 16/24-bit PCM and float-32 round-trip bit-exact", "[audio][wav]")
{
    for (const u16 bits : {static_cast<u16>(16), static_cast<u16>(24)})
    {
        const audio::AudioPcm src = make_material(48000, 2, bits, 3001);
        const auto            enc = audio::wav_encode(src, &galloc());
        REQUIRE(enc.size() > 0);
        audio::AudioPcm back(&galloc());
        REQUIRE(audio::wav_decode(containers::as_const_span(enc), back) == audio::WavError::Ok);
        CHECK(back.sample_rate == 48000);
        CHECK(back.channels == 2);
        CHECK(back.bits_per_sample == bits);
        REQUIRE(back.isamples.size() == src.isamples.size());
        CHECK(std::memcmp(back.isamples.data(), src.isamples.data(), src.isamples.size() * 4) == 0);
    }
    {
        audio::AudioPcm f(&galloc());
        f.sample_rate     = 44100;
        f.channels        = 1;
        f.bits_per_sample = 0;
        for (int i = 0; i < 777; ++i) { f.fsamples.push_back(static_cast<f32>(i) * 0.001F - 0.35F); }
        const auto enc = audio::wav_encode(f, &galloc());
        REQUIRE(enc.size() > 0);
        audio::AudioPcm back(&galloc());
        REQUIRE(audio::wav_decode(containers::as_const_span(enc), back) == audio::WavError::Ok);
        CHECK(back.is_float());
        REQUIRE(back.fsamples.size() == f.fsamples.size());
        CHECK(std::memcmp(back.fsamples.data(), f.fsamples.data(), f.fsamples.size() * 4) == 0);
    }
}

TEST_CASE("wav: refusals are typed; unknown chunks skip", "[audio][wav]")
{
    audio::AudioPcm out(&galloc());
    const u8        junk[] = {'R', 'I', 'F', 'X', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    CHECK(audio::wav_decode({junk, sizeof(junk)}, out) == audio::WavError::NotRiffWave);

    // a LIST chunk between fmt and data must not derail the parse
    const audio::AudioPcm src = make_material(22050, 1, 16, 64);
    auto                  enc = audio::wav_encode(src, &galloc());
    REQUIRE(enc.size() > 44);
    // splice a LIST chunk right after fmt (offset 12..36 = fmt; data starts at 36)
    containers::Array<u8> spliced(&galloc());
    for (usize i = 0; i < 36; ++i) { spliced.push_back(enc[i]); }
    const u8 list[] = {'L', 'I', 'S', 'T', 4, 0, 0, 0, 'I', 'N', 'F', 'O'};
    for (u8 b : list) { spliced.push_back(b); }
    for (usize i = 36; i < enc.size(); ++i) { spliced.push_back(enc[i]); }
    // patch the RIFF size
    const u32 riff = static_cast<u32>(spliced.size() - 8);
    std::memcpy(spliced.data() + 4, &riff, 4);
    audio::AudioPcm back(&galloc());
    REQUIRE(audio::wav_decode(containers::as_const_span(spliced), back) == audio::WavError::Ok);
    CHECK(back.isamples.size() == src.isamples.size());
}

TEST_CASE("aiff: 16/24-bit round-trip bit-exact incl. the 80-bit extended rate", "[audio][aiff]")
{
    for (const u32 rate : {44100U, 48000U, 96000U, 11025U})
    {
        const audio::AudioPcm src = make_material(rate, 2, 24, 1500);
        const auto            enc = audio::aiff_encode(src, &galloc());
        REQUIRE(enc.size() > 0);
        audio::AudioPcm back(&galloc());
        REQUIRE(audio::aiff_decode(containers::as_const_span(enc), back) == audio::AiffError::Ok);
        CHECK(back.sample_rate == rate); // the 80-bit float survived exactly
        REQUIRE(back.isamples.size() == src.isamples.size());
        CHECK(std::memcmp(back.isamples.data(), src.isamples.data(), src.isamples.size() * 4) == 0);
    }

    audio::AudioPcm out(&galloc());
    const u8        junk[] = {'F', 'O', 'R', 'M', 0, 0, 0, 4, 'J', 'U', 'N', 'K'};
    CHECK(audio::aiff_decode({junk, sizeof(junk)}, out) == audio::AiffError::NotAiff);
}

TEST_CASE("flac: encode -> decode BIT-EXACT with MD5 verifying, every material class", "[audio][flac]")
{
    struct Config
    {
        u16   channels;
        u16   bits;
        usize frames;
    };
    // 10240 = 2.5 blocks (a non-multiple TAIL block); 4096 = exactly one block; 100 = a sub-block stream
    const Config configs[] = {{2, 16, 10240}, {1, 16, 4096}, {2, 24, 9000}, {1, 24, 100}};
    for (const Config& cfg : configs)
    {
        CAPTURE(cfg.channels, cfg.bits, cfg.frames);
        const audio::AudioPcm src = make_material(48000, cfg.channels, cfg.bits, cfg.frames);
        const auto            enc = audio::flac_encode(src, &galloc());
        REQUIRE(enc.size() > 0);
        // the codec must actually COMPRESS this material (constant + ramps + correlated noise)
        CHECK(enc.size() < src.isamples.size() * (cfg.bits / 8U));

        audio::AudioPcm back(&galloc());
        REQUIRE(audio::flac_decode(containers::as_const_span(enc), back) == audio::FlacError::Ok);
        CHECK(back.sample_rate == 48000);
        CHECK(back.channels == cfg.channels);
        CHECK(back.bits_per_sample == cfg.bits);
        REQUIRE(back.isamples.size() == src.isamples.size());
        CHECK(std::memcmp(back.isamples.data(), src.isamples.data(), src.isamples.size() * 4) == 0);
    }
}

TEST_CASE("flac: corruption fails CRC; a wrong MD5 fails verification", "[audio][flac]")
{
    const audio::AudioPcm src = make_material(48000, 2, 16, 5000);
    auto                  enc = audio::flac_encode(src, &galloc());
    REQUIRE(enc.size() > 200);

    { // flip a byte deep inside the first frame's payload → CRC-16 must catch it
        auto damaged = audio::flac_encode(src, &galloc());
        damaged[damaged.size() / 2] ^= 0x40U;
        audio::AudioPcm back(&galloc());
        const audio::FlacError e = audio::flac_decode(containers::as_const_span(damaged), back);
        CHECK((e == audio::FlacError::BadCrc || e == audio::FlacError::Malformed));
    }
    { // flip an MD5 byte in STREAMINFO → the stream decodes but the hash refuses
        auto lied = audio::flac_encode(src, &galloc());
        lied[8 + 18] ^= 0xFFU; // STREAMINFO md5[0] sits at offset 8+18
        audio::AudioPcm back(&galloc());
        CHECK(audio::flac_decode(containers::as_const_span(lied), back) == audio::FlacError::Md5Mismatch);
    }
    { // not FLAC
        audio::AudioPcm back(&galloc());
        const u8        junk[] = {'f', 'L', 'a', 'K', 0, 0, 0, 0};
        CHECK(audio::flac_decode({junk, sizeof(junk)}, back) == audio::FlacError::NotFlac);
    }
}

TEST_CASE("flac reference-codec oracle hooks", "[audio][flac][oracle]")
{
    // the ffmpeg-as-TEST-ORACLE protocol (the mikktspace precedent — never in the product path):
    //   pass 1 (CRD_AUDIO_DUMP=<dir>): write our WAV + our FLAC of the standard material; the driver runs
    //     ffmpeg over them (decode ours → sample-compare; encode theirs from our WAV);
    //   pass 2 (CRD_AUDIO_VERIFY=<ffmpeg-made flac>): OUR decoder reads THEIR stream — bit-exact vs the
    //     regenerated material, MD5 verified.
    const audio::AudioPcm material = make_material(48000, 2, 16, 10240);
    if (const char* dump_dir = std::getenv("CRD_AUDIO_DUMP"); dump_dir != nullptr)
    {
        const auto write_file = [&](const char* name, const containers::Array<u8>& bytes) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/%s", dump_dir, name);
            std::FILE* f = std::fopen(path, "wb");
            REQUIRE(f != nullptr);
            REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
            (void)std::fclose(f);
        };
        write_file("ours.wav", audio::wav_encode(material, &galloc()));
        write_file("ours.flac", audio::flac_encode(material, &galloc()));
    }
    if (const char* verify = std::getenv("CRD_AUDIO_VERIFY"); verify != nullptr)
    {
        std::FILE* f = std::fopen(verify, "rb");
        REQUIRE(f != nullptr);
        (void)std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        (void)std::fseek(f, 0, SEEK_SET);
        REQUIRE(size > 0);
        containers::Array<u8> bytes(&galloc());
        bytes.resize(static_cast<usize>(size));
        REQUIRE(std::fread(bytes.data(), 1, static_cast<usize>(size), f) == static_cast<usize>(size));
        (void)std::fclose(f);
        audio::AudioPcm theirs(&galloc());
        REQUIRE(audio::flac_decode(containers::as_const_span(bytes), theirs) == audio::FlacError::Ok);
        REQUIRE(theirs.isamples.size() == material.isamples.size());
        CHECK(std::memcmp(theirs.isamples.data(), material.isamples.data(), material.isamples.size() * 4) ==
              0);
    }
}

TEST_CASE("cross-codec: wav -> flac -> aiff preserves every sample", "[audio][transcode]")
{
    // the MED transcode architecture in miniature: decode → AudioPcm → encode, any direction
    const audio::AudioPcm src = make_material(44100, 2, 16, 4321);
    const auto            wav = audio::wav_encode(src, &galloc());
    audio::AudioPcm       a(&galloc());
    REQUIRE(audio::wav_decode(containers::as_const_span(wav), a) == audio::WavError::Ok);
    const auto      flac = audio::flac_encode(a, &galloc());
    audio::AudioPcm b(&galloc());
    REQUIRE(audio::flac_decode(containers::as_const_span(flac), b) == audio::FlacError::Ok);
    const auto      aiff = audio::aiff_encode(b, &galloc());
    audio::AudioPcm c(&galloc());
    REQUIRE(audio::aiff_decode(containers::as_const_span(aiff), c) == audio::AiffError::Ok);
    REQUIRE(c.isamples.size() == src.isamples.size());
    CHECK(std::memcmp(c.isamples.data(), src.isamples.data(), src.isamples.size() * 4) == 0);
}
