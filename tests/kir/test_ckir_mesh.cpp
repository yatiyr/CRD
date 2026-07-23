// test_ckir_mesh.cpp — D-007 B19-c2: MESH EXTRACTION (TSDF fusion → marching cubes). This file gates the TSDF fusion
// kernel: a posed depth map integrated into a Truncated Signed Distance Field on a voxel grid. Pinned against a
// closed-form plane — the fused field must be a signed ramp that crosses zero exactly at the observed surface, be
// positive in free space (in front), negative behind (within μ), and unobserved (weight 0) beyond the truncation.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_gsplat2d.hpp>
#include <crd/kir/ckir_mesh.hpp>
#include <crd/kir/ckir_scan.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace kir = crd::kir;

TEST_CASE("ckir tsdf fusion: a plane depth map integrates to a signed ramp crossing zero at the surface", "[ckir][mesh]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U, nullptr, "tsdf");

    constexpr int nx  = 8;
    constexpr int ny  = 8;
    constexpr int nz  = 16;
    constexpr int nvox = nx * ny * nz;
    constexpr int imw = 32;
    constexpr int imh = 32;
    const double  dd  = 5.0;   // the plane sits at view-z 5
    const double  h   = 0.25;  // voxel size
    const double  oz  = 3.0;   // grid spans z in [3,7]
    const double  mu  = 1.0;   // truncation

    // depth map: the whole image observes the plane at depth 5.
    crd::containers::Array<double> depth(&alloc);
    depth.resize((static_cast<crd::usize>(imw) * static_cast<crd::usize>(imh)), 0.0);
    for (int i = 0; i < imw * imh; ++i) { depth[static_cast<crd::usize>(i)] = dd; }

    // camera: R = identity, t = 0 ⇒ view-z == world z. Wide FOV so the grid projects in-bounds.
    crd::containers::Array<double> cam(&alloc);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 0.0;
    cam[12] = 30.0; cam[13] = 30.0; cam[14] = 16.0; cam[15] = 16.0;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    crd::containers::Array<double> gp(&alloc);
    gp.resize(5U, 0.0);
    gp[0] = -1.0; gp[1] = -1.0; gp[2] = oz; gp[3] = h; gp[4] = mu; // origin (-1,-1,3), h, mu

    crd::containers::Array<double> tsum(&alloc);
    crd::containers::Array<double> wsum(&alloc);
    tsum.resize(static_cast<crd::usize>(nvox), 0.0);
    wsum.resize(static_cast<crd::usize>(nvox), 0.0);

    kir::mesh::TsdfConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz; cfg.img_w = imw; cfg.img_h = imh;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::mesh::build_tsdf_integrate_kernel(g, cfg);
    kir::KernelBuffer bb[5] = {{depth.data(), imw * imh, 0, 0}, {cam.data(), 20, 0, 1}, {gp.data(), 5, 0, 2},
                               {tsum.data(), nvox, 0, 3}, {wsum.data(), nvox, 0, 4}};
    kir::eval_cpu_kernel(g, e, bb, 5, e.local_size[0], &alloc, static_cast<crd::u32>(nvox / cfg.local_size));

    // central column (i=4, j=4): tid = k·(nx·ny) + j·nx + i
    const auto vox = [&](int k) { const int idx = k * (nx * ny) + 4 * nx + 4; return static_cast<crd::usize>(idx); };
    const auto tsdf = [&](int k) { const double w = wsum[vox(k)]; return w > 0.0 ? tsum[vox(k)] / w : 0.0; };
    const auto zof  = [&](int k) { return oz + (static_cast<double>(k) + 0.5) * h; };

    // in free space (voxel closer than the surface) ⇒ tsdf > 0; behind (within μ) ⇒ tsdf < 0.
    for (int k = 3; k <= 11; ++k)
    {
        const double sdf = dd - zof(k); // the exact signed distance
        double sdf_t = sdf; if (sdf_t > 1.0) { sdf_t = 1.0; } if (sdf_t < -1.0) { sdf_t = -1.0; } // truncated to [-1,1]
        CHECK(wsum[vox(k)] > 0.5);                    // observed by this view
        CHECK(crd::math::abs(tsdf(k) - sdf_t) < 1.0e-4); // matches the closed-form truncated SDF
    }
    // the zero crossing is between k=7 (z=4.875, +) and k=8 (z=5.125, −), i.e. at z ≈ 5 = the surface.
    CHECK(tsdf(7) > 0.0);
    CHECK(tsdf(8) < 0.0);
    const double t7 = tsdf(7);
    const double t8 = tsdf(8);
    const double zc = zof(7) + (zof(8) - zof(7)) * (t7 / (t7 - t8)); // linear zero crossing
    CHECK(crd::math::abs(zc - dd) < 1.0e-3);

    // voxels farther behind the surface than μ are unobserved ⇒ weight 0 (z=6.125 at k=12, sdf=-1.125 < -μ).
    CHECK(wsum[vox(12)] < 0.5);
    CHECK(wsum[vox(15)] < 0.5);

    // MULTI-VIEW accumulation: integrating the SAME view again doubles the weight but leaves the average unchanged.
    kir::eval_cpu_kernel(g, e, bb, 5, e.local_size[0], &alloc, static_cast<crd::u32>(nvox / cfg.local_size));
    CHECK(crd::math::abs(wsum[vox(7)] - 2.0) < 1.0e-5);          // weight doubled
    CHECK(crd::math::abs((tsum[vox(7)] / wsum[vox(7)]) - t7) < 1.0e-5); // running average unchanged
}

TEST_CASE("ckir tsdf: 2DGS render -> surface depth -> TSDF crosses zero at the surfel plane (end to end)", "[ckir][mesh]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U, nullptr, "tsdf-e2e");

    constexpr int imw = 32;
    constexpr int imh = 32;
    constexpr int nx  = 8;
    constexpr int ny  = 8;
    constexpr int nz  = 16;
    constexpr int nvox = nx * ny * nz;
    const double  h   = 0.25;
    const double  oz  = -2.0; // grid spans world z in [-2,2]; the surfel surface is at world z 0

    // camera: t = (0,0,5) so a surfel at world 0 sits at view-z 5; wide-ish FOV.
    crd::containers::Array<double> cam(&alloc);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 50.0; cam[13] = 50.0; cam[14] = 16.5; cam[15] = 16.5;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    // one big facing surfel at world 0 (normal +z), opaque.
    crd::containers::Array<double> surf(&alloc);
    surf.resize(13U, 0.0);
    surf[0] = 0.0; surf[1] = 0.0; surf[2] = 0.0;
    surf[3] = 0.6; surf[4] = 0.6;
    surf[5] = 0.0; surf[6] = 0.0; surf[7] = 0.0; surf[8] = 1.0;
    surf[9] = 0.99; surf[10] = 0.0; surf[11] = 0.0; surf[12] = 0.0;

    // project + render (1 surfel, already "sorted")
    kir::gsplat::Gsplat2dProjectConfig pcfg;
    kir::KGraph                        pg(&alloc);
    const kir::KEntry                  pe = kir::gsplat::build_gsplat2d_project_kernel(pg, pcfg);
    crd::containers::Array<double>     prep(&alloc);
    prep.resize(19U, 0.0);
    kir::KernelBuffer pb[3] = {{surf.data(), 13, 0, 0}, {cam.data(), 20, 0, 1}, {prep.data(), 19, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);

    kir::gsplat::Gsplat2dRenderConfig rcfg;
    rcfg.width = imw; rcfg.height = imh; rcfg.max_splats = 1;
    kir::KGraph       rg(&alloc);
    const kir::KEntry re = kir::gsplat::build_gsplat2d_render_kernel(rg, rcfg);
    crd::containers::Array<double> par(&alloc);
    par.resize(5U, 0.0); par[0] = 1.0; par[4] = 1.0 / 255.0;
    crd::containers::Array<double> render(&alloc);
    render.resize((static_cast<crd::usize>(imw) * static_cast<crd::usize>(imh)) * 8U, 0.0);
    kir::KernelBuffer rb[4] = {{prep.data(), 19, 0, 0}, {cam.data(), 20, 0, 1}, {par.data(), 5, 0, 2}, {render.data(), imw * imh * 8, 0, 3}};
    kir::eval_cpu_kernel(rg, re, rb, 4, re.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    // extract surface depth (depthSum/(1-T))
    crd::containers::Array<double> depth(&alloc);
    depth.resize((static_cast<crd::usize>(imw) * static_cast<crd::usize>(imh)), 0.0);
    kir::KGraph       sg(&alloc);
    const kir::KEntry se = kir::mesh::build_surface_depth_kernel(sg, 64);
    kir::KernelBuffer sb[2] = {{render.data(), imw * imh * 8, 0, 0}, {depth.data(), imw * imh, 0, 1}};
    kir::eval_cpu_kernel(sg, se, sb, 2, se.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    // the centre pixel must have observed the surface at depth ~5.
    CHECK(crd::math::abs(depth[static_cast<crd::usize>(16 * imw + 16)] - 5.0) < 1.0e-2);

    // integrate into a TSDF grid (world space; the SAME camera projects voxels)
    crd::containers::Array<double> gp(&alloc);
    gp.resize(5U, 0.0);
    gp[0] = -1.0; gp[1] = -1.0; gp[2] = oz; gp[3] = h; gp[4] = 1.0;
    crd::containers::Array<double> tsum(&alloc);
    crd::containers::Array<double> wsum(&alloc);
    tsum.resize(static_cast<crd::usize>(nvox), 0.0);
    wsum.resize(static_cast<crd::usize>(nvox), 0.0);
    kir::mesh::TsdfConfig tcfg;
    tcfg.nx = nx; tcfg.ny = ny; tcfg.nz = nz; tcfg.img_w = imw; tcfg.img_h = imh;
    kir::KGraph       tg(&alloc);
    const kir::KEntry te = kir::mesh::build_tsdf_integrate_kernel(tg, tcfg);
    kir::KernelBuffer tb[5] = {{depth.data(), imw * imh, 0, 0}, {cam.data(), 20, 0, 1}, {gp.data(), 5, 0, 2},
                               {tsum.data(), nvox, 0, 3}, {wsum.data(), nvox, 0, 4}};
    kir::eval_cpu_kernel(tg, te, tb, 5, te.local_size[0], &alloc, static_cast<crd::u32>(nvox / tcfg.local_size));

    // central column: the fused field crosses zero at world z = 0 (the surfel plane).
    const auto vox  = [&](int k) { const int idx = k * (nx * ny) + 4 * nx + 4; return static_cast<crd::usize>(idx); };
    const auto tsdf = [&](int k) { const double w = wsum[vox(k)]; return w > 0.0 ? tsum[vox(k)] / w : 0.0; };
    const auto zof  = [&](int k) { return oz + (static_cast<double>(k) + 0.5) * h; };
    CHECK(wsum[vox(7)] > 0.5);
    CHECK(wsum[vox(8)] > 0.5);
    CHECK(tsdf(7) > 0.0);   // world z = -0.125, in free space (in front)
    CHECK(tsdf(8) < 0.0);   // world z = +0.125, behind the surface
    const double t7 = tsdf(7);
    const double t8 = tsdf(8);
    const double zc = zof(7) + (zof(8) - zof(7)) * (t7 / (t7 - t8));
    CHECK(crd::math::abs(zc - 0.0) < 2.0e-2); // zero crossing at the surfel plane, world z = 0
}

#include <crd/kir/mc_tables.hpp>

namespace
{
// host: fill an analytic sphere SDF on the voxel grid. field[v] = |center(v) - c| - R.
void fill_sphere(crd::containers::Array<double>& field, int nx, int ny, int nz, double ox, double oy, double oz,
                 double hh, double cx, double cy, double cz, double rr)
{
    field.resize(static_cast<crd::usize>(nx) * static_cast<crd::usize>(ny) * static_cast<crd::usize>(nz), 0.0);
    for (int k = 0; k < nz; ++k)
    {
        for (int j = 0; j < ny; ++j)
        {
            for (int i = 0; i < nx; ++i)
            {
                const double x = ox + (static_cast<double>(i) + 0.5) * hh - cx;
                const double y = oy + (static_cast<double>(j) + 0.5) * hh - cy;
                const double z = oz + (static_cast<double>(k) + 0.5) * hh - cz;
                const crd::usize v = static_cast<crd::usize>(k) * static_cast<crd::usize>(nx * ny) + static_cast<crd::usize>(j) * static_cast<crd::usize>(nx) + static_cast<crd::usize>(i);
                field[v] = crd::math::sqrt(x * x + y * y + z * z) - rr;
            }
        }
    }
}
// host marching-cubes triangle count for a cell (same tables as the kernel).
int host_cell_tris(const crd::containers::Array<double>& field, int ci, int cj, int ck, int nx, int nxy)
{
    int idx = 0;
    for (int c = 0; c < 8; ++c)
    {
        const int ox = kir::mesh::kMcCornerOff[c * 3 + 0];
        const int oy = kir::mesh::kMcCornerOff[c * 3 + 1];
        const int oz = kir::mesh::kMcCornerOff[c * 3 + 2];
        const crd::usize v = static_cast<crd::usize>(ck + oz) * static_cast<crd::usize>(nxy) + static_cast<crd::usize>(cj + oy) * static_cast<crd::usize>(nx) + static_cast<crd::usize>(ci + ox);
        if (field[v] < 0.0) { idx |= (1 << c); }
    }
    int n = 0;
    for (int t = 0; t < 5; ++t) { if (kir::mesh::kMcTriTable[idx * 16 + t * 3] >= 0) { ++n; } }
    return n;
}
} // namespace

TEST_CASE("ckir marching cubes: per-cell triangle count matches the host reference (sphere SDF)", "[ckir][mesh]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U, nullptr, "mc-count");

    constexpr int nx = 17;
    constexpr int ny = 17;
    constexpr int nz = 17;
    constexpr int ncells = (nx - 1) * (ny - 1) * (nz - 1); // 4096
    const double  h  = 0.125;
    const double  o  = -1.0;
    const double  rr = 0.6;

    crd::containers::Array<double> field(&alloc);
    fill_sphere(field, nx, ny, nz, o, o, o, h, 0.0, 0.0, 0.0, rr);

    crd::containers::Array<double> tri(&alloc);
    tri.resize(256U * 16U, 0.0);
    for (int i = 0; i < 256 * 16; ++i) { tri[static_cast<crd::usize>(i)] = static_cast<double>(kir::mesh::kMcTriTable[i]); }

    crd::containers::Array<double> count(&alloc);
    count.resize(static_cast<crd::usize>(ncells), 0.0);

    kir::mesh::McConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::mesh::build_mc_count_kernel(g, cfg);
    kir::KernelBuffer bb[3] = {{field.data(), nx * ny * nz, 0, 0}, {tri.data(), 256 * 16, 0, 1}, {count.data(), ncells, 0, 2}};
    kir::eval_cpu_kernel(g, e, bb, 3, e.local_size[0], &alloc, static_cast<crd::u32>(ncells / cfg.local_size));

    const int cnx = nx - 1;
    const int cny = ny - 1;
    int mism = 0;
    int total = 0;
    for (int c = 0; c < ncells; ++c)
    {
        const int ci = c % cnx;
        const int cj = (c / cnx) % cny;
        const int ck = c / (cnx * cny);
        const int exp = host_cell_tris(field, ci, cj, ck, nx, nx * ny);
        const int got = static_cast<int>(count[static_cast<crd::usize>(c)]);
        if (exp != got) { ++mism; }
        total += got;
    }
    INFO("count mismatches = " << mism << ", total triangles = " << total);
    CHECK(mism == 0);       // per-cell counts match the host MC reference exactly
    CHECK(total > 100);     // the sphere surface genuinely crosses many cells
}

namespace
{
// host reference: the surface vertex on cube edge e (float maths, matching the kernel's f32).
void host_edge_vertex(const crd::containers::Array<double>& field, int ci, int cj, int ck, int nx, int nxy,
                      double ox, double oy, double oz, double hh, int e, float* v)
{
    const int a = kir::mesh::kMcEdgeConn[e * 2 + 0];
    const int b = kir::mesh::kMcEdgeConn[e * 2 + 1];
    const int axo = kir::mesh::kMcCornerOff[a * 3 + 0]; const int ayo = kir::mesh::kMcCornerOff[a * 3 + 1]; const int azo = kir::mesh::kMcCornerOff[a * 3 + 2];
    const int bxo = kir::mesh::kMcCornerOff[b * 3 + 0]; const int byo = kir::mesh::kMcCornerOff[b * 3 + 1]; const int bzo = kir::mesh::kMcCornerOff[b * 3 + 2];
    const int xa = ci + axo; const int ya = cj + ayo; const int za = ck + azo;
    const int xb = ci + bxo; const int yb = cj + byo; const int zb = ck + bzo;
    const float fa = static_cast<float>(field[static_cast<crd::usize>(za) * static_cast<crd::usize>(nxy) + static_cast<crd::usize>(ya) * static_cast<crd::usize>(nx) + static_cast<crd::usize>(xa)]);
    const float fb = static_cast<float>(field[static_cast<crd::usize>(zb) * static_cast<crd::usize>(nxy) + static_cast<crd::usize>(yb) * static_cast<crd::usize>(nx) + static_cast<crd::usize>(xb)]);
    const float den = fa - fb;
    const float tv  = fa / (crd::math::abs(den) > 1.0e-9F ? den : 1.0F);
    const float pax = static_cast<float>(ox + (static_cast<double>(xa) + 0.5) * hh);
    const float pay = static_cast<float>(oy + (static_cast<double>(ya) + 0.5) * hh);
    const float paz = static_cast<float>(oz + (static_cast<double>(za) + 0.5) * hh);
    const float pbx = static_cast<float>(ox + (static_cast<double>(xb) + 0.5) * hh);
    const float pby = static_cast<float>(oy + (static_cast<double>(yb) + 0.5) * hh);
    const float pbz = static_cast<float>(oz + (static_cast<double>(zb) + 0.5) * hh);
    v[0] = pax + tv * (pbx - pax); v[1] = pay + tv * (pby - pay); v[2] = paz + tv * (pbz - paz);
}
} // namespace

TEST_CASE("ckir marching cubes: extract a sphere mesh (vertices on-surface, area, normals, vs host)", "[ckir][mesh]")
{
    crd::memory::TlsfAllocator alloc(128U << 20U, nullptr, "mc-emit");

    constexpr int nx = 9;
    constexpr int ny = 9;
    constexpr int nz = 9;
    constexpr int ncells = (nx - 1) * (ny - 1) * (nz - 1); // 512
    const double  h  = 0.25;
    const double  o  = -1.0;
    const double  rr = 0.5;

    crd::containers::Array<double> field(&alloc);
    fill_sphere(field, nx, ny, nz, o, o, o, h, 0.0, 0.0, 0.0, rr);

    crd::containers::Array<double> tri(&alloc);
    tri.resize(256U * 16U, 0.0);
    for (int i = 0; i < 256 * 16; ++i) { tri[static_cast<crd::usize>(i)] = static_cast<double>(kir::mesh::kMcTriTable[i]); }
    crd::containers::Array<double> econ(&alloc);
    crd::containers::Array<double> coff(&alloc);
    econ.resize(24U, 0.0); coff.resize(24U, 0.0);
    for (int i = 0; i < 24; ++i) { econ[static_cast<crd::usize>(i)] = static_cast<double>(kir::mesh::kMcEdgeConn[i]); coff[static_cast<crd::usize>(i)] = static_cast<double>(kir::mesh::kMcCornerOff[i]); }

    kir::mesh::McConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;

    // count
    crd::containers::Array<double> count(&alloc);
    count.resize(static_cast<crd::usize>(ncells), 0.0);
    kir::KGraph       cg(&alloc);
    const kir::KEntry ce = kir::mesh::build_mc_count_kernel(cg, cfg);
    kir::KernelBuffer cb[3] = {{field.data(), nx * ny * nz, 0, 0}, {tri.data(), 256 * 16, 0, 1}, {count.data(), ncells, 0, 2}};
    kir::eval_cpu_kernel(cg, ce, cb, 3, ce.local_size[0], &alloc, static_cast<crd::u32>(ncells / cfg.local_size));

    // exclusive scan -> per-cell offset
    kir::KGraph g0(&alloc);
    kir::KGraph g1(&alloc);
    kir::KGraph g2(&alloc);
    kir::KGraph* sg[3] = {&g0, &g1, &g2};
    const kir::ScanPlan plan = kir::build_scan(sg, ncells, false, 256, 1);
    REQUIRE(plan.single_pass);
    crd::containers::Array<double> off(&alloc);
    off.resize(static_cast<crd::usize>(ncells), 0.0);
    kir::KernelBuffer scb[2] = {{count.data(), ncells, 0, 0}, {off.data(), ncells, 0, 1}};
    kir::eval_cpu_kernel(*plan.block_graph, plan.block, scb, 2, plan.block.local_size[0], &alloc, static_cast<crd::u32>(plan.nblocks));
    const int total = static_cast<int>(off[static_cast<crd::usize>(ncells - 1)] + count[static_cast<crd::usize>(ncells - 1)]);
    REQUIRE(total > 50);

    // emit
    crd::containers::Array<double> gpm(&alloc);
    gpm.resize(4U, 0.0); gpm[0] = o; gpm[1] = o; gpm[2] = o; gpm[3] = h;
    crd::containers::Array<double> out(&alloc);
    out.resize(static_cast<crd::usize>(total) * 18U, 0.0);
    kir::KGraph       eg(&alloc);
    const kir::KEntry ee = kir::mesh::build_mc_emit_kernel(eg, cfg);
    kir::KernelBuffer eb[7] = {{field.data(), nx * ny * nz, 0, 0}, {gpm.data(), 4, 0, 1}, {tri.data(), 256 * 16, 0, 2},
                               {econ.data(), 24, 0, 3}, {coff.data(), 24, 0, 4}, {off.data(), ncells, 0, 5}, {out.data(), total * 18, 0, 6}};
    kir::eval_cpu_kernel(eg, ee, eb, 7, ee.local_size[0], &alloc, static_cast<crd::u32>(ncells / cfg.local_size));

    // (1) every vertex lies on the sphere; (2) area ~ 4 pi R^2; (3) normals point outward.
    const int cnx = nx - 1; const int cny = ny - 1;
    double worst_r = 0.0;
    double area = 0.0;
    int    bad_n = 0;
    for (int tt = 0; tt < total; ++tt)
    {
        const crd::usize b = static_cast<crd::usize>(tt) * 18U;
        double vv[3][3];
        for (int k = 0; k < 3; ++k) { for (int c = 0; c < 3; ++c) { vv[k][c] = out[b + static_cast<crd::usize>(k * 6 + c)]; } }
        for (int k = 0; k < 3; ++k)
        {
            const double r = crd::math::sqrt(vv[k][0] * vv[k][0] + vv[k][1] * vv[k][1] + vv[k][2] * vv[k][2]);
            if (crd::math::abs(r - rr) > worst_r) { worst_r = crd::math::abs(r - rr); }
        }
        const double ax = vv[1][0] - vv[0][0]; const double ay = vv[1][1] - vv[0][1]; const double az = vv[1][2] - vv[0][2];
        const double bx = vv[2][0] - vv[0][0]; const double by = vv[2][1] - vv[0][1]; const double bz = vv[2][2] - vv[0][2];
        const double cx = ay * bz - az * by; const double cy = az * bx - ax * bz; const double cz = ax * by - ay * bx;
        area += 0.5 * crd::math::sqrt(cx * cx + cy * cy + cz * cz);
        // stored normal (slot 3..5) should agree with the centroid radial direction (outward)
        const double mx = (vv[0][0] + vv[1][0] + vv[2][0]) / 3.0;
        const double my = (vv[0][1] + vv[1][1] + vv[2][1]) / 3.0;
        const double mz = (vv[0][2] + vv[1][2] + vv[2][2]) / 3.0;
        const double nx0 = out[b + 3U]; const double ny0 = out[b + 4U]; const double nz0 = out[b + 5U];
        if (mx * nx0 + my * ny0 + mz * nz0 <= 0.0) { ++bad_n; } // normal must point away from the centre
    }
    const double sphere_area = 4.0 * 3.14159265358979 * rr * rr;
    std::printf("[MC sphere] tris=%d, worst |r-R|=%.4f, area=%.4f (ideal %.4f), outward normals ok=%d/%d\n",
                total, worst_r, area, sphere_area, total - bad_n, total);
    CHECK(worst_r < 0.03);                                  // vertices lie on the sphere (linear-interp error ~ h^2)
    CHECK(bad_n == 0);                                      // every face normal points outward
    CHECK(crd::math::abs(area - sphere_area) < 0.25 * sphere_area); // total area approximates the sphere (coarse MC)

    // (4) exact match vs the host MC reference (same tables, same interpolation, same order).
    crd::containers::Array<int> hoff(&alloc);
    hoff.resize(static_cast<crd::usize>(ncells), 0);
    int acc = 0;
    for (int c = 0; c < ncells; ++c) { hoff[static_cast<crd::usize>(c)] = acc; acc += host_cell_tris(field, c % cnx, (c / cnx) % cny, c / (cnx * cny), nx, nx * ny); }
    int mismatch = 0;
    for (int c = 0; c < ncells; ++c)
    {
        const int ci = c % cnx; const int cj = (c / cnx) % cny; const int ck = c / (cnx * cny);
        int idx = 0;
        for (int cc = 0; cc < 8; ++cc)
        {
            const int oxx = kir::mesh::kMcCornerOff[cc * 3 + 0]; const int oyy = kir::mesh::kMcCornerOff[cc * 3 + 1]; const int ozz = kir::mesh::kMcCornerOff[cc * 3 + 2];
            if (field[static_cast<crd::usize>(ck + ozz) * static_cast<crd::usize>(nx * ny) + static_cast<crd::usize>(cj + oyy) * static_cast<crd::usize>(nx) + static_cast<crd::usize>(ci + oxx)] < 0.0) { idx |= (1 << cc); }
        }
        for (int t = 0; t < 5; ++t)
        {
            const int ea = kir::mesh::kMcTriTable[idx * 16 + t * 3 + 0];
            if (ea < 0) { break; }
            const int ebb = kir::mesh::kMcTriTable[idx * 16 + t * 3 + 1];
            const int ecc = kir::mesh::kMcTriTable[idx * 16 + t * 3 + 2];
            // host emits the same REVERSED winding as the kernel: (v0, v2, v1).
            float hv[3][3];
            host_edge_vertex(field, ci, cj, ck, nx, nx * ny, o, o, o, h, ea, hv[0]);
            host_edge_vertex(field, ci, cj, ck, nx, nx * ny, o, o, o, h, ecc, hv[1]);
            host_edge_vertex(field, ci, cj, ck, nx, nx * ny, o, o, o, h, ebb, hv[2]);
            const crd::usize ob = static_cast<crd::usize>(hoff[static_cast<crd::usize>(c)] + t) * 18U;
            for (int k = 0; k < 3; ++k)
            {
                for (int cx2 = 0; cx2 < 3; ++cx2)
                {
                    if (crd::math::abs(static_cast<double>(out[ob + static_cast<crd::usize>(k * 6 + cx2)]) -
                                       static_cast<double>(hv[k][cx2])) > 1.0e-4) { ++mismatch; }
                }
            }
        }
    }
    INFO("emit vertex mismatches vs host = " << mismatch);
    CHECK(mismatch == 0); // the GPU-authored MC emit == the host reference, vertex for vertex
}
