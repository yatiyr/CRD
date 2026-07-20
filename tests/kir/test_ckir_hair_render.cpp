// test_ckir_hair_render.cpp — D-007 B18: the hair renderer's REGRESSION GATE.
//
// Runs the shared renderer (hair_render.hpp) small and fast and asserts the frame is non-degenerate: real coverage, real
// energy, real tonal range, a substantial partial-coverage population, and a filter that changes the frame without
// rewriting it. A regression that blanks, blows out, or de-antialiases the render fails HERE rather than silently
// shipping garbage.
//
// The pretty pictures live in test_ckir_hair_showcase.cpp — same renderer, larger settings, hidden from the default run.

#include "hair_render.hpp"

#include <catch2/catch_test_macros.hpp>

namespace hr = hair_render;

TEST_CASE("B18: hair beauty frame renders through the CKIR pipeline", "[kir][hair][render]")
{
    crd::memory::TlsfAllocator alloc(512U << 20U);

    hr::SceneConfig sc; // defaults are the gate settings: 640x520, 150 bundles x 128 = 19200 strands
    hr::HairLook    look;

    crd::containers::Array<double> img(&alloc);
    const hr::Stats                st = hr::render(alloc, sc, look, img);

    const double coverage = static_cast<double>(st.covered) / static_cast<double>(sc.width * sc.height);
    std::printf("[B18 hair frame] %dx%d  covered=%d (%.1f%%)  partial=%d (%.1f%%)  peak=%.3f  mean=%.3f  filter=%.4f\n",
                sc.width, sc.height, st.covered, coverage * 100.0, st.partial,
                st.covered > 0 ? 100.0 * static_cast<double>(st.partial) / static_cast<double>(st.covered) : 0.0,
                st.peak, st.mean, st.filter_delta);
    hr::write_bmp("build/hair_beauty.bmp", sc.width, sc.height, img);

    // non-degenerate frame: real coverage, real energy, real tonal range
    CHECK(coverage > 0.04);
    CHECK(coverage < 0.85);
    CHECK(st.peak > 0.05);
    CHECK(st.mean > 0.02);
    CHECK(st.mean < 0.85);
    // A groom this fine must produce a substantial PARTIAL-coverage population — those pixels are the silhouette and the
    // flyaways. If it did not, strands would be rendering as opaque quads and the silhouette would be a staircase that no
    // downstream filter could repair (the depth guard deliberately refuses to blend hair with background).
    CHECK(st.partial > st.covered / 20);
    // The filter must CHANGE the frame (or it is not running) but must not rewrite it (or it is just a blur).
    CHECK(st.filter_delta > 1.0e-4);
    CHECK(st.filter_delta < 0.05);
}
