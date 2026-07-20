// test_ckir_hair_showcase.cpp — D-007 B18: the hair SHOWCASE. Same renderer as the gate, run large.
//
// Hidden from the default run (the leading '.' in the tag makes Catch2 skip it unless asked) because it is minutes, not
// seconds — it exists to produce imagery, not to gate. Run it with:
//
//     crd-kir-tests.exe "[.showcase]"
//
// WHAT IT DEMONSTRATES. Hair "type" is almost entirely the helical styling operator from the B18-d strand kernel fighting
// gravity: a coil has high curl amplitude, high frequency, and resists droop; straight hair has none and falls. Hair
// COLOUR is the Chiang 2016 melanin pair — eumelanin (brown/black) against pheomelanin (red/yellow) — which is why these
// read as hair rather than as tinted plastic: the pigment lives in the TRANSMISSIVE lobes, so it only shows where light
// passes THROUGH the fibre. That is also why every variant keeps the same strong warm rim light.

#include "hair_render.hpp"

#include <catch2/catch_test_macros.hpp>

namespace hr = hair_render;

namespace
{
// The four canonical hair types, expressed as styling-vs-gravity. Note how little else changes.
[[nodiscard]] hr::HairLook groom_straight()
{
    hr::HairLook l;
    l.curl_amp_lo = 0.004; l.curl_amp_hi = 0.012; // essentially no helix
    l.curl_freq_lo = 0.7;  l.curl_freq_hi = 1.1;
    l.taper = 0.15;
    l.droop = 3.05;                                // nothing resists gravity ⇒ it hangs long and heavy
    l.out_lift = 0.80;                             // lies close to the scalp: straight hair has little volume
    l.beta_m = 0.075;                              // smoother cuticle ⇒ a tighter, glassier primary highlight
    l.beta_n = 0.20;
    return l;
}
[[nodiscard]] hr::HairLook groom_wavy()
{
    hr::HairLook l;
    l.curl_amp_lo = 0.030; l.curl_amp_hi = 0.075;
    l.curl_freq_lo = 1.2;  l.curl_freq_hi = 2.6;
    l.taper = 0.35;
    l.droop = 2.55;
    l.out_lift = 1.00;
    return l;
}
[[nodiscard]] hr::HairLook groom_curly()
{
    hr::HairLook l;
    l.curl_amp_lo = 0.085; l.curl_amp_hi = 0.150;
    l.curl_freq_lo = 3.4;  l.curl_freq_hi = 5.2;
    l.taper = 0.70;                                // the curl HOLDS to the tip instead of relaxing
    l.droop = 1.85;                                // the coil's spring partly beats gravity
    l.out_lift = 1.35;                             // ...which is what gives curly hair its volume
    l.beta_m = 0.115;                              // rougher cuticle ⇒ the highlight breaks up into glints
    l.beta_n = 0.30;
    l.len_jitter = 0.62;
    return l;
}
[[nodiscard]] hr::HairLook groom_coily()
{
    hr::HairLook l;
    l.curl_amp_lo = 0.140; l.curl_amp_hi = 0.215;
    l.curl_freq_lo = 6.5;  l.curl_freq_hi = 9.5;
    l.taper = 0.92;
    l.droop = 1.05;                                // a tight coil barely droops at all
    l.out_lift = 1.70;                             // it stands OFF the scalp — the silhouette is the giveaway
    l.beta_m = 0.135;
    l.beta_n = 0.34;
    l.len_jitter = 0.70;
    return l;
}

// Natural hair colour is a 2-D melanin space, not an RGB picker. These are the canonical corners of it.
struct Pigment
{
    const char* name;
    double      eu;   // eumelanin  — brown/black
    double      ph;   // pheomelanin — red/yellow
    double      exposure;
};
constexpr Pigment kPigments[] = {
    {"black",    8.00, 0.00, 1.55}, // near-total absorption: ALL the look is the R lobe (surface specular)
    {"brown",    1.30, 0.20, 1.10},
    {"auburn",   0.30, 1.50, 1.00}, // pheomelanin-dominant: TT/TRT carry a deep red that only shows lit from behind
    {"blonde",   0.10, 0.20, 0.95},
    {"platinum", 0.02, 0.05, 0.72}, // almost no pigment ⇒ multiple scattering dominates; needs the least exposure
};
} // namespace

TEST_CASE("B18 SHOWCASE: hair types", "[.showcase][kir][hair]")
{
    crd::memory::TlsfAllocator alloc(1024U << 20U);

    hr::SceneConfig sc;
    sc.width      = 760;
    sc.height     = 900;
    // ⭐ MANY THIN LOCKS, not a few fat ones. Every strand in a bundle follows the SAME curve offset by its (u,v) in the
    //    tuft's cross-section, so a bundle is a coherent lock — and a 128-strand bundle 25 px wide renders as one solid
    //    ribbon. Spending the same strand budget on 12x more, 2x thinner tufts is what turns noodles into hair.
    sc.bundles      = 900; // 900 x 72 = 64,800 strands
    sc.per_bundle   = 72;
    sc.bundle_width = 0.070;
    sc.points     = 26;   // more samples: a tight coil needs them or the helix reads as a polyline
    sc.lmap       = 256;
    sc.lfrag      = 56;
    sc.eye        = {1.05, -5.9, 0.70};
    sc.at         = {0.0, 0.0, -0.42};
    sc.verbose    = false;

    struct Entry
    {
        const char*  file;
        hr::HairLook look;
    };
    const Entry entries[4] = {{"build/showcase_straight.bmp", groom_straight()},
                              {"build/showcase_wavy.bmp", groom_wavy()},
                              {"build/showcase_curly.bmp", groom_curly()},
                              {"build/showcase_coily.bmp", groom_coily()}};

    for (const Entry& en : entries)
    {
        hr::HairLook l = en.look;
        l.eumelanin    = 1.30; // one pigment across all four so the comparison isolates SHAPE
        l.pheomelanin  = 0.20;
        sc.exposure    = 1.10;
        crd::containers::Array<double> img(&alloc);
        const hr::Stats                st = hr::render(alloc, sc, l, img);
        hr::write_bmp(en.file, sc.width, sc.height, img);
        std::printf("[showcase] %-32s covered=%6d  partial=%5d (%.0f%%)  mean=%.3f\n", en.file, st.covered, st.partial,
                    st.covered > 0 ? 100.0 * static_cast<double>(st.partial) / static_cast<double>(st.covered) : 0.0, st.mean);
        CHECK(st.covered > 0);
    }
}

TEST_CASE("B18 SHOWCASE: hair colours", "[.showcase][kir][hair]")
{
    crd::memory::TlsfAllocator alloc(1024U << 20U);

    hr::SceneConfig sc;
    sc.width      = 760;
    sc.height     = 900;
    sc.bundles      = 900;
    sc.per_bundle   = 72;
    sc.bundle_width = 0.070;
    sc.points     = 26;
    sc.lmap       = 256;
    sc.lfrag      = 56;
    sc.eye        = {1.05, -5.9, 0.70};
    sc.at         = {0.0, 0.0, -0.42};
    sc.verbose    = false;

    for (const Pigment& pg : kPigments)
    {
        hr::HairLook l = groom_wavy(); // one groom across all five so the comparison isolates PIGMENT
        l.eumelanin    = pg.eu;
        l.pheomelanin  = pg.ph;
        sc.exposure    = pg.exposure;
        crd::containers::Array<double> img(&alloc);
        const hr::Stats                st = hr::render(alloc, sc, l, img);
        char                           path[128];
        std::snprintf(path, sizeof(path), "build/showcase_%s.bmp", pg.name);
        hr::write_bmp(path, sc.width, sc.height, img);
        std::printf("[showcase] %-32s eu=%.2f ph=%.2f  covered=%6d  mean=%.3f\n", path, pg.eu, pg.ph, st.covered, st.mean);
        CHECK(st.covered > 0);
    }
}
