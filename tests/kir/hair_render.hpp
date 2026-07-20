#pragma once

// hair_render.hpp — D-007 B18: the shared HAIR RENDERER used by both the regression gate and the showcase.
//
// One renderer, two callers. The gate (test_ckir_hair_render.cpp) runs it small and fast and asserts the frame is
// non-degenerate; the showcase (test_ckir_hair_showcase.cpp) runs it large and slow to produce actual imagery across hair
// types and colours. Keeping a single implementation is the point — a showcase that drifts from the gated path proves
// nothing about the engine.
//
// Everything on screen comes from Cerid: strands from the B18-d hair-mesh kernel (crd::kir::hairgeom), shading from the
// B18-a/b Chiang BCSDF (crd::kir::hair) through the CKIR CPU path, self-shadow from the B18-c deep-opacity-map kernels
// (crd::kir::hairms), and the B18-e compositing filter over the top.
//
// ⚠ WHY THIS IS CPU-BOUND, and what that does NOT mean. The strand count here is limited by the CPU tree-walking oracle
//   (~10 us per shaded pixel through eval_cpu), NOT by anything about hair rendering. On a GPU the same work is trivial:
//   the deferred design exists precisely because 1px-wide strands waste ~75% of a hardware rasterizer's lanes on 2x2
//   helper invocations, so we rasterize in compute instead. Production grooms run 100K-150K render strands generated on
//   the fly from a few thousand guides (TressFX 4 / UE5 Groom / Frostbite); our hair-mesh path goes further and stores
//   nothing at all — a strand is a (u,v) regenerated per pass. The ceiling below is a test-harness budget, nothing more.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_hair.hpp>
#include <crd/kir/ckir_hair_geom.hpp>
#include <crd/kir/ckir_hair_scatter.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>

namespace hair_render
{
namespace kir = crd::kir;

struct V3d
{
    double x = 0;
    double y = 0;
    double z = 0;
};
inline V3d    operator+(V3d a, V3d b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3d    operator-(V3d a, V3d b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3d    operator*(V3d a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(V3d a, V3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3d    cross(V3d a, V3d b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3d    norm(V3d a)
{
    const double l = crd::math::sqrt(dot(a, a));
    return l > 1e-12 ? a * (1.0 / l) : V3d{0, 0, 1};
}

// ── MELANIN (Chiang 2016 Eq 9) — the physically-grounded way to author hair colour. Real hair pigment is two compounds:
//    EUMELANIN (brown/black) and PHEOMELANIN (red/yellow). Every natural hair colour is a point in that 2-D space, which is
//    why picking "hair colours" as RGB always looks wrong — absorption is spectral and the transmissive lobes carry it.
[[nodiscard]] inline V3d melanin_sigma(double eumelanin, double pheomelanin)
{
    return {eumelanin * 0.419 + pheomelanin * 0.187, eumelanin * 0.697 + pheomelanin * 0.400,
            eumelanin * 1.370 + pheomelanin * 1.050};
}

// A hair LOOK: pigment + fibre roughness + groom shape. Hair "type" (straight/wavy/curly/coily) is almost entirely the
// helical styling operator from the strand kernel plus how much gravity wins over the curl's spring.
struct HairLook
{
    const char* name = "blonde-wavy";
    double      eumelanin   = 0.10;
    double      pheomelanin = 0.20;
    // ROUGHNESS AND THE SPECKLE. Each pixel resolves ONE strand, so it sees that strand's single fibre offset h rather
    // than the average over h a dense groom would give. A very tight lobe therefore turns the primary highlight into
    // scattered white glints instead of a coherent sheen — it reads as grizzled, frosted hair. Broadening the lobes is the
    // honest fix at this strands-per-pixel ratio: it reduces the same variance more strands would have averaged away.
    double      beta_m      = 0.155; // longitudinal roughness
    double      beta_n      = 0.325; // azimuthal roughness
    double      alpha       = 2.8;  // cuticle scale tilt (degrees) — what SEPARATES the R and TRT highlights
    double      curl_amp_lo = 0.030;
    double      curl_amp_hi = 0.075;
    double      curl_freq_lo = 1.2;
    double      curl_freq_hi = 2.6;
    double      taper       = 0.35; // curl amplitude at the tip (0 = curl relaxes into a straight tip)
    double      droop       = 2.55; // gravity pull; a tight coil resists it, straight hair does not
    double      out_lift    = 1.00; // how far the groom stands off the scalp (volume)
    double      len_jitter  = 0.50;
    double      tip_scatter = 0.22; // per-STRAND length spread inside a tuft; a uniform tip reads as a flat cut
};

struct SceneConfig
{
    int width      = 640;
    int height     = 520;
    int bundles    = 150;
    int per_bundle = 128;
    int points     = 20;
    int layers     = 5;
    int lmap       = 176; // deep-opacity-map resolution in light space
    int lfrag      = 32;  // fragments per DOM cell
    int dom_layers = 4;

    double hair_radius    = 0.0016; // WORLD radius of one fibre — sub-pixel, which is why coverage must be partial
    double bundle_width   = 0.135; // half-width of a tuft's cross-section. With N strands over this width, strand SPACING
                                   // is what decides whether a tuft reads as fibres or as a solid ribbon — see below.
    double melanin_jitter = 0.35;   // per-strand pigment variation; without it a groom reads as plastic
    double root_darken    = 0.55;   // extra pigment at the root (real hair is darker where it is thickest/newest)
    bool   draw_head      = true;

    V3d    eye{0.9, -5.4, 0.55};
    V3d    at{0.0, 0.0, -0.35};
    double flen = 1.55;

    // 3-point rig. The RIM is the important one: it drives TT (light transmitted THROUGH the fibre), and TT is the lobe
    // that carries the pigment's colour — so a strong warm rim is what makes blonde read as blonde instead of as white
    // specular. The KEY gives the R highlight; the FILL keeps the shadow side from going black.
    V3d    key_dir{-0.62, -0.68, 0.55};
    V3d    key_col{1.00, 0.90, 0.76};
    double key_int = 2.4;
    V3d    rim_dir{0.32, 0.86, 0.45};
    V3d    rim_col{1.00, 0.74, 0.42};
    double rim_int = 6.8;
    V3d    fill_dir{0.80, -0.35, -0.20};
    V3d    fill_col{0.55, 0.62, 0.80};
    double fill_int = 0.22;

    double exposure = 0.95;

    // SUPERSAMPLING - the fix for hair that reads as PEN STROKES. A deferred G-buffer keeps ONE strand per pixel (the
    // depth-test winner), but at production density ~148 strands actually overlap every covered pixel. Shading one of
    // them is a hard sample of a stochastic signal, so each strand appears as a discrete hard-edged line: a drawing.
    // Real strand renderers average many samples per pixel; nothing downstream can recover what the depth test discarded,
    // which is why the compositing filter could never fix this - it can only smear the one sample it was given.
    // Rendering at N x and box-resolving gives N*N independent strand samples per output pixel.
    int supersample = 1;

    // ── THE COMPOSITING FILTER'S REACH. Lipp's sigma_par = 4 / radius = 5 is calibrated for 1 spp deferred rendering,
    //   where a pixel holds ONE strand and there are real GAPS to bridge. At high strand density there are no gaps, so
    //   that reach stops being reconstruction and becomes a directional SMEAR along the tangent - which is exactly the
    //   "oil painting / brush stroke" look. At density the filter's only job is anti-aliasing, so it wants ~1 px.
    double filter_sigma_par   = 4.0;
    double filter_sigma_perp  = 1.0;
    double filter_sigma_color = 0.9;
    int    filter_radius      = 5;

    // Per-fragment opacity deposited into the deep-opacity map. This must fall as strand count RISES, or the groom
    // self-shadows itself into a black mass: total optical depth per light cell scales with fragments per cell.
    double dom_alpha = 0.105;
    // Inter-fibre multiple scattering gain. Light hair owes most of its brightness to light bouncing BETWEEN fibres,
    // not to the single-fibre lobes, so too small a value reads as a dark waxy shell with a lit rim.
    double ms_gain = 0.045;
    bool   verbose  = true;

    // ── WHERE THE KERNELS RUN. Every CKIR kernel in this renderer goes through this hook. Left null it uses the CPU
    //    oracle, which is what the regression gate wants (no GPU required, deterministic, and the oracle IS the
    //    reference). The showcase supplies a GPU implementation instead and runs the very same graphs on the device —
    //    same emitters, same kernels the B18-a/b/c/e dispatch gates verify against this oracle.
    //    The oracle is a tree-walking interpreter at ~10 us per shaded lane; the GPU does the same work in microseconds.
    using KernelDispatchFn = void (*)(void* ctx, kir::KGraph& g, const kir::KEntry& e, kir::KernelBuffer* bufs, int nbufs,
                                      crd::u32 groups);
    void*            dispatch_ctx = nullptr;
    KernelDispatchFn dispatch     = nullptr;
};

inline void run_kernel(const SceneConfig& sc, kir::KGraph& g, const kir::KEntry& e, kir::KernelBuffer* bufs, int nbufs,
                       crd::u32 groups, crd::memory::IAllocator& scratch)
{
    if (sc.dispatch != nullptr) { sc.dispatch(sc.dispatch_ctx, g, e, bufs, nbufs, groups); }
    else { kir::eval_cpu_kernel(g, e, bufs, nbufs, e.local_size[0], &scratch, groups); }
}

struct Stats
{
    int    covered      = 0;
    int    partial      = 0;
    double peak         = 0.0;
    double mean         = 0.0;
    double filter_delta = 0.0;
};

// Minimal 24-bit BMP writer (bottom-up BGR) — no third-party image dependency for an LDR preview.
inline void write_bmp(const char* path, int w, int h, const crd::containers::Array<double>& rgb)
{
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path, "wb") != 0) { f = nullptr; } // MSVC treats the deprecated fopen as an error under /WX
#else
    f = std::fopen(path, "wb");
#endif
    if (f == nullptr) { return; }
    const int     rowsz  = ((w * 3 + 3) / 4) * 4;
    const int     imgsz  = rowsz * h;
    const int     filesz = 54 + imgsz;
    unsigned char hdr[54] = {};
    hdr[0]  = 'B';
    hdr[1]  = 'M';
    hdr[2]  = static_cast<unsigned char>(filesz & 0xFF);
    hdr[3]  = static_cast<unsigned char>((filesz >> 8) & 0xFF);
    hdr[4]  = static_cast<unsigned char>((filesz >> 16) & 0xFF);
    hdr[5]  = static_cast<unsigned char>((filesz >> 24) & 0xFF);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = static_cast<unsigned char>(w & 0xFF);
    hdr[19] = static_cast<unsigned char>((w >> 8) & 0xFF);
    hdr[22] = static_cast<unsigned char>(h & 0xFF);
    hdr[23] = static_cast<unsigned char>((h >> 8) & 0xFF);
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = static_cast<unsigned char>(imgsz & 0xFF);
    hdr[35] = static_cast<unsigned char>((imgsz >> 8) & 0xFF);
    hdr[36] = static_cast<unsigned char>((imgsz >> 16) & 0xFF);
    std::fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; --y)
    {
        for (int x = 0; x < w; ++x)
        {
            unsigned char px[3];
            for (int c = 0; c < 3; ++c)
            {
                double v = rgb[static_cast<crd::usize>(y) * static_cast<crd::usize>(w) * 3U
                               + static_cast<crd::usize>(x) * 3U + static_cast<crd::usize>(2 - c)]; // BGR
                if (v < 0.0) { v = 0.0; }
                if (v > 1.0) { v = 1.0; }
                px[c] = static_cast<unsigned char>(crd::math::lround(v * 255.0));
            }
            std::fwrite(px, 1, 3, f);
        }
        const unsigned char pad[3] = {0, 0, 0};
        std::fwrite(pad, 1, static_cast<size_t>(rowsz - w * 3), f);
    }
    std::fclose(f);
}

// Render one groom. `img` receives width*height*3 tonemapped RGB in [0,1].
inline Stats render(crd::memory::IAllocator& alloc, const SceneConfig& sc, const HairLook& look,
                    crd::containers::Array<double>& img)
{
    using crd::containers::Array;
    const int    ss  = sc.supersample > 1 ? sc.supersample : 1;
    const int    kW  = sc.width * ss;
    const int    kH  = sc.height * ss;
    const int    kPts = sc.points;
    const int    kLMap = sc.lmap;
    const int    kLFrag = sc.lfrag;
    const auto   uz  = [](int v) { return static_cast<crd::usize>(v); };
    const crd::usize npix = uz(kW) * uz(kH);
    Stats        st;

    // ── 1. GROOM ────────────────────────────────────────────────────────────────────────────────────────────────────
    kir::hairgeom::StrandGenConfig gcfg;
    gcfg.layers = sc.layers;
    gcfg.points = kPts;
    gcfg.taper  = look.taper;

    const int strand_count = sc.bundles * sc.per_bundle;
    Array<double> pos(&alloc);
    Array<double> tng(&alloc);
    Array<double> wpar(&alloc);
    Array<double> pos_x(&alloc);
    pos.resize(uz(strand_count) * uz(kPts) * 6U, 0.0); // the kernel writes [pos.xyz, tan.xyz] interleaved
    pos_x.resize(uz(strand_count) * uz(kPts) * 3U, 0.0);
    tng.resize(uz(strand_count) * uz(kPts) * 3U, 0.0);
    wpar.resize(uz(strand_count) * uz(kPts), 0.0);

    crd::u32   rs  = 1234567U;
    const auto rnd = [&]() {
        rs = rs * 1664525U + 1013904223U;
        return static_cast<double>(rs >> 8) / static_cast<double>(1U << 24);
    };

    // ── Build EVERY tuft's layer geometry and EVERY strand's styling up front, then generate the whole groom in ONE
    //    dispatch. Previously this was one dispatch per tuft, each paying a full buffer-create + upload + submit + fence +
    //    readback (~25 ms) — 900 of those dominated the frame. Nothing about the maths changed; only the batching.
    Array<double> lay(&alloc);
    Array<double> spar(&alloc);
    lay.resize(uz(sc.bundles) * uz(sc.layers) * 4U * 3U, 0.0);
    spar.resize(uz(strand_count) * 6U, 0.0);

    for (int b = 0; b < sc.bundles; ++b)
    {
        // Tufts over the SCALP DOME on a golden-angle spiral (even coverage, no seams), each falling under gravity while
        // drifting off the head — the shape a real groom takes.
        const double uu    = (static_cast<double>(b) + 0.5) / static_cast<double>(sc.bundles);
        const double polar = crd::math::acos(1.0 - uu * 0.92); // 0 = crown, grows toward the hairline
        const double azim  = static_cast<double>(b) * 2.39996323;
        const V3d    nrm{crd::math::sin(polar) * crd::math::cos(azim), crd::math::sin(polar) * crd::math::sin(azim),
                      crd::math::cos(polar)};
        const V3d    root = nrm * 1.0;
        const V3d    e1   = norm(cross(nrm, crd::math::abs(nrm.z) < 0.85 ? V3d{0, 0, 1} : V3d{1, 0, 0}));
        const V3d    e2   = cross(nrm, e1);
        const double jlen   = (1.0 - look.len_jitter * 0.5) + look.len_jitter * rnd();
        const double jout   = 0.85 + 0.4 * rnd();
        const double jtwist = (rnd() - 0.5) * 0.5;

        for (int i = 0; i < sc.layers; ++i)
        {
            const double t     = static_cast<double>(i) / static_cast<double>(sc.layers - 1);
            const double droop = look.droop * jlen * t * t; // gravity dominates further from the root
            const double outw  = (0.10 + 0.34 * t) * jout * look.out_lift;
            const double wid   = sc.bundle_width * (1.0 - 0.42 * t);
            const double tw    = jtwist * t;
            const V3d    ctr   = root + nrm * outw + V3d{0, 0, -droop};
            const V3d    a1    = e1 * crd::math::cos(tw) + e2 * crd::math::sin(tw);
            const V3d    a2    = e2 * crd::math::cos(tw) - e1 * crd::math::sin(tw);
            const double cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int c = 0; c < 4; ++c)
            {
                const V3d        pv = ctr + a1 * (cs[c][0] * wid) + a2 * (cs[c][1] * wid);
                const crd::usize o  = (uz(b) * uz(sc.layers) * 4U + uz(i * 4 + c)) * 3U;
                lay[o + 0U] = pv.x;
                lay[o + 1U] = pv.y;
                lay[o + 2U] = pv.z;
            }
        }

        // ⭐ PER-STRAND styling. Amplitude, frequency, PHASE and length are now buffer data, so no two strands in a tuft
        //    coil in lockstep. That single decorrelation is what stops a bundle rendering as one swept ribbon, and it is
        //    also why the whole groom is one shader instead of one per tuft.
        for (int k = 0; k < sc.per_bundle; ++k)
        {
            const crd::usize o = (uz(b) * uz(sc.per_bundle) + uz(k)) * 6U;
            spar[o + 0U] = rnd();                                                               // u
            spar[o + 1U] = rnd();                                                               // v
            spar[o + 2U] = look.curl_amp_lo + (look.curl_amp_hi - look.curl_amp_lo) * rnd();    // amplitude
            spar[o + 3U] = look.curl_freq_lo + (look.curl_freq_hi - look.curl_freq_lo) * rnd(); // frequency
            spar[o + 4U] = rnd() * 2.0 * 3.14159265358979323846;                                // PHASE
            spar[o + 5U] = 1.0 - look.tip_scatter * rnd();                                      // length fraction
        }
    }

    {
        kir::hairgeom::StrandGenConfig gs = gcfg;
        gs.bundles          = sc.bundles;
        gs.per_bundle       = sc.per_bundle;
        gs.per_strand_style = true;
        kir::KGraph       g(&alloc);
        const kir::KEntry e     = kir::hairgeom::build_strand_gen_kernel(g, gs);
        kir::KernelBuffer bb[3] = {{lay.data(), sc.bundles * sc.layers * 4 * 3, 0, 0},
                                   {spar.data(), strand_count * 6, 0, 1},
                                   {pos.data(), strand_count * kPts * 6, 0, 2}}; // interleaved [pos.xyz, tan.xyz]
        const crd::u32    grp   = (static_cast<crd::u32>(strand_count) + e.local_size[0] - 1U) / e.local_size[0];
        run_kernel(sc, g, e, bb, 3, grp, alloc);
    }
    // de-interleave into the flat position/tangent arrays the rasterizer walks
    for (int si = 0; si < strand_count; ++si)
    {
        for (int j = 0; j < kPts; ++j)
        {
            const crd::usize so = (uz(si) * uz(kPts) + uz(j)) * 6U;
            const crd::usize go = (uz(si) * uz(kPts) + uz(j)) * 3U;
            for (int c = 0; c < 3; ++c) { tng[go + uz(c)] = pos[so + 3U + uz(c)]; }
            for (int c = 0; c < 3; ++c) { pos_x[go + uz(c)] = pos[so + uz(c)]; }
            wpar[uz(si) * uz(kPts) + uz(j)] = static_cast<double>(j) / static_cast<double>(kPts - 1);
        }
    }
    for (crd::usize i = 0; i < pos_x.size(); ++i) { pos[i] = pos_x[i]; }
    pos.resize(uz(strand_count) * uz(kPts) * 3U, 0.0);


    // ── 2. RASTERIZE ────────────────────────────────────────────────────────────────────────────────────────────────
    const V3d    eye  = sc.eye;
    const V3d    fwd  = norm(sc.at - eye);
    const V3d    rgt  = norm(cross(fwd, V3d{0, 0, 1}));
    const V3d    up   = cross(rgt, fwd);
    const double flen = sc.flen;

    Array<double> zbuf(&alloc);
    Array<double> zhead(&alloc);
    Array<double> gtan(&alloc);
    Array<double> gw(&alloc);
    Array<double> cov(&alloc);
    Array<int>    gid(&alloc);
    zbuf.resize(npix, 1.0e30);
    zhead.resize(npix, 1.0e30);
    gtan.resize(npix * 3U, 0.0);
    gw.resize(npix, 0.0);
    cov.resize(npix, 0.0);
    gid.resize(npix, -1);

    const auto project = [&](V3d p, double& sx, double& sy, double& depth) {
        const V3d d = p - eye;
        depth       = dot(d, fwd);
        if (depth < 1.0e-3) { return false; }
        sx = (dot(d, rgt) / depth) * flen;
        sy = (dot(d, up) / depth) * flen;
        sx = (sx * 0.5 + 0.5) * kW;
        sy = (0.5 - sy * 0.5) * kH;
        return true;
    };
    const auto ray_of = [&](int ix, int iy) {
        const double ndx = ((static_cast<double>(ix) + 0.5) / kW * 2.0 - 1.0) / flen;
        const double ndy = (1.0 - (static_cast<double>(iy) + 0.5) / kH * 2.0) / flen;
        return norm(fwd + rgt * ndx + up * ndy);
    };

    // ── 2a. HEAD. Hair with nothing underneath reads as an object, not a character — and it also removes the depth
    //        reference that tells you the groom has an inside. A scalp ellipsoid, ray-traced per pixel, fixes both and
    //        gives the strands something to occlude.
    Array<double> headrgb(&alloc);
    headrgb.resize(npix * 3U, 0.0);
    if (sc.draw_head)
    {
        const V3d    hc{0.0, 0.0, -0.06};
        const V3d    hr{0.96, 1.02, 1.14}; // an ellipsoid: a sphere reads as a ball, never as a skull
        const V3d    skin{0.76, 0.57, 0.46};
        for (int iy = 0; iy < kH; ++iy)
        {
            for (int ix = 0; ix < kW; ++ix)
            {
                const V3d rd = ray_of(ix, iy);
                // ray-ellipsoid = ray-sphere in the scaled frame
                const V3d o{(eye.x - hc.x) / hr.x, (eye.y - hc.y) / hr.y, (eye.z - hc.z) / hr.z};
                const V3d d{rd.x / hr.x, rd.y / hr.y, rd.z / hr.z};
                const double a  = dot(d, d);
                const double bq = 2.0 * dot(o, d);
                const double c  = dot(o, o) - 1.0;
                const double di = bq * bq - 4.0 * a * c;
                if (di <= 0.0) { continue; }
                const double t = (-bq - crd::math::sqrt(di)) / (2.0 * a);
                if (t <= 1.0e-3) { continue; }
                const V3d wp = eye + rd * t;
                const V3d n  = norm(V3d{(wp.x - hc.x) / (hr.x * hr.x), (wp.y - hc.y) / (hr.y * hr.y),
                                       (wp.z - hc.z) / (hr.z * hr.z)});
                const crd::usize p = uz(iy) * uz(kW) + uz(ix);
                zhead[p]           = dot(wp - eye, fwd);
                // wrap-diffuse skin: cheap subsurface stand-in so the scalp is not a hard-terminator plastic ball
                const auto lam = [&](V3d ld) { return crd::math::max(0.0, (dot(n, ld) + 0.35) / 1.35); };
                const double kl = lam(sc.key_dir) * sc.key_int * 0.16;
                const double rl = lam(sc.rim_dir) * sc.rim_int * 0.05;
                const double fl = lam(sc.fill_dir) * sc.fill_int * 0.5;
                headrgb[p * 3U + 0U] = skin.x * (kl * sc.key_col.x + rl * sc.rim_col.x + fl * sc.fill_col.x);
                headrgb[p * 3U + 1U] = skin.y * (kl * sc.key_col.y + rl * sc.rim_col.y + fl * sc.fill_col.y);
                headrgb[p * 3U + 2U] = skin.z * (kl * sc.key_col.z + rl * sc.rim_col.z + fl * sc.fill_col.z);
            }
        }
    }

    for (int si = 0; si < strand_count; ++si)
    {
        for (int j = 0; j + 1 < kPts; ++j)
        {
            const crd::usize o0 = (uz(si) * uz(kPts) + uz(j)) * 3U;
            const crd::usize o1 = o0 + 3U;
            const V3d        p0{pos[o0], pos[o0 + 1U], pos[o0 + 2U]};
            const V3d        p1{pos[o1], pos[o1 + 1U], pos[o1 + 2U]};
            double           x0 = 0.0;
            double           y0 = 0.0;
            double           d0 = 0.0;
            double           x1 = 0.0;
            double           y1 = 0.0;
            double           d1 = 0.0;
            if (!project(p0, x0, y0, d0) || !project(p1, x1, y1, d1)) { continue; }
            const int steps = static_cast<int>(crd::math::abs(x1 - x0) + crd::math::abs(y1 - y0)) + 2;
            for (int t = 0; t <= steps; ++t)
            {
                const double a  = static_cast<double>(t) / static_cast<double>(steps);
                const double px = x0 + (x1 - x0) * a;
                const double py = y0 + (y1 - y0) * a;
                const double dz = d0 + (d1 - d0) * a;
                // ⚠ SUB-PIXEL POSITION. Snapping to int(px) quantises every strand onto the pixel lattice, leaving a
                //   staircase no downstream filter can undo — opacity alone cannot encode WHERE in the pixel the strand
                //   lies. Splat BILINEARLY over the straddling 2x2: analytic coverage for a thin line.
                const double fx = px - 0.5;
                const double fy = py - 0.5;
                const int    bx = static_cast<int>(crd::math::floor(fx));
                const int    by = static_cast<int>(crd::math::floor(fy));
                const double tx = fx - static_cast<double>(bx);
                const double ty = fy - static_cast<double>(by);
                const double aw = sc.hair_radius * flen * static_cast<double>(kW) / dz;
                const double ac = aw < 1.0 ? aw : 1.0;
                for (int qy = 0; qy < 2; ++qy)
                {
                    for (int qx = 0; qx < 2; ++qx)
                    {
                        const int ix = bx + qx;
                        const int iy = by + qy;
                        if (ix < 0 || ix >= kW || iy < 0 || iy >= kH) { continue; }
                        const double bw = (qx == 0 ? 1.0 - tx : tx) * (qy == 0 ? 1.0 - ty : ty);
                        if (bw < 1.0e-4) { continue; }
                        const crd::usize pi = uz(iy) * uz(kW) + uz(ix);
                        if (dz >= zhead[pi]) { continue; } // occluded by the scalp — contributes neither colour nor alpha
                        // Coverage accrues from EVERY fragment, including ones the depth test discards: an occluded
                        // strand still blocks light through this pixel. 1-prod(1-a) is order-independent.
                        cov[pi] = cov[pi] + (1.0 - cov[pi]) * ac * bw;
                        if (dz >= zbuf[pi]) { continue; }
                        zbuf[pi] = dz;
                        gid[pi]  = si;
                        gw[pi]   = wpar[uz(si) * uz(kPts) + uz(j)] + (1.0 / kPts) * a;
                        for (int c = 0; c < 3; ++c) { gtan[pi * 3U + uz(c)] = tng[o0 + uz(c)]; }
                    }
                }
            }
        }
    }

    Array<int> pix(&alloc);
    for (crd::usize i = 0; i < npix; ++i) { if (gid[i] >= 0) { pix.push_back(static_cast<int>(i)); } }
    const int nc = static_cast<int>(pix.size());
    st.covered   = nc;
    if (nc == 0) { return st; }

    // ── 2b. LIGHT-VIEW DEEP OPACITY MAP — the self-shadow, and the single biggest cue that a groom has VOLUME. ────────
    struct LightMap
    {
        V3d           lx, ly, lz;
        double        minx = 0, miny = 0, inv = 1.0;
        Array<double> dom;
        explicit LightMap(crd::memory::IAllocator& a) : dom(&a) {}
    };
    kir::hairms::DomConfig domc;
    domc.layers       = sc.dom_layers;
    domc.frags_per_px = kLFrag;
    domc.span         = 2.6;
    const int dom_stride = 1 + domc.layers;

    LightMap lm_key(alloc);
    LightMap lm_rim(alloc);
    LightMap lm_fil(alloc);

    const auto build_lightmap = [&](V3d ldir, LightMap& lm) {
        // ⛔ The depth axis must run along the direction light TRAVELS: lz = -ldir. With lz = +ldir the "nearest to light"
        //    reduction selects the FARTHEST surface and the shadow inverts — a bug that looks like a lighting-rig error.
        lm.lz = norm(ldir * -1.0);
        lm.lx = norm(cross(lm.lz, crd::math::abs(lm.lz.z) < 0.9 ? V3d{0, 0, 1} : V3d{1, 0, 0}));
        lm.ly = cross(lm.lz, lm.lx);
        double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
        for (int s = 0; s < strand_count; ++s)
        {
            for (int j = 0; j < kPts; ++j)
            {
                const crd::usize o = (uz(s) * uz(kPts) + uz(j)) * 3U;
                const V3d        p{pos[o], pos[o + 1U], pos[o + 2U]};
                const double     ux = dot(p, lm.lx);
                const double     uy = dot(p, lm.ly);
                if (ux < minx) { minx = ux; }
                if (ux > maxx) { maxx = ux; }
                if (uy < miny) { miny = uy; }
                if (uy > maxy) { maxy = uy; }
            }
        }
        const double pad = 0.06 * crd::math::max(maxx - minx, maxy - miny);
        minx -= pad; miny -= pad; maxx += pad; maxy += pad;
        lm.minx = minx;
        lm.miny = miny;
        lm.inv  = static_cast<double>(kLMap) / crd::math::max(maxx - minx, maxy - miny);

        Array<double> frag(&alloc);
        Array<int>    used(&alloc);
        frag.resize(uz(kLMap) * uz(kLMap) * uz(kLFrag) * 2U, 0.0);
        used.resize(uz(kLMap) * uz(kLMap), 0);
        for (crd::usize c = 0; c < uz(kLMap) * uz(kLMap); ++c)
        {
            for (int k = 0; k < kLFrag; ++k)
            {
                frag[(c * uz(kLFrag) + uz(k)) * 2U + 0U] = 1.0e30; // empty slots must lose the z0 reduction...
                frag[(c * uz(kLFrag) + uz(k)) * 2U + 1U] = 0.0;    // ...and contribute no opacity
            }
        }
        // ⚠ Splat SEGMENTS, not vertices. Depositing only the sampled points leaves the light map a dotted line while the
        //   camera sees a continuous one; pixels landing in the holes come back unshadowed and the frame speckles.
        const auto deposit = [&](int cx, int cy, double dz) {
            if (cx < 0 || cx >= kLMap || cy < 0 || cy >= kLMap) { return; }
            const crd::usize c = uz(cy) * uz(kLMap) + uz(cx);
            int              n = used[c];
            if (n < kLFrag)
            {
                frag[(c * uz(kLFrag) + uz(n)) * 2U + 0U] = dz;
                frag[(c * uz(kLFrag) + uz(n)) * 2U + 1U] = sc.dom_alpha;
                used[c] = n + 1;
                return;
            }
            // ⚠ RETENTION POLICY. At capacity, keep the fragments NEAREST THE LIGHT. Keeping whichever arrived first is
            //   uncorrelated with depth, so neighbouring cells disagree arbitrarily → hard blocks at cell resolution.
            int    worst = 0;
            double wd    = -1e30;
            for (int k = 0; k < kLFrag; ++k)
            {
                const double d = frag[(c * uz(kLFrag) + uz(k)) * 2U + 0U];
                if (d > wd) { wd = d; worst = k; }
            }
            if (dz < wd) { frag[(c * uz(kLFrag) + uz(worst)) * 2U + 0U] = dz; }
        };
        for (int s = 0; s < strand_count; ++s)
        {
            for (int j = 0; j + 1 < kPts; ++j)
            {
                const crd::usize oa = (uz(s) * uz(kPts) + uz(j)) * 3U;
                const V3d        pa{pos[oa], pos[oa + 1U], pos[oa + 2U]};
                const V3d        pb{pos[oa + 3U], pos[oa + 4U], pos[oa + 5U]};
                const double     ax = (dot(pa, lm.lx) - minx) * lm.inv;
                const double     ay = (dot(pa, lm.ly) - miny) * lm.inv;
                const double     bx2 = (dot(pb, lm.lx) - minx) * lm.inv;
                const double     by2 = (dot(pb, lm.ly) - miny) * lm.inv;
                const double     za = dot(pa, lm.lz);
                const double     zb = dot(pb, lm.lz);
                const int steps = static_cast<int>(crd::math::abs(bx2 - ax) + crd::math::abs(by2 - ay)) + 2;
                for (int t = 0; t <= steps; ++t)
                {
                    const double u = static_cast<double>(t) / static_cast<double>(steps);
                    deposit(static_cast<int>(ax + (bx2 - ax) * u), static_cast<int>(ay + (by2 - ay) * u), za + (zb - za) * u);
                }
            }
        }
        if (sc.verbose)
        {
            int filled = 0, sat = 0;
            for (crd::usize c = 0; c < uz(kLMap) * uz(kLMap); ++c)
            {
                if (used[c] > 0) { ++filled; }
                if (used[c] >= kLFrag) { ++sat; }
            }
            std::printf("  [DOM] cells=%d occupied=%d saturated=%d (%.1f%%)\n", kLMap * kLMap, filled, sat,
                        filled > 0 ? 100.0 * static_cast<double>(sat) / static_cast<double>(filled) : 0.0);
        }
        lm.dom.resize(uz(kLMap) * uz(kLMap) * uz(dom_stride), 0.0);
        kir::KGraph       gd(&alloc);
        const kir::KEntry ed    = kir::hairms::build_dom_build_kernel(gd, domc);
        kir::KernelBuffer db[2] = {{frag.data(), kLMap * kLMap * kLFrag * 2, 0, 0},
                                   {lm.dom.data(), kLMap * kLMap * dom_stride, 0, 1}};
        run_kernel(sc, gd, ed, db, 2, static_cast<crd::u32>((kLMap * kLMap + 63) / 64), alloc);
    };

    // ── 3. SHADE ────────────────────────────────────────────────────────────────────────────────────────────────────
    Array<double> shade(&alloc);
    shade.resize(uz(nc) * 3U, 0.0);
    const V3d base_sigma = melanin_sigma(look.eumelanin, look.pheomelanin);

    for (int pass = 0; pass < 3; ++pass)
    {
        const V3d    ldir  = (pass == 0) ? sc.key_dir : (pass == 1) ? sc.rim_dir : sc.fill_dir;
        const V3d    lcol  = (pass == 0) ? sc.key_col : (pass == 1) ? sc.rim_col : sc.fill_col;
        const double inten = (pass == 0) ? sc.key_int : (pass == 1) ? sc.rim_int : sc.fill_int;
        LightMap&    lm    = (pass == 0) ? lm_key : (pass == 1) ? lm_rim : lm_fil;
        build_lightmap(ldir, lm);

        // ⭐ THE SHIPPED KERNEL, not a second implementation. Shading runs `build_hair_bcsdf_kernel` — the exact graph the
        //    B18-a/b Vulkan and DX12 gates dispatch and compare against the CPU oracle. It takes 6 floats per lane
        //    (sinTo, phiO, sinTi, phiI, h, sigma_a) and returns one, so RGB is THREE dispatches with three per-strand
        //    sigmas. That is not an approximation of the vec3 form: the colour channels of a hair BCSDF couple only
        //    through absorption, so three scalar evaluations reproduce it exactly.
        const int     nlane = ((nc + 63) / 64) * 64; // the kernel has no tail guard, so pad to whole workgroups
        Array<double> bin(&alloc);
        Array<double> bout(&alloc);
        Array<double> fo(&alloc);
        Array<double> sig_c(&alloc);
        bin.resize(uz(nlane) * 6U, 0.0);
        bout.resize(uz(nlane), 0.0);
        fo.resize(uz(nc) * 3U, 0.0);
        sig_c.resize(uz(nc) * 3U, 0.0);

        for (int i = 0; i < nc; ++i)
        {
            const crd::usize p    = uz(pix[uz(i)]);
            const V3d        tanw = norm(V3d{gtan[p * 3U], gtan[p * 3U + 1U], gtan[p * 3U + 2U]});
            const int        ix   = static_cast<int>(p) % kW;
            const int        iy   = static_cast<int>(p) / kW;
            const V3d        vdir = ray_of(ix, iy) * -1.0; // toward the camera
            const V3d        b1   = norm(cross(tanw, crd::math::abs(tanw.z) < 0.9 ? V3d{0, 0, 1} : V3d{1, 0, 0}));
            const V3d        b2   = cross(tanw, b1);
            // near-field fibre offset + pigment, both decorrelated per strand so neighbours never share a highlight
            const int sid = gid[p];
            crd::u32  hh  = static_cast<crd::u32>(sid) * 2654435761U + 12345U;
            hh ^= hh >> 15; hh *= 2246822519U; hh ^= hh >> 13;
            crd::u32 hp = static_cast<crd::u32>(sid) * 374761393U + 668265263U;
            hp ^= hp >> 13; hp *= 1274126177U; hp ^= hp >> 16;
            const double jit  = 1.0 + sc.melanin_jitter * ((static_cast<double>(hp >> 8) / static_cast<double>(1U << 24)) * 2.0 - 1.0);
            const double root = 1.0 + sc.root_darken * (1.0 - gw[p]); // darker at the root, lighter at the tip
            const double kk   = jit * root;
            sig_c[uz(i) * 3U + 0U] = base_sigma.x * kk;
            sig_c[uz(i) * 3U + 1U] = base_sigma.y * kk;
            sig_c[uz(i) * 3U + 2U] = base_sigma.z * kk;

            const crd::usize o = uz(i) * 6U;
            bin[o + 0U] = dot(vdir, tanw);                                  // sinThetaO
            bin[o + 1U] = crd::math::atan2(dot(vdir, b2), dot(vdir, b1));   // phiO
            bin[o + 2U] = dot(ldir, tanw);                                  // sinThetaI
            bin[o + 3U] = crd::math::atan2(dot(ldir, b2), dot(ldir, b1));   // phiI
            bin[o + 4U] = (static_cast<double>(hh >> 8) / static_cast<double>(1U << 24)) * 2.0 - 1.0; // h
        }

        kir::hair::HairKernelConfig hk;
        hk.eta       = 1.55;
        hk.beta_m    = look.beta_m;
        hk.beta_n    = look.beta_n;
        hk.alpha_deg = look.alpha;
        for (int c = 0; c < 3; ++c)
        {
            for (int i = 0; i < nc; ++i) { bin[uz(i) * 6U + 5U] = sig_c[uz(i) * 3U + uz(c)]; }
            kir::KGraph       gb(&alloc);
            const kir::KEntry eb    = kir::hair::build_hair_bcsdf_kernel(gb, hk);
            kir::KernelBuffer bb[2] = {{bin.data(), nlane * 6, 0, 0}, {bout.data(), nlane, 0, 1}};
            run_kernel(sc, gb, eb, bb, 2, static_cast<crd::u32>(nlane / 64), alloc);
            for (int i = 0; i < nc; ++i) { fo[uz(i) * 3U + uz(c)] = bout[uz(i)]; }
        }

        // ── SELF-SHADOW via the B18-c DOM lookup kernel, 4-tap bilinear PCF (a single nearest-cell tap makes transmittance
        //    jump at cell borders — classic unfiltered shadow-map blockiness, which projects to multi-pixel blocks).
        const int     nq = ((nc * 4 + 63) / 64) * 64;
        Array<double> qry(&alloc), qout(&alloc), fracx(&alloc), fracy(&alloc);
        qry.resize(uz(nq) * 2U, 0.0);
        qout.resize(uz(nq) * 2U, 0.0);
        fracx.resize(nc, 0.0);
        fracy.resize(nc, 0.0);
        for (int i = 0; i < nc; ++i)
        {
            const crd::usize p   = uz(pix[uz(i)]);
            const V3d        wp  = eye + ray_of(static_cast<int>(p) % kW, static_cast<int>(p) / kW) * zbuf[p];
            const double     gx  = (dot(wp, lm.lx) - lm.minx) * lm.inv - 0.5;
            const double     gy  = (dot(wp, lm.ly) - lm.miny) * lm.inv - 0.5;
            const int        bx  = static_cast<int>(gx < 0.0 ? gx - 1.0 : gx);
            const int        by  = static_cast<int>(gy < 0.0 ? gy - 1.0 : gy);
            fracx[uz(i)] = gx - static_cast<double>(bx);
            fracy[uz(i)] = gy - static_cast<double>(by);
            const double ld = dot(wp, lm.lz);
            for (int k = 0; k < 4; ++k)
            {
                int tx = bx + (k & 1);
                int ty = by + (k >> 1);
                if (tx < 0) { tx = 0; }
                if (tx >= kLMap) { tx = kLMap - 1; }
                if (ty < 0) { ty = 0; }
                if (ty >= kLMap) { ty = kLMap - 1; }
                const crd::usize qi = (uz(i) * 4U + uz(k)) * 2U;
                qry[qi + 0U] = static_cast<double>(ty * kLMap + tx);
                qry[qi + 1U] = ld;
            }
        }
        {
            kir::KGraph       gq(&alloc);
            const kir::KEntry eq    = kir::hairms::build_dom_lookup_kernel(gq, domc);
            kir::KernelBuffer qb[3] = {{lm.dom.data(), kLMap * kLMap * dom_stride, 0, 0},
                                       {qry.data(), nq * 2, 0, 1},
                                       {qout.data(), nq * 2, 0, 2}};
            run_kernel(sc, gq, eq, qb, 3, static_cast<crd::u32>(nq / 64), alloc);
        }

        for (int i = 0; i < nc; ++i)
        {
            const crd::usize p    = uz(pix[uz(i)]);
            const V3d        tanw = norm(V3d{gtan[p * 3U], gtan[p * 3U + 1U], gtan[p * 3U + 2U]});
            const double     ct   = crd::math::sqrt(crd::math::abs(1.0 - dot(ldir, tanw) * dot(ldir, tanw)));
            const double     fx2  = fracx[uz(i)];
            const double     fy2  = fracy[uz(i)];
            const double     t00  = qout[(uz(i) * 4U + 0U) * 2U];
            const double     t10  = qout[(uz(i) * 4U + 1U) * 2U];
            const double     t01  = qout[(uz(i) * 4U + 2U) * 2U];
            const double     t11  = qout[(uz(i) * 4U + 3U) * 2U];
            const double     occ  = (t00 * (1.0 - fx2) + t10 * fx2) * (1.0 - fy2) + (t01 * (1.0 - fx2) + t11 * fx2) * fy2;
            // MULTIPLE-SCATTERING FILL: light hair owes most of its brightness to light bouncing BETWEEN fibres, not to
            // the single-fibre lobes. A wrap-diffuse term tinted by the fibre's own transmission stands in for the B18-c
            // dual-scattering tier, and is scaled by the pigment so black hair does not glow.
            const double wrap = 0.5 + 0.5 * ct;
            // The inter-fibre bounce is TINTED by how far light gets through the pigment, and that tint must be strong:
            // a weak exponent makes every groom trend to cream regardless of melanin, which is the classic "wax" look.
            const double tint[3] = {crd::math::exp(-base_sigma.x * 3.2), crd::math::exp(-base_sigma.y * 3.2),
                                    crd::math::exp(-base_sigma.z * 3.2)};
            for (int c = 0; c < 3; ++c)
            {
                const double lc = (c == 0) ? lcol.x : (c == 1) ? lcol.y : lcol.z;
                double       vv = fo[uz(i) * 3U + uz(c)];
                if (!(vv == vv) || vv < 0.0) { vv = 0.0; } // NaN/negative guard at grazing configurations
                const double direct = vv * ct * lc * inten * occ;
                const double ms     = sc.ms_gain * tint[c] * lc * wrap * inten * occ;
                shade[uz(i) * 3U + uz(c)] += direct + ms;
            }
        }
    }

    // ── 4. COMPOSITE ────────────────────────────────────────────────────────────────────────────────────────────────
    img.resize(npix * 3U, 0.0);
    for (int y = 0; y < kH; ++y)
    {
        for (int x = 0; x < kW; ++x)
        {
            const double     t = static_cast<double>(y) / kH;
            const crd::usize o = (uz(y) * uz(kW) + uz(x)) * 3U;
            img[o + 0U] = 0.021 + 0.028 * (1.0 - t);
            img[o + 1U] = 0.024 + 0.034 * (1.0 - t);
            img[o + 2U] = 0.036 + 0.052 * (1.0 - t);
        }
    }
    if (sc.draw_head)
    {
        for (crd::usize p = 0; p < npix; ++p)
        {
            if (zhead[p] < 1.0e29)
            {
                for (int c = 0; c < 3; ++c) { img[p * 3U + uz(c)] = headrgb[p * 3U + uz(c)]; }
            }
        }
    }
    for (int i = 0; i < nc; ++i)
    {
        const crd::usize p = uz(pix[uz(i)]);
        const double     a = cov[p] < 1.0 ? cov[p] : 1.0;
        if (a > 0.05 && a < 0.95) { ++st.partial; }
        for (int c = 0; c < 3; ++c)
        {
            const double v = shade[uz(i) * 3U + uz(c)];
            // OVER, not overwrite — this is what antialiases the silhouette. The depth-guarded filter below deliberately
            // refuses to blend hair with background, so if coverage were binary NO pass could ever soften that edge.
            img[p * 3U + uz(c)] = v * a + img[p * 3U + uz(c)] * (1.0 - a);
            if (v > st.peak) { st.peak = v; }
        }
    }

    for (crd::usize i = 0; i < npix * 3U; ++i)
    {
        double v = img[i] * sc.exposure;
        v        = v * (1.0 + v * 0.24) / (1.0 + v); // shouldered Reinhard: retains contrast in the mid-tones
        v        = crd::math::pow(v, 1.0 / 2.2);
        img[i]   = v;
        st.mean += v;
    }
    st.mean /= static_cast<double>(npix * 3U);

    // ── 5. B18-e COMPOSITING FILTER ─────────────────────────────────────────────────────────────────────────────────
    // ⚠ Runs in TONEMAPPED space on purpose: the paper's sigma_c = 0.9 presumes display-range values, and in linear HDR
    //   every colour difference would swamp it — the filter would silently no-op exactly where hair is brightest.
    // ⚠ HARNESS NOTE: this evaluates the filter formula directly rather than through eval_cpu_kernel. The shipped CKIR
    //   kernel `build_hair_filter_kernel` is the gated artefact (5 CPU gates + Vulkan and DX12 dispatch gates); the CPU
    //   oracle is ~11 ms/pixel, so a full frame through it is roughly an hour. On GPU these taps are sub-millisecond.
    {
        kir::hairgeom::HairFilterConfig fcfg;
        fcfg.width        = kW;
        fcfg.height       = kH;
        fcfg.depth_reject = 0.06; // WORLD units here (zbuf stores view distance, not normalised depth)
        fcfg.sigma_par    = sc.filter_sigma_par;
        fcfg.sigma_perp   = sc.filter_sigma_perp;
        fcfg.sigma_color  = sc.filter_sigma_color;
        fcfg.radius       = sc.filter_radius;

        Array<double> fcol(&alloc), ftan(&alloc), fdep(&alloc);
        fcol.resize(npix * 3U, 0.0);
        ftan.resize(npix * 2U, 0.0);
        fdep.resize(npix, 0.0);
        for (crd::usize p = 0; p < npix; ++p)
        {
            for (int c = 0; c < 3; ++c) { fcol[p * 3U + uz(c)] = img[p * 3U + uz(c)]; }
            const bool hashair = zbuf[p] < 1.0e29;
            fdep[p]            = hashair ? zbuf[p] : 1.0e3;
            const V3d    tw    = {gtan[p * 3U + 0U], gtan[p * 3U + 1U], gtan[p * 3U + 2U]};
            double       tsx   = dot(tw, rgt);
            double       tsy   = dot(tw, up);
            const double tl    = crd::math::sqrt(tsx * tsx + tsy * tsy);
            if (!hashair || tl < 1.0e-9) { tsx = 1.0; tsy = 0.0; }
            else { tsx /= tl; tsy /= tl; }
            ftan[p * 2U + 0U] = tsx;
            ftan[p * 2U + 1U] = tsy;
        }
        const int    frad = fcfg.radius;
        const double isp  = 1.0 / (fcfg.sigma_par * fcfg.sigma_par);
        const double isq  = 1.0 / (fcfg.sigma_perp * fcfg.sigma_perp);
        const double isc  = 1.0 / (fcfg.sigma_color * fcfg.sigma_color);
        double       moved = 0.0;
        for (int y = 0; y < kH; ++y)
        {
            for (int x = 0; x < kW; ++x)
            {
                const crd::usize p = uz(y) * uz(kW) + uz(x);
                // Background pixels all carry the same far depth, so the guard lets them gather ONLY each other - and the
                // sky is a smooth gradient, so a normalised symmetric kernel returns what was already there. Skipping is
                // equivalent, not an approximation, and it is ~5x of the filter's cost at this coverage.
                if (fdep[p] > 9.0e2) { continue; }
                const double tsx_p = ftan[p * 2U + 0U];
                const double     tsy_p = ftan[p * 2U + 1U];
                double           acc[3] = {0.0, 0.0, 0.0};
                double           wsum   = 0.0;
                for (int dy = -frad; dy <= frad; ++dy)
                {
                    for (int dx = -frad; dx <= frad; ++dx)
                    {
                        const int qx = x + dx;
                        const int qy = y + dy;
                        if (qx < 0 || qx >= kW || qy < 0 || qy >= kH) { continue; }
                        const crd::usize q = uz(qy) * uz(kW) + uz(qx);
                        if (crd::math::abs(fdep[q] - fdep[p]) >= fcfg.depth_reject) { continue; }
                        const double dpar  = static_cast<double>(dx) * tsx_p + static_cast<double>(dy) * tsy_p;
                        const double dperp = static_cast<double>(dx) * (-tsy_p) + static_cast<double>(dy) * tsx_p;
                        double       cd2   = 0.0;
                        for (int c = 0; c < 3; ++c)
                        {
                            const double d = fcol[q * 3U + uz(c)] - fcol[p * 3U + uz(c)];
                            cd2 += d * d;
                        }
                        const double w = crd::math::exp(-(dpar * dpar * isp + dperp * dperp * isq)) * crd::math::exp(-cd2 * isc);
                        for (int c = 0; c < 3; ++c) { acc[c] += w * fcol[q * 3U + uz(c)]; }
                        wsum += w;
                    }
                }
                const double nw = wsum > 1.0e-8 ? wsum : 1.0e-8;
                for (int c = 0; c < 3; ++c)
                {
                    const double nv = acc[c] / nw;
                    moved += crd::math::abs(nv - img[p * 3U + uz(c)]);
                    img[p * 3U + uz(c)] = nv;
                }
            }
        }
        st.filter_delta = moved / static_cast<double>(npix * 3U);
    }

    // ── 6. RESOLVE. Average in LINEAR light (undo the gamma, average, re-apply): averaging display-encoded values
    //    darkens every edge, which on a groom made almost entirely of edges reads as a dirty silhouette.
    if (ss > 1)
    {
        Array<double> down(&alloc);
        down.resize(uz(sc.width) * uz(sc.height) * 3U, 0.0);
        const double inv = 1.0 / static_cast<double>(ss * ss);
        for (int y = 0; y < sc.height; ++y)
        {
            for (int x = 0; x < sc.width; ++x)
            {
                double acc[3] = {0.0, 0.0, 0.0};
                for (int sy = 0; sy < ss; ++sy)
                {
                    for (int sx = 0; sx < ss; ++sx)
                    {
                        const crd::usize sp = uz(y * ss + sy) * uz(kW) + uz(x * ss + sx);
                        for (int c = 0; c < 3; ++c) { acc[c] += crd::math::pow(img[sp * 3U + uz(c)], 2.2); }
                    }
                }
                const crd::usize dp = uz(y) * uz(sc.width) + uz(x);
                for (int c = 0; c < 3; ++c) { down[dp * 3U + uz(c)] = crd::math::pow(acc[c] * inv, 1.0 / 2.2); }
            }
        }
        img.resize(uz(sc.width) * uz(sc.height) * 3U, 0.0);
        for (crd::usize i = 0; i < down.size(); ++i) { img[i] = down[i]; }
    }
    return st;
}

} // namespace hair_render
