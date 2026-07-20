#pragma once

// hair_swatch.hpp — the HAIR SWATCH scene: strands rooted on a flat patch, the configuration every hair paper renders
// (Marschner 2003 fig. 8, Chiang 2016 fig. 9, d'Eon 2011). A patch isolates the FIBRE MODEL from grooming: no head, no
// scalp, no styling to hide behind — what you see is the scattering model and the sampling, which is the point.
//
// Geometry is generated on the HOST and uploaded once: strands are a chain of swept-sphere segments
// [ax,ay,az,ra, bx,by,bz,rb], the layout ckir_lss.hpp / build_scene_curves consume directly.
//
// ⛔⛔ THE CENTRELINE MUST BE ANALYTICALLY SMOOTH. The first version built stray curvature as a RANDOM WALK — a small
//     random step per control point. That is a zigzag by construction: every point is a corner, and since a swept
//     sphere renders its silhouette EXACTLY, every one of those corners is visible as a hard kink. Raising the segment
//     count made it worse, not better (more corners, and a walk of n steps wanders ∝√n). The fix is not more points,
//     it is a centreline that is smooth as a FUNCTION: every strand here is a sum of sinusoids whose amplitudes,
//     frequencies and phases are randomised PER STRAND, so the curve is C^∞ and the segment count only controls how
//     finely that smooth curve is sampled. Randomness belongs in the parameters, never in the points.
//
// ⭐ WHAT ELSE MAKES A SWATCH READ AS HAIR RATHER THAN AS A BRUSH:
//   · the curl RAMPS IN — hair leaves the scalp nearly straight and tightens along its length; a constant-radius
//     helix from the root looks like a spring, not like hair;
//   · CLUMPING — real hair gathers into tufts that converge, and the dark channels between them give a groom depth;
//   · TAPER — a fibre narrows toward its tip, so the mass fades rather than ends;
//   · FLYAWAYS — a few strands leaving the mass entirely. A perfectly bounded silhouette reads as CG instantly.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hairswatch
{

struct SwatchConfig
{
    // ⭐ FEW AND FAT, NOT MANY AND THIN. A ringlet is legible only when it is a COHESIVE ROPE with visible gaps
    //   either side of it. 200 loose tufts average into an undifferentiated mass; ~40 tight locks read as
    //   individual curls. Density comes from strands-per-lock, separation from lock COUNT.
    // locks are laid out on a JITTERED GRID, not scattered at random: random placement clusters some locks and leaves
    // holes elsewhere, and a ringlet only reads if it has clear space either side of it.
    int    grid_x      = 1;
    int    grid_z      = 1;
    int    per_clump   = 4000; // strands per lock
    int    segments    = 64;   // samples of the SMOOTH centreline (see the note above)
    double patch_w     = 0.098; // root patch extent in x
    double patch_d     = 0.036; // ...and in z
    // ⭐ THE STRANDS HANG. Rooted on an elevated card and falling under gravity, each lock becomes a separate ringlet
    //   with air around it — which is how every hair-swatch reference is shot. Grown UPWARD they lean together and
    //   pack into one mass no matter how tight the locks are.
    double root_y      = 0.62;
    // ⭐ LENGTH IS WHAT MAKES A CURL LEGIBLE. A ringlet needs several loops before the eye reads it as a curl rather
    //   than as texture, and a short strand simply cannot show them. Long strands + real gravity also let the locks
    //   ARC OVER and hang, which separates them; standing straight up they pack into an undifferentiated brush.
    double length      = 0.56; // strand length
    // ⭐ REAL HAIR IS ~35 µm IN RADIUS ON A 30 cm STRAND — a ratio of about 1.2e-4. Against this 0.56 length that is
    //   6.5e-5, and the fibres become SUB-PIXEL at any sane framing. That is not a problem to design around, it is the
    //   actual regime film hair lives in, and it is why high sample counts are not optional: a strand thinner than a
    //   pixel is resolved by coverage statistics, not by geometry. Fat strands are the single biggest tell of CG hair.
    double root_radius = 0.000068;
    double tip_radius  = 0.000032;

    // styling: a helix whose amplitude/frequency define the hair TYPE
    double curl_amp    = 0.0;
    double curl_freq   = 0.0;
    double curl_ramp   = 0.30; // fraction of the length over which the curl reaches full amplitude
    double wave_amp    = 0.0;  // a slower, larger-scale undulation on top of the curl
    double wave_freq   = 0.0;
    double swing       = 0.10; // how far the falling lock drifts forward
    // HIGH convergence is what holds a lock together as a rope. Low values were separating the strands, which is
    // the opposite of what makes a ringlet visible.
    double clump_tight = 0.42; // 0 = no convergence, 1 = strands meet at the tip
    double stray       = 0.010; // amplitude of the smooth low-frequency wander (NOT a per-point step)
    double flyaway_frac = 0.022;
    double flyaway_amp  = 0.035;
    crd::u32 seed      = 0x9E3779B9U;

    [[nodiscard]] int clumps() const { return grid_x * grid_z; }
};

// Generate the swatch into `segs` (8 floats per segment). Returns the segment count.
[[nodiscard]] inline crd::u32 build_swatch(const SwatchConfig& cfg, crd::containers::Array<float>& segs,
                                           crd::containers::Array<float>& tans, crd::memory::IAllocator* scratch)
{
    constexpr double kTau = 6.28318530717958647692;
    const int        nstr = cfg.clumps() * cfg.per_clump;
    const int        nseg = nstr * cfg.segments;
    segs.resize(static_cast<crd::usize>(nseg) * 8U, 0.0F);
    tans.resize(static_cast<crd::usize>(nseg) * 6U, 0.0F);
    crd::containers::Array<double> pts(scratch);
    crd::containers::Array<double> vtan(scratch);

    crd::u32   st  = cfg.seed;
    const auto rnd = [&]() {
        st = st * 1664525U + 1013904223U;
        return static_cast<double>(st >> 8U) / 16777216.0;
    };

    crd::usize o = 0U;
    for (int c = 0; c < cfg.clumps(); ++c)
    {
        const int    gi = c % cfg.grid_x;
        const int    gj = c / cfg.grid_x;
        const double ux = (static_cast<double>(gi) + 0.5) / static_cast<double>(cfg.grid_x) - 0.5;
        const double uz = (static_cast<double>(gj) + 0.5) / static_cast<double>(cfg.grid_z) - 0.5;
        const double jx_ = (rnd() * 2.0 - 1.0) * 0.16 / static_cast<double>(cfg.grid_x);
        const double jz_ = (rnd() * 2.0 - 1.0) * 0.16 / static_cast<double>(cfg.grid_z);
        const double cx   = (ux + jx_) * cfg.patch_w;
        const double cz   = (uz + jz_) * cfg.patch_d;
        const double ctx  = (rnd() * 2.0 - 1.0) * 0.030; // lock-wide lean
        const double ctz  = (rnd() * 2.0 - 1.0) * 0.022;
        const double cph  = rnd() * kTau;                // the LOCK's curl phase — shared, see below
        const double camp = 0.86 + 0.28 * rnd();         // ...its radius
        const double cfrq = 0.92 + 0.16 * rnd();         // ...and its pitch
        const double clen = cfg.length * (0.84 + 0.28 * rnd());

        for (int s = 0; s < cfg.per_clump; ++s)
        {
            const double a0 = rnd() * kTau;
            const double r0 = 0.0090 * crd::math::sqrt(rnd());
            const double rx = cx + r0 * crd::math::cos(a0);
            const double rz = cz + r0 * crd::math::sin(a0);

            // ⛔⛔ THE HELIX BELONGS TO THE LOCK, NOT TO THE STRAND. Curly hair forms RINGLETS: a whole clump spirals
            //     together as one rope, and the hairs inside it stay roughly parallel to their neighbours. Giving each
            //     strand its own phase (this started at ±0.8 rad) destroys that — every hair sweeps its own wide arc,
            //     they cross each other constantly, and the result reads as tangled wire rather than as curls. The
            //     phase and radius are therefore CLUMP properties with only a whisker of per-strand variation, which
            //     is what keeps a ringlet coherent while stopping it from looking extruded.
            //
            // ⛔  AND THE RADIUS MUST BE SMALL. A real ringlet is ~2-3% of the strand's length across; the first pass
            //     used over 10%, so every "curl" was a wide open spiral the size of the swatch itself.
            const double ph    = cph + (rnd() * 2.0 - 1.0) * 0.10;
            const double amp_j = camp * (0.94 + 0.12 * rnd());
            const double frq_j = cfrq * (0.97 + 0.06 * rnd());
            const double len   = clen * (0.88 + 0.24 * rnd());
            const bool   fly   = rnd() < cfg.flyaway_frac;
            const double flyx  = (rnd() * 2.0 - 1.0) * cfg.flyaway_amp;
            const double flyz  = (rnd() * 2.0 - 1.0) * cfg.flyaway_amp;

            // the SMOOTH stray-curvature term: two sinusoids, random per strand. C^∞, so no corner can appear no
            // matter how finely it is sampled — the property the random walk did not have.
            const double s1a = cfg.stray * (0.6 + 0.8 * rnd());
            const double s1f = 0.7 + 0.9 * rnd();
            const double s1p = rnd() * kTau;
            const double s2a = cfg.stray * 0.45 * (0.6 + 0.8 * rnd());
            const double s2f = 1.9 + 1.5 * rnd();
            const double s2p = rnd() * kTau;
            const double s3a = cfg.stray * (0.6 + 0.8 * rnd());
            const double s3f = 0.7 + 0.9 * rnd();
            const double s3p = rnd() * kTau;
            const double s4a = cfg.stray * 0.45 * (0.6 + 0.8 * rnd());
            const double s4f = 1.9 + 1.5 * rnd();
            const double s4p = rnd() * kTau;

            const auto point = [&](double t, double& ox, double& oy, double& oz) {
                // curl ramp: straight at the root, full amplitude by `curl_ramp` — smoothstep so the ramp itself
                // introduces no curvature discontinuity.
                const double rt = t < cfg.curl_ramp ? t / cfg.curl_ramp : 1.0;
                const double rs = rt * rt * (3.0 - 2.0 * rt);
                const double a  = cfg.curl_amp * amp_j * rs;
                const double th = cfg.curl_freq * frq_j * t * kTau + ph;

                const double pull = cfg.clump_tight * t * t;
                const double bx   = rx + (cx - rx) * pull;
                const double bz   = rz + (cz - rz) * pull;

                const double wu = cfg.wave_amp * crd::math::sin(cfg.wave_freq * t * kTau + ph * 0.5);
                const double nx = s1a * crd::math::sin(s1f * t * kTau + s1p) + s2a * crd::math::sin(s2f * t * kTau + s2p);
                const double nz = s3a * crd::math::sin(s3f * t * kTau + s3p) + s4a * crd::math::sin(s4f * t * kTau + s4p);

                ox = bx + a * crd::math::sin(th) + wu + nx * t + ctx * t + (fly ? flyx * t * t : 0.0);
                oy = cfg.root_y - t * len; // rooted on the card, falling
                oz = bz + a * crd::math::cos(th) + nz * t + ctz * t + cfg.swing * t * t + (fly ? flyz * t * t : 0.0);
            };

            // ⭐ SMOOTH TANGENTS, the hair equivalent of vertex normals. Sample the centreline first, then take a
            //   CENTRAL DIFFERENCE at each vertex; the renderer lerps between a segment's two endpoint tangents. Using
            //   the segment direction instead is flat shading, and because the R lobe is only a couple of degrees wide
            //   the flat tangent chops the specular into one bright dash per segment. Analytic thickness does not buy
            //   you a continuous highlight — that needs a continuous tangent FIELD.
            const int np = cfg.segments + 1;
            pts.resize(static_cast<crd::usize>(np) * 3U, 0.0);
            for (int j = 0; j <= cfg.segments; ++j)
            {
                double qx = 0.0;
                double qy = 0.0;
                double qz = 0.0;
                point(static_cast<double>(j) / static_cast<double>(cfg.segments), qx, qy, qz);
                pts[static_cast<crd::usize>(j) * 3U + 0U] = qx;
                pts[static_cast<crd::usize>(j) * 3U + 1U] = qy;
                pts[static_cast<crd::usize>(j) * 3U + 2U] = qz;
            }
            vtan.resize(static_cast<crd::usize>(np) * 3U, 0.0);
            for (int j = 0; j <= cfg.segments; ++j)
            {
                const int    a = j > 0 ? j - 1 : 0;
                const int    b = j < cfg.segments ? j + 1 : cfg.segments;
                double       dx = pts[static_cast<crd::usize>(b) * 3U + 0U] - pts[static_cast<crd::usize>(a) * 3U + 0U];
                double       dy = pts[static_cast<crd::usize>(b) * 3U + 1U] - pts[static_cast<crd::usize>(a) * 3U + 1U];
                double       dz = pts[static_cast<crd::usize>(b) * 3U + 2U] - pts[static_cast<crd::usize>(a) * 3U + 2U];
                const double dl = crd::math::sqrt(dx * dx + dy * dy + dz * dz);
                const double di = dl > 1.0e-20 ? 1.0 / dl : 0.0;
                vtan[static_cast<crd::usize>(j) * 3U + 0U] = dx * di;
                vtan[static_cast<crd::usize>(j) * 3U + 1U] = dy * di;
                vtan[static_cast<crd::usize>(j) * 3U + 2U] = dz * di;
            }

            for (int j = 1; j <= cfg.segments; ++j)
            {
                const double t  = static_cast<double>(j) / static_cast<double>(cfg.segments);
                const double t0 = static_cast<double>(j - 1) / static_cast<double>(cfg.segments);
                const double pr = cfg.root_radius + (cfg.tip_radius - cfg.root_radius) * t0;
                const double nr = cfg.root_radius + (cfg.tip_radius - cfg.root_radius) * t;
                const crd::usize ia = static_cast<crd::usize>(j - 1) * 3U;
                const crd::usize ib = static_cast<crd::usize>(j) * 3U;

                float* q = segs.data() + o;
                q[0] = static_cast<float>(pts[ia + 0U]); q[1] = static_cast<float>(pts[ia + 1U]);
                q[2] = static_cast<float>(pts[ia + 2U]); q[3] = static_cast<float>(pr);
                q[4] = static_cast<float>(pts[ib + 0U]); q[5] = static_cast<float>(pts[ib + 1U]);
                q[6] = static_cast<float>(pts[ib + 2U]); q[7] = static_cast<float>(nr);
                float* w = tans.data() + (o / 8U) * 6U;
                w[0] = static_cast<float>(vtan[ia + 0U]); w[1] = static_cast<float>(vtan[ia + 1U]);
                w[2] = static_cast<float>(vtan[ia + 2U]);
                w[3] = static_cast<float>(vtan[ib + 0U]); w[4] = static_cast<float>(vtan[ib + 1U]);
                w[5] = static_cast<float>(vtan[ib + 2U]);
                o += 8U;
            }
        }
    }
    return static_cast<crd::u32>(nseg);
}

// σₐ from melanin concentrations (Chiang 2016 §4): eumelanin is the brown/black pigment, pheomelanin the red/yellow.
// Real hair colour is ABSORPTION, which is why a "blonde" is not a beige tint but a near-transparent fibre.
inline void melanin_sigma(double eu, double ph, double out[3])
{
    out[0] = eu * 0.419 + ph * 0.187;
    out[1] = eu * 0.697 + ph * 0.400;
    out[2] = eu * 1.370 + ph * 1.050;
}

} // namespace crd::hairswatch
