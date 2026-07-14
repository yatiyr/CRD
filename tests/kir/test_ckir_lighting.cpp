// test_ckir_lighting.cpp — D-007 B8-a: the CKIR lighting library's Cook-Torrance core (crd::kir::lighting) proven BIT-EXACT
// against a faithful C++ transcription of Filament's surface_brdf.fs on the CPU oracle. The graph runs in F64 (constants
// derive dtype from operands), isolating the operation structure. `brdf_direct` integrates D_GGX · V_SmithGGXCorrelated ·
// F_Schlick · Fd_Burley · the Karis DFG · Kulla-Conty energy compensation — matching it end-to-end validates every term.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_cook.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_lighting.hpp>
#include <crd/kir/ckir_material.hpp>
#include <crd/kir/ckir_post.hpp>
#include <crd/kir/ckir_render.hpp>
#include <crd/kir/ckir_bloom.hpp>
#include <crd/kir/ckir_cinematic.hpp>
#include <crd/kir/ckir_finish.hpp>
#include <crd/kir/ckir_screen.hpp>
#include <crd/kir/ckir_taa.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;
namespace lt  = crd::kir::lighting;
namespace ck  = crd::kir::cook;
namespace mat = crd::kir::material;
namespace rn  = crd::kir::render;
namespace scr = crd::kir::screen;
namespace pst = crd::kir::post;
namespace taa = crd::kir::taa;
namespace blm = crd::kir::bloom;
namespace ci  = crd::kir::cinematic;
namespace fin = crd::kir::finish;

namespace
{
constexpr int kN  = 20;
constexpr double kPi = lt::kPi;

double rclamp01(double x) { const double lo = x > 0.0 ? x : 0.0; return lo < 1.0 ? lo : 1.0; }
double rpow5(double x) { const double x2 = x * x; return x2 * x2 * x; }
double rdot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; } // oracle dot order

// the reference BRDF — a line-by-line port of Filament surface_brdf.fs + surface_shading_model_standard.fs.
double ref_brdf(const double base[3], double metallic, double perceptual, const double n[3], const double v[3], const double l[3], const double lc[3], int c)
{
    double f0[3];
    double diff[3];
    for (int i = 0; i < 3; ++i)
    {
        f0[i]   = 0.04 * (1.0 - metallic) + base[i] * metallic; // mix(0.04, base, metallic)
        diff[i] = base[i] * (1.0 - metallic);
    }
    const double f90 = rclamp01(f0[0] * (50.0 * 0.33) + f0[1] * (50.0 * 0.33) + f0[2] * (50.0 * 0.33));
    const double alpha = perceptual * perceptual;

    double h[3] = {v[0] + l[0], v[1] + l[1], v[2] + l[2]};
    const double hl = crd::math::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    h[0] /= hl;
    h[1] /= hl;
    h[2] /= hl;
    const double noh = rclamp01(rdot3(n, h));
    const double dnv = rdot3(n, v);
    const double nov = (dnv < 0.0 ? -dnv : dnv) + 1e-5;
    const double nol = rclamp01(rdot3(n, l));
    const double loh = rclamp01(rdot3(l, h));

    const double a  = noh * alpha;
    const double dd = ((alpha / ((1.0 - noh * noh) + a * a)) < (453.5) ? (alpha / ((1.0 - noh * noh) + a * a)) : (453.5));
    const double dterm = dd * (dd * (1.0 / kPi));

    const double a2 = alpha * alpha;
    const double lv = nol * crd::math::sqrt((nov - a2 * nov) * nov + a2);
    const double ll = nov * crd::math::sqrt((nol - a2 * nol) * nol + a2);
    const double vterm = 0.5 / ((lv + ll) > (0.0000077) ? (lv + ll) : (0.0000077));

    const double fterm = f0[c] + (f90 - f0[c]) * rpow5(1.0 - loh);
    const double fr    = (dterm * vterm) * fterm;

    const double f90b = 0.5 + 2.0 * alpha * (loh * loh);
    const double ls   = 1.0 + (f90b - 1.0) * rpow5(1.0 - nol);
    const double vs   = 1.0 + (f90b - 1.0) * rpow5(1.0 - nov);
    const double fd   = diff[c] * (ls * vs * (1.0 / kPi));

    // Karis env-BRDF approx → (scale, bias); energy compensation uses bias.
    const double rx = perceptual * -1.0 + 1.0;
    const double ry = perceptual * -0.0275 + 0.0425;
    const double rw = perceptual * 0.022 + (-0.04);
    const double rx2  = rx * rx;
    const double e2   = crd::math::exp2(-9.28 * nov);
    const double a004 = (rx2 < e2 ? rx2 : e2) * rx + ry;
    const double bias0 = 1.04 * a004 + rw;
    const double bias  = bias0 > 1.0e-3 ? bias0 : 1.0e-3; // guard: the analytic DFG bias undershoots negative at high roughness (see energy_compensation)
    const double ecmp  = 1.0 + f0[c] * (1.0 / bias - 1.0);

    return (fd + fr * ecmp) * nol * lc[c];
}
} // namespace

TEST_CASE("B8-a: lighting brdf_direct (Cook-Torrance GGX + multiscatter) bit-exact vs Filament", "[kir][lighting][brdf]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int br = g.input(sh, kir::DType::F64);
    const int bg = g.input(sh, kir::DType::F64);
    const int bb = g.input(sh, kir::DType::F64);
    const int me = g.input(sh, kir::DType::F64);
    const int ro = g.input(sh, kir::DType::F64);
    const int nx = g.input(sh, kir::DType::F64);
    const int ny = g.input(sh, kir::DType::F64);
    const int nz = g.input(sh, kir::DType::F64);
    const int vx = g.input(sh, kir::DType::F64);
    const int vy = g.input(sh, kir::DType::F64);
    const int vz = g.input(sh, kir::DType::F64);
    const int lx = g.input(sh, kir::DType::F64);
    const int ly = g.input(sh, kir::DType::F64);
    const int lz = g.input(sh, kir::DType::F64);

    double bv[3][kN];
    double mev[kN];
    double rov[kN];
    double nrm[3][kN];
    double vw[3][kN];
    double lw[3][kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        bv[0][i] = 0.05 + 0.045 * i; // base rgb in (0,1)
        bv[1][i] = 0.9 - 0.03 * i;
        bv[2][i] = 0.2 + 0.02 * i;
        mev[i]   = 0.02 + 0.045 * i;  // metallic [0.02, 0.87]
        rov[i]   = 0.08 + 0.043 * i;  // perceptual roughness [0.08, 0.9]
        nrm[0][i] = 0.1 * i - 0.9;    nrm[1][i] = 0.4;                 nrm[2][i] = 1.0;
        vw[0][i]  = 0.2;              vw[1][i]  = 0.1 * i - 0.8;       vw[2][i]  = 1.2;
        lw[0][i]  = 0.6 - 0.05 * i;   lw[1][i]  = 0.5;                 lw[2][i]  = 0.9 + 0.02 * i;
        unit(nrm[0][i], nrm[1][i], nrm[2][i]);
        unit(vw[0][i], vw[1][i], vw[2][i]);
        unit(lw[0][i], lw[1][i], lw[2][i]);
    }
    const double* inp[] = {bv[0], bv[1], bv[2], mev, rov, nrm[0], nrm[1], nrm[2], vw[0], vw[1], vw[2], lw[0], lw[1], lw[2]};

    const int base = g.vec3(br, bg, bb);
    const int nn   = g.vec3(nx, ny, nz);
    const int vv   = g.vec3(vx, vy, vz);
    const int llv  = g.vec3(lx, ly, lz);
    const int lcol = g.vec3(g.constant(3.0, sh, kir::DType::F64), g.constant(2.5, sh, kir::DType::F64), g.constant(2.0, sh, kir::DType::F64)); // light colour·intensity

    const double lc[3] = {3.0, 2.5, 2.0};
    const int    node  = lt::brdf_direct(g, base, me, ro, nn, vv, llv, lcol);

    double o[kN * 3];
    kir::eval_cpu(g, inp, &alloc, node, o);
    int bad = 0;
    for (int i = 0; i < kN; ++i)
    {
        const double base_i[3] = {bv[0][i], bv[1][i], bv[2][i]};
        const double n_i[3]    = {nrm[0][i], nrm[1][i], nrm[2][i]};
        const double v_i[3]    = {vw[0][i], vw[1][i], vw[2][i]};
        const double l_i[3]    = {lw[0][i], lw[1][i], lw[2][i]};
        for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref_brdf(base_i, mev[i], rov[i], n_i, v_i, l_i, lc, c)) { ++bad; } }
    }
    CHECK(bad == 0);
}

namespace
{
double rmax2(double a, double b) { return a > b ? a : b; }
double rlen3(double x, double y, double z) { return crd::math::sqrt(x * x + y * y + z * z); }

double ref_aniso(const double f0[3], double f90, double alpha, double aniso, const double t[3], const double b[3], const double n[3], const double v[3], const double l[3], int c)
{
    const double at = rmax2(alpha * (1.0 + aniso), 0.007);
    const double ab = rmax2(alpha * (1.0 - aniso), 0.007);
    double h[3] = {v[0] + l[0], v[1] + l[1], v[2] + l[2]};
    const double hl = rlen3(h[0], h[1], h[2]);
    h[0] /= hl; h[1] /= hl; h[2] /= hl;
    const double dnv = rdot3(n, v);
    const double nov = (dnv < 0.0 ? -dnv : dnv) + 1e-5;
    const double nol = rclamp01(rdot3(n, l));
    const double noh = rclamp01(rdot3(n, h));
    const double loh = rclamp01(rdot3(l, h));
    const double toh = rdot3(t, h);
    const double boh = rdot3(b, h);
    const double a2  = at * ab;
    const double d2  = (ab * toh) * (ab * toh) + (at * boh) * (at * boh) + (a2 * noh) * (a2 * noh);
    const double b2  = a2 / d2;
    const double dterm = a2 * b2 * b2 * (1.0 / kPi);
    const double lv = nol * rlen3(at * rdot3(t, v), ab * rdot3(b, v), nov);
    const double ll = nov * rlen3(at * rdot3(t, l), ab * rdot3(b, l), nol);
    const double vterm = 0.5 / rmax2(lv + ll, 0.0000077);
    return (dterm * vterm) * (f0[c] + (f90 - f0[c]) * rpow5(1.0 - loh));
}
double ref_sheen(const double color[3], double alpha, double nov, double nol, double noh, int c)
{
    const double inva  = 1.0 / alpha;
    const double sin2h = rmax2(1.0 - noh * noh, 0.0078125);
    const double dc    = (2.0 + inva) * crd::math::pow(sin2h, inva * 0.5) / (2.0 * kPi);
    const double vn    = 1.0 / rmax2(4.0 * (nol + nov - nol * nov), 0.00001532);
    return (dc * vn) * color[c];
}
double ref_clearcoat(double weight, double coat_perceptual, double noh, double loh)
{
    const double alpha = coat_perceptual * coat_perceptual;
    const double a  = noh * alpha;
    const double kk = alpha / ((1.0 - noh * noh) + a * a);
    const double dd = (kk < 453.5 ? kk : 453.5);
    const double dterm = dd * (dd * (1.0 / kPi));
    const double vk    = 0.25 / rmax2(loh * loh, 0.0000039);
    const double fcc   = (0.04 + (1.0 - 0.04) * rpow5(1.0 - loh)) * weight;
    return dterm * vk * fcc;
}
double ref_ss(const double color[3], double power, double thickness, double nol, const double v[3], const double l[3], int c)
{
    const double nl[3] = {-l[0], -l[1], -l[2]};
    const double svoh  = rclamp01(rdot3(v, nl));
    const double fwd   = crd::math::exp2(svoh * power - power);
    const double back  = rclamp01(nol * thickness + (1.0 - thickness)) * 0.5;
    const double ss    = (back * (1.0 - fwd) + 1.0 * fwd) * (1.0 - thickness);
    return color[c] * (ss * (1.0 / kPi));
}
} // namespace

TEST_CASE("B8-b: OpenPBR lobes (aniso/sheen/clearcoat/subsurface) bit-exact vs Filament", "[kir][lighting][lobes]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int nx = g.input(sh, kir::DType::F64); const int ny = g.input(sh, kir::DType::F64); const int nz = g.input(sh, kir::DType::F64);
    const int vx = g.input(sh, kir::DType::F64); const int vy = g.input(sh, kir::DType::F64); const int vz = g.input(sh, kir::DType::F64);
    const int lx = g.input(sh, kir::DType::F64); const int ly = g.input(sh, kir::DType::F64); const int lz = g.input(sh, kir::DType::F64);
    const int al = g.input(sh, kir::DType::F64); const int an = g.input(sh, kir::DType::F64); // alpha, anisotropy

    double narr[3][kN]; double varr[3][kN]; double larr[3][kN]; double tarr[3][kN]; double barr[3][kN]; double alv[kN]; double anv[kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        narr[0][i] = 0.08 * i - 0.7; narr[1][i] = 0.3;            narr[2][i] = 1.0;
        varr[0][i] = 0.15;           varr[1][i] = 0.07 * i - 0.6; varr[2][i] = 1.1;
        larr[0][i] = 0.5 - 0.04 * i; larr[1][i] = 0.45;           larr[2][i] = 0.85 + 0.02 * i;
        unit(narr[0][i], narr[1][i], narr[2][i]); unit(varr[0][i], varr[1][i], varr[2][i]); unit(larr[0][i], larr[1][i], larr[2][i]);
        const double a0[3] = {1.0, 0.0, 0.0};
        const double dn = narr[0][i] * a0[0] + narr[1][i] * a0[1] + narr[2][i] * a0[2];
        tarr[0][i] = a0[0] - narr[0][i] * dn; tarr[1][i] = a0[1] - narr[1][i] * dn; tarr[2][i] = a0[2] - narr[2][i] * dn;
        unit(tarr[0][i], tarr[1][i], tarr[2][i]);
        barr[0][i] = narr[1][i] * tarr[2][i] - narr[2][i] * tarr[1][i];
        barr[1][i] = narr[2][i] * tarr[0][i] - narr[0][i] * tarr[2][i];
        barr[2][i] = narr[0][i] * tarr[1][i] - narr[1][i] * tarr[0][i];
        alv[i] = 0.02 + 0.03 * i;
        anv[i] = 0.05 * i - 0.4;
    }
    const double* inp[] = {narr[0], narr[1], narr[2], varr[0], varr[1], varr[2], larr[0], larr[1], larr[2], alv, anv};

    const int nn = g.vec3(nx, ny, nz); const int vv = g.vec3(vx, vy, vz); const int llv = g.vec3(lx, ly, lz);
    // rebuild the orthonormal frame IN-GRAPH the same way as the reference (so it need not be fed as inputs).
    const int arb = g.vec3(g.constant(1.0, sh, kir::DType::F64), g.constant(0.0, sh, kir::DType::F64), g.constant(0.0, sh, kir::DType::F64));
    const int tng = g.normalize(g.binary(kir::KOp::Sub, arb, g.binary(kir::KOp::Mul, nn, g.splat(g.dot(nn, arb), 3))));
    const int bit = g.cross(nn, tng);

    namespace lt2 = crd::kir::lighting;
    const int f0v = g.vec3(g.constant(0.6, sh, kir::DType::F64), g.constant(0.5, sh, kir::DType::F64), g.constant(0.9, sh, kir::DType::F64));
    const int f90 = g.constant(1.0, sh, kir::DType::F64);
    const int shc = g.vec3(g.constant(0.9, sh, kir::DType::F64), g.constant(0.7, sh, kir::DType::F64), g.constant(0.5, sh, kir::DType::F64));
    const int ssc = g.vec3(g.constant(0.8, sh, kir::DType::F64), g.constant(0.3, sh, kir::DType::F64), g.constant(0.2, sh, kir::DType::F64));
    const int hh  = g.normalize(g.binary(kir::KOp::Add, vv, llv));
    const int nov = g.binary(kir::KOp::Add, g.unary(kir::KOp::Abs, g.dot(nn, vv)), g.constant(1e-5, sh, kir::DType::F64));
    const int nol = crd::kir::nodes::clamp01(g, g.dot(nn, llv));
    const int noh = crd::kir::nodes::clamp01(g, g.dot(nn, hh));
    const int loh = crd::kir::nodes::clamp01(g, g.dot(llv, hh));

    const double f0r[3] = {0.6, 0.5, 0.9};
    const double shr[3] = {0.9, 0.7, 0.5};
    const double ssr[3] = {0.8, 0.3, 0.2};
    int bad = 0;
    const auto chkv = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };

    chkv(lt2::aniso_specular_lobe(g, f0v, f90, al, an, tng, bit, nn, vv, llv), [&](int i, int c) {
        const double t_i[3] = {tarr[0][i], tarr[1][i], tarr[2][i]}; const double b_i[3] = {barr[0][i], barr[1][i], barr[2][i]};
        const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]}; const double v_i[3] = {varr[0][i], varr[1][i], varr[2][i]}; const double l_i[3] = {larr[0][i], larr[1][i], larr[2][i]};
        return ref_aniso(f0r, 1.0, alv[i], anv[i], t_i, b_i, n_i, v_i, l_i, c); });
    chkv(lt2::sheen_lobe(g, shc, al, nov, nol, noh), [&](int i, int c) {
        const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]}; const double v_i[3] = {varr[0][i], varr[1][i], varr[2][i]}; const double l_i[3] = {larr[0][i], larr[1][i], larr[2][i]};
        double h[3] = {v_i[0] + l_i[0], v_i[1] + l_i[1], v_i[2] + l_i[2]}; const double hl = rlen3(h[0], h[1], h[2]); h[0] /= hl; h[1] /= hl; h[2] /= hl;
        const double dnv = rdot3(n_i, v_i); const double rov = (dnv < 0 ? -dnv : dnv) + 1e-5; const double rol = rclamp01(rdot3(n_i, l_i)); const double roh = rclamp01(rdot3(n_i, h));
        return ref_sheen(shr, alv[i], rov, rol, roh, c); });
    chk(lt2::clearcoat_lobe(g, an, al, noh, loh), [&](int i) {
        const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]}; const double v_i[3] = {varr[0][i], varr[1][i], varr[2][i]}; const double l_i[3] = {larr[0][i], larr[1][i], larr[2][i]};
        double h[3] = {v_i[0] + l_i[0], v_i[1] + l_i[1], v_i[2] + l_i[2]}; const double hl = rlen3(h[0], h[1], h[2]); h[0] /= hl; h[1] /= hl; h[2] /= hl;
        const double roh = rclamp01(rdot3(n_i, h)); const double rlh = rclamp01(rdot3(l_i, h));
        return ref_clearcoat(anv[i], alv[i], roh, rlh); });
    chkv(lt2::subsurface_term(g, ssc, al, an, nol, vv, llv), [&](int i, int c) {
        const double v_i[3] = {varr[0][i], varr[1][i], varr[2][i]}; const double l_i[3] = {larr[0][i], larr[1][i], larr[2][i]}; const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]};
        const double rol = rclamp01(rdot3(n_i, l_i));
        return ref_ss(ssr, alv[i], anv[i], rol, v_i, l_i, c); });

    CHECK(bad == 0);
}

namespace
{
constexpr double kPiG = crd::kir::lighting::kPiGltf; // glTF M_PI (distinct from Filament's kPi above)

double rsq(double x) { return x * x; }
double rclamp(double x, double lo, double hi) { const double m = x > lo ? x : lo; return m < hi ? m : hi; }
double rmix(double x, double y, double t) { return x * (1.0 - t) + y * t; }                                                       // oracle Mix
double rsmooth(double e0, double e1, double x) { const double u = (x - e0) / (e1 - e0); const double hi = u > 1.0 ? 1.0 : u; const double t = u < 0.0 ? 0.0 : hi; return t * t * (3.0 - 2.0 * t); } // oracle Smoothstep
void   rnorm3(const double a[3], double out[3]) { double s = 0.0; for (int k = 0; k < 3; ++k) { s += a[k] * a[k]; } const double len = crd::math::sqrt(s); for (int k = 0; k < 3; ++k) { out[k] = a[k] / len; } }

// evalSensitivity — line-for-line from glTF iridescence.glsl (op grouping matches the CKIR graph exactly).
void reval_sensitivity(double opd, const double sh[3], double out[3])
{
    const double phase = (2.0 * kPiG * opd) * 1.0e-9;
    const double val[3] = {5.4856e-13, 4.4201e-13, 5.2481e-13};
    const double pos[3] = {1.6810e+06, 1.7953e+06, 2.2084e+06};
    const double var[3] = {4.3278e+09, 9.3046e+09, 6.6121e+09};
    const double sqp = phase * phase;
    double       xyz[3];
    for (int k = 0; k < 3; ++k)
    {
        const double b = crd::math::sqrt((2.0 * kPiG) * var[k]);
        const double c = crd::math::cos(pos[k] * phase + sh[k]);
        const double d = crd::math::exp((-sqp) * var[k]);
        xyz[k]         = ((val[k] * b) * c) * d;
    }
    const double extra = ((9.7470e-14 * crd::math::sqrt((2.0 * kPiG) * 4.5282e+09)) * crd::math::cos(2.2399e+06 * phase + sh[0])) * crd::math::exp((-4.5282e+09) * sqp);
    xyz[0]             = xyz[0] + extra;
    for (int k = 0; k < 3; ++k) { xyz[k] = xyz[k] / 1.0685e-7; }
    const double cx = xyz[0];
    const double cy = xyz[1];
    const double cz = xyz[2];
    out[0]          = (3.2404542 * cx + (-1.5371385) * cy) + (-0.4985314) * cz;
    out[1]          = (-0.9692660 * cx + 1.8760108 * cy) + 0.0415560 * cz;
    out[2]          = (0.0556434 * cx + (-0.2040259) * cy) + 1.0572252 * cz;
}
// evalIridescence — the Belcour-Barla thin-film Fresnel (glTF).
void reval_iridescence(double outside_ior, double eta2, double cos_theta1, double thickness, const double base_f0[3], double out[3])
{
    const double irid_ior = rmix(outside_ior, eta2, rsmooth(0.0, 0.03, thickness));
    const double sin2      = rsq(outside_ior / irid_ior) * (1.0 - rsq(cos_theta1));
    const double cos2sq    = 1.0 - sin2;
    const double cos_t2    = crd::math::sqrt(cos2sq > 0.0 ? cos2sq : 0.0);
    const double r0        = rsq((irid_ior - outside_ior) / (irid_ior + outside_ior));
    const double r12       = r0 + (1.0 - r0) * rpow5(1.0 - cos_theta1);
    const double t121      = 1.0 - r12;
    const double phi12     = (irid_ior < outside_ior) ? kPiG : 0.0;
    const double phi21     = kPiG - phi12;
    double       base_ior[3];
    double       r1[3];
    double       r23[3];
    double       phi23[3];
    for (int k = 0; k < 3; ++k) { const double f = rclamp(base_f0[k], 0.0, 0.9999); const double s = crd::math::sqrt(f); base_ior[k] = (1.0 + s) / (1.0 - s); }
    for (int k = 0; k < 3; ++k) { r1[k]  = rsq((base_ior[k] - irid_ior) / (base_ior[k] + irid_ior)); }
    for (int k = 0; k < 3; ++k) { r23[k] = r1[k] + (1.0 - r1[k]) * rpow5(1.0 - cos_t2); }
    for (int k = 0; k < 3; ++k) { phi23[k] = (base_ior[k] < irid_ior) ? kPiG : 0.0; }
    const double opd = ((2.0 * irid_ior) * thickness) * cos_t2;
    double       phi[3];
    for (int k = 0; k < 3; ++k) { phi[k] = phi21 + phi23[k]; }
    double r123r[3];
    double out_i[3];
    double cm[3];
    for (int k = 0; k < 3; ++k)
    {
        const double r123 = rclamp(r12 * r23[k], 1e-5, 0.9999);
        r123r[k]          = crd::math::sqrt(r123);
        const double rs   = (rsq(t121) * r23[k]) / (1.0 - r123);
        out_i[k]          = r12 + rs;
        cm[k]             = rs - t121;
    }
    for (int m = 1; m <= 2; ++m)
    {
        const double sh[3] = {static_cast<double>(m) * phi[0], static_cast<double>(m) * phi[1], static_cast<double>(m) * phi[2]};
        double       sens[3];
        reval_sensitivity(static_cast<double>(m) * opd, sh, sens);
        for (int k = 0; k < 3; ++k) { cm[k] = cm[k] * r123r[k]; const double sm = 2.0 * sens[k]; out_i[k] = out_i[k] + cm[k] * sm; }
    }
    for (int k = 0; k < 3; ++k) { out[k] = out_i[k] > 0.0 ? out_i[k] : 0.0; }
}

// transmission BTDF helpers (glTF KHR_materials_transmission; D/V are Filament's stable forms, so π = kPi here).
double rdggx(double noh, double alpha) { const double a = noh * alpha; const double denom = (1.0 - noh * noh) + a * a; double kk = alpha / denom; kk = kk < 453.5 ? kk : 453.5; return kk * (kk * (1.0 / kPi)); }
double rvsmith(double nov, double nol, double alpha) { const double a2 = alpha * alpha; const double lv = nol * crd::math::sqrt((nov - a2 * nov) * nov + a2); const double ll = nov * crd::math::sqrt((nol - a2 * nol) * nol + a2); const double s = lv + ll; return 0.5 / (s > 0.0000077 ? s : 0.0000077); }
double ref_transmission(const double base[3], const double f0[3], double f90, double alpha, double ior, const double n[3], const double view[3], const double l[3], int c)
{
    const double tr = alpha * rclamp(ior * 2.0 - 2.0, 0.0, 1.0);
    double       nn[3];
    double       vv[3];
    double       ll[3];
    rnorm3(n, nn);
    rnorm3(view, vv);
    rnorm3(l, ll);
    double dp = 0.0;
    for (int k = 0; k < 3; ++k) { dp += (-ll[k]) * nn[k]; }             // dot(-l, n), oracle order
    double lm_pre[3];
    for (int k = 0; k < 3; ++k) { lm_pre[k] = ll[k] + (2.0 * nn[k]) * dp; }
    double lmir[3];
    rnorm3(lm_pre, lmir);
    double h_pre[3];
    for (int k = 0; k < 3; ++k) { h_pre[k] = lmir[k] + vv[k]; }
    double h[3];
    rnorm3(h_pre, h);
    const double noh  = rclamp01(rdot3(nn, h));
    const double voh  = rclamp01(rdot3(vv, h));
    const double dnv  = rdot3(nn, vv);
    const double nov  = (dnv < 0.0 ? -dnv : dnv) + 1e-5;
    const double nolm = rclamp01(rdot3(nn, lmir));
    const double d    = rdggx(noh, tr);
    const double vis  = rvsmith(nov, nolm, tr);
    const double f    = f0[c] + (f90 - f0[c]) * rpow5(1.0 - voh);
    return ((1.0 - f) * base[c]) * d * vis;
}
double ref_volatten(const double rad[3], double dist, const double col[3], double adist, int c) { const double coeff = (-crd::math::log(col[c])) / adist; const double trans = crd::math::exp((-coeff) * dist); return trans * rad[c]; }
void   ref_refract_ray(const double n[3], const double v[3], double ior, double thickness, double out[3])
{
    double nn[3];
    rnorm3(n, nn);
    const double iv[3] = {-v[0], -v[1], -v[2]};
    const double eta   = 1.0 / ior;
    double       dp    = 0.0;
    for (int k = 0; k < 3; ++k) { dp += nn[k] * iv[k]; }               // oracle Refract dot order (n·i)
    const double kk = 1.0 - eta * eta * (1.0 - dp * dp);
    double       rv[3];
    if (kk < 0.0) { for (int k = 0; k < 3; ++k) { rv[k] = 0.0; } }
    else { const double coef = eta * dp + crd::math::sqrt(kk); for (int k = 0; k < 3; ++k) { rv[k] = eta * iv[k] - coef * nn[k]; } }
    double rn[3];
    rnorm3(rv, rn);
    for (int k = 0; k < 3; ++k) { out[k] = rn[k] * thickness; }
}
} // namespace

TEST_CASE("B8-b: thin-film iridescence (Belcour-Barla) bit-exact vs glTF", "[kir][lighting][thinfilm]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int e2 = g.input(sh, kir::DType::F64); // thin-film IOR (eta2)
    const int c1 = g.input(sh, kir::DType::F64); // cosTheta1
    const int th = g.input(sh, kir::DType::F64); // thickness (nm)
    const int fr = g.input(sh, kir::DType::F64);
    const int fg = g.input(sh, kir::DType::F64);
    const int fb = g.input(sh, kir::DType::F64);

    double e2v[kN];
    double c1v[kN];
    double thv[kN];
    double f0[3][kN];
    for (int i = 0; i < kN; ++i)
    {
        e2v[i]    = 1.2 + 0.03 * i;   // 1.20 .. 1.77
        c1v[i]    = 0.3 + 0.03 * i;   // 0.30 .. 0.87  (all in (0,1), no TIR over this range)
        thv[i]    = 100.0 + 40.0 * i; // 100 .. 860 nm
        f0[0][i]  = 0.04 + 0.004 * i; // base reflectance rgb, all < 1
        f0[1][i]  = 0.08 + 0.003 * i;
        f0[2][i]  = 0.12 + 0.002 * i;
    }
    const double* inp[] = {e2v, c1v, thv, f0[0], f0[1], f0[2]};

    const int outside = g.constant(1.0, sh, kir::DType::F64); // air
    const int basef0  = g.vec3(fr, fg, fb);
    const int node    = lt::eval_iridescence(g, outside, e2, c1, th, basef0);

    double o[kN * 3];
    kir::eval_cpu(g, inp, &alloc, node, o);
    int bad = 0;
    for (int i = 0; i < kN; ++i)
    {
        const double bf[3] = {f0[0][i], f0[1][i], f0[2][i]};
        double       ref[3];
        reval_iridescence(1.0, e2v[i], c1v[i], thv[i], bf, ref);
        for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref[c]) { ++bad; } }
    }
    CHECK(bad == 0);
}

TEST_CASE("B8-b: transmission BTDF + Beer's-law absorption + refraction ray bit-exact vs glTF", "[kir][lighting][transmission]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int nx = g.input(sh, kir::DType::F64); const int ny = g.input(sh, kir::DType::F64); const int nz = g.input(sh, kir::DType::F64);
    const int vx = g.input(sh, kir::DType::F64); const int vy = g.input(sh, kir::DType::F64); const int vz = g.input(sh, kir::DType::F64);
    const int lx = g.input(sh, kir::DType::F64); const int ly = g.input(sh, kir::DType::F64); const int lz = g.input(sh, kir::DType::F64);
    const int al = g.input(sh, kir::DType::F64); const int io = g.input(sh, kir::DType::F64); // alpha, ior
    const int di = g.input(sh, kir::DType::F64);                                              // transmission distance

    double narr[3][kN]; double varr[3][kN]; double larr[3][kN]; double alv[kN]; double iov[kN]; double dsv[kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        narr[0][i] = 0.08 * i - 0.7; narr[1][i] = 0.35;           narr[2][i] = 1.0;
        varr[0][i] = 0.2;            varr[1][i] = 0.06 * i - 0.5; varr[2][i] = 1.15;
        larr[0][i] = 0.55 - 0.05 * i; larr[1][i] = 0.4;            larr[2][i] = 0.9 + 0.02 * i;
        unit(narr[0][i], narr[1][i], narr[2][i]); unit(varr[0][i], varr[1][i], varr[2][i]); unit(larr[0][i], larr[1][i], larr[2][i]);
        alv[i] = 0.05 + 0.03 * i;    // linear roughness alpha
        iov[i] = 1.2 + 0.03 * i;     // ior > 1  → eta = 1/ior < 1, no TIR
        dsv[i] = 0.1 + 0.05 * i;     // transmission distance
    }
    const double* inp[] = {narr[0], narr[1], narr[2], varr[0], varr[1], varr[2], larr[0], larr[1], larr[2], alv, iov, dsv};

    const int nn = g.vec3(nx, ny, nz); const int vv = g.vec3(vx, vy, vz); const int llv = g.vec3(lx, ly, lz);
    const int base = g.vec3(g.constant(0.85, sh, kir::DType::F64), g.constant(0.6, sh, kir::DType::F64), g.constant(0.4, sh, kir::DType::F64));
    const int f0v  = g.vec3(g.constant(0.04, sh, kir::DType::F64), g.constant(0.04, sh, kir::DType::F64), g.constant(0.04, sh, kir::DType::F64));
    const int f90  = g.constant(1.0, sh, kir::DType::F64);
    const int attc = g.vec3(g.constant(0.7, sh, kir::DType::F64), g.constant(0.5, sh, kir::DType::F64), g.constant(0.3, sh, kir::DType::F64)); // attenuation colour
    const int adst = g.constant(0.5, sh, kir::DType::F64);                                                                                     // attenuation distance
    const int rad  = g.vec3(g.constant(1.5, sh, kir::DType::F64), g.constant(1.2, sh, kir::DType::F64), g.constant(0.9, sh, kir::DType::F64)); // incident radiance
    const int thk  = g.constant(0.25, sh, kir::DType::F64);                                                                                    // thickness

    const double basr[3] = {0.85, 0.6, 0.4};
    const double f0r[3]  = {0.04, 0.04, 0.04};
    const double attr[3] = {0.7, 0.5, 0.3};
    const double radr[3] = {1.5, 1.2, 0.9};
    int          bad     = 0;
    const auto   chkv    = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };

    chkv(lt::transmission_btdf(g, base, f0v, f90, al, io, nn, vv, llv), [&](int i, int c) {
        const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]}; const double v_i[3] = {varr[0][i], varr[1][i], varr[2][i]}; const double l_i[3] = {larr[0][i], larr[1][i], larr[2][i]};
        return ref_transmission(basr, f0r, 1.0, alv[i], iov[i], n_i, v_i, l_i, c); });
    chkv(lt::volume_attenuation(g, rad, di, attc, adst), [&](int i, int c) { return ref_volatten(radr, dsv[i], attr, 0.5, c); });
    chkv(lt::refraction_ray(g, nn, vv, io, thk), [&](int i, int c) {
        const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]}; const double v_i[3] = {varr[0][i], varr[1][i], varr[2][i]}; double r[3]; ref_refract_ray(n_i, v_i, iov[i], 0.25, r); return r[c]; });

    CHECK(bad == 0);
}

namespace
{
// Filament surface_light_punctual.fs attenuations (saturate = clamp01; falloff = 1/radius²).
double rsquare_falloff(double d2, double falloff) { const double f = d2 * falloff; const double sf = rclamp01(1.0 - f * f); return sf * sf; }
double rdist_atten(const double p[3], double falloff) { const double d2 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2]; return rsquare_falloff(d2, falloff) / rmax2(d2, 1e-4); }
double rangle_atten(const double ld[3], const double l[3], double scale, double offset) { const double cd = rdot3(ld, l); const double a = rclamp01(cd * scale + offset); return a * a; }
} // namespace

namespace
{
void rcross(const double a[3], const double b[3], double out[3]) { out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0]; } // oracle Cross order
// IntegrateEdgeVec (Heitz ltc_code): the acos rational approximation.
void rintegrate_edge_vec(const double v1[3], const double v2[3], double out[3])
{
    const double x   = rdot3(v1, v2);
    const double y   = x < 0.0 ? -x : x;
    const double a   = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    const double b   = 3.4175940 + (4.1616724 + y) * y;
    const double vv  = a / b;
    const double tst = (x > 0.0) ? vv : 0.5 * crd::math::rsqrt(rmax2(1.0 - x * x, 1e-7)) - vv;
    double       cr[3];
    rcross(v1, v2, cr);
    for (int k = 0; k < 3; ++k) { out[k] = cr[k] * tst; }
}
// column-major 3×3 helpers matching the oracle (MatFromCols / MatTranspose / MatMatMul / MatVecMul).
void rmat3_cols(const double c0[3], const double c1[3], const double c2[3], double m[9]) { for (int r = 0; r < 3; ++r) { m[0 * 3 + r] = c0[r]; m[1 * 3 + r] = c1[r]; m[2 * 3 + r] = c2[r]; } }
void rtranspose3(const double m[9], double out[9]) { for (int col = 0; col < 3; ++col) { for (int r = 0; r < 3; ++r) { out[col * 3 + r] = m[r * 3 + col]; } } }
void rmatmul3(const double a[9], const double b[9], double out[9]) { for (int col = 0; col < 3; ++col) { for (int r = 0; r < 3; ++r) { double s = 0.0; for (int k = 0; k < 3; ++k) { s = s + a[k * 3 + r] * b[col * 3 + k]; } out[col * 3 + r] = s; } } }
void rmatvec3(const double m[9], const double v[3], double out[3]) { for (int r = 0; r < 3; ++r) { double s = 0.0; for (int col = 0; col < 3; ++col) { s = s + m[col * 3 + r] * v[col]; } out[r] = s; } }
// LTC_Evaluate rect (clipless).
double rltc_rect(const double n[3], const double v[3], const double p[3], const double minv[9], const double pts[4][3], double scale, bool two_sided)
{
    const double nov = rdot3(v, n);
    double       t1pre[3];
    for (int k = 0; k < 3; ++k) { t1pre[k] = v[k] - n[k] * nov; }
    double t1[3];
    rnorm3(t1pre, t1);
    double t2[3];
    rcross(n, t1, t2);
    double basis[9];
    rmat3_cols(t1, t2, n, basis);
    double basis_t[9];
    rtranspose3(basis, basis_t);
    double m[9];
    rmatmul3(minv, basis_t, m);
    double lproj[4][3];
    for (int i = 0; i < 4; ++i) { double d[3]; for (int k = 0; k < 3; ++k) { d[k] = pts[i][k] - p[k]; } double md[3]; rmatvec3(m, d, md); rnorm3(md, lproj[i]); }
    double e1[3];
    double e3[3];
    double dir[3];
    for (int k = 0; k < 3; ++k) { e1[k] = pts[1][k] - pts[0][k]; e3[k] = pts[3][k] - pts[0][k]; dir[k] = pts[0][k] - p[k]; }
    double lnrm[3];
    rcross(e1, e3, lnrm);
    const bool behind = rdot3(dir, lnrm) < 0.0;
    double     e01[3];
    double     e12[3];
    double     e23[3];
    double     e30[3];
    rintegrate_edge_vec(lproj[0], lproj[1], e01);
    rintegrate_edge_vec(lproj[1], lproj[2], e12);
    rintegrate_edge_vec(lproj[2], lproj[3], e23);
    rintegrate_edge_vec(lproj[3], lproj[0], e30);
    double s = 0.0;
    for (int k = 0; k < 3; ++k) { const double vs = ((e01[k] + e12[k]) + e23[k]) + e30[k]; s = s + vs * vs; }
    const double len = crd::math::sqrt(s);
    double       sum = len * scale;
    if (!two_sided && behind) { sum = 0.0; }
    return sum;
}
} // namespace

TEST_CASE("B8-d: LTC integrate_edge_vec + rect area light bit-exact vs Heitz ltc_code", "[kir][lighting][area]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int ax = g.input(sh, kir::DType::F64); const int ay = g.input(sh, kir::DType::F64); const int az = g.input(sh, kir::DType::F64);
    const int bx = g.input(sh, kir::DType::F64); const int by = g.input(sh, kir::DType::F64); const int bz = g.input(sh, kir::DType::F64);
    const int px = g.input(sh, kir::DType::F64); const int py = g.input(sh, kir::DType::F64); const int pz = g.input(sh, kir::DType::F64);

    double aarr[3][kN]; double barr[3][kN]; double parr[3][kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        aarr[0][i] = 0.1 * i - 0.9; aarr[1][i] = 0.4;             aarr[2][i] = 0.9; // two unit vectors for integrate_edge_vec
        barr[0][i] = 0.3;           barr[1][i] = 0.1 * i - 0.85;  barr[2][i] = 1.0;
        unit(aarr[0][i], aarr[1][i], aarr[2][i]); unit(barr[0][i], barr[1][i], barr[2][i]);
        parr[0][i] = 0.25 * i - 2.5; parr[1][i] = 0.15 * i - 1.5; parr[2][i] = 0.0; // shading point below the rect (z=2)
    }
    const double* inp[] = {aarr[0], aarr[1], aarr[2], barr[0], barr[1], barr[2], parr[0], parr[1], parr[2]};

    const int va = g.vec3(ax, ay, az);
    const int vb = g.vec3(bx, by, bz);
    int       bad = 0;

    // 1) integrate_edge_vec bit-exact (the acos edge form factor)
    {
        double o[kN * 3];
        kir::eval_cpu(g, inp, &alloc, lt::integrate_edge_vec(g, va, vb), o);
        for (int i = 0; i < kN; ++i)
        {
            const double a_i[3] = {aarr[0][i], aarr[1][i], aarr[2][i]};
            const double b_i[3] = {barr[0][i], barr[1][i], barr[2][i]};
            double       ref[3];
            rintegrate_edge_vec(a_i, b_i, ref);
            for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref[c]) { ++bad; } }
        }
    }
    // 2) ltc_evaluate_rect bit-exact — a fixed rect light + a fitted-like Minv, varying the shading point P.
    const auto kc = [&](double x, double y, double z) { return g.vec3(g.constant(x, sh, kir::DType::F64), g.constant(y, sh, kir::DType::F64), g.constant(z, sh, kir::DType::F64)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = g.normalize(kc(0.3, 0.2, 1.0)); // V not parallel to N (else T1 = normalize(0) = NaN)
    const int  pp   = g.vec3(px, py, pz);
    const int  q0 = kc(-1.0, -1.0, 2.0); const int q1 = kc(1.0, -1.0, 2.0); const int q2 = kc(1.0, 1.0, 2.0); const int q3 = kc(-1.0, 1.0, 2.0);
    // fitted-like Minv (columns): a mild off-diagonal LTC matrix, plus the identity (diffuse) — both exercised.
    const int  minv = g.mat3(kc(0.9, 0.0, 0.05), kc(0.0, 0.8, 0.0), kc(0.1, 0.0, 1.0));
    const int  ident = g.mat3(kc(1.0, 0.0, 0.0), kc(0.0, 1.0, 0.0), kc(0.0, 0.0, 1.0));
    const double pts[4][3] = {{-1.0, -1.0, 2.0}, {1.0, -1.0, 2.0}, {1.0, 1.0, 2.0}, {-1.0, 1.0, 2.0}};
    const double minv_r[9] = {0.9, 0.0, 0.05, 0.0, 0.8, 0.0, 0.1, 0.0, 1.0}; // column-major
    const double ident_r[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    const double nr[3] = {0.0, 0.0, 1.0};
    double       vr[3] = {0.3, 0.2, 1.0};
    { double vpre[3] = {0.3, 0.2, 1.0}; rnorm3(vpre, vr); } // match the in-graph normalize
    const double inv2pi = 1.0 / (2.0 * kPi);
    const auto   chk = [&](int node, const double* mr, double scale, bool ts) {
        double o[kN];
        kir::eval_cpu(g, inp, &alloc, node, o);
        for (int i = 0; i < kN; ++i)
        {
            const double p_i[3] = {parr[0][i], parr[1][i], parr[2][i]};
            if (o[i] != rltc_rect(nr, vr, p_i, mr, pts, scale, ts)) { ++bad; }
        }
    };
    chk(lt::ltc_evaluate_rect(g, n, view, pp, ident, q0, q1, q2, q3, g.constant(inv2pi, sh, kir::DType::F64), true), ident_r, inv2pi, true);
    chk(lt::ltc_evaluate_rect(g, n, view, pp, minv, q0, q1, q2, q3, g.constant(1.0, sh, kir::DType::F64), true), minv_r, 1.0, true);

    CHECK(bad == 0);
}

namespace
{
// Filament SH L2 irradiance reference (band grouping + (3·nz)·nz order matter for bit-exactness).
void rsh_irradiance(const double n[3], const double sh[9][3], double out[3])
{
    const double nx = n[0];
    const double ny = n[1];
    const double nz = n[2];
    const double t4 = ny * nx;
    const double t5 = ny * nz;
    const double t6 = (3.0 * nz) * nz - 1.0;
    const double t7 = nz * nx;
    const double t8 = nx * nx - ny * ny;
    for (int c = 0; c < 3; ++c)
    {
        const double band1 = (sh[1][c] * ny + sh[2][c] * nz) + sh[3][c] * nx;
        double       band2 = sh[4][c] * t4 + sh[5][c] * t5;
        band2              = band2 + sh[6][c] * t6;
        band2              = band2 + sh[7][c] * t7;
        band2              = band2 + sh[8][c] * t8;
        const double r     = (sh[0][c] + band1) + band2;
        out[c]             = r > 0.0 ? r : 0.0;
    }
}
// Karis analytic env-BRDF (scale, bias) — matches env_brdf_approx.
void ref_env_brdf(double perceptual, double nov, double& scale, double& bias)
{
    const double rx   = perceptual * -1.0 + 1.0;
    const double ry   = perceptual * -0.0275 + 0.0425;
    const double rz   = perceptual * -0.572 + 1.04;
    const double rw   = perceptual * 0.022 + (-0.04);
    const double rx2  = rx * rx;
    const double e2   = crd::math::exp2(-9.28 * nov);
    const double a004 = (rx2 < e2 ? rx2 : e2) * rx + ry;
    scale             = -1.04 * a004 + rz;
    bias              = 1.04 * a004 + rw;
}
double ref_ibl_specular(const double prefiltered[3], const double f0[3], double perceptual, double nov, int c)
{
    double scale = 0.0;
    double bias  = 0.0;
    ref_env_brdf(perceptual, nov, scale, bias);
    const double term   = f0[c] * scale + bias;
    const double base    = prefiltered[c] * term;
    const double bias_sf = bias > 1.0e-3 ? bias : 1.0e-3; // energy_compensation floors the DFG bias (see ckir_lighting.hpp)
    const double energy  = 1.0 + f0[c] * (1.0 / bias_sf - 1.0);
    return base * energy;
}
} // namespace

TEST_CASE("B8-e: IBL SH L2 irradiance + Karis split-sum specular bit-exact vs Filament", "[kir][lighting][ibl]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int nx = g.input(sh, kir::DType::F64); const int ny = g.input(sh, kir::DType::F64); const int nz = g.input(sh, kir::DType::F64);
    const int pr = g.input(sh, kir::DType::F64); const int nv = g.input(sh, kir::DType::F64); // perceptual roughness, NoV
    double    narr[3][kN]; double prv[kN]; double nvv[kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        narr[0][i] = 0.1 * i - 0.9; narr[1][i] = 0.3 + 0.02 * i; narr[2][i] = 0.8;
        unit(narr[0][i], narr[1][i], narr[2][i]);
        prv[i] = 0.06 + 0.045 * i; // perceptual roughness
        nvv[i] = 0.2 + 0.035 * i;  // NoV
    }
    const double* inp[] = {narr[0], narr[1], narr[2], prv, nvv};

    const int nn = g.vec3(nx, ny, nz);
    const auto kc = [&](double x, double y, double z) { return g.vec3(g.constant(x, sh, kir::DType::F64), g.constant(y, sh, kir::DType::F64), g.constant(z, sh, kir::DType::F64)); };
    // a representative sky SH set (9 RGB coefficients).
    const double shr[9][3] = {{0.7, 0.75, 0.9}, {0.15, 0.16, 0.2}, {0.28, 0.3, 0.38}, {-0.08, -0.07, -0.05}, {0.02, 0.02, 0.03}, {-0.03, -0.03, -0.02}, {0.1, 0.11, 0.14}, {0.04, 0.04, 0.03}, {-0.05, -0.05, -0.06}};
    int        shn[9];
    for (int i = 0; i < 9; ++i) { shn[i] = kc(shr[i][0], shr[i][1], shr[i][2]); }
    const int  prefiltered = kc(0.6, 0.7, 0.95);
    const int  f0          = kc(0.04, 0.05, 0.08);
    const double pref_r[3] = {0.6, 0.7, 0.95};
    const double f0r[3]    = {0.04, 0.05, 0.08};

    int        bad = 0;
    const auto chkv = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };

    chkv(lt::ibl_diffuse(g, kc(0.8, 0.5, 0.3), lt::sh_irradiance(g, nn, shn)), [&](int i, int c) {
        const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]}; double irr[3]; rsh_irradiance(n_i, shr, irr);
        const double diff[3] = {0.8, 0.5, 0.3};
        return diff[c] * irr[c]; });
    chkv(lt::ibl_specular(g, prefiltered, f0, pr, nv), [&](int i, int c) { return ref_ibl_specular(pref_r, f0r, prv[i], nvv[i], c); });

    CHECK(bad == 0);
}

namespace
{
void rimportance_ggx(const double u[2], double roughness, double out[3])
{
    const double a2   = roughness * roughness;
    const double phi  = (2.0 * kPi) * u[0];
    const double cos2 = (1.0 - u[1]) / (1.0 + (a2 - 1.0) * u[1]);
    const double sint = crd::math::sqrt(1.0 - cos2);
    out[0] = crd::math::cos(phi) * sint;
    out[1] = crd::math::sin(phi) * sint;
    out[2] = crd::math::sqrt(cos2);
}
void rdfg_integrand(const double u[2], double nov, double roughness, double out[2])
{
    double h[3];
    rimportance_ggx(u, roughness, h);
    const double vx   = crd::math::sqrt(1.0 - nov * nov);
    const double voh  = (vx * h[0] + 0.0 * h[1]) + nov * h[2];
    const double noh  = h[2];
    const double nol  = 2.0 * voh * noh - nov;
    const double k    = (roughness * roughness) * 0.5;
    const auto   g1   = [&](double x) { return x / (x * (1.0 - k) + k); };
    const double gvis = (g1(nov) * g1(nol) * voh) / (noh * nov);
    const double fc   = rpow5(1.0 - voh);
    if (nol > 0.0) { out[0] = (1.0 - fc) * gvis; out[1] = fc * gvis; }
    else { out[0] = 0.0; out[1] = 0.0; }
}
} // namespace

TEST_CASE("B8-e: IBL generation (GGX importance sample + split-sum DFG integrand) bit-exact vs Karis", "[kir][lighting][ibl]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int ux = g.input(sh, kir::DType::F64); const int uy = g.input(sh, kir::DType::F64);
    const int nv = g.input(sh, kir::DType::F64); const int rg = g.input(sh, kir::DType::F64);
    double    uxv[kN]; double uyv[kN]; double nvv[kN]; double rgv[kN];
    for (int i = 0; i < kN; ++i)
    {
        uxv[i] = 0.03 + 0.045 * i;  // sample.x ∈ (0,1)
        uyv[i] = 0.05 + 0.04 * i;   // sample.y ∈ (0,1)
        nvv[i] = 0.15 + 0.04 * i;   // NoV
        rgv[i] = 0.1 + 0.04 * i;    // linear roughness (alpha)
    }
    const double* inp[] = {uxv, uyv, nvv, rgv};
    const int     u2 = g.vec2(ux, uy);

    int        bad = 0;
    const auto chkv = [&](int node, int comps, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < comps; ++c) { if (o[i * comps + c] != ref(i, c)) { ++bad; } } } };

    chkv(lt::importance_sample_ggx(g, u2, rg), 3, [&](int i, int c) {
        const double u_i[2] = {uxv[i], uyv[i]}; double h[3]; rimportance_ggx(u_i, rgv[i], h); return h[c]; });
    chkv(lt::dfg_integrand(g, u2, nv, rg), 2, [&](int i, int c) {
        const double u_i[2] = {uxv[i], uyv[i]}; double ab[2]; rdfg_integrand(u_i, nvv[i], rgv[i], ab); return ab[c]; });

    CHECK(bad == 0);
}

namespace
{
constexpr double kLtcPi = 3.14159265; // ltc_code PI
double rlen(const double v[3]) { return crd::math::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
double rdet2(const double m[4]) { return m[0] * m[3] - m[2] * m[1]; } // column-major 2×2
double rdet3(const double m[9])                                       // cofactor expansion along row 0 (oracle order)
{
    double det = 0.0;
    for (int col0 = 0; col0 < 3; ++col0)
    {
        double minor[4];
        int    mi = 0;
        for (int col = 0; col < 3; ++col) { if (col == col0) { continue; } for (int row = 1; row < 3; ++row) { minor[mi++] = m[col * 3 + row]; } }
        const double cof = m[col0 * 3 + 0] * rdet2(minor);
        det              = det + ((col0 % 2 == 0) ? cof : -cof);
    }
    return det;
}
double rminor_det3(const double m[9], int sr, int sc)
{
    double minor[4];
    int    mi = 0;
    for (int col = 0; col < 3; ++col) { if (col == sc) { continue; } for (int row = 0; row < 3; ++row) { if (row == sr) { continue; } minor[mi++] = m[col * 3 + row]; } }
    return rdet2(minor);
}
void rmat_inverse3(const double m[9], double out[9])
{
    const double det = rdet3(m);
    for (int ri = 0; ri < 3; ++ri) { for (int cj = 0; cj < 3; ++cj) { const double sign = ((ri + cj) % 2 == 0) ? 1.0 : -1.0; out[cj * 3 + ri] = sign * rminor_det3(m, cj, ri) / det; } }
}
double rfpo(double d, double l) { return l / (d * (d * d + l * l)) + crd::math::atan(l / d) / (d * d); }
double rfwt(double d, double l) { return l * l / (d * (d * d + l * l)); }
double ri_diffuse_line(const double p1[3], const double p2[3])
{
    double wtpre[3];
    for (int k = 0; k < 3; ++k) { wtpre[k] = p2[k] - p1[k]; }
    double wt[3];
    rnorm3(wtpre, wt);
    const double p1z = p1[2];
    const double p2z = p2[2];
    double       p1c[3];
    double       p2c[3];
    for (int k = 0; k < 3; ++k) { p1c[k] = (p1z < 0.0) ? (p1[k] * p2z - p2[k] * p1z) / (p2z - p1z) : p1[k]; }
    for (int k = 0; k < 3; ++k) { p2c[k] = (p2z < 0.0) ? (-p1[k] * p2z + p2[k] * p1z) / (-p2z + p1z) : p2[k]; }
    const double l1 = rdot3(p1c, wt);
    const double l2 = rdot3(p2c, wt);
    double       po[3];
    for (int k = 0; k < 3; ++k) { po[k] = p1c[k] - l1 * wt[k]; }
    const double d   = rlen(po);
    const double i   = (rfpo(d, l2) - rfpo(d, l1)) * po[2] + (rfwt(d, l2) - rfwt(d, l1)) * wt[2];
    const double ipi = i / kLtcPi;
    return (p1z <= 0.0 && p2z <= 0.0) ? 0.0 : ipi;
}
double ri_ltc_line(const double minv[9], const double p1[3], const double p2[3])
{
    double p1o[3];
    double p2o[3];
    rmatvec3(minv, p1, p1o);
    rmatvec3(minv, p2, p2o);
    const double i_diffuse = ri_diffuse_line(p1o, p2o);
    double       cr[3];
    rcross(p1, p2, cr);
    double ortho[3];
    rnorm3(cr, ortho);
    double mt[9];
    rtranspose3(minv, mt);
    double mti[9];
    rmat_inverse3(mt, mti);
    double mo[3];
    rmatvec3(mti, ortho, mo);
    return (1.0 / rlen(mo)) * i_diffuse;
}
double rltc_line(const double n[3], const double v[3], const double p[3], const double minv[9], const double pa[3], const double pb[3], double radius)
{
    const double nov = rdot3(v, n);
    double       t1pre[3];
    for (int k = 0; k < 3; ++k) { t1pre[k] = v[k] - n[k] * nov; }
    double t1[3];
    rnorm3(t1pre, t1);
    double t2[3];
    rcross(n, t1, t2);
    double basis[9];
    rmat3_cols(t1, t2, n, basis);
    double bm[9];
    rtranspose3(basis, bm);
    double da[3];
    double db[3];
    for (int k = 0; k < 3; ++k) { da[k] = pa[k] - p[k]; db[k] = pb[k] - p[k]; }
    double p1[3];
    double p2[3];
    rmatvec3(bm, da, p1);
    rmatvec3(bm, db, p2);
    return radius * ri_ltc_line(minv, p1, p2);
}
} // namespace

TEST_CASE("B8-d: LTC line/tube area light bit-exact vs Heitz ltc_code", "[kir][lighting][area]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int px = g.input(sh, kir::DType::F64); const int py = g.input(sh, kir::DType::F64); const int pz = g.input(sh, kir::DType::F64);
    double    parr[3][kN];
    for (int i = 0; i < kN; ++i) { parr[0][i] = 0.2 * i - 2.0; parr[1][i] = 0.12 * i - 1.2; parr[2][i] = 0.0; }
    const double* inp[] = {parr[0], parr[1], parr[2]};

    const auto kc = [&](double x, double y, double z) { return g.vec3(g.constant(x, sh, kir::DType::F64), g.constant(y, sh, kir::DType::F64), g.constant(z, sh, kir::DType::F64)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = g.normalize(kc(0.2, 0.15, 1.0));
    const int  pp   = g.vec3(px, py, pz);
    const int  pa   = kc(-1.2, 0.3, 2.0); const int pb = kc(1.1, -0.2, 2.3);
    const int  ident = g.mat3(kc(1.0, 0.0, 0.0), kc(0.0, 1.0, 0.0), kc(0.0, 0.0, 1.0));
    const int  minv  = g.mat3(kc(0.85, 0.0, 0.06), kc(0.0, 0.75, 0.0), kc(0.12, 0.0, 1.0));
    const int  rad   = g.constant(0.5, sh, kir::DType::F64);

    const double nr[3] = {0.0, 0.0, 1.0};
    double       vr[3] = {0.2, 0.15, 1.0};
    { double vp[3] = {0.2, 0.15, 1.0}; rnorm3(vp, vr); }
    const double par[3] = {-1.2, 0.3, 2.0};
    const double pbr[3] = {1.1, -0.2, 2.3};
    const double ident_r[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    const double minv_r[9]  = {0.85, 0.0, 0.06, 0.0, 0.75, 0.0, 0.12, 0.0, 1.0};
    int          bad        = 0;
    const auto   chk = [&](int node, const double* mr) {
        double o[kN];
        kir::eval_cpu(g, inp, &alloc, node, o);
        for (int i = 0; i < kN; ++i)
        {
            const double p_i[3] = {parr[0][i], parr[1][i], parr[2][i]};
            if (o[i] != rltc_line(nr, vr, p_i, mr, par, pbr, 0.5)) { ++bad; }
        }
    };
    chk(lt::ltc_evaluate_line(g, n, view, pp, ident, pa, pb, rad), ident_r); // diffuse tube (Minv = I)
    chk(lt::ltc_evaluate_line(g, n, view, pp, minv, pa, pb, rad), minv_r);    // specular tube (exercises mat_inverse)
    CHECK(bad == 0);
}

namespace
{
// SolveCubic (Blinn) reference — matches the graph op-for-op.
void rsolve_cubic(const double coeff[4], double out[3])
{
    const double cw  = coeff[3];
    const double aa  = cw;
    const double cx  = coeff[0] / cw;
    const double cy  = (coeff[1] / cw) / 3.0;
    const double cz  = (coeff[2] / cw) / 3.0;
    const double bb  = cz;
    const double cc  = cy;
    const double dd  = cx;
    const double dx  = -cz * cz + cy;
    const double dy  = -cy * cz + cx;
    const double dz  = cz * cx - cy * cy;
    const double disc = 4.0 * dx * dz - dy * dy;
    const double sd  = crd::math::sqrt(disc);
    const double t23 = (2.0 / 3.0) * kLtcPi;
    const double da_a = -2.0 * bb * dx + dy;
    const double th_a = crd::math::atan2(sd, -da_a) / 3.0;
    const double sc_a = 2.0 * crd::math::sqrt(-dx);
    const double x1a = sc_a * crd::math::cos(th_a);
    const double x3a = sc_a * crd::math::cos(th_a + t23);
    const double xl  = (x1a + x3a > 2.0 * bb) ? x1a : x3a;
    const double xlx = xl - bb;
    const double xly = aa;
    const double dd_d = -dd * dy + 2.0 * cc * dz;
    const double th_d = crd::math::atan2(dd * sd, -dd_d) / 3.0;
    const double sc_d = 2.0 * crd::math::sqrt(-dz);
    const double x1d = sc_d * crd::math::cos(th_d);
    const double x3d = sc_d * crd::math::cos(th_d + t23);
    const double xs  = (x1d + x3d < 2.0 * cc) ? x1d : x3d;
    const double xsx = -dd;
    const double xsy = xs + cc;
    const double ee  = xly * xsy;
    const double ff  = -(xlx * xsy) - xly * xsx;
    const double gg  = xlx * xsx;
    const double xmx = cc * ff - bb * gg;
    const double xmy = -(bb * ff) + cc * ee;
    const double r0  = xsx / xsy;
    const double r1  = xmx / xmy;
    const double r2  = xlx / xly;
    if (r0 < r1 && r0 < r2) { out[0] = r1; out[1] = r0; out[2] = r2; }
    else if (r2 < r0 && r2 < r1) { out[0] = r0; out[1] = r2; out[2] = r1; }
    else { out[0] = r0; out[1] = r1; out[2] = r2; }
}
} // namespace

TEST_CASE("B8-d: LTC SolveCubic (Blinn ellipse cubic) bit-exact vs Heitz ltc_code", "[kir][lighting][area]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int c0 = g.input(sh, kir::DType::F64); const int c1 = g.input(sh, kir::DType::F64); const int c2 = g.input(sh, kir::DType::F64);
    double    c0v[kN]; double c1v[kN]; double c2v[kN];
    for (int i = 0; i < kN; ++i)
    {
        // disk-realistic cubic from a valid ellipse (a,b>0, small centre offset) → c3 = 1.
        const double a = 1.5 + 0.08 * i;
        const double b = 4.0 + 0.15 * i;
        const double x0 = 0.15 + 0.01 * i;
        const double y0 = 0.1 - 0.004 * i;
        c0v[i] = a * b;
        c1v[i] = a * b * (1.0 + x0 * x0 + y0 * y0) - a - b;
        c2v[i] = 1.0 - a * (1.0 + x0 * x0) - b * (1.0 + y0 * y0);
    }
    const double* inp[] = {c0v, c1v, c2v};
    const int     coeff = g.vec4(c0, c1, c2, g.constant(1.0, sh, kir::DType::F64));

    double o[kN * 3];
    kir::eval_cpu(g, inp, &alloc, lt::solve_cubic(g, coeff), o);
    int bad = 0;
    for (int i = 0; i < kN; ++i)
    {
        const double cf[4] = {c0v[i], c1v[i], c2v[i], 1.0};
        double       ref[3];
        rsolve_cubic(cf, ref);
        for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref[c]) { ++bad; } }
    }
    CHECK(bad == 0);
}

namespace
{
double rltc_disk(const double n[3], const double v[3], const double p[3], const double minv[9], const double p0[3], const double p1[3], const double p2[3], double scale, bool two_sided)
{
    const double nov = rdot3(v, n);
    double       t1pre[3];
    for (int k = 0; k < 3; ++k) { t1pre[k] = v[k] - n[k] * nov; }
    double t1[3];
    rnorm3(t1pre, t1);
    double t2[3];
    rcross(n, t1, t2);
    double basis[9];
    rmat3_cols(t1, t2, n, basis);
    double rmat[9];
    rtranspose3(basis, rmat);
    double l0[3];
    double l1[3];
    double l2[3];
    { double d[3]; for (int k = 0; k < 3; ++k) { d[k] = p0[k] - p[k]; } rmatvec3(rmat, d, l0); }
    { double d[3]; for (int k = 0; k < 3; ++k) { d[k] = p1[k] - p[k]; } rmatvec3(rmat, d, l1); }
    { double d[3]; for (int k = 0; k < 3; ++k) { d[k] = p2[k] - p[k]; } rmatvec3(rmat, d, l2); }
    double cpre[3];
    double v1pre[3];
    double v2pre[3];
    for (int k = 0; k < 3; ++k) { cpre[k] = 0.5 * (l0[k] + l2[k]); v1pre[k] = 0.5 * (l1[k] - l2[k]); v2pre[k] = 0.5 * (l1[k] - l0[k]); }
    double cvec[3];
    double v1[3];
    double v2[3];
    rmatvec3(minv, cpre, cvec);
    rmatvec3(minv, v1pre, v1);
    rmatvec3(minv, v2pre, v2);
    double cr[3];
    rcross(v1, v2, cr);
    const bool   cull = rdot3(cr, cvec) < 0.0;
    const double d11  = rdot3(v1, v1);
    const double d22  = rdot3(v2, v2);
    const double d12  = rdot3(v1, v2);
    const double tr   = d11 + d22;
    const double det  = crd::math::sqrt(-d12 * d12 + d11 * d22);
    const double u    = 0.5 * crd::math::sqrt(tr - 2.0 * det);
    const double vv   = 0.5 * crd::math::sqrt(tr + 2.0 * det);
    const double emax = (u + vv) * (u + vv);
    const double emin = (u - vv) * (u - vv);
    const bool   d11gt = d11 > d22;
    double       v1gp[3];
    double       v2gp[3];
    for (int k = 0; k < 3; ++k)
    {
        v1gp[k] = d11gt ? (d12 * v1[k] + (emax - d11) * v2[k]) : (d12 * v2[k] + (emax - d22) * v1[k]);
        v2gp[k] = d11gt ? (d12 * v1[k] + (emin - d11) * v2[k]) : (d12 * v2[k] + (emin - d22) * v1[k]);
    }
    double v1gen[3];
    double v2gen[3];
    rnorm3(v1gp, v1gen);
    rnorm3(v2gp, v2gen);
    const double agen = 1.0 / emax;
    const double bgen = 1.0 / emin;
    const double aax  = 1.0 / d11;
    const double bax  = 1.0 / d22;
    double       v1ax[3];
    double       v2ax[3];
    for (int k = 0; k < 3; ++k) { v1ax[k] = v1[k] * crd::math::sqrt(aax); v2ax[k] = v2[k] * crd::math::sqrt(bax); }
    const bool   ecc = (d12 < 0.0 ? -d12 : d12) / crd::math::sqrt(d11 * d22) > 0.0001;
    const double a_val  = ecc ? agen : aax;
    const double b_val  = ecc ? bgen : bax;
    double       vv1[3];
    double       vv2[3];
    for (int k = 0; k < 3; ++k) { vv1[k] = ecc ? v1gen[k] : v1ax[k]; vv2[k] = ecc ? v2gen[k] : v2ax[k]; }
    double v3[3];
    rcross(vv1, vv2, v3);
    if (rdot3(cvec, v3) < 0.0) { for (int k = 0; k < 3; ++k) { v3[k] = -v3[k]; } }
    const double ld  = rdot3(v3, cvec);
    const double x0  = rdot3(vv1, cvec) / ld;
    const double y0  = rdot3(vv2, cvec) / ld;
    const double l2q = ld * ld;
    const double a2  = a_val * l2q;
    const double b2  = b_val * l2q;
    const double x0s = x0 * x0;
    const double y0s = y0 * y0;
    const double k0  = a2 * b2;
    const double k1  = k0 * (1.0 + x0s + y0s) - a2 - b2;
    const double k2  = 1.0 - a2 * (1.0 + x0s) - b2 * (1.0 + y0s);
    const double cf[4] = {k0, k1, k2, 1.0};
    double       roots[3];
    rsolve_cubic(cf, roots);
    const double e1r = roots[0];
    const double e2r = roots[1];
    const double e3r = roots[2];
    const double adx = a2 * x0 / (a2 - e2r);
    const double ady = b2 * y0 / (b2 - e2r);
    double       rot[9];
    rmat3_cols(vv1, vv2, v3, rot);
    const double avgv[3] = {adx, ady, 1.0};
    double       avgpre[3];
    rmatvec3(rot, avgv, avgpre);
    double avg[3];
    rnorm3(avgpre, avg);
    const double l1r  = crd::math::sqrt(-e2r / e3r);
    const double l2r  = crd::math::sqrt(-e2r / e1r);
    const double ffac = l1r * l2r * crd::math::rsqrt((1.0 + l1r * l1r) * (1.0 + l2r * l2r));
    double       spec = ffac * scale;
    if (!two_sided && cull) { spec = 0.0; }
    return spec;
}
} // namespace

TEST_CASE("B8-d: LTC LUT Minv reconstruction (isotropic + anisotropic) bit-exact", "[kir][lighting][area]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int ax = g.input(sh, kir::DType::F64); const int ay = g.input(sh, kir::DType::F64); const int az = g.input(sh, kir::DType::F64); const int aw = g.input(sh, kir::DType::F64);
    const int bx = g.input(sh, kir::DType::F64); const int by = g.input(sh, kir::DType::F64);
    double    t1[4][kN]; double t2[2][kN];
    for (int i = 0; i < kN; ++i)
    {
        t1[0][i] = 1.0 + 0.02 * i; t1[1][i] = 0.2 - 0.01 * i; t1[2][i] = 0.15 + 0.005 * i; t1[3][i] = 0.9 - 0.01 * i;
        t2[0][i] = 0.05 + 0.008 * i; t2[1][i] = 1.0 - 0.01 * i;
    }
    const double* inp[] = {t1[0], t1[1], t1[2], t1[3], t2[0], t2[1]};
    const int     v1  = g.vec4(ax, ay, az, aw);
    const int     v2  = g.vec2(bx, by);

    int bad = 0;
    { // isotropic: columns (a,0,c),(0,1,0),(b,0,d) → column-major [a,0,c, 0,1,0, b,0,d]
        double o[kN * 9];
        kir::eval_cpu(g, inp, &alloc, lt::ltc_matrix(g, v1), o);
        for (int i = 0; i < kN; ++i)
        {
            const double want[9] = {t1[0][i], 0.0, t1[2][i], 0.0, 1.0, 0.0, t1[1][i], 0.0, t1[3][i]};
            for (int c = 0; c < 9; ++c) { if (o[i * 9 + c] != want[c]) { ++bad; } }
        }
    }
    { // anisotropic: columns (m00,m01,m20),(m01,m11,0),(m02,0,m22)
        double o[kN * 9];
        kir::eval_cpu(g, inp, &alloc, lt::ltc_matrix_aniso(g, v1, v2), o);
        for (int i = 0; i < kN; ++i)
        {
            const double want[9] = {t1[0][i], t2[0][i], t1[2][i], t2[0][i], t2[1][i], 0.0, t1[1][i], 0.0, t1[3][i]};
            for (int c = 0; c < 9; ++c) { if (o[i * 9 + c] != want[c]) { ++bad; } }
        }
    }
    CHECK(bad == 0);
}

TEST_CASE("B8-d: LTC disk/sphere area light bit-exact vs Heitz ltc_code", "[kir][lighting][area]")
{
    crd::memory::TlsfAllocator alloc(80U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int px = g.input(sh, kir::DType::F64); const int py = g.input(sh, kir::DType::F64); const int pz = g.input(sh, kir::DType::F64);
    double    parr[3][kN];
    for (int i = 0; i < kN; ++i) { parr[0][i] = 0.18 * i - 1.8; parr[1][i] = 0.1 * i - 1.0; parr[2][i] = 0.0; }
    const double* inp[] = {parr[0], parr[1], parr[2]};

    const auto kc = [&](double x, double y, double z) { return g.vec3(g.constant(x, sh, kir::DType::F64), g.constant(y, sh, kir::DType::F64), g.constant(z, sh, kir::DType::F64)); };
    const int  n    = kc(0.0, 0.0, 1.0);
    const int  view = g.normalize(kc(0.2, 0.15, 1.0));
    const int  pp   = g.vec3(px, py, pz);
    // disk: center (0,0,2.5), dirx=(1,0,0)·1.0, diry=(0,1,0)·0.8 → the 3 encoding corners.
    const int  q0 = kc(1.0, -0.8, 2.5); const int q1 = kc(1.0, 0.8, 2.5); const int q2 = kc(-1.0, 0.8, 2.5);
    const int  ident = g.mat3(kc(1.0, 0.0, 0.0), kc(0.0, 1.0, 0.0), kc(0.0, 0.0, 1.0));
    const int  minv  = g.mat3(kc(0.85, 0.0, 0.06), kc(0.0, 0.75, 0.0), kc(0.12, 0.0, 1.0));
    const int  scl   = g.constant(0.9, sh, kir::DType::F64);

    const double nr[3]  = {0.0, 0.0, 1.0};
    double       vr[3]  = {0.2, 0.15, 1.0};
    { double vp[3] = {0.2, 0.15, 1.0}; rnorm3(vp, vr); }
    const double q0r[3] = {1.0, -0.8, 2.5};
    const double q1r[3] = {1.0, 0.8, 2.5};
    const double q2r[3] = {-1.0, 0.8, 2.5};
    const double ident_r[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    const double minv_r[9]  = {0.85, 0.0, 0.06, 0.0, 0.75, 0.0, 0.12, 0.0, 1.0};
    int          bad        = 0;
    const auto   chk = [&](int node, const double* mr) {
        double o[kN];
        kir::eval_cpu(g, inp, &alloc, node, o);
        for (int i = 0; i < kN; ++i)
        {
            const double p_i[3] = {parr[0][i], parr[1][i], parr[2][i]};
            if (o[i] != rltc_disk(nr, vr, p_i, mr, q0r, q1r, q2r, 0.9, true)) { ++bad; }
        }
    };
    chk(lt::ltc_evaluate_disk(g, n, view, pp, ident, q0, q1, q2, scl, true), ident_r);
    chk(lt::ltc_evaluate_disk(g, n, view, pp, minv, q0, q1, q2, scl, true), minv_r);
    CHECK(bad == 0);
}

namespace
{
void rshadow_project(const double wp[3], const double vp[16], double out[3])
{
    const double wp4[4] = {wp[0], wp[1], wp[2], 1.0};
    double       clip[4];
    for (int r = 0; r < 4; ++r) { double s = 0.0; for (int c = 0; c < 4; ++c) { s = s + vp[c * 4 + r] * wp4[c]; } clip[r] = s; } // column-major matvec, oracle order
    const double invw = 1.0 / clip[3];
    out[0] = (clip[0] * invw) * 0.5 + 0.5;
    out[1] = (clip[1] * invw) * 0.5 + 0.5;
    out[2] = clip[2] * invw;
}
void rnormal_offset(const double wp[3], const double n[3], double nol, double scale, double out[3])
{
    const double sin_a = crd::math::sqrt(rmax2(1.0 - nol * nol, 0.0));
    const double off   = scale * sin_a;
    for (int k = 0; k < 3; ++k) { out[k] = wp[k] + n[k] * off; }
}
double rslope_scaled(double nol, double base, double max_bias)
{
    const double v = base * (crd::math::sqrt(rmax2(1.0 - nol * nol, 0.0)) / nol);
    const double m = v > 0.0 ? v : 0.0;
    return m < max_bias ? m : max_bias;
}
void rreceiver_plane(const double dx[3], const double dy[3], double out[2])
{
    const double bx  = dy[1] * dx[2] - dx[1] * dy[2];
    const double by  = dx[0] * dy[2] - dy[0] * dx[2];
    const double det = dx[0] * dy[1] - dx[1] * dy[0];
    const double inv = 1.0 / det;
    out[0] = bx * inv;
    out[1] = by * inv;
}
} // namespace

TEST_CASE("B8-f: shadow-map projection + bias stack (normal-offset/slope-scaled/receiver-plane) bit-exact", "[kir][lighting][shadow]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int wx = g.input(sh, kir::DType::F64); const int wy = g.input(sh, kir::DType::F64); const int wz = g.input(sh, kir::DType::F64);
    const int nx = g.input(sh, kir::DType::F64); const int ny = g.input(sh, kir::DType::F64); const int nz = g.input(sh, kir::DType::F64);
    const int nl = g.input(sh, kir::DType::F64);
    const int ax = g.input(sh, kir::DType::F64); const int ay = g.input(sh, kir::DType::F64); const int az = g.input(sh, kir::DType::F64); // ddx(shadowCoord)
    const int bx = g.input(sh, kir::DType::F64); const int by = g.input(sh, kir::DType::F64); const int bz = g.input(sh, kir::DType::F64); // ddy(shadowCoord)
    double    warr[3][kN]; double narr[3][kN]; double nlv[kN]; double dxa[3][kN]; double dya[3][kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        warr[0][i] = 0.1 * i - 0.9; warr[1][i] = 0.06 * i - 0.5; warr[2][i] = 0.03 * i - 0.3;
        narr[0][i] = 0.05 * i - 0.4; narr[1][i] = 0.2; narr[2][i] = 1.0; unit(narr[0][i], narr[1][i], narr[2][i]);
        nlv[i] = 0.2 + 0.035 * i;
        dxa[0][i] = 0.03 + 0.002 * i; dxa[1][i] = 0.001 * i - 0.005; dxa[2][i] = 0.02 + 0.001 * i;
        dya[0][i] = 0.001 * i - 0.004; dya[1][i] = 0.03 - 0.001 * i; dya[2][i] = 0.015 + 0.0008 * i;
    }
    const double* inp[] = {warr[0], warr[1], warr[2], narr[0], narr[1], narr[2], nlv, dxa[0], dxa[1], dxa[2], dya[0], dya[1], dya[2]};

    const int  wp   = g.vec3(wx, wy, wz);
    const int  nn   = g.vec3(nx, ny, nz);
    const int  dxv  = g.vec3(ax, ay, az);
    const int  dyv  = g.vec3(bx, by, bz);
    const auto kv4  = [&](double x, double y, double z, double w) { return g.vec4(g.constant(x, sh, kir::DType::F64), g.constant(y, sh, kir::DType::F64), g.constant(z, sh, kir::DType::F64), g.constant(w, sh, kir::DType::F64)); };
    const int  lvp  = g.mat4(kv4(1.0, 0.0, 0.0, 0.0), kv4(0.0, 1.0, 0.0, 0.0), kv4(0.15, 0.1, 0.6, 0.25), kv4(0.05, -0.05, 0.4, 1.0));
    const double vp16[16] = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.15, 0.1, 0.6, 0.25, 0.05, -0.05, 0.4, 1.0};
    const int  scale = g.constant(0.03, sh, kir::DType::F64);
    const int  base  = g.constant(0.002, sh, kir::DType::F64);
    const int  maxb  = g.constant(0.01, sh, kir::DType::F64);

    int        bad = 0;
    const auto chkv = [&](int node, int comps, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < comps; ++c) { if (o[i * comps + c] != ref(i, c)) { ++bad; } } } };
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };

    chkv(lt::shadow_project(g, wp, lvp), 3, [&](int i, int c) { const double w_i[3] = {warr[0][i], warr[1][i], warr[2][i]}; double o[3]; rshadow_project(w_i, vp16, o); return o[c]; });
    chkv(lt::normal_offset_bias(g, wp, nn, nl, scale), 3, [&](int i, int c) { const double w_i[3] = {warr[0][i], warr[1][i], warr[2][i]}; const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]}; double o[3]; rnormal_offset(w_i, n_i, nlv[i], 0.03, o); return o[c]; });
    chk(lt::slope_scaled_bias(g, nl, base, maxb), [&](int i) { return rslope_scaled(nlv[i], 0.002, 0.01); });
    chkv(lt::receiver_plane_bias(g, dxv, dyv), 2, [&](int i, int c) { const double dx_i[3] = {dxa[0][i], dxa[1][i], dxa[2][i]}; const double dy_i[3] = {dya[0][i], dya[1][i], dya[2][i]}; double o[2]; rreceiver_plane(dx_i, dy_i, o); return o[c]; });

    CHECK(bad == 0);
}

namespace
{
double rfract(double x) { return x - crd::math::floor(x); }
double rign(double fx, double fy) { return rfract(52.9829189 * rfract(fx * 0.06711056 + fy * 0.00583715)); }
double rpcss(double zr, double zb, double ls) { return ((zr - zb) / zb) * ls; }
double rchebyshev(double m1, double m2, double t, double minvar)
{
    const double var  = rmax2(m2 - m1 * m1, minvar);
    const double d    = t - m1;
    const double pmax = var / (var + d * d);
    return (t <= m1) ? 1.0 : pmax;
}
double rreduce(double p, double a) { return rclamp01((p - a) / (1.0 - a)); }
double revsm_shadow(const double m[4], double d, double cp, double cn, double minvar, double bleed)
{
    const double wp = crd::math::exp(cp * d);
    const double wn = -crd::math::exp((-cn) * d);
    const double pp = rchebyshev(m[0], m[1], wp, minvar);
    const double pn = rchebyshev(m[2], m[3], wn, minvar);
    return rreduce(pp < pn ? pp : pn, bleed);
}
double rmsm(const double m[4], double zf, double depth_bias, double moment_bias)
{
    const double b0     = m[0] * (1.0 - moment_bias) + 0.0 * moment_bias;
    const double b1     = m[1] * (1.0 - moment_bias) + 0.375 * moment_bias;
    const double b2     = m[2] * (1.0 - moment_bias) + 0.0 * moment_bias;
    const double b3     = m[3] * (1.0 - moment_bias) + 0.375 * moment_bias;
    const double z0     = zf - depth_bias;
    const double l21d11 = -(b0 * b1) + b2;
    const double d11    = -(b0 * b0) + b1;
    const double sqvar  = -(b1 * b1) + b3;
    const double d22d11 = sqvar * d11 - l21d11 * l21d11;
    const double invd11 = 1.0 / d11;
    const double l21    = l21d11 * invd11;
    const double invd22 = d11 / d22d11;
    double       c1     = z0 - b0;
    double       c2     = (z0 * z0 - b1) - l21 * c1;
    c1                  = c1 * invd11;
    c2                  = c2 * invd22;
    c1                  = c1 - l21 * c2;
    const double c0     = 1.0 - (c1 * b0 + c2 * b1);
    const double p      = c1 / c2;
    const double q      = c0 / c2;
    const double r      = crd::math::sqrt(rmax2((p * p) * 0.25 - q, 0.0));
    const double z1     = (-p) * 0.5 - r;
    const double z2     = (-p) * 0.5 + r;
    double       s[4];
    if (z2 < z0) { s[0] = z1; s[1] = z0; s[2] = 1.0; s[3] = 1.0; }
    else if (z1 < z0) { s[0] = z0; s[1] = z1; s[2] = 0.0; s[3] = 1.0; }
    else { s[0] = 0.0; s[1] = 0.0; s[2] = 0.0; s[3] = 0.0; }
    const double quotient  = (s[0] * z2 - b0 * (s[0] + z2) + b1) / ((z2 - s[1]) * (z0 - z1));
    const double intensity = s[2] + s[3] * quotient;
    return 1.0 - rclamp01(intensity);
}
} // namespace

TEST_CASE("B8-g: filtered soft shadows (IGN/PCSS/EVSM/MSM) bit-exact vs reference", "[kir][lighting][softshadow]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int fx = g.input(sh, kir::DType::F64); const int fy = g.input(sh, kir::DType::F64);
    const int zr = g.input(sh, kir::DType::F64); const int zb = g.input(sh, kir::DType::F64); const int ls = g.input(sh, kir::DType::F64);
    const int em0 = g.input(sh, kir::DType::F64); const int em1 = g.input(sh, kir::DType::F64); const int em2 = g.input(sh, kir::DType::F64); const int em3 = g.input(sh, kir::DType::F64); const int ez = g.input(sh, kir::DType::F64);
    const int mm0 = g.input(sh, kir::DType::F64); const int mm1 = g.input(sh, kir::DType::F64); const int mm2 = g.input(sh, kir::DType::F64); const int mm3 = g.input(sh, kir::DType::F64); const int mz = g.input(sh, kir::DType::F64);

    double fxv[kN]; double fyv[kN]; double zrv[kN]; double zbv[kN]; double lsv[kN];
    double emv[4][kN]; double ezv[kN]; double mmv[4][kN]; double mzv[kN];
    for (int i = 0; i < kN; ++i)
    {
        fxv[i] = 1.0 + 1.7 * i; fyv[i] = 2.0 + 1.3 * i;
        zrv[i] = 0.4 + 0.02 * i; zbv[i] = 0.2 + 0.015 * i; lsv[i] = 0.05 + 0.01 * i;
        // EVSM moments = average of the exp-warps of two edge depths d_a,d_b (cp=40, cn=8).
        const double da = 0.3 + 0.02 * i;
        const double db = 0.55 - 0.01 * i;
        const double cp = 40.0;
        const double cn = 8.0;
        const double wpa = crd::math::exp(cp * da); const double wpb = crd::math::exp(cp * db);
        const double wna = -crd::math::exp((-cn) * da); const double wnb = -crd::math::exp((-cn) * db);
        emv[0][i] = 0.5 * (wpa + wpb); emv[1][i] = 0.5 * (wpa * wpa + wpb * wpb); emv[2][i] = 0.5 * (wna + wnb); emv[3][i] = 0.5 * (wna * wna + wnb * wnb);
        ezv[i] = 0.35 + 0.015 * i;
        // MSM moments = average of (d,d²,d³,d⁴) over THREE distinct depths (a well-conditioned filtered penumbra — a 2-point
        // distribution makes the 4-moment Hankel matrix rank-deficient → NaN, so the filter must span ≥3 depths).
        const double dc = 0.4 + 0.005 * i;
        mmv[0][i] = (da + db + dc) / 3.0;
        mmv[1][i] = (da * da + db * db + dc * dc) / 3.0;
        mmv[2][i] = (da * da * da + db * db * db + dc * dc * dc) / 3.0;
        mmv[3][i] = (da * da * da * da + db * db * db * db + dc * dc * dc * dc) / 3.0;
        mzv[i] = 0.35 + 0.012 * i;
    }
    const double* inp[] = {fxv, fyv, zrv, zbv, lsv, emv[0], emv[1], emv[2], emv[3], ezv, mmv[0], mmv[1], mmv[2], mmv[3], mzv};

    const auto kc  = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    const auto chk = [&](int node, auto ref) { int b = 0; double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++b; } } return b; };

    const int b_ign  = chk(lt::detail::ign(g, g.vec2(fx, fy)), [&](int i) { return rign(fxv[i], fyv[i]); });
    const int b_pcss = chk(lt::pcss_penumbra(g, zr, zb, ls), [&](int i) { return rpcss(zrv[i], zbv[i], lsv[i]); });
    const int b_evsm = chk(lt::evsm_shadow(g, g.vec4(em0, em1, em2, em3), ez, kc(40.0), kc(8.0), kc(1e-4), kc(0.2)), [&](int i) { const double m[4] = {emv[0][i], emv[1][i], emv[2][i], emv[3][i]}; return revsm_shadow(m, ezv[i], 40.0, 8.0, 1e-4, 0.2); });
    const int b_msm  = chk(lt::msm_hamburger(g, g.vec4(mm0, mm1, mm2, mm3), mz, kc(0.0), kc(0.003)), [&](int i) { const double m[4] = {mmv[0][i], mmv[1][i], mmv[2][i], mmv[3][i]}; return rmsm(m, mzv[i], 0.0, 0.003); });
    WARN("[softshadow] bad ign=" << b_ign << " pcss=" << b_pcss << " evsm=" << b_evsm << " msm=" << b_msm);
    CHECK(b_ign == 0);
    CHECK(b_pcss == 0);
    CHECK(b_evsm == 0);
    CHECK(b_msm == 0);
}

namespace
{
double rcsm_split(double near, double far, double lambda, double i, double count)
{
    const double si = i / count;
    const double lg = near * crd::math::exp(si * crd::math::log(far / near));
    const double un = near + (far - near) * si;
    return lambda * lg + (1.0 - lambda) * un;
}
double rstep(double edge, double v) { return v < edge ? 0.0 : 1.0; }
double rcsm_select(double d, double s0, double s1, double s2) { return (rstep(s0, d) + rstep(s1, d)) + rstep(s2, d); }
void   rcsm_snap(const double uv[2], double ms, double out[2]) { for (int k = 0; k < 2; ++k) { out[k] = crd::math::nearbyint(uv[k] * ms) / ms; } }
double rcsm_blend(double d, double split, double w) { return rclamp01((d - (split - w)) / w); }
} // namespace

TEST_CASE("B8-h: cascaded shadow maps (split/select/texel-snap/blend) bit-exact", "[kir][lighting][csm]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int nf = g.input(sh, kir::DType::F64); const int ff = g.input(sh, kir::DType::F64); const int lam = g.input(sh, kir::DType::F64); const int ci = g.input(sh, kir::DType::F64);
    const int vd = g.input(sh, kir::DType::F64); const int s0 = g.input(sh, kir::DType::F64); const int s1 = g.input(sh, kir::DType::F64); const int s2 = g.input(sh, kir::DType::F64);
    const int ux = g.input(sh, kir::DType::F64); const int uy = g.input(sh, kir::DType::F64); const int spl = g.input(sh, kir::DType::F64); const int bw = g.input(sh, kir::DType::F64);
    double    nfv[kN]; double ffv[kN]; double lamv[kN]; double civ[kN]; double vdv[kN]; double s0v[kN]; double s1v[kN]; double s2v[kN]; double uxv[kN]; double uyv[kN]; double splv[kN]; double bwv[kN];
    for (int i = 0; i < kN; ++i)
    {
        nfv[i] = 0.1 + 0.01 * i; ffv[i] = 100.0 + 5.0 * i; lamv[i] = 0.2 + 0.03 * i; civ[i] = static_cast<double>(i % 4);
        vdv[i] = 0.1 * i; s0v[i] = 0.5; s1v[i] = 1.0; s2v[i] = 1.5;
        uxv[i] = 0.13 + 0.04 * i; uyv[i] = 0.27 + 0.03 * i; splv[i] = 0.8 + 0.05 * i; bwv[i] = 0.1 + 0.005 * i;
    }
    const double* inp[] = {nfv, ffv, lamv, civ, vdv, s0v, s1v, s2v, uxv, uyv, splv, bwv};

    const auto kc  = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chkv = [&](int node, auto ref) { double o[kN * 2]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 2; ++c) { if (o[i * 2 + c] != ref(i, c)) { ++bad; } } } };

    chk(lt::csm_split_practical(g, nf, ff, lam, ci, kc(4.0)), [&](int i) { return rcsm_split(nfv[i], ffv[i], lamv[i], civ[i], 4.0); });
    chk(lt::csm_select_cascade(g, vd, s0, s1, s2), [&](int i) { return rcsm_select(vdv[i], s0v[i], s1v[i], s2v[i]); });
    chkv(lt::csm_texel_snap(g, g.vec2(ux, uy), kc(1024.0)), [&](int i, int c) { const double u[2] = {uxv[i], uyv[i]}; double o[2]; rcsm_snap(u, 1024.0, o); return o[c]; });
    chk(lt::csm_blend_factor(g, vd, spl, bw), [&](int i) { return rcsm_blend(vdv[i], splv[i], bwv[i]); });

    CHECK(bad == 0);
}

namespace
{
double rmax(double x, double y) { return x > y ? x : y; }
double rcontact(const double rz[4], const double sz[4], double bias, double thick, double fade)
{
    double o[4];
    for (int k = 0; k < 4; ++k) { const double d = rz[k] - sz[k]; const double lo = d < bias ? 0.0 : 1.0; const double hi = thick < d ? 0.0 : 1.0; o[k] = lo * hi; }
    const double occ = rmax(rmax(o[0], o[1]), rmax(o[2], o[3]));
    return rclamp01(1.0 - occ * fade);
}
double rfom(double a0, double a1, double b1, double a2, double b2, double d)
{
    const double pi = 3.14159265359;
    const double w1 = (2.0 * pi) * d;
    const double w2 = (4.0 * pi) * d;
    const double t1 = (1.0 / pi) * (a1 * crd::math::sin(w1) + b1 * (1.0 - crd::math::cos(w1)));
    const double t2 = (1.0 / (2.0 * pi)) * (a2 * crd::math::sin(w2) + b2 * (1.0 - crd::math::cos(w2)));
    const double tau = ((0.5 * a0) * d + t1) + t2;
    return crd::math::exp(0.0 - tau);
}
double rvsm_level(double z, double base, double maxl) { const double l = crd::math::floor(crd::math::log2(z / base)); const double m = l > 0.0 ? l : 0.0; return m < maxl ? m : maxl; }
void   rvsm_page(const double uv[2], double pages, double out[2]) { for (int k = 0; k < 2; ++k) { out[k] = crd::math::floor(uv[k] * pages); } }
} // namespace

TEST_CASE("B8-i: screen-space + translucent shadows (contact / Fourier-opacity / VSM addressing) bit-exact", "[kir][lighting][ssshadow]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int rz0 = g.input(sh, kir::DType::F64); const int rz1 = g.input(sh, kir::DType::F64); const int rz2 = g.input(sh, kir::DType::F64); const int rz3 = g.input(sh, kir::DType::F64);
    const int sz0 = g.input(sh, kir::DType::F64); const int sz1 = g.input(sh, kir::DType::F64); const int sz2 = g.input(sh, kir::DType::F64); const int sz3 = g.input(sh, kir::DType::F64);
    const int fa0 = g.input(sh, kir::DType::F64); const int fa1 = g.input(sh, kir::DType::F64); const int fb1 = g.input(sh, kir::DType::F64); const int fa2 = g.input(sh, kir::DType::F64); const int fb2 = g.input(sh, kir::DType::F64); const int fd = g.input(sh, kir::DType::F64);
    const int vz  = g.input(sh, kir::DType::F64); const int ux = g.input(sh, kir::DType::F64); const int uy = g.input(sh, kir::DType::F64);
    double rz0v[kN]; double rz1v[kN]; double rz2v[kN]; double rz3v[kN]; double sz0v[kN]; double sz1v[kN]; double sz2v[kN]; double sz3v[kN];
    double fa0v[kN]; double fa1v[kN]; double fb1v[kN]; double fa2v[kN]; double fb2v[kN]; double fdv[kN]; double vzv[kN]; double uxv[kN]; double uyv[kN];
    for (int i = 0; i < kN; ++i)
    {
        rz0v[i] = 0.30 + 0.01 * i; rz1v[i] = 0.50; rz2v[i] = 0.20 + 0.02 * i; rz3v[i] = 0.90 - 0.01 * i;
        sz0v[i] = 0.20 + 0.02 * i; sz1v[i] = 0.45; sz2v[i] = 0.25; sz3v[i] = 0.40 + 0.01 * i;
        fa0v[i] = 0.50 + 0.05 * i; fa1v[i] = 0.20; fb1v[i] = 0.30; fa2v[i] = 0.10; fb2v[i] = 0.15; fdv[i] = 0.05 + 0.045 * i;
        vzv[i] = 0.50 + 0.70 * i; uxv[i] = 0.03 + 0.048 * i; uyv[i] = 0.07 + 0.045 * i;
    }
    const double* inp[] = {rz0v, rz1v, rz2v, rz3v, sz0v, sz1v, sz2v, sz3v, fa0v, fa1v, fb1v, fa2v, fb2v, fdv, vzv, uxv, uyv};

    const auto kc  = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chkv = [&](int node, auto ref) { double o[kN * 2]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 2; ++c) { if (o[i * 2 + c] != ref(i, c)) { ++bad; } } } };

    chk(lt::contact_shadow(g, g.vec4(rz0, rz1, rz2, rz3), g.vec4(sz0, sz1, sz2, sz3), kc(0.01), kc(0.5), kc(0.9)),
        [&](int i) { const double rz[4] = {rz0v[i], rz1v[i], rz2v[i], rz3v[i]}; const double sz[4] = {sz0v[i], sz1v[i], sz2v[i], sz3v[i]}; return rcontact(rz, sz, 0.01, 0.5, 0.9); });
    chk(lt::fourier_opacity_transmittance(g, fa0, fa1, fb1, fa2, fb2, fd), [&](int i) { return rfom(fa0v[i], fa1v[i], fb1v[i], fa2v[i], fb2v[i], fdv[i]); });
    chk(lt::vsm_clipmap_level(g, vz, kc(1.0), kc(6.0)), [&](int i) { return rvsm_level(vzv[i], 1.0, 6.0); });
    chkv(lt::vsm_page_coord(g, g.vec2(ux, uy), kc(16.0)), [&](int i, int c) { const double u[2] = {uxv[i], uyv[i]}; double o[2]; rvsm_page(u, 16.0, o); return o[c]; });

    CHECK(bad == 0);
}

namespace
{
void   rmatvec4(const double m[16], const double v[4], double out[4]) { for (int r = 0; r < 4; ++r) { double s = 0.0; for (int col = 0; col < 4; ++col) { s += m[col * 4 + r] * v[col]; } out[r] = s; } }
void   rcross3(const double a[3], const double b[3], double out[3]) { out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0]; }
double rdot4(const double a[4], const double b[4]) { double s = 0.0; for (int k = 0; k < 4; ++k) { s += a[k] * b[k]; } return s; }
double rlen4(const double a[4]) { double s = 0.0; for (int k = 0; k < 4; ++k) { s += a[k] * a[k]; } return crd::math::sqrt(s); }
void   rlbs(const double* M[4], const double w[4], const double p[3], bool is_normal, double out[3])
{
    const double p4[4] = {p[0], p[1], p[2], is_normal ? 0.0 : 1.0};
    double       c[4][3];
    for (int i = 0; i < 4; ++i) { double mv[4]; rmatvec4(M[i], p4, mv); for (int k = 0; k < 3; ++k) { c[i][k] = mv[k] * w[i]; } }
    for (int k = 0; k < 3; ++k) { out[k] = (c[0][k] + c[1][k]) + (c[2][k] + c[3][k]); }
    if (is_normal) { const double len = crd::math::sqrt((out[0] * out[0] + out[1] * out[1]) + out[2] * out[2]); for (int k = 0; k < 3; ++k) { out[k] /= len; } }
}
void rdquat(const double r0[4], const double d0[4], const double r1[4], const double d1[4], double w0, double w1, const double p[3], double out[3])
{
    const double dt  = rdot4(r0, r1);
    const double sgn = (2.0 * (dt < 0.0 ? 0.0 : 1.0)) - 1.0;
    const double w1s = w1 * sgn;
    double       br[4]; double bd[4];
    for (int k = 0; k < 4; ++k) { br[k] = r0[k] * w0 + r1[k] * w1s; bd[k] = d0[k] * w0 + d1[k] * w1s; }
    const double len = rlen4(br);
    double       qr[4]; double qd[4];
    for (int k = 0; k < 4; ++k) { qr[k] = br[k] / len; qd[k] = bd[k] / len; }
    const double rxyz[3] = {qr[0], qr[1], qr[2]}; const double rw = qr[3];
    const double dxyz[3] = {qd[0], qd[1], qd[2]}; const double dw = qd[3];
    double       cr1[3]; rcross3(rxyz, p, cr1);
    double       inner[3]; for (int k = 0; k < 3; ++k) { inner[k] = cr1[k] + p[k] * rw; }
    double       cr2[3]; rcross3(rxyz, inner, cr2);
    double       rot[3]; for (int k = 0; k < 3; ++k) { rot[k] = p[k] + cr2[k] * 2.0; }
    double       cr3[3]; rcross3(rxyz, dxyz, cr3);
    double       tt[3]; for (int k = 0; k < 3; ++k) { tt[k] = (dxyz[k] * rw - rxyz[k] * dw) + cr3[k]; }
    for (int k = 0; k < 3; ++k) { out[k] = rot[k] + tt[k] * 2.0; }
}
} // namespace

namespace
{
// a simple cookable material: base_color from the uv, fixed metallic/roughness, the interpolated normal, 0.8 opacity. Built in
// F32 — the material/surface struct is F32-typed (GPU-native); lowering stays bit-stable in F32 so the round-trip still holds.
int cook_test_surface(kir::KGraph& g, int struct_id, const ck::SurfaceInputs& in, void* /*user*/)
{
    const auto sh   = kir::make_shape({kN});
    const auto kf   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  base = g.vec3(g.swizzle(in.uv, 0), g.swizzle(in.uv, 1), kf(0.5));
    const int  emis = g.vec3(kf(0.0), kf(0.0), kf(0.0));
    return mat::build_surface(g, struct_id, base, kf(0.1), kf(0.5), in.world_normal, emis, kf(1.0), kf(0.8));
}
} // namespace

namespace
{
double rcluster_z_slice(double z, double near, double far, double ns) { return crd::math::floor((crd::math::log(z / near) / crd::math::log(far / near)) * ns); }
double rsq_dist_aabb(const double c[3], const double amin[3], const double amax[3])
{
    double s = 0.0;
    for (int k = 0; k < 3; ++k) { const double m = c[k] > amin[k] ? c[k] : amin[k]; const double cl = m < amax[k] ? m : amax[k]; const double d = c[k] - cl; s += d * d; }
    return s;
}
double rin1(double a) { const double ab = a < 0.0 ? -a : a; return ab > 0.5 ? 0.0 : 1.0; }
} // namespace

namespace
{
double rgtao_slice(double h1, double h2, double gamma, double nlen)
{
    const double cg    = crd::math::cos(gamma);
    const double sg    = crd::math::sin(gamma);
    const auto   inner = [&](double h) { return (-crd::math::cos(2.0 * h - gamma) + cg) + (2.0 * h * sg); };
    return (0.25 * nlen) * (inner(h1) + inner(h2));
}
void rgtao_mb(double vis, const double alb[3], double out[3])
{
    for (int c = 0; c < 3; ++c)
    {
        const double a = alb[c] * 2.0404 + (-0.3324);
        const double b = alb[c] * (-4.7951) + 0.6417;
        const double cc = alb[c] * 2.7552 + 0.6903;
        const double p = (((vis * a + b) * vis + cc) * vis);
        out[c] = vis > p ? vis : p;
    }
}
double rspec_occ(double nov, double ao, double rough)
{
    const double e = crd::math::exp2(-16.0 * rough - 1.0);
    const double p = crd::math::pow(nov + ao, e);
    return rclamp01((p - 1.0) + ao);
}
crd::i64 rssilvb_mask(double min_h, double max_h, double nsec)
{
    const double   sf      = crd::math::floor(min_h * nsec);
    const double   start_f = sf < (nsec - 1.0) ? sf : (nsec - 1.0);
    const double   raw_f   = crd::math::ceil((max_h - min_h) * nsec);
    const double   sub     = nsec - start_f;
    const double   max_cnt = 31.0 < sub ? 31.0 : sub;
    const double   count_f = rclamp(raw_f, 0.0, max_cnt);
    const crd::i64 s       = static_cast<crd::i64>(start_f);
    const crd::i64 c       = static_cast<crd::i64>(count_f);
    const crd::i64 bits    = (static_cast<crd::i64>(1) << c) - static_cast<crd::i64>(1);
    return bits << s;
}
double rssilvb_ao(crd::i64 bitfield, double nsec)
{
    crd::u32 v   = static_cast<crd::u32>(bitfield);
    int      cnt = 0;
    while (v != 0U) { cnt += static_cast<int>(v & 1U); v >>= 1U; }
    return 1.0 - static_cast<double>(cnt) / nsec;
}
} // namespace

namespace
{
double rreflect(const double iv[3], const double nv[3], int c)
{
    double dp = 0.0;
    for (int k = 0; k < 3; ++k) { dp += nv[k] * iv[k]; }
    return iv[c] - (2.0 * dp) * nv[c];
}
double rssr_hit(double rz, double sz, double thick)
{
    const double d = rz - sz;
    return (d < 0.0 ? 0.0 : 1.0) * (thick < d ? 0.0 : 1.0);
}
double rssr_edge(double ux, double uy, double border)
{
    const double ex = ux < (1.0 - ux) ? ux : (1.0 - ux);
    const double ey = uy < (1.0 - uy) ? uy : (1.0 - uy);
    return rsmooth(0.0, border, ex < ey ? ex : ey);
}
double rssr_conf(double rough, double edge, double dist) { return rclamp01(((1.0 - rough) * edge) * crd::math::exp(-2.0 * dist)); }
} // namespace

namespace
{
double rev100(double lum) { return crd::math::log2(lum * (100.0 / 12.5)); }
double rexposure(double ev) { return 1.0 / (1.2 * crd::math::exp2(ev)); }
double ragx_contrast(double x)
{
    const double x2 = x * x; const double x4 = x2 * x2; const double x6 = x4 * x2;
    double r = 15.5 * x6;
    r        = r - 40.14 * (x4 * x);
    r        = r + 31.96 * x4;
    r        = r - 6.868 * (x2 * x);
    r        = r + 0.4298 * x2;
    r        = r + 0.1191 * x;
    return r - 0.00232;
}
void ragx(const double col[3], double out[3])
{
    const double c0[3] = {0.842479062253094, 0.0784335999999992, 0.0792237451477643};
    const double c1[3] = {0.0423282422610123, 0.878468636469772, 0.0791661274605434};
    const double c2[3] = {0.0423756549057051, 0.0784336, 0.879142973793104};
    for (int r = 0; r < 3; ++r)
    {
        const double v   = c0[r] * col[0] + c1[r] * col[1] + c2[r] * col[2];
        const double vp  = v > 1.0e-10 ? v : 1.0e-10;
        double       lg  = crd::math::log2(vp);
        const double m0v = lg > -12.47393 ? lg : -12.47393;
        lg               = m0v < 4.026069 ? m0v : 4.026069;
        out[r]           = ragx_contrast((lg - (-12.47393)) / (4.026069 - (-12.47393)));
    }
}
double rmin2(double a, double b) { return a < b ? a : b; }
void rpbr(const double col[3], double out[3])
{
    const double x      = rmin2(col[0], rmin2(col[1], col[2]));
    const double offset = x < 0.08 ? x - 6.25 * (x * x) : 0.04;
    double       c1[3]; for (int k = 0; k < 3; ++k) { c1[k] = col[k] - offset; }
    const double peak    = rmax2(c1[0], rmax2(c1[1], c1[2]));
    const double start_c = 0.8 - 0.04; const double dd = 1.0 - start_c;
    const double npk     = 1.0 - (dd * dd) / ((peak + dd) - start_c);
    const double gg      = 1.0 - 1.0 / (0.15 * (peak - npk) + 1.0);
    for (int k = 0; k < 3; ++k) { const double sc = c1[k] * (npk / peak); const double cmp = sc * (1.0 - gg) + npk * gg; out[k] = peak < start_c ? c1[k] : cmp; }
}
double rsrgb(double x) { return x <= 0.0031308 ? 12.92 * x : 1.055 * crd::math::pow(x, 1.0 / 2.4) - 0.055; }
double rpq(double l)
{
    const double lp = crd::math::pow(l, 0.1593017578125);
    return crd::math::pow((0.8359375 + 18.8515625 * lp) / (1.0 + 18.6875 * lp), 78.84375);
}
void rgamut(const double col[3], double amount, double out[3])
{
    const double luma = 0.2126 * col[0] + 0.7152 * col[1] + 0.0722 * col[2];
    const double peak = rmax2(col[0], rmax2(col[1], col[2]));
    const double over = rclamp01((peak - 1.0) * amount);
    for (int k = 0; k < 3; ++k) { out[k] = col[k] * (1.0 - over) + luma * over; }
}
} // namespace

TEST_CASE("B13-c: HDR exposure + tonemap (AgX / PBR-Neutral) + output encode (sRGB / PQ) + gamut compress bit-exact", "[kir][lighting][hdr]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int lum = g.input(sh, kir::DType::F64); const int ev = g.input(sh, kir::DType::F64);
    const int cr = g.input(sh, kir::DType::F64); const int cg = g.input(sh, kir::DType::F64); const int cb = g.input(sh, kir::DType::F64);
    const int lv = g.input(sh, kir::DType::F64); const int amt = g.input(sh, kir::DType::F64);
    double lumv[kN]; double evv[kN]; double crv[kN]; double cgv[kN]; double cbv[kN]; double lvv[kN]; double amtv[kN];
    for (int i = 0; i < kN; ++i)
    {
        lumv[i] = 0.1 + 0.5 * i; evv[i] = -2.0 + 0.3 * i; crv[i] = 0.05 + 0.12 * i; cgv[i] = 0.1 + 0.09 * i; cbv[i] = 0.2 + 0.06 * i; lvv[i] = 0.02 + 0.048 * i; amtv[i] = 0.3 + 0.03 * i;
    }
    const double* inp[] = {lumv, evv, crv, cgv, cbv, lvv, amtv};

    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chk3 = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };
    const int  col = g.vec3(cr, cg, cb);

    chk(pst::ev100_from_luminance(g, lum), [&](int i) { return rev100(lumv[i]); });
    chk(pst::exposure_from_ev100(g, ev), [&](int i) { return rexposure(evv[i]); });
    chk3(pst::agx(g, col), [&](int i, int c) { const double cc[3] = {crv[i], cgv[i], cbv[i]}; double o[3]; ragx(cc, o); return o[c]; });
    chk3(pst::pbr_neutral(g, col), [&](int i, int c) { const double cc[3] = {crv[i], cgv[i], cbv[i]}; double o[3]; rpbr(cc, o); return o[c]; });
    chk3(pst::srgb_encode(g, col), [&](int i, int c) { const double cc[3] = {crv[i], cgv[i], cbv[i]}; return rsrgb(cc[c]); });
    chk(pst::pq_encode(g, lv), [&](int i) { return rpq(lvv[i]); });
    chk3(pst::gamut_compress(g, col, amt), [&](int i, int c) { const double cc[3] = {crv[i], cgv[i], cbv[i]}; double o[3]; rgamut(cc, amtv[i], o); return o[c]; });

    CHECK(bad == 0);
}

namespace
{
// B13-a TAA references — exact op grouping of ckir_taa.hpp against the CPU oracle (Mix = a(1−t)+bt, Step(edge,v)=v<edge?0:1,
// Fract = x−floor(x), Abs = x<0?−x:x, Clamp = min(max(a,b),c)).
double rabs(double x) { return x < 0.0 ? -x : x; } // rmix/rstep/rfract already defined above (identical) — reused
void rycocg(const double rgb[3], double out[3])
{
    out[0] = rgb[0] * 0.25 + rgb[1] * 0.5 + rgb[2] * 0.25;         // Y  (dot a0·b0+a1·b1+a2·b2)
    out[1] = rgb[0] * 0.5 + rgb[1] * 0.0 + rgb[2] * (-0.5);        // Co
    out[2] = rgb[0] * (-0.25) + rgb[1] * 0.5 + rgb[2] * (-0.25);   // Cg
}
void ryc2rgb(const double y[3], double out[3])
{
    out[0] = (y[0] + y[1]) - y[2]; // Y+Co−Cg
    out[1] = y[0] + y[2];          // Y+Cg
    out[2] = (y[0] - y[1]) - y[2]; // Y−Co−Cg
}
void rclip(const double hist[3], const double amin[3], const double amax[3], double out[3])
{
    double centre[3]; double extent[3]; double v[3]; double ratio[3];
    for (int k = 0; k < 3; ++k)
    {
        centre[k] = (amax[k] + amin[k]) * 0.5;
        extent[k] = (amax[k] - amin[k]) * 0.5;
        v[k]      = hist[k] - centre[k];
        ratio[k]  = rabs(v[k]) / (extent[k] + 1.0e-7);
    }
    const double maxr  = rmax2(ratio[0], rmax2(ratio[1], ratio[2]));
    const double denom = rmax2(maxr, 1.0);
    for (int k = 0; k < 3; ++k) { out[k] = centre[k] + v[k] / denom; }
}
void rvarclip(const double hist[3], const double m1[3], const double m2[3], double gamma, double out[3])
{
    double amin[3]; double amax[3];
    for (int k = 0; k < 3; ++k)
    {
        const double var  = rmax2(m2[k] - m1[k] * m1[k], 0.0);
        const double gsig = crd::math::sqrt(var) * gamma;
        amin[k]           = m1[k] - gsig;
        amax[k]           = m1[k] + gsig;
    }
    rclip(hist, amin, amax, out);
}
void rcatmull(double t, double out[4])
{
    const double t2 = t * t; const double t3 = t2 * t;
    out[0] = ((-0.5 * t) - (0.5 * t3)) + t2;
    out[1] = (1.0 - (2.5 * t2)) + (1.5 * t3);
    out[2] = ((0.5 * t) + (2.0 * t2)) - (1.5 * t3);
    out[3] = ((-0.5) * t2) + (0.5 * t3);
}
double rlumafb(double hl, double cl, double mn, double mx)
{
    const double diff = rabs(hl - cl);
    const double den  = rmax2(rmax2(hl, cl), 1.0e-5);
    const double fac  = rclamp01(diff / den);
    return mn + (mx - mn) * fac;
}
double rdisoc(double pd, double cd, double thr) { return rclamp01(1.0 - rabs(pd - cd) / thr); }
double rign(double fragx, double fragy, double frame)
{
    const double fx = fragx + 5.588238 * frame;
    const double d  = fx * 0.06711056 + fragy * 0.00583715;
    return rfract(52.9829189 * rfract(d));
}
} // namespace

TEST_CASE("B13-a: temporal AA resolve (YCoCg / AABB+variance clip / Catmull-Rom / luma-feedback / disocclusion / dither / frame-gen / SMAA) bit-exact", "[kir][lighting][taa]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int xr = g.input(sh, kir::DType::F64); const int xg = g.input(sh, kir::DType::F64); const int xb = g.input(sh, kir::DType::F64);
    const int hr = g.input(sh, kir::DType::F64); const int hg = g.input(sh, kir::DType::F64); const int hb = g.input(sh, kir::DType::F64);
    const int ma = g.input(sh, kir::DType::F64); const int mb = g.input(sh, kir::DType::F64); const int mc = g.input(sh, kir::DType::F64);
    const int qa = g.input(sh, kir::DType::F64); const int qb = g.input(sh, kir::DType::F64); const int qc = g.input(sh, kir::DType::F64);
    const int tt = g.input(sh, kir::DType::F64); const int al = g.input(sh, kir::DType::F64);
    const int hl = g.input(sh, kir::DType::F64); const int cl = g.input(sh, kir::DType::F64);
    const int pd = g.input(sh, kir::DType::F64); const int cd = g.input(sh, kir::DType::F64);
    const int fx = g.input(sh, kir::DType::F64); const int fy = g.input(sh, kir::DType::F64); const int fr = g.input(sh, kir::DType::F64);
    const int ns = g.input(sh, kir::DType::F64);
    const int lc = g.input(sh, kir::DType::F64); const int ll = g.input(sh, kir::DType::F64); const int lp = g.input(sh, kir::DType::F64);

    double xrv[kN]; double xgv[kN]; double xbv[kN]; double hrv[kN]; double hgv[kN]; double hbv[kN];
    double mav[kN]; double mbv[kN]; double mcv[kN]; double qav[kN]; double qbv[kN]; double qcv[kN];
    double ttv[kN]; double alv[kN]; double hlv[kN]; double clv[kN]; double pdv[kN]; double cdv[kN];
    double fxv[kN]; double fyv[kN]; double frv[kN]; double nsv[kN]; double lcv[kN]; double llv[kN]; double lpv[kN];
    for (int i = 0; i < kN; ++i)
    {
        xrv[i] = 0.05 + 0.11 * i; xgv[i] = 0.12 + 0.07 * i; xbv[i] = 0.2 + 0.05 * i;
        hrv[i] = 0.08 + 0.09 * i; hgv[i] = 0.15 + 0.06 * i; hbv[i] = 0.1 + 0.08 * i;
        mav[i] = 0.2 + 0.04 * i; mbv[i] = 0.25 + 0.03 * i; mcv[i] = 0.18 + 0.05 * i;
        qav[i] = 0.1 + 0.05 * i; qbv[i] = 0.12 + 0.04 * i; qcv[i] = 0.09 + 0.06 * i;
        ttv[i] = 0.02 + 0.045 * i; alv[i] = 0.03 + 0.02 * i; hlv[i] = 0.1 + 0.08 * i; clv[i] = 0.2 + 0.05 * i;
        pdv[i] = 0.3 + 0.02 * i; cdv[i] = 0.31 + 0.019 * i;
        fxv[i] = 1.0 + 3.0 * i; fyv[i] = 2.0 + 2.5 * i; frv[i] = static_cast<double>(i % 8);
        nsv[i] = 0.02 + 0.045 * i; lcv[i] = 0.3 + 0.03 * i; llv[i] = 0.28 + 0.031 * i; lpv[i] = 0.34 + 0.028 * i;
    }
    const double* inp[] = {xrv, xgv, xbv, hrv, hgv, hbv, mav, mbv, mcv, qav, qbv, qcv, ttv, alv, hlv, clv, pdv, cdv, fxv, fyv, frv, nsv, lcv, llv, lpv};

    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chkv = [&](int node, int comps, auto ref) { double o[kN * 4]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < comps; ++c) { if (o[i * comps + c] != ref(i, c)) { ++bad; } } } };

    const int cur  = g.vec3(xr, xg, xb); const int his = g.vec3(hr, hg, hb);
    const int mom1 = g.vec3(ma, mb, mc); const int mom2 = g.vec3(qa, qb, qc);
    const int frag = g.vec2(fx, fy);

    chkv(taa::rgb_to_ycocg(g, cur), 3, [&](int i, int c) { const double rgb[3] = {xrv[i], xgv[i], xbv[i]}; double o[3]; rycocg(rgb, o); return o[c]; });
    chkv(taa::ycocg_to_rgb(g, cur), 3, [&](int i, int c) { const double y[3] = {xrv[i], xgv[i], xbv[i]}; double o[3]; ryc2rgb(y, o); return o[c]; });
    chkv(taa::clip_aabb(g, his, mom1, mom2), 3, [&](int i, int c) { const double h[3] = {hrv[i], hgv[i], hbv[i]}; const double lo[3] = {mav[i], mbv[i], mcv[i]}; const double hi[3] = {qav[i], qbv[i], qcv[i]}; double o[3]; rclip(h, lo, hi, o); return o[c]; });
    chkv(taa::variance_clip(g, his, mom1, mom2, 1.25), 3, [&](int i, int c) { const double h[3] = {hrv[i], hgv[i], hbv[i]}; const double m1[3] = {mav[i], mbv[i], mcv[i]}; const double m2[3] = {qav[i], qbv[i], qcv[i]}; double o[3]; rvarclip(h, m1, m2, 1.25, o); return o[c]; });
    chkv(taa::catmull_rom_weights(g, tt), 4, [&](int i, int c) { double o[4]; rcatmull(ttv[i], o); return o[c]; });
    chk(taa::luma_feedback(g, hl, cl, 0.05, 0.85), [&](int i) { return rlumafb(hlv[i], clv[i], 0.05, 0.85); });
    chkv(taa::taa_resolve(g, his, cur, al), 3, [&](int i, int c) { const double h[3] = {hrv[i], hgv[i], hbv[i]}; const double cu[3] = {xrv[i], xgv[i], xbv[i]}; return rmix(h[c], cu[c], alv[i]); });
    chk(taa::disocclusion(g, pd, cd, 0.1), [&](int i) { return rdisoc(pdv[i], cdv[i], 0.1); });
    chk(taa::ign_temporal(g, frag, fr), [&](int i) { return rign(fxv[i], fyv[i], frv[i]); });
    chkv(taa::dither_apply(g, cur, ns, 255.0), 3, [&](int i, int c) { const double cu[3] = {xrv[i], xgv[i], xbv[i]}; const double amt = (nsv[i] - 0.5) / 255.0; return cu[c] + amt; });
    chkv(taa::frame_gen_blend(g, his, cur, tt), 3, [&](int i, int c) { const double h[3] = {hrv[i], hgv[i], hbv[i]}; const double cu[3] = {xrv[i], xgv[i], xbv[i]}; return rmix(h[c], cu[c], ttv[i]); });
    chkv(taa::smaa_luma_edge(g, lc, ll, lp, 0.1), 2, [&](int i, int c) { return c == 0 ? rstep(0.1, rabs(lcv[i] - llv[i])) : rstep(0.1, rabs(lcv[i] - lpv[i])); });

    CHECK(bad == 0);
}

namespace
{
// B13-b BLOOM references — exact op grouping of ckir_bloom.hpp (dot order a0·b0+a1·b1+a2·b2; Smoothstep = rsmooth; Mix/Clamp
// as the oracle). Taps are constructed from three base channels: tap[j][c] = base_c·(1+0.07j) + (0.03j+0.02c).
double bl_luma(const double c[3]) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
double bl_kw(const double c[3]) { return 1.0 / (1.0 + bl_luma(c)); }
void bl_q(const double a[3], const double b[3], const double c[3], const double d[3], double o[3])
{
    for (int k = 0; k < 3; ++k) { o[k] = ((a[k] + b[k]) + (c[k] + d[k])) * 0.25; }
}
void bl_down13(const double t[13][3], double o[3])
{
    double inner[3]; double tl[3]; double tr[3]; double bl[3]; double br[3];
    bl_q(t[9], t[10], t[11], t[12], inner); bl_q(t[0], t[1], t[3], t[4], tl); bl_q(t[1], t[2], t[4], t[5], tr);
    bl_q(t[3], t[4], t[6], t[7], bl); bl_q(t[4], t[5], t[7], t[8], br);
    for (int k = 0; k < 3; ++k) { const double outer = ((tl[k] + tr[k]) + (bl[k] + br[k])); o[k] = inner[k] * 0.5 + outer * 0.125; }
}
void bl_downk(const double t[13][3], double o[3])
{
    double b[5][3];
    bl_q(t[9], t[10], t[11], t[12], b[0]); bl_q(t[0], t[1], t[3], t[4], b[1]); bl_q(t[1], t[2], t[4], t[5], b[2]);
    bl_q(t[3], t[4], t[6], t[7], b[3]); bl_q(t[4], t[5], t[7], t[8], b[4]);
    const double w[5] = {bl_kw(b[0]) * 0.5, bl_kw(b[1]) * 0.125, bl_kw(b[2]) * 0.125, bl_kw(b[3]) * 0.125, bl_kw(b[4]) * 0.125};
    const double den  = ((w[0] + w[1]) + (w[2] + w[3])) + w[4];
    for (int k = 0; k < 3; ++k) { const double num = ((b[0][k] * w[0] + b[1][k] * w[1]) + (b[2][k] * w[2] + b[3][k] * w[3])) + b[4][k] * w[4]; o[k] = num / (den + 1.0e-5); }
}
void bl_softknee(const double c[3], double thr, double knee, double o[3])
{
    const double brr  = rmax2(c[0], rmax2(c[1], c[2]));
    const double soft = rclamp((brr + knee) - thr, 0.0, 2.0 * knee);
    const double sq   = (soft * soft) / (4.0 * knee + 1.0e-5);
    const double con  = rmax2(sq, brr - thr) / rmax2(brr, 1.0e-5);
    for (int k = 0; k < 3; ++k) { o[k] = c[k] * con; }
}
void bl_tent(const double t[9][3], double o[3])
{
    for (int k = 0; k < 3; ++k)
    {
        const double corners = ((t[0][k] + t[2][k]) + (t[6][k] + t[8][k]));
        const double edges   = ((t[1][k] * 2.0 + t[3][k] * 2.0) + (t[5][k] * 2.0 + t[7][k] * 2.0));
        const double centre  = t[4][k] * 4.0;
        o[k]                 = ((corners + edges) + centre) * (1.0 / 16.0);
    }
}
double bl_halo(double ux, double uy, double cx, double cy, double radius, double thick)
{
    const double dx = ux - cx; const double dy = uy - cy;
    const double r  = crd::math::sqrt(dx * dx + dy * dy);
    return 1.0 - rsmooth(0.0, thick, rabs(r - radius));
}
double bl_star(double ang, double blades, double sharp) { return crd::math::pow(0.5 + 0.5 * crd::math::cos(blades * ang), sharp); }
void bl_tint(double t, double o[3]) { o[0] = rclamp01(1.0 - t); o[1] = rclamp01(4.0 * (t * (1.0 - t))); o[2] = rclamp01(t); }
} // namespace

TEST_CASE("B13-b: bloom (Karis 13-tap + firefly downsample / soft-knee / tent upsample / composite / FFT complex-mul / lens halo+starburst) bit-exact", "[kir][lighting][bloom]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int br = g.input(sh, kir::DType::F64); const int bg = g.input(sh, kir::DType::F64); const int bb = g.input(sh, kir::DType::F64);
    const int are = g.input(sh, kir::DType::F64); const int aim = g.input(sh, kir::DType::F64); const int bre = g.input(sh, kir::DType::F64); const int bim = g.input(sh, kir::DType::F64);
    const int ang = g.input(sh, kir::DType::F64);
    const int ux = g.input(sh, kir::DType::F64); const int uy = g.input(sh, kir::DType::F64); const int cx = g.input(sh, kir::DType::F64); const int cy = g.input(sh, kir::DType::F64);
    const int tp = g.input(sh, kir::DType::F64); const int inten = g.input(sh, kir::DType::F64);
    const int sr = g.input(sh, kir::DType::F64); const int sg = g.input(sh, kir::DType::F64); const int sb = g.input(sh, kir::DType::F64);

    double brv[kN]; double bgv[kN]; double bbv[kN]; double arev[kN]; double aimv[kN]; double brev[kN]; double bimv[kN]; double angv[kN];
    double uxv[kN]; double uyv[kN]; double cxv[kN]; double cyv[kN]; double tpv[kN]; double intv[kN]; double srv[kN]; double sgv[kN]; double sbv[kN];
    for (int i = 0; i < kN; ++i)
    {
        brv[i] = 0.1 + 0.13 * i; bgv[i] = 0.15 + 0.09 * i; bbv[i] = 0.2 + 0.07 * i;
        arev[i] = 0.2 + 0.05 * i; aimv[i] = -0.3 + 0.04 * i; brev[i] = 0.1 + 0.06 * i; bimv[i] = 0.25 - 0.02 * i;
        angv[i] = 0.1 + 0.28 * i; uxv[i] = 0.05 * i; uyv[i] = 0.03 * i; cxv[i] = 0.5; cyv[i] = 0.5;
        tpv[i] = 0.02 + 0.045 * i; intv[i] = 0.03 + 0.02 * i; srv[i] = 0.3 + 0.02 * i; sgv[i] = 0.25 + 0.03 * i; sbv[i] = 0.2 + 0.025 * i;
    }
    const double* inp[] = {brv, bgv, bbv, arev, aimv, brev, bimv, angv, uxv, uyv, cxv, cyv, tpv, intv, srv, sgv, sbv};

    // tap[j] = vec3(base_c·(1+0.07j) + (0.03j+0.02c)); reference reads the same value.
    const int  bcn[3]   = {br, bg, bb};
    const auto tap_node = [&](int j) { const auto ch = [&](int c) { return g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, bcn[c], g.constant(1.0 + 0.07 * j, sh, kir::DType::F64)), g.constant(0.03 * j + 0.02 * c, sh, kir::DType::F64)); }; return g.vec3(ch(0), ch(1), ch(2)); };
    const auto tap_val  = [&](int i, int j, int c) { const double* bv[3] = {brv, bgv, bbv}; return bv[c][i] * (1.0 + 0.07 * j) + (0.03 * j + 0.02 * c); };

    int taps13[13]; for (int j = 0; j < 13; ++j) { taps13[j] = tap_node(j); }
    int taps9[9];   for (int j = 0; j < 9; ++j) { taps9[j] = taps13[j]; }

    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chkv = [&](int node, int comps, auto ref) { double o[kN * 4]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < comps; ++c) { if (o[i * comps + c] != ref(i, c)) { ++bad; } } } };
    const auto fill_t = [&](int i, double t[13][3]) { for (int j = 0; j < 13; ++j) { for (int c = 0; c < 3; ++c) { t[j][c] = tap_val(i, j, c); } } };

    chkv(blm::downsample_13tap(g, taps13), 3, [&](int i, int c) { double t[13][3]; fill_t(i, t); double o[3]; bl_down13(t, o); return o[c]; });
    chkv(blm::downsample_karis(g, taps13), 3, [&](int i, int c) { double t[13][3]; fill_t(i, t); double o[3]; bl_downk(t, o); return o[c]; });
    chkv(blm::soft_knee(g, taps13[0], 0.8, 0.5), 3, [&](int i, int c) { double t[13][3]; fill_t(i, t); double o[3]; bl_softknee(t[0], 0.8, 0.5, o); return o[c]; });
    chkv(blm::upsample_tent(g, taps9), 3, [&](int i, int c) { double t[13][3]; fill_t(i, t); double t9[9][3]; for (int j = 0; j < 9; ++j) { for (int k = 0; k < 3; ++k) { t9[j][k] = t[j][k]; } } double o[3]; bl_tent(t9, o); return o[c]; });
    chkv(blm::combine(g, g.vec3(sr, sg, sb), taps13[1], inten), 3, [&](int i, int c) { const double sc[3] = {srv[i], sgv[i], sbv[i]}; const double bcol = tap_val(i, 1, c); return rmix(sc[c], bcol, intv[i]); });
    chkv(blm::complex_mul(g, are, aim, bre, bim), 2, [&](int i, int c) { return c == 0 ? (arev[i] * brev[i] - aimv[i] * bimv[i]) : (arev[i] * bimv[i] + aimv[i] * brev[i]); });
    chk(blm::lens_halo(g, g.vec2(ux, uy), g.vec2(cx, cy), 0.3, 0.15), [&](int i) { return bl_halo(uxv[i], uyv[i], cxv[i], cyv[i], 0.3, 0.15); });
    chk(blm::starburst(g, ang, 6.0, 2.5), [&](int i) { return bl_star(angv[i], 6.0, 2.5); });
    chkv(blm::spectral_tint(g, tp), 3, [&](int i, int c) { double o[3]; bl_tint(tpv[i], o); return o[c]; });

    CHECK(bad == 0);
}

namespace
{
// B13-d CINEMATIC references — exact op grouping of ckir_cinematic.hpp (Exp/Cos/Sin/Sqrt as the oracle, Smoothstep = rsmooth,
// Mix = a(1−t)+bt, Clamp = min(max(a,b),c)).
double ci_coc(double depth, double focus, double f, double n) { return (f * f / n) * (depth - focus) / (depth * (focus - f)); }
void ci_cgauss(double r2, double a, double b, double o[2])
{
    const double env = crd::math::exp(a * r2);
    o[0]             = env * crd::math::cos(b * r2);
    o[1]             = env * crd::math::sin(b * r2);
}
double ci_bokeh(double re, double im, double cre, double cim) { return re * cre - im * cim; }
double ci_cov(double tap_coc, double dist) { return rclamp01((tap_coc - dist) + 0.5); }
void ci_dofcomp(const double sh[3], const double bl[3], double coc, double maxc, double o[3])
{
    const double t = rclamp01(rabs(coc) / maxc);
    for (int k = 0; k < 3; ++k) { o[k] = rmix(sh[k], bl[k], t); }
}
void ci_velscale(double vx, double vy, double shutter, double maxl, double o[2])
{
    const double v0  = vx * shutter; const double v1 = vy * shutter;
    const double len = crd::math::sqrt(v0 * v0 + v1 * v1) + 1.0e-6;
    const double s   = rmin2(1.0, maxl / len);
    o[0]             = v0 * s; o[1] = v1 * s;
}
double ci_cone(double dist, double vlen) { return rclamp01(1.0 - dist / (vlen + 1.0e-6)); }
double ci_cyl(double dist, double vlen) { return 1.0 - rsmooth(0.95 * vlen, 1.05 * vlen, dist); }
double ci_soft(double za, double zb, double ext) { return rclamp01(1.0 - (za - zb) / ext); }
} // namespace

TEST_CASE("B13-d: cinematic (thin-lens CoC / Garcia complex-Gaussian phasor / bokeh realize / near-far composite / velocity scale / McGuire cone-cylinder-soft-depth) bit-exact", "[kir][lighting][cine]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int dep = g.input(sh, kir::DType::F64); const int foc = g.input(sh, kir::DType::F64);
    const int r2 = g.input(sh, kir::DType::F64); const int re = g.input(sh, kir::DType::F64); const int im = g.input(sh, kir::DType::F64);
    const int tapc = g.input(sh, kir::DType::F64); const int dst = g.input(sh, kir::DType::F64);
    const int shr = g.input(sh, kir::DType::F64); const int shg = g.input(sh, kir::DType::F64); const int shb = g.input(sh, kir::DType::F64);
    const int blr = g.input(sh, kir::DType::F64); const int blg = g.input(sh, kir::DType::F64); const int blb = g.input(sh, kir::DType::F64);
    const int coc = g.input(sh, kir::DType::F64);
    const int vx = g.input(sh, kir::DType::F64); const int vy = g.input(sh, kir::DType::F64);
    const int mds = g.input(sh, kir::DType::F64); const int mvl = g.input(sh, kir::DType::F64);
    const int za = g.input(sh, kir::DType::F64); const int zb = g.input(sh, kir::DType::F64);

    double depv[kN]; double focv[kN]; double r2v[kN]; double rev[kN]; double imv[kN]; double tcv[kN]; double dsv[kN];
    double shrv[kN]; double shgv[kN]; double shbv[kN]; double blrv[kN]; double blgv[kN]; double blbv[kN]; double cocv[kN];
    double vxv[kN]; double vyv[kN]; double mdsv[kN]; double mvlv[kN]; double zav[kN]; double zbv[kN];
    for (int i = 0; i < kN; ++i)
    {
        depv[i] = 2.0 + 0.5 * i; focv[i] = 4.0 + 0.1 * i; r2v[i] = 0.01 + 0.05 * i; rev[i] = 0.2 + 0.06 * i; imv[i] = -0.3 + 0.04 * i;
        tcv[i] = 0.5 + 0.4 * i; dsv[i] = 0.2 * i;
        shrv[i] = 0.2 + 0.03 * i; shgv[i] = 0.15 + 0.04 * i; shbv[i] = 0.1 + 0.035 * i; blrv[i] = 0.3 + 0.02 * i; blgv[i] = 0.25 + 0.03 * i; blbv[i] = 0.2 + 0.025 * i;
        cocv[i] = -5.0 + 0.6 * i; vxv[i] = -4.0 + 0.5 * i; vyv[i] = 2.0 + 0.3 * i; mdsv[i] = 0.3 * i; mvlv[i] = 1.0 + 0.5 * i; zav[i] = 0.2 + 0.02 * i; zbv[i] = 0.25 + 0.018 * i;
    }
    const double* inp[] = {depv, focv, r2v, rev, imv, tcv, dsv, shrv, shgv, shbv, blrv, blgv, blbv, cocv, vxv, vyv, mdsv, mvlv, zav, zbv};

    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chkv = [&](int node, int comps, auto ref) { double o[kN * 4]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < comps; ++c) { if (o[i * comps + c] != ref(i, c)) { ++bad; } } } };

    chk(ci::circle_of_confusion(g, dep, foc, 0.05, 2.8), [&](int i) { return ci_coc(depv[i], focv[i], 0.05, 2.8); });
    chkv(ci::complex_gaussian(g, r2, -4.0, 1.0), 2, [&](int i, int c) { double o[2]; ci_cgauss(r2v[i], -4.0, 1.0, o); return o[c]; });
    chk(ci::bokeh_realize(g, re, im, 0.5, -0.3), [&](int i) { return ci_bokeh(rev[i], imv[i], 0.5, -0.3); });
    chk(ci::coc_coverage(g, tapc, dst), [&](int i) { return ci_cov(tcv[i], dsv[i]); });
    chkv(ci::dof_composite(g, g.vec3(shr, shg, shb), g.vec3(blr, blg, blb), coc, 10.0), 3, [&](int i, int c) { const double s3[3] = {shrv[i], shgv[i], shbv[i]}; const double b3[3] = {blrv[i], blgv[i], blbv[i]}; double o[3]; ci_dofcomp(s3, b3, cocv[i], 10.0, o); return o[c]; });
    chkv(ci::velocity_scale(g, g.vec2(vx, vy), 0.5, 16.0), 2, [&](int i, int c) { double o[2]; ci_velscale(vxv[i], vyv[i], 0.5, 16.0, o); return o[c]; });
    chk(ci::mb_cone(g, mds, mvl), [&](int i) { return ci_cone(mdsv[i], mvlv[i]); });
    chk(ci::mb_cylinder(g, mds, mvl), [&](int i) { return ci_cyl(mdsv[i], mvlv[i]); });
    chk(ci::mb_soft_depth(g, za, zb, 0.05), [&](int i) { return ci_soft(zav[i], zbv[i], 0.05); });

    CHECK(bad == 0);
}

namespace
{
// B13-e FINISH references — exact op grouping of ckir_finish.hpp (Sqrt/Min/Max/Div as the oracle; Clamp = min(max(a,b),c)).
double fi_specaa(double alpha, const double dx[3], const double dy[3], double kappa, double sig2)
{
    const double var = kappa * ((dx[0] * dx[0] + dx[1] * dx[1] + dx[2] * dx[2]) + (dy[0] * dy[0] + dy[1] * dy[1] + dy[2] * dy[2]));
    const double ker = rmin2(2.0 * var, sig2);
    return rclamp01(crd::math::sqrt(alpha * alpha + ker));
}
void fi_ca(double ux, double uy, double cx, double cy, double strength, double o[2])
{
    const double dx = ux - cx; const double dy = uy - cy; const double r2 = dx * dx + dy * dy; const double s = strength * r2;
    o[0] = dx * s; o[1] = dy * s;
}
double fi_vig(double ux, double uy, double cx, double cy, double invf2)
{
    const double dx = ux - cx; const double dy = uy - cy; const double att = 1.0 / (1.0 + (dx * dx + dy * dy) * invf2);
    return att * att;
}
void fi_grain(const double c[3], double noise, double intensity, double o[3])
{
    const double lum  = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    const double resp = lum * (1.0 - lum);
    const double amt  = ((noise * 2.0 - 1.0) * intensity) * resp;
    for (int k = 0; k < 3; ++k) { o[k] = c[k] + amt; }
}
void fi_cas(const double c[3], const double u[3], const double d[3], const double l[3], const double r[3], double sharp, double o[3])
{
    const double neg = -(0.125 + 0.075 * sharp);
    for (int k = 0; k < 3; ++k)
    {
        const double mn  = rmin2(rmin2(rmin2(u[k], d[k]), rmin2(l[k], r[k])), c[k]);
        const double mx  = rmax2(rmax2(rmax2(u[k], d[k]), rmax2(l[k], r[k])), c[k]);
        const double amp = crd::math::sqrt(rclamp01(rmin2(mn, 1.0 - mx) / (mx + 1.0e-5)));
        const double w   = amp * neg;
        const double num = c[k] + ((u[k] + d[k]) + (l[k] + r[k])) * w;
        o[k]             = num / (w * 4.0 + 1.0);
    }
}
} // namespace

TEST_CASE("B13-e: finish (Tokuyoshi geometric specular AA / chromatic aberration / cos4 vignette / Lottes grain / AMD CAS sharpen) bit-exact", "[kir][lighting][finish]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int al = g.input(sh, kir::DType::F64);
    const int dx0 = g.input(sh, kir::DType::F64); const int dx1 = g.input(sh, kir::DType::F64); const int dx2 = g.input(sh, kir::DType::F64);
    const int dy0 = g.input(sh, kir::DType::F64); const int dy1 = g.input(sh, kir::DType::F64); const int dy2 = g.input(sh, kir::DType::F64);
    const int ux = g.input(sh, kir::DType::F64); const int uy = g.input(sh, kir::DType::F64); const int cxn = g.input(sh, kir::DType::F64); const int cyn = g.input(sh, kir::DType::F64);
    const int gr = g.input(sh, kir::DType::F64); const int gg = g.input(sh, kir::DType::F64); const int gb = g.input(sh, kir::DType::F64); const int gn = g.input(sh, kir::DType::F64);
    const int kr = g.input(sh, kir::DType::F64); const int kg = g.input(sh, kir::DType::F64); const int kb = g.input(sh, kir::DType::F64);

    double alv[kN]; double dx0v[kN]; double dx1v[kN]; double dx2v[kN]; double dy0v[kN]; double dy1v[kN]; double dy2v[kN];
    double uxv[kN]; double uyv[kN]; double cxv[kN]; double cyv[kN]; double grv[kN]; double ggv[kN]; double gbv[kN]; double gnv[kN];
    double krv[kN]; double kgv[kN]; double kbv[kN];
    for (int i = 0; i < kN; ++i)
    {
        alv[i] = 0.05 + 0.04 * i; dx0v[i] = 0.01 + 0.02 * i; dx1v[i] = -0.02 + 0.015 * i; dx2v[i] = 0.03 * i;
        dy0v[i] = -0.01 + 0.018 * i; dy1v[i] = 0.02 + 0.01 * i; dy2v[i] = 0.005 * i;
        uxv[i] = 0.04 * i; uyv[i] = 0.03 * i; cxv[i] = 0.5; cyv[i] = 0.5;
        grv[i] = 0.1 + 0.04 * i; ggv[i] = 0.2 + 0.03 * i; gbv[i] = 0.15 + 0.035 * i; gnv[i] = 0.02 + 0.045 * i;
        krv[i] = 0.3 + 0.02 * i; kgv[i] = 0.25 + 0.025 * i; kbv[i] = 0.2 + 0.03 * i;
    }
    const double* inp[] = {alv, dx0v, dx1v, dx2v, dy0v, dy1v, dy2v, uxv, uyv, cxv, cyv, grv, ggv, gbv, gnv, krv, kgv, kbv};

    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chkv = [&](int node, int comps, auto ref) { double o[kN * 4]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < comps; ++c) { if (o[i * comps + c] != ref(i, c)) { ++bad; } } } };

    // CAS taps: tap_t[k] = kbase_k·(1+0.1t) + 0.05t, t = 0..4 (c,u,d,l,r).
    const int  kbn[3]  = {kr, kg, kb};
    const auto castap  = [&](int t) { const auto ch = [&](int c) { return g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, kbn[c], g.constant(1.0 + 0.1 * t, sh, kir::DType::F64)), g.constant(0.05 * t, sh, kir::DType::F64)); }; return g.vec3(ch(0), ch(1), ch(2)); };
    const auto casval  = [&](int i, int t, int c) { const double* kv[3] = {krv, kgv, kbv}; return kv[c][i] * (1.0 + 0.1 * t) + 0.05 * t; };
    const auto fillcas = [&](int i, int t, double o[3]) { for (int c = 0; c < 3; ++c) { o[c] = casval(i, t, c); } };

    chk(fin::specular_aa(g, al, g.vec3(dx0, dx1, dx2), g.vec3(dy0, dy1, dy2), 0.5, 0.18), [&](int i) { const double dx[3] = {dx0v[i], dx1v[i], dx2v[i]}; const double dy[3] = {dy0v[i], dy1v[i], dy2v[i]}; return fi_specaa(alv[i], dx, dy, 0.5, 0.18); });
    chkv(fin::ca_offset(g, g.vec2(ux, uy), g.vec2(cxn, cyn), 0.6), 2, [&](int i, int c) { double o[2]; fi_ca(uxv[i], uyv[i], cxv[i], cyv[i], 0.6, o); return o[c]; });
    chk(fin::vignette(g, g.vec2(ux, uy), g.vec2(cxn, cyn), 1.5), [&](int i) { return fi_vig(uxv[i], uyv[i], cxv[i], cyv[i], 1.5); });
    chkv(fin::film_grain(g, g.vec3(gr, gg, gb), gn, 0.08), 3, [&](int i, int c) { const double cc[3] = {grv[i], ggv[i], gbv[i]}; double o[3]; fi_grain(cc, gnv[i], 0.08, o); return o[c]; });
    chkv(fin::cas_sharpen(g, castap(0), castap(1), castap(2), castap(3), castap(4), 0.7), 3, [&](int i, int c) { double cc[3]; double u[3]; double d[3]; double l[3]; double r[3]; fillcas(i, 0, cc); fillcas(i, 1, u); fillcas(i, 2, d); fillcas(i, 3, l); fillcas(i, 4, r); double o[3]; fi_cas(cc, u, d, l, r, 0.7, o); return o[c]; });

    CHECK(bad == 0);
}

TEST_CASE("B12-b: screen-space reflections (reflect ray / Hi-Z hit / edge fade / confidence) bit-exact", "[kir][lighting][ssr]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int ix = g.input(sh, kir::DType::F64); const int iy = g.input(sh, kir::DType::F64); const int iz = g.input(sh, kir::DType::F64);
    const int nx = g.input(sh, kir::DType::F64); const int ny = g.input(sh, kir::DType::F64); const int nz = g.input(sh, kir::DType::F64);
    const int rz = g.input(sh, kir::DType::F64); const int sz = g.input(sh, kir::DType::F64);
    const int ux = g.input(sh, kir::DType::F64); const int uy = g.input(sh, kir::DType::F64); const int rgh = g.input(sh, kir::DType::F64); const int dst = g.input(sh, kir::DType::F64);
    double ixv[kN]; double iyv[kN]; double izv[kN]; double nxv[kN]; double nyv[kN]; double nzv[kN]; double rzv[kN]; double szv[kN]; double uxv[kN]; double uyv[kN]; double rgv[kN]; double dstv[kN];
    for (int i = 0; i < kN; ++i)
    {
        ixv[i] = 0.2 + 0.02 * i; iyv[i] = -0.3 + 0.01 * i; izv[i] = -1.0; nxv[i] = 0.0; nyv[i] = 0.1 + 0.02 * i; nzv[i] = 1.0;
        rzv[i] = 0.3 + 0.02 * i; szv[i] = 0.4; uxv[i] = 0.02 + 0.048 * i; uyv[i] = 0.05 + 0.045 * i; rgv[i] = 0.1 + 0.04 * i; dstv[i] = 0.05 + 0.03 * i;
    }
    const double* inp[] = {ixv, iyv, izv, nxv, nyv, nzv, rzv, szv, uxv, uyv, rgv, dstv};

    const auto kc  = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chk3 = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };

    chk3(scr::ssr_reflect(g, g.vec3(ix, iy, iz), g.vec3(nx, ny, nz)), [&](int i, int c) { const double iv[3] = {ixv[i], iyv[i], izv[i]}; const double nv[3] = {nxv[i], nyv[i], nzv[i]}; return rreflect(iv, nv, c); });
    chk(scr::ssr_hiz_hit(g, rz, sz, kc(0.5)), [&](int i) { return rssr_hit(rzv[i], szv[i], 0.5); });
    chk(scr::ssr_edge_fade(g, g.vec2(ux, uy), kc(0.1)), [&](int i) { return rssr_edge(uxv[i], uyv[i], 0.1); });
    const int ef = scr::ssr_edge_fade(g, g.vec2(ux, uy), kc(0.1));
    chk(scr::ssr_confidence(g, rgh, ef, dst), [&](int i) { return rssr_conf(rgv[i], rssr_edge(uxv[i], uyv[i], 0.1), dstv[i]); });

    CHECK(bad == 0);
}

TEST_CASE("B12-a: screen-space ambient occlusion (GTAO / multibounce / spec-occ / SSILVB bitmask) bit-exact", "[kir][lighting][ssao]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int h1 = g.input(sh, kir::DType::F64); const int h2 = g.input(sh, kir::DType::F64); const int gam = g.input(sh, kir::DType::F64); const int nlen = g.input(sh, kir::DType::F64);
    const int vis = g.input(sh, kir::DType::F64); const int nov = g.input(sh, kir::DType::F64); const int aoc = g.input(sh, kir::DType::F64); const int rgh = g.input(sh, kir::DType::F64);
    const int mn1 = g.input(sh, kir::DType::F64); const int mx1 = g.input(sh, kir::DType::F64); const int mn2 = g.input(sh, kir::DType::F64); const int mx2 = g.input(sh, kir::DType::F64);
    double h1v[kN]; double h2v[kN]; double gamv[kN]; double nlv[kN]; double visv[kN]; double novv[kN]; double aov[kN]; double rgv[kN]; double mn1v[kN]; double mx1v[kN]; double mn2v[kN]; double mx2v[kN];
    for (int i = 0; i < kN; ++i)
    {
        h1v[i] = -0.5 + 0.05 * i; h2v[i] = 0.3 + 0.04 * i; gamv[i] = 0.1 + 0.03 * i; nlv[i] = 0.8 + 0.005 * i;
        visv[i] = 0.3 + 0.03 * i; novv[i] = 0.5 + 0.02 * i; aov[i] = 0.4 + 0.02 * i; rgv[i] = 0.1 + 0.04 * i;
        mn1v[i] = 0.05 + 0.03 * i; mx1v[i] = 0.2 + 0.03 * i; mn2v[i] = 0.4 + 0.02 * i; mx2v[i] = 0.7 + 0.01 * i;
    }
    const double* inp[] = {h1v, h2v, gamv, nlv, visv, novv, aov, rgv, mn1v, mx1v, mn2v, mx2v};

    const auto kc  = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    const int  alb = g.vec3(kc(0.8), kc(0.4), kc(0.2));
    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chk3 = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };

    chk(scr::gtao_slice(g, h1, h2, gam, nlen), [&](int i) { return rgtao_slice(h1v[i], h2v[i], gamv[i], nlv[i]); });
    chk3(scr::gtao_multibounce(g, vis, alb), [&](int i, int c) { const double al[3] = {0.8, 0.4, 0.2}; double o[3]; rgtao_mb(visv[i], al, o); return o[c]; });
    chk(scr::spec_occlusion(g, nov, aoc, rgh), [&](int i) { return rspec_occ(novv[i], aov[i], rgv[i]); });
    chk(scr::ssilvb_sector_mask(g, mn1, mx1, 32.0), [&](int i) { return static_cast<double>(rssilvb_mask(mn1v[i], mx1v[i], 32.0)); });
    // AO from an accumulated 2-sample bitfield (OR the two sector masks).
    const int bf = g.binary(kir::KOp::BitOr, scr::ssilvb_sector_mask(g, mn1, mx1, 32.0), scr::ssilvb_sector_mask(g, mn2, mx2, 32.0));
    chk(scr::ssilvb_ao(g, bf, 32.0), [&](int i) { const crd::i64 m = rssilvb_mask(mn1v[i], mx1v[i], 32.0) | rssilvb_mask(mn2v[i], mx2v[i], 32.0); return rssilvb_ao(m, 32.0); });

    CHECK(bad == 0);
}

namespace
{
double rssgi(const double rad[3], crd::i64 sm, crd::i64 bf, double cos_n, double nsec, int c)
{
    const crd::i64 nb  = sm & (~bf);
    crd::u32       v   = static_cast<crd::u32>(nb);
    int            cnt = 0;
    while (v != 0U) { cnt += static_cast<int>(v & 1U); v >>= 1U; }
    return rad[c] * ((static_cast<double>(cnt) / nsec) * cos_n);
}
} // namespace

TEST_CASE("B12-c: screen-space GI (visibility-bitmask indirect diffuse bounce) bit-exact", "[kir][lighting][ssgi]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int mn1 = g.input(sh, kir::DType::F64); const int mx1 = g.input(sh, kir::DType::F64); const int mn2 = g.input(sh, kir::DType::F64); const int mx2 = g.input(sh, kir::DType::F64);
    const int rr = g.input(sh, kir::DType::F64); const int rg = g.input(sh, kir::DType::F64); const int rb = g.input(sh, kir::DType::F64); const int cosn = g.input(sh, kir::DType::F64);
    double mn1v[kN]; double mx1v[kN]; double mn2v[kN]; double mx2v[kN]; double rrv[kN]; double rgv[kN]; double rbv[kN]; double cnv[kN];
    for (int i = 0; i < kN; ++i)
    {
        mn1v[i] = 0.05 + 0.03 * i; mx1v[i] = 0.4 + 0.02 * i; mn2v[i] = 0.1 + 0.02 * i; mx2v[i] = 0.3 + 0.01 * i;
        rrv[i] = 0.6 + 0.02 * i; rgv[i] = 0.4; rbv[i] = 0.3; cnv[i] = 0.3 + 0.03 * i;
    }
    const double* inp[] = {mn1v, mx1v, mn2v, mx2v, rrv, rgv, rbv, cnv};

    int        bad = 0;
    const auto chk3 = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };

    const int sm  = scr::ssilvb_sector_mask(g, mn1, mx1, 32.0);
    const int bf  = scr::ssilvb_sector_mask(g, mn2, mx2, 32.0);
    const int rad = g.vec3(rr, rg, rb);
    chk3(scr::ssgi_bounce(g, rad, sm, bf, cosn, 32.0), [&](int i, int c) {
        const double     r[3] = {rrv[i], rgv[i], rbv[i]};
        const crd::i64   smi  = rssilvb_mask(mn1v[i], mx1v[i], 32.0);
        const crd::i64   bfi  = rssilvb_mask(mn2v[i], mx2v[i], 32.0);
        return rssgi(r, smi, bfi, cnv[i], 32.0, c); });

    CHECK(bad == 0);
}

namespace
{
constexpr double kPiVol = 3.14159265358979323846; // matches screen::vol::kPi (full precision, not the Filament-π used elsewhere)
double rhg(double ct, double ga) { const double g2 = ga * ga; const double d = (1.0 + g2) - (2.0 * ga) * ct; return (1.0 - g2) / ((4.0 * kPiVol) * crd::math::pow(d, 1.5)); }
double rcs(double ct, double ga) { const double g2 = ga * ga; const double c2 = ct * ct; const double num = ((3.0 / (8.0 * kPiVol)) * (1.0 - g2)) * (1.0 + c2); const double d = (1.0 + g2) - (2.0 * ga) * ct; return num / ((2.0 + g2) * crd::math::pow(d, 1.5)); }
double rdraine(double ct, double ga, double al) { const double g2 = ga * ga; const double c2 = ct * ct; const double num = (1.0 - g2) * (1.0 + al * c2); const double norm = 1.0 + (al * (1.0 + 2.0 * g2)) / 3.0; const double d = (1.0 + g2) - (2.0 * ga) * ct; return num / (((4.0 * kPiVol) * norm) * crd::math::pow(d, 1.5)); }
double rbeer(double sigma, double dist) { return crd::math::exp(-(sigma * dist)); }
double rburley(double r, double d) { const double rd = r / d; const double e1 = crd::math::exp(-rd); const double e2 = crd::math::exp(-(rd * (1.0 / 3.0))); return (e1 + e2) / ((d * (8.0 * kPiVol)) * r); }
double rgauss(double r, double var) { return crd::math::exp(-(r * r) / (2.0 * var)) / crd::math::sqrt((2.0 * kPiVol) * var); }
} // namespace

TEST_CASE("B12-d/e: volumetric phase family + Beer-Lambert + froxel scatter + Burley/Gaussian SSS bit-exact", "[kir][lighting][volsss]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int ct = g.input(sh, kir::DType::F64); const int ga = g.input(sh, kir::DType::F64); const int al = g.input(sh, kir::DType::F64); const int dist = g.input(sh, kir::DType::F64); const int dens = g.input(sh, kir::DType::F64);
    const int rr = g.input(sh, kir::DType::F64); const int var = g.input(sh, kir::DType::F64);
    double ctv[kN]; double gav[kN]; double alv[kN]; double distv[kN]; double densv[kN]; double rrv[kN]; double varv[kN];
    for (int i = 0; i < kN; ++i)
    {
        ctv[i] = -0.9 + 0.09 * i; gav[i] = -0.4 + 0.04 * i; alv[i] = 0.2 + 0.03 * i; distv[i] = 0.1 + 0.05 * i; densv[i] = 0.3 + 0.02 * i;
        rrv[i] = 0.1 + 0.05 * i; varv[i] = 0.2 + 0.02 * i;
    }
    const double* inp[] = {ctv, gav, alv, distv, densv, rrv, varv};

    const auto kc  = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chk3 = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };

    chk(scr::henyey_greenstein(g, ct, ga), [&](int i) { return rhg(ctv[i], gav[i]); });
    chk(scr::cornette_shanks(g, ct, ga), [&](int i) { return rcs(ctv[i], gav[i]); });
    chk(scr::draine_mie(g, ct, ga, al), [&](int i) { return rdraine(ctv[i], gav[i], alv[i]); });
    const double sig[3] = {0.5, 0.3, 0.2};
    chk3(scr::beer_lambert(g, g.vec3(kc(0.5), kc(0.3), kc(0.2)), dist), [&](int i, int c) { return rbeer(sig[c], distv[i]); });
    const int    phase = scr::henyey_greenstein(g, ct, ga);
    const int    trans = scr::beer_lambert(g, g.vec3(kc(0.5), kc(0.3), kc(0.2)), dist);
    const double lc[3] = {1.0, 0.8, 0.6};
    chk3(scr::froxel_scatter(g, g.vec3(kc(1.0), kc(0.8), kc(0.6)), phase, trans, dens), [&](int i, int c) { return ((lc[c] * rhg(ctv[i], gav[i])) * rbeer(sig[c], distv[i])) * densv[i]; });
    const double dvals[3] = {0.4, 0.25, 0.15};
    chk3(scr::burley_diffusion(g, rr, g.vec3(kc(0.4), kc(0.25), kc(0.15))), [&](int i, int c) { return rburley(rrv[i], dvals[c]); });
    chk(scr::sss_gaussian(g, rr, var), [&](int i) { return rgauss(rrv[i], varv[i]); });

    CHECK(bad == 0);
}

TEST_CASE("B8-l: render-path math (clustered light cull / deferred G-buffer decode / decal projection) bit-exact", "[kir][lighting][render]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int vz = g.input(sh, kir::DType::F64); const int ux = g.input(sh, kir::DType::F64); const int uy = g.input(sh, kir::DType::F64);
    const int cx = g.input(sh, kir::DType::F64); const int cy = g.input(sh, kir::DType::F64); const int cz = g.input(sh, kir::DType::F64); const int rad = g.input(sh, kir::DType::F64);
    const int wx = g.input(sh, kir::DType::F64); const int wy = g.input(sh, kir::DType::F64); const int wz = g.input(sh, kir::DType::F64);
    double vzv[kN]; double uxv[kN]; double uyv[kN]; double cxv[kN]; double cyv[kN]; double czv[kN]; double radv[kN]; double wxv[kN]; double wyv[kN]; double wzv[kN];
    for (int i = 0; i < kN; ++i)
    {
        vzv[i] = 0.2 + 0.9 * i; uxv[i] = 0.02 + 0.048 * i; uyv[i] = 0.05 + 0.045 * i;
        cxv[i] = -1.5 + 0.15 * i; cyv[i] = 0.1; czv[i] = 0.2; radv[i] = 0.3 + 0.05 * i;
        wxv[i] = -0.8 + 0.08 * i; wyv[i] = 0.1; wzv[i] = 0.0;
    }
    const double* inp[] = {vzv, uxv, uyv, cxv, cyv, czv, radv, wxv, wyv, wzv};

    const auto kc = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    int        bad = 0;
    const auto chk  = [&](int node, auto ref) { double o[kN]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { if (o[i] != ref(i)) { ++bad; } } };
    const auto chk3 = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };

    // Forward+ : the exponential z-slice + the froxel coord + the light-sphere-vs-cluster cull.
    chk(rn::cluster_z_slice(g, vz, kc(0.1), kc(100.0), kc(16.0)), [&](int i) { return rcluster_z_slice(vzv[i], 0.1, 100.0, 16.0); });
    chk3(rn::cluster_coord(g, g.vec2(ux, uy), vz, kc(16.0), kc(8.0), kc(16.0), kc(0.1), kc(100.0)),
         [&](int i, int c) { if (c == 0) { return crd::math::floor(uxv[i] * 16.0); } if (c == 1) { return crd::math::floor(uyv[i] * 8.0); } return rcluster_z_slice(vzv[i], 0.1, 100.0, 16.0); });
    const int amin = g.vec3(kc(-1.0), kc(-1.0), kc(-1.0));
    const int amax = g.vec3(kc(1.0), kc(1.0), kc(1.0));
    chk(rn::sphere_aabb_sq_dist(g, g.vec3(cx, cy, cz), amin, amax), [&](int i) { const double c[3] = {cxv[i], cyv[i], czv[i]}; const double lo[3] = {-1, -1, -1}; const double hi[3] = {1, 1, 1}; return rsq_dist_aabb(c, lo, hi); });
    chk(rn::light_cluster_cull(g, g.vec3(cx, cy, cz), rad, amin, amax), [&](int i) { const double c[3] = {cxv[i], cyv[i], czv[i]}; const double lo[3] = {-1, -1, -1}; const double hi[3] = {1, 1, 1}; const double sq = rsq_dist_aabb(c, lo, hi); return (radv[i] * radv[i] < sq) ? 0.0 : 1.0; });

    // Clustered decals : project a world point into the decal box (inv = scale 1.5) → uv + inside.
    const double dm[16] = {1.5, 0, 0, 0, 0, 1.5, 0, 0, 0, 0, 1.5, 0, 0, 0, 0, 1};
    const int    dminv  = g.mat4(g.vec4(kc(1.5), kc(0), kc(0), kc(0)), g.vec4(kc(0), kc(1.5), kc(0), kc(0)), g.vec4(kc(0), kc(0), kc(1.5), kc(0)), g.vec4(kc(0), kc(0), kc(0), kc(1)));
    chk3(rn::decal_project(g, g.vec3(wx, wy, wz), dminv), [&](int i, int c) {
        const double wp[4] = {wxv[i], wyv[i], wzv[i], 1.0}; double loc[4]; rmatvec4(dm, wp, loc);
        if (c == 0) { return loc[0] + 0.5; } if (c == 1) { return loc[1] + 0.5; } return (rin1(loc[0]) * rin1(loc[1])) * rin1(loc[2]); });

    // Deferred : the B5 G-buffer round-trips base/metallic/roughness through pack → decode (F32 surface; direct channels).
    {
        kir::KGraph gf(&alloc);
        const auto  kf = [&](double v) { return gf.constant(v, sh, kir::DType::F32); };
        const int   base = gf.vec3(kf(0.7), kf(0.3), kf(0.2));
        const int   nrm  = gf.normalize(gf.vec3(kf(0.0), kf(0.0), kf(1.0)));
        const int   sid  = mat::define_surface(gf);
        const int   surf = mat::build_surface(gf, sid, base, kf(0.1), kf(0.55), nrm, gf.vec3(kf(0.0), kf(0.0), kf(0.0)), kf(1.0), kf(1.0));
        kir::KEntry tmp; mat::pack_gbuffer(gf, tmp, surf);
        double      base_dec[kN * 3]; kir::eval_cpu(gf, nullptr, &alloc, gf.swizzle(tmp.out[0].node, 0, 1, 2), base_dec);
        double      met_dec[kN];      kir::eval_cpu(gf, nullptr, &alloc, gf.swizzle(tmp.out[0].node, 3), met_dec);
        double      rgh_dec[kN];      kir::eval_cpu(gf, nullptr, &alloc, gf.swizzle(tmp.out[1].node, 3), rgh_dec);
        const double eb[3] = {static_cast<double>(0.7F), static_cast<double>(0.3F), static_cast<double>(0.2F)};
        for (int i = 0; i < kN; ++i)
        {
            for (int c = 0; c < 3; ++c) { if (base_dec[i * 3 + c] != eb[c]) { ++bad; } } // g0.rgb recovers base
            if (met_dec[i] != static_cast<double>(0.1F)) { ++bad; }                       // g0.a recovers metallic
            if (rgh_dec[i] != static_cast<double>(0.55F)) { ++bad; }                      // g1.a recovers roughness
        }
    }

    CHECK(bad == 0);
}

TEST_CASE("B8-k: material cook seam (per-pass variants + lowering round-trip + ShaderOption specialize)", "[kir][lighting][cook]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    const kir::Shape           sh = kir::make_shape({kN});
    const ck::MaterialTemplate tmpl{cook_test_surface, nullptr};

    // ── (1) STRUCTURAL: each pass produces the right fragment output shape + alpha-test wiring. Each variant gets a FRESH
    // graph — `build_fs_for_pass` runs `lower_entry` which renumbers the graph, so one graph = one cooked variant. ──
    const ck::PassType passes[] = {ck::PassType::Shadow, ck::PassType::DepthPrepass, ck::PassType::GBuffer, ck::PassType::Forward};
    const int          expect[] = {0, 0, mat::kGBufferAttachments, 1};
    for (int p = 0; p < 4; ++p)
    {
        for (int mask = 0; mask < 2; ++mask)
        {
            kir::KGraph       g(&alloc);
            const auto        kf = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
            ck::SurfaceInputs in;
            in.uv = g.vec2(kf(0.3), kf(0.4)); in.world_normal = g.normalize(g.vec3(kf(0.0), kf(0.0), kf(1.0))); in.view_dir = g.normalize(g.vec3(kf(0.0), kf(0.0), kf(1.0)));
            const int          ldir = g.vec3(kf(0.2), kf(-0.3), kf(-1.0)); const int lcol = g.vec3(kf(1.5), kf(1.5), kf(1.5));
            const auto         am   = mask != 0 ? mat::AlphaMode::Masked : mat::AlphaMode::Opaque;
            kir::KEntry        e; ck::build_fs_for_pass(tmpl, passes[p], {am, 0.5}, in, g, e, ldir, lcol);
            CHECK(e.stage == kir::KStage::Fragment);
            CHECK(e.n_out == expect[p]);
            if (mask != 0) { CHECK(e.discard_cond >= 0); } // masked → alpha test wired
            else { CHECK(e.discard_cond < 0); }            // opaque → no alpha test
        }
    }

    int bad = 0;

    // ── (2) ROUND-TRIP BIT-STABLE: lowering the Forward variant does not change a single evaluated pixel ──
    {
        kir::KGraph g(&alloc);
        const int   uvx = g.input(sh, kir::DType::F32); const int uvy = g.input(sh, kir::DType::F32);
        const int   nnx = g.input(sh, kir::DType::F32); const int nny = g.input(sh, kir::DType::F32); const int nnz = g.input(sh, kir::DType::F32);
        const int   vvx = g.input(sh, kir::DType::F32); const int vvy = g.input(sh, kir::DType::F32); const int vvz = g.input(sh, kir::DType::F32);
        double      uvxv[kN]; double uvyv[kN]; double nnxv[kN]; double nnyv[kN]; double nnzv[kN]; double vvxv[kN]; double vvyv[kN]; double vvzv[kN];
        for (int i = 0; i < kN; ++i) { uvxv[i] = 0.2 + 0.03 * i; uvyv[i] = 0.3 + 0.02 * i; nnxv[i] = 0.1 + 0.02 * i; nnyv[i] = 0.2 - 0.01 * i; nnzv[i] = 0.9; vvxv[i] = 0.0; vvyv[i] = 0.02 * i; vvzv[i] = 1.0; }
        const double* inp[] = {uvxv, uvyv, nnxv, nnyv, nnzv, vvxv, vvyv, vvzv};
        const auto    kf = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
        ck::SurfaceInputs in; in.uv = g.vec2(uvx, uvy); in.world_normal = g.normalize(g.vec3(nnx, nny, nnz)); in.view_dir = g.normalize(g.vec3(vvx, vvy, vvz));
        const int   ldir = g.vec3(kf(0.2), kf(-0.3), kf(-1.0)); const int lcol = g.vec3(kf(1.5), kf(1.5), kf(1.5));
        kir::KEntry e; ck::build_fs_for_pass(tmpl, ck::PassType::Forward, {mat::AlphaMode::Opaque, 0.5}, in, g, e, ldir, lcol, /*do_lower=*/false);
        double      before[kN * 4]; kir::eval_cpu(g, inp, &alloc, e.out[0].node, before);
        crd::kir::lower::lower_entry(g, e);
        double      after[kN * 4]; kir::eval_cpu(g, inp, &alloc, e.out[0].node, after);
        for (int i = 0; i < kN * 4; ++i) { if (before[i] != after[i]) { ++bad; } }
    }

    // ── (3) SHADEROPTION SPECIALIZE: pinning a runtime selector bakes the chosen branch, bit-identical to a direct build ──
    {
        double xv[kN]; for (int i = 0; i < kN; ++i) { xv[i] = 0.1 + 0.05 * i; }
        for (int pick = 0; pick < 2; ++pick)
        {
            kir::KGraph g(&alloc);
            const auto  kf  = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
            const int   x   = g.input(sh, kir::DType::F64);
            const int   opt = g.input(sh, kir::DType::F64);        // the runtime ShaderOption selector
            const int   a   = g.binary(kir::KOp::Mul, x, kf(2.0)); // branch A (option ≠ 0)
            const int   b   = g.binary(kir::KOp::Add, x, kf(1.0)); // branch B (option = 0)
            const int   col = g.select(opt, a, b);
            kir::KEntry e; e.stage = kir::KStage::Fragment; e.n_out = 1; e.out[0] = {col, 0};
            ck::specialize_variant(g, e, opt, pick == 0 ? 1.0 : 0.0);
            double        optv[kN]; for (int i = 0; i < kN; ++i) { optv[i] = pick == 0 ? 1.0 : 0.0; }
            const double* inp[] = {xv, optv};
            double        out[kN]; kir::eval_cpu(g, inp, &alloc, e.out[0].node, out);
            for (int i = 0; i < kN; ++i) { const double want = pick == 0 ? xv[i] * 2.0 : xv[i] + 1.0; if (out[i] != want) { ++bad; } }
        }
    }

    CHECK(bad == 0);
}

TEST_CASE("B8-j: skinning (linear-blend position/normal + dual-quaternion) bit-exact", "[kir][lighting][skin]")
{
    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int px = g.input(sh, kir::DType::F64); const int py = g.input(sh, kir::DType::F64); const int pz = g.input(sh, kir::DType::F64);
    const int lw0 = g.input(sh, kir::DType::F64); const int lw1 = g.input(sh, kir::DType::F64); const int lw2 = g.input(sh, kir::DType::F64); const int lw3 = g.input(sh, kir::DType::F64);
    const int nx = g.input(sh, kir::DType::F64); const int ny = g.input(sh, kir::DType::F64); const int nz = g.input(sh, kir::DType::F64);
    const int r1x = g.input(sh, kir::DType::F64); const int r1y = g.input(sh, kir::DType::F64); const int r1z = g.input(sh, kir::DType::F64); const int r1w = g.input(sh, kir::DType::F64);
    const int dw0 = g.input(sh, kir::DType::F64); const int dw1 = g.input(sh, kir::DType::F64);
    double pxv[kN]; double pyv[kN]; double pzv[kN]; double lw0v[kN]; double lw1v[kN]; double lw2v[kN]; double lw3v[kN]; double nxv[kN]; double nyv[kN]; double nzv[kN];
    double r1xv[kN]; double r1yv[kN]; double r1zv[kN]; double r1wv[kN]; double dw0v[kN]; double dw1v[kN];
    for (int i = 0; i < kN; ++i)
    {
        pxv[i] = 0.20 + 0.01 * i; pyv[i] = 0.30 - 0.01 * i; pzv[i] = 0.10 + 0.02 * i;
        lw0v[i] = 0.40 + 0.01 * i; lw1v[i] = 0.30; lw2v[i] = 0.20 - 0.01 * i; lw3v[i] = 0.10;
        nxv[i] = 0.30 + 0.02 * i; nyv[i] = 0.60 - 0.01 * i; nzv[i] = 0.50;
        r1xv[i] = 0.20 + 0.01 * i; r1yv[i] = 0.10; r1zv[i] = 0.30 - 0.01 * i; r1wv[i] = 0.50 - 0.05 * i; // r1.w crosses 0 → both antipodal branches
        dw0v[i] = 0.60 + 0.01 * i; dw1v[i] = 0.40 - 0.01 * i;
    }
    const double* inp[] = {pxv, pyv, pzv, lw0v, lw1v, lw2v, lw3v, nxv, nyv, nzv, r1xv, r1yv, r1zv, r1wv, dw0v, dw1v};

    const auto kc = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    const int  m0 = g.mat4(g.vec4(kc(1), kc(0), kc(0), kc(0)), g.vec4(kc(0), kc(1), kc(0), kc(0)), g.vec4(kc(0), kc(0), kc(1), kc(0)), g.vec4(kc(0), kc(0), kc(0), kc(1)));
    const int  m1 = g.mat4(g.vec4(kc(1), kc(0), kc(0), kc(0)), g.vec4(kc(0), kc(1), kc(0), kc(0)), g.vec4(kc(0), kc(0), kc(1), kc(0)), g.vec4(kc(0.5), kc(0.3), kc(0), kc(1)));
    const int  m2 = g.mat4(g.vec4(kc(0), kc(1), kc(0), kc(0)), g.vec4(kc(-1), kc(0), kc(0), kc(0)), g.vec4(kc(0), kc(0), kc(1), kc(0)), g.vec4(kc(0), kc(0), kc(0), kc(1)));
    const int  m3 = g.mat4(g.vec4(kc(1), kc(0), kc(0), kc(0)), g.vec4(kc(0), kc(1), kc(0), kc(0)), g.vec4(kc(0), kc(0), kc(1), kc(0)), g.vec4(kc(0), kc(0), kc(0), kc(1)));
    const int  lw = g.vec4(lw0, lw1, lw2, lw3);
    const int  pp = g.vec3(px, py, pz);
    const int  nn = g.vec3(nx, ny, nz);
    const int  r0 = g.vec4(kc(0), kc(0), kc(0), kc(1)); const int d0 = g.vec4(kc(0), kc(0), kc(0), kc(0)); const int d1 = g.vec4(kc(0.1), kc(0.05), kc(0.02), kc(0.0));
    const int  r1 = g.vec4(r1x, r1y, r1z, r1w);

    const double  bm0[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const double  bm1[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5, 0.3, 0, 1};
    const double  bm2[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const double  bm3[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const double* bmats[4] = {bm0, bm1, bm2, bm3};
    const double  rq0[4]  = {0, 0, 0, 1}; const double dq0[4] = {0, 0, 0, 0}; const double dq1[4] = {0.1, 0.05, 0.02, 0.0};

    int        bad  = 0;
    const auto chk3 = [&](int node, auto ref) { double o[kN * 3]; kir::eval_cpu(g, inp, &alloc, node, o); for (int i = 0; i < kN; ++i) { for (int c = 0; c < 3; ++c) { if (o[i * 3 + c] != ref(i, c)) { ++bad; } } } };

    chk3(lt::lbs_skin_position(g, m0, m1, m2, m3, lw, pp), [&](int i, int c) { const double w[4] = {lw0v[i], lw1v[i], lw2v[i], lw3v[i]}; const double p[3] = {pxv[i], pyv[i], pzv[i]}; double o[3]; rlbs(bmats, w, p, false, o); return o[c]; });
    chk3(lt::lbs_skin_normal(g, m0, m1, m2, m3, lw, nn), [&](int i, int c) { const double w[4] = {lw0v[i], lw1v[i], lw2v[i], lw3v[i]}; const double p[3] = {nxv[i], nyv[i], nzv[i]}; double o[3]; rlbs(bmats, w, p, true, o); return o[c]; });
    chk3(lt::dquat_skin_position(g, r0, d0, r1, d1, dw0, dw1, pp), [&](int i, int c) { const double r1c[4] = {r1xv[i], r1yv[i], r1zv[i], r1wv[i]}; const double p[3] = {pxv[i], pyv[i], pzv[i]}; double o[3]; rdquat(rq0, dq0, r1c, dq1, dw0v[i], dw1v[i], p, o); return o[c]; });

    CHECK(bad == 0);
}

TEST_CASE("B8-c: punctual lights (directional + point + spot) forward loop bit-exact vs Filament", "[kir][lighting][lights]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({kN});

    const int nx = g.input(sh, kir::DType::F64); const int ny = g.input(sh, kir::DType::F64); const int nz = g.input(sh, kir::DType::F64);
    const int vx = g.input(sh, kir::DType::F64); const int vy = g.input(sh, kir::DType::F64); const int vz = g.input(sh, kir::DType::F64);
    const int wx = g.input(sh, kir::DType::F64); const int wy = g.input(sh, kir::DType::F64); const int wz = g.input(sh, kir::DType::F64);

    double narr[3][kN]; double varr[3][kN]; double warr[3][kN];
    const auto unit = [](double& x, double& y, double& z) { const double d = crd::math::sqrt(x * x + y * y + z * z); x /= d; y /= d; z /= d; };
    for (int i = 0; i < kN; ++i)
    {
        narr[0][i] = 0.06 * i - 0.6; narr[1][i] = 0.5;             narr[2][i] = 1.0;
        varr[0][i] = 0.1;            varr[1][i] = 0.05 * i - 0.45; varr[2][i] = 1.2;
        unit(narr[0][i], narr[1][i], narr[2][i]); unit(varr[0][i], varr[1][i], varr[2][i]);
        warr[0][i] = 0.5 * i - 5.0;  warr[1][i] = 0.2;             warr[2][i] = 0.1 * i; // surface pos → distance/cone to fixed lights varies
    }
    const double* inp[] = {narr[0], narr[1], narr[2], varr[0], varr[1], varr[2], warr[0], warr[1], warr[2]};

    const int nn = g.vec3(nx, ny, nz); const int vv = g.vec3(vx, vy, vz); const int ww = g.vec3(wx, wy, wz);
    const auto kc = [&](double x, double y, double z) { return g.vec3(g.constant(x, sh, kir::DType::F64), g.constant(y, sh, kir::DType::F64), g.constant(z, sh, kir::DType::F64)); };
    const auto ks = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    // fixed material + fixed lights
    const int base = kc(0.8, 0.3, 0.2);
    const int met  = ks(0.1);
    const int rgh  = ks(0.4);
    const int ddir = kc(0.3, -1.0, 0.2);  const int dcol = kc(2.0, 2.0, 2.0);                    // directional
    const int ppos = kc(2.0, 3.0, 1.0);   const int pcol = kc(5.0, 4.0, 3.0); const int pfall = ks(0.01);      // point  (radius 10)
    const int spos = kc(0.0, 4.0, 0.0);   const int scol = kc(6.0, 5.0, 4.0); const int sfall = ks(0.015625);  // spot   (radius 8)
    const int sdir = g.normalize(kc(0.0, -1.0, 0.0)); const int sscale = ks(4.0); const int soff = ks(-2.0);

    const int lit = g.binary(kir::KOp::Add, g.binary(kir::KOp::Add,
                                lt::directional_light(g, base, met, rgh, nn, vv, ddir, dcol),
                                lt::point_light(g, base, met, rgh, nn, vv, ww, ppos, pcol, pfall)),
                             lt::spot_light(g, base, met, rgh, nn, vv, ww, spos, scol, sfall, sdir, sscale, soff));

    // reference
    const double bref[3] = {0.8, 0.3, 0.2};
    const double dcr[3]  = {2.0, 2.0, 2.0};
    const double pcr[3]  = {5.0, 4.0, 3.0};
    const double scr[3]  = {6.0, 5.0, 4.0};
    double       sdr[3]  = {0.0, -1.0, 0.0};
    unit(sdr[0], sdr[1], sdr[2]);
    const double negsd[3] = {-sdr[0], -sdr[1], -sdr[2]};

    double o[kN * 3];
    kir::eval_cpu(g, inp, &alloc, lit, o);
    int bad = 0;
    for (int i = 0; i < kN; ++i)
    {
        const double n_i[3] = {narr[0][i], narr[1][i], narr[2][i]};
        const double v_i[3] = {varr[0][i], varr[1][i], varr[2][i]};
        const double w_i[3] = {warr[0][i], warr[1][i], warr[2][i]};
        double       ld[3]  = {-0.3, 1.0, -0.2};
        unit(ld[0], ld[1], ld[2]); // normalize(-direction)
        const double pp[3]  = {2.0 - w_i[0], 3.0 - w_i[1], 1.0 - w_i[2]};
        double       lp[3];
        rnorm3(pp, lp);
        const double att_p = rdist_atten(pp, 0.01);
        const double sp[3]  = {0.0 - w_i[0], 4.0 - w_i[1], 0.0 - w_i[2]};
        double       ls[3];
        rnorm3(sp, ls);
        const double att_s = rdist_atten(sp, 0.015625) * rangle_atten(negsd, ls, 4.0, -2.0);
        for (int c = 0; c < 3; ++c)
        {
            const double want = ref_brdf(bref, 0.1, 0.4, n_i, v_i, ld, dcr, c)
                              + ref_brdf(bref, 0.1, 0.4, n_i, v_i, lp, pcr, c) * att_p
                              + ref_brdf(bref, 0.1, 0.4, n_i, v_i, ls, scr, c) * att_s;
            if (o[i * 3 + c] != want) { ++bad; }
        }
    }
    CHECK(bad == 0);
}
